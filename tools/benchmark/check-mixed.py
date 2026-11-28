"""Exercise real mixed Engine requests and independently score their snapshots."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from evaluation import check, static_fixture as fixture


def launch(run, label, command, timeout=150):
    result = subprocess.run([str(part) for part in command], cwd=ROOT,
                            capture_output=True, text=True, errors="replace", timeout=timeout)
    fixture.write_text(run / (label + ".log"), result.stdout + result.stderr)
    check.require(result.returncode == 0, f"{label} failed, see {run / (label + '.log')}")


def prepare(run, ratio, args):
    fixture.main(["--output", str(run), "--run-id", run.name, "--vectors", "128",
                  "--dimension", "16", "--duration-ms", str(args.duration_ms),
                  "--ops-per-second", str(args.ops_per_second),
                  "--sample-interval-ms", str(args.sample_interval_ms)])
    config_path = run / "run-config.json"
    config = check.load(config_path)
    rows = check.load_rows(run / "trace.jsonl")
    write_count = len(rows) * (100 - ratio) // 100
    check.require(write_count > 0, "The selected window must schedule at least one write")
    positions = {((2 * i + 1) * len(rows)) // (2 * write_count): i
                 for i in range(write_count)}
    rng = fixture.SplitMix64(20261128)
    payloads = fixture.make_vectors(rng, write_count, 16, 4)
    fixture.write_f32(run / "inputs/updates.f32", payloads)
    for position, i in positions.items():
        row = rows[position]
        del row["query_id"], row["query_row"]
        is_insert = i % 2 == 0
        row.update(kind="write", operation_id=f"w-{i}",
                   operation="insert" if is_insert else "upsert",
                   id=str(900000 + i if is_insert else 1000 + 7 * (i % 4)),
                   payload_row=str(i))
    fixture.dump_rows(run / "trace.jsonl", rows)
    config["trace"].update(sha256=fixture.digest(run / "trace.jsonl"),
                           operations=str(write_count), queries=str(len(rows) - write_count))
    config["dataset"]["sha256"]["updates"] = fixture.digest(run / "inputs/updates.f32")
    config["workload"].update(workers=args.workers,
                              query_fraction=(len(rows) - write_count) / len(rows),
                              insert_fraction=((write_count + 1) // 2) / len(rows),
                              upsert_fraction=(write_count // 2) / len(rows))
    engine = config["engine"]
    engine.update(io_backend=args.io_backend, cache_policy=args.cache_policy,
                  max_queries=args.workers, max_delta_segments=2)
    engine["resources"]["max_retained_delta_vectors"] = args.delta_slots
    engine["resources"]["background_workspace_bytes"] = str(8 << 20)
    engine["resources"]["min_free_disk_bytes"] = str(64 << 20)
    engine["rebuild_memory"] = dict(engine["memory"])
    config["search"]["prefetch_blocks"] = args.prefetch_blocks
    fixture.dump(config_path, config)
    return config


def exact_answers(run, config, queries, writes):
    dataset = config["dataset"]
    k = config["search"]["k"]
    dimension = dataset["dimension"]
    base, query_vectors = check.load_dataset(run, config, 0)
    trace = {row["operation_id"]: row for row in check.load_rows(run / "trace.jsonl")}
    payloads = check.load_payload_files(run, writes, dimension, config)
    for write in writes:
        location = write["payload"]
        path = check.artifact_path(run, location["file"])
        write["_vector"] = payloads[path][int(location["row"])]
    successful = [row for row in queries if row["status"] == "ok"]
    snapshots = {int(row["snapshot_sequence"]) for row in successful}
    states, _, _ = check.replay_prefixes(base, writes, snapshots)
    answers = []
    for row in successful:
        event = trace[row["operation_id"]]
        snapshot = int(row["snapshot_sequence"])
        state = states[snapshot]
        vector = query_vectors[int(event["query_id"])]
        answer = check.exact_top_k(state, vector, k)
        answers.append({
            "schema_version": 1, "protocol_version": "EVAL-1.1", "purpose": config["purpose"],
            "run_id": config["run_id"], "operation_id": row["operation_id"],
            "query_id": row["query_id"], "snapshot_sequence": str(snapshot), "k": k,
            "live_vectors": str(len(state)), "metric": "squared_l2",
            "method": "exact_scan_float64",
            "top_k_ids": [str(identifier) for _, identifier, _ in answer],
        })
    fixture.dump_rows(run / config["answers"]["file"], answers)


def run_case(args, run, ratio):
    run.mkdir()
    config = prepare(run, ratio, args)
    launch(run, "build", [args.job_runner, 512, 120, run / "build-job.json", args.build,
                          "--config", run / "build.json", "--report", run / "build-report.json"])
    job_timeout = max(120, args.duration_ms // 1000 + 60)
    launch(run, "bench", [args.job_runner, 384, job_timeout, run / "job.json", args.binary,
                          "--config", run / "run-config.json", "--output", run,
                          "--job-metrics", run / "job.json"], timeout=job_timeout + 30)
    queries = check.load_rows(run / "queries.jsonl")
    writes = check.load_rows(run / "writes.jsonl")
    receipts = check.load_rows(run / "receipts.jsonl")
    confirmed = [row for row in writes if row["status"] == "ok"]
    check.require(receipts == confirmed, "Receipts disagree with successful write records")
    exact_answers(run, config, queries, writes)
    evaluation = check.evaluate(run)
    recall = evaluation["recall_at_k"]["mean"]
    check.require(evaluation["verdict"] == "pass" and recall >= 0.85,
                  f"Mixed run failed independent evaluation: {recall}")
    intervals = [(int(row["api_start_ns"]), int(row["completed_ns"])) for row in confirmed]
    overlapped = sum(any(int(query["api_start_ns"]) < end and start < int(query["completed_ns"])
                         for start, end in intervals)
                     for query in queries if query["status"] == "ok")
    check.require(overlapped > 0, "No actual read/write API intervals overlapped")
    counters = check.load(run / "counters.json")["engine_counters"]
    check.require(int(counters["checkpoint_bytes_written"]) > 0 and
                  int(counters["merge_bytes_written"]) > 0,
                  "Mixed run did not cross a checkpoint and compaction")
    print(f"{ratio}:{100-ratio} Recall@10={recall:.3f} receipts={len(receipts)} "
          f"overlapping_queries={overlapped} artifacts={run}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--job-runner", type=Path, required=True)
    parser.add_argument("--output-parent", type=Path, required=True)
    parser.add_argument("--io-backend", choices=("synchronous", "windows_iocp"),
                        default="synchronous")
    parser.add_argument("--cache-policy", choices=("disabled", "clock"), default="disabled")
    parser.add_argument("--prefetch-blocks", type=int, default=0)
    parser.add_argument("--duration-ms", type=int, default=4000)
    parser.add_argument("--ops-per-second", type=int, default=100)
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--delta-slots", type=int, default=16)
    parser.add_argument("--sample-interval-ms", type=int, default=50)
    parser.add_argument("--query-fraction", type=int, choices=(80, 95), action="append")
    args = parser.parse_args()
    parent = args.output_parent.resolve()
    if not str(parent).isascii():
        parent = Path(tempfile.gettempdir()).resolve()
    check.require(str(parent).isascii(), "The first-build fixture needs an ASCII workspace")
    parent.mkdir(parents=True, exist_ok=True)
    workspace = Path(tempfile.mkdtemp(prefix="mixed-", dir=parent))
    for ratio in args.query_fraction or (95, 80):
        run_case(args, workspace / f"mix-{ratio}", ratio)


if __name__ == "__main__":
    main()
