#!/usr/bin/env python3
import argparse
import re
import statistics
import subprocess
import tempfile
import time
from pathlib import Path


SCRIPT_TEMPLATE = """
from modules.threading import poolStats, setPoolQueueLimit;

async fn work(n) {
    var x = n + 1
    for (var i = 0; i < __INNER_LOOPS__; i = i + 1) {
        x = (x * 1103515245 + 12345) % 2147483647
    }
    return x % 1000
}

setPoolQueueLimit(__QUEUE_LIMIT__)
var before = poolStats()

var tasks = []
for (var i = 0; i < __TASK_COUNT__; i = i + 1) {
    tasks.append(work(i))
}

var sum = 0
for (var i = 0; i < __TASK_COUNT__; i = i + 1) {
    sum = sum + await tasks[i]
}

var after = poolStats()

print("before_submitted=" + str(before.submitted))
print("before_completed=" + str(before.completed))
print("after_submitted=" + str(after.submitted))
print("after_completed=" + str(after.completed))
print("after_activeWorkers=" + str(after.activeWorkers))
print("checksum=" + str(sum))
""".strip()


def run_once(binary: str, script_path: Path):
    start = time.perf_counter()
    proc = subprocess.run([binary, str(script_path)], capture_output=True, text=True)
    elapsed = time.perf_counter() - start
    if proc.returncode != 0:
        raise RuntimeError(f"{binary} failed with code {proc.returncode}: {proc.stderr.strip()}")
    return elapsed, proc.stdout


def parse_metric(stdout: str, key: str):
    m = re.search(rf"^{re.escape(key)}=(.+)$", stdout, flags=re.MULTILINE)
    return m.group(1).strip() if m else ""


def benchmark_limit(binary: str, queue_limit: int, iterations: int, task_count: int, inner_loops: int):
    samples = []
    last_stdout = ""
    with tempfile.TemporaryDirectory(prefix="dotk-pool-bench-") as td:
        script_path = Path(td) / f"pool_{queue_limit}.k"
        script = (
            SCRIPT_TEMPLATE.replace("__QUEUE_LIMIT__", str(queue_limit))
            .replace("__TASK_COUNT__", str(task_count))
            .replace("__INNER_LOOPS__", str(inner_loops))
        )
        script_path.write_text(
            script,
            encoding="utf-8",
        )

        for _ in range(iterations):
            elapsed, stdout = run_once(binary, script_path)
            samples.append(elapsed)
            last_stdout = stdout

    return {
        "queue_limit": queue_limit,
        "median_s": statistics.median(samples),
        "samples": samples,
        "before_submitted": parse_metric(last_stdout, "before_submitted"),
        "before_completed": parse_metric(last_stdout, "before_completed"),
        "after_submitted": parse_metric(last_stdout, "after_submitted"),
        "after_completed": parse_metric(last_stdout, "after_completed"),
        "after_activeWorkers": parse_metric(last_stdout, "after_activeWorkers"),
        "checksum": parse_metric(last_stdout, "checksum"),
    }


def main():
    parser = argparse.ArgumentParser(description="Tune DotK async worker-pool queue limit.")
    parser.add_argument("binary", help="Path to dotk binary, e.g. ./dotk.out")
    parser.add_argument("--limits", default="8,16,32,64,128", help="Comma-separated queue limits")
    parser.add_argument("--iters", type=int, default=5, help="Iterations per queue limit")
    parser.add_argument("--tasks", type=int, default=500, help="Async tasks per run")
    parser.add_argument("--loops", type=int, default=3000, help="Inner compute loop per task")
    args = parser.parse_args()

    limits = [int(x.strip()) for x in args.limits.split(",") if x.strip()]
    if not limits:
        raise SystemExit("No queue limits provided")

    binary_path = Path(args.binary)
    if not binary_path.exists():
        raise SystemExit(f"Binary not found: {args.binary}")
    binary = args.binary
    if "/" not in binary and not binary.startswith("."):
        binary = f"./{binary}"

    results = []
    for q in limits:
        results.append(benchmark_limit(binary, q, args.iters, args.tasks, args.loops))

    print(f"{'Queue':>8} {'Median(s)':>12} {'Checksum':>12} {'afterCompleted':>15} {'afterActive':>12}")
    print("-" * 68)
    for row in results:
        print(
            f"{row['queue_limit']:8d} {row['median_s']:12.6f} {row['checksum']:>12} "
            f"{row['after_completed']:>15} {row['after_activeWorkers']:>12}"
        )

    fastest = min(results, key=lambda r: r["median_s"])
    print("-" * 68)
    print(
        f"Best queue_limit={fastest['queue_limit']} median={fastest['median_s']:.6f}s "
        f"checksum={fastest['checksum']}"
    )

    print("\nSnapshot (last run per limit):")
    for row in results:
        print(
            f"q={row['queue_limit']}: before(sub={row['before_submitted']}, comp={row['before_completed']}), "
            f"after(sub={row['after_submitted']}, comp={row['after_completed']}, active={row['after_activeWorkers']})"
        )


if __name__ == "__main__":
    main()