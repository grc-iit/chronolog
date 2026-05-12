# Darshan Detection

Status: complete.

Evidence directory: `.agent/results/20260511-221405/`

## Detection

Darshan commands were not present on the initial `PATH`.

Cluster modules are available:

- `darshan-runtime/3.4.6-gcc-11.4.0-u7vfz6e`
- `darshan-util/3.4.6-gcc-11.4.0-75rttfw`

Loading those modules also loads the dependency stack including `openmpi/5.0.5-gcc-11.4.0-og56sxz`.

## Commands After Module Load

| Command | Status |
|---|---|
| `darshan-parser` | available |
| `darshan-job-summary.pl` | available |
| `darshan-summary-per-file.sh` | available |
| `darshan-convert` | available |
| `darshan-dxt-parser` | available |
| `darshan-config` | available |
| `darshan-runtime-config` | missing |

## Validation

`darshan-config` succeeded for:

- `--log-path`
- `--pre-ld-flags`
- `--post-ld-flags`
- `--dyn-ld-flags`

The reported log path is controlled by:

```text
$DARSHAN_LOG_DIR_PATH
```

Environment snippet for later runs:

```text
.agent/config/darshan-env.sh
```

Darshan should be used for ChronoLog I/O behavior when the ChronoLog I/O path and launch mode support Darshan instrumentation.
