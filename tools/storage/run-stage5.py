"""Compare the Stage 5 read paths under one index, workload and Job limit."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
JOB_LIMIT_MIB = 384
MODES = {
    "sync": ("synchronous", "disabled", 0),
    "sync-clock": ("synchronous", "clock", 0),
    "iocp": ("windows_iocp", "disabled", 0),
    "iocp-clock": ("windows_iocp", "clock", 0),
    "iocp-prefetch": ("windows_iocp", "clock", 8),
}


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def dump(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def launch(command, log):
    with log.open("wb") as output:
        result = subprocess.run(command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f"{command[0]} failed ({result.returncode}), see {log}")


def measure(run, profile, mode, reference, cache_bytes):
    result = load(run / "evaluation.json")
    if result["verdict"] != "pass" or result["recall_at_k"]["mean"] < 0.85:
        raise SystemExit(f"Recall or checker regression in {run}")
    rows = sorted((json.loads(line) for line in
                   (run / "queries.jsonl").read_text(encoding="utf-8").splitlines()),
                  key=lambda row: row["operation_id"])
    if any(row["status"] != "ok" for row in rows):
        raise SystemExit(f"Unsuccessful query in {run}")
    semantic = [(row["operation_id"], row["query_id"], row["snapshot_sequence"], row["neighbors"])
                for row in rows]
    if reference is not None and semantic != reference:
        raise SystemExit(f"Search outputs differ from the baseline in {run}")
    latencies = sorted((int(row["completed_ns"]) - int(row["scheduled_ns"])) / 1e6
                       for row in rows)
    stats = load(run / "summary.json")["read_path"]
    counts = load(run / "counters.json")["engine_counters"]
    job = load(run / "job.json")
    hits, misses = int(stats["cache_hits"]), int(stats["cache_misses"])
    submitted, consumed = int(counts["prefetch_submitted"]), int(counts["prefetch_consumed"])
    ready = int(stats["prefetch_ready_at_demand"])
    bytes_completed = int(counts["io_read_bytes_completed"])
    logical_bytes = (hits + misses) * 4096
    if (int(stats["cache_resident_bytes"]) > cache_bytes
            or stats["peak_inflight_reads"] > 64 or stats["peak_prefetch_reads"] > 16
            or not 0 <= ready <= consumed <= submitted or int(counts["io_read_errors"])
            or not 0 < job["kernel_peak_job_commit_bytes"] <= JOB_LIMIT_MIB * 2**20
            or (MODES[mode][1] == "disabled" and hits)
            or (not MODES[mode][2] and submitted)):
        raise SystemExit(f"Read path or Job accounting invariant failed in {run}")
    return {
        "profile": profile, "mode": mode, "queries": len(rows),
        "recall_at_10": result["recall_at_k"]["mean"],
        "read_calls": int(counts["io_reads_submitted"]), "completed_bytes": bytes_completed,
        "completed_to_logical_byte_ratio": bytes_completed / logical_bytes,
        "cache_hits": hits, "cache_misses": misses, "cache_hit_rate": hits / (hits + misses),
        "cache_resident_bytes": int(stats["cache_resident_bytes"]),
        "prefetch_submitted": submitted, "prefetch_consumed": consumed,
        "prefetch_ready_at_demand": ready,
        "prefetch_use_rate": consumed / submitted if submitted else None,
        "peak_inflight_reads": stats["peak_inflight_reads"],
        "peak_prefetch_reads": stats["peak_prefetch_reads"],
        "peak_job_commit_bytes": job["kernel_peak_job_commit_bytes"],
        "p50_ms": latencies[math.ceil(len(rows) * 0.5) - 1],
        "p95_ms": latencies[math.ceil(len(rows) * 0.95) - 1],
        "p99_ms": latencies[math.ceil(len(rows) * 0.99) - 1],
    }, semantic, latencies


def plot(output, distributions):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    figure, axes = plt.subplots(1, 2, figsize=(11, 4), sharey=True, layout="constrained")
    for axis, (profile, modes) in zip(axes, distributions.items(), strict=True):
        for mode, values in modes.items():
            axis.step(values, [(i + 1) / len(values) for i in range(len(values))],
                      where="post", label=mode)
        axis.set(xscale="log", title=f"{profile.capitalize()} cache budget",
                 xlabel="End-to-end latency (ms)")
        axis.grid(alpha=0.25)
    axes[0].set_ylabel("Fraction of queries")
    axes[1].legend(loc="lower right")
    figure.savefig(output, dpi=150)
    plt.close(figure)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True,
                        help="New directory on the NTFS test volume with an ASCII path")
    parser.add_argument("--binaries", type=Path, default=ROOT / "build/windows/Release")
    parser.add_argument("--duration-ms", type=int, default=3000)
    parser.add_argument("--rate", type=int, default=20)
    args = parser.parse_args()
    output = args.output.resolve()
    if not str(output).isascii() or output.exists():
        parser.error("--output must be a new directory with an ASCII path")
    output.mkdir(parents=True)
    binaries = args.binaries.resolve()
    job_runner = binaries / "job_runner.exe"
    index = None
    reference = None
    cases = []
    distributions = {}
    for profile, cache_bytes in (("resident", 8 * 2**20), ("tight", 128 * 2**10)):
        distributions[profile] = {}
        for mode, (backend, policy, window) in MODES.items():
            run = output / f"{profile}-{mode}"
            run.mkdir()
            launch([sys.executable, "-m", "evaluation.static_fixture", "--output", run,
                    "--run-id", run.name, "--duration-ms", str(args.duration_ms),
                    "--ops-per-second", str(args.rate)], run / "fixture.log")
            config = load(run / "run-config.json")
            engine = config["engine"]
            engine.update(io_backend=backend, cache_policy=policy, max_queries=2)
            memory = engine["memory"]
            memory["reserve_bytes"] = str(int(memory["reserve_bytes"])
                                          + int(memory["cache_bytes"]) - cache_bytes)
            memory["cache_bytes"] = str(cache_bytes)
            config["search"]["prefetch_blocks"] = window
            config["workload"]["workers"] = 2
            dump(run / "run-config.json", config)
            if index is None:
                launch([job_runner, "512", "120", run / "build-job.json",
                        binaries / "vector_build.exe", "--config", run / "build.json",
                        "--report", run / "build-report.json"], run / "build.log")
                index = run / "index"
            else:
                shutil.copytree(index, run / "index")
            launch([job_runner, str(JOB_LIMIT_MIB), "120", run / "job.json",
                    binaries / "vector_bench.exe", "--config", run / "run-config.json",
                    "--output", run, "--target", "engine", "--job-metrics", run / "job.json"],
                   run / "bench.log")
            launch([sys.executable, "-m", "evaluation.check", "--run", run], run / "check.log")
            case, reference, values = measure(run, profile, mode, reference, cache_bytes)
            cases.append(case)
            distributions[profile][mode] = values
            print(f"{run.name}: {case['read_calls']} reads, {case['p95_ms']:.3f} ms p95",
                  flush=True)
    if not any(case["prefetch_ready_at_demand"] > 0 for case in cases):
        raise SystemExit("No prefetch was observed ready ahead of demand")
    graph = next(index.glob("segments/*/graph.bin"))
    dump(output / "summary.json", {
        "fixture": {"vectors": 512, "dimension": 16, "k": 10, "workers": 2,
                    "duration_ms": args.duration_ms, "ops_per_second": args.rate,
                    "graph_sha256": hashlib.sha256(graph.read_bytes()).hexdigest()},
        "job_limit_mib": JOB_LIMIT_MIB,
        "identical_query_results": True,
        "cases": cases,
    })
    plot(output / "latency-cdf.png", distributions)


if __name__ == "__main__":
    main()
