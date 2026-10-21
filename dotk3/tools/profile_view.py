#!/usr/bin/env python3
"""
profile_view.py — Visualize dotk3 profiler report as a self-contained HTML page.

Usage:
    dotk3 --profile-vm verbose myprogram.k3 2>&1 >/dev/null | python3 tools/profile_view.py
    dotk3 --profile-vm verbose myprogram.k3 2>report.txt && python3 tools/profile_view.py report.txt
    dotk3 --profile-vm verbose myprogram.k3 2>report.txt && python3 tools/profile_view.py report.txt -o profile.html

The generated HTML file is self-contained (no network required) and opens automatically.
"""

import re
import sys
import json
import argparse
import subprocess
import tempfile
import os

# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

def parse_report(text: str) -> dict:
    data: dict = {}

    # --- source CPU profile ---
    m = re.search(r"Samples: (\d+), attributed CPU: ([\d.]+) ms", text)
    if m:
        data["samples"] = int(m.group(1))
        data["attributedCpuMs"] = float(m.group(2))

    cpu_table = re.search(r"Self ms\s+Inclusive ms\s+Function / source\n(.*?)(?:\n\n|\nDotK VM|\Z)", text, re.DOTALL)
    if cpu_table:
        rows = []
        for line in cpu_table.group(1).strip().splitlines():
            parts = line.split()
            if len(parts) >= 3:
                try:
                    rows.append({"self": float(parts[0]), "inclusive": float(parts[1]), "label": " ".join(parts[2:])})
                except ValueError:
                    pass
        data["cpuProfile"] = rows

    # --- VM profile ---
    m = re.search(r"Wall: ([\d.]+) ms; CPU: ([\d.]+) ms", text)
    if m:
        data["wallMs"] = float(m.group(1))
        data["cpuMs"] = float(m.group(2))

    m = re.search(r"Execution: (\d+) slot-bytecode calls, (\d+) lexical-bytecode calls", text)
    if m:
        data["slotCalls"] = int(m.group(1))
        data["lexicalCalls"] = int(m.group(2))

    m = re.search(r"Allocations: (\d+) objects; estimated base/payload bytes: (\d+); peak live objects: (\d+); live: (\d+)", text)
    if m:
        data["allocObjects"] = int(m.group(1))
        data["allocBytes"] = int(m.group(2))
        data["peakLive"] = int(m.group(3))
        data["liveObjects"] = int(m.group(4))

    m = re.search(r"GC: (\d+) collections; (\d+) reclaimed; total ([\d.]+) ms; pauses p50/p95/max ([\d.]+)/([\d.]+)/([\d.]+) ms", text)
    if m:
        data["gcCollections"] = int(m.group(1))
        data["gcReclaimed"] = int(m.group(2))
        data["gcTotalMs"] = float(m.group(3))
        data["gcP50"] = float(m.group(4))
        data["gcP95"] = float(m.group(5))
        data["gcMax"] = float(m.group(6))

    m = re.search(r"Frames peak: (\d+); operand values peak: (\d+)", text)
    if m:
        data["framesPeak"] = int(m.group(1))
        data["operandPeak"] = int(m.group(2))

    # phase timings
    phases = {}
    for name in ("Parse", "Compile", "Initialize", "Execute", "GC"):
        m = re.search(rf"{name}: ([\d.]+) ms wall, ([\d.]+) ms CPU \((\d+)\)", text)
        if m:
            phases[name] = {"wall": float(m.group(1)), "cpu": float(m.group(2)), "calls": int(m.group(3))}
    if phases:
        data["phases"] = phases

    # --- arithmetic paths (verbose) ---
    m = re.search(r"Arithmetic paths: (\d+) checked integer specializations, (\d+) float specializations, (\d+) generic operations", text)
    if m:
        data["intSpecializations"] = int(m.group(1))
        data["floatSpecializations"] = int(m.group(2))
        data["genericOps"] = int(m.group(3))

    # binding cache (verbose)
    m = re.search(r"Binding cache: (\d+) hits, (\d+) misses, (\d+) invalidations", text)
    if m:
        data["cacheHits"] = int(m.group(1))
        data["cacheMisses"] = int(m.group(2))
        data["cacheInvalidations"] = int(m.group(3))

    # --- opcodes (verbose) ---
    opcodes_section = re.search(r"^Opcodes:\n(.*?)(?:\nAllocation kinds:|\nFunction/native|\nProfiler bounds|\Z)", text, re.MULTILINE | re.DOTALL)
    if opcodes_section:
        opcodes = {}
        for line in opcodes_section.group(1).strip().splitlines():
            m = re.match(r"\s+(\w+): (\d+)", line)
            if m:
                opcodes[m.group(1)] = int(m.group(2))
        if opcodes:
            data["opcodes"] = opcodes

    # --- allocation kinds (verbose) ---
    alloc_section = re.search(r"^Allocation kinds:\n(.*?)(?:\nFunction/native|\nProfiler bounds|\Z)", text, re.MULTILINE | re.DOTALL)
    if alloc_section:
        kinds = {}
        for line in alloc_section.group(1).strip().splitlines():
            m = re.match(r"\s+(\w+): (\d+) objects, estimated (\d+) bytes", line)
            if m:
                kinds[m.group(1)] = {"objects": int(m.group(2)), "bytes": int(m.group(3))}
        if kinds:
            data["allocationKinds"] = kinds

    # --- function activations ---
    act_section = re.search(r"Function/native activations.*?:\n(.*?)(?:\nNative calls|\nAllocation sites|\nProfiler bounds|\Z)", text, re.DOTALL)
    if act_section:
        activations = {}
        for line in act_section.group(1).strip().splitlines():
            m = re.match(r"\s+(.+): (\d+)", line)
            if m:
                activations[m.group(1).strip()] = int(m.group(2))
        if activations:
            data["activations"] = activations

    # --- native calls ---
    native_section = re.search(r"Native calls.*?:\n(.*?)(?:\nAllocation sites|\nProfiler bounds|\Z)", text, re.DOTALL)
    if native_section:
        natives = {}
        for line in native_section.group(1).strip().splitlines():
            m = re.match(r"\s+(.+): (\d+); ([\d.]+) ms wall, ([\d.]+) ms CPU", line)
            if m:
                natives[m.group(1).strip()] = {"calls": int(m.group(2)), "wall": float(m.group(3)), "cpu": float(m.group(4))}
        if natives:
            data["nativeCalls"] = natives

    # --- allocation sites ---
    sites_section = re.search(r"Allocation sites.*?:\n(.*?)(?:\nProfiler bounds|\Z)", text, re.DOTALL)
    if sites_section:
        sites = {}
        for line in sites_section.group(1).strip().splitlines():
            m = re.match(r"\s+(.+): (\d+) objects", line)
            if m:
                sites[m.group(1).strip()] = int(m.group(2))
        if sites:
            data["allocationSites"] = sites

    # --- dropped ---
    m = re.search(r"dropped samples/frames/events/sites (\d+)/(\d+)/(\d+)/(\d+)", text)
    if m:
        data["dropped"] = {"samples": int(m.group(1)), "frames": int(m.group(2)), "events": int(m.group(3)), "sites": int(m.group(4))}

    return data


# ---------------------------------------------------------------------------
# HTML generation
# ---------------------------------------------------------------------------

HTML_TEMPLATE = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>dotk3 Profile Report</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4/dist/chart.umd.min.js"></script>
<style>
  :root { --bg: #0f1117; --panel: #1a1d27; --border: #2a2d3e; --text: #e2e8f0; --muted: #94a3b8; --accent: #6366f1; --green: #22c55e; --red: #ef4444; --yellow: #f59e0b; }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: var(--bg); color: var(--text); font-family: 'Segoe UI', system-ui, sans-serif; font-size: 14px; padding: 24px; }
  h1 { font-size: 22px; font-weight: 700; margin-bottom: 8px; color: #fff; }
  h2 { font-size: 15px; font-weight: 600; margin-bottom: 12px; color: var(--accent); text-transform: uppercase; letter-spacing: .05em; }
  .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(320px, 1fr)); gap: 16px; margin-bottom: 16px; }
  .grid-wide { grid-template-columns: repeat(auto-fill, minmax(480px, 1fr)); }
  .card { background: var(--panel); border: 1px solid var(--border); border-radius: 10px; padding: 18px; }
  .stat-row { display: flex; justify-content: space-between; align-items: center; padding: 5px 0; border-bottom: 1px solid var(--border); }
  .stat-row:last-child { border-bottom: none; }
  .stat-label { color: var(--muted); }
  .stat-value { font-weight: 600; font-variant-numeric: tabular-nums; }
  .warn { color: var(--yellow); }
  canvas { max-height: 220px; }
  table { width: 100%; border-collapse: collapse; font-size: 13px; }
  th { text-align: left; color: var(--muted); font-weight: 500; padding: 4px 8px; border-bottom: 1px solid var(--border); }
  td { padding: 4px 8px; border-bottom: 1px solid var(--border); font-variant-numeric: tabular-nums; }
  tr:last-child td { border-bottom: none; }
  .pill { display: inline-block; padding: 2px 8px; border-radius: 99px; font-size: 11px; font-weight: 600; }
  .pill-green { background: #14532d; color: var(--green); }
  .pill-red { background: #450a0a; color: var(--red); }
  .subtitle { color: var(--muted); font-size: 12px; margin-bottom: 16px; }
  .dropped-warn { background: #422006; border: 1px solid #92400e; border-radius: 8px; padding: 10px 14px; margin-bottom: 16px; color: var(--yellow); font-size: 13px; }
</style>
</head>
<body>
<h1>dotk3 Profile Report</h1>
<p class="subtitle" id="subtitle"></p>
<div id="dropped-warn"></div>
<div class="grid" id="overview"></div>
<div class="grid grid-wide" id="charts"></div>
<div class="grid grid-wide" id="tables"></div>
<script>
const DATA = __DATA__;

function fmt(n) { return n == null ? '—' : n.toLocaleString(undefined, {maximumFractionDigits: 3}); }
function fmtMs(n) { return n == null ? '—' : n.toFixed(3) + ' ms'; }
function fmtBytes(n) { if (n == null) return '—'; if (n < 1024) return n + ' B'; if (n < 1048576) return (n/1024).toFixed(1) + ' KB'; return (n/1048576).toFixed(1) + ' MB'; }

// subtitle
document.getElementById('subtitle').textContent =
    (DATA.wallMs != null ? `Wall: ${fmtMs(DATA.wallMs)}  CPU: ${fmtMs(DATA.cpuMs)}` : '') +
    (DATA.samples != null ? `  ·  ${DATA.samples} CPU samples (${fmtMs(DATA.attributedCpuMs)} attributed)` : '');

// dropped warning
if (DATA.dropped) {
    const d = DATA.dropped;
    const total = d.samples + d.frames + d.events + d.sites;
    if (total > 0)
        document.getElementById('dropped-warn').innerHTML =
            `<div class="dropped-warn">⚠ Profiler dropped data: ${d.samples} samples / ${d.frames} frames / ${d.events} events / ${d.sites} sites — workload exceeded profiler bounds.</div>`;
}

// ---- overview cards ----
function card(title, rows) {
    const el = document.createElement('div');
    el.className = 'card';
    el.innerHTML = `<h2>${title}</h2>` + rows.map(([l,v]) =>
        `<div class="stat-row"><span class="stat-label">${l}</span><span class="stat-value">${v}</span></div>`).join('');
    return el;
}
const ov = document.getElementById('overview');
if (DATA.phases) {
    const p = DATA.phases;
    ov.appendChild(card('Phase Timings', Object.entries(p).map(([k,v]) => [k, fmtMs(v.wall) + ' wall / ' + fmtMs(v.cpu) + ' CPU'])));
}
if (DATA.allocObjects != null) {
    ov.appendChild(card('Memory', [
        ['Allocations', fmt(DATA.allocObjects) + ' objects'],
        ['Estimated bytes', fmtBytes(DATA.allocBytes)],
        ['Peak live', fmt(DATA.peakLive)],
        ['Live at exit', fmt(DATA.liveObjects)],
    ]));
}
if (DATA.gcCollections != null) {
    ov.appendChild(card('GC', [
        ['Collections', fmt(DATA.gcCollections)],
        ['Reclaimed', fmt(DATA.gcReclaimed)],
        ['Total pause', fmtMs(DATA.gcTotalMs)],
        ['Pauses p50/p95/max', fmtMs(DATA.gcP50) + ' / ' + fmtMs(DATA.gcP95) + ' / ' + fmtMs(DATA.gcMax)],
    ]));
}
if (DATA.slotCalls != null) {
    ov.appendChild(card('Execution', [
        ['Slot-bytecode calls', fmt(DATA.slotCalls)],
        ['Lexical-bytecode calls', fmt(DATA.lexicalCalls)],
        ['Frames peak', fmt(DATA.framesPeak)],
        ['Operand stack peak', fmt(DATA.operandPeak)],
    ]));
}
if (DATA.cacheHits != null) {
    const total = DATA.cacheHits + DATA.cacheMisses;
    const rate = total > 0 ? ((DATA.cacheHits / total) * 100).toFixed(1) + '%' : '—';
    ov.appendChild(card('Binding Cache', [
        ['Hits', fmt(DATA.cacheHits)],
        ['Misses', fmt(DATA.cacheMisses)],
        ['Invalidations', fmt(DATA.cacheInvalidations)],
        ['Hit rate', rate],
    ]));
}
if (DATA.intSpecializations != null) {
    const total = DATA.intSpecializations + DATA.floatSpecializations + DATA.genericOps;
    ov.appendChild(card('Arithmetic Paths', [
        ['Int specializations', fmt(DATA.intSpecializations)],
        ['Float specializations', fmt(DATA.floatSpecializations)],
        ['Generic operations', fmt(DATA.genericOps)],
        ['Specialization rate', total > 0 ? (((total - DATA.genericOps) / total) * 100).toFixed(1) + '%' : '—'],
    ]));
}

// ---- chart helpers ----
const COLORS = ['#6366f1','#22c55e','#f59e0b','#ef4444','#06b6d4','#a855f7','#ec4899','#84cc16','#f97316','#14b8a6'];
function makeChart(id, type, labels, datasets, opts = {}) {
    const canvas = document.getElementById(id);
    if (!canvas) return;
    new Chart(canvas, { type, data: { labels, datasets }, options: { responsive: true, plugins: { legend: { labels: { color: '#94a3b8' } } }, scales: type !== 'doughnut' ? { x: { ticks: { color: '#94a3b8' }, grid: { color: '#1e2130' } }, y: { ticks: { color: '#94a3b8' }, grid: { color: '#1e2130' } } } : undefined, ...opts } });
}
function chartCard(title, canvasId) {
    const el = document.createElement('div');
    el.className = 'card';
    el.innerHTML = `<h2>${title}</h2><canvas id="${canvasId}"></canvas>`;
    return el;
}

const charts = document.getElementById('charts');

// CPU profile chart (top 12 by self)
if (DATA.cpuProfile && DATA.cpuProfile.length > 0) {
    const top = DATA.cpuProfile.slice(0, 12);
    const card = chartCard('CPU Profile — Top Functions (self ms)', 'chartCpu');
    charts.appendChild(card);
    setTimeout(() => makeChart('chartCpu', 'bar',
        top.map(r => r.label.split('  ')[0].slice(0, 40)),
        [{ label: 'Self ms', data: top.map(r => r.self), backgroundColor: '#6366f1' },
         { label: 'Inclusive ms', data: top.map(r => r.inclusive), backgroundColor: '#22c55e55' }],
        { indexAxis: 'y' }
    ));
}

// Opcodes chart (top 15)
if (DATA.opcodes) {
    const sorted = Object.entries(DATA.opcodes).sort((a,b) => b[1]-a[1]).slice(0, 15);
    const card = chartCard('Opcode Counts (top 15)', 'chartOps');
    charts.appendChild(card);
    setTimeout(() => makeChart('chartOps', 'bar',
        sorted.map(([k]) => k),
        [{ label: 'Count', data: sorted.map(([,v]) => v), backgroundColor: COLORS }],
        { indexAxis: 'y' }
    ));
}

// Allocation kinds doughnut
if (DATA.allocationKinds) {
    const sorted = Object.entries(DATA.allocationKinds).sort((a,b) => b[1].objects - a[1].objects);
    const card = chartCard('Allocation Kinds (objects)', 'chartAlloc');
    charts.appendChild(card);
    setTimeout(() => makeChart('chartAlloc', 'doughnut',
        sorted.map(([k]) => k),
        [{ data: sorted.map(([,v]) => v.objects), backgroundColor: COLORS }]
    ));
}

// Phase wall times
if (DATA.phases) {
    const entries = Object.entries(DATA.phases);
    const card = chartCard('Phase Wall Times', 'chartPhases');
    charts.appendChild(card);
    setTimeout(() => makeChart('chartPhases', 'bar',
        entries.map(([k]) => k),
        [{ label: 'Wall ms', data: entries.map(([,v]) => v.wall), backgroundColor: '#6366f1' },
         { label: 'CPU ms', data: entries.map(([,v]) => v.cpu), backgroundColor: '#22c55e' }]
    ));
}

// ---- tables ----
const tables = document.getElementById('tables');
function tableCard(title, headers, rows) {
    if (!rows || !rows.length) return;
    const el = document.createElement('div');
    el.className = 'card';
    const ths = headers.map(h => `<th>${h}</th>`).join('');
    const trs = rows.map(r => `<tr>${r.map(c => `<td>${c}</td>`).join('')}</tr>`).join('');
    el.innerHTML = `<h2>${title}</h2><table><thead><tr>${ths}</tr></thead><tbody>${trs}</tbody></table>`;
    tables.appendChild(el);
}

if (DATA.activations) {
    const rows = Object.entries(DATA.activations).sort((a,b) => b[1]-a[1]).slice(0, 20);
    tableCard('Top Function Activations', ['Function', 'Calls'], rows.map(([k,v]) => [k, fmt(v)]));
}
if (DATA.nativeCalls) {
    const rows = Object.entries(DATA.nativeCalls).sort((a,b) => b[1].wall-a[1].wall);
    tableCard('Native Calls', ['Native', 'Calls', 'Wall', 'CPU'], rows.map(([k,v]) => [k, fmt(v.calls), fmtMs(v.wall), fmtMs(v.cpu)]));
}
if (DATA.allocationSites) {
    const rows = Object.entries(DATA.allocationSites).sort((a,b) => b[1]-a[1]).slice(0, 20);
    tableCard('Top Allocation Sites', ['Site', 'Objects'], rows.map(([k,v]) => [k, fmt(v)]));
}
if (DATA.allocationKinds) {
    const rows = Object.entries(DATA.allocationKinds).sort((a,b) => b[1].objects-a[1].objects);
    tableCard('Allocation Kinds', ['Kind', 'Objects', 'Est. Bytes'], rows.map(([k,v]) => [k, fmt(v.objects), fmtBytes(v.bytes)]));
}
</script>
</body>
</html>
"""


def build_html(data: dict) -> str:
    return HTML_TEMPLATE.replace("__DATA__", json.dumps(data, indent=2))


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", nargs="?", help="Path to profiler report file (default: stdin)")
    parser.add_argument("-o", "--output", help="Write HTML to this file instead of a temp file")
    parser.add_argument("--no-open", action="store_true", help="Don't open the browser automatically")
    parser.add_argument("--json", action="store_true", help="Print parsed JSON instead of generating HTML")
    args = parser.parse_args()

    if args.input:
        with open(args.input, encoding="utf-8", errors="replace") as f:
            text = f.read()
    else:
        if sys.stdin.isatty():
            print("Reading profiler report from stdin... (pipe from dotk3 stderr or pass a file)", file=sys.stderr)
        text = sys.stdin.read()

    data = parse_report(text)

    if not data:
        print("Error: no profiler data found in input. Run dotk3 with --profile or --profile-vm.", file=sys.stderr)
        sys.exit(1)

    if args.json:
        print(json.dumps(data, indent=2))
        return

    html = build_html(data)

    if args.output:
        out_path = args.output
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(html)
        print(f"Written to {out_path}", file=sys.stderr)
    else:
        fd, out_path = tempfile.mkstemp(suffix=".html", prefix="dotk3_profile_")
        os.close(fd)
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(html)
        print(f"Written to {out_path}", file=sys.stderr)

    if not args.no_open:
        url = f"file://{os.path.abspath(out_path)}"
        # Use xdg-open on Linux; falls back to open on macOS. Detach so we don't block.
        opener = "xdg-open" if sys.platform.startswith("linux") else "open"
        try:
            subprocess.Popen([opener, url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except FileNotFoundError:
            print(f"Open {url} in your browser", file=sys.stderr)


if __name__ == "__main__":
    main()
