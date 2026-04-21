#!/usr/bin/env python3
"""
extract_plot_results.py — extract and plot ares_test.sh results.

Usage:
    python3 extract_plot_results.py <results_file>
    python3 extract_plot_results.py   # auto-detects ares_test_logs_latest/*.results

The timestamp is derived from the results file name
(ares_test_YYYYMMDD_HHMMSS.results) and used as the subdirectory under the
output roots, mirroring the ares_test_logs_YYYYMMDD_HHMMSS naming convention.

A matching log file (same directory, same timestamp stem, .log extension) is
auto-discovered alongside the results file.  When found, per-run performance
metrics printed by chrono-performance-test are parsed out and added as extra
columns in the CSVs and as additional plots.  Multiple MPI processes each print
their own metric block; values are summed across processes to give total system
throughput for that run.

Extract phase — writes aggregated CSVs (mean/min/max across repetitions):
    history_perf_data/<timestamp>/by_event_size/
        {test}_{protocol}_compscale{N}_{clients}.csv
    history_perf_data/<timestamp>/by_client_scale/
        {test}_{protocol}_compscale{N}_esz{M}.csv
    history_perf_data/<timestamp>/by_component_scale/
        {test}_{protocol}_{clients}_esz{M}.csv

Plot phase — one PNG per CSV per metric group, under:
    history_perf_plot/<timestamp>/by_event_size/
    history_perf_plot/<timestamp>/by_client_scale/
    history_perf_plot/<timestamp>/by_component_scale/

  {stem}.png         Wall time (s)
  {stem}_ops.png     Operation throughput (operation/s) — 7 series
  {stem}_bw.png      Bandwidth (MB/s) — 2 series
  {stem}_events.png  Record-event throughput (event/s)

Metric plots are only generated when a matching .log file is found.
"""

import csv
import sys
import re
from collections import defaultdict
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


# ---------------------------------------------------------------------------
# Usage / help
# ---------------------------------------------------------------------------

USAGE = """\
USAGE
  extract_plot_results.py [OPTIONS] [results_file]

DESCRIPTION
  Reads a .results CSV file produced by ares_test.sh, aggregates performance
  metrics across repetitions, writes one set of summary CSVs per comparison
  axis, and generates PNG plots for each CSV.

  If results_file is omitted the script auto-detects the newest *.results
  file inside ares_test_logs_latest/ next to this script.

  A companion .log file (same directory, same timestamp stem) is discovered
  automatically.  When found, per-run metrics printed by chrono-performance-test
  are parsed and added as columns to every output CSV.  Additional metric plots
  (operation/s, MB/s, event/s) are generated alongside the wall-time plots.

POSITIONAL ARGUMENT
  results_file    Path to a .results CSV file.  Optional — see auto-detection.

OPTIONS
  -h, --help      Print this help message and exit.

INPUT FORMAT
  The .results file is a CSV produced by ares_test.sh with columns:
    test, protocol, scale, clients, event_size, event_count,
    rep, status, wall_time, tag

  Rows where status != "OK" are skipped.  The event_count column was added
  in a later revision; older files fall back to parsing ecnt=N from the tag.

  The .log file is the tee output from ares_test.sh.  At the end of each run
  chrono-performance-test prints 10 metric lines followed by the CSV result
  line (used as the anchor to associate metrics with the matching tag).

METRIC COLUMNS ADDED TO CSVs (when a .log file is available)
  For each of the following 10 metrics:
    connect_ops_per_s, createchronicle_ops_per_s, acquirestory_ops_per_s,
    releasestory_ops_per_s, destroystory_ops_per_s, destroychronicle_ops_per_s,
    disconnect_ops_per_s   — operation/s
    e2e_bandwidth_mbs, record_bandwidth_mbs   — MB/s
    record_events_per_s   — event/s
  Columns added per metric:  mean_<key>, min_<key>, max_<key>

OUTPUT DIRECTORY STRUCTURE
  history_perf_data/<timestamp>/
    by_event_size/
      {test}_{protocol}_compscale{N}_{clients}.csv
        Columns: event_size, event_count, mean_wall_s, min_wall_s, max_wall_s,
                 [metric columns...], reps
        Fixed:   test, protocol, component_scale, clients
        Varying: event_size (and its associated event_count)

    by_client_scale/
      {test}_{protocol}_compscale{N}_esz{M}.csv
        Columns: total_clients, clients, event_count, mean_wall_s, min_wall_s,
                 max_wall_s, [metric columns...], reps
        Fixed:   test, protocol, component_scale, event_size
        Varying: total_clients / clients config

    by_component_scale/
      {test}_{protocol}_{clients}_esz{M}.csv
        Columns: component_scale, event_count, mean_wall_s, min_wall_s,
                 max_wall_s, [metric columns...], reps
        Fixed:   test, protocol, clients, event_size
        Varying: component_scale (and its associated event_count)

  history_perf_plot/<timestamp>/
    by_event_size/      {stem}.png, {stem}_ops.png, {stem}_bw.png, {stem}_events.png
    by_client_scale/    same set per CSV
    by_component_scale/ same set per CSV

EXAMPLES
  # Auto-detect the latest results and generate all CSVs and plots:
  python3 extract_plot_results.py

  # Process a specific results file:
  python3 extract_plot_results.py ares_test_logs_20260420_002918/ares_test_20260420_002918.results
"""


def usage():
    print(USAGE, end="")


if len(sys.argv) >= 2 and sys.argv[1] in ("-h", "--help"):
    usage()
    sys.exit(0)


# ---------------------------------------------------------------------------
# Metric specifications
# ---------------------------------------------------------------------------
# Each entry: (key, compiled_regex, short_label, unit)
# The regex matches a line printed by chrono-performance-test at the end of
# each run.  Multiple MPI processes each print their own set; values are
# summed across all processes to give total system throughput for that run.

METRIC_SPECS = [
    ("connect_ops_per_s",
     re.compile(r"^Connect throughput:\s+([\d.eE+\-]+)\s+connections/s"),
     "Connect", "operation/s"),
    ("createchronicle_ops_per_s",
     re.compile(r"^CreateChronicle throughput:\s+([\d.eE+\-]+)\s+creations/s"),
     "CreateChronicle", "operation/s"),
    ("acquirestory_ops_per_s",
     re.compile(r"^AcquireStory throughput:\s+([\d.eE+\-]+)\s+acquisitions/s"),
     "AcquireStory", "operation/s"),
    ("releasestory_ops_per_s",
     re.compile(r"^ReleaseStory throughput:\s+([\d.eE+\-]+)\s+releases/s"),
     "ReleaseStory", "operation/s"),
    ("destroystory_ops_per_s",
     re.compile(r"^DestroyStory throughput:\s+([\d.eE+\-]+)\s+destructions/s$"),
     "DestroyStory", "operation/s"),
    ("destroychronicle_ops_per_s",
     re.compile(r"^DestroyChronicle throughput:\s+([\d.eE+\-]+)\s+destructions/s$"),
     "DestroyChronicle", "operation/s"),
    ("disconnect_ops_per_s",
     re.compile(r"^Disconnect throughput:\s+([\d.eE+\-]+)\s+disconnections/s"),
     "Disconnect", "operation/s"),
    ("e2e_bandwidth_mbs",
     re.compile(r"^End-to-end \(incl\. metadata time\) bandwidth:\s+([\d.eE+\-]+)\s+MB/s"),
     "End-to-end bw", "MB/s"),
    ("record_bandwidth_mbs",
     re.compile(r"^Record-event \(incl\. metadata time\) bandwidth:\s+([\d.eE+\-]+)\s+MB/s"),
     "Record-event bw", "MB/s"),
    ("record_events_per_s",
     re.compile(r"^Record-event \(incl\. metadata time\) throughput:\s+([\d.eE+\-]+)\s+events/s"),
     "Record-event", "event/s"),
]

METRIC_KEYS    = [k             for k, *_       in METRIC_SPECS]
OPS_METRICS    = [(k, lb) for k, _, lb, u in METRIC_SPECS if u == "operation/s"]
BW_METRICS     = [(k, lb) for k, _, lb, u in METRIC_SPECS if u == "MB/s"]
EVENTS_METRICS = [(k, lb) for k, _, lb, u in METRIC_SPECS if u == "event/s"]

# All value keys aggregated together: wall time + all metrics
ALL_VALUE_KEYS = ("wall",) + tuple(METRIC_KEYS)

# Regex to detect a CSV result line written by ares_test.sh inside the log.
# Format: test,protocol,scale,clients,esz,ecnt,rep,STATUS,Ns,test=...
_CSV_RESULT_RE = re.compile(
    r"^[a-z_]+,[^,]+,\d+,[^,]+,\d+,\d+,\d+,[A-Z][A-Z_()\d]*,\d+s,test="
)


# ---------------------------------------------------------------------------
# Locate results file
# ---------------------------------------------------------------------------

SCRIPT_DIR = Path(__file__).parent.resolve()


def find_results_file():
    latest = SCRIPT_DIR / "ares_test_logs_latest"
    matches = sorted(latest.glob("*.results"))
    if not matches:
        sys.exit(f"No .results file found in {latest}")
    return matches[-1]


results_file = Path(sys.argv[1]) if len(sys.argv) >= 2 else find_results_file()
print(f"Processing: {results_file}")

m = re.search(r"(\d{8}_\d{6})", results_file.name)
timestamp = m.group(1) if m else results_file.stem

DATA_ROOT = SCRIPT_DIR / "history_perf_data" / timestamp
PLOT_ROOT = SCRIPT_DIR / "history_perf_plot" / timestamp


# ---------------------------------------------------------------------------
# Locate and parse log file for per-run metrics
# ---------------------------------------------------------------------------

log_file = results_file.with_suffix(".log")
metrics_by_tag = {}

if log_file.exists():
    print(f"Log file   : {log_file}")
    current_metrics: dict = defaultdict(list)
    with open(log_file) as _lf:
        for _raw in _lf:
            _line = _raw.rstrip("\n")
            _matched = False
            for _key, _pat, _, _ in METRIC_SPECS:
                _m = _pat.match(_line)
                if _m:
                    current_metrics[_key].append(float(_m.group(1)))
                    _matched = True
                    break
            if _matched:
                continue
            # CSV result line — anchor: associate collected metrics with this tag
            if _CSV_RESULT_RE.match(_line):
                _parts = _line.split(",", 9)
                if len(_parts) == 10:
                    _tag = _parts[9].strip()
                    # Sum across processes for total system throughput
                    metrics_by_tag[_tag] = {
                        k: sum(v) for k, v in current_metrics.items() if v
                    }
                current_metrics = defaultdict(list)
    print(f"  parsed metrics for {len(metrics_by_tag)} runs")
else:
    print(f"No log file found at {log_file} — metric columns will be omitted")

has_metrics = bool(metrics_by_tag)


# ---------------------------------------------------------------------------
# Parse results CSV
# ---------------------------------------------------------------------------

def parse_wall(s):
    return int(s.rstrip("s"))


def total_clients_num(clients_str):
    p, n = clients_str.split("x")
    return int(p) * int(n)


def fmt_size(esz):
    esz = int(esz)
    if esz >= 1024 * 1024:
        return f"{esz // (1024 * 1024)}M"
    if esz >= 1024:
        return f"{esz // 1024}K"
    return str(esz)


def safe_name(*parts):
    return "_".join(str(p).replace("+", "_").replace("/", "_") for p in parts)


def parse_event_count(row):
    """Read event_count from its dedicated column; fall back to parsing the tag
    field for results files written before the column was added."""
    v = row.get("event_count", "").strip()
    if v:
        return int(v)
    m = re.search(r"\becnt=(\d+)\b", row.get("tag", ""))
    return int(m.group(1)) if m else 0


raw_data = []
with open(results_file) as f:
    for row in csv.DictReader(f):
        if row["status"] != "OK":
            continue
        tag = row.get("tag", "").strip()
        run_metrics = metrics_by_tag.get(tag, {})
        entry = {
            "test":          row["test"],
            "protocol":      row["protocol"],
            "scale":         int(row["scale"]),
            "clients":       row["clients"],
            "total_clients": total_clients_num(row["clients"]),
            "event_size":    int(row["event_size"]),
            "event_count":   parse_event_count(row),
            "rep":           int(row["rep"]),
            "wall":          parse_wall(row["wall_time"]),
        }
        for k in METRIC_KEYS:
            entry[k] = run_metrics.get(k)  # None when log not available
        raw_data.append(entry)

if not raw_data:
    sys.exit("No OK rows found in results file.")


# ---------------------------------------------------------------------------
# Aggregation helper
# ---------------------------------------------------------------------------

def _col(vk):
    """Return the column-name prefix for value key vk."""
    return "wall_s" if vk == "wall" else vk


def aggregate(rows, x_key, group_keys, value_keys=ALL_VALUE_KEYS, passthrough_keys=()):
    """
    Returns {group_tuple: {x_val: {stat_cols..., "reps": int, ...passthrough}}}

    value_keys: fields in each row to aggregate with mean/min/max.
    For each vk in value_keys the output dict has keys
      mean_<col(vk)>, min_<col(vk)>, max_<col(vk)>
    where col("wall") == "wall_s" and col(metric_key) == metric_key.

    passthrough_keys: extra fields constant within (group, x) — first value wins.
    """
    buckets     = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    passthrough = defaultdict(lambda: defaultdict(dict))
    for r in rows:
        g = tuple(r[k] for k in group_keys)
        x = r[x_key]
        for vk in value_keys:
            v = r.get(vk)
            if v is not None:
                buckets[g][x][vk].append(v)
        for pk in passthrough_keys:
            if pk not in passthrough[g][x]:
                passthrough[g][x][pk] = r[pk]
    result = {}
    for g, xd in buckets.items():
        result[g] = {}
        for x, vkd in xd.items():
            reps = max((len(v) for v in vkd.values()), default=0)
            entry = {"reps": reps, **passthrough[g][x]}
            for vk in value_keys:
                col = _col(vk)
                vals = vkd.get(vk, [])
                if vals:
                    entry[f"mean_{col}"] = sum(vals) / len(vals)
                    entry[f"min_{col}"]  = min(vals)
                    entry[f"max_{col}"]  = max(vals)
                else:
                    entry[f"mean_{col}"] = None
                    entry[f"min_{col}"]  = None
                    entry[f"max_{col}"]  = None
            result[g][x] = entry
    return result


# ---------------------------------------------------------------------------
# CSV writer
# ---------------------------------------------------------------------------

def write_csv(path, fieldnames, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)


# ---------------------------------------------------------------------------
# Plotting helpers
# ---------------------------------------------------------------------------

def plot_errorbar(ax, xs, means, mins, maxs, **kwargs):
    err_lo = [m - lo for m, lo in zip(means, mins)]
    err_hi = [hi - m  for m, hi in zip(means, maxs)]
    ax.errorbar(
        xs, means, yerr=[err_lo, err_hi],
        fmt="o-", capsize=4, linewidth=1.5, markersize=5, **kwargs,
    )


def save_fig(fig, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def _metric_plot(save_path, title, xs, xd, metrics, ylabel, xlabel,
                 log2_xscale=False, xlabels=None, figsize=(9, 4)):
    """
    Generate a plot for a group of metrics sharing the same unit.

    metrics: list of (metric_key, label) pairs.
    xs:      sorted list of x values.
    xd:      aggregated dict {x_val: stat_dict} from aggregate().
    Returns True if the plot was saved, False if all metrics had no data.
    """
    fig, ax = plt.subplots(figsize=figsize)
    n_plotted = 0
    for mk, label in metrics:
        means = [xd[x].get(f"mean_{mk}") for x in xs]
        if all(v is None for v in means):
            continue
        mins  = [xd[x].get(f"min_{mk}")  or 0 for x in xs]
        maxs  = [xd[x].get(f"max_{mk}")  or 0 for x in xs]
        means = [v or 0 for v in means]
        plot_errorbar(ax, xs, means, mins, maxs, label=label)
        n_plotted += 1
    if not n_plotted:
        plt.close(fig)
        return False
    if log2_xscale:
        ax.set_xscale("log", base=2)
    if xlabels is not None:
        ax.set_xticks(xs)
        ax.set_xticklabels(xlabels, rotation=45, ha="right")
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    if n_plotted > 1:
        ax.legend(fontsize="small")
    ax.grid(True, linestyle="--", alpha=0.5)
    save_fig(fig, save_path)
    return True


# ---------------------------------------------------------------------------
# Build fieldnames for output CSVs
# ---------------------------------------------------------------------------

_metric_stat_fields = []
for _mk in METRIC_KEYS:
    _metric_stat_fields += [f"mean_{_mk}", f"min_{_mk}", f"max_{_mk}"]

ESZ_FIELDS = (
    ["event_size", "event_count", "mean_wall_s", "min_wall_s", "max_wall_s"] +
    _metric_stat_fields + ["reps"]
)
CLIENT_FIELDS = (
    ["total_clients", "clients", "event_count",
     "mean_wall_s", "min_wall_s", "max_wall_s"] +
    _metric_stat_fields + ["reps"]
)
COMPSCALE_FIELDS = (
    ["component_scale", "event_count", "mean_wall_s", "min_wall_s", "max_wall_s"] +
    _metric_stat_fields + ["reps"]
)


# ---------------------------------------------------------------------------
# Axis 1: by_event_size
# Fixed: (test, protocol, component_scale, clients)
# x: event_size  — log2 axis; shows how throughput/bandwidth varies with payload
# event_count varies per row (scale fixed; larger events → fewer events)
# ---------------------------------------------------------------------------

by_esz_agg = aggregate(
    raw_data,
    x_key="event_size",
    group_keys=["test", "protocol", "scale", "clients"],
    passthrough_keys=("event_count",),
)

esz_data_dir = DATA_ROOT / "by_event_size"
esz_plot_dir = PLOT_ROOT / "by_event_size"
esz_csv_count  = 0
esz_plot_count = 0

for group, xd in by_esz_agg.items():
    test, protocol, scale, clients = group
    proto_tag = protocol.replace("+", "_")
    stem = safe_name(test, proto_tag, f"compscale{scale}", clients)
    title = f"{test}  ·  {protocol}  ·  compscale={scale}  ·  clients={clients}"

    # --- Build row list for CSV ---
    csv_rows = []
    for esz in sorted(xd.keys()):
        s = xd[esz]
        row = {
            "event_size":  esz,
            "event_count": s.get("event_count"),
            "mean_wall_s": round(s["mean_wall_s"], 3) if s["mean_wall_s"] is not None else None,
            "min_wall_s":  s["min_wall_s"],
            "max_wall_s":  s["max_wall_s"],
            "reps":        s["reps"],
        }
        for mk in METRIC_KEYS:
            row[f"mean_{mk}"] = round(s[f"mean_{mk}"], 6) if s[f"mean_{mk}"] is not None else None
            row[f"min_{mk}"]  = s[f"min_{mk}"]
            row[f"max_{mk}"]  = s[f"max_{mk}"]
        csv_rows.append(row)

    write_csv(esz_data_dir / f"{stem}.csv", ESZ_FIELDS, csv_rows)
    esz_csv_count += 1

    xs      = [r["event_size"]  for r in csv_rows]
    xlabels = [f"{fmt_size(r['event_size'])}\n(n={r['event_count']})" for r in csv_rows]
    xlabel  = "Event size  (n = event count per client)"

    # Wall-time plot
    means = [r["mean_wall_s"] or 0 for r in csv_rows]
    mins  = [r["min_wall_s"]  or 0 for r in csv_rows]
    maxs  = [r["max_wall_s"]  or 0 for r in csv_rows]
    fig, ax = plt.subplots(figsize=(8, 4))
    plot_errorbar(ax, xs, means, mins, maxs)
    ax.set_xscale("log", base=2)
    ax.set_xticks(xs)
    ax.set_xticklabels(xlabels, rotation=45, ha="right")
    ax.set_xlabel(xlabel)
    ax.set_ylabel("Wall time (s)")
    ax.set_title(title)
    ax.grid(True, linestyle="--", alpha=0.5)
    save_fig(fig, esz_plot_dir / f"{stem}.png")
    esz_plot_count += 1

    # Metric plots (only when log data is available)
    if has_metrics:
        for metric_group, ylabel, suffix, fs in [
            (OPS_METRICS,    "operation/s", "_ops",    (11, 4)),
            (BW_METRICS,     "MB/s",        "_bw",     (8,  4)),
            (EVENTS_METRICS, "event/s",     "_events", (8,  4)),
        ]:
            saved = _metric_plot(
                esz_plot_dir / f"{stem}{suffix}.png",
                f"[{suffix.lstrip('_')}] {title}",
                xs, xd, metric_group, ylabel, xlabel,
                log2_xscale=True, xlabels=xlabels, figsize=fs,
            )
            if saved:
                esz_plot_count += 1

print(f"by_event_size    : {esz_csv_count} CSVs, {esz_plot_count} plots  →  {esz_data_dir}")


# ---------------------------------------------------------------------------
# Axis 2: by_client_scale
# Fixed: (test, protocol, component_scale, event_size)
# x: total_clients  — shows how throughput scales with client count
# event_count is constant for all rows (scale and esz both fixed)
# ---------------------------------------------------------------------------

# Build a map from (group_keys, total_clients) -> clients_str for tick labels.
clients_str_map = defaultdict(dict)
for r in raw_data:
    g = (r["test"], r["protocol"], r["scale"], r["event_size"])
    clients_str_map[g][r["total_clients"]] = r["clients"]

by_client_agg = aggregate(
    raw_data,
    x_key="total_clients",
    group_keys=["test", "protocol", "scale", "event_size"],
    passthrough_keys=("event_count",),
)

client_data_dir = DATA_ROOT / "by_client_scale"
client_plot_dir = PLOT_ROOT / "by_client_scale"
client_csv_count  = 0
client_plot_count = 0

for group, xd in by_client_agg.items():
    test, protocol, scale, esz = group
    proto_tag = protocol.replace("+", "_")
    stem  = safe_name(test, proto_tag, f"compscale{scale}", f"esz{esz}")
    ecnt  = next(iter(xd.values())).get("event_count", 0)
    title = (f"{test}  ·  {protocol}  ·  compscale={scale}  ·  "
             f"esz={fmt_size(esz)}  ·  n={ecnt}")

    # --- Build row list ---
    csv_rows = []
    for tc in sorted(xd.keys()):
        s = xd[tc]
        row = {
            "total_clients": tc,
            "clients":       clients_str_map[group].get(tc, ""),
            "event_count":   s.get("event_count"),
            "mean_wall_s":   round(s["mean_wall_s"], 3) if s["mean_wall_s"] is not None else None,
            "min_wall_s":    s["min_wall_s"],
            "max_wall_s":    s["max_wall_s"],
            "reps":          s["reps"],
        }
        for mk in METRIC_KEYS:
            row[f"mean_{mk}"] = round(s[f"mean_{mk}"], 6) if s[f"mean_{mk}"] is not None else None
            row[f"min_{mk}"]  = s[f"min_{mk}"]
            row[f"max_{mk}"]  = s[f"max_{mk}"]
        csv_rows.append(row)

    write_csv(client_data_dir / f"{stem}.csv", CLIENT_FIELDS, csv_rows)
    client_csv_count += 1

    xs      = [r["total_clients"] for r in csv_rows]
    xlabels = [r["clients"]       for r in csv_rows]
    xlabel  = "Client config (processes × nodes)"

    # Wall-time plot
    means = [r["mean_wall_s"] or 0 for r in csv_rows]
    mins  = [r["min_wall_s"]  or 0 for r in csv_rows]
    maxs  = [r["max_wall_s"]  or 0 for r in csv_rows]
    fig, ax = plt.subplots(figsize=(7, 4))
    plot_errorbar(ax, xs, means, mins, maxs)
    ax.set_xticks(xs)
    ax.set_xticklabels(xlabels, rotation=45, ha="right")
    ax.set_xlabel(xlabel)
    ax.set_ylabel("Wall time (s)")
    ax.set_title(title)
    ax.grid(True, linestyle="--", alpha=0.5)
    save_fig(fig, client_plot_dir / f"{stem}.png")
    client_plot_count += 1

    # Metric plots
    if has_metrics:
        for metric_group, ylabel, suffix, fs in [
            (OPS_METRICS,    "operation/s", "_ops",    (10, 4)),
            (BW_METRICS,     "MB/s",        "_bw",     (7,  4)),
            (EVENTS_METRICS, "event/s",     "_events", (7,  4)),
        ]:
            saved = _metric_plot(
                client_plot_dir / f"{stem}{suffix}.png",
                f"[{suffix.lstrip('_')}] {title}",
                xs, xd, metric_group, ylabel, xlabel,
                xlabels=xlabels, figsize=fs,
            )
            if saved:
                client_plot_count += 1

print(f"by_client_scale  : {client_csv_count} CSVs, {client_plot_count} plots  →  {client_data_dir}")


# ---------------------------------------------------------------------------
# Axis 3: by_component_scale
# Fixed: (test, protocol, clients, event_size)
# x: component_scale  — shows how adding component nodes affects throughput
# event_count varies per row (different scale → different ecnt at fixed esz)
# ---------------------------------------------------------------------------

by_compscale_agg = aggregate(
    raw_data,
    x_key="scale",
    group_keys=["test", "protocol", "clients", "event_size"],
    passthrough_keys=("event_count",),
)

compscale_data_dir = DATA_ROOT / "by_component_scale"
compscale_plot_dir = PLOT_ROOT / "by_component_scale"
compscale_csv_count  = 0
compscale_plot_count = 0

for group, xd in by_compscale_agg.items():
    test, protocol, clients, esz = group
    proto_tag = protocol.replace("+", "_")
    stem  = safe_name(test, proto_tag, clients, f"esz{esz}")
    title = f"{test}  ·  {protocol}  ·  clients={clients}  ·  esz={fmt_size(esz)}"

    # --- Build row list ---
    csv_rows = []
    for sc in sorted(xd.keys()):
        s = xd[sc]
        row = {
            "component_scale": sc,
            "event_count":     s.get("event_count"),
            "mean_wall_s":     round(s["mean_wall_s"], 3) if s["mean_wall_s"] is not None else None,
            "min_wall_s":      s["min_wall_s"],
            "max_wall_s":      s["max_wall_s"],
            "reps":            s["reps"],
        }
        for mk in METRIC_KEYS:
            row[f"mean_{mk}"] = round(s[f"mean_{mk}"], 6) if s[f"mean_{mk}"] is not None else None
            row[f"min_{mk}"]  = s[f"min_{mk}"]
            row[f"max_{mk}"]  = s[f"max_{mk}"]
        csv_rows.append(row)

    write_csv(compscale_data_dir / f"{stem}.csv", COMPSCALE_FIELDS, csv_rows)
    compscale_csv_count += 1

    xs      = [r["component_scale"] for r in csv_rows]
    xlabels = [f"scale={r['component_scale']}\n(n={r['event_count']})" for r in csv_rows]
    xlabel  = "Component scale  (n = event count per client)"

    # Wall-time plot
    means = [r["mean_wall_s"] or 0 for r in csv_rows]
    mins  = [r["min_wall_s"]  or 0 for r in csv_rows]
    maxs  = [r["max_wall_s"]  or 0 for r in csv_rows]
    fig, ax = plt.subplots(figsize=(6, 4))
    plot_errorbar(ax, xs, means, mins, maxs)
    ax.set_xticks(xs)
    ax.set_xticklabels(xlabels)
    ax.set_xlabel(xlabel)
    ax.set_ylabel("Wall time (s)")
    ax.set_title(title)
    ax.grid(True, linestyle="--", alpha=0.5)
    save_fig(fig, compscale_plot_dir / f"{stem}.png")
    compscale_plot_count += 1

    # Metric plots
    if has_metrics:
        for metric_group, ylabel, suffix, fs in [
            (OPS_METRICS,    "operation/s", "_ops",    (9, 4)),
            (BW_METRICS,     "MB/s",        "_bw",     (6, 4)),
            (EVENTS_METRICS, "event/s",     "_events", (6, 4)),
        ]:
            saved = _metric_plot(
                compscale_plot_dir / f"{stem}{suffix}.png",
                f"[{suffix.lstrip('_')}] {title}",
                xs, xd, metric_group, ylabel, xlabel,
                xlabels=xlabels, figsize=fs,
            )
            if saved:
                compscale_plot_count += 1

print(f"by_component_scale: {compscale_csv_count} CSVs, {compscale_plot_count} plots  →  {compscale_data_dir}")
print("Done.")
