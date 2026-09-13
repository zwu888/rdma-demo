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

## Reference results (2026-09-12, direct cable, 100Gb ConnectX-4)

- Bandwidth (64KB, RDMA Write): ~92.5 Gb/s
- Latency (2B, RDMA Write): ~0.94 us typical/average
- Message rate (2B, Send): ~3.6 Mpps
