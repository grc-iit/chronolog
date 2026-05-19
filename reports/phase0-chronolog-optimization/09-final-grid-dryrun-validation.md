# Final Grid Dry-run Validation

The requested final figure grid runner was validated in dry-run mode on 2026-05-19.

Dry-run artifact:

```text
.agent/results/20260519-180958-requested-final-figure-grid-dryrun/
```

Validation command:

```bash
RESULT_ROOT=.agent/results/20260519-180958-requested-final-figure-grid-dryrun \
  .agent/scripts/phase0_requested_figure_grid.sh
```

## Expansion result

| Item | Count |
|---|---:|
| Semantic batch directories | 220 |
| Generated executable commands | 300 |
| Empty command batches | 0 |
| Commands per node count | 60 |
| Commands per payload size | 75 |

Node coverage generated:

```text
1, 2, 4, 5, 16
```

Payload coverage generated:

```text
1024, 4096, 16384, 65536 bytes
```

## Interpretation

This validates that the benchmark harness can expand the requested grid with explicit semantic batches and size-aware operation counts. It does not produce performance figures by itself, because `DRY_RUN=1` is the default.

The stricter final figure objective remains incomplete until the same runner is executed with `DRY_RUN=0`, the resulting rows pass acceptance checks, and the final report is regenerated from those metrics.
