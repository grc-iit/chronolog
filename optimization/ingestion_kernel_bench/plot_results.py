#!/usr/bin/env python3
"""
plot_results.py — Visualize ingestion_kernel_bench sweep results.

Reads the CSV produced by sweep.sh and writes plots into <out_dir>.

LOCAL CSV columns (no n_clients):
  queue,threads,stories,total_events,ingest_ms,merge_ms,pipeline_ms,
  ingest_Mevs,pipeline_Mevs

NETWORK CSV columns (with n_clients):
  rpc,queue,threads,n_clients,stories,total_events,ingest_ms,merge_ms,
  pipeline_ms,ingest_Mevs,pipeline_Mevs

Plots always generated:
  throughput_vs_threads.png     — Mev/s vs threads, one subplot per n_clients

Multi-client plots (when distinct n_clients > 1 values are present):
  throughput_vs_clients.png     — Mev/s vs n_clients, subplots for selected threads
  throughput_heatmap.png        — full (n_clients × threads) throughput grid per queue
  queue_speedup_heatmap.png     — lockfree/mutex ratio grid (n_clients × threads)
  client_scaling_speedup.png    — speedup(n_clients) vs ideal linear, per queue

Usage:
    python3 plot_results.py results/results_network.csv [results/]
"""

import csv
import sys
from collections import defaultdict
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import matplotlib.colors as mcolors
    import numpy as np
except ImportError:
    print("matplotlib/numpy not installed.  pip install matplotlib numpy")
    sys.exit(1)


QUEUE_STYLES = {
    "mutex":    {"color": "#1f77b4", "marker": "o", "linestyle": "-",
                 "label": "mutex+std::deque"},
    "lockfree": {"color": "#d62728", "marker": "s", "linestyle": "--",
                 "label": "moodycamel (lock-free)"},
}

RPC_LINESTYLE = {"sendrecv": "-", "rdma": ":"}


def load_rows(csv_path):
    rows = []
    with open(csv_path) as f:
        reader = csv.DictReader(f)
        for r in reader:
            rows.append({
                "queue":         r["queue"],
                "threads":       int(r["threads"]),
                "stories":       int(r["stories"]),
                "total_events":  int(r["total_events"]),
                "ingest_ms":     float(r["ingest_ms"]),
                "merge_ms":      float(r["merge_ms"]),
                "pipeline_ms":   float(r["pipeline_ms"]),
                "ingest_Mevs":   float(r["ingest_Mevs"]),
                "pipeline_Mevs": float(r["pipeline_Mevs"]),
                "rpc":           r.get("rpc", "sendrecv"),
                "n_clients":     int(r.get("n_clients", 1)),
            })
    return rows


def _qstyle(queue, rpc="sendrecv", alpha=1.0):
    base = QUEUE_STYLES.get(queue, {
        "color": "gray", "marker": "^", "linestyle": "-.", "label": queue,
    })
    ls = RPC_LINESTYLE.get(rpc, base["linestyle"])
    lbl = base["label"] + (f" ({rpc})" if rpc != "sendrecv" else "")
    return {**base, "linestyle": ls, "label": lbl, "alpha": alpha}


def _nc_alpha(nc_idx, n_nc):
    """Map n_clients index to alpha: lightest for fewest clients, full for most."""
    return 0.35 + 0.65 * nc_idx / max(n_nc - 1, 1)


# ---------------------------------------------------------------------------
# Plot 1: throughput vs threads, one subplot per n_clients value
# Lines: one per queue (and rpc if multiple).
# ---------------------------------------------------------------------------
def plot_throughput_vs_threads(rows, out_path, subtitle):
    nc_vals  = sorted({r["n_clients"] for r in rows})
    rpc_vals = sorted({r["rpc"]       for r in rows})
    n_nc = len(nc_vals)

    fig, axes = plt.subplots(1, n_nc, figsize=(5 * n_nc, 5), sharey=True,
                              squeeze=False)

    for i, nc in enumerate(nc_vals):
        ax = axes[0][i]
        subset = [r for r in rows if r["n_clients"] == nc]
        by_key = defaultdict(list)
        for r in subset:
            by_key[(r["queue"], r["rpc"])].append((r["threads"], r["ingest_Mevs"]))

        for (q, rpc), pts in sorted(by_key.items()):
            pts.sort()
            ts, vals = zip(*pts)
            s = _qstyle(q, rpc)
            ax.plot(ts, vals, color=s["color"], marker=s["marker"],
                    linestyle=s["linestyle"], linewidth=2, markersize=7,
                    label=s["label"])

        all_t = sorted({r["threads"] for r in subset})
        ax.set_title(f"n_clients = {nc}")
        ax.set_xlabel("Server ingestion threads")
        if i == 0:
            ax.set_ylabel("Ingest throughput (Mev/s)")
        ax.set_xticks(all_t)
        ax.tick_params(axis="x", rotation=45)
        ax.grid(True, alpha=0.3)
        ax.legend(loc="best", fontsize=8)

    fig.suptitle(f"Throughput vs server threads  {subtitle}")
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"Plot: {out_path}")


# ---------------------------------------------------------------------------
# Plot 2: throughput vs n_clients, subplots for selected thread counts
# ---------------------------------------------------------------------------
_SELECTED_THREADS_N = 4   # how many thread counts to show as subplots

def plot_throughput_vs_clients(rows, out_path, subtitle):
    thread_vals = sorted({r["threads"] for r in rows})
    rpc_vals    = sorted({r["rpc"]     for r in rows})

    # Pick representative thread counts evenly spaced across the range
    if len(thread_vals) <= _SELECTED_THREADS_N:
        sel_threads = thread_vals
    else:
        step = (len(thread_vals) - 1) / (_SELECTED_THREADS_N - 1)
        sel_threads = [thread_vals[round(i * step)] for i in range(_SELECTED_THREADS_N)]

    n_sub = len(sel_threads)
    fig, axes = plt.subplots(1, n_sub, figsize=(5 * n_sub, 5), sharey=True,
                              squeeze=False)

    for i, t in enumerate(sel_threads):
        ax = axes[0][i]
        subset = [r for r in rows if r["threads"] == t]
        by_key = defaultdict(list)
        for r in subset:
            by_key[(r["queue"], r["rpc"])].append((r["n_clients"], r["ingest_Mevs"]))

        for (q, rpc), pts in sorted(by_key.items()):
            pts.sort()
            nc, vals = zip(*pts)
            s = _qstyle(q, rpc)
            ax.plot(nc, vals, color=s["color"], marker=s["marker"],
                    linestyle=s["linestyle"], linewidth=2, markersize=7,
                    label=s["label"])

        all_nc = sorted({r["n_clients"] for r in subset})
        ax.set_title(f"threads = {t}")
        ax.set_xlabel("Total client processes")
        if i == 0:
            ax.set_ylabel("Ingest throughput (Mev/s)")
        ax.set_xticks(all_nc)
        ax.grid(True, alpha=0.3)
        ax.legend(loc="best", fontsize=8)

    fig.suptitle(f"Throughput vs #clients  {subtitle}")
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"Plot: {out_path}")


# ---------------------------------------------------------------------------
# Plot 3: 2D throughput heatmap  (n_clients rows × threads cols)
# One panel per (rpc, queue) pair.
# ---------------------------------------------------------------------------
def plot_throughput_heatmap(rows, out_path, subtitle):
    nc_vals     = sorted({r["n_clients"] for r in rows})
    thread_vals = sorted({r["threads"]   for r in rows})
    queue_vals  = sorted({r["queue"]     for r in rows})
    rpc_vals    = sorted({r["rpc"]       for r in rows})

    idx = {(r["queue"], r["rpc"], r["n_clients"], r["threads"]): r["ingest_Mevs"]
           for r in rows}

    n_rows = len(rpc_vals)
    n_cols = len(queue_vals)
    fig, axes = plt.subplots(n_rows, n_cols,
                              figsize=(5 * n_cols, 1 + 0.6 * len(nc_vals)) * n_rows,
                              squeeze=False)
    # Fix figsize — recompute
    plt.close(fig)
    fig, axes = plt.subplots(n_rows, n_cols,
                              figsize=(5.5 * n_cols, 1.5 + 0.55 * len(nc_vals) * n_rows),
                              squeeze=False)

    for ri, rpc in enumerate(rpc_vals):
        for qi, q in enumerate(queue_vals):
            ax = axes[ri][qi]
            mat = np.full((len(nc_vals), len(thread_vals)), np.nan)
            for ni, nc in enumerate(nc_vals):
                for ti, t in enumerate(thread_vals):
                    v = idx.get((q, rpc, nc, t))
                    if v is not None:
                        mat[ni, ti] = v

            vmax = np.nanmax(mat) if not np.all(np.isnan(mat)) else 1.0
            im = ax.imshow(mat, aspect="auto", cmap="viridis", vmin=0, vmax=vmax)

            for ni in range(len(nc_vals)):
                for ti in range(len(thread_vals)):
                    v = mat[ni, ti]
                    if not np.isnan(v):
                        tc = "white" if v < 0.55 * vmax else "black"
                        ax.text(ti, ni, f"{v:.1f}", ha="center", va="center",
                                fontsize=7, color=tc)

            ax.set_xticks(range(len(thread_vals)))
            ax.set_xticklabels(thread_vals, fontsize=8)
            ax.set_yticks(range(len(nc_vals)))
            ax.set_yticklabels(nc_vals, fontsize=8)
            ax.set_xlabel("Server threads")
            ax.set_ylabel("n_clients")
            ax.set_title(f"{q}  [{rpc}]")
            plt.colorbar(im, ax=ax, label="Mev/s", shrink=0.85)

    fig.suptitle(f"Ingest throughput (Mev/s)  {subtitle}")
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"Plot: {out_path}")


# ---------------------------------------------------------------------------
# Plot 4: queue speedup heatmap  lockfree/mutex  (n_clients × threads)
# ---------------------------------------------------------------------------
def plot_queue_speedup_heatmap(rows, out_path, subtitle):
    nc_vals     = sorted({r["n_clients"] for r in rows})
    thread_vals = sorted({r["threads"]   for r in rows})
    rpc_vals    = sorted({r["rpc"]       for r in rows})

    idx = {(r["queue"], r["rpc"], r["n_clients"], r["threads"]): r["ingest_Mevs"]
           for r in rows}

    n_rpc = len(rpc_vals)
    fig, axes = plt.subplots(1, n_rpc,
                              figsize=(5.5 * n_rpc, 1.5 + 0.55 * len(nc_vals)),
                              squeeze=False)

    for ri, rpc in enumerate(rpc_vals):
        ax = axes[0][ri]
        mat = np.full((len(nc_vals), len(thread_vals)), np.nan)
        for ni, nc in enumerate(nc_vals):
            for ti, t in enumerate(thread_vals):
                m  = idx.get(("mutex",    rpc, nc, t))
                lf = idx.get(("lockfree", rpc, nc, t))
                if m and lf and m > 0:
                    mat[ni, ti] = lf / m

        vmax = max(2.0, np.nanmax(mat)) if not np.all(np.isnan(mat)) else 2.0
        im = ax.imshow(mat, aspect="auto", cmap="RdYlGn", vmin=0.5, vmax=vmax)

        for ni in range(len(nc_vals)):
            for ti in range(len(thread_vals)):
                v = mat[ni, ti]
                if not np.isnan(v):
                    ax.text(ti, ni, f"{v:.2f}x", ha="center", va="center", fontsize=8)

        ax.set_xticks(range(len(thread_vals)))
        ax.set_xticklabels(thread_vals, fontsize=8)
        ax.set_yticks(range(len(nc_vals)))
        ax.set_yticklabels(nc_vals, fontsize=8)
        ax.set_xlabel("Server threads")
        ax.set_ylabel("n_clients")
        ax.set_title(f"lockfree / mutex  [{rpc}]")
        plt.colorbar(im, ax=ax, label="speedup ratio", shrink=0.85)

    fig.suptitle(f"Container optimization speedup  {subtitle}")
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"Plot: {out_path}")


# ---------------------------------------------------------------------------
# Plot 5: client scaling speedup
# Left:  speedup(n_clients) / speedup(n_clients_min) per (queue, rpc)
#        at max thread count — shows horizontal scaling efficiency
# Right: lockfree/mutex speedup vs n_clients at max thread count
# ---------------------------------------------------------------------------
def plot_client_scaling_speedup(rows, out_path, subtitle):
    thread_vals = sorted({r["threads"]   for r in rows})
    rpc_vals    = sorted({r["rpc"]       for r in rows})
    nc_vals     = sorted({r["n_clients"] for r in rows})
    max_t = thread_vals[-1]

    idx = {(r["queue"], r["rpc"], r["n_clients"], r["threads"]): r["ingest_Mevs"]
           for r in rows}

    fig, axes = plt.subplots(1, 2, figsize=(13, 5))

    # ---- left: client scaling speedup for each (queue, rpc) at max threads ----
    for rpc in rpc_vals:
        for q in sorted({r["queue"] for r in rows}):
            base = idx.get((q, rpc, nc_vals[0], max_t))
            if not base or base <= 0:
                continue
            speedups = [idx.get((q, rpc, nc, max_t), float("nan")) / base
                        for nc in nc_vals]
            s = _qstyle(q, rpc)
            axes[0].plot(nc_vals, speedups, color=s["color"], marker=s["marker"],
                         linestyle=s["linestyle"], linewidth=2, markersize=7,
                         label=s["label"])

    if nc_vals:
        ideal = [nc / nc_vals[0] for nc in nc_vals]
        axes[0].plot(nc_vals, ideal, "k--", alpha=0.4, linewidth=1.5,
                     label="ideal linear")

    axes[0].set_xlabel("Total client processes")
    axes[0].set_ylabel(f"Throughput speedup vs {nc_vals[0]} clients")
    axes[0].set_title(f"Client scaling  (threads={max_t})")
    axes[0].set_xticks(nc_vals)
    axes[0].grid(True, alpha=0.3)
    axes[0].legend(loc="best", fontsize=8)

    # ---- right: lockfree/mutex ratio at each n_clients, grouped by rpc ----
    rpc_colors = {"sendrecv": "#2ca02c", "rdma": "#ff7f0e",
                  "default": "#9467bd"}
    x = np.arange(len(nc_vals))
    bar_w = 0.8 / max(len(rpc_vals), 1)

    for ri, rpc in enumerate(rpc_vals):
        ratios = []
        for nc in nc_vals:
            m  = idx.get(("mutex",    rpc, nc, max_t))
            lf = idx.get(("lockfree", rpc, nc, max_t))
            ratios.append(lf / m if (m and lf and m > 0) else float("nan"))
        offset = (ri - (len(rpc_vals) - 1) / 2) * bar_w
        color = rpc_colors.get(rpc, rpc_colors["default"])
        bars = axes[1].bar(x + offset, ratios, bar_w, color=color,
                           alpha=0.85, label=rpc)
        for bar, ratio in zip(bars, ratios):
            if not np.isnan(ratio):
                axes[1].text(bar.get_x() + bar.get_width() / 2,
                             bar.get_height() + 0.02,
                             f"{ratio:.2f}x", ha="center", va="bottom",
                             fontsize=8)

    axes[1].axhline(1.0, color="k", linestyle="--", alpha=0.5)
    axes[1].set_xticks(x)
    axes[1].set_xticklabels([f"{nc}" for nc in nc_vals])
    axes[1].set_xlabel("Total client processes")
    axes[1].set_ylabel("Speedup (lockfree / mutex)")
    axes[1].set_title(f"Queue optimization benefit  (threads={max_t})")
    axes[1].grid(True, alpha=0.3, axis="y")
    if rpc_vals:
        axes[1].legend(loc="best", fontsize=8)

    fig.suptitle(f"Multi-dimensional speedup  {subtitle}")
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"Plot: {out_path}")


# ---------------------------------------------------------------------------
# Legacy latency breakdown (kept for local-mode CSV compat)
# ---------------------------------------------------------------------------
def plot_latency_breakdown(rows, out_path, subtitle):
    # Use only the first n_clients slice to avoid mixed data
    nc_min = min(r["n_clients"] for r in rows)
    rpc_first = sorted({r["rpc"] for r in rows})[0]
    subset = [r for r in rows if r["n_clients"] == nc_min and r["rpc"] == rpc_first]

    queues  = sorted({r["queue"]   for r in subset})
    threads = sorted({r["threads"] for r in subset})
    if not queues or not threads:
        return

    by = {(r["queue"], r["threads"]): r for r in subset}
    n_groups = len(threads)
    n_queues = len(queues)
    width = 0.8 / n_queues
    x = np.arange(n_groups)

    fig, ax = plt.subplots(figsize=(max(9, n_groups * 1.2), 5))
    for qi, q in enumerate(queues):
        ingest_ms = [by[(q, t)]["ingest_ms"] for t in threads if (q, t) in by]
        merge_ms  = [by[(q, t)]["merge_ms"]  for t in threads if (q, t) in by]
        present   = [t                        for t in threads if (q, t) in by]
        offset = (qi - (n_queues - 1) / 2) * width
        s = _qstyle(q)
        ax.bar(x[:len(present)] + offset, ingest_ms, width,
               color=s["color"], alpha=0.85, label=f"{s['label']} (ingest)")
        ax.bar(x[:len(present)] + offset, merge_ms, width,
               color=s["color"], alpha=0.45, bottom=ingest_ms, hatch="//",
               label=f"{s['label']} (merge)")

    ax.set_xticks(x)
    ax.set_xticklabels(threads, rotation=45)
    ax.set_xlabel("Server ingestion threads")
    ax.set_ylabel("Wall time (ms)")
    ax.set_title(f"Latency breakdown  (n_clients={nc_min}, rpc={rpc_first})  {subtitle}")
    ax.grid(True, alpha=0.3, axis="y")
    ax.legend(loc="best", fontsize=8)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"Plot: {out_path}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    csv_path = sys.argv[1]
    out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("results")
    out_dir.mkdir(parents=True, exist_ok=True)

    rows = load_rows(csv_path)
    if not rows:
        print(f"No rows in {csv_path}")
        sys.exit(1)

    nc_vals = sorted({r["n_clients"] for r in rows})
    n_clients_per_client = rows[0]["total_events"] // max(nc_vals[0], 1)
    subtitle = (f"(stories={rows[0]['stories']}, "
                f"events/client≈{n_clients_per_client:,})")

    # ---- always: throughput vs threads (one subplot per n_clients) ----
    plot_throughput_vs_threads(
        rows, str(out_dir / "throughput_vs_threads.png"), subtitle)

    # ---- always: latency breakdown at baseline n_clients ----
    plot_latency_breakdown(
        rows, str(out_dir / "latency_breakdown.png"), subtitle)

    # ---- multi-client plots (n_clients varies) ----
    if len(nc_vals) > 1 or nc_vals[0] > 1:
        plot_throughput_vs_clients(
            rows, str(out_dir / "throughput_vs_clients.png"), subtitle)
        plot_throughput_heatmap(
            rows, str(out_dir / "throughput_heatmap.png"), subtitle)
        plot_queue_speedup_heatmap(
            rows, str(out_dir / "queue_speedup_heatmap.png"), subtitle)
        plot_client_scaling_speedup(
            rows, str(out_dir / "client_scaling_speedup.png"), subtitle)

    print(f"\nAll plots written to {out_dir}/")


if __name__ == "__main__":
    main()
