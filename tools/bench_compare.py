#!/usr/bin/env python3
import statistics
import subprocess
import sys
import time
from pathlib import Path


def read_workloads(path: Path):
    workloads = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        workloads.append(line)
    return workloads


def run_one(binary: str, script_path: str) -> float:
    start = time.perf_counter()
    proc = subprocess.run([binary, script_path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    elapsed = time.perf_counter() - start
    if proc.returncode != 0:
        raise RuntimeError(f"{binary} failed on {script_path} with exit code {proc.returncode}")
    return elapsed


def run_many(binary: str, script_path: str, iters: int):
    samples = []
    for _ in range(iters):
        samples.append(run_one(binary, script_path))
    return statistics.median(samples), samples


def main():
    if len(sys.argv) != 5:
        print("Usage: bench_compare.py <baseline_bin> <candidate_bin> <workloads_file> <iters>")
        return 2

    baseline = sys.argv[1]
    candidate = sys.argv[2]
    workloads_file = Path(sys.argv[3])
    iters = int(sys.argv[4])

    if not Path(baseline).exists():
        print(f"Baseline binary not found: {baseline}")
        print("Tip: run `make save-baseline` first.")
        return 2
    if not Path(candidate).exists():
        print(f"Candidate binary not found: {candidate}")
        return 2
    if not workloads_file.exists():
        print(f"Workloads file not found: {workloads_file}")
        return 2

    workloads = read_workloads(workloads_file)
    if not workloads:
        print("No workloads found.")
        return 2

    base_total = 0.0
    cand_total = 0.0
    failures = 0

    print(f"Baseline : {baseline}")
    print(f"Candidate: {candidate}")
    print(f"Iterations per workload: {iters}")
    print()
    print(f"{'Workload':45} {'Baseline(s)':>12} {'Candidate(s)':>12} {'Speedup':>10}")
    print("-" * 84)

    for workload in workloads:
        if not Path(workload).exists():
            print(f"Missing workload file: {workload}")
            return 2

        try:
            base_med, _ = run_many(baseline, workload, iters)
            cand_med, _ = run_many(candidate, workload, iters)
        except RuntimeError as err:
            failures += 1
            print(f"{workload[:45]:45} {'FAILED':>12} {'FAILED':>12} {'-':>10}")
            print(f"  -> {err}")
            continue

        base_total += base_med
        cand_total += cand_med

        speedup = base_med / cand_med if cand_med > 0 else float("inf")
        print(f"{workload[:45]:45} {base_med:12.6f} {cand_med:12.6f} {speedup:10.3f}x")

    print("-" * 84)
    total_speedup = base_total / cand_total if cand_total > 0 else float("inf")
    print(f"{'TOTAL(median-sum)':45} {base_total:12.6f} {cand_total:12.6f} {total_speedup:10.3f}x")

    if failures:
        print(f"\nCompleted with {failures} workload failure(s).")
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
