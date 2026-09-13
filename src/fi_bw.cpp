// fi_bw: a small pipelined libfabric MSG bandwidth/latency demo.
//
// fi_pingpong (used elsewhere in this repo) sends one message, waits for
// the full round-trip ack, then sends the next -- so its throughput is
// capped at message_size / round_trip_time rather than the link's real
// capacity (see README's "Why libfabric's bandwidth is lower too"). This
// program instead keeps several sends/recvs outstanding at once (like
// ib_write_bw's TX depth), to get a saturation-style bandwidth number
// out of libfabric for a fair comparison.
//
// Connection setup uses a small out-of-band plain-TCP control channel to
// exchange the server's native fi_getname() address with the client,
// rather than having the client resolve the server by IP through
// fi_getinfo/rdma_cm. On this verbs provider version, rdma_bind_addr()
// on a plain sockaddr_in for a *source* (listening) query reliably fails
// with EADDRNOTAVAIL for this domain, and the fallback path that does
// work internally uses native FI_SOCKADDR_IB addressing -- which a
// client dialing the server's IP string has no way to end up on. Passing
// the server's real address directly (as fi_pingpong/fabtests examples
// do) sidesteps the whole issue.
//
// Usage:
//   fi_bw server [-d domain] [-p port] [-s size] [-w window] [-n iters]
//   fi_bw client <server_ip> [-d domain] [-p port] [-s size] [-w window] [-n iters]

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_errno.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Config {
    bool is_server = false;
    std::string server_ip;
    std::string local_ip; // server: local address to bind (optional)
    std::string domain;
    std::string provider = "verbs";
    std::string port = "47592";
    size_t size = 65536;
    int window = 16;
    long iters = 20000;
};

static void die(const char *what, int rc) {
    fprintf(stderr, "%s failed: %s\n", what, fi_strerror(-rc));
    exit(1);
}

#define CHECK(what, call)                                                     \
    do {                                                                     \
        int _rc = (call);                                                    \
        if (_rc < 0)                                                         \
            die(what, _rc);                                                  \
    } while (0)

static Config parse_args(int argc, char **argv) {
    Config cfg;
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s server|client [server_ip] [-d domain] [-p port] "
                "[-s size] [-w window] [-n iters]\n",
                argv[0]);
        exit(1);
    }
    std::string role = argv[1];
    cfg.is_server = (role == "server");
    int i = 2;
    if (!cfg.is_server) {
        if (argc < 3) {
            fprintf(stderr, "client requires <server_ip>\n");
            exit(1);
        }
        cfg.server_ip = argv[2];
        i = 3;
    }
    for (; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for %s\n", a.c_str());
                exit(1);
            }
            return argv[++i];
        };
        if (a == "-d") cfg.domain = next();
        else if (a == "-l") cfg.local_ip = next();
        else if (a == "-P") cfg.provider = next();
        else if (a == "-p") cfg.port = next();
        else if (a == "-s") cfg.size = std::stoul(next());
        else if (a == "-w") cfg.window = std::stoi(next());
        else if (a == "-n") cfg.iters = std::stol(next());
        else {
            fprintf(stderr, "unknown argument: %s\n", a.c_str());
            exit(1);
        }
    }
    return cfg;
}

// constrain_ep_type must be false for the server's source/listen query:
// this libfabric verbs provider version fails rdma_bind_addr() for a
// plain sockaddr_in when ep_attr->type is constrained up front, and only
// falls back to its working native FI_SOCKADDR_IB resolution path when
// the type is left unconstrained. The domain only offers FI_EP_MSG
// anyway, so leaving it unconstrained doesn't change what comes back.
static fi_info *make_hints(const Config &cfg, bool constrain_ep_type = true) {
    fi_info *hints = fi_allocinfo();
    hints->caps = FI_MSG;
    if (constrain_ep_type)
        hints->ep_attr->type = FI_EP_MSG;
    if (cfg.provider == "verbs") {
        // The verbs provider requires these; other providers (e.g. tcp)
        // don't support them at all, so only request them here.
        hints->mode = FI_CONTEXT | FI_RX_CQ_DATA;
        hints->domain_attr->mr_mode =
            FI_MR_LOCAL | FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY;
    }
    hints->fabric_attr->prov_name = strdup(cfg.provider.c_str());
    if (!cfg.domain.empty())
        hints->domain_attr->name = strdup(cfg.domain.c_str());
    return hints;
}

// --- tiny out-of-band TCP control channel, used only to hand the
// server's native fi_getname() address to the client before connecting.
// Not part of the RDMA data path.

static void oob_send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) { perror("send"); exit(1); }
        p += n;
        len -= n;
    }
}

static void oob_recv_all(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) { fprintf(stderr, "control channel closed early\n"); exit(1); }
        p += n;
        len -= n;
    }
}

// Listens on cfg.port (plain TCP), accepts one connection, sends addr,
// then closes.
static void oob_server_send_addr(const Config &cfg, const void *addr, size_t addrlen) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons((uint16_t)(std::stoi(cfg.port) + 1)); // oob control port, separate from the RDMA data port
    if (bind(lfd, (sockaddr *)&sin, sizeof(sin)) < 0) { perror("bind"); exit(1); }
    if (listen(lfd, 1) < 0) { perror("listen"); exit(1); }
    printf("control channel listening on port %s ...\n", cfg.port.c_str());
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) { perror("accept"); exit(1); }
    uint32_t len_n = htonl((uint32_t)addrlen);
    oob_send_all(cfd, &len_n, sizeof(len_n));
    oob_send_all(cfd, addr, addrlen);
    close(cfd);
    close(lfd);
}

// Connects to server_ip:port and receives the address it sends.
static std::vector<char> oob_client_recv_addr(const Config &cfg) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)(std::stoi(cfg.port) + 1)); // oob control port, separate from the RDMA data port
    if (inet_pton(AF_INET, cfg.server_ip.c_str(), &sin.sin_addr) != 1) {
        fprintf(stderr, "invalid server IP: %s\n", cfg.server_ip.c_str());
        exit(1);
    }
    // The control channel may briefly refuse while the server is still
    // starting up; retry for a few seconds.
    int rc = -1;
    for (int attempt = 0; attempt < 50; attempt++) {
        rc = connect(fd, (sockaddr *)&sin, sizeof(sin));
        if (rc == 0) break;
        usleep(100000);
    }
    if (rc < 0) { perror("connect"); exit(1); }
    uint32_t len_n = 0;
    oob_recv_all(fd, &len_n, sizeof(len_n));
    uint32_t len = ntohl(len_n);
    std::vector<char> addr(len);
    oob_recv_all(fd, addr.data(), len);
    close(fd);
    return addr;
}

// Waits for one connection-management event and returns it. Exits on
// error or on receiving a different event than expected.
static fi_info *wait_for_event(fid_eq *eq, uint32_t expected, fi_eq_cm_entry *entry) {
    uint32_t event;
    ssize_t rd = fi_eq_sread(eq, &event, entry, sizeof(*entry), -1, 0);
    if (rd < 0) {
        fi_eq_err_entry err{};
        fi_eq_readerr(eq, &err, 0);
        fprintf(stderr, "eq error: %s\n", fi_strerror(err.err));
        exit(1);
    }
    if (event != expected) {
        fprintf(stderr, "unexpected eq event %u (wanted %u)\n", event, expected);
        exit(1);
    }
    return entry->info;
}

struct Endpoint {
    fid_fabric *fabric = nullptr;
    fid_domain *domain = nullptr;
    fid_eq *eq = nullptr;
    fid_pep *pep = nullptr;
    fid_ep *ep = nullptr;
    fid_cq *txcq = nullptr;
    fid_cq *rxcq = nullptr;
    fi_info *info = nullptr; // owns the info used to build fabric/domain
};

static void open_fabric_domain_eq(Endpoint &e, fi_info *info) {
    e.info = info;
    CHECK("fi_fabric", fi_fabric(info->fabric_attr, &e.fabric, NULL));
    CHECK("fi_domain", fi_domain(e.fabric, info, &e.domain, NULL));
    fi_eq_attr eq_attr{};
    eq_attr.size = 16;
    eq_attr.wait_obj = FI_WAIT_UNSPEC;
    CHECK("fi_eq_open", fi_eq_open(e.fabric, &eq_attr, &e.eq, NULL));
}

static void open_cqs_and_enable(Endpoint &e, fi_info *ep_info, int window) {
    fi_cq_attr cq_attr{};
    cq_attr.format = FI_CQ_FORMAT_CONTEXT;
    cq_attr.size = window + 8;
    cq_attr.wait_obj = FI_WAIT_NONE;
    CHECK("fi_cq_open(tx)", fi_cq_open(e.domain, &cq_attr, &e.txcq, NULL));
    CHECK("fi_cq_open(rx)", fi_cq_open(e.domain, &cq_attr, &e.rxcq, NULL));

    CHECK("fi_endpoint", fi_endpoint(e.domain, ep_info, &e.ep, NULL));
    CHECK("bind eq", fi_ep_bind(e.ep, &e.eq->fid, 0));
    CHECK("bind txcq", fi_ep_bind(e.ep, &e.txcq->fid, FI_TRANSMIT));
    CHECK("bind rxcq", fi_ep_bind(e.ep, &e.rxcq->fid, FI_RECV));
    CHECK("fi_enable", fi_enable(e.ep));
}

static void print_result(const Config &cfg, double elapsed_s) {
    double gbits = (double)cfg.iters * cfg.size * 8.0 / elapsed_s / 1e9;
    double mbps = (double)cfg.iters * cfg.size / elapsed_s / 1e6;
    double usec_per_xfer = elapsed_s * 1e6 / cfg.iters;
    double mmsgs = (double)cfg.iters / elapsed_s / 1e6;
    printf("bytes\titers\twindow\ttime[s]\tBW[Gb/s]\tMB/s\tusec/xfer\tMmsgs/s\n");
    printf("%zu\t%ld\t%d\t%.3f\t%.2f\t\t%.2f\t%.3f\t\t%.3f\n", cfg.size, cfg.iters,
           cfg.window, elapsed_s, gbits, mbps, usec_per_xfer, mmsgs);
}

// Per-completion latency stats, same methodology as dpdk_perf's latency
// mode: stdev (spread around the mean) and RFC 3550 mean jitter
// (average consecutive-sample change) answer different questions, and
// percentiles are what actually show a long tail that avg/stdev alone
// can hide. Note: at window > 1 this is completion latency *under
// pipelined load* (queued behind other outstanding sends), not a
// strictly serialized RTT like `-w 1` gives -- expect it to rise with
// window depth, and that's not a regression, it's the tradeoff for the
// higher throughput pipelining buys (see README).
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

static void run_server(const Config &cfg) {
    fi_info *hints = make_hints(cfg, /*constrain_ep_type=*/false);
    fi_info *info = nullptr;
    const char *local_node = cfg.local_ip.empty() ? NULL : cfg.local_ip.c_str();
    // For verbs, service is left NULL: the RDMA identify doesn't need to
    // match any particular port -- the client gets our real address
    // out-of-band (see file header comment), not by dialing this port
    // via rdma_cm. Other providers (e.g. tcp) have no address fallback
    // when both node and service are NULL, so they need a real service.
    const char *local_service = (cfg.provider == "verbs") ? NULL : cfg.port.c_str();
    CHECK("fi_getinfo", fi_getinfo(FI_VERSION(1, 5), local_node, local_service,
                                    FI_SOURCE, hints, &info));
    fi_freeinfo(hints);

    Endpoint e;
    open_fabric_domain_eq(e, info);

    CHECK("fi_passive_ep", fi_passive_ep(e.fabric, info, &e.pep, NULL));
    CHECK("bind pep eq", fi_pep_bind(e.pep, &e.eq->fid, 0));
    CHECK("fi_listen", fi_listen(e.pep));

    size_t addrlen = 0;
    fi_getname(&e.pep->fid, NULL, &addrlen); // returns -FI_ETOOSMALL, sets addrlen
    std::vector<char> local_addr(addrlen);
    CHECK("fi_getname", fi_getname(&e.pep->fid, local_addr.data(), &addrlen));
    oob_server_send_addr(cfg, local_addr.data(), addrlen);

    fi_eq_cm_entry entry{};
    fi_info *conn_info = wait_for_event(e.eq, FI_CONNREQ, &entry);

    // TODO: fix -P tcp. fi_endpoint(domain, conn_info, ...) below tries to
    // bind the new per-connection endpoint to the same address:port the
    // passive endpoint is already listening on, which the tcp provider
    // rejects with EADDRINUSE (the verbs provider doesn't hit this --
    // each connection gets its own RDMA_CM identifier, no shared socket
    // to collide on). Works fine for -P verbs; only tcp is broken. See
    // the "TCP baseline" section of README.md for how fi_pingpong was
    // used instead in the meantime.
    open_cqs_and_enable(e, conn_info, cfg.window);
    CHECK("fi_accept", fi_accept(e.ep, NULL, 0));
    wait_for_event(e.eq, FI_CONNECTED, &entry);
    printf("connected.\n");

    size_t buflen = cfg.size * cfg.window;
    std::vector<char> buf(buflen);
    fid_mr *mr = nullptr;
    CHECK("fi_mr_reg", fi_mr_reg(e.domain, buf.data(), buflen, FI_RECV, 0, 0, 0, &mr, NULL));
    void *desc = fi_mr_desc(mr);

    std::vector<fi_context> ctx(cfg.window);
    long posted = std::min<long>(cfg.window, cfg.iters);
    for (long i = 0; i < posted; i++)
        CHECK("fi_recv", fi_recv(e.ep, buf.data() + i * cfg.size, cfg.size, desc,
                                  0, &ctx[i]));

    long completed = 0;
    while (completed < cfg.iters) {
        fi_cq_entry cqe{};
        ssize_t ret = fi_cq_read(e.rxcq, &cqe, 1);
        if (ret == -FI_EAGAIN) continue;
        if (ret < 0) {
            fi_cq_err_entry err{};
            fi_cq_readerr(e.rxcq, &err, 0);
            fprintf(stderr, "rx cq error: %s\n", fi_strerror(err.err));
            exit(1);
        }
        completed++;
        size_t idx = (fi_context *)cqe.op_context - ctx.data();
        if (posted < cfg.iters) {
            CHECK("fi_recv", fi_recv(e.ep, buf.data() + idx * cfg.size, cfg.size,
                                      desc, 0, &ctx[idx]));
            posted++;
        }
    }
    printf("done (server does not report BW; wall clock includes idle wait time).\n");

    fi_close(&mr->fid);
    fi_close(&e.ep->fid);
    fi_close(&e.pep->fid);
    fi_close(&e.txcq->fid);
    fi_close(&e.rxcq->fid);
    fi_close(&e.eq->fid);
    fi_close(&e.domain->fid);
    fi_close(&e.fabric->fid);
    fi_freeinfo(info);
}

static void run_client(const Config &cfg) {
    // Get the server's real address out-of-band instead of resolving it
    // by IP through fi_getinfo/rdma_cm (see file header comment).
    std::vector<char> server_addr = oob_client_recv_addr(cfg);

    fi_info *hints = make_hints(cfg);
    fi_info *info = nullptr;
    CHECK("fi_getinfo", fi_getinfo(FI_VERSION(1, 5), NULL, NULL, 0, hints, &info));
    fi_freeinfo(hints);

    Endpoint e;
    open_fabric_domain_eq(e, info);
    open_cqs_and_enable(e, info, cfg.window);

    CHECK("fi_connect", fi_connect(e.ep, server_addr.data(), NULL, 0));
    fi_eq_cm_entry entry{};
    wait_for_event(e.eq, FI_CONNECTED, &entry);
    printf("connected to %s:%s\n", cfg.server_ip.c_str(), cfg.port.c_str());

    size_t buflen = cfg.size * cfg.window;
    std::vector<char> buf(buflen, 'x');
    fid_mr *mr = nullptr;
    CHECK("fi_mr_reg", fi_mr_reg(e.domain, buf.data(), buflen, FI_SEND, 0, 0, 0, &mr, NULL));
    void *desc = fi_mr_desc(mr);

    std::vector<fi_context> ctx(cfg.window);
    std::vector<uint64_t> send_ns(cfg.window);
    std::vector<double> lat_us;
    lat_us.reserve(cfg.iters);
    long posted = std::min<long>(cfg.window, cfg.iters);

    auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < posted; i++) {
        send_ns[i] = now_ns();
        CHECK("fi_send", fi_send(e.ep, buf.data() + i * cfg.size, cfg.size, desc,
                                  0, &ctx[i]));
    }

    long completed = 0;
    while (completed < cfg.iters) {
        fi_cq_entry cqe{};
        ssize_t ret = fi_cq_read(e.txcq, &cqe, 1);
        if (ret == -FI_EAGAIN) continue;
        if (ret < 0) {
            fi_cq_err_entry err{};
            fi_cq_readerr(e.txcq, &err, 0);
            fprintf(stderr, "tx cq error: %s\n", fi_strerror(err.err));
            exit(1);
        }
        completed++;
        size_t idx = (fi_context *)cqe.op_context - ctx.data();
        lat_us.push_back((now_ns() - send_ns[idx]) / 1000.0);
        if (posted < cfg.iters) {
            send_ns[idx] = now_ns();
            CHECK("fi_send", fi_send(e.ep, buf.data() + idx * cfg.size, cfg.size,
                                      desc, 0, &ctx[idx]));
            posted++;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double elapsed_s = std::chrono::duration<double>(t1 - t0).count();
    print_result(cfg, elapsed_s);
    print_latency_stats(lat_us);

    fi_close(&mr->fid);
    fi_close(&e.ep->fid);
    fi_close(&e.txcq->fid);
    fi_close(&e.rxcq->fid);
    fi_close(&e.eq->fid);
    fi_close(&e.domain->fid);
    fi_close(&e.fabric->fid);
    fi_freeinfo(info);
}

int main(int argc, char **argv) {
    Config cfg = parse_args(argc, argv);
    if (cfg.is_server)
        run_server(cfg);
    else
        run_client(cfg);
    return 0;
}
