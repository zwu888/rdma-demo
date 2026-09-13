// dpdk_perf: a small DPDK throughput/latency test, mirroring fi_bw.cpp's
// role/window structure but against raw ethdev/rte_flow instead of
// libfabric verbs.
//
// Safety posture matches every DPDK test elsewhere in this repo: the
// port is put into rte_flow *isolated* mode and only ever gets one
// narrow, explicit flow rule (matching UDP dst port 5201, the same
// pattern proven to work in the testpmd tests -- see README) directing
// that traffic to queue 0. Nothing else is ever redirected away from
// the kernel/RDMA path.
//
// An earlier version matched on a raw, non-IP EtherType (0x88B5)
// instead of UDP; that was one of two bugs fixed during development
// (rewritten to real IPv4/UDP framing to match the proven-working
// testpmd pattern). The other, much less obvious one: the latency
// mode's round trip was calling rte_eth_rx_burst(port, queue, buf, 1)
// -- requesting exactly one packet -- which this mlx5 PMD silently
// returns 0 for, forever, even when a matching packet is waiting.
// Bumping the requested burst size to 32 (while still only consuming
// the first packet received) fixed it completely. Both are worth
// knowing if you're writing your own mlx5/DPDK code: don't assume a
// non-IP EtherType will be reliably classified in isolated mode, and
// don't call rte_eth_rx_burst() with nb_pkts=1 on this PMD.
//
// Usage:
//   dpdk_perf [EAL args] -- server|client <peer-mac> <own-ip> <peer-ip>
//       throughput|latency [size] [iters]
//
// Example (matches this repo's PCI addresses and RoCE-link IPs):
//   # server (hpz6g4)
//   sudo dpdk_perf -l 0-1 -n 4 -a 0000:2d:00.0 -- server
//       ec:0d:9a:a4:cc:86 192.168.100.2 192.168.100.1 throughput
//
//   # client (hpz8g4)
//   sudo dpdk_perf -l 0-1 -n 4 -a 0000:15:00.0 -- client
//       ec:0d:9a:78:62:72 192.168.100.1 192.168.100.2 throughput

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_udp.h>

#include <arpa/inet.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static constexpr uint16_t kUdpPort = 5201; // arbitrary, unused elsewhere
static constexpr uint16_t kPortId = 0;
static constexpr uint32_t kMaxSize = 1470; // stays under Ethernet MTU
static constexpr uint32_t kHdrSize =
    sizeof(rte_ether_hdr) + sizeof(rte_ipv4_hdr) + sizeof(rte_udp_hdr);
// Ethernet minimum frame size is 64B including the 4B hardware-appended
// CRC, so software must build at least 60B (header+payload). Frames
// smaller than this are runts and get silently dropped -- costly bug to
// miss, since tx_burst() still reports success for them.
static constexpr uint32_t kMinFrameSize = 60;

struct Payload {
    uint64_t seq;
    uint64_t send_ns; // client's monotonic clock at send, for latency mode
};

static void die(const char *what) {
    fprintf(stderr, "%s\n", what);
    exit(1);
}

static uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static void parse_mac(const std::string &s, rte_ether_addr *out) {
    unsigned b[6];
    if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3],
               &b[4], &b[5]) != 6) {
        die("invalid MAC address, expected aa:bb:cc:dd:ee:ff");
    }
    for (int i = 0; i < 6; i++) out->addr_bytes[i] = (uint8_t)b[i];
}

static uint32_t parse_ip(const std::string &s) {
    in_addr a{};
    if (inet_pton(AF_INET, s.c_str(), &a) != 1) die("invalid IPv4 address");
    return a.s_addr; // already network byte order
}

// Must run before rte_eth_dev_configure()/start(): tells the PMD this
// port will only ever see traffic matched by explicit rte_flow rules,
// installed separately by install_flow_rule() once the port is started.
static void enable_isolation() {
    rte_flow_error err{};
    if (rte_flow_isolate(kPortId, 1, &err) != 0)
        die((std::string("rte_flow_isolate: ") + err.message).c_str());
}

// Installs the one rule this whole program relies on: only UDP frames
// with dst port kUdpPort ever reach queue 0. Everything else (RDMA, SSH,
// the OOB control channels used elsewhere in this repo, ordinary IP
// traffic) keeps flowing through the kernel exactly as before -- this
// rule doesn't touch it. This mlx5 PMD requires the port to already be
// started before it will accept flow rule creation.
static void install_flow_rule() {
    rte_flow_error err{};
    rte_flow_attr attr{};
    attr.ingress = 1;

    rte_flow_item_udp udp_spec{};
    rte_flow_item_udp udp_mask{};
    udp_spec.hdr.dst_port = rte_cpu_to_be_16(kUdpPort);
    udp_mask.hdr.dst_port = 0xFFFF;

    rte_flow_item pattern[4] = {};
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
    pattern[2].type = RTE_FLOW_ITEM_TYPE_UDP;
    pattern[2].spec = &udp_spec;
    pattern[2].mask = &udp_mask;
    pattern[3].type = RTE_FLOW_ITEM_TYPE_END;

    rte_flow_action_queue queue_action{};
    queue_action.index = 0;

    rte_flow_action actions[2] = {};
    actions[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
    actions[0].conf = &queue_action;
    actions[1].type = RTE_FLOW_ACTION_TYPE_END;

    rte_flow *flow = rte_flow_create(kPortId, &attr, pattern, actions, &err);
    if (flow == nullptr)
        die((std::string("rte_flow_create: ") + err.message).c_str());
    printf("flow rule installed: udp dst port %u -> queue 0 (handle=%p)\n",
           kUdpPort, (void *)flow);
    fflush(stdout);
}

static rte_mempool *setup_port() {
    rte_mempool *pool =
        rte_pktmbuf_pool_create("mbuf_pool", 8191, 256, 0,
                                 RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (!pool) die("rte_pktmbuf_pool_create failed");

    enable_isolation();

    rte_eth_conf port_conf{};
    if (rte_eth_dev_configure(kPortId, 1, 1, &port_conf) != 0)
        die("rte_eth_dev_configure failed");
    if (rte_eth_rx_queue_setup(kPortId, 0, 256, rte_socket_id(), nullptr,
                                pool) != 0)
        die("rte_eth_rx_queue_setup failed");
    rte_eth_txconf txconf{};
    if (rte_eth_tx_queue_setup(kPortId, 0, 256, rte_socket_id(), &txconf) != 0)
        die("rte_eth_tx_queue_setup failed");

    if (rte_eth_dev_start(kPortId) != 0) die("rte_eth_dev_start failed");

    rte_eth_link link{};
    rte_eth_link_get(kPortId, &link);
    printf("link: %s, speed=%u Mbps\n",
           link.link_status == RTE_ETH_LINK_UP ? "UP" : "DOWN", link.link_speed);
    fflush(stdout);

    install_flow_rule();
    return pool;
}

static rte_mbuf *make_packet(rte_mempool *pool, const rte_ether_addr &src_mac,
                              const rte_ether_addr &dst_mac, uint32_t src_ip,
                              uint32_t dst_ip, uint64_t seq, uint32_t size) {
    if (size < kMinFrameSize) size = kMinFrameSize; // avoid silently-dropped runts
    rte_mbuf *m = rte_pktmbuf_alloc(pool);
    if (!m) die("rte_pktmbuf_alloc failed (pool exhausted)");

    auto *eth = (rte_ether_hdr *)rte_pktmbuf_append(m, sizeof(rte_ether_hdr));
    eth->dst_addr = dst_mac;
    eth->src_addr = src_mac;
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    uint16_t payload_len =
        (uint16_t)std::max<uint32_t>(sizeof(Payload), size - kHdrSize);

    auto *ip = (rte_ipv4_hdr *)rte_pktmbuf_append(m, sizeof(rte_ipv4_hdr));
    memset(ip, 0, sizeof(*ip));
    ip->version_ihl = 0x45;
    ip->total_length =
        rte_cpu_to_be_16(sizeof(rte_ipv4_hdr) + sizeof(rte_udp_hdr) + payload_len);
    ip->time_to_live = 64;
    ip->next_proto_id = IPPROTO_UDP;
    ip->src_addr = src_ip;
    ip->dst_addr = dst_ip;
    ip->hdr_checksum = rte_ipv4_cksum(ip);

    auto *udp = (rte_udp_hdr *)rte_pktmbuf_append(m, sizeof(rte_udp_hdr));
    udp->src_port = rte_cpu_to_be_16(kUdpPort);
    udp->dst_port = rte_cpu_to_be_16(kUdpPort);
    udp->dgram_len = rte_cpu_to_be_16(sizeof(rte_udp_hdr) + payload_len);
    udp->dgram_cksum = 0; // optional for IPv4, left disabled

    auto *p = (Payload *)rte_pktmbuf_append(m, sizeof(Payload));
    p->seq = seq;
    p->send_ns = now_ns();
    if (payload_len > sizeof(Payload))
        rte_pktmbuf_append(m, payload_len - sizeof(Payload));
    return m;
}

static void run_server_throughput() {
    rte_mbuf *bufs[32];
    uint64_t total_pkts = 0, total_bytes = 0;
    auto last_report = std::chrono::steady_clock::now();
    printf("server: counting UDP dst port %u frames (Ctrl+C to stop)\n",
           kUdpPort);
    fflush(stdout);
    while (true) {
        uint16_t n = rte_eth_rx_burst(kPortId, 0, bufs, 32);
        for (uint16_t i = 0; i < n; i++) {
            total_bytes += rte_pktmbuf_pkt_len(bufs[i]);
            rte_pktmbuf_free(bufs[i]);
        }
        total_pkts += n;
        auto now = std::chrono::steady_clock::now();
        if (now - last_report > std::chrono::seconds(2)) {
            printf("total: %lu pkts, %lu bytes\n", (unsigned long)total_pkts,
                   (unsigned long)total_bytes);
            fflush(stdout);
            last_report = now;
        }
    }
}

// Echoes every received frame straight back to its sender (src/dst MAC
// and IP swapped -- both swaps are checksum-invariant since IPv4's
// header checksum is a sum over both address words, unaffected by which
// one is "source" vs "destination"), for the client to measure
// round-trip latency against.
static void run_server_latency(const rte_ether_addr &own_mac, uint32_t own_ip) {
    rte_mempool *mp = rte_mempool_lookup("mbuf_pool");
    if (!mp) die("mbuf_pool not found");

    rte_mbuf *bufs[32];
    printf("server: echoing UDP dst port %u frames (Ctrl+C to stop)\n",
           kUdpPort);
    fflush(stdout);
    uint64_t total_rx = 0;
    auto last_report = std::chrono::steady_clock::now();
    while (true) {
        uint16_t n = rte_eth_rx_burst(kPortId, 0, bufs, 32);
        total_rx += n;
        auto now = std::chrono::steady_clock::now();
        if (now - last_report > std::chrono::seconds(2)) {
            printf("server: %lu frames received so far\n",
                   (unsigned long)total_rx);
            fflush(stdout);
            last_report = now;
        }

        // Build fresh reply mbufs rather than mutating the received ones
        // in place, to rule out leftover RX-context mbuf metadata (e.g.
        // offload flags, packet_type) confusing the TX path.
        rte_mbuf *replies[32];
        uint16_t nreplies = 0;
        for (uint16_t i = 0; i < n; i++) {
            auto *eth = rte_pktmbuf_mtod(bufs[i], rte_ether_hdr *);
            auto *ip = (rte_ipv4_hdr *)(eth + 1);
            auto *udp = (rte_udp_hdr *)(ip + 1);
            auto *payload = (Payload *)(udp + 1);
            uint32_t total_len = rte_pktmbuf_pkt_len(bufs[i]);
            rte_mbuf *reply = make_packet(mp, own_mac, eth->src_addr, own_ip,
                                           ip->src_addr, payload->seq, total_len);
            replies[nreplies++] = reply;
            rte_pktmbuf_free(bufs[i]);
        }
        if (nreplies > 0) {
            uint16_t sent = rte_eth_tx_burst(kPortId, 0, replies, nreplies);
            if (sent < nreplies) {
                printf("server: tx_burst only sent %u/%u echoes\n", sent,
                       nreplies);
                fflush(stdout);
            }
            for (uint16_t i = sent; i < nreplies; i++)
                rte_pktmbuf_free(replies[i]);
        }
    }
}

static void run_client_throughput(const rte_ether_addr &own_mac,
                                   const rte_ether_addr &peer_mac,
                                   uint32_t own_ip, uint32_t peer_ip,
                                   uint32_t size, long iters) {
    rte_mempool *mp = rte_mempool_lookup("mbuf_pool");
    if (!mp) die("mbuf_pool not found");

    printf("client: sending %ld frames of %u bytes...\n", iters, size);
    fflush(stdout);
    auto t0 = std::chrono::steady_clock::now();
    long sent_total = 0;
    while (sent_total < iters) {
        rte_mbuf *bufs[32];
        int burst = (int)std::min<long>(32, iters - sent_total);
        for (int i = 0; i < burst; i++)
            bufs[i] = make_packet(mp, own_mac, peer_mac, own_ip, peer_ip,
                                   sent_total + i, size);
        uint16_t sent = rte_eth_tx_burst(kPortId, 0, bufs, burst);
        for (uint16_t i = sent; i < burst; i++) rte_pktmbuf_free(bufs[i]);
        sent_total += sent;
    }
    auto t1 = std::chrono::steady_clock::now();
    double elapsed_s = std::chrono::duration<double>(t1 - t0).count();
    double gbits = (double)sent_total * size * 8.0 / elapsed_s / 1e9;
    double mpps = (double)sent_total / elapsed_s / 1e6;
    printf("sent %ld frames in %.3fs -> %.2f Gb/s, %.3f Mpps\n", sent_total,
           elapsed_s, gbits, mpps);
}

static void run_client_latency(const rte_ether_addr &own_mac,
                                const rte_ether_addr &peer_mac, uint32_t own_ip,
                                uint32_t peer_ip, long iters) {
    rte_mempool *mp = rte_mempool_lookup("mbuf_pool");
    if (!mp) die("mbuf_pool not found");

    printf("client: %ld round trips...\n", iters);
    fflush(stdout);
    std::vector<double> rtts_us;
    rtts_us.reserve(iters);
    long timeouts = 0;
    for (long i = 0; i < iters; i++) {
        rte_mbuf *tx = make_packet(mp, own_mac, peer_mac, own_ip, peer_ip, i,
                                    kMinFrameSize);
        uint64_t send_ns = now_ns();
        rte_mbuf *bufs[1] = {tx};
        if (rte_eth_tx_burst(kPortId, 0, bufs, 1) != 1) {
            rte_pktmbuf_free(tx);
            i--; // retry this iteration
            continue;
        }
        // wait for the echoed reply, bounded so a lost packet can't hang
        // the whole run -- report progress along the way for visibility.
        // Request a burst of 32 (matching the working server code), not 1:
        // requesting nb_pkts=1 from rte_eth_rx_burst() is the suspected bug.
        rte_mbuf *rx[32];
        uint16_t got = 0;
        while (got == 0 && (now_ns() - send_ns) < 1'000'000'000ULL)
            got = rte_eth_rx_burst(kPortId, 0, rx, 32);
        if (got == 0) {
            timeouts++;
            if (timeouts <= 5)
                fprintf(stderr, "iter %ld: no reply within 1s\n", i);
            continue;
        }
        uint64_t recv_ns = now_ns();
        for (uint16_t k = 0; k < got; k++) rte_pktmbuf_free(rx[k]);
        rtts_us.push_back((recv_ns - send_ns) / 1000.0);
        if (i > 0 && i % 500 == 0) {
            printf("... %ld/%ld done (%ld timeouts so far)\n", i, iters,
                   timeouts);
            fflush(stdout);
        }
    }
    if (timeouts > 0)
        printf("total timeouts: %ld/%ld\n", timeouts, iters);
    if (rtts_us.empty()) {
        printf("no successful round trips -- nothing to report\n");
        return;
    }
    double sum = 0, mn = rtts_us[0], mx = rtts_us[0];
    for (double v : rtts_us) {
        sum += v;
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    printf("RTT us: avg=%.3f min=%.3f max=%.3f (n=%zu)\n", sum / rtts_us.size(),
           mn, mx, rtts_us.size());
}

int main(int argc, char **argv) {
    int eal_argc = rte_eal_init(argc, argv);
    if (eal_argc < 0) die("rte_eal_init failed");
    argc -= eal_argc;
    argv += eal_argc;

    if (argc < 5) {
        fprintf(stderr,
                "usage: %s [EAL args] -- server|client <peer-mac> <own-ip> "
                "<peer-ip> [throughput|latency] [size] [iters]\n",
                argv[0]);
        return 1;
    }
    std::string role = argv[1];
    rte_ether_addr peer_mac{};
    parse_mac(argv[2], &peer_mac);
    uint32_t own_ip = parse_ip(argv[3]);
    uint32_t peer_ip = parse_ip(argv[4]);
    std::string mode = argc > 5 ? argv[5] : "throughput";
    uint32_t size = argc > 6 ? (uint32_t)std::stoul(argv[6]) : kMaxSize;
    long iters = argc > 7 ? std::stol(argv[7]) : 20000;

    setup_port();

    rte_ether_addr own_mac{};
    rte_eth_macaddr_get(kPortId, &own_mac);
    printf("port MAC: " RTE_ETHER_ADDR_PRT_FMT "\n",
           RTE_ETHER_ADDR_BYTES(&own_mac));
    fflush(stdout);

    if (role == "server") {
        if (mode == "latency")
            run_server_latency(own_mac, own_ip);
        else
            run_server_throughput();
    } else if (role == "client") {
        if (mode == "latency")
            run_client_latency(own_mac, peer_mac, own_ip, peer_ip, iters);
        else
            run_client_throughput(own_mac, peer_mac, own_ip, peer_ip, size,
                                   iters);
        rte_eth_stats stats{};
        rte_eth_stats_get(kPortId, &stats);
        printf("raw port stats: ipackets=%lu opackets=%lu ierrors=%lu "
               "imissed=%lu rx_nombuf=%lu\n",
               (unsigned long)stats.ipackets, (unsigned long)stats.opackets,
               (unsigned long)stats.ierrors, (unsigned long)stats.imissed,
               (unsigned long)stats.rx_nombuf);
    } else {
        die("role must be server or client");
    }

    rte_eth_dev_stop(kPortId);
    rte_eth_dev_close(kPortId);
    rte_eal_cleanup();
    return 0;
}
