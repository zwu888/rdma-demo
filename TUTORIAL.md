# Writing your own RDMA/DPDK perf test program

This repo has three custom C++ benchmark programs — `fi_bw.cpp`
(libfabric), `dpdk_perf.cpp` (raw DPDK `ethdev`), and `ibv_bw.cpp` (raw
`libibverbs`) — that all measure the same thing (throughput and
latency, with jitter/percentile stats) over three different APIs. This
is a tutorial on how to write one of these yourself, based on what
actually worked and what actually broke while building all three.

If you just want to run the existing tools, see the main
[README.md](README.md). This document is for writing a *new* one, or
understanding why these are built the way they are.

## The common shape

All three programs follow the same architecture, regardless of which
API sits underneath:

```
Config struct (role, peer address, size, window, iters, mode)
  |
  v
Out-of-band TCP control channel: exchange whatever addressing info
the data-path API needs before it's usable (see below -- this is the
single most important decision in the whole design)
  |
  v
Bring up the connection (API-specific: QP state machine, fi_connect,
or nothing at all for a connectionless DPDK flow)
  |
  v
Two modes:
  - throughput: pipelined, windowed, one-sided where the API allows it
  - latency: ping-pong echo, per-message send timestamp tracked per
    "window slot", RTT computed on completion
  |
  v
Stats: avg/min/max, stdev, RFC 3550 mean jitter, percentiles
(p50/p90/p99/p99.9/max) -- not just an average. See "Why percentiles
matter" below.
```

Writing a new one means filling in the same skeleton with a different
API's connection-setup and data-path calls.

## Decision #1: how do the two sides find each other?

**Use a plain out-of-band (OOB) TCP socket to exchange whatever
addressing info the real API needs, before touching the real data
path.** Don't rely on the API's own built-in address resolution
(`fi_getinfo` by IP string, `rdma_cm`, DNS, etc.) for a point-to-point
test like this.

This sounds like overkill until you hit the actual failure modes it
avoids:

- **`fi_bw.cpp`**: libfabric's `verbs` provider fails
  `rdma_bind_addr()` for a plain IP *source* (listening) query on this
  hardware, with a fallback path that resolves to a native
  `FI_SOCKADDR_IB` address the client has no way to dial by IP string.
  Passing the server's real `fi_getname()` address directly, over a
  tiny OOB socket, sidesteps the whole problem.
- **`dpdk_perf.cpp`**: DPDK has no connection concept at the `ethdev`
  level at all — you're just sending raw frames. The OOB channel here
  isn't for *addressing* so much as coordination (letting the receiver
  install its `rte_flow` rule and be ready before the sender starts).
- **`ibv_bw.cpp`**: raw ibverbs QPs need each other's QPN, starting
  PSN, GID, and (for RDMA operations) remote memory `addr`/`rkey` —
  none of which exist until you generate them locally. There's no way
  to "look up" a peer's QP over the network; you exchange this out of
  band or you don't connect at all.

The exchange struct itself is trivial — a fixed-size packed struct sent
over a connect()/accept() socket — but get this decision right first;
it's the thing that determines whether the rest of the program can even
work on real hardware with real quirks, versus just working in the
common case.

## Decision #2: throughput mode — pipeline, don't ping-pong

If your throughput-mode client sends one message, waits for a reply,
then sends the next, your bandwidth number measures
`message_size / round_trip_time`, not the link's actual capacity. This
is exactly why `fi_pingpong` (a *library-provided* example tool used
elsewhere in this README) measures dramatically lower bandwidth than
`ib_write_bw` or this repo's own `fi_bw`/`dpdk_perf`/`ibv_bw` — it's an
unpipelined tool, not a libfabric limitation. See the README's "Why
libfabric's bandwidth is lower too" for the full story.

The fix is the same in all three programs: keep `window` operations
outstanding at once. Post `window` sends up front, then in a loop: poll
for a completion, and if more remain, post another into the just-freed
slot. `window=16` or higher gets you close to line rate; `window=1`
gives you a true (but throughput-capped) serialized measurement —
useful for latency mode, not for a bandwidth number.

One-sided operations help further where the API supports them:
`ibv_bw`'s throughput mode uses plain `RDMA_WRITE` (one-sided — the
receiver's CPU is never involved, no matching receive needed), which is
why it gets closest to `ib_write_bw`'s reference number of the three
tools here.

## Decision #3: latency mode — echo, with per-slot timestamps

Latency mode is a ping-pong: the client sends, the server echoes it
straight back, the client measures the round trip. To support a window
depth greater than 1 (useful for measuring completion latency *under
pipelined load*, not just serialized RTT — see the README's "Jitter and
latency distribution" for why that's a meaningful, separate
measurement), track a send timestamp *per window slot*, not a single
global one:

```cpp
std::vector<uint64_t> send_ns(window);
// on post:  send_ns[slot] = now_ns();
// on completion for that slot: rtt = now_ns() - send_ns[slot];
```

The server side just needs: pre-post `window` receive buffers, and on
each completion, send the same data straight back and re-post a receive
for that slot. `fi_bw` and `dpdk_perf` both use two-sided Send/Recv for
this. `ibv_bw` initially tried a one-sided `RDMA_WRITE_WITH_IMM` (the
one RDMA write variant that still consumes a receive WR and generates a
notification), which is a legitimate, more "authentically RDMA" way to
do it — but hit an unresolved hardware/firmware issue on this specific
NIC (see the README's `ibv_bw` section), so it fell back to plain
two-sided Send/Recv, same as the other two.

## Decision #4: report percentiles, not just an average

An average, or even average+stdev, can look perfectly healthy while
hiding a long tail of rare bad events. All three programs compute the
same three things from the same array of per-message latency samples:

```cpp
// stdev: spread around the mean
// RFC 3550 mean jitter: avg |sample[i] - sample[i-1]| -- consecutive-
//   sample variation, what real-time/AV jitter buffers care about
// percentiles: sort the samples, index by ceil(p * n) - 1
```

This is what actually surfaced the interesting findings in this repo —
e.g. that DPDK's tail was consistently far tighter than RDMA's, even
though RDMA's median was consistently lower. An average alone would
have missed that story entirely. If you're writing a new perf tool,
build the percentile reporting in from the start; retrofitting it after
you've already drawn conclusions from averages is how you end up having
to redo your measurements.

## What actually went wrong, per API (read this before you copy the pattern)

### libfabric (`fi_bw.cpp`)

- **`mode`/`mr_mode` hint bits.** The `verbs` provider requires
  `hints->mode = FI_CONTEXT | FI_RX_CQ_DATA` and specific `mr_mode`
  bits (`FI_MR_LOCAL | FI_MR_VIRT_ADDR | FI_MR_ALLOCATED |
  FI_MR_PROV_KEY`) — omit them and `fi_getinfo()` just returns nothing,
  with no obviously actionable error.
- **The `ep_type`-constrained source-query bug.** Constraining
  `hints->ep_attr->type = FI_EP_MSG` for the *server's* (source/listen)
  `fi_getinfo()` call breaks address resolution on this provider
  version; leave it unconstrained for that one call only (the domain
  only offers one type anyway, so nothing is lost).
- **A completion's `op_context` pointer doubles as an index.** Rather
  than a separate lookup table mapping completions back to buffer
  slots, allocate an array of `fi_context` and use pointer arithmetic
  (`(fi_context*)cqe.op_context - ctx.data()`) to recover the slot index
  directly. Small trick, used throughout.

### DPDK (`dpdk_perf.cpp`)

- **Isolate the port, install one narrow rule, never a wildcard
  capture.** `rte_flow_isolate()` before `rte_eth_dev_configure()`, then
  exactly one `rte_flow_create()` rule matching only your test traffic
  (a specific UDP port here). This is a safety requirement, not just
  good practice, if the port is shared with anything else (kernel
  networking, RDMA, another test) — the README's "DPDK compatibility"
  section covers why in depth.
- **Raw, non-standard EtherTypes aren't reliably classified.** An
  early version matched a private EtherType (`0x88B5`) directly; the
  sender side worked, but replies never arrived, and the likely cause
  was the NIC's flow-steering hardware not reliably handling arbitrary
  non-IP EtherTypes in isolated mode even though `rte_flow_create()`
  accepted the rule without error. Use real IPv4/UDP framing.
- **Ethernet's 60-byte minimum frame size is a hard requirement.**
  Frames under that get silently dropped as runts — and critically,
  `tx_burst()` still reports success for them, so this fails silently
  unless you check the receiver's actual counters.
- **`rte_eth_rx_burst(..., nb_pkts=1)` is a real, hard-to-spot PMD bug**
  on this mlx5 driver version: it silently and permanently returns 0,
  even with a matching packet waiting, no error, no log line. Always
  request a burst size larger than 1 (e.g. 32) and only consume the
  first result if that's all you need.

### Raw ibverbs (`ibv_bw.cpp`)

- **RoCE v2's GID table has multiple entries tagged the identical type
  string.** `ports/1/gid_attrs/types/<i>` says `"RoCE v2"` for *both* the
  always-present link-local IPv6 GID and the actual IPv4-mapped one you
  need — type alone doesn't distinguish them. Check the GID's own bytes
  (10 zero bytes, then `0xff 0xff`, is the IPv4-mapped signature)
  instead of trusting the type string.
- **A shared CQ needs headroom for both queues combined.** If
  `send_cq` and `recv_cq` point at the same CQ object (simplest for a
  test tool), size the CQ for `2 * qp_capacity`, not just
  `qp_capacity` — otherwise you get intermittent `ibv_post_send()`
  `ENOMEM` under sustained load once both queues are simultaneously
  busy.
- **`ibv_post_send()`/`ibv_post_recv()` return their error code
  directly — they do not set `errno`.** A generic error helper that
  reads `errno` on their failure will print a stale, unrelated error
  left over from an earlier syscall. This one cost real debugging time
  chasing a misleading "Protocol not supported" message that had
  nothing to do with the actual failure.
- **When something's flaky and code review doesn't find it, check the
  hardware's own counters before concluding it's an application bug.**
  `/sys/class/infiniband/<dev>/ports/1/hw_counters/` (fields like
  `local_ack_timeout_err`, `out_of_buffer`, `rnr_nak_retry_err`) gave a
  concrete, quantitative answer — an exact retry-count-matching jump in
  `local_ack_timeout_err` — that six rounds of code-level testing
  (opcode changes, buffer sizing, timing delays) couldn't have found on
  their own. See the README's `ibv_bw` section for the full trail.

## A debugging technique worth calling out on its own

When something fails intermittently and code review keeps coming up
empty, **swap which physical machine plays which role** before
assuming the bug is in one specific function. This repo's DPDK latency
bug looked, at first, like it might be a hardware quirk specific to one
host — running the exact same server/client code with the roles
reversed between the two machines and getting the identical failure
proved the bug followed the *code path* (the client-side receive
logic), not either machine's hardware. That one test eliminated an
entire category of hypotheses in one step.

## Where to go from here

- Read the actual source: `src/fi_bw.cpp`, `src/dpdk_perf.cpp`,
  `src/ibv_bw.cpp` — each has a header comment summarizing its own
  design decisions and known issues.
- The main [README.md](README.md) has the full measured results,
  comparison tables, and the complete investigation trails for every
  bug mentioned above, if you want the blow-by-blow rather than the
  summary here.
- `src/Makefile` has a `fi_bw-debug` target
  (`-O0 -g -fsanitize=address,undefined`) if you're extending one of
  these and want a debug build with sanitizers rather than the default
  optimized one.
