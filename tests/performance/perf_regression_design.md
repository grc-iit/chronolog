# Performance Regression Test — Design

Author: kfeng
Status: design / pre-implementation

## 1. Goal

A **manually run** script that runs a fixed narrow performance case on Ares,
compares the resulting metrics against the **previous released version's**
numbers, and reports whether any event size shows a **≥ 10 % regression**.

Result data accumulates over releases so trends can be plotted later, without
forcing a rerun of any past release.

## 2. Test case (the regression case)

A single, frozen configuration. The full sweep that `ares_test.sh` runs today
is preserved separately for trend/exploration work; the regression case runs
*only* this narrow configuration so it is fast, repeatable, and unambiguous.

| Parameter | Value |
| --- | --- |
| Recording groups | **4** |
| Per group | 1 keeper, 1 grapher, 1 player |
| Client nodes | **4** |
| Clients per node | **20** (→ 80 clients total) |
| Event sizes | **1 KB → 1 MB** (powers of two: 1024, 2048, …, 1048576) |
| Protocol | **ofi+sockets** |
| Ingestion thread count | **default** for keeper, grapher, player |
| Reps per (size) | **3** (mean used; median used when reps > 5) |
| Tracked metrics | `record_event` bandwidth (MB/s) **and** throughput (ops/s) — one number per metric per event size |

## 3. Storage — orphan branch `perf-baselines`

Baseline data lives on a **dedicated orphan branch** with no shared history
with `main` / `develop`. Pattern is identical to `gh-pages`. Results are
committed manually after each release run.

Rationale: code branches stay focused on code; the perf data set can grow
without inflating the main worktree; the perf history can be squashed or
rewritten later without touching code history.

### Branch layout

```
perf-baselines/                           # branch root
├── README.md                              # how this branch is used + schema
├── schema.json                            # JSON Schema for baseline files
└── <version>/                             # one dir per released version
    └── ares/                              # platform
        └── ofi+sockets/                   # protocol
            └── 4groups_4x20clients/       # config slug
                └── record_event.json      # bandwidth + throughput for this config
```

When new metric sets or configs are added later, they slot into sibling files /
sibling dirs without disturbing existing ones.

### Per-baseline JSON schema

```json
{
  "schema_version": 2,
  "version": "2.7.0",
  "git_sha": "1868888d…",
  "test_date": "2026-04-22T17:30:00Z",
  "platform": "ares",
  "config": {
    "recording_groups": 4,
    "keepers_per_group": 1,
    "graphers_per_group": 1,
    "players_per_group": 1,
    "client_nodes": 4,
    "clients_per_node": 20,
    "protocol": "ofi+sockets",
    "ingestion_thread_count": {
      "keeper": "default", "grapher": "default", "player": "default"
    }
  },
  "reps": 3,
  "aggregation": "mean",
  "metrics": {
    "record_event_bw_mbps": {
      "1024":    62.4,
      "2048":   118.5,
      "4096":   220.1,
      "8192":   400.2,
      "16384":  720.0,
      "32768": 1280.0,
      "65536": 2150.0,
      "131072":3500.0,
      "262144":5100.0,
      "524288":6900.0,
      "1048576":8350.0
    },
    "record_event_ops": {
      "1024":  64000,
      "2048":  60928,
      "4096":  56576,
      "8192":  51226,
      "16384": 46080,
      "32768": 40960,
      "65536": 34406,
      "131072":26624,
      "262144":19418,
      "524288":13107,
      "1048576":7962
    }
  }
}
```

`record_event_bw_mbps` values are the per-size mean (reps ≤ 5) or median (reps > 5) of `reps` runs, in MB/s.
`record_event_ops` values are the per-size mean (reps ≤ 5) or median (reps > 5) of `reps` runs, in ops/s
(total events / elapsed seconds across all clients).

## 4. Version identification

- **Current version**: parsed from `CMakeLists.txt` —
  `project(ChronoLog VERSION X.Y.Z …)`. Already authoritative; no other
  source of truth.
- **Previous version**: latest annotated git tag matching `v[0-9]+.[0-9]+.[0-9]+`
  (no pre-release suffixes, e.g., `-test`, `-rc` excluded) that is **strictly
  semver-less-than** the current version.
- **Lookup**: `git tag --list 'v[0-9]*' | sort -V | …` then strip leading `v`.

If no eligible previous version exists, or if its baseline file is missing,
the script **passes with a "bootstrap" warning**, prints the current run's
numbers, and writes the new baseline. This keeps the very first release after
this lands from blocking.

## 5. Comparison rule

Per-size, strict, applied independently to each metric:

> **PASS** ↔ for every event size, current ≥ 0.90 × previous (for both MB/s and ops/s).
> **FAIL** ↔ any single (size, metric) pair is < 0.90 × previous.

`compare_perf.py` always prints a table per metric regardless of outcome,
with a ✓/✗ marker and the percent change vs. the previous baseline.

The threshold (10 %) lives in a single constant in `compare_perf.py`.

Noise control: with the default `reps=3`, mean is used. Switch to median
automatically by setting `REPS` > 5 — the script reads per-rep values from the
raw log and computes the median, which is more robust to outlier runs at higher
rep counts. If variance is still too high, bump `reps` further or repeat any
borderline regression once before failing.

## 6. Workflow — manual invocation

All steps are run manually on Ares by the developer releasing a new version.

```
# 1. Check out the repo at the release tag and build/install as usual.

# 2. Run the fixed regression case and emit the baseline JSON:
tests/performance/run_perf_baseline.sh --out current_run.json

# 3. Check out the perf-baselines branch into a side directory:
git worktree add .perf-baselines perf-baselines

# 4. Compare current run against the previous release:
python3 tests/performance/compare_perf.py current_run.json \
    --baselines-root .perf-baselines \
    --threshold 0.10

# 5. If the comparison passes, save the new baseline:
tests/performance/save_baseline.sh current_run.json --baselines-root .perf-baselines

# 6. Clean up:
git worktree remove .perf-baselines
```

`run_perf_baseline.sh` drives `ares_test.sh` with the fixed narrow-case
environment variables (4 groups, 4×20 clients, ofi+sockets, default threads,
1 KB–1 MB sizes, reps=3). It parses chrono-bench stdout for both the
`Record-event … bandwidth` and `Record-event … rate` lines per size and emits
`current_run.json` conforming to the schema above.

## 7. Files

| Path | Purpose |
| --- | --- |
| `tests/performance/run_perf_baseline.sh` | Drives `ares_test.sh` with the fixed narrow case, emits `current_run.json`. |
| `tests/performance/compare_perf.py` | Loads `current_run.json` + previous baseline JSON, prints per-metric tables, exits 1 on any ≥10% drop. |
| `tests/performance/save_baseline.sh` | Commits `current_run.json` to the right path on the `perf-baselines` branch. |
| `tests/performance/setup_perf_baselines_branch.sh` | One-time bootstrap of the `perf-baselines` orphan branch. |
| `perf-baselines` branch | Created once via `git switch --orphan perf-baselines`, with `README.md` + `schema.json` only. |

`tests/performance/ares_test.sh` is **not modified by this design**; the
regression case calls into it via env vars only.

## 8. Bootstrap plan

For the very first release that exercises this (call it `v3.0.0`):

1. Developer runs `run_perf_baseline.sh`, gets `current_run.json`.
2. `compare_perf.py` looks up the previous baseline → not found.
3. Script logs `[BOOTSTRAP] No baseline for previous version 2.7.0` and exits 0.
4. Developer runs `save_baseline.sh`, which writes
   `3.0.0/ares/ofi+sockets/4groups_4x20clients/record_event.json` and pushes.
5. From `v3.0.1` onwards the comparison is real.

Optional one-time backfill: run the perf case manually on Ares for `v2.6.0`
and `v2.7.0`, hand-author their JSONs and push them to `perf-baselines`.

## 9. Future extensions (out of scope for this PR)

- More metrics per run (e2e bandwidth, acquire / release / disconnect rates,
  P50/P95/P99 latencies). Each extends the `metrics` object in the JSON.
- More configs (other protocols, other scales, ingestion thread sweeps). Each
  becomes a sibling directory under the version dir.
- Trend dashboard: a small static site that shallow-clones `perf-baselines`,
  walks the version tree, and produces per-metric line plots over releases.
- Aggregate / weighted comparison rules in `compare_perf.py`.

## 10. Open decisions to confirm

| # | Decision | Default |
| --- | --- | --- |
| 1 | Branch name | `perf-baselines` |
| 2 | Threshold | 10 %, per-size strict, applied to both metrics independently |
| 3 | Reps | 3, mean; auto-median when reps > 5 |
| 4 | Bootstrap | pass with warning; backfill is optional |
