#!/usr/bin/env python3
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path


BENCH_RE = re.compile(r"BENCH\s+mode=(\S+)\s+frames=(\d+)\s+ms=([0-9.]+)\s+fps=([0-9.]+)")


def run_once(binary: str, script: str, native: bool, obj_path: str, workload_mode: str, frames: int, warmup: int, timeout_s: int):
    cmd = [binary]
    if native:
        cmd.append("--native")
    cmd.extend([script, obj_path, workload_mode, str(frames), str(warmup)])

    start = time.perf_counter()
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s)
    wall_ms = (time.perf_counter() - start) * 1000.0

    out = (proc.stdout or "") + "\n" + (proc.stderr or "")
    match = BENCH_RE.search(out)
    if proc.returncode != 0:
        raise RuntimeError(f"exit={proc.returncode} cmd={' '.join(cmd)}\n{out.strip()}")
    if not match:
        raise RuntimeError(f"missing BENCH output cmd={' '.join(cmd)}\n{out.strip()}")

    return {
        "mode": match.group(1),
        "frames": int(match.group(2)),
        "script_ms": float(match.group(3)),
        "script_fps": float(match.group(4)),
        "wall_ms": wall_ms,
    }


def summarize(samples):
    return {
        "script_ms_med": statistics.median(s["script_ms"] for s in samples),
        "script_fps_med": statistics.median(s["script_fps"] for s in samples),
        "wall_ms_med": statistics.median(s["wall_ms"] for s in samples),
    }


def print_block(title: str, vm_stats: dict, native_stats: dict):
    speedup = vm_stats["script_ms_med"] / native_stats["script_ms_med"] if native_stats["script_ms_med"] > 0 else float("inf")
    fps_gain = native_stats["script_fps_med"] / vm_stats["script_fps_med"] if vm_stats["script_fps_med"] > 0 else float("inf")

    print(f"\n[{title}]")
    print(f"  VM      median script ms : {vm_stats['script_ms_med']:.3f}")
    print(f"  Native  median script ms : {native_stats['script_ms_med']:.3f}")
    print(f"  Speedup (VM/Native)      : {speedup:.3f}x")
    print(f"  VM      median FPS       : {vm_stats['script_fps_med']:.3f}")
    print(f"  Native  median FPS       : {native_stats['script_fps_med']:.3f}")
    print(f"  FPS ratio (Native/VM)    : {fps_gain:.3f}x")


def main():
    if len(sys.argv) not in (1, 2, 3, 4, 5, 6, 7):
        print("Usage: bench_x11_native_vs_vm.py [binary] [script] [obj] [runs] [frames] [warmup]")
        return 2

    binary = sys.argv[1] if len(sys.argv) > 1 else "./dotk.out"
    script = sys.argv[2] if len(sys.argv) > 2 else "benchmarks/x11_viewer_bench.k"
    obj_path = sys.argv[3] if len(sys.argv) > 3 else "models/cow.obj"
    runs = int(sys.argv[4]) if len(sys.argv) > 4 else 5
    frames = int(sys.argv[5]) if len(sys.argv) > 5 else 300
    warmup = int(sys.argv[6]) if len(sys.argv) > 6 else 60

    for path in (binary, script, obj_path):
        if not Path(path).exists():
            print(f"Missing path: {path}")
            return 2

    timeout_s = max(20, int((frames + warmup) / 20))

    print(f"Binary : {binary}")
    print(f"Script : {script}")
    print(f"OBJ    : {obj_path}")
    print(f"Runs   : {runs}")
    print(f"Frames : {frames} (+ warmup {warmup})")

    results = {
        "script": {"vm": [], "native": []},
        "native": {"vm": [], "native": []},
    }

    for workload_mode in ("script", "native"):
        for i in range(runs):
            results[workload_mode]["vm"].append(run_once(binary, script, False, obj_path, workload_mode, frames, warmup, timeout_s))
            results[workload_mode]["native"].append(run_once(binary, script, True, obj_path, workload_mode, frames, warmup, timeout_s))
            print(f"  completed run {i + 1}/{runs} for workload={workload_mode}")

    for workload_mode in ("script", "native"):
        vm_stats = summarize(results[workload_mode]["vm"])
        native_stats = summarize(results[workload_mode]["native"])
        print_block(workload_mode, vm_stats, native_stats)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
