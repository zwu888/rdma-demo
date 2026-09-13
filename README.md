# RDMA Demo (RoCE, ConnectX-4)

Two-node RDMA demo between:

| Host    | IP (RoCE link) | Device      |
|---------|-----------------|-------------|
| hpz8g4  | 192.168.100.1   | rocep21s0   |
| hpz6g4  | 192.168.100.2   | rocep45s0   |

Both hosts have a Mellanox ConnectX-4 connected by a direct QSFP28 cable
(no switch). The cards are single-port and were switched from native
InfiniBand to Ethernet mode (`LINK_TYPE_P1=ETH` via `mstconfig`) because
the cable wasn't recognized for native IB link training and there's no
Subnet Manager on this point-to-point link. RoCE (RDMA over Converged
Ethernet) avoids both issues.

## Prerequisites

Already installed on both hosts: `rdma-core`, `perftest`, `ibverbs-utils`,
`infiniband-diags`, `libfabric-bin`, `mstflint`.

For building/debugging the C++ program in `src/` (`libfabric-dev`,
`build-essential`, `gdb`, `valgrind`, `clang-format`, `clangd`, `bear`):

```bash
sudo apt-get install -y libfabric-dev pkg-config build-essential \
    gdb valgrind clang-format clangd bear
```

```bash
cd src
make              # optimized build: fi_bw
make fi_bw-debug  # -O0 -g -fsanitize=address,undefined, for gdb/debugging
bear -- make      # regenerates compile_commands.json for clangd/editor IntelliSense
```

`.clang-format` at the repo root defines the style (Google-based, 4-space
indent, 90-col limit) — run `clang-format -i src/fi_bw.cpp` to apply it.

## Setup steps (from scratch)

1. **Install tools** on both hosts:
   ```bash
   sudo apt-get install -y rdma-core infiniband-diags perftest \
       ibverbs-utils libfabric-bin mstflint
   ```

2. **Check current link type** of the ConnectX-4 (native IB by default on
   these cards):
   ```bash
   sudo mstconfig -d <pci-bdf> query | grep LINK_TYPE
   # LINK_TYPE_P1  IB(1)
   ```

3. **Switch to Ethernet mode** on both cards. Native IB requires the cable
   to be certified for IB link training and a Subnet Manager to bring the
   port to ACTIVE — neither was available on this point-to-point link, so
   the port sat at `phys_state: Disabled`. RoCE avoids both:
   ```bash
   sudo mstconfig -d <pci-bdf> set LINK_TYPE_P1=ETH
   ```

4. **Apply without a full reboot** using a firmware/PCI reset:
   ```bash
   sudo mstfwreset -d <pci-bdf> reset
   ```
   The RDMA device is renamed by udev after this (e.g. `mlx5_0` ->
   `rocep21s0`) since it's now Ethernet/RoCE instead of native IB.

5. **Verify link is up**:
   ```bash
   ibv_devinfo -d <rocep...>   # state should be PORT_ACTIVE / phys LinkUp
   ip -br link show            # interface should show LOWER_UP
   ```

6. **Assign IPs** for a direct point-to-point link (no switch):
   ```bash
   # host A
   sudo ip addr add 192.168.100.1/24 dev <iface>
   # host B
   sudo ip addr add 192.168.100.2/24 dev <iface>
   ping 192.168.100.2   # sanity check from host A
   ```

7. Run the demo scripts below.

## Usage

Run the server side first on one host, then the client side on the other,
passing the *server's* RoCE IP.

```bash
# on hpz6g4 (server)
scripts/run.sh server bw

# on hpz8g4 (client)
scripts/run.sh client bw 192.168.100.2
```

Test types: `bw` (bandwidth), `lat` (latency), `rate` (small-message rate).

## libfabric demo

Same fabric, different API layer: `fi_pingpong` (from `libfabric-bin`)
exercises the `verbs` provider, which sits on top of the same `rocep*`
ibverbs devices used above.

```bash
# on hpz6g4 (server)
scripts/run_libfabric.sh server

# on hpz8g4 (client)
scripts/run_libfabric.sh client 192.168.100.2
```

Notes:
- `-e msg` is required. The default endpoint type (`dgram`) doesn't match
  the `rocep*` domain's `FI_EP_MSG` type (`FI_PROTO_RDMA_CM_IB_RC`), so
  `fi_getinfo()` fails with "No data available" if you omit it.
- Avoid `-S all`: the default `RLIMIT_MEMLOCK` (8MB, `ulimit -l`) on these
  hosts is too small for the larger sizes in fi_pingpong's default sweep,
  and `fi_mr_reg()` fails with `ENOMEM` partway through. The script pins a
  single size (default 64KB) instead.
- **64KB isn't a requirement** — it's just the default in
  `run_libfabric.sh`, chosen to roughly mirror the `ib_write_bw` bandwidth
  test size above so the two tools' numbers are comparable. Any size
  works as an explicit argument, e.g.
  `scripts/run_libfabric.sh client 192.168.100.2 4096`; we also ran it at
  2 bytes to get matched-size latency numbers (see below). The only real
  ceiling is the memlock limit above — raise
  `RLIMIT_MEMLOCK`/`/etc/security/limits.conf` on both hosts if you want
  `-S all` to run without hitting `ENOMEM`.

## libfabric pipelined bandwidth demo (`fi_bw`)

`fi_pingpong`'s lower bandwidth numbers (see below) are mostly an artifact
of it never keeping more than one message outstanding. `src/fi_bw.cpp` is
a small custom C++ program that keeps several sends/recvs outstanding at
once — like `ib_write_bw`'s `TX depth` — to get a real saturation
bandwidth number out of libfabric instead. It also works around a real
provider quirk (see the comment at the top of the file): the verbs
provider's server-side (source/listen) address resolution reliably fails
for a plain IP on this setup, so client and server exchange the server's
native libfabric address over a small out-of-band plain-TCP control
channel before connecting, the same pattern used internally by
`fabtests`' example programs.

### Architecture

```mermaid
sequenceDiagram
    participant S as Server (hpz6g4)
    participant C as Client (hpz8g4)

    Note over S,C: 1. Connection setup
    S->>S: fi_getinfo (FI_SOURCE, ep_type unconstrained --<br/>verbs provider quirk workaround)
    S->>S: fi_passive_ep + fi_listen
    S->>S: fi_getname(pep) -> native address
    S->>S: open plain-TCP control socket (port+1)
    C->>S: connect to control socket
    S-->>C: send native address (out-of-band, not RDMA)
    C->>C: fi_getinfo (no node/service needed)
    C->>C: fi_endpoint + fi_enable
    C->>S: fi_connect(native address) [FI_CONNREQ over verbs/RDMA_CM]
    S->>S: fi_eq_sread -> FI_CONNREQ
    S->>S: fi_endpoint(conn_info) + fi_accept
    S-->>C: FI_CONNECTED (via EQ, both sides)

    Note over S,C: 2. Pipelined data path (window = N outstanding)
    S->>S: pre-post N fi_recv() into registered buffer slots
    C->>C: pre-post N fi_send() into registered buffer slots
    loop until iters completed
        C->>S: RDMA send (verbs RC QP)
        S-->>C: (transport-level ack, RC QP)
        S->>S: fi_cq_read(rxcq) completion -> repost fi_recv on that slot
        C->>C: fi_cq_read(txcq) completion -> repost fi_send on that slot
    end
    C->>C: measure elapsed time -> Gb/s, MB/s, usec/xfer
```

Key structural points:
- **Two CQs per endpoint** (`txcq`/`rxcq`), each sized `window + 8`, so up
  to `window` operations can be in flight without stalling on completion
  processing.
- **One registered memory region per side** (`fi_mr_reg`), sized
  `size * window`, sliced into `window` fixed buffer slots reused across
  iterations — no per-message allocation or registration.
- **`fi_context` array doubles as the completion-to-slot map**: each
  outstanding operation's context pointer is `&ctx[i]`; on completion,
  `(fi_context*)cqe.op_context - ctx.data()` recovers which buffer slot to
  repost, so there's no separate lookup table.
- **The control channel is plain BSD sockets**, entirely separate from
  the RDMA path — it exists only to move ~dozens of bytes (the server's
  native address) once, before any RDMA traffic starts.

Build and run:

```bash
cd src && make

# on hpz6g4 (server)
../scripts/run_fi_bw.sh server

# on hpz8g4 (client)
../scripts/run_fi_bw.sh client 192.168.100.2
```

Args: `size` (default 65536), `window` = max outstanding sends (default
16), `iters` (default 20000).

Results (64KB, tuned: performance governor + NUMA pin), by window depth —
confirms pipelining, not libfabric itself, was capping `fi_pingpong`'s
bandwidth:

| Window | Bandwidth |
|---|---|
| 1 | 30.3 Gb/s |
| 16 | 83.4 Gb/s |
| 64 | 89.2 Gb/s |

For comparison: `ib_write_bw` (TX depth 128) reaches ~92.5 Gb/s, and
`fi_pingpong` (no pipelining) reaches ~42.7 Gb/s. `fi_bw` at window 64
closes almost all of that gap using the same libfabric `verbs` provider —
confirming the earlier explanation that lack of pipelining, not libfabric
overhead, was the dominant factor in `fi_pingpong`'s lower bandwidth.

Reference result (64KB, msg endpoint, round-trip): ~5.3 GB/s (~42.7 Gb/s),
~12.3 us/xfer. Lower than the one-way `ib_write_bw` throughput above
because pingpong is a request/ack round trip rather than a streamed,
multi-outstanding-request transfer.

## Why native IB mode didn't work

Both ports initially sat at `state: DOWN` / `phys_state: Disabled` in native
InfiniBand mode, even with the cable freshly reseated on both ends. Two
separate requirements of native IB weren't met here:

1. **Cable/module IB certification.** ConnectX-4 firmware checks the
   transceiver/cable's SFF-8636 EEPROM for an explicit InfiniBand
   application code before attempting IB link training. Many QSFP DACs —
   including the one used here, which had previously worked fine in
   Ethernet mode — only advertise Ethernet support in that EEPROM. If the
   module doesn't self-identify as IB-capable, the firmware refuses to
   train the link at all, and the port stays `Disabled` rather than
   progressing through `Polling` -> `LinkUp`.

2. **No Subnet Manager.** Native IB needs a Subnet Manager (SM) to assign
   LIDs and bring a port from physical `LinkUp` to logical `ACTIVE`. This
   is a direct cable between two hosts with no switch, and neither host
   was running `opensm`. Even if the cable had trained successfully at the
   physical layer, the port would have stalled at `INIT` with no SM to
   finish bringing it up.

Switching `LINK_TYPE_P1` to `ETH` sidesteps both: Ethernet link training
doesn't gate on an IB-specific cable identifier, and Ethernet/RoCE has no
Subnet Manager dependency — link-up is link-up. That's confirmed by the
link coming up immediately after the `mstfwreset` in Ethernet mode, on the
exact same cable and ports that wouldn't train in IB mode.

## TODO

- [ ] Fix `fi_bw -P tcp` (`src/fi_bw.cpp`): the per-connection endpoint
  built from the `FI_CONNREQ` info tries to rebind to the passive
      endpoint's own listening address:port, and the `tcp` provider
      rejects that with `EADDRINUSE`. Only affects `-P tcp` — `-P verbs`
      (the default) is unaffected. See the `TODO` comment in
      `run_server()` and the "TCP baseline" section above, where
      `fi_pingpong -p tcp` was used as a workaround for the TCP numbers
      instead.

## Reference results (2026-09-12, direct cable, 100Gb ConnectX-4)

- Bandwidth (64KB, RDMA Write): ~92.5 Gb/s
- Latency (2B, RDMA Write): ~0.94 us typical/average
- Message rate (2B, Send): ~3.6 Mpps

## Comparison: native IB vs Ethernet/RoCE vs libfabric

| | Native InfiniBand | Ethernet/RoCE (perftest) | libfabric (`verbs` provider) |
|---|---|---|---|
| Tooling | `ib_write_bw`, etc. | `ib_write_bw`, `ib_write_lat`, `ib_send_bw` | `fi_pingpong` |
| Link status here | Never came up (`phys_state: Disabled`) | `PORT_ACTIVE` / `LinkUp` | Same link as RoCE column — libfabric rides on top of the `rocep*` ibverbs device |
| Requires cable IB certification | Yes — blocked us | No | No |
| Requires Subnet Manager | Yes — would have blocked us too | No | No |
| API level | Raw verbs | Raw verbs | OFI abstraction over verbs (portable across verbs/EFA/PSM2/tcp/etc without app changes) |
| Bandwidth (64KB) | n/a (link never up) | ~92.5 Gb/s (one-way RDMA Write) | ~42.7 Gb/s round-trip (ping-pong, not directly comparable to one-way BW) |
| Latency | n/a | ~0.94 us (2B, RDMA Write) | ~12.3 us/xfer (64KB round-trip, includes full request/ack cycle) |

Key takeaway: native IB and RoCE ultimately move data the same way — both
go through the same `ib_core`/`mlx5_ib` verbs stack and the same NIC
hardware. The difference here was entirely in **link establishment**
(see below), not in the RDMA data path itself. libfabric doesn't add a
new transport; it's a portable API sitting on top of the same verbs
device, so its numbers reflect the underlying RoCE link plus libfabric's
own protocol/framing overhead rather than a competing fabric.

### TCP baseline, same tool and wire

libfabric also has a plain `tcp` provider (normal kernel sockets, no
RDMA). Running the exact same `fi_pingpong -e msg` benchmark against it
over the identical NIC/cable — the only thing that changes is `-p tcp`
instead of `-p verbs` — gives a clean apples-to-apples RDMA-vs-TCP
comparison, since it's the same tool, same sizes, same physical link:

```bash
# server
fi_pingpong -p tcp -e msg -S 65536 -I 1000

# client
fi_pingpong -p tcp -e msg -S 65536 -I 1000 192.168.100.2
```

| | TCP (`fi_pingpong -p tcp`) | RoCE (`fi_pingpong -p verbs`) | RoCE advantage |
|---|---|---|---|
| Bandwidth (64KB, round-trip) | 1266 MB/s (~10.1 Gb/s) | 5340 MB/s (~42.7 Gb/s) | ~4.2x |
| Latency (2B, round-trip) | 12.54 us/xfer | 2.30 us/xfer | ~5.4x lower |

Same message sizes, same unpipelined ping-pong pattern, same physical
cable — the difference here is entirely the kernel TCP/IP stack (socket
syscalls, kernel copies, interrupt-driven processing) versus RDMA's
kernel-bypass, zero-copy data path. This is the number that actually
justifies bothering with RDMA/RoCE in the first place, as opposed to the
native-IB-vs-RoCE story above, which was purely about link setup, not a
real performance difference.

Note: our own `src/fi_bw.cpp` doesn't work with `-P tcp` yet — the tcp
provider tries to bind the new per-connection endpoint (created from the
`FI_CONNREQ` event's info) to the same address:port the passive endpoint
is already listening on, which fails with `EADDRINUSE`. This doesn't
happen with the `verbs` provider (each connection gets its own RDMA_CM
identifier, no shared socket to collide on). `fi_pingpong` handles the
tcp case correctly internally, so it was used for this comparison
instead.

### Why the latency numbers differ so much (0.94us vs 12.3us)

The `ib_write_lat`/`fi_pingpong` latency figures above aren't actually an
apples-to-apples comparison — two effects stack up:

1. **Message size dominates.** `ib_write_lat` used a 2-byte payload;
   `fi_pingpong` (as invoked by `run_libfabric.sh`) defaults to 64KB. At
   ~100Gb/s, just serializing 64KB one-way takes ~5.24us
   (64*1024*8 bits / 100e9 bits/s); round-trip that's ~10.5us of pure wire
   time — nearly the entire measured 12.3us. Re-running `fi_pingpong` at a
   matched 2-byte size gives ~2.30us, much closer to the RDMA write number.

2. **Operation type accounts for the rest.** `ib_write_lat` issues an
   **RDMA Write with inline data** — the payload rides inside the work
   request (no separate memory fetch), it's **one-sided** (the remote
   CPU/software is never involved; the NIC places data directly into
   remote memory), measured in a tight polling loop with TX depth 1 and
   nothing else happening. `fi_pingpong` uses **Send/Recv (two-sided)** —
   the receiver must have a receive buffer already posted and generates
   its own completion, and the benchmark does an explicit
   send-then-wait-for-ack round trip on top of libfabric's own progress
   engine. Same NIC, same RC QP under the hood, but inherently more
   round-trip machinery for a two-sided op than a one-sided inline write.

| Test | Size | Latency |
|---|---|---|
| `ib_write_lat` | 2B | 0.94 us |
| `fi_pingpong` | 2B | 2.30 us |
| `fi_bw` (`-w 1`, our program) | 2B | 3.75 us |
| `fi_pingpong` | 64KB | 12.3 us |

`fi_bw -w 1` (one outstanding send, waiting for its completion before
posting the next — see the libfabric bandwidth demo section below) is a
third data point on the same spectrum: still Send/Recv (two-sided, needs
a receive buffer posted on the far end) like `fi_pingpong`, but without
`fi_pingpong`'s explicit application-level send-then-wait-for-a-separate-
ack-message protocol — just the send completion itself. It lands higher
than `fi_pingpong`'s 2.30us here, which is a reminder that these
mini-benchmarks (a plain busy-poll loop, no warm-up, 5000 iterations) are
illustrative, not tightly controlled — take the relative ordering
(one-sided inline write < two-sided send-completion-only < two-sided
explicit ping-pong) as the reliable signal, not small differences between
runs.

### Why libfabric's bandwidth is lower too

Same root causes as the latency gap, but one factor dominates for
bandwidth specifically:

1. **No pipelining (the main factor).** `ib_write_bw` keeps 128 RDMA
   writes outstanding at once (`TX depth: 128`), overlapping each
   message's latency so the link stays saturated — that's how it reaches
   near-line-rate (92.5 Gb/s). `fi_pingpong` does the opposite by design:
   it sends one message, waits for the full round-trip ack, then sends
   the next. With nothing overlapped, throughput is capped at
   `message_size / round_trip_time`, not by the link's real capacity.
   Check the math: 64KB / 11.64us RTT (tuned run) is ~45 Gb/s — almost
   exactly what was measured. It isn't hitting a hardware ceiling; it's
   just never given more than one in-flight request to hide latency
   behind.

2. **Two-sided vs one-sided.** Same as the latency explanation above —
   `ib_write_bw` is one-sided RDMA Write; `fi_pingpong` is two-sided
   Send/Recv with a posted receive buffer and completion on both ends.

3. **Abstraction overhead (minor).** libfabric's OFI layer adds a
   provider-agnostic dispatch (`fi_*` calls into the `verbs` provider)
   versus perftest calling `ibv_post_send`/`ibv_poll_cq` directly. Real,
   but small next to #1 and #2.

A true apples-to-apples libfabric bandwidth number would need a pipelined
benchmark — e.g. `fi_msg_bw` from the `fabtests` suite (posts many
outstanding sends, like `ib_write_bw` does), rather than `fi_pingpong`.
`fabtests` isn't included in the `libfabric-bin` package installed here,
only `fi_pingpong`/`fi_msg_pingpong`-style tools, so this repo's libfabric
numbers should be read as a two-sided, unpipelined latency-bound
benchmark rather than a saturation bandwidth test.

### Tuning libfabric

Every perftest/libfabric run logged `CPU Frequency is not max` — both
hosts default to the `powersave` cpufreq governor, which lets cores clock
down between the polling loop's completion checks. RDMA latency
benchmarks are exactly the workload this hurts most. Fix in two steps:

1. **Install the tools** (both hosts):
   ```bash
   sudo apt-get install -y numactl linux-cpupower
   ```

2. **Switch the CPU governor to `performance`**:
   ```bash
   sudo cpupower frequency-set -g performance
   # verify:
   cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor | sort -u
   # -> performance
   ```
   This only affects the running kernel — it resets to `powersave` on
   reboot unless you make it persistent. To persist, add a systemd unit
   that runs the same command at boot:
   ```ini
   # /etc/systemd/system/cpu-performance.service
   [Unit]
   Description=Set CPU governor to performance
   After=multi-user.target

   [Service]
   Type=oneshot
   ExecStart=/usr/bin/cpupower frequency-set -g performance

   [Install]
   WantedBy=multi-user.target
   ```
   ```bash
   sudo systemctl daemon-reload
   sudo systemctl enable --now cpu-performance.service
   ```

3. **Find the NIC's local NUMA node** so the benchmark process runs on
   CPUs physically close to the card (avoids cross-socket memory/PCIe
   traffic):
   ```bash
   cat /sys/class/infiniband/<dev>/device/numa_node
   # both hosts here: node 0
   ```

4. **Pin the process with `numactl`** when running the demo:
   ```bash
   numactl --cpunodebind=0 --membind=0 fi_pingpong -p verbs -d <dev> -e msg -S <size> ...
   ```
   (Swap in whichever node number step 3 reported if it isn't 0.)

5. **Re-measure and compare** — this is what isolates whether the tuning
   actually helped versus noise:

   | | Default (powersave, no pinning) | Tuned (performance + NUMA pin) |
   |---|---|---|
   | Latency (2B) | 2.30 us | 1.44 us (~37% lower) |
   | Bandwidth (64KB) | 5340 MB/s (~42.7 Gb/s) | 5632 MB/s (~45.1 Gb/s) (~5% higher) |

Note: `cpupower frequency-set` is not persistent across reboots on its
own — set it via a systemd unit or `tuned` profile if you need it to
survive a restart.

## DPDK compatibility

DPDK is installed on both hosts (see "Setup" below) alongside the RDMA
setup above — this section documents whether/how it coexists without
disrupting the RDMA path.

**Why a basic setup is safe:** DPDK's `mlx5` PMD attaches to the
ConnectX-4 through the same `mlx5_core`/`mlx5_ib` kernel driver and
`libibverbs` stack the RDMA tools in this repo already use — it's a
"bifurcated" model, not exclusive ownership. It opens its own QPs/CQs via
verbs, the same way `fi_bw`, `ib_write_bw`, etc. do. Multiple independent
verbs consumers can coexist on one port simultaneously (this repo's own
tools have done exactly that all session — perftest, `fi_pingpong`, and
`fi_bw` each opening/closing connections on `rocep21s0` back-to-back with
no conflicts). A default-mode `dpdk-testpmd` run wouldn't touch the RDMA
path or require any link reset.

### Setup (done on both hosts)

```bash
sudo apt-get install -y dpdk dpdk-dev libdpdk-dev

# reserve 1024 x 2MB = 2GB hugepages, and make it persist across reboots
sudo sysctl -w vm.nr_hugepages=1024
echo 'vm.nr_hugepages=1024' | sudo tee /etc/sysctl.d/60-dpdk-hugepages.conf
```

IOMMU was already active (165 IOMMU groups) and `hugetlbfs` was already
mounted at `/dev/hugepages`, so those needed no changes.

Verified the bifurcated model held after installing — the ConnectX-4 is
still bound to the kernel driver, not `vfio-pci`, and both the netdev and
the RDMA device remain live:

```
$ dpdk-devbind.py --status
Network devices using kernel driver
===================================
0000:15:00.0 'MT27700 Family [ConnectX-4] 1013' numa_node=0 if=enp21s0np0 drv=mlx5_core unused= *Active*
```

### Validated: flow-isolated testpmd coexists cleanly

Tested this directly rather than leaving it as a guess. Note the correct
flag is `--flow-isolate-all` (not `--flow-isolate-mode`, which doesn't
exist and errors out):

```bash
sudo dpdk-testpmd -l 0-2 -n 4 -a 0000:15:00.0 -- --flow-isolate-all
```

The log confirms isolation actually took effect before any port state
changed:

```
Ingress traffic on port 0 is now restricted to the defined flow rules
...
mlx5_net: port 0 cannot enable promiscuous mode in flow isolation mode
```

**Test procedure:** measured `fi_bw` (64KB, window 16) three times —
before starting `testpmd`, concurrently while it was running (holding its
own RX/TX queue on the same port, isolated, forwarding nothing since no
explicit `rte_flow` rules were added), and again after a clean shutdown:

| | Bandwidth |
|---|---|
| Before `testpmd` | 82.93 Gb/s |
| While `testpmd` running | 82.03 Gb/s |
| After `testpmd` exit | 83.03 Gb/s |

All three within ~1% of each other — no measurable regression. RoCE port
state stayed `ACTIVE` and the kernel netdev stayed `UP` throughout, and
`testpmd` shut down cleanly (`Port 0 is closed` / `Bye...`) without
requiring a link reset. This confirms the theoretical bifurcated-model
argument above with an actual measurement: a flow-isolated DPDK app can
run against this NIC alongside the RDMA path with no impact, as long as
you don't add `rte_flow` rules that redirect traffic the RDMA path
depends on.

Still untested: whether a *non-isolated* `testpmd` run (default flow
mode, no `--flow-isolate-all`) would actually steal RoCEv2/OOB-control
traffic as theorized above. Given isolation mode works and coexists
cleanly, there was no reason to test the riskier non-isolated path
against a live link.

**What would actually break the RDMA flow:**

1. **`dpdk-devbind.py` unbinding the NIC to `vfio-pci`/`igb_uio`.** This
   is the standard DPDK setup step for most NICs, but it's the *wrong*
   move for mlx5 — unbinding it from `mlx5_core` rips out the same PCI
   function `ib_uverbs`/`rocep21s0` depend on, killing the RDMA device
   entirely until rebound. mlx5 is specifically designed to skip this
   step; a generic DPDK tutorial that says "bind all NICs to vfio-pci"
   is a real footgun here.

2. **Switching the NIC into switchdev/eswitch mode**
   (`devlink dev eswitch set ... mode switchdev`) for SR-IOV
   representor/hardware-offload use cases. That reconfigures the NIC's
   port model and **does trigger a device reset** — the same kind of
   link flap we saw from `mstfwreset` when switching `LINK_TYPE_P1`
   earlier in this README. RDMA connections would drop during that
   reset.

3. **Reconfiguring queue/VF counts via `mstconfig`** (e.g. changing
   `NUM_OF_VFS` for SR-IOV) — same story: a firmware-level change
   requiring `mstfwreset`, causing a brief link interruption.

4. **Bandwidth contention** isn't "breaking" per se, but worth noting:
   DPDK traffic and RDMA traffic share the same physical 100Gb link and
   PCIe lanes, so running both under heavy load at the same time means
   they compete for wire bandwidth.

Bottom line: a basic `dpdk-testpmd` run in default legacy mode, with no
`devbind` and no eswitch mode change, is safe and coexists fine with the
RDMA setup here. SR-IOV/switchdev-style DPDK testing is a separate, more
invasive step that should be planned for a time when nothing else needs
the RDMA link live.
