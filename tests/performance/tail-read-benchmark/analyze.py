#!/usr/bin/env python3
"""Summarize and plot the live_tail_read A/B benchmark.

Reads a results tree produced by run_bench.sh:

    <results>/manifest.txt
    <results>/rep<N>/<arm>/<tag>.log
    <results>/rep<N>/<arm>/<tag>.{latency,playback,write}.csv
    <results>/rep<N>/<arm>/<tag>.keeper-<host>.csv

and writes summary.csv, summary.md and PNG figures next to them.

Tags encode the sweep point: A_<arm>_r<ranks>, B1_<arm>_p<poll_ms>,
B2_<arm>_n<playback_n>.
"""

import argparse
import csv
import glob
import os
import re
import sys
from collections import defaultdict

import numpy as np

# Categorical slots 1 and 2 of the reference palette. Validated for this
# two-series use: CVD dE 24.7 (protan), normal-vision dE 33.6, both >= 3:1 on the
# light surface.
ARM_COLOR = {"off": "#2a78d6", "on": "#eb6834"}
ARM_LABEL = {"off": "sealed-only (existing)", "on": "live tail read (new)"}
ARM_ORDER = ["off", "on"]

TEXT_PRIMARY = "#0b0b0b"
TEXT_SECONDARY = "#52514e"
SURFACE = "#fcfcfb"
GRID = "#d9d8d4"

TAG_RE = re.compile(r"^(?P<fam>A|B1|B2|SMOKE)_(?P<arm>off|on)(?:_(?P<cfg>[rpn])(?P<val>\d+))?$")


def pct(values, p):
    if len(values) == 0:
        return float("nan")
    return float(np.percentile(values, p))


def read_manifest(root):
    info = {}
    path = os.path.join(root, "manifest.txt")
    if os.path.exists(path):
        for line in open(path):
            if "=" in line:
                k, _, v = line.strip().partition("=")
                info[k] = v
    return info


def load_two_col(path, value_col, time_col):
    """Return (times_ns, values) from a CSV with a rank column we ignore."""
    if not os.path.exists(path):
        return np.array([]), np.array([])
    times, vals = [], []
    with open(path) as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            try:
                vals.append(float(row[value_col]))
                times.append(float(row[time_col]))
            except (KeyError, ValueError, TypeError):
                continue
    return np.array(times), np.array(vals)


def trim_warmup(times, values, warmup_s):
    """Drop samples in the first `warmup_s` of the run."""
    if times.size == 0 or warmup_s <= 0:
        return values
    cutoff = times.min() + warmup_s * 1e9
    return values[times >= cutoff]


def parse_log_scalars(path):
    """Pull never_seen / logged / applied rate out of the run log."""
    out = {"logged": float("nan"), "never_seen": float("nan"), "applied_ev_s": float("nan")}
    if not os.path.exists(path):
        return out
    text = open(path, errors="replace").read()
    m = re.search(r"Events logged:\s*(\d+),\s*seen:\s*(\d+),\s*never seen[^:]*:\s*(\d+)", text)
    if m:
        out["logged"] = float(m.group(1))
        out["never_seen"] = float(m.group(3))
    m = re.search(r"Applied write load:\s*([\d.]+)\s*events/s", text)
    if m:
        out["applied_ev_s"] = float(m.group(1))
    return out


def keeper_metrics(paths):
    """Mean %CPU of one core and peak RSS (MB), pooled over keeper nodes."""
    cpus, peak_rss = [], 0.0
    for path in paths:
        ticks = 100.0
        rows = []
        for line in open(path, errors="replace"):
            line = line.strip()
            if line.startswith("#"):
                m = re.search(r"ticks_per_sec=(\d+)", line)
                if m:
                    ticks = float(m.group(1))
                continue
            if not line or line.startswith("epoch_s"):
                continue
            parts = line.split(",")
            if len(parts) < 4:
                continue
            try:
                rows.append((float(parts[0]), float(parts[1]) + float(parts[2]), float(parts[3])))
            except ValueError:
                continue
        for i in range(1, len(rows)):
            dt = rows[i][0] - rows[i - 1][0]
            dticks = rows[i][1] - rows[i - 1][1]
            if dt > 0 and dticks >= 0:
                cpus.append((dticks / ticks) / dt * 100.0)
        for r in rows:
            peak_rss = max(peak_rss, r[2] / 1024.0)
    return (float(np.mean(cpus)) if cpus else float("nan"), peak_rss)


def collect(root, warmup_s):
    """One record per (rep, arm, family, config) sweep point."""
    records = []
    for log_path in sorted(glob.glob(os.path.join(root, "rep*", "*", "*.log"))):
        tag = os.path.basename(log_path)[: -len(".log")]
        m = TAG_RE.match(tag)
        if not m:
            continue  # deploy_start.log etc.
        fam, arm = m.group("fam"), m.group("arm")
        cfg_val = int(m.group("val")) if m.group("val") else 0
        rep = os.path.basename(os.path.dirname(os.path.dirname(log_path)))
        run_dir = os.path.dirname(log_path)
        prefix = os.path.join(run_dir, tag)

        scal = parse_log_scalars(log_path)

        lat_t, lat_v = load_two_col(prefix + ".latency.csv", "latency_ms", "event_time_ns")
        pb_t, pb_v = load_two_col(prefix + ".playback.csv", "playback_us", "t_ns")
        wr_t, wr_v = load_two_col(prefix + ".write.csv", "write_us", "t_ns")

        # B2 sweeps tail depth, so early polls run against a story that has not
        # filled to playback_n yet and are artificially cheap. Trim until the
        # story could plausibly hold that many events.
        pb_warmup = warmup_s
        if fam == "B2" and cfg_val > 0 and scal["applied_ev_s"] > 0:
            pb_warmup = max(warmup_s, cfg_val / scal["applied_ev_s"])

        lat = trim_warmup(lat_t, lat_v, warmup_s)
        pbk = trim_warmup(pb_t, pb_v, pb_warmup)
        wrt = trim_warmup(wr_t, wr_v, warmup_s)

        cpu, rss = keeper_metrics(sorted(glob.glob(prefix + ".keeper-*.csv")))

        records.append(
            dict(
                rep=rep, arm=arm, family=fam, config=cfg_val, tag=tag,
                n_latency=len(lat),
                lat_p50=pct(lat, 50), lat_p90=pct(lat, 90), lat_p99=pct(lat, 99),
                lat_max=float(lat.max()) if len(lat) else float("nan"),
                pb_p50=pct(pbk, 50), pb_p99=pct(pbk, 99),
                wr_p50=pct(wrt, 50), wr_p99=pct(wrt, 99),
                keeper_cpu_pct=cpu, keeper_rss_mb=rss,
                logged=scal["logged"], never_seen=scal["never_seen"],
                applied_ev_s=scal["applied_ev_s"],
                _lat=lat, _pb_warmup_s=pb_warmup,
            )
        )
    return records


METRIC_COLS = [
    "lat_p50", "lat_p90", "lat_p99", "lat_max",
    "pb_p50", "pb_p99", "wr_p50", "wr_p99",
    "keeper_cpu_pct", "keeper_rss_mb", "applied_ev_s", "never_seen",
]


def aggregate(records):
    """Median across reps for each (family, arm, config)."""
    groups = defaultdict(list)
    for r in records:
        groups[(r["family"], r["arm"], r["config"])].append(r)
    out = []
    for (fam, arm, cfg), rs in sorted(groups.items()):
        row = dict(family=fam, arm=arm, config=cfg, reps=len(rs))
        for col in METRIC_COLS:
            vals = [r[col] for r in rs if not np.isnan(r[col])]
            row[col] = float(np.median(vals)) if vals else float("nan")
        out.append(row)
    return out


def write_csv(path, records):
    cols = ["rep", "arm", "family", "config", "tag", "n_latency", "logged"] + METRIC_COLS
    with open(path, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=cols, extrasaction="ignore")
        w.writeheader()
        for r in sorted(records, key=lambda r: (r["family"], r["config"], r["arm"], r["rep"])):
            w.writerow(r)


CFG_NAME = {"A": "ranks", "B1": "poll_ms", "B2": "playback_n", "SMOKE": "-"}


def fmt(v, nd=1):
    return "-" if v is None or (isinstance(v, float) and np.isnan(v)) else f"{v:,.{nd}f}"


def write_markdown(path, agg, info, warmup_s):
    lines = ["# live_tail_read A/B — results", ""]
    for k in ("job_id", "keepers", "clients", "seal_window", "tail_capacity", "payload", "git_commit"):
        for key, val in info.items():
            if key.startswith(k):
                lines.append(f"- `{key}` = {val}")
    lines += ["", f"Warmup trimmed: {warmup_s:.0f}s (B2 playback additionally trimmed until the story fills to depth).", ""]

    for fam in ("A", "B1", "B2", "SMOKE"):
        rows = [r for r in agg if r["family"] == fam]
        if not rows:
            continue
        lines += [f"## Family {fam}", "",
                  f"| {CFG_NAME[fam]} | arm | send→visible p50 (ms) | p99 (ms) | playback() p50 (us) | playback() p99 (us) "
                  f"| log_event() p50 (us) | p99 (us) | keeper CPU (% core) | keeper RSS (MB) | never seen |",
                  "|---|---|---|---|---|---|---|---|---|---|---|"]
        flagged = False
        for r in sorted(rows, key=lambda r: (r["config"], ARM_ORDER.index(r["arm"]))):
            # A materially truncated sample means the dropped events are exactly
            # the slowest ones, so the latency columns are a lower bound and are
            # NOT comparable between arms (the arms drop different fractions).
            drop = r["never_seen"] / r["logged"] if r.get("logged") else 0.0
            suspect = drop > 0.05
            flagged = flagged or suspect
            mark = "†" if suspect else ""
            lines.append(
                f"| {r['config']} | {r['arm']} | {fmt(r['lat_p50'])}{mark} | {fmt(r['lat_p99'])}{mark} "
                f"| {fmt(r['pb_p50'])} | {fmt(r['pb_p99'])} | {fmt(r['wr_p50'])} | {fmt(r['wr_p99'])} "
                f"| {fmt(r['keeper_cpu_pct'])} | {fmt(r['keeper_rss_mb'])} | {fmt(r['never_seen'], 0)} |"
            )
        lines.append("")
        if flagged:
            lines += [
                "† >5% of events never became visible within max_wait, so these latency figures are a "
                "LOWER BOUND and are **not comparable between arms** — the two arms truncate different "
                "fractions of the sample. This is expected on the shared-story families, where playback_n "
                "is deliberately smaller than the events-per-chunk-period; their metrics are the "
                "playback() and log_event() service times, which are per-call and unaffected.",
                "",
            ]
    open(path, "w").write("\n".join(lines))


# --------------------------------------------------------------------------
# Figures
# --------------------------------------------------------------------------

def style_axes(ax, xlabel, ylabel, title=None):
    ax.set_facecolor(SURFACE)
    ax.grid(True, color=GRID, linewidth=0.8, alpha=0.7)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
    ax.tick_params(colors=TEXT_SECONDARY, labelsize=9)
    ax.set_xlabel(xlabel, color=TEXT_SECONDARY, fontsize=10)
    ax.set_ylabel(ylabel, color=TEXT_SECONDARY, fontsize=10)
    if title:
        ax.set_title(title, color=TEXT_PRIMARY, fontsize=11, loc="left", pad=8)


def fig_cdf(records, out_path):
    """Family A: send->visible CDF, one facet per rank count, log x."""
    import matplotlib.pyplot as plt

    pts = sorted({r["config"] for r in records if r["family"] == "A"})
    if not pts:
        return None
    fig, axes = plt.subplots(1, len(pts), figsize=(3.6 * len(pts), 4.4), squeeze=False,
                             sharey=True, facecolor=SURFACE)
    handles = []
    for i, (ax, cfg) in enumerate(zip(axes[0], pts)):
        for arm in ARM_ORDER:
            pooled = np.concatenate(
                [r["_lat"] for r in records if r["family"] == "A" and r["arm"] == arm and r["config"] == cfg]
                or [np.array([])]
            )
            pooled = pooled[np.isfinite(pooled)]
            if pooled.size == 0:
                continue
            xs = np.sort(pooled)
            ys = np.arange(1, xs.size + 1) / xs.size
            (line,) = ax.plot(xs, ys, color=ARM_COLOR[arm], linewidth=2, label=ARM_LABEL[arm])
            if i == 0:
                handles.append(line)
        ax.set_xscale("log")
        ax.set_ylim(0, 1.02)
        # Only the leftmost facet carries the y-label; repeating it is noise.
        style_axes(ax, "send→visible latency (ms, log)", "cumulative fraction" if i == 0 else "", f"{cfg} ranks")
    # Figure-level legend above the facets, so it cannot collide with a curve.
    if handles:
        fig.legend(handles=handles, frameon=False, fontsize=9, labelcolor=TEXT_SECONDARY,
                   loc="upper left", bbox_to_anchor=(0.008, 0.94), ncol=len(handles))
    fig.suptitle("Tail-read freshness: sealed-only vs live tail read",
                 color=TEXT_PRIMARY, fontsize=13, x=0.008, y=0.985, ha="left")
    fig.tight_layout(rect=(0, 0, 1, 0.88))
    fig.savefig(out_path, dpi=150, facecolor=SURFACE)
    plt.close(fig)
    return out_path


def fig_sweep(agg, family, y_p50, y_p99, xlabel, ylabel, title, out_path, logx=True):
    """Line-with-markers sweep, p50 solid and p99 dashed, two arms."""
    import matplotlib.pyplot as plt

    rows = [r for r in agg if r["family"] == family]
    if not rows:
        return None
    fig, ax = plt.subplots(figsize=(7.2, 4.4), facecolor=SURFACE)
    for arm in ARM_ORDER:
        sel = sorted([r for r in rows if r["arm"] == arm], key=lambda r: r["config"])
        if not sel:
            continue
        xs = [r["config"] for r in sel]
        ax.plot(xs, [r[y_p50] for r in sel], color=ARM_COLOR[arm], linewidth=2,
                marker="o", markersize=8, label=f"{ARM_LABEL[arm]} — p50")
        ax.plot(xs, [r[y_p99] for r in sel], color=ARM_COLOR[arm], linewidth=2,
                linestyle="--", marker="s", markersize=8, alpha=0.85,
                label=f"{ARM_LABEL[arm]} — p99")
    if logx:
        ax.set_xscale("log", base=2)
        ax.set_xticks([r["config"] for r in rows])
        ax.get_xaxis().set_major_formatter(plt.ScalarFormatter())
    ax.set_yscale("log")
    style_axes(ax, xlabel, ylabel, title)
    ax.legend(frameon=False, fontsize=9, labelcolor=TEXT_SECONDARY)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150, facecolor=SURFACE)
    plt.close(fig)
    return out_path


def fig_keeper_cpu(agg, out_path):
    """Grouped bars: keeper CPU per arm, one group per sweep point."""
    import matplotlib.pyplot as plt

    rows = [r for r in agg if r["family"] in ("A", "B1", "B2") and not np.isnan(r["keeper_cpu_pct"])]
    if not rows:
        return None
    keys = sorted({(r["family"], r["config"]) for r in rows})
    labels = [f"{f}\n{CFG_NAME[f]}={c}" for f, c in keys]
    x = np.arange(len(keys))
    width = 0.38
    fig, ax = plt.subplots(figsize=(max(7.2, 0.85 * len(keys)), 4.4), facecolor=SURFACE)
    for i, arm in enumerate(ARM_ORDER):
        vals = []
        for f, c in keys:
            match = [r["keeper_cpu_pct"] for r in rows if r["family"] == f and r["config"] == c and r["arm"] == arm]
            vals.append(match[0] if match else np.nan)
        # 2px surface gap between adjacent bars
        ax.bar(x + (i - 0.5) * width, vals, width * 0.94, color=ARM_COLOR[arm],
               label=ARM_LABEL[arm], edgecolor=SURFACE, linewidth=2)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=8)
    style_axes(ax, "", "keeper CPU (% of one core)", "Keeper CPU cost of serving the active tail")
    ax.legend(frameon=False, fontsize=9, labelcolor=TEXT_SECONDARY)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150, facecolor=SURFACE)
    plt.close(fig)
    return out_path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results_dir")
    ap.add_argument("--warmup", type=float, default=10.0, help="seconds trimmed from the start of each run")
    ap.add_argument("--no-plots", action="store_true")
    args = ap.parse_args()

    info = read_manifest(args.results_dir)
    records = collect(args.results_dir, args.warmup)
    if not records:
        print(f"No sweep points found under {args.results_dir}", file=sys.stderr)
        return 1

    agg = aggregate(records)
    write_csv(os.path.join(args.results_dir, "summary.csv"), records)
    write_markdown(os.path.join(args.results_dir, "summary.md"), agg, info, args.warmup)
    print(f"Wrote summary.csv and summary.md ({len(records)} sweep points, {len(agg)} aggregated)")

    stale = [r for r in records if r["family"] == "A" and r["never_seen"] > 0]
    if stale:
        print("WARNING: Family A points with never_seen > 0 — those latency figures are LOWER BOUNDS:")
        for r in stale:
            print(f"  {r['rep']}/{r['tag']}: never_seen={r['never_seen']:.0f} of {r['logged']:.0f}")

    if args.no_plots:
        return 0
    made = [
        fig_cdf(records, os.path.join(args.results_dir, "fig_A_cdf.png")),
        fig_sweep(agg, "B2", "pb_p50", "pb_p99", "playback_n (tail depth)", "playback() service time (us, log)",
                  "Read-path cost vs tail depth", os.path.join(args.results_dir, "fig_B2_playback.png")),
        fig_sweep(agg, "B1", "wr_p50", "wr_p99", "poll interval (ms) — lower is more read pressure",
                  "log_event() service time (us, log)", "Write-path interference from concurrent tail reads",
                  os.path.join(args.results_dir, "fig_B1_write.png")),
        fig_keeper_cpu(agg, os.path.join(args.results_dir, "fig_keeper_cpu.png")),
    ]
    for p in made:
        if p:
            print(f"Wrote {os.path.basename(p)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
