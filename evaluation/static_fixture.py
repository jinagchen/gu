"""Prepare a static-baseline measurement fixture: inputs, query-only trace and
independent exact answers.

Usage:
    python -m evaluation.static_fixture --output runs/stage4-static-001

Everything written here is deterministic and byte-stable (SplitMix64, LF
endings, float32 little endian), so two runs of the same command produce
identical hashes. It is a small synthetic fixture, not a retrieval-quality
claim; the answers are exact brute-force top-K computed in float64, which makes
them independent of the C++ engine under test.

A static baseline has no writes, so the EVAL-1.1 trace contains query rows only
and the update payload is empty.
"""

import argparse
from fractions import Fraction
import hashlib
import json
import struct
import sys
from pathlib import Path

MASK64 = (1 << 64) - 1
PROTOCOL = "EVAL-1.1"
F32 = struct.Struct("<f")
U64 = struct.Struct("<Q")
WINDOW_NS = 1_000_000_000


class SplitMix64:
    """Fixed generator so fixtures stay byte-identical across Python versions."""

    def __init__(self, seed):
        self.state = seed & MASK64

    def next(self):
        self.state = (self.state + 0x9E3779B97F4A7C15) & MASK64
        value = self.state
        value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
        value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & MASK64
        return value ^ (value >> 31)

    def unit(self):
        return self.next() / float(1 << 64)

    def below(self, bound):
        return self.next() % bound


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_text(path, text):
    path.write_text(text, encoding="utf-8", newline="\n")


def dump(path, value):
    write_text(path, json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n")


def dump_rows(path, rows):
    write_text(path, "".join(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n"
                             for row in rows))


def write_f32(path, rows):
    path.write_bytes(b"".join(b"".join(F32.pack(value) for value in row) for row in rows))


def write_u64(path, values):
    path.write_bytes(b"".join(U64.pack(value) for value in values))


def scheduled_ns(ordinal, rate):
    """Mirror of the runner's open-loop arrival clock."""
    return int(Fraction(ordinal) * WINDOW_NS / Fraction(rate))


def planned(duration_ns, rate):
    """Mirror of the runner's arrival count for one measurement window."""
    count = int(-(-Fraction(duration_ns) * Fraction(rate) // WINDOW_NS))
    while count > 0 and scheduled_ns(count - 1, rate) >= duration_ns:
        count -= 1
    while scheduled_ns(count, rate) < duration_ns:
        count += 1
    return count


def squared_l2(left, right):
    total = 0.0
    for a, b in zip(left, right):
        delta = float(a) - float(b)
        total += delta * delta
    return total


def exact_top_k(base, query, k):
    scored = [(squared_l2(query, row), identifier) for identifier, row in base]
    scored.sort(key=lambda item: (item[0], item[1]))
    return scored[:k]


def make_vectors(rng, count, dimension, clusters):
    """Tight clusters keep the graph interesting without breaking exactness."""
    centers = [[rng.unit() * 2.0 - 1.0 for _ in range(dimension)] for _ in range(clusters)]
    rows = []
    for index in range(count):
        center = centers[index % clusters]
        rows.append([F32.unpack(F32.pack(center[axis] + (rng.unit() - 0.5) * 0.02))[0]
                     for axis in range(dimension)])
    return rows


def build_arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path,
                        help="run directory to prepare; must be empty or absent")
    parser.add_argument("--vectors", type=int, default=512)
    parser.add_argument("--dimension", type=int, default=16)
    # A single search costs roughly 40 ms of service time here, so the arrival
    # rate must stay well under 25 ops/s. Pushing it higher saturates the runner
    # and turns throughput into a machine-load observation instead of a property
    # of the index, which makes repeated runs impossible to compare.
    parser.add_argument("--ops-per-second", type=int, default=10)
    parser.add_argument("--clusters", type=int, default=16)
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--candidate-limit", type=int, default=128)
    parser.add_argument("--duration-ms", type=int, default=6000)
    parser.add_argument("--sample-interval-ms", type=int, default=50)
    parser.add_argument("--seed", type=int, default=20261121)
    parser.add_argument("--run-id", default="stage4-static-001")
    return parser.parse_args(argv)


def main(argv=None):
    args = build_arguments(argv)
    if args.vectors < args.k + 1 or args.dimension < 1 or args.ops_per_second < 1:
        raise SystemExit("fixture needs vectors > k, a dimension and a positive rate")
    duration_ns = args.duration_ms * 1_000_000
    query_count = planned(duration_ns, args.ops_per_second)
    if query_count < args.k + 1:
        raise SystemExit("measurement window schedules too few queries "
                         f"({query_count}); raise --duration-ms or --ops-per-second")
    output = args.output.resolve()
    inputs = output / "inputs"
    inputs.mkdir(parents=True, exist_ok=True)

    rng = SplitMix64(args.seed)
    base_rows = make_vectors(rng, args.vectors, args.dimension, args.clusters)
    base_ids = [1000 + 7 * index for index in range(args.vectors)]

    # Queries are perturbed copies of real vectors, so exact neighbours exist.
    query_rows, query_ids = [], []
    for index in range(query_count):
        source = base_rows[rng.below(args.vectors)]
        query_rows.append([F32.unpack(F32.pack(value + (rng.unit() - 0.5) * 0.01))[0]
                           for value in source])
        query_ids.append(5000 + 3 * index)

    write_f32(inputs / "base.f32", base_rows)
    write_u64(inputs / "base.ids.u64", base_ids)
    write_f32(inputs / "queries.f32", query_rows)
    write_u64(inputs / "query.ids.u64", query_ids)
    write_f32(inputs / "updates.f32", [])

    base = list(zip(base_ids, base_rows))
    trace_rows, answer_rows = [], []
    for ordinal in range(query_count):
        row = (args.seed % query_count + ordinal % query_count) % query_count
        operation = f"q-{ordinal}"
        trace_rows.append({
            "schema_version": 1,
            "protocol_version": PROTOCOL,
            "purpose": "measurement",
            "run_id": args.run_id,
            "arrival_index": ordinal,
            "kind": "query",
            "operation_id": operation,
            "query_id": str(query_ids[row]),
            "query_row": str(row),
        })
        truth = exact_top_k(base, query_rows[row], args.k)
        answer_rows.append({
            "schema_version": 1,
            "protocol_version": PROTOCOL,
            "purpose": "measurement",
            "run_id": args.run_id,
            "operation_id": operation,
            "query_id": str(query_ids[row]),
            "snapshot_sequence": "0",
            "k": args.k,
            "live_vectors": str(len(base)),
            "metric": "squared_l2",
            "method": "exact_scan_float64",
            "top_k_ids": [str(identifier) for _, identifier in truth],
        })
    dump_rows(output / "trace.jsonl", trace_rows)
    dump_rows(output / "ground-truth.jsonl", answer_rows)

    engine = json.loads((Path(__file__).resolve().parents[1] /
                         "configs/examples/static-run.json").read_text(encoding="utf-8"))["engine"]
    engine["directory"] = "index"
    engine["cache_policy"] = "disabled"
    config = {
        "schema_version": 1,
        "protocol_version": PROTOCOL,
        "purpose": "measurement",
        "run_id": args.run_id,
        "index": {"directory": "index", "build_id": "static-baseline-001",
                  "manifest_generation": "1"},
        "dataset": {
            "format": "raw-f32le-v1",
            "vectors": "inputs/base.f32",
            "ids": "inputs/base.ids.u64",
            "queries": "inputs/queries.f32",
            "query_ids": "inputs/query.ids.u64",
            "updates": "inputs/updates.f32",
            "vector_count": str(args.vectors),
            "query_count": str(query_count),
            "dimension": args.dimension,
            "metric": "squared_l2",
            "sha256": {
                "vectors": digest(inputs / "base.f32"),
                "ids": digest(inputs / "base.ids.u64"),
                "queries": digest(inputs / "queries.f32"),
                "query_ids": digest(inputs / "query.ids.u64"),
                "updates": digest(inputs / "updates.f32"),
            },
        },
        "trace": {"file": "trace.jsonl", "sha256": digest(output / "trace.jsonl"),
                  "operations": "0", "queries": str(query_count), "seed": str(args.seed)},
        "answers": {"file": "ground-truth.jsonl", "method": "exact_scan_float64"},
        "engine": engine,
        "search": {"k": args.k, "candidate_limit": args.candidate_limit,
                   "prefetch_blocks": 0, "timeout_ms": "5000"},
        "workload": {"mode": "open_loop",
                     "target_ops_per_second": float(args.ops_per_second), "workers": 1,
                     "client_queue_capacity": 128, "client_queue_bytes": "1048576",
                     "query_fraction": 1.0, "insert_fraction": 0.0, "upsert_fraction": 0.0,
                     "seed": str(args.seed)},
        "measurement": {"warmup_ms": "0", "duration_ms": str(args.duration_ms),
                        "drain_timeout_ms": "30000",
                        "sample_interval_ms": str(args.sample_interval_ms),
                        "result_queue_capacity": 128, "result_queue_bytes": "1048576",
                        "max_record_bytes": 65536},
    }
    # Authored configuration, not the emitted artifact: vector_bench publishes
    # effective-config.json itself, and the runner refuses to overwrite one.
    dump(output / "run-config.json", config)

    root = Path(__file__).resolve().parents[1]
    build = json.loads((root / "configs/examples/build-run.json").read_text(encoding="utf-8"))
    build["purpose"] = "measurement"
    build["build_id"] = "static-baseline-001"
    build["dataset"] = {
        "format": "raw-f32le-v1",
        "vectors": "inputs/base.f32",
        "ids": "inputs/base.ids.u64",
        "vector_count": str(args.vectors),
        "dimension": args.dimension,
        "metric": "squared_l2",
        "sha256": {"vectors": config["dataset"]["sha256"]["vectors"],
                   "ids": config["dataset"]["sha256"]["ids"]},
    }
    build["native"]["native_version"] = build["native"]["version"]
    build["native"]["parameters"] = "R32 exact-knn plus xor long-range links"
    build["staging"] = {"directory": "index.staging", "same_volume": True,
                        "reuse_allowed": False}
    build["output"] = {"directory": "index", "checkpoint_sequence": "0",
                       "generation": "1", "publish_current": True}
    build["notes"] = [
        "Static baseline fixture: exact kNN graph with deterministic long-range links.",
        "Input hashes are real; purpose is measurement, so a mismatch must fail the build.",
        "The Job commit limit is enforced by the launching job_runner, not by this file.",
    ]
    dump(output / "build.json", build)

    print(json.dumps({
        "run_directory": str(output),
        "vectors": args.vectors, "queries": query_count, "dimension": args.dimension,
        "k": args.k, "duration_ms": args.duration_ms,
        "ops_per_second": args.ops_per_second,
        "trace_sha256": config["trace"]["sha256"],
        "base_sha256": config["dataset"]["sha256"]["vectors"],
    }, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
