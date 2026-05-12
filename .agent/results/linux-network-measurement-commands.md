# Linux Network Measurement Commands

Status: complete.

Evidence directory: `.agent/results/20260511-221607/`

## Required Commands

| Command | Path | Purpose |
|---|---|---|
| `iperf3` | `/usr/bin/iperf3` | raw node-to-node bandwidth |
| `ss` | `/usr/bin/ss` | socket state and send/receive queues |
| `nstat` | `/usr/bin/nstat` | kernel TCP/IP counters |
| `sar -n` | `/usr/bin/sar` | network throughput over time |
| `ethtool` | `/usr/sbin/ethtool` | NIC speed, driver stats, drops, offload settings |

Optional targeted-debugging command:

| Command | Path | Purpose |
|---|---|---|
| `tcpdump` | `/usr/bin/tcpdump` | packet capture only for targeted debugging |

## Versions

| Command | Version |
|---|---|
| `iperf3` | 3.9 |
| `ss`/`nstat` | iproute2 5.15.0 |
| `sar` | sysstat 12.5.2 |
| `ethtool` | 5.16 |
| `tcpdump` | 4.99.1 |

## Smoke Validation

Successful smoke checks:

- `ss -tan`
- `nstat -az TcpInSegs TcpOutSegs IpInReceives IpOutRequests`
- `sar -n DEV 1 1`
- `ip -o link show`
- `ethtool eno1`

Detected first non-loopback interface:

```text
eno1
```

`ethtool eno1` reported:

```text
Speed: 1000Mb/s
Duplex: Full
Link detected: yes
```

Some `ethtool` netlink operations reported `Operation not permitted`; read-only link settings are still available. Driver stats/offload changes may require elevated privileges and should be treated as read-only unless the cluster permits them.
