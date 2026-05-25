#!/usr/bin/env python3
"""
Generate an interactive HTML page showing the send/recv vs RDMA crossover surface
across message size and message count, for ofi+sockets vs ofi+verbs.

Reads CSV output from thallium_crossover_benchmark.sh (run with -p both).

Usage:
    python plot_crossover.py results_both.csv                     # combined CSV
    python plot_crossover.py -s sockets.csv -v verbs.csv          # separate CSVs
    python plot_crossover.py results_both.csv -o crossover.html   # custom output

The generated HTML is fully standalone (uses Plotly.js CDN) and interactive:
  - Heatmap showing verbs/sockets speedup across (msg_size, rep_count)
  - 3D surface of the same data
  - Per-protocol latency and bandwidth curves
  - Crossover boundary highlighted
"""

import argparse
import csv
import json
import sys
import os
from collections import defaultdict

def parse_csv(filepath):
    """Parse benchmark CSV into list of dicts. Handles both single and combined formats."""
    rows = []
    with open(filepath) as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(row)
    return rows

def build_data_matrix(rows):
    """
    Build lookup: data[protocol][mode][(msg_size, reps)] = {latency_us, bandwidth_MBps}
    Handles both single-protocol (no 'protocol' column) and combined CSVs.
    """
    data = defaultdict(lambda: defaultdict(dict))
    for row in rows:
        proto = row.get('protocol', 'unknown')
        mode = row['mode']
        msg_size = int(row['msg_size'])
        reps = int(row['repetitions'])

        if 'avg_latency_us' in row:
            lat = float(row['avg_latency_us'])
        else:
            lat = 0.0

        if 'bandwidth_MBps' in row:
            bw = float(row['bandwidth_MBps'])
        elif 'bw_per_client_MBps' in row:
            bw = float(row['bw_per_client_MBps'])
        else:
            bw = 0.0

        data[proto][mode][(msg_size, reps)] = {'latency_us': lat, 'bandwidth_MBps': bw}
    return data

def generate_html(data, output_path):
    """Generate interactive HTML with Plotly.js visualizations."""

    # Collect all unique msg_sizes and rep_counts across all protocols/modes
    all_keys = set()
    for proto in data:
        for mode in data[proto]:
            all_keys.update(data[proto][mode].keys())

    msg_sizes = sorted(set(k[0] for k in all_keys))
    rep_counts = sorted(set(k[1] for k in all_keys))

    # Build JSON data for each protocol and mode
    plot_data = {}
    for proto in data:
        plot_data[proto] = {}
        for mode in data[proto]:
            latencies = []
            bandwidths = []
            for reps in rep_counts:
                lat_row = []
                bw_row = []
                for size in msg_sizes:
                    entry = data[proto][mode].get((size, reps), {})
                    lat_row.append(entry.get('latency_us', None))
                    bw_row.append(entry.get('bandwidth_MBps', None))
                latencies.append(lat_row)
                bandwidths.append(bw_row)
            plot_data[proto][mode] = {
                'latencies': latencies,
                'bandwidths': bandwidths,
            }

    # Compute speedup matrices (sockets_latency / verbs_latency) for each mode
    speedup_data = {}
    protos = list(data.keys())
    has_both = len(protos) >= 2

    if has_both:
        # Find sockets and verbs protocols
        sockets_proto = next((p for p in protos if 'socket' in p.lower()), protos[0])
        verbs_proto = next((p for p in protos if 'verbs' in p.lower() or p != sockets_proto), protos[1])

        for mode in set(data[sockets_proto].keys()) & set(data[verbs_proto].keys()):
            speedup_matrix = []
            for reps in rep_counts:
                row = []
                for size in msg_sizes:
                    s_entry = data[sockets_proto][mode].get((size, reps), {})
                    v_entry = data[verbs_proto][mode].get((size, reps), {})
                    s_lat = s_entry.get('latency_us', None)
                    v_lat = v_entry.get('latency_us', None)
                    if s_lat and v_lat and v_lat > 0:
                        row.append(round(s_lat / v_lat, 3))
                    else:
                        row.append(None)
                speedup_matrix.append(row)
            speedup_data[mode] = speedup_matrix

    # Human-readable size labels
    def fmt_size(s):
        if s >= 1048576:
            return f"{s // 1048576}MB"
        elif s >= 1024:
            return f"{s // 1024}KB"
        return f"{s}B"

    size_labels = [fmt_size(s) for s in msg_sizes]
    rep_labels = [str(r) for r in rep_counts]

    # Prepare JSON for embedding
    js_data = json.dumps({
        'msg_sizes': msg_sizes,
        'rep_counts': rep_counts,
        'size_labels': size_labels,
        'rep_labels': rep_labels,
        'plot_data': plot_data,
        'speedup_data': speedup_data,
        'protocols': protos,
        'has_both': has_both,
        'has_reps': len(rep_counts) > 1,
    }, indent=2)

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>Thallium Send/Recv vs RDMA Crossover Analysis</title>
<script src="https://cdn.plot.ly/plotly-2.35.2.min.js"></script>
<style>
  * {{ margin: 0; padding: 0; box-sizing: border-box; }}
  body {{ font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
         background: #1a1a2e; color: #e0e0e0; }}
  .header {{ background: #16213e; padding: 20px 30px; border-bottom: 2px solid #0f3460; }}
  .header h1 {{ font-size: 1.4em; color: #e94560; }}
  .header p {{ font-size: 0.9em; color: #888; margin-top: 4px; }}
  .tabs {{ display: flex; background: #16213e; padding: 0 20px; gap: 2px; }}
  .tab {{ padding: 10px 20px; cursor: pointer; background: #1a1a2e; color: #888;
          border-top: 2px solid transparent; font-size: 0.9em; transition: all 0.2s; }}
  .tab:hover {{ color: #e0e0e0; }}
  .tab.active {{ color: #e94560; border-top-color: #e94560; background: #1a1a2e; }}
  .panel {{ display: none; padding: 20px; }}
  .panel.active {{ display: block; }}
  .plot {{ width: 100%; height: 600px; }}
  .note {{ font-size: 0.85em; color: #666; padding: 10px 30px; }}
  .side-by-side {{ display: flex; gap: 10px; }}
  .side-by-side > div {{ flex: 1; }}
</style>
</head>
<body>

<div class="header">
  <h1>Thallium Send/Recv vs RDMA Crossover Analysis</h1>
  <p>Interactive visualization of transport protocol performance across message sizes and repetitions</p>
</div>

<div class="tabs" id="tabs"></div>
<div id="panels"></div>
<div class="note" id="note"></div>

<script>
const D = {js_data};

// --- Tab management ---
const tabDefs = [];

if (D.has_both && D.has_reps) {{
  tabDefs.push({{ id: 'heatmap', label: 'Speedup Heatmap' }});
  tabDefs.push({{ id: 'surface', label: '3D Surface' }});
}}
tabDefs.push({{ id: 'latency', label: 'Latency Comparison' }});
tabDefs.push({{ id: 'bandwidth', label: 'Bandwidth Comparison' }});
if (D.has_both && !D.has_reps) {{
  tabDefs.push({{ id: 'speedup1d', label: 'Speedup (1D)' }});
}}

const tabsEl = document.getElementById('tabs');
const panelsEl = document.getElementById('panels');

tabDefs.forEach((t, i) => {{
  const tab = document.createElement('div');
  tab.className = 'tab' + (i === 0 ? ' active' : '');
  tab.textContent = t.label;
  tab.onclick = () => switchTab(t.id);
  tab.id = 'tab-' + t.id;
  tabsEl.appendChild(tab);

  const panel = document.createElement('div');
  panel.className = 'panel' + (i === 0 ? ' active' : '');
  panel.id = 'panel-' + t.id;
  panel.innerHTML = '<div class="plot" id="plot-' + t.id + '"></div>';
  panelsEl.appendChild(panel);
}});

function switchTab(id) {{
  document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
  document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
  document.getElementById('tab-' + id).classList.add('active');
  document.getElementById('panel-' + id).classList.add('active');
  Plotly.Plots.resize(document.getElementById('plot-' + id));
}}

const darkLayout = {{
  paper_bgcolor: '#1a1a2e',
  plot_bgcolor: '#1a1a2e',
  font: {{ color: '#e0e0e0', size: 12 }},
  margin: {{ t: 50, b: 60, l: 70, r: 30 }},
}};

// --- Heatmap: speedup(msg_size, reps) ---
if (D.has_both && D.has_reps) {{
  const mode = 'recv';  // primary comparison mode
  const z = D.speedup_data[mode] || D.speedup_data[Object.keys(D.speedup_data)[0]];
  if (z) {{
    Plotly.newPlot('plot-heatmap', [{{
      z: z,
      x: D.size_labels,
      y: D.rep_labels,
      type: 'heatmap',
      colorscale: [
        [0, '#2196F3'],    // sockets faster (blue)
        [0.5, '#ffffff'],  // crossover (white)
        [1, '#e94560'],    // verbs faster (red)
      ],
      zmid: 1,
      colorbar: {{
        title: {{ text: 'Speedup (verbs/sockets)', side: 'right' }},
        tickfont: {{ color: '#e0e0e0' }},
        titlefont: {{ color: '#e0e0e0' }},
      }},
      hovertemplate: 'Size: %{{x}}<br>Reps: %{{y}}<br>Speedup: %{{z:.2f}}x<extra></extra>',
    }}, {{
      // Contour at speedup = 1 (crossover boundary)
      z: z,
      x: D.size_labels,
      y: D.rep_labels,
      type: 'contour',
      contours: {{ start: 1, end: 1, size: 0, coloring: 'none' }},
      line: {{ color: '#00ff00', width: 3 }},
      showscale: false,
      hoverinfo: 'skip',
    }}], {{
      ...darkLayout,
      title: 'Verbs vs Sockets Speedup (recv mode)',
      xaxis: {{ title: 'Message Size', color: '#e0e0e0' }},
      yaxis: {{ title: 'Repetitions', color: '#e0e0e0' }},
    }});

    // --- 3D Surface ---
    Plotly.newPlot('plot-surface', [{{
      z: z,
      x: D.size_labels,
      y: D.rep_labels,
      type: 'surface',
      colorscale: [
        [0, '#2196F3'],
        [0.5, '#ffffff'],
        [1, '#e94560'],
      ],
      cmid: 1,
      hovertemplate: 'Size: %{{x}}<br>Reps: %{{y}}<br>Speedup: %{{z:.2f}}x<extra></extra>',
    }}], {{
      ...darkLayout,
      title: 'Verbs/Sockets Speedup Surface (recv mode)',
      scene: {{
        xaxis: {{ title: 'Message Size', color: '#e0e0e0' }},
        yaxis: {{ title: 'Repetitions', color: '#e0e0e0' }},
        zaxis: {{ title: 'Speedup', color: '#e0e0e0' }},
        bgcolor: '#1a1a2e',
      }},
    }});
  }}
}}

// --- Latency comparison ---
(function() {{
  const traces = [];
  const colors = {{ 'ofi+sockets': '#2196F3', 'ofi_sockets': '#2196F3',
                     'ofi+verbs': '#e94560', 'ofi_verbs': '#e94560' }};
  const dashes = {{ 'recv': 'solid', 'rdma': 'dash', 'sendrecv': 'dot' }};

  // Use first rep count (or only one if 1D)
  const repIdx = 0;

  for (const proto of D.protocols) {{
    const pd = D.plot_data[proto];
    if (!pd) continue;
    for (const mode of Object.keys(pd)) {{
      const lats = pd[mode].latencies[repIdx];
      if (!lats) continue;
      const color = colors[proto] || '#888';
      traces.push({{
        x: D.size_labels,
        y: lats,
        name: proto + ' ' + mode,
        type: 'scatter',
        mode: 'lines+markers',
        line: {{ color: color, dash: dashes[mode] || 'solid', width: 2 }},
        marker: {{ size: 5 }},
        hovertemplate: '%{{x}}: %{{y:.1f}} us<extra>' + proto + ' ' + mode + '</extra>',
      }});
    }}
  }}

  Plotly.newPlot('plot-latency', traces, {{
    ...darkLayout,
    title: 'Average Latency per RPC' + (D.has_reps ? ' (reps=' + D.rep_counts[0] + ')' : ''),
    xaxis: {{ title: 'Message Size', color: '#e0e0e0' }},
    yaxis: {{ title: 'Latency (us)', type: 'log', color: '#e0e0e0' }},
    legend: {{ font: {{ color: '#e0e0e0' }} }},
  }});
}})();

// --- Bandwidth comparison ---
(function() {{
  const traces = [];
  const colors = {{ 'ofi+sockets': '#2196F3', 'ofi_sockets': '#2196F3',
                     'ofi+verbs': '#e94560', 'ofi_verbs': '#e94560' }};
  const dashes = {{ 'recv': 'solid', 'rdma': 'dash', 'sendrecv': 'dot' }};

  const repIdx = 0;

  for (const proto of D.protocols) {{
    const pd = D.plot_data[proto];
    if (!pd) continue;
    for (const mode of Object.keys(pd)) {{
      const bws = pd[mode].bandwidths[repIdx];
      if (!bws) continue;
      const color = colors[proto] || '#888';
      traces.push({{
        x: D.size_labels,
        y: bws,
        name: proto + ' ' + mode,
        type: 'scatter',
        mode: 'lines+markers',
        line: {{ color: color, dash: dashes[mode] || 'solid', width: 2 }},
        marker: {{ size: 5 }},
        hovertemplate: '%{{x}}: %{{y:.1f}} MB/s<extra>' + proto + ' ' + mode + '</extra>',
      }});
    }}
  }}

  Plotly.newPlot('plot-bandwidth', traces, {{
    ...darkLayout,
    title: 'Bandwidth' + (D.has_reps ? ' (reps=' + D.rep_counts[0] + ')' : ''),
    xaxis: {{ title: 'Message Size', color: '#e0e0e0' }},
    yaxis: {{ title: 'Bandwidth (MB/s)', type: 'log', color: '#e0e0e0' }},
    legend: {{ font: {{ color: '#e0e0e0' }} }},
  }});
}})();

// --- 1D Speedup (when only one rep count) ---
if (D.has_both && !D.has_reps) {{
  const traces = [];
  for (const mode of Object.keys(D.speedup_data)) {{
    const row = D.speedup_data[mode][0];
    traces.push({{
      x: D.size_labels,
      y: row,
      name: mode,
      type: 'scatter',
      mode: 'lines+markers',
      marker: {{ size: 6 }},
      hovertemplate: '%{{x}}: %{{y:.2f}}x<extra>' + mode + '</extra>',
    }});
  }}
  // Crossover line at y=1
  traces.push({{
    x: D.size_labels,
    y: Array(D.size_labels.length).fill(1),
    name: 'break-even',
    type: 'scatter',
    mode: 'lines',
    line: {{ color: '#00ff00', dash: 'dash', width: 1 }},
    hoverinfo: 'skip',
  }});

  Plotly.newPlot('plot-speedup1d', traces, {{
    ...darkLayout,
    title: 'Verbs vs Sockets Speedup (>1 = verbs faster)',
    xaxis: {{ title: 'Message Size', color: '#e0e0e0' }},
    yaxis: {{ title: 'Speedup Factor', color: '#e0e0e0' }},
    legend: {{ font: {{ color: '#e0e0e0' }} }},
  }});
}}

document.getElementById('note').textContent =
  D.has_both
    ? 'Blue = sockets faster, Red = verbs faster. Green contour = crossover boundary (speedup = 1x).'
    : 'Run with -p both to see the crossover surface between ofi+sockets and ofi+verbs.';
</script>
</body>
</html>"""

    with open(output_path, 'w') as f:
        f.write(html)
    print(f"Interactive HTML written to: {output_path}")

def main():
    parser = argparse.ArgumentParser(
        description='Generate interactive crossover analysis HTML from benchmark CSVs')
    parser.add_argument('csv', nargs='?', help='Combined CSV file (with protocol column)')
    parser.add_argument('-s', '--sockets', help='CSV file for ofi+sockets results')
    parser.add_argument('-v', '--verbs', help='CSV file for ofi+verbs results')
    parser.add_argument('-o', '--output', default='crossover_analysis.html',
                        help='Output HTML file (default: crossover_analysis.html)')
    args = parser.parse_args()

    if not args.csv and not args.sockets and not args.verbs:
        parser.error('Provide a combined CSV, or -s/-v for separate per-protocol CSVs')

    all_rows = []

    if args.csv:
        rows = parse_csv(args.csv)
        # If no protocol column, try to infer from filename
        if rows and 'protocol' not in rows[0]:
            proto = 'unknown'
            fname = os.path.basename(args.csv).lower()
            if 'socket' in fname:
                proto = 'ofi+sockets'
            elif 'verbs' in fname:
                proto = 'ofi+verbs'
            for r in rows:
                r['protocol'] = proto
        all_rows.extend(rows)

    if args.sockets:
        rows = parse_csv(args.sockets)
        for r in rows:
            r['protocol'] = 'ofi+sockets'
        all_rows.extend(rows)

    if args.verbs:
        rows = parse_csv(args.verbs)
        for r in rows:
            r['protocol'] = 'ofi+verbs'
        all_rows.extend(rows)

    if not all_rows:
        print("Error: no data found in CSV files", file=sys.stderr)
        sys.exit(1)

    data = build_data_matrix(all_rows)
    generate_html(data, args.output)

if __name__ == '__main__':
    main()
