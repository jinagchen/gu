"""Generate a deterministic dynamic trace and its independent exact answers.

Usage:
    python -m evaluation.trace --output evaluation/examples/stage3-b

Everything written here is a contract fixture: tiny synthetic data with exact
brute-force answers. It is not a measurement, not a DiskANN build, and not
evidence about recall or performance of any real engine.
"""

import argparse
import hashlib
import json
import math
import struct
import sys
from pathlib import Path

MASK64 = (1 << 64) - 1
F32 = struct.Struct("<f")
U64 = struct.Struct("<Q")
FORMAT = "raw-f32le-v1"
PROTOCOL = "EVAL-1.1"

# Fixture memory partitions, matching configs/examples/static-run.json.
QUERY_MEMORY = {
    "total_bytes": "268435456",
    "cache_bytes": "107374182",
    "delta_bytes": "53687091",
    "metadata_bytes": "53687091",
    "workspace_bytes": "40265318",
    "reserve_bytes": "13421774",
}
RESOURCES = {
    "max_disk_segments": 16,
    "max_retained_generations": 2,
    "max_retained_delta_vectors": 12288,
    "background_workspace_bytes": "209715200",
    "min_free_disk_bytes": "42949672960",
}
READ_SCHEDULING = {
    "demand_reserve": 8,
    "background_reserve": 1,
    "max_background_reads": 8,
    "demand_burst_before_background": 8,
}
WAL = {"rotation_bytes": "67108864", "max_frame_bytes": "1048576"}


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

    def below(self, bound):
        return self.next() % bound

    def unit(self):
        return (self.next() >> 11) / float(1 << 53)


def f32(value):
    return F32.unpack(F32.pack(value))[0]


def write_f32(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as handle:
        for row in rows:
            handle.write(struct.pack("<" + "f" * len(row), *row))


def write_u64(path, values):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as handle:
        for value in values:
            handle.write(U64.pack(value))


def squared_l2(left, right):
    total = 0.0
    for a, b in zip(left, right):
        delta = float(a) - float(b)
        total += delta * delta
    return total


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dump(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n",
                    encoding="utf-8")


def dump_rows(path, rows):
    path.write_text("".join(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n"
                            for row in rows), encoding="utf-8")


def build_timeline(rng, base_count, write_count, query_count):
    """Interleave writes and queries; every query sees the committed prefix."""
    total = write_count + query_count
    slots = list(range(total))
    for index in range(query_count):
        # Keep slot 0 a write so at least one query observes a non-empty prefix.
        pick = 1 + index + rng.below(total - index - 1)
        slots[index], slots[pick] = slots[pick], slots[index]
    query_slots = set(slots[:query_count])
    target_snapshots = min(2, write_count, query_count)
    prefixes = {slot - rank for rank, slot in enumerate(sorted(query_slots))}
    if len(prefixes) < target_snapshots:
        # Preserve the seeded layout whenever it already covers the target.
        # Otherwise put queries after the first and last write.
        query_slots.remove(min(query_slots))
        query_slots.add(1)
        query_slots.remove(max(query_slots))
        query_slots.add(total - 1)
    if sum(1 for slot in range(total) if slot not in query_slots) == 0:
        raise SystemExit("trace generation failed: no write slot available")

    live_ids = list(range(base_count))
    events, next_id, write_index, query_index = [], base_count, 0, 0
    for slot in range(total):
        if slot in query_slots:
            events.append({
                "kind": "query",
                "query_id": query_index,
                "query_row": query_index,
                "committed_prefix_at_arrival": write_index,
            })
            query_index += 1
            continue
        operation = "insert" if (not live_ids or rng.unit() < 0.55) else "upsert"
        if operation == "insert":
            identifier = next_id
            next_id += 1
            live_ids.append(identifier)
        else:
            identifier = live_ids[rng.below(len(live_ids))]
        write_index += 1
        events.append({
            "kind": "write",
            "operation_id": f"w-{write_index:03d}",
            "operation": operation,
            "id": identifier,
            "payload_row": write_index - 1,
            "sequence": write_index,
        })
    distinct = {event["committed_prefix_at_arrival"] for event in events if event["kind"] == "query"}
    if len(distinct) < target_snapshots:
        raise SystemExit("trace generation failed: queries do not observe distinct snapshots")
    return events


def exact_top_k(state, query, k):
    scored = [(squared_l2(query, vector), identifier, version)
              for identifier, (version, vector) in state.items()]
    scored.sort(key=lambda item: (item[0], item[1]))
    return scored[:k]


def build_fixture(output, seed, base_count, dimension, write_count, query_count, k):
    counts = (base_count, dimension, write_count, query_count, k)
    if any(type(value) is not int or value <= 0 for value in counts):
        raise ValueError("all counts must be positive integers")
    if k > base_count:
        raise ValueError("k must not exceed the base vector count")
    rng = SplitMix64(seed)
    run_id = "stage3-b-exact"
    output.mkdir(parents=True, exist_ok=False)
    inputs = output / "inputs"

    base_rows = [[f32(rng.unit() * 2.0 - 1.0) for _ in range(dimension)]
                 for _ in range(base_count)]
    base = {identifier: (0, row) for identifier, row in enumerate(base_rows)}
    base_ids = list(range(base_count))

    events = build_timeline(rng, base_count, write_count, query_count)

    update_rows, write_rows = [], []
    query_rows, query_vectors, answer_rows = [], [], []
    state = dict(base)
    clock = 1_000_000
    for event in events:
        if event["kind"] == "write":
            row = [f32(rng.unit() * 2.0 - 1.0) for _ in range(dimension)]
            update_rows.append(row)
            state[event["id"]] = (event["sequence"], row)
            scheduled = clock
            api_start = scheduled + 1_000
            completed = api_start + 800_000
            clock = completed + 1_000
            write_rows.append({
                "schema_version": 1,
                "protocol_version": PROTOCOL,
                "purpose": "contract_example",
                "run_id": run_id,
                "operation_id": event["operation_id"],
                "operation": event["operation"],
                "id": str(event["id"]),
                "payload": {
                    "file": "inputs/updates.f32",
                    "row": str(event["payload_row"]),
                    "dimension": dimension,
                    "sha256": None,
                },
                "sequence": str(event["sequence"]),
                "status": "ok",
                "scheduled_ns": str(scheduled),
                "api_start_ns": str(api_start),
                "completed_ns": str(completed),
                "error": None,
            })
            continue

        source = base_rows[rng.below(base_count)]
        query = [f32(value + (rng.unit() - 0.5) * 0.1) for value in source]
        query_vectors.append(query)
        snapshot = event["committed_prefix_at_arrival"]
        neighbours = exact_top_k(state, query, k)
        scheduled = clock
        api_start = scheduled + 1_000
        completed = api_start + 400_000
        clock = completed + 1_000
        query_rows.append({
            "schema_version": 1,
            "protocol_version": PROTOCOL,
            "purpose": "contract_example",
            "run_id": run_id,
            "operation_id": f"q-{event['query_id']:03d}",
            "query_id": str(event["query_id"]),
            "snapshot_sequence": str(snapshot),
            "view_generation": str(snapshot + 1),
            "status": "ok",
            "k": k,
            "scheduled_ns": str(scheduled),
            "api_start_ns": str(api_start),
            "completed_ns": str(completed),
            "error": None,
            "neighbors": [{"id": str(identifier), "version": str(version),
                           "squared_distance": distance}
                          for distance, identifier, version in neighbours],
        })
        answer_rows.append({
            "schema_version": 1,
            "protocol_version": PROTOCOL,
            "purpose": "contract_example",
            "run_id": run_id,
            "operation_id": f"q-{event['query_id']:03d}",
            "query_id": str(event["query_id"]),
            "snapshot_sequence": str(snapshot),
            "k": k,
            "live_vectors": str(len(state)),
            "metric": "squared_l2",
            "method": "exact_scan_float64",
            "top_k_ids": [str(identifier) for _, identifier, _ in neighbours],
        })

    trace_rows = []
    for index, event in enumerate(events):
        row = {
            "schema_version": 1,
            "protocol_version": PROTOCOL,
            "purpose": "contract_example",
            "run_id": run_id,
            "arrival_index": index,
            "kind": event["kind"],
            "operation_id": (event["operation_id"] if event["kind"] == "write"
                             else f"q-{event['query_id']:03d}"),
        }
        if event["kind"] == "write":
            row["operation"] = event["operation"]
            row["id"] = str(event["id"])
            row["payload_row"] = str(event["payload_row"])
        else:
            row["query_id"] = str(event["query_id"])
            row["query_row"] = str(event["query_row"])
        trace_rows.append(row)

    write_f32(inputs / "base.f32", base_rows)
    write_u64(inputs / "base.ids.u64", base_ids)
    write_f32(inputs / "updates.f32", update_rows)
    write_f32(inputs / "queries.f32", query_vectors)
    write_u64(inputs / "query.ids.u64", list(range(len(query_vectors))))
    for row in write_rows:
        row["payload"]["sha256"] = digest(inputs / "updates.f32")
    dump_rows(output / "trace.jsonl", trace_rows)
    dump_rows(output / "writes.jsonl", write_rows)
    dump_rows(output / "queries.jsonl", query_rows)
    dump_rows(output / "ground-truth.jsonl", answer_rows)

    duration = clock + 1_000
    counters = {
        "schema_version": 1,
        "protocol_version": PROTOCOL,
        "purpose": "contract_example",
        "run_id": run_id,
        "duration_ns": str(duration),
        "drain_ns": "0",
        "run_incomplete": False,
        "requires_review": False,
        "measurement_start_sequence": "0",
        "measurement_end_sequence": str(len(write_rows)),
        "by_type": {
            "query": {"scheduled": str(len(query_rows)), "succeeded": str(len(query_rows)),
                      "failed": "0", "rejected": "0", "pending": "0",
                      "window_completed": str(len(query_rows)),
                      "window_succeeded": str(len(query_rows))},
            "insert": {"scheduled": str(sum(1 for row in write_rows
                                             if row["operation"] == "insert")),
                       "succeeded": str(sum(1 for row in write_rows
                                            if row["operation"] == "insert")),
                       "failed": "0", "rejected": "0", "pending": "0",
                       "window_completed": str(sum(1 for row in write_rows
                                                   if row["operation"] == "insert")),
                       "window_succeeded": str(sum(1 for row in write_rows
                                                   if row["operation"] == "insert"))},
            "upsert": {"scheduled": str(sum(1 for row in write_rows
                                             if row["operation"] == "upsert")),
                       "succeeded": str(sum(1 for row in write_rows
                                            if row["operation"] == "upsert")),
                       "failed": "0", "rejected": "0", "pending": "0",
                       "window_completed": str(sum(1 for row in write_rows
                                                   if row["operation"] == "upsert")),
                       "window_succeeded": str(sum(1 for row in write_rows
                                                   if row["operation"] == "upsert"))},
        },
        "engine_counters": {
            "io_reads_submitted": "0",
            "io_read_bytes_completed": "0",
            "io_read_errors": "0",
            "prefetch_submitted": "0",
            "prefetch_consumed": "0",
            "wal_bytes_written": "0",
            "checkpoint_bytes_written": "0",
            "merge_bytes_written": "0",
        },
    }
    dump(output / "counters.json", counters)

    config = {
        "schema_version": 1,
        "protocol_version": PROTOCOL,
        "purpose": "contract_example",
        "run_id": run_id,
        "index": {"directory": "index", "build_id": "build-contract-001",
                  "manifest_generation": "1"},
        "dataset": {
            "format": FORMAT,
            "vectors": "inputs/base.f32",
            "ids": "inputs/base.ids.u64",
            "queries": "inputs/queries.f32",
            "query_ids": "inputs/query.ids.u64",
            "updates": "inputs/updates.f32",
            "vector_count": str(base_count),
            "query_count": str(query_count),
            "dimension": dimension,
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
                  "operations": str(write_count), "queries": str(query_count),
                  "seed": str(seed)},
        "answers": {"file": "ground-truth.jsonl", "method": "exact_scan_float64"},
        "engine": {
            "directory": "index",
            "io_backend": "synchronous",
            "direct_reads": True,
            "cache_policy": "disabled",
            "block_bytes": 4096,
            "memory": QUERY_MEMORY,
            "rebuild_memory": None,
            "resources": RESOURCES,
            "read_scheduling": READ_SCHEDULING,
            "wal": WAL,
            "max_queries": 1,
            "max_pending_writes": 64,
            "max_inflight_reads": 64,
            "max_prefetch_reads": 16,
            "max_delta_segments": 4,
            "group_commit_window_ms": "1",
        },
        "search": {"k": k, "candidate_limit": 128, "prefetch_blocks": 0,
                   "timeout_ms": "5000"},
    }
    dump(output / "effective-config.json", config)

    environment = {
        "schema_version": 1,
        "protocol_version": PROTOCOL,
        "purpose": "contract_example",
        "run_id": run_id,
        "code_commit": None,
        "build_mode": None,
        "io_backend": "synchronous",
        "unavailable_reason": "Synthetic evaluation fixture; no engine or device was used.",
    }
    dump(output / "environment.json", environment)
    (output / "stdout.log").write_text(
        "fixture generated by python -m evaluation.trace\n", encoding="utf-8")
    (output / "stderr.log").write_text("", encoding="utf-8")
    return {"run_id": run_id, "writes": len(write_rows), "queries": len(query_rows),
            "snapshots": sorted({int(row["snapshot_sequence"]) for row in query_rows})}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path,
                        help="new run directory to create")
    parser.add_argument("--seed", type=int, default=20261116)
    parser.add_argument("--base", type=int, default=24, help="base vector count")
    parser.add_argument("--dimension", type=int, default=4)
    parser.add_argument("--writes", type=int, default=10)
    parser.add_argument("--queries", type=int, default=6)
    parser.add_argument("--k", type=int, default=10)
    args = parser.parse_args(argv)
    if min(args.base, args.dimension, args.writes, args.queries, args.k) <= 0:
        parser.error("all counts must be positive")
    if args.k > args.base:
        parser.error("k must not exceed the base vector count")
    if args.output.exists():
        parser.error("output directory already exists")
    summary = build_fixture(args.output.resolve(), args.seed, args.base, args.dimension,
                            args.writes, args.queries, args.k)
    print(f"wrote {summary['run_id']}: {summary['writes']} writes, "
          f"{summary['queries']} queries, snapshots {summary['snapshots']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
