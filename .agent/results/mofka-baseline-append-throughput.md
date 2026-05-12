# Mofka Baseline Append Throughput

Status: complete for local smoke metrics.

Validation run:

```text
.agent/results/20260512-003314/
```

Metrics:

```json
{
  "system": "mofka",
  "workflow": "append_throughput",
  "node_count": 1,
  "client_count": 1,
  "message_size_bytes": 1024,
  "operation_count": 50,
  "duration_seconds": 5.1393059430411085,
  "throughput_ops_per_sec": 9.728940163156201,
  "avg_latency_ms": 100.68273379001766,
  "p50_latency_ms": 100.6871679564938,
  "p95_latency_ms": 100.81775905564427,
  "p99_latency_ms": 100.85097898263484,
  "success": true
}
```

Evidence:

- Metrics: `.agent/results/20260512-003314/mofka/metrics.json`
- Benchmark stdout/stderr: `.agent/results/20260512-003314/mofka/append-benchmark.stdout.log`, `.agent/results/20260512-003314/mofka/append-benchmark.stderr.log`
- Bedrock query before benchmark: `.agent/results/20260512-003314/mofka/bedrock-query-before-benchmark.json`
- Workload config: `.agent/results/20260512-003314/config/mofka-workload.json`
- Launch config manifest: `.agent/results/20260512-003314/config/mofka-config-manifest.env`
- Summary: `.agent/results/20260512-003314/summary.md`

Configuration notes:

- This is a local smoke on `ares`, not a final distributed result.
- Protocol is `ofi+tcp`; local `na+sm` append traffic is blocked by kernel Yama ptrace policy for Mercury shared-memory RMA.
- Partition type is Mofka memory partition. Dynamic creation of the Yokan/Warabi-backed default partition still needs follow-up because the current local path reports a Mofka Bedrock module-registration error.
- The master Yokan provider tag was corrected to `mofka:master`, which is what `MofkaDriver` expects.

Mofka source-change status:

- No Mofka source tree was edited for this run.
- The local `opt/mochi-spack-packages` helper repository still contains compatibility edits used to expose/build Mofka with the installed Spack version. These are packaging/exposure changes, not Mofka benchmark-source changes.
