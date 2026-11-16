"""Validate one dynamic run directory, then compute Recall@K from exact answers.

Usage:
    python -m evaluation.check --run evaluation/examples/stage3-b

The checker owns the acceptance rules from EVAL-1.1 section 7: protocol types,
request conservation, monotonic clocks, versions bounded by the snapshot, write
sequence continuity, and neighbour validity. Answers are recomputed from the
base vectors plus the write log, so a fabricated ground-truth file is detected
instead of trusted.

Exit code 0 means the run is protocol-complete and scored. It does not mean the
recall target was met; that is reported separately in evaluation.json.
"""

import argparse
import hashlib
import json
import math
import re
import struct
import sys
from pathlib import Path, PureWindowsPath

UINT64_MAX = (1 << 64) - 1
UINT32_MAX = (1 << 32) - 1
PROTOCOL = "EVAL-1.1"
ERROR_CODES = (
    "invalid_argument", "not_found", "already_exists", "resource_exhausted",
    "deadline_exceeded", "search_incomplete", "io_error", "corruption",
    "unsupported", "closed", "recovery_required",
)
COUNT_FIELDS = ("scheduled", "succeeded", "failed", "rejected", "pending",
                "window_completed", "window_succeeded")
ENGINE_COUNTERS = ("io_reads_submitted", "io_read_bytes_completed", "io_read_errors",
                   "prefetch_submitted", "prefetch_consumed", "wal_bytes_written",
                   "checkpoint_bytes_written", "merge_bytes_written")
QUERY_FIELDS = ("schema_version", "protocol_version", "purpose", "run_id", "operation_id",
                "query_id", "snapshot_sequence", "view_generation", "status", "k",
                "scheduled_ns", "api_start_ns", "completed_ns", "error", "neighbors")
WRITE_FIELDS = ("schema_version", "protocol_version", "purpose", "run_id", "operation_id",
                "operation", "id", "payload", "sequence", "status", "scheduled_ns",
                "api_start_ns", "completed_ns", "error")
PAYLOAD_FIELDS = ("file", "row", "dimension", "sha256")


class ContractError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ContractError(message)


def shape(value, fields, where=""):
    require(type(value) is dict, f"Expected object{where}")
    expected = set(fields)
    require(set(value) == expected,
            f"Fields mismatch{where}: missing={sorted(expected - set(value))}, "
            f"extra={sorted(set(value) - expected)}")


def text(value, maximum=None):
    require(type(value) is str and bool(value), "Expected nonempty string")
    if maximum is not None:
        require(len(value.encode("utf-8")) <= maximum, "String exceeds byte limit")
    return value


def u64(value, positive=False):
    require(type(value) is str and re.fullmatch(r"0|[1-9][0-9]{0,19}", value) is not None,
            f"Expected canonical uint64 decimal string, got {value!r}")
    result = int(value)
    require(result <= UINT64_MAX and (not positive or result > 0), "uint64 range")
    return result


def u32(value, positive=False):
    require(type(value) is int and not isinstance(value, bool) and
            0 <= value <= UINT32_MAX and (not positive or value > 0),
            "Expected bounded JSON integer, not bool")
    return value


def boolean(value):
    require(type(value) is bool, "Expected JSON boolean")


def finite(value):
    require(type(value) in (int, float) and not isinstance(value, bool) and
            math.isfinite(value), "Expected finite number")
    return float(value)


def header(row):
    require(type(row["schema_version"]) is int and row["schema_version"] == 1,
            "Unsupported schema")
    require(row["protocol_version"] == PROTOCOL, "Unsupported protocol")
    require(row["purpose"] in ("contract_example", "measurement"), "Invalid purpose")
    text(row["run_id"], 128)


def checksum(value, purpose):
    if value is None:
        require(purpose == "contract_example", "Missing measurement checksum")
    else:
        require(type(value) is str and re.fullmatch(r"[0-9a-f]{64}", value) is not None,
                "Invalid SHA256")


def relative_path(value):
    text(value, 1024)
    path = PureWindowsPath(value)
    require(not path.drive and not path.root and ".." not in path.parts and
            ":" not in value and path.parts, "Path must stay in the run directory")


def parse_json(raw):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, f"Duplicate JSON key: {key}")
            result[key] = value
        return result

    def parse_float(value):
        number = float(value)
        require(math.isfinite(number), "Nonfinite JSON number")
        return number

    def reject_constant(value):
        raise ContractError(f"Nonfinite JSON constant: {value}")

    return json.loads(raw, object_pairs_hook=unique, parse_float=parse_float,
                      parse_constant=reject_constant)


def load(path):
    return parse_json(path.read_text(encoding="utf-8"))


def load_rows(path):
    return [parse_json(line) for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip()]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_f32_rows(path, dimension):
    require(dimension > 0, "Dimension must be positive")
    raw = path.read_bytes()
    stride = dimension * 4
    require(len(raw) % stride == 0, f"{path.name} length is not a multiple of the row size")
    unpack = struct.Struct("<" + "f" * dimension).unpack_from
    rows = [list(unpack(raw, offset)) for offset in range(0, len(raw), stride)]
    for row in rows:
        for value in row:
            require(math.isfinite(value), f"{path.name} contains a non-finite value")
    return rows


def read_u64_rows(path):
    raw = path.read_bytes()
    require(len(raw) % 8 == 0, f"{path.name} length is not a multiple of 8")
    return list(struct.unpack("<" + "Q" * (len(raw) // 8), raw))


def squared_l2(left, right):
    total = 0.0
    for a, b in zip(left, right):
        delta = float(a) - float(b)
        total += delta * delta
    return total


def exact_top_k(state, query, k):
    scored = [(squared_l2(query, vector), identifier, version)
              for identifier, (version, vector) in state.items()]
    scored.sort(key=lambda item: (item[0], item[1]))
    return scored[:k]


def distance_tolerance(reported, expected):
    return 1e-6 + 1e-9 * max(abs(reported), abs(expected))


class Dataset:
    """Base vectors plus every written payload, indexed by (id, version)."""

    def __init__(self, base, updates):
        self.base = base
        self.updates = updates

    def vector(self, identifier, version):
        if version == 0:
            require(identifier in self.base, f"Unknown base vector {identifier}")
            return self.base[identifier]
        key = (identifier, version)
        require(key in self.updates, f"Missing payload for id={identifier} version={version}")
        return self.updates[key]


def load_config(run_dir):
    path = run_dir / "effective-config.json"
    require(path.exists(), "Missing effective-config.json")
    config = load(path)
    require(type(config) is dict, "effective-config must be an object")
    for key in ("schema_version", "protocol_version", "purpose", "run_id", "dataset",
                "search", "trace"):
        require(key in config, f"effective-config missing {key}")
    header(config)
    dataset = config["dataset"]
    for key in ("format", "vectors", "ids", "vector_count", "dimension", "metric"):
        require(key in dataset, f"dataset missing {key}")
    require(dataset["format"] == "raw-f32le-v1", "Unsupported dataset format")
    require(dataset["metric"] == "squared_l2", "Unsupported metric")
    u32(dataset["dimension"], positive=True)
    u64(dataset["vector_count"], positive=True)
    for key in ("vectors", "ids"):
        relative_path(dataset[key])
    hashes = dataset.get("sha256", {})
    for key in ("vectors", "ids"):
        if key in hashes:
            checksum(hashes[key], config["purpose"])
    search = config["search"]
    require("k" in search and "candidate_limit" in search, "search section incomplete")
    u32(search["k"], positive=True)
    u32(search["candidate_limit"], positive=True)
    require(search["candidate_limit"] >= search["k"], "Candidate budget below K")
    if "queries" in dataset:
        relative_path(dataset["queries"])
    return config


def load_dataset(run_dir, config):
    dataset = config["dataset"]
    dimension = dataset["dimension"]
    base_path = run_dir / dataset["vectors"]
    ids_path = run_dir / dataset["ids"]
    require(base_path.exists() and ids_path.exists(), "Missing base dataset files")
    base_rows = read_f32_rows(base_path, dimension)
    base_ids = read_u64_rows(ids_path)
    require(len(base_rows) == len(base_ids), "Vector and ID counts differ")
    require(len(base_ids) == u64(dataset["vector_count"]),
            "Declared vector_count does not match the files")
    require(len(set(base_ids)) == len(base_ids), "Duplicate base ID")
    for key, path in (("vectors", base_path), ("ids", ids_path)):
        recorded = dataset.get("sha256", {}).get(key)
        if recorded is not None:
            require(digest(path) == recorded, f"{key} checksum mismatch")
    return {identifier: row for identifier, row in zip(base_ids, base_rows)}


def load_payload_files(run_dir, write_rows, dimension, purpose):
    """Read each referenced payload file once and validate it against the rows."""
    files = {}
    for row in write_rows:
        payload = row["payload"]
        shape(payload, PAYLOAD_FIELDS, " (payload)")
        relative_path(payload["file"])
        u32(payload["dimension"], positive=True)
        require(payload["dimension"] == dimension, "Payload dimension differs from the index")
        checksum(payload["sha256"], purpose)
        path = run_dir / payload["file"]
        if path not in files:
            require(path.exists(), f"Missing payload file {payload['file']}")
            files[path] = read_f32_rows(path, dimension)
            if payload["sha256"] is not None:
                require(digest(path) == payload["sha256"], "Payload checksum mismatch")
        require(u64(payload["row"]) < len(files[path]), "Payload row outside the file")
    return files


def validate_error(error, status):
    shape(error, ("code", "message", "system_error", "write_effect", "attempted_sequence"),
          " (error)")
    require(error["code"] in ERROR_CODES, f"Unknown error code {error['code']}")
    text(error["message"], 4096)
    require(type(error["system_error"]) is int and
            -(1 << 31) <= error["system_error"] < (1 << 31), "Invalid system error")
    require(error["write_effect"] in ("none", "may_have_applied"), "Invalid write effect")
    if error["attempted_sequence"] is not None:
        u64(error["attempted_sequence"], positive=True)
    if status == "rejected":
        require(error["code"] == "resource_exhausted" and
                error["write_effect"] == "none" and
                error["attempted_sequence"] is None, "Invalid client rejection")


def validate_timing(row):
    scheduled = u64(row["scheduled_ns"])
    completed = u64(row["completed_ns"])
    require(scheduled <= completed, "Completion before arrival")
    if row["status"] == "rejected":
        require(row["api_start_ns"] is None, "Rejected request entered the API")
        return scheduled, None, completed
    api_start = u64(row["api_start_ns"])
    require(scheduled <= api_start <= completed, "Invalid API timing")
    return scheduled, api_start, completed


def validate_write(row):
    shape(row, WRITE_FIELDS, " (write)")
    header(row)
    text(row["operation_id"], 128)
    require(row["status"] in ("ok", "error", "rejected"), "Invalid write status")
    require(row["operation"] in ("insert", "upsert"), "Invalid write operation")
    identifier = u64(row["id"])
    validate_timing(row)
    if row["status"] == "ok":
        require(row["error"] is None, "Successful write carries an error")
        return identifier, u64(row["sequence"], positive=True)
    require(row["sequence"] is None, "Failed write reports a success receipt")
    validate_error(row["error"], row["status"])
    return identifier, None


def validate_query(row, k):
    shape(row, QUERY_FIELDS, " (query)")
    header(row)
    text(row["operation_id"], 128)
    require(row["status"] in ("ok", "error", "rejected"), "Invalid query status")
    u64(row["query_id"])
    require(u32(row["k"], positive=True) == k, "Query K differs from the configured K")
    validate_timing(row)
    if row["status"] != "ok":
        require(row["snapshot_sequence"] is None and row["view_generation"] is None,
                "Failed query fabricates a snapshot")
        require(type(row["neighbors"]) is list and not row["neighbors"],
                "Failed query reports neighbours")
        validate_error(row["error"], row["status"])
        require(row["error"]["write_effect"] == "none" and
                row["error"]["attempted_sequence"] is None, "Query has a write effect")
        return None
    require(row["error"] is None, "Successful query carries an error")
    snapshot = u64(row["snapshot_sequence"])
    u64(row["view_generation"], positive=True)
    require(type(row["neighbors"]) is list, "Expected neighbours array")
    require(len(row["neighbors"]) <= k, "Too many neighbours")
    return snapshot


def check_neighbours(query_row, dataset, state, snapshot):
    """Every returned ID must be visible at the snapshot with its current version."""
    seen, ordering, maximum_error = set(), [], 0.0
    for neighbour in query_row["neighbors"]:
        shape(neighbour, ("id", "version", "squared_distance"), " (neighbour)")
        identifier = u64(neighbour["id"])
        version = u64(neighbour["version"])
        require(identifier not in seen, "Duplicate neighbour ID")
        seen.add(identifier)
        require(version <= snapshot, "Neighbour version is newer than the snapshot")
        require(identifier in state, f"ID {identifier} is not visible at S={snapshot}")
        visible_version = state[identifier][0]
        require(version == visible_version,
                f"ID {identifier} returned version {version}, current version is {visible_version}")
        reported = finite(neighbour["squared_distance"])
        require(reported >= 0, "Negative squared distance")
        expected = squared_l2(query_row["_query_vector"], dataset.vector(identifier, version))
        error = abs(reported - expected)
        maximum_error = max(maximum_error, error)
        require(error <= distance_tolerance(reported, expected),
                f"Distance mismatch for id={identifier}: reported {reported}, recomputed {expected}")
        ordering.append((reported, identifier))
    require(ordering == sorted(ordering), "Neighbour order is not (distance, id) ascending")
    return seen, maximum_error


def validate_trace(run_dir, config, query_rows, write_rows):
    """Tie the trace that drove the run to the artifacts it produced."""
    trace = config.get("trace")
    if trace is None:
        return
    require(type(trace) is dict and "file" in trace, "trace section needs a file")
    relative_path(trace["file"])
    path = run_dir / trace["file"]
    require(path.exists(), f"Missing trace file {trace['file']}")
    if "sha256" in trace:
        checksum(trace["sha256"], config["purpose"])
        if trace["sha256"] is not None:
            require(digest(path) == trace["sha256"], "Trace checksum mismatch")
    rows = load_rows(path)
    require(rows, "Trace is empty")
    for index, row in enumerate(rows):
        header(row)
        require(row["run_id"] == config["run_id"], "Trace belongs to another run")
        require(row["purpose"] == config["purpose"], "Trace purpose differs from the run")
        require(row["arrival_index"] == index, "Trace arrival_index must be dense from zero")
        require(row["kind"] in ("write", "query"), "Invalid trace kind")

    writes = [row for row in rows if row["kind"] == "write"]
    queries = [row for row in rows if row["kind"] == "query"]
    if "operations" in trace:
        require(len(writes) == u64(trace["operations"]),
                "trace.operations must equal the write rows")
    if "queries" in trace:
        require(len(queries) == u64(trace["queries"]),
                "trace.queries must equal the query rows")

    writes_by_id = {row["operation_id"]: row for row in write_rows}
    queries_by_id = {row["operation_id"]: row for row in query_rows}
    committed, seen_writes, seen_queries = 0, 0, 0
    for row in rows:
        if row["kind"] == "write":
            record = writes_by_id.get(row["operation_id"])
            require(record is not None,
                    f"Trace references unknown write {row['operation_id']}")
            require(record["id"] == row["id"] and record["sequence"] == row["sequence"],
                    "Trace disagrees with writes.jsonl")
            committed += 1
            seen_writes += 1
        else:
            record = queries_by_id.get(row["operation_id"])
            require(record is not None,
                    f"Trace references unknown query {row['operation_id']}")
            require(record["query_id"] == row["query_id"], "Trace query_id mismatch")
            require(record["snapshot_sequence"] == row["committed_prefix_at_arrival"],
                    "Query snapshot does not match its arrival point")
            require(record["snapshot_sequence"] is None or
                    u64(record["snapshot_sequence"]) <= committed,
                    "Query saw a prefix beyond the writes that had arrived")
            seen_queries += 1
    require(seen_writes == len(write_rows), "writes.jsonl contains operations outside the trace")
    require(seen_queries == len(query_rows), "queries.jsonl contains queries outside the trace")


def replay_prefixes(base, write_rows, snapshots):
    """Rebuild the exact effective state of every observed snapshot."""
    initial = {identifier: (0, vector) for identifier, vector in base.items()}
    prefixes = {snapshot: dict(initial) for snapshot in snapshots}
    ordered = sorted((row for row in write_rows if row["status"] == "ok"),
                     key=lambda row: u64(row["sequence"]))
    updates, sequences = {}, []
    state = dict(initial)
    for row in ordered:
        sequence = u64(row["sequence"])
        sequences.append(sequence)
        identifier = u64(row["id"])
        if row["operation"] == "insert":
            require(identifier not in state, f"Insert of existing ID {identifier} succeeded")
        else:
            require(identifier in state, f"Upsert of unknown ID {identifier} succeeded")
        updates[(identifier, sequence)] = row["_vector"]
        state[identifier] = (sequence, row["_vector"])
        for snapshot in prefixes:
            if sequence <= snapshot:
                prefixes[snapshot][identifier] = (sequence, row["_vector"])
    if sequences:
        require(sequences[0] == 1, "Write sequences do not start at 1")
        require(sequences == list(range(1, len(sequences) + 1)),
                "Write sequence gap in the acknowledged prefix")
    return prefixes, updates, sequences


def evaluate(run_dir):
    require(run_dir.is_dir(), f"Run directory not found: {run_dir}")
    config = load_config(run_dir)
    k = config["search"]["k"]
    dimension = config["dataset"]["dimension"]

    counters_path = run_dir / "counters.json"
    require(counters_path.exists(), "Missing counters.json")
    counters = load(counters_path)
    shape(counters, ("schema_version", "protocol_version", "purpose", "run_id", "duration_ns",
                     "drain_ns", "run_incomplete", "requires_review",
                     "measurement_start_sequence", "measurement_end_sequence",
                     "by_type", "engine_counters"), " (counters)")
    header(counters)
    boolean(counters["run_incomplete"])
    boolean(counters["requires_review"])
    require(counters["run_id"] == config["run_id"], "Counters belong to another run")
    require(counters["purpose"] == config["purpose"], "Mixed purposes in the run")
    duration = u64(counters["duration_ns"], positive=True)
    end_ns = duration + u64(counters["drain_ns"])
    u64(counters["measurement_start_sequence"])
    u64(counters["measurement_end_sequence"])
    shape(counters["by_type"], ("query", "insert", "upsert"), " (by_type)")
    shape(counters["engine_counters"], ENGINE_COUNTERS, " (engine_counters)")
    for value in counters["engine_counters"].values():
        u64(value)

    query_rows = load_rows(run_dir / "queries.jsonl")
    write_rows = load_rows(run_dir / "writes.jsonl")
    require(query_rows or write_rows, "Run contains no requests")

    base = load_dataset(run_dir, config)
    payload_files = load_payload_files(run_dir, write_rows, dimension, config["purpose"])
    for row in write_rows:
        validate_write(row)
        payload = row["payload"]
        row["_vector"] = payload_files[run_dir / payload["file"]][u64(payload["row"])]
        require(u64(row["scheduled_ns"]) < duration, "Write arrival outside the window")
        require(u64(row["completed_ns"]) <= end_ns, "Write completion beyond the drain")

    snapshots = {}
    for row in query_rows:
        snapshot = validate_query(row, k)
        require(u64(row["scheduled_ns"]) < duration, "Query arrival outside the window")
        require(u64(row["completed_ns"]) <= end_ns, "Query completion beyond the drain")
        if snapshot is not None:
            snapshots[snapshot] = snapshots.get(snapshot, 0) + 1

    if "query_count" in config["dataset"]:
        require(len(query_rows) == u64(config["dataset"]["query_count"]),
                "Declared query_count does not match queries.jsonl")
    validate_trace(run_dir, config, query_rows, write_rows)

    prefixes, updates, sequences = replay_prefixes(base, write_rows, snapshots)
    dataset = Dataset(base, updates)

    query_vectors = None
    if "queries" in config["dataset"]:
        path = run_dir / config["dataset"]["queries"]
        require(path.exists(), "Missing query vector file")
        query_vectors = read_f32_rows(path, dimension)
        recorded = config["dataset"].get("sha256", {}).get("queries")
        if recorded is not None:
            require(digest(path) == recorded, "queries checksum mismatch")

    answer_rows = {}
    answer_path = run_dir / "ground-truth.jsonl"
    if answer_path.exists():
        for row in load_rows(answer_path):
            for key in ("operation_id", "query_id", "snapshot_sequence", "k", "top_k_ids"):
                require(key in row, f"ground-truth missing {key}")
            require(u32(row["k"], positive=True) == k, "Answer K differs from the run K")
            require(type(row["top_k_ids"]) is list, "top_k_ids must be a list")
            key = (row["operation_id"], row["query_id"], row["snapshot_sequence"])
            require(key not in answer_rows, "Duplicate answer join key")
            answer_rows[key] = row

    scored, aggregate, maximum_error = [], 0.0, 0.0
    for row in query_rows:
        if row["status"] != "ok":
            scored.append({"operation_id": row["operation_id"], "query_id": row["query_id"],
                           "snapshot_sequence": None, "score": 0.0,
                           "reason": row["status"]})
            continue
        require(query_vectors is not None, "Query vectors are required to verify distances")
        index = u64(row["query_id"])
        require(index < len(query_vectors), "query_id outside the query file")
        row["_query_vector"] = query_vectors[index]
        snapshot = u64(row["snapshot_sequence"])
        state = prefixes[snapshot]
        seen, error = check_neighbours(row, dataset, state, snapshot)
        maximum_error = max(maximum_error, error)
        truth = exact_top_k(state, row["_query_vector"], k)
        truth_ids = [identifier for _, identifier, _ in truth]
        require(len(truth_ids) == min(k, len(state)),
                "Exact answer is short of min(K, live_count)")
        if answer_rows:
            key = (row["operation_id"], row["query_id"], row["snapshot_sequence"])
            require(key in answer_rows, "Missing independent answer for a query")
            require([int(item) for item in answer_rows[key]["top_k_ids"]] == truth_ids,
                    "Ground-truth file disagrees with the recomputed exact answer")
        matched = len(seen.intersection(truth_ids))
        aggregate += matched / k
        scored.append({"operation_id": row["operation_id"], "query_id": row["query_id"],
                       "snapshot_sequence": snapshot, "score": matched / k,
                       "matched": matched, "returned": len(seen)})

    observed = {kind: dict.fromkeys(COUNT_FIELDS, 0) for kind in counters["by_type"]}
    for row, kind in [(row, "query") for row in query_rows] + \
                     [(row, row["operation"]) for row in write_rows]:
        counts = observed[kind]
        counts["scheduled"] += 1
        counts[{"ok": "succeeded", "error": "failed", "rejected": "rejected"}[row["status"]]] += 1
        if u64(row["completed_ns"]) < duration:
            counts["window_completed"] += 1
            counts["window_succeeded"] += row["status"] == "ok"
    for kind, counts in counters["by_type"].items():
        shape(counts, COUNT_FIELDS, f" (counters.by_type.{kind})")
        numbers = {key: u64(value) for key, value in counts.items()}
        require(numbers["scheduled"] == sum(numbers[key] for key in
                                            ("succeeded", "failed", "rejected", "pending")),
                f"Request conservation failed for {kind}")
        for key in ("scheduled", "succeeded", "failed", "rejected"):
            require(numbers[key] == observed[kind][key], f"Counter mismatch: {kind}.{key}")
        require(not numbers["pending"] or counters["run_incomplete"], "Pending requests hidden")

    uncertain = any(row["status"] != "ok" and row["error"]["write_effect"] == "may_have_applied"
                    for row in write_rows)
    require(not uncertain or counters["requires_review"], "Uncertain write hidden")

    for query in query_rows:
        if query["status"] != "ok":
            continue
        snapshot = u64(query["snapshot_sequence"])
        for write in write_rows:
            if write["status"] != "ok":
                continue
            sequence = u64(write["sequence"])
            if u64(write["completed_ns"]) < u64(query["api_start_ns"]):
                require(sequence <= snapshot, "Query missed an acknowledged write")
            if u64(write["api_start_ns"]) > u64(query["completed_ns"]):
                require(sequence > snapshot, "Query observed a future write")

    ok_queries = [item for item in scored if item["snapshot_sequence"] is not None]
    recall = aggregate / len(query_rows) if query_rows else 0.0
    recall_ok = (sum(item["score"] for item in ok_queries) / len(ok_queries)
                 if ok_queries else 0.0)

    details = []
    for snapshot in sorted(snapshots):
        items = [item for item in scored if item["snapshot_sequence"] == snapshot]
        details.append({
            "snapshot_sequence": snapshot,
            "queries": len(items),
            "live_vectors": len(prefixes[snapshot]),
            "mean_recall": sum(item["score"] for item in items) / len(items) if items else 0.0,
        })

    result = {
        "schema_version": 1,
        "protocol_version": PROTOCOL,
        "purpose": config["purpose"],
        "run_id": config["run_id"],
        "queries": {
            "total": len(query_rows),
            "ok": sum(1 for row in query_rows if row["status"] == "ok"),
            "failed": sum(1 for row in query_rows if row["status"] == "error"),
            "rejected": sum(1 for row in query_rows if row["status"] == "rejected"),
        },
        "writes": {
            "acknowledged": len(sequences),
            "first_sequence": sequences[0] if sequences else 0,
            "last_sequence": sequences[-1] if sequences else 0,
            "uncertain": uncertain,
        },
        "recall_at_k": {"k": k, "mean": recall, "mean_ok_only": recall_ok,
                        "per_query": scored},
        "snapshots": {"distinct": len(snapshots), "details": details},
        "checks": {
            "protocol_types": "pass",
            "request_conservation": "pass",
            "clock_monotonic": "pass",
            "version_within_snapshot": "pass",
            "neighbour_versions_current": "pass",
            "neighbour_distances_recomputed": "pass",
            "write_sequence_prefix": "pass",
            "acknowledged_write_visibility": "pass",
            "independent_answer_join": "pass" if answer_rows else "not_provided",
        },
        "distance_check": {"max_abs_error": maximum_error,
                           "tolerance": "1e-6 + 1e-9*max(|reported|,|expected|)"},
        "verdict": "pass",
        "notes": [
            "Recall is scored against answers recomputed from base vectors plus the write log.",
            "Protocol completeness is not evidence that the 0.85 recall target was reached.",
        ],
    }
    (run_dir / "evaluation.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description="Validate a dynamic run and score recall.")
    parser.add_argument("--run", required=True, type=Path, help="run directory to check")
    args = parser.parse_args(argv)
    try:
        result = evaluate(args.run.resolve())
    except (ContractError, OSError, ValueError, KeyError, TypeError) as error:
        print(f"Stage-three B evaluation failed: {error}", file=sys.stderr)
        return 1
    recall = result["recall_at_k"]
    print(f"PASS: {result['run_id']} {result['queries']['total']} queries, "
          f"Recall@{recall['k']} = {recall['mean']:.4f} "
          f"({result['snapshots']['distinct']} snapshots, "
          f"{result['writes']['acknowledged']} acknowledged writes)")
    if recall["mean"] < 0.85:
        print("WARNING: recall is below the 0.85 acceptance target", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
