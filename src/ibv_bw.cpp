// ibv_bw: a small pipelined raw-ibverbs bandwidth/latency test, completing
// the trio in this repo alongside fi_bw.cpp (libfabric) and dpdk_perf.cpp
// (raw DPDK ethdev). No libfabric, no rdma_cm -- just libibverbs directly,
// with QP setup done the classic way (RESET->INIT->RTR->RTS by hand,
// GID/QPN/PSN/rkey/addr exchanged over a plain out-of-band TCP socket),
// the same pattern used in canonical raw-verbs examples like the rdma-core
// `ibv_rc_pingpong` tool.
//
// Throughput mode: pipelined one-sided RDMA_WRITE (like ib_write_bw's own
// methodology) -- the server does nothing but hold the QP open; the
// client's own send-completion is what's measured, same as perftest.
//
// Latency mode: two-sided Send/Recv ping-pong (an earlier version used
// RDMA_WRITE_WITH_IMM; see the "Known issue" comment on run_server_latency
// for why that was dropped) -- the server echoes back on receiving a
// Send, the same way fi_bw/dpdk_perf's servers echo their own two-sided
// messages.
//
// Usage:
//   ibv_bw server|client <peer-ip> -d <device> [-s size] [-w window]
//       [-n iters] [throughput|latency]
//
// Example (matches this repo's RoCE-link IPs and device names):
//   # server (hpz6g4)
//   ibv_bw server -d rocep45s0 -s 65536 -w 16 -n 20000 throughput
//
//   # client (hpz8g4)
//   ibv_bw client 192.168.100.2 -d rocep21s0 -s 65536 -w 16 -n 20000 throughput

#include <infiniband/verbs.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

struct Config {
    bool is_server = false;
    std::string server_ip;
    std::string device;
    std::string port = "18515"; // OOB control port (arbitrary, unused elsewhere)
    size_t size = 65536;
    int window = 16;
    long iters = 20000;
    std::string mode = "throughput";
};

static void die(const char *what) {
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
    exit(1);
}

static void die_msg(const std::string &what) {
    fprintf(stderr, "%s\n", what.c_str());
    exit(1);
}

// ibv_post_send()/ibv_post_recv() return the error code directly as
// their return value -- they don't set the global errno. Using die()
// (which reads errno) on their return would print a stale, unrelated
// error left over from some earlier syscall.
static void die_rc(const char *what, int rc) {
    fprintf(stderr, "%s: %s\n", what, strerror(rc));
    exit(1);
}

static uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// --- out-of-band TCP control channel: exchanges QP/MR info needed to
// bring both sides' QPs up. Not part of the RDMA data path.

static void oob_send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) die("send");
        p += n;
        len -= n;
    }
}

static void oob_recv_all(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) die_msg("control channel closed early");
        p += n;
        len -= n;
    }
}

static int oob_listen_accept(const Config &cfg) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons((uint16_t)std::stoi(cfg.port));
    if (bind(lfd, (sockaddr *)&sin, sizeof(sin)) < 0) die("bind");
    if (listen(lfd, 1) < 0) die("listen");
    printf("control channel listening on port %s ...\n", cfg.port.c_str());
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) die("accept");
    close(lfd);
    return cfd;
}

static int oob_connect(const Config &cfg) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)std::stoi(cfg.port));
    if (inet_pton(AF_INET, cfg.server_ip.c_str(), &sin.sin_addr) != 1)
        die_msg("invalid server IP");
    int rc = -1;
    for (int attempt = 0; attempt < 50; attempt++) {
        rc = connect(fd, (sockaddr *)&sin, sizeof(sin));
        if (rc == 0) break;
        usleep(100000);
    }
    if (rc < 0) die("connect");
    return fd;
}

// Everything needed to bring the peer's QP to RTR and target our MR.
struct QpInfo {
    uint32_t qpn;
    uint32_t psn;
    uint8_t gid[16];
    uint64_t addr;
    uint32_t rkey;
} __attribute__((packed));

static QpInfo exchange_qp_info(int fd, const QpInfo &local) {
    QpInfo remote{};
    oob_send_all(fd, &local, sizeof(local));
    oob_recv_all(fd, &remote, sizeof(remote));
    return remote;
}

// --- ibverbs setup

// RoCE v2 has multiple GID table entries tagged type "RoCE v2": one
// derived from the link-local IPv6 address (always present, but not
// routable to a plain IPv4 peer -- using it fails ibv_modify_qp(RTR)
// with ENETUNREACH) and one IPv4-mapped (::ffff:a.b.c.d) entry that
// actually is. Type alone doesn't distinguish them; the GID's own bytes
// do (bytes 0-9 zero, bytes 10-11 = 0xff 0xff for an IPv4-mapped
// address). Check both, the same way perftest tools do.
static int find_roce_v2_gid_index(const std::string &device,
                                   ibv_context *context) {
    for (int i = 0; i < 16; i++) {
        std::string path = "/sys/class/infiniband/" + device +
                            "/ports/1/gid_attrs/types/" + std::to_string(i);
        std::ifstream f(path);
        if (!f) continue;
        std::string type;
        std::getline(f, type);
        if (type.find("RoCE v2") == std::string::npos) continue;

        ibv_gid gid{};
        if (ibv_query_gid(context, 1, i, &gid) != 0) continue;
        bool ipv4_mapped = true;
        for (int b = 0; b < 10; b++)
            if (gid.raw[b] != 0) ipv4_mapped = false;
        if (gid.raw[10] != 0xff || gid.raw[11] != 0xff) ipv4_mapped = false;
        if (ipv4_mapped) return i;
    }
    die_msg("no IPv4-mapped RoCE v2 GID found");
    return -1;
}

struct Ctx {
    ibv_context *context = nullptr;
    ibv_pd *pd = nullptr;
    ibv_cq *cq = nullptr;
    ibv_qp *qp = nullptr;
    int gid_index = 0;
    ibv_gid gid{};
};

static Ctx setup_verbs(const Config &cfg, int qp_cap) {
    Ctx c;
    int ndev = 0;
    ibv_device **devs = ibv_get_device_list(&ndev);
    if (!devs) die("ibv_get_device_list");
    ibv_device *target = nullptr;
    for (int i = 0; i < ndev; i++)
        if (cfg.device == ibv_get_device_name(devs[i])) target = devs[i];
    if (!target) die_msg("device not found: " + cfg.device);

    c.context = ibv_open_device(target);
    if (!c.context) die("ibv_open_device");
    ibv_free_device_list(devs);

    c.pd = ibv_alloc_pd(c.context);
    if (!c.pd) die("ibv_alloc_pd");

    // send_cq and recv_cq below are the SAME cq object, so its capacity
    // must cover both queues' outstanding completions combined, not
    // just one queue's worth -- undersizing this (e.g. to just qp_cap)
    // causes a CQ overrun under sustained load: ibv_post_send() starts
    // failing with ENOMEM once the shared CQ can't hold both sides'
    // pending completions at once.
    int cq_size = qp_cap * 2 + 16;
    c.cq = ibv_create_cq(c.context, cq_size, NULL, NULL, 0);
    if (!c.cq) die("ibv_create_cq");

    ibv_qp_init_attr qp_attr{};
    qp_attr.send_cq = c.cq;
    qp_attr.recv_cq = c.cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr = qp_cap;
    qp_attr.cap.max_recv_wr = qp_cap;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;
    qp_attr.cap.max_inline_data = 0;
    c.qp = ibv_create_qp(c.pd, &qp_attr);
    if (!c.qp) die("ibv_create_qp");

    ibv_qp_attr init_attr{};
    init_attr.qp_state = IBV_QPS_INIT;
    init_attr.port_num = 1;
    init_attr.pkey_index = 0;
    init_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    if (ibv_modify_qp(c.qp, &init_attr,
                       IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                           IBV_QP_ACCESS_FLAGS) != 0)
        die("ibv_modify_qp(INIT)");

    c.gid_index = find_roce_v2_gid_index(cfg.device, c.context);
    if (ibv_query_gid(c.context, 1, c.gid_index, &c.gid) != 0)
        die("ibv_query_gid");

    return c;
}

static void bring_up_qp(Ctx &c, uint32_t my_psn, const QpInfo &remote) {
    ibv_qp_attr rtr{};
    rtr.qp_state = IBV_QPS_RTR;
    rtr.path_mtu = IBV_MTU_1024;
    rtr.dest_qp_num = remote.qpn;
    rtr.rq_psn = remote.psn;
    rtr.max_dest_rd_atomic = 1;
    rtr.min_rnr_timer = 12;
    rtr.ah_attr.is_global = 1;
    memcpy(rtr.ah_attr.grh.dgid.raw, remote.gid, 16);
    rtr.ah_attr.grh.sgid_index = c.gid_index;
    rtr.ah_attr.grh.hop_limit = 1;
    rtr.ah_attr.port_num = 1;
    if (ibv_modify_qp(c.qp, &rtr,
                       IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                           IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                           IBV_QP_MAX_DEST_RD_ATOMIC |
                           IBV_QP_MIN_RNR_TIMER) != 0)
        die("ibv_modify_qp(RTR)");

    ibv_qp_attr rts{};
    rts.qp_state = IBV_QPS_RTS;
    rts.timeout = 14;
    rts.retry_cnt = 7;
    rts.rnr_retry = 7;
    rts.sq_psn = my_psn;
    rts.max_rd_atomic = 1;
    if (ibv_modify_qp(c.qp, &rts,
                       IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                           IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                           IBV_QP_MAX_QP_RD_ATOMIC) != 0)
        die("ibv_modify_qp(RTS)");
}

static void print_latency_stats(std::vector<double> &lat_us) {
    if (lat_us.empty()) return;
    double sum = 0, mn = lat_us[0], mx = lat_us[0];
    for (double v : lat_us) {
        sum += v;
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    double avg = sum / lat_us.size();
    double var_sum = 0;
    for (double v : lat_us) var_sum += (v - avg) * (v - avg);
    double stdev = std::sqrt(var_sum / lat_us.size());
    double rfc3550_sum = 0;
    for (size_t i = 1; i < lat_us.size(); i++)
        rfc3550_sum += std::fabs(lat_us[i] - lat_us[i - 1]);
    double rfc3550_jitter =
        lat_us.size() > 1 ? rfc3550_sum / (lat_us.size() - 1) : 0.0;

    std::vector<double> sorted = lat_us;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) {
        size_t idx = (size_t)std::ceil(p * sorted.size()) - 1;
        idx = std::min(idx, sorted.size() - 1);
        return sorted[idx];
    };
    printf("latency us: avg=%.3f min=%.3f max=%.3f stdev=%.3f "
           "jitter(rfc3550)=%.3f (n=%zu)\n",
           avg, mn, mx, stdev, rfc3550_jitter, lat_us.size());
    printf("latency us percentiles: p50=%.3f p90=%.3f p99=%.3f p99.9=%.3f "
           "max=%.3f\n",
           pct(0.50), pct(0.90), pct(0.99), pct(0.999), sorted.back());
}

static void run_throughput_client(const Config &cfg, Ctx &c, char *buf,
                                   uint64_t remote_addr, uint32_t remote_rkey,
                                   ibv_mr *mr) {
    printf("client: sending %ld RDMA writes of %zu bytes...\n", cfg.iters,
           cfg.size);
    long posted = 0, completed = 0;
    auto t0 = std::chrono::steady_clock::now();
    long window_used = 0;
    while (completed < cfg.iters) {
        while (posted < cfg.iters && window_used < cfg.window) {
            size_t slot = posted % cfg.window;
            ibv_sge sge{};
            sge.addr = (uint64_t)(buf + slot * cfg.size);
            sge.length = cfg.size;
            sge.lkey = mr->lkey;
            ibv_send_wr wr{};
            wr.wr_id = slot;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            wr.opcode = IBV_WR_RDMA_WRITE;
            wr.send_flags = IBV_SEND_SIGNALED;
            wr.wr.rdma.remote_addr = remote_addr + slot * cfg.size;
            wr.wr.rdma.rkey = remote_rkey;
            ibv_send_wr *bad = nullptr;
            int rc = ibv_post_send(c.qp, &wr, &bad);
            if (rc != 0) die_rc("ibv_post_send", rc);
            posted++;
            window_used++;
        }
        ibv_wc wc{};
        int n = ibv_poll_cq(c.cq, 1, &wc);
        if (n < 0) die("ibv_poll_cq");
        if (n == 0) continue;
        if (wc.status != IBV_WC_SUCCESS)
            die_msg(std::string("send completion error: ") +
                    ibv_wc_status_str(wc.status));
        completed++;
        window_used--;
    }
    auto t1 = std::chrono::steady_clock::now();
    double elapsed_s = std::chrono::duration<double>(t1 - t0).count();
    double gbits = (double)cfg.iters * cfg.size * 8.0 / elapsed_s / 1e9;
    double mbps = (double)cfg.iters * cfg.size / elapsed_s / 1e6;
    double mmsgs = (double)cfg.iters / elapsed_s / 1e6;
    printf("bytes\titers\twindow\ttime[s]\tBW[Gb/s]\tMB/s\tMmsgs/s\n");
    printf("%zu\t%ld\t%d\t%.3f\t%.2f\t\t%.2f\t%.3f\n", cfg.size, cfg.iters,
           cfg.window, elapsed_s, gbits, mbps, mmsgs);
}

// Latency mode uses plain two-sided Send/Recv rather than
// RDMA_WRITE_WITH_IMM. An earlier version used WRITE_WITH_IMM (the one
// RDMA write variant that can still notify the passive side, since it
// consumes a posted receive WR) -- ibv_post_send() for it failed
// intermittently with EPROTONOSUPPORT/ENOMEM on this ConnectX-4
// firmware in ways that didn't track any QP/CQ sizing issue found (a
// real CQ-sizing bug was found and fixed along the way (see
// setup_verbs), but didn't explain this). Two-sided Send/Recv needs no remote
// addr/rkey at all, is universally supported, and is what fi_bw and
// dpdk_perf already use for their own echo logic elsewhere in this
// repo -- simpler and more consistent than chasing the WITH_IMM issue
// further. Throughput mode's one-sided RDMA_WRITE is unaffected by any
// of this and still demonstrates true one-sided RDMA.
static void run_latency_client(const Config &cfg, Ctx &c, char *buf,
                                ibv_mr *mr) {
    // Pre-post window receive WRs for the server's echoed replies.
    for (int i = 0; i < cfg.window; i++) {
        ibv_sge sge{};
        sge.addr = (uint64_t)(buf + i * cfg.size);
        sge.length = cfg.size;
        sge.lkey = mr->lkey;
        ibv_recv_wr wr{};
        wr.wr_id = i;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        ibv_recv_wr *bad = nullptr;
        int rc = ibv_post_recv(c.qp, &wr, &bad);
        if (rc != 0) die_rc("ibv_post_recv", rc);
    }

    printf("client: %ld round trips...\n", cfg.iters);
    std::vector<uint64_t> send_ns(cfg.window);
    std::vector<double> lat_us;
    lat_us.reserve(cfg.iters);

    long posted_send = 0, completed_send = 0, completed_recv = 0;
    long window_used = 0;
    while (completed_recv < cfg.iters) {
        while (posted_send < cfg.iters && window_used < cfg.window) {
            size_t slot = posted_send % cfg.window;
            send_ns[slot] = now_ns();
            ibv_sge sge{};
            sge.addr = (uint64_t)(buf + (cfg.window + slot) * cfg.size);
            sge.length = cfg.size;
            sge.lkey = mr->lkey;
            ibv_send_wr wr{};
            wr.wr_id = 0x8000000000000000ULL | slot;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            wr.opcode = IBV_WR_SEND;
            wr.send_flags = IBV_SEND_SIGNALED;
            ibv_send_wr *bad = nullptr;
            int rc = ibv_post_send(c.qp, &wr, &bad);
            if (rc != 0) die_rc("ibv_post_send", rc);
            posted_send++;
            window_used++;
        }
        ibv_wc wc{};
        int n = ibv_poll_cq(c.cq, 1, &wc);
        if (n < 0) die("ibv_poll_cq");
        if (n == 0) continue;
        if (wc.status != IBV_WC_SUCCESS)
            die_msg(std::string("completion error: ") +
                    ibv_wc_status_str(wc.status) + " (vendor_err=" +
                    std::to_string(wc.vendor_err) + ", opcode=" +
                    std::to_string(wc.opcode) + ")");
        if (wc.wr_id & 0x8000000000000000ULL) {
            completed_send++;
            window_used--;
        } else if (wc.opcode == IBV_WC_RECV) {
            size_t slot = wc.wr_id;
            lat_us.push_back((now_ns() - send_ns[slot]) / 1000.0);
            completed_recv++;
            // repost this recv slot for a future reply
            ibv_sge sge{};
            sge.addr = (uint64_t)(buf + slot * cfg.size);
            sge.length = cfg.size;
            sge.lkey = mr->lkey;
            ibv_recv_wr wr{};
            wr.wr_id = slot;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            ibv_recv_wr *bad = nullptr;
            int rc = ibv_post_recv(c.qp, &wr, &bad);
            if (rc != 0) die_rc("ibv_post_recv", rc);
            if (completed_recv > 0 && completed_recv % 500 == 0)
                printf("... %ld/%ld done\n", completed_recv, cfg.iters);
        }
    }
    print_latency_stats(lat_us);
}

// Server for latency mode: echoes every Send it receives straight back
// to the sender via its own Send. Throughput mode's server needs
// nothing beyond holding the QP open, since RDMA_WRITE is one-sided.
static void run_server_latency(const Config &cfg, Ctx &c, char *buf,
                                ibv_mr *mr) {
    for (int i = 0; i < cfg.window; i++) {
        ibv_sge sge{};
        sge.addr = (uint64_t)(buf + i * cfg.size);
        sge.length = cfg.size;
        sge.lkey = mr->lkey;
        ibv_recv_wr wr{};
        wr.wr_id = i;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        ibv_recv_wr *bad = nullptr;
        int rc = ibv_post_recv(c.qp, &wr, &bad);
        if (rc != 0) die_rc("ibv_post_recv", rc);
    }
    printf("server: echoing Send/Recv messages (Ctrl+C to stop)\n");
    uint64_t total_rx = 0;
    while (true) {
        ibv_wc wc{};
        int n = ibv_poll_cq(c.cq, 1, &wc);
        if (n < 0) die("ibv_poll_cq");
        if (n == 0) continue;
        if (wc.status != IBV_WC_SUCCESS)
            die_msg(std::string("completion error: ") +
                    ibv_wc_status_str(wc.status) + " (vendor_err=" +
                    std::to_string(wc.vendor_err) + ", opcode=" +
                    std::to_string(wc.opcode) + ")");
        if (wc.opcode == IBV_WC_RECV) {
            size_t slot = wc.wr_id;
            total_rx++;
            ibv_sge sge{};
            sge.addr = (uint64_t)(buf + slot * cfg.size);
            sge.length = cfg.size;
            sge.lkey = mr->lkey;
            ibv_send_wr wr{};
            wr.wr_id = 0x8000000000000000ULL | slot;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            wr.opcode = IBV_WR_SEND;
            wr.send_flags = IBV_SEND_SIGNALED;
            ibv_send_wr *bad = nullptr;
            int rc = ibv_post_send(c.qp, &wr, &bad);
            if (rc != 0) die_rc("ibv_post_send", rc);

            ibv_recv_wr rwr{};
            rwr.wr_id = slot;
            rwr.sg_list = &sge;
            rwr.num_sge = 1;
            ibv_recv_wr *rbad = nullptr;
            int rc2 = ibv_post_recv(c.qp, &rwr, &rbad);
            if (rc2 != 0) die_rc("ibv_post_recv", rc2);
        }
        if (total_rx % 1000 == 1) printf("server: %lu received so far\n",
                                          (unsigned long)total_rx);
    }
}

int main(int argc, char **argv) {
    Config cfg;
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s server|client [server_ip] -d <device> [-s size] "
                "[-w window] [-n iters] [-p port] [throughput|latency]\n",
                argv[0]);
        return 1;
    }
    cfg.is_server = std::string(argv[1]) == "server";
    int i = 2;
    if (!cfg.is_server) {
        if (argc < 3) die_msg("client requires <server_ip>");
        cfg.server_ip = argv[2];
        i = 3;
    }
    for (; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) die_msg("missing value for " + a);
            return argv[++i];
        };
        if (a == "-d") cfg.device = next();
        else if (a == "-s") cfg.size = std::stoul(next());
        else if (a == "-w") cfg.window = std::stoi(next());
        else if (a == "-n") cfg.iters = std::stol(next());
        else if (a == "-p") cfg.port = next();
        else if (a == "throughput" || a == "latency") cfg.mode = a;
        else die_msg("unknown argument: " + a);
    }
    if (cfg.device.empty()) die_msg("-d <device> is required");

    Ctx c = setup_verbs(cfg, cfg.window * 2 + 8);

    size_t slots = (size_t)cfg.window * (cfg.mode == "latency" ? 2 : 1);
    std::vector<char> buf(cfg.size * slots, 'x');
    ibv_mr *mr = ibv_reg_mr(c.pd, buf.data(), buf.size(),
                             IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                                 IBV_ACCESS_REMOTE_READ);
    if (!mr) die("ibv_reg_mr");

    QpInfo local{};
    local.qpn = c.qp->qp_num;
    local.psn = lrand48() & 0xffffff;
    memcpy(local.gid, c.gid.raw, 16);
    local.addr = (uint64_t)buf.data();
    local.rkey = mr->rkey;

    int fd = cfg.is_server ? oob_listen_accept(cfg) : oob_connect(cfg);
    QpInfo remote = exchange_qp_info(fd, local);
    close(fd);

    bring_up_qp(c, local.psn, remote);
    printf("QP up: local qpn=%u <-> remote qpn=%u\n", local.qpn, remote.qpn);

    if (cfg.is_server) {
        if (cfg.mode == "latency") {
            run_server_latency(cfg, c, buf.data(), mr);
        } else {
            printf("server: holding QP open for one-sided RDMA writes "
                   "(Ctrl+C to stop)\n");
            while (true) pause();
        }
    } else {
        if (cfg.mode == "latency")
            run_latency_client(cfg, c, buf.data(), mr);
        else
            run_throughput_client(cfg, c, buf.data(), remote.addr,
                                   remote.rkey, mr);
    }

    ibv_dereg_mr(mr);
    ibv_destroy_qp(c.qp);
    ibv_destroy_cq(c.cq);
    ibv_dealloc_pd(c.pd);
    ibv_close_device(c.context);
    return 0;
}
