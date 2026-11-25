"""Stage 5 B-side measurement: read-path benefit across the cache/workingset spectrum.

The A-side delivery compares two cache budgets that sit at opposite ends: 8 MiB
holds the whole 512-node graph, 128 KiB holds almost none of it. That leaves the
middle unmeasured, so the report can say a cache helps but not where the benefit
starts or how it degrades.

This script fixes one larger graph whose footprint is several times the smallest
budget and sweeps the cache across it, then sweeps the prefetch window on one
budget. It measures only; thresholds live in the report.

Every run is verified twice: `evaluation.check` for the EVAL-1.1 contract and
`verify-read-path.py` for the read-path counters that EVAL-1.1 does not cover.
"""

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
JOB_LIMIT_MIB = 384
KIB = 1024
MIB = 1024 * 1024


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def dump(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def launch(command, log):
    with log.open("wb") as output:
        result = subprocess.run(command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f"{command[0]} failed ({result.returncode}), see {log}")


def resize_cache(config, cache_bytes):
    """Move budget between the cache part and the reserve, keeping the total."""
    memory = config["engine"]["memory"]
    total = int(memory["total_bytes"])
    previous = int(memory["cache_bytes"])
    others = sum(int(memory[key]) for key in ("delta_bytes", "metadata_bytes",
                                             "workspace_bytes", "reserve_bytes"))
    available = total - others + int(memory["reserve_bytes"])
    if cache_bytes > available:
        raise SystemExit(f"cache budget {cache_bytes} exceeds what the partition allows "
                         f"({available}); raise memory.total_bytes")
    memory["reserve_bytes"] = str(int(memory["reserve_bytes"]) + previous - cache_bytes)
    memory["cache_bytes"] = str(cache_bytes)


def measure(run, cache_bytes, graph_bytes):
    result = load(run / "evaluation.json")
    if result["verdict"] != "pass":
        raise SystemExit(f"{run.name}: EVAL-1.1 verdict is {result['verdict']}")
    rows = load_rows(run / "queries.jsonl")
    if any(row["status"] != "ok" for row in rows):
        raise SystemExit(f"{run.name}: unsuccessful query")
    summary = load(run / "summary.json")
    counts = load(run / "counters.json")["engine_counters"]
    path = summary["read_path"]
    hits = int(path["cache_hits"])
    misses = int(path["cache_misses"])
    demands = hits + misses
    # A saturated run measures the machine, not the index. Refuse it rather than
    # publish a throughput or latency number that cannot be reproduced.
    config = load(run / "effective-config.json")
    target = float(config["workload"]["target_ops_per_second"])
    achieved = float(summary["window_completed_qps"])
    if target > 0 and achieved < 0.90 * target:
        raise SystemExit(f"{run.name}: saturated, {achieved:.2f} of {target:.2f} ops/s")
    return {
        "run": run.name,
        "cache_bytes": cache_bytes,
        "graph_bytes": graph_bytes,
        "cache_to_graph": cache_bytes / graph_bytes if graph_bytes else 0.0,
        "queries": len(rows),
        "read_calls": int(counts["io_reads_submitted"]),
        "completed_bytes": int(counts["io_read_bytes_completed"]),
        "cache_hits": hits,
        "cache_misses": misses,
        "logical_demands": demands,
        "hit_rate": hits / demands if demands else 0.0,
        "prefetch_submitted": int(counts["prefetch_submitted"]),
        "prefetch_consumed": int(counts["prefetch_consumed"]),
        "prefetch_ready_at_demand": int(path["prefetch_ready_at_demand"]),
        "prefetch_use_rate": (int(counts["prefetch_consumed"]) / int(counts["prefetch_submitted"])
                              if int(counts["prefetch_submitted"]) else None),
        "peak_inflight_reads": int(path["peak_inflight_reads"]),
        "peak_prefetch_reads": int(path["peak_prefetch_reads"]),
        "cache_resident_bytes": int(path["cache_resident_bytes"]),
        "window_completed_qps": achieved,
        "latency_p50_ms": percentile(rows, 0.50),
        "latency_p95_ms": percentile(rows, 0.95),
        "latency_p99_ms": percentile(rows, 0.99),
    }


def load_rows(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def percentile(rows, fraction):
    values = sorted((int(row["completed_ns"]) - int(row["scheduled_ns"])) / 1e6 for row in rows)
    index = max(0, min(len(values) - 1, int(-(-len(values) * fraction // 1)) - 1))
    return values[index]


def plot(output, sweep, prefetch):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    figure, axes = plt.subplots(1, 2, figsize=(11, 4), layout="constrained")

    axis = axes[0]
    for mode in sorted({row["mode"] for row in sweep}):
        points = sorted((row["cache_to_graph"], row["hit_rate"])
                        for row in sweep if row["mode"] == mode)
        axis.plot([p[0] for p in points], [p[1] for p in points], marker="o", label=mode)
    axis.set(xscale="log", xlabel="Cache budget / graph footprint",
             ylabel="Cache hit rate", title="Hit rate against cache size")
    axis.grid(alpha=0.25)
    axis.legend(loc="lower right")

    axis = axes[1]
    for mode in sorted({row["mode"] for row in prefetch}):
        points = sorted((row["prefetch_window"], row["latency_p95_ms"])
                        for row in prefetch if row["mode"] == mode)
        axis.plot([p[0] for p in points], [p[1] for p in points], marker="o", label=mode)
    axis.set(xlabel="Prefetch window (blocks per query)", ylabel="End-to-end P95 (ms)",
             title="P95 against prefetch window")
    axis.grid(alpha=0.25)
    axis.legend(loc="lower right")

    figure.savefig(output, dpi=150)
    plt.close(figure)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True,
                        help="New ASCII directory on the NTFS test volume")
    parser.add_argument("--binaries", type=Path, default=ROOT / "build/windows/Release")
    parser.add_argument("--vectors", type=int, default=4096)
    parser.add_argument("--dimension", type=int, default=16)
    parser.add_argument("--duration-ms", type=int, default=4000)
    parser.add_argument("--rate", type=int, default=10)
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--cache-kib", type=int, nargs="+",
                        default=[256, 512, 1024, 2048, 4096, 8192, 16384, 32768])
    parser.add_argument("--prefetch-windows", type=int, nargs="+", default=[0, 2, 4, 8, 16])
    parser.add_argument("--prefetch-cache-kib", type=int, default=8192)
    parser.add_argument("--skip-prefetch", action="store_true")
    args = parser.parse_args()

    output = args.output.resolve()
    if not str(output).isascii():
        parser.error("--output must be ASCII: the first build rejects other paths")
    if output.exists():
        parser.error("--output must be a new directory")
    output.mkdir(parents=True)
    binaries = args.binaries.resolve()
    job_runner = binaries / "job_runner.exe"

    template = output / "template"
    template.mkdir()
    launch([sys.executable, "-m", "evaluation.static_fixture", "--output", template,
            "--run-id", "template", "--vectors", str(args.vectors),
            "--dimension", str(args.dimension),
            "--duration-ms", str(args.duration_ms),
            "--ops-per-second", str(args.rate)], template / "fixture.log")
    launch([job_runner, "512", "300", template / "build-job.json",
            binaries / "vector_build.exe", "--config", template / "build.json",
            "--report", template / "build-report.json"], template / "build.log")
    index = template / "index"
    graph = next(index.glob("segments/*/graph.bin"))
    graph_bytes = graph.stat().st_size
    graph_sha = hashlib.sha256(graph.read_bytes()).hexdigest()
    print(f"graph: {args.vectors} nodes, {graph_bytes} bytes ({graph_bytes / MIB:.1f} MiB)",
          flush=True)

    def run_case(name, cache_bytes, backend, policy, window):
        run = output / name
        run.mkdir()
        # Each run gets its own fixture so `trace.jsonl`, `queries.jsonl` and the
        # answers all carry this run's id; EVAL-1.1 cross-checks that field and
        # rejects a shared fixture. The index is shared because it depends only
        # on the vectors, which are identical for a fixed seed.
        launch([sys.executable, "-m", "evaluation.static_fixture", "--output", run,
                "--run-id", name, "--vectors", str(args.vectors),
                "--dimension", str(args.dimension),
                "--duration-ms", str(args.duration_ms),
                "--ops-per-second", str(args.rate)], run / "fixture.log")
        shutil.copytree(index, run / "index")
        config = load(run / "run-config.json")
        config["engine"].update(io_backend=backend, cache_policy=policy)
        config["search"]["prefetch_blocks"] = window
        config["workload"]["workers"] = args.workers
        # Admission slots gate how many queries may hold workspace at once. Two
        # is enough for two workers on a synchronous backend, but an asynchronous
        # backend keeps a query resident while its batch completes, so a slot
        # held longer than the inter-arrival gap rejects the next query with
        # resource_exhausted. Give every worker a spare slot.
        config["engine"]["max_queries"] = max(2, args.workers + 2)
        resize_cache(config, cache_bytes)
        dump(run / "run-config.json", config)
        launch([job_runner, str(JOB_LIMIT_MIB), "300", run / "job.json",
                binaries / "vector_bench.exe", "--config", run / "run-config.json",
                "--output", run, "--target", "engine", "--job-metrics", run / "job.json"],
               run / "bench.log")
        launch([sys.executable, "-m", "evaluation.check", "--run", run], run / "check.log")
        launch([sys.executable, str(ROOT / "tools/storage/verify-read-path.py"),
                "--run", run, "--json", run / "read-path-check.json"], run / "read-path.log")
        row = measure(run, cache_bytes, graph_bytes)
        row["mode"] = f"{policy}/{backend}"
        row["prefetch_window"] = window
        print(f"{name}: hit={row['hit_rate']:.4f} reads={row['read_calls']} "
              f"p95={row['latency_p95_ms']:.2f} ms", flush=True)
        return row

    sweep, prefetch = [], []
    for kib in args.cache_kib:
        cache_bytes = kib * KIB
        for policy, backend in (("clock", "synchronous"), ("clock", "windows_iocp")):
            name = f"sweep-{kib:05d}kib-{backend}"
            sweep.append(run_case(name, cache_bytes, backend, policy, 0))

    if not args.skip_prefetch:
        for window in args.prefetch_windows:
            name = f"prefetch-w{window:02d}"
            prefetch.append(run_case(name, args.prefetch_cache_kib * KIB,
                                     "windows_iocp", "clock", window))

    dump(output / "summary.json", {
        "fixture": {"vectors": args.vectors, "dimension": args.dimension,
                    "graph_bytes": graph_bytes, "graph_sha256": graph_sha,
                    "workers": args.workers, "duration_ms": args.duration_ms,
                    "ops_per_second": args.rate},
        "job_limit_mib": JOB_LIMIT_MIB,
        "sweep": sweep,
        "prefetch": prefetch,
    })
    plot(output / "read-path-sweep.png", sweep, prefetch or sweep)
    print(f"wrote {output / 'summary.json'}", flush=True)


if __name__ == "__main__":
    main()
