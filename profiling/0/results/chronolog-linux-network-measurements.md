# ChronoLog Linux Network Measurement Commands Validation

Checkpoint: verify Linux network measurement outputs
Status: complete
Time: 2026-05-11 23:34 CT

## Command Summary

Collected outputs from the required Linux network measurement commands around a local ChronoLog validation run:

- `iperf3`: localhost raw bandwidth check.
- `ss`: socket state and send/receive queues before, during, and after ChronoLog.
- `nstat`: kernel TCP/IP counters before and after ChronoLog.
- `sar -n DEV`: network throughput during the ChronoLog validation run.
- `ethtool`: link settings and driver stats for the default interface `eno1`.

## Evidence

- ChronoLog `chrono-bench` exited with status 0.
- Metrics file: `.agent/results/20260511-233350/chronolog/metrics.json`.
- Network output listing: `.agent/results/20260511-233350/chronolog/profiles/network/network-files.txt`.
- `iperf3` client output: `.agent/results/20260511-233350/chronolog/profiles/network/iperf3-client.log`.
- `ss` output during ChronoLog: `.agent/results/20260511-233350/chronolog/profiles/network/ss-during.txt`.
- `nstat` before/after: `.agent/results/20260511-233350/chronolog/profiles/network/nstat-before.txt`, `.agent/results/20260511-233350/chronolog/profiles/network/nstat-after.txt`.
- `sar -n DEV`: `.agent/results/20260511-233350/chronolog/profiles/network/sar-n-dev.txt`.
- `ethtool` output is captured in `.agent/results/20260511-233350/chronolog/stdout.log`.

## Notes

`iperf3` on localhost reported about 23 Gbits/sec over two seconds. `ethtool` read-only link settings were available for `eno1`; some lower-level driver/offload operations can require elevated privileges and were not changed.
