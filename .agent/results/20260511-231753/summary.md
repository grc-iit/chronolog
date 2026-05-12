# ChronoLog perf Validation Probe

Result: blocked by kernel perf permission policy.

The local perf binary was invoked against the ChronoLog `chrono-visor` binary, but unprivileged perf events are denied while `/proc/sys/kernel/perf_event_paranoid` is `4`.

Evidence:

- stdout: `.agent/results/20260511-231753/chronolog/stdout.log`
- stderr: `.agent/results/20260511-231753/chronolog/stderr.log`
- perf stat output: `.agent/results/20260511-231753/chronolog/profiles/perf/perf-stat.txt`
- perf.data: `.agent/results/20260511-231753/chronolog/profiles/perf/perf.data`
- metrics: `.agent/results/20260511-231753/chronolog/metrics.json`
