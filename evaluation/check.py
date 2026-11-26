"""Validate one dynamic run directory, then compute Recall@K from exact answers.

Usage:
    python -m evaluation.check --run evaluation/examples/stage3-b

The checker owns the acceptance rules from EVAL-1.1 section 7: protocol types,
request conservation, monotonic clocks, versions bounded by the snapshot, write
sequence continuity, and neighbour validity. Answers are recomputed from the
base vectors plus the write log, so a fabricated ground-truth file is detected
instead of trusted.

Exit code 0 means the run is protocol-complete and scored, 1 means invalid
artifacts, and 2 means incomplete or uncertain and not scored. Recall and
resource acceptance remain separate from protocol validation.
"""

import argparse
import hashlib
import heapq
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
TRACE_COMMON = ("schema_version", "protocol_version", "purpose", "run_id",
                "arrival_index", "kind", "operation_id")
DATA_FILES = ("vectors", "ids", "queries", "query_ids", "updates")


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
    require(type(value) in (int, float), "Expected finite number")
    try:
        number = float(value)
    except OverflowError as error:
        raise ContractError("Expected finite number") from error
    require(math.isfinite(number), "Expected finite number")
    return number


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


def artifact_path(run_dir, value):
    relative_path(value)
    path = (run_dir / value).resolve()
    require(path.is_relative_to(run_dir.resolve()), "Artifact escapes the run directory")
    return path


def same_run(row, config):
    header(row)
    require(all(row[key] == config[key] for key in
                ("run_id", "purpose", "protocol_version")), "Mixed run artifacts")


def checked_file(run_dir, filename, recorded, purpose):
    checksum(recorded, purpose)
    path = artifact_path(run_dir, filename)
    require(path.is_file(), f"Missing input file: {filename}")
    if recorded is not None:
        require(digest(path) == recorded, f"Checksum mismatch: {filename}")
    return path


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
        if identifier in self.base and self.base[identifier][0] == version:
            return self.base[identifier][1]
        key = (identifier, version)
        require(key in self.updates, f"Missing payload for id={identifier} version={version}")
        return self.updates[key]


def memory_budget(value):
    fields = ("total_bytes", "cache_bytes", "delta_bytes", "metadata_bytes",
              "workspace_bytes", "reserve_bytes")
    shape(value, fields, " (memory)")
    numbers = {key: u64(value[key], positive=key != "cache_bytes") for key in fields}
    require(sum(numbers[key] for key in fields[1:]) == numbers["total_bytes"],
            "Memory partitions do not sum")
    return numbers


def validate_engine(engine, dimension):
    shape(engine, ("directory", "io_backend", "direct_reads", "cache_policy", "block_bytes",
                   "memory", "rebuild_memory", "resources", "read_scheduling", "wal",
                   "max_queries", "max_pending_writes", "max_inflight_reads",
                   "max_prefetch_reads", "max_delta_segments", "group_commit_window_ms"),
          " (engine)")
    text(engine["directory"])
    require(engine["io_backend"] in ("synchronous", "windows_iocp"), "Invalid backend")
    require(engine["cache_policy"] in ("disabled", "clock"), "Invalid cache policy")
    boolean(engine["direct_reads"])
    u32(engine["block_bytes"], positive=True)
    query = memory_budget(engine["memory"])
    rebuild = (None if engine["rebuild_memory"] is None
               else memory_budget(engine["rebuild_memory"]))
    resources = engine["resources"]
    shape(resources, ("max_disk_segments", "max_retained_generations",
                      "max_retained_delta_vectors", "background_workspace_bytes",
                      "min_free_disk_bytes"), " (resources)")
    for key in ("max_disk_segments", "max_retained_generations", "max_retained_delta_vectors"):
        u32(resources[key], positive=True)
    require(resources["max_disk_segments"] <= 16, "ReadView segment capacity exceeded")
    require(resources["max_retained_generations"] >= 2, "Need two retained generations")
    background = u64(resources["background_workspace_bytes"], positive=True)
    u64(resources["min_free_disk_bytes"])
    if rebuild is not None:
        require(query["total_bytes"] == rebuild["total_bytes"], "Rebuild changes total limit")
        require(background < rebuild["workspace_bytes"], "No foreground rebuild workspace")
    retained = resources["max_retained_delta_vectors"] * (dimension * 4 + 32)
    for budget in (query, rebuild):
        if budget is not None:
            require(retained <= budget["delta_bytes"], "Retained delta payload exceeds budget")
    for key in ("max_queries", "max_pending_writes", "max_inflight_reads", "max_delta_segments"):
        u32(engine[key], positive=True)
    require(engine["max_delta_segments"] <= 4, "ReadView delta capacity exceeded")
    u32(engine["max_prefetch_reads"])
    require(u64(engine["group_commit_window_ms"]) <= (1 << 63) - 1, "chrono overflow")
    scheduling = engine["read_scheduling"]
    shape(scheduling, ("demand_reserve", "background_reserve", "max_background_reads",
                       "demand_burst_before_background"), " (read_scheduling)")
    for key in scheduling:
        u32(scheduling[key], positive=True)
    available = engine["max_inflight_reads"] - scheduling["demand_reserve"]
    require(scheduling["background_reserve"] <= scheduling["max_background_reads"] <= available,
            "Invalid background reservation")
    require(engine["max_prefetch_reads"] <= available - scheduling["background_reserve"],
            "Prefetch consumes reserved slots")
    shape(engine["wal"], ("rotation_bytes", "max_frame_bytes"), " (wal)")
    frame = (64 + dimension * 4 + 4 + 7) // 8 * 8
    frame_cap = u64(engine["wal"]["max_frame_bytes"], positive=True)
    require(frame <= frame_cap <= UINT32_MAX, "WAL frame cannot fit")
    require(u64(engine["wal"]["rotation_bytes"], positive=True) >= frame_cap + 64,
            "WAL rotation cannot fit maximum frame")


def load_config(run_dir):
    path = artifact_path(run_dir, "effective-config.json")
    require(path.exists(), "Missing effective-config.json")
    config = load(path)
    require(type(config) is dict, "effective-config must be an object")
    required = ("schema_version", "protocol_version", "purpose", "run_id", "index",
                "dataset", "search", "trace", "answers", "engine")
    for key in required:
        require(key in config, f"effective-config missing {key}")
    require(set(config) <= set(required) | {"baseline", "workload", "measurement",
                                           "unbudgeted_metadata_bytes"},
            "Unknown effective-config field")
    header(config)
    dataset = config["dataset"]
    shape(dataset, ("format", *DATA_FILES, "vector_count", "query_count",
                    "dimension", "metric", "sha256"), " (dataset)")
    require(dataset["format"] == "raw-f32le-v1", "Unsupported dataset format")
    require(dataset["metric"] == "squared_l2", "Unsupported metric")
    u32(dataset["dimension"], positive=True)
    u64(dataset["vector_count"], positive=True)
    u64(dataset["query_count"], positive=True)
    for key in DATA_FILES:
        relative_path(dataset[key])
    shape(dataset["sha256"], DATA_FILES, " (dataset.sha256)")
    for key in DATA_FILES:
        checksum(dataset["sha256"][key], config["purpose"])
    shape(config["index"], ("directory", "build_id", "manifest_generation"), " (index)")
    text(config["index"]["directory"])
    text(config["index"]["build_id"], 128)
    u64(config["index"]["manifest_generation"], positive=True)
    validate_engine(config["engine"], dataset["dimension"])
    require((run_dir / config["index"]["directory"]).resolve() ==
            (run_dir / config["engine"]["directory"]).resolve(), "Engine index directory mismatch")
    search = config["search"]
    shape(search, ("k", "candidate_limit", "prefetch_blocks", "timeout_ms"), " (search)")
    u32(search["k"], positive=True)
    u32(search["candidate_limit"], positive=True)
    require(search["candidate_limit"] >= search["k"], "Candidate budget below K")
    u32(search["prefetch_blocks"])
    require(search["prefetch_blocks"] <= config["engine"]["max_prefetch_reads"],
            "Query prefetch exceeds cap")
    require(u64(search["timeout_ms"], positive=True) <= (1 << 63) - 1, "chrono overflow")
    answers = config["answers"]
    shape(answers, ("file", "method"), " (answers)")
    relative_path(answers["file"])
    require(answers["method"] == "exact_scan_float64", "Unsupported answer method")
    require(type(config["trace"]) is dict, "trace must be an object")
    if "unbudgeted_metadata_bytes" in config:
        u64(config["unbudgeted_metadata_bytes"])
    validate_run_settings(config)
    return config


def validate_run_settings(config):
    if "workload" in config:
        workload = config["workload"]
        shape(workload, ("mode", "target_ops_per_second", "workers", "client_queue_capacity",
                         "client_queue_bytes", "query_fraction", "insert_fraction",
                         "upsert_fraction", "seed"), " (workload)")
        require(workload["mode"] in ("open_loop", "closed_loop"), "Unsupported workload mode")
        require(finite(workload["target_ops_per_second"]) > 0, "Invalid target rate")
        for key in ("workers", "client_queue_capacity"):
            u32(workload[key], positive=True)
        u64(workload["client_queue_bytes"], positive=True)
        u64(workload["seed"])
        fractions = [finite(workload[key]) for key in
                     ("query_fraction", "insert_fraction", "upsert_fraction")]
        require(all(0 <= value <= 1 for value in fractions) and
                abs(sum(fractions) - 1) <= 1e-9, "Invalid workload fractions")
    if "measurement" in config:
        measurement = config["measurement"]
        shape(measurement, ("warmup_ms", "duration_ms", "drain_timeout_ms", "sample_interval_ms",
                            "result_queue_capacity", "result_queue_bytes", "max_record_bytes"),
              " (measurement)")
        for key in ("warmup_ms", "duration_ms", "drain_timeout_ms", "sample_interval_ms"):
            require(u64(measurement[key], positive=key != "warmup_ms") <= (1 << 63) - 1,
                    "chrono overflow")
        u32(measurement["result_queue_capacity"], positive=True)
        bound = u32(measurement["max_record_bytes"], positive=True)
        require(bound >= 16384 + 6 * 4096 + 128 * config["search"]["k"],
                "Result record bound cannot fit K")
        require(u64(measurement["result_queue_bytes"], positive=True) >= bound,
                "Result queue byte cap below the record bound")
        if "workload" in config:
            require(u64(config["workload"]["client_queue_bytes"]) >= bound,
                    "Client queue byte cap below the record bound")


def load_dataset(run_dir, config, start):
    dataset = config["dataset"]
    dimension = dataset["dimension"]
    paths = {key: checked_file(run_dir, dataset[key], dataset["sha256"][key],
                               config["purpose"]) for key in DATA_FILES}
    base_path, ids_path = paths["vectors"], paths["ids"]
    base_rows = read_f32_rows(base_path, dimension)
    base_ids = read_u64_rows(ids_path)
    require(len(base_rows) == len(base_ids), "Vector and ID counts differ")
    require(len(base_ids) == u64(dataset["vector_count"]),
            "Declared vector_count does not match the files")
    require(len(set(base_ids)) == len(base_ids), "Duplicate base ID")
    versions = [0] * len(base_ids)
    if "baseline" in config:
        baseline = config["baseline"]
        shape(baseline, ("sequence", "versions", "sha256"), " (baseline)")
        require(u64(baseline["sequence"]) == start, "Baseline sequence differs from start")
        path = checked_file(run_dir, baseline["versions"], baseline["sha256"], config["purpose"])
        versions = read_u64_rows(path)
        require(len(versions) == len(base_ids), "Baseline version count differs from base IDs")
        require(all(version <= start for version in versions), "Baseline version exceeds start")
        nonzero = [version for version in versions if version]
        require(len(nonzero) == len(set(nonzero)), "Duplicate nonzero baseline version")
        require(max(versions, default=0) == start, "Baseline omits the latest committed sequence")
    else:
        require(start == 0, "Nonzero measurement start requires an explicit baseline")
    query_vectors = read_f32_rows(paths["queries"], dimension)
    query_ids = read_u64_rows(paths["query_ids"])
    require(len(query_vectors) == len(query_ids) == u64(dataset["query_count"]),
            "Declared query_count does not match query vectors and IDs")
    require(len(set(query_ids)) == len(query_ids), "Duplicate source query ID")
    read_f32_rows(paths["updates"], dimension)
    return ({identifier: (version, row)
             for identifier, version, row in zip(base_ids, versions, base_rows)},
            dict(zip(query_ids, query_vectors)))


def load_payload_files(run_dir, write_rows, dimension, config):
    """Read each referenced payload file once and validate it against the rows."""
    files = {}
    for row in write_rows:
        payload = row["payload"]
        shape(payload, PAYLOAD_FIELDS, " (payload)")
        relative_path(payload["file"])
        u32(payload["dimension"], positive=True)
        require(payload["dimension"] == dimension, "Payload dimension differs from the index")
        checksum(payload["sha256"], config["purpose"])
        path = artifact_path(run_dir, payload["file"])
        require(path == artifact_path(run_dir, config["dataset"]["updates"]),
                "Payload file differs from the registered updates file")
        require(payload["sha256"] == config["dataset"]["sha256"]["updates"],
                "Payload checksum differs from the registered updates checksum")
        if path not in files:
            require(path.exists(), f"Missing payload file {payload['file']}")
            files[path] = read_f32_rows(path, dimension)
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
    seen, ordering = set(), []
    for neighbour in row["neighbors"]:
        shape(neighbour, ("id", "version", "squared_distance"), " (neighbour)")
        identifier = u64(neighbour["id"])
        require(identifier not in seen, "Duplicate neighbour ID")
        seen.add(identifier)
        require(u64(neighbour["version"]) <= snapshot,
                "Neighbour version is newer than the snapshot")
        distance = finite(neighbour["squared_distance"])
        require(distance >= 0, "Negative squared distance")
        ordering.append((distance, identifier))
    require(ordering == sorted(ordering), "Neighbour order is not (distance, id) ascending")
    return snapshot


def check_neighbours(query_row, dataset, state, snapshot):
    """Every returned ID must be visible at the snapshot with its current version."""
    require(len(query_row["neighbors"]) == min(query_row["k"], len(state)),
            "Successful query is short of min(K, live_count)")
    seen, maximum_error = set(), 0.0
    for neighbour in query_row["neighbors"]:
        identifier = u64(neighbour["id"])
        version = u64(neighbour["version"])
        seen.add(identifier)
        require(identifier in state, f"ID {identifier} is not visible at S={snapshot}")
        visible_version = state[identifier][0]
        require(version == visible_version,
                f"ID {identifier} returned version {version}, current version is {visible_version}")
        reported = finite(neighbour["squared_distance"])
        expected = squared_l2(query_row["_query_vector"], dataset.vector(identifier, version))
        error = abs(reported - expected)
        maximum_error = max(maximum_error, error)
        require(error <= distance_tolerance(reported, expected),
                f"Distance mismatch for id={identifier}: reported {reported}, recomputed {expected}")
    return seen, maximum_error


def trace_rows(path, config):
    """Validate input events without inferring receipts or captured snapshots."""
    rows = load_rows(path)
    require(rows, "Trace is empty")
    seen = set()
    for index, row in enumerate(rows):
        require(type(row) is dict and row.get("kind") in ("write", "query"),
                "Invalid trace kind")
        extra = (("operation", "id", "payload_row") if row["kind"] == "write"
                 else ("query_id", "query_row"))
        shape(row, (*TRACE_COMMON, *extra), " (trace row)")
        same_run(row, config)
        require(u32(row["arrival_index"]) == index, "Trace arrival_index must be dense from zero")
        identifier = text(row["operation_id"], 128)
        require(identifier not in seen, "Duplicate operation ID in trace")
        seen.add(identifier)
        if row["kind"] == "write":
            require(row["operation"] in ("insert", "upsert"), "Invalid trace write operation")
            u64(row["id"])
            u64(row["payload_row"])
        else:
            u64(row["query_id"])
            u64(row["query_row"])
    return rows


def validate_trace(run_dir, config, query_rows, write_rows, query_vectors):
    """Tie the trace that drove the run to the artifacts it produced."""
    trace = config["trace"]
    required = ("file", "sha256", "operations", "queries", "seed")
    require(set(required) <= set(trace) <= set(required) | {"update_distribution", "snapshot_policy"},
            "Invalid trace fields")
    u64(trace["seed"])
    path = checked_file(run_dir, trace["file"], trace["sha256"], config["purpose"])
    rows = trace_rows(path, config)
    writes = [row for row in rows if row["kind"] == "write"]
    queries = [row for row in rows if row["kind"] == "query"]
    require(len(writes) == u64(trace["operations"]), "trace.operations must equal the write rows")
    require(len(queries) == u64(trace["queries"]), "trace.queries must equal the query rows")
    if "update_distribution" in trace:
        require(trace["update_distribution"] in ("uniform_existing_and_new", "zipf_existing"),
                "Unregistered update distribution")
    if "snapshot_policy" in trace:
        policy = trace["snapshot_policy"]
        shape(policy, ("mode", "min_distinct_snapshots", "max_queries_per_snapshot"),
              " (snapshot_policy)")
        require(policy["mode"] == "committed_prefix", "Unsupported snapshot policy")
        u32(policy["min_distinct_snapshots"], positive=True)
        u32(policy["max_queries_per_snapshot"], positive=True)
    if "workload" in config:
        for kind in ("query", "insert", "upsert"):
            count = sum((row["kind"] == "query" if kind == "query" else
                         row["kind"] == "write" and row["operation"] == kind) for row in rows)
            require(abs(config["workload"][kind + "_fraction"] - count / len(rows)) <= 1e-9,
                    "Workload fraction disagrees with trace")
    writes_by_id = {row["operation_id"]: row for row in write_rows}
    queries_by_id = {row["operation_id"]: row for row in query_rows}
    expected = {row["operation_id"] for row in rows}
    require(set(writes_by_id) | set(queries_by_id) <= expected,
            "Result contains operations outside the trace")
    missing = dict.fromkeys(("query", "insert", "upsert"), 0)
    source_ids = list(query_vectors)
    previous_scheduled = 0
    for row in rows:
        other = queries_by_id if row["kind"] == "write" else writes_by_id
        require(row["operation_id"] not in other, "Result kind differs from trace")
        record = (writes_by_id if row["kind"] == "write" else queries_by_id).get(row["operation_id"])
        if row["kind"] == "query":
            index = u64(row["query_row"])
            require(index < len(source_ids) and source_ids[index] == u64(row["query_id"]),
                    "Trace query_id does not match query_row in the query ID file")
        else:
            updates = artifact_path(run_dir, config["dataset"]["updates"])
            require(u64(row["payload_row"]) < updates.stat().st_size //
                    (config["dataset"]["dimension"] * 4), "Trace payload row outside updates file")
        if record is None:
            missing[row["operation"] if row["kind"] == "write" else "query"] += 1
            continue
        scheduled = u64(record["scheduled_ns"])
        require(scheduled >= previous_scheduled, "Result arrival times disagree with trace order")
        previous_scheduled = scheduled
        if row["kind"] == "write":
            require(record["operation"] == row["operation"] and record["id"] == row["id"] and
                    record["payload"]["row"] == row["payload_row"],
                    "Trace disagrees with write operation, ID or payload row")
        else:
            require(record["query_id"] == row["query_id"], "Trace query_id mismatch")
    return missing, {row["operation_id"]: row for row in queries}


def replay_prefixes(base, write_rows, snapshots):
    """Rebuild the exact effective state of every observed snapshot."""
    initial = dict(base)
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
        updates[(identifier, sequence)] = row["_vector"]
        state[identifier] = (sequence, row["_vector"])
        for snapshot in prefixes:
            if sequence <= snapshot:
                prefixes[snapshot][identifier] = (sequence, row["_vector"])
    return prefixes, updates, sequences


def validate_environment(run_dir, config, end_ns, complete):
    path = artifact_path(run_dir, "environment.json")
    require(path.is_file(), "Missing environment.json")
    environment = load(path)
    same_run(environment, config)
    text(environment["io_backend"])
    if config["purpose"] == "contract_example":
        text(environment["unavailable_reason"])
        return
    for key in ("formal_engine_measurement", "engine_options_applied",
                "input_content_used", "input_hashes_verified"):
        require(environment.get(key) is True, f"Measurement environment must confirm {key}")
    require(environment["io_backend"] == config["engine"]["io_backend"],
            "Measured backend differs from engine configuration")
    boolean(environment["direct_reads"])
    require(environment["direct_reads"] == config["engine"]["direct_reads"],
            "Measured direct_reads differs from engine configuration")
    for key in ("code_commit_at_configure", "build_config", "measurement_domain",
                "participating_processes", "log_volume", "log_filesystem"):
        text(environment[key])
    limit = u64(environment["job_limit_commit_bytes"], positive=True)
    require(limit >= u64(config["engine"]["memory"]["total_bytes"]),
            "Job limit is below engine budget")
    u64(environment["sample_interval_ms"], positive=True)
    u32(environment["workers"], positive=True)
    u32(environment["runtime_threads"], positive=True)
    require("unbudgeted_metadata_bytes" in config,
            "Missing unbudgeted metadata disclosure in effective-config")
    require(u64(environment["unbudgeted_metadata_bytes"]) ==
            u64(config["unbudgeted_metadata_bytes"]),
            "Unbudgeted metadata disclosure differs")
    metrics_path = artifact_path(run_dir, environment["external_job_metrics"])
    require(metrics_path.is_file(), "Missing external Job metrics")
    metrics = load(metrics_path)
    # job_runner emits native JSON integers; preserve its separate wire format.
    shape(metrics, ("schema_version", "limit_commit_bytes", "kernel_peak_job_commit_bytes",
                    "sampled_peak_private_bytes", "sampled_peak_working_set_bytes",
                    "total_processes", "sample_interval_ms", "elapsed_ms",
                    "timed_out", "exit_code"), " (external Job metrics)")
    require(type(metrics["schema_version"]) is int and metrics["schema_version"] == 1,
            "Unsupported Job metrics schema")
    for key in ("limit_commit_bytes", "kernel_peak_job_commit_bytes",
                "sampled_peak_private_bytes", "sampled_peak_working_set_bytes",
                "total_processes", "sample_interval_ms", "elapsed_ms", "exit_code"):
        require(type(metrics[key]) is int and 0 <= metrics[key] <= UINT64_MAX,
                "Invalid Job metric integer")
    require(metrics["total_processes"] > 0 and metrics["sample_interval_ms"] > 0 and
            metrics["elapsed_ms"] > 0, "Empty Job observation")
    boolean(metrics["timed_out"])
    require(metrics["limit_commit_bytes"] == limit, "Job limit differs from environment")
    peak = metrics["kernel_peak_job_commit_bytes"]
    require(peak > 0, "Job peak is zero")
    if complete:
        require(not metrics["timed_out"] and metrics["exit_code"] == 0,
                "Job did not complete successfully")
    if "workload" in config:
        require(environment["workers"] == config["workload"]["workers"],
                "Measured worker count differs")
    if "measurement" in config:
        require(environment["sample_interval_ms"] == config["measurement"]["sample_interval_ms"],
                "Measured sample interval differs")
    samples = artifact_path(run_dir, "samples.jsonl")
    require(samples.is_file(), "Missing samples.jsonl")
    rows = load_rows(samples)
    require(rows, "Empty resource samples")
    previous, previous_peak = 0, 0
    for row in rows:
        shape(row, ("schema_version", "protocol_version", "purpose", "run_id", "sampled_ns",
                    "private_bytes", "working_set_bytes", "kernel_peak_job_commit_bytes",
                    "unavailable_reason"), " (resource sample)")
        same_run(row, config)
        sampled = u64(row["sampled_ns"])
        require(previous <= sampled <= end_ns, "Resource sample time mismatch")
        previous = sampled
        unknown = False
        for key in ("private_bytes", "working_set_bytes", "kernel_peak_job_commit_bytes"):
            if row[key] is None:
                unknown = True
            else:
                value = u64(row[key])
                if key == "kernel_peak_job_commit_bytes":
                    require(value <= metrics[key], "Sample peak exceeds final Job peak")
                    require(value >= previous_peak, "Resource Job peak regressed")
                    previous_peak = value
        if unknown:
            text(row["unavailable_reason"])
        else:
            require(row["unavailable_reason"] is None, "Unexpected resource unavailable reason")
    return {
        "job_limit_commit_bytes": str(limit),
        "kernel_peak_job_commit_bytes": str(peak),
        "kernel_peak_exceeds_limit": peak > limit,
        "memory_acceptance": "not_evaluated",
    }


def validate_publication_history(query_rows, write_rows):
    """Check strict real-time orders; overlapping calls may publish in either order."""
    observations = []
    for kind, rows, key in (("write", write_rows, "sequence"),
                            ("query", query_rows, "snapshot_sequence")):
        observations.extend((u64(row["api_start_ns"]), u64(row["completed_ns"]),
                             kind, u64(row[key])) for row in rows if row["status"] == "ok")
    pending, maximum_write, maximum_query = [], -1, -1
    uncertain_end = min((u64(row["completed_ns"]) for row in write_rows
                         if row["status"] == "error" and
                         row["error"]["write_effect"] == "may_have_applied"), default=None)
    uncertain_sequence = min((u64(row["error"]["attempted_sequence"]) for row in write_rows
                              if row["status"] == "error" and
                              row["error"]["write_effect"] == "may_have_applied" and
                              row["error"]["attempted_sequence"] is not None), default=None)
    for started, completed, kind, sequence in sorted(observations):
        while pending and pending[0][0] < started:
            _, previous_kind, previous_sequence = heapq.heappop(pending)
            if previous_kind == "write":
                maximum_write = max(maximum_write, previous_sequence)
            else:
                maximum_query = max(maximum_query, previous_sequence)
        if kind == "write":
            require(uncertain_end is None or started <= uncertain_end,
                    "Successful write after uncertain write failure")
            require(uncertain_sequence is None or sequence < uncertain_sequence,
                    "Successful write sequence reaches uncertain prefix")
            require(sequence > max(maximum_write, maximum_query),
                    "Write sequence contradicts completed operations")
        else:
            require(sequence >= max(maximum_write, maximum_query),
                    "Query snapshot contradicts completed operations")
        heapq.heappush(pending, (completed, kind, sequence))


def validate_counts(counters, query_rows, write_rows, missing, start, end, duration):
    observed = {kind: dict.fromkeys(COUNT_FIELDS, 0) for kind in counters["by_type"]}
    sequences = []
    for row, kind in [(row, "query") for row in query_rows] + [
            (row, row["operation"]) for row in write_rows]:
        counts = observed[kind]
        counts["scheduled"] += 1
        counts[{"ok": "succeeded", "error": "failed", "rejected": "rejected"}[row["status"]]] += 1
        if u64(row["completed_ns"]) < duration:
            counts["window_completed"] += 1
            counts["window_succeeded"] += row["status"] == "ok"
        if row["status"] == "ok":
            if kind == "query":
                require(start <= u64(row["snapshot_sequence"]) <= end, "Snapshot outside run")
            else:
                sequence = u64(row["sequence"])
                require(start < sequence <= end, "Receipt outside measurement sequence interval")
                sequences.append(sequence)
    require(len(set(sequences)) == len(sequences), "Duplicate success sequence")
    for kind, counts in counters["by_type"].items():
        shape(counts, COUNT_FIELDS, f" (counters.by_type.{kind})")
        numbers = {key: u64(value) for key, value in counts.items()}
        require(numbers["scheduled"] == sum(numbers[key] for key in
                                            ("succeeded", "failed", "rejected", "pending")),
                f"Request conservation failed for {kind}")
        require(numbers["window_succeeded"] <= numbers["window_completed"] <=
                numbers["succeeded"] + numbers["failed"] + numbers["rejected"] and
                numbers["window_succeeded"] <= numbers["succeeded"], "Invalid window counters")
        for key in COUNT_FIELDS:
            if key not in ("pending", "scheduled"):
                require(numbers[key] == observed[kind][key], f"Counter mismatch: {kind}.{key}")
        require(numbers["scheduled"] == observed[kind]["scheduled"] + numbers["pending"] and
                numbers["pending"] == missing[kind], f"Missing terminal records for {kind}")
        require(not numbers["pending"] or counters["run_incomplete"], "Pending requests hidden")
    uncertain = any(row["status"] != "ok" and row["error"]["write_effect"] == "may_have_applied"
                    for row in write_rows)
    require(not uncertain or counters["requires_review"], "Uncertain write hidden")
    if not counters["run_incomplete"] and not counters["requires_review"]:
        ordered = sorted(sequences)
        require(len(ordered) == end - start and
                all(sequence == start + offset + 1 for offset, sequence in enumerate(ordered)),
                "Success write sequence prefix missing")
    return sorted(sequences), uncertain


def _evaluate(run_dir):
    require(run_dir.is_dir(), f"Run directory not found: {run_dir}")
    config = load_config(run_dir)
    k = config["search"]["k"]
    dimension = config["dataset"]["dimension"]

    counters_path = artifact_path(run_dir, "counters.json")
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
    require(end_ns <= UINT64_MAX, "End time overflow")
    start = u64(counters["measurement_start_sequence"])
    end = u64(counters["measurement_end_sequence"])
    require(start <= end, "Sequence regression")
    shape(counters["by_type"], ("query", "insert", "upsert"), " (by_type)")
    shape(counters["engine_counters"], ENGINE_COUNTERS, " (engine_counters)")
    for value in counters["engine_counters"].values():
        u64(value)

    query_rows = load_rows(artifact_path(run_dir, "queries.jsonl"))
    write_rows = load_rows(artifact_path(run_dir, "writes.jsonl"))
    seen_operations = set()
    for row in query_rows + write_rows:
        same_run(row, config)
        identifier = text(row["operation_id"], 128)
        require(identifier not in seen_operations, "Duplicate operation ID")
        seen_operations.add(identifier)

    resource_evidence = validate_environment(
        run_dir, config, end_ns,
        not counters["run_incomplete"] and not counters["requires_review"])
    resource_review = resource_evidence is not None and resource_evidence["kernel_peak_exceeds_limit"]
    if "measurement" in config:
        require(duration == u64(config["measurement"]["duration_ms"]) * 1_000_000,
                "Measurement duration differs from counters")
        require(u64(counters["drain_ns"]) <=
                u64(config["measurement"]["drain_timeout_ms"]) * 1_000_000,
                "Drain exceeds configured timeout")
    base, query_vectors = load_dataset(run_dir, config, start)
    payload_files = load_payload_files(run_dir, write_rows, dimension, config)
    for row in write_rows:
        validate_write(row)
        payload = row["payload"]
        row["_vector"] = payload_files[artifact_path(run_dir, payload["file"])][u64(payload["row"])]
        require(u64(row["scheduled_ns"]) < duration, "Write arrival outside the window")
        require(u64(row["completed_ns"]) <= end_ns, "Write completion beyond the drain")

    snapshots = {}
    for row in query_rows:
        snapshot = validate_query(row, k)
        require(u64(row["scheduled_ns"]) < duration, "Query arrival outside the window")
        require(u64(row["completed_ns"]) <= end_ns, "Query completion beyond the drain")
        if snapshot is not None:
            snapshots[snapshot] = snapshots.get(snapshot, 0) + 1

    missing, trace_queries = validate_trace(run_dir, config, query_rows, write_rows, query_vectors)
    sequences, uncertain = validate_counts(counters, query_rows, write_rows,
                                           missing, start, end, duration)
    validate_publication_history(query_rows, write_rows)

    answer_rows = {}
    answer_path = artifact_path(run_dir, config["answers"]["file"])
    require(answer_path.is_file(), "Missing independent answers")
    for row in load_rows(answer_path):
        shape(row, ("schema_version", "protocol_version", "purpose", "run_id", "operation_id",
                    "query_id", "snapshot_sequence", "k", "live_vectors", "metric",
                    "method", "top_k_ids"), " (answer)")
        same_run(row, config)
        text(row["operation_id"], 128)
        u64(row["query_id"])
        u64(row["snapshot_sequence"])
        u64(row["live_vectors"])
        require(row["metric"] == "squared_l2" and row["method"] == "exact_scan_float64",
                "Unsupported independent answer method")
        require(u32(row["k"], positive=True) == k, "Answer K differs from the run K")
        require(type(row["top_k_ids"]) is list, "top_k_ids must be a list")
        ids = [u64(item) for item in row["top_k_ids"]]
        require(len(ids) == len(set(ids)) and len(ids) <= k, "Invalid independent answer IDs")
        key = (row["operation_id"], row["query_id"], row["snapshot_sequence"])
        event = trace_queries.get(row["operation_id"])
        require(event is not None and event["query_id"] == row["query_id"],
                "Answer operation or query ID differs from trace")
        require(key not in answer_rows, "Duplicate answer join key")
        answer_rows[key] = row

    successful_keys = {(row["operation_id"], row["query_id"], row["snapshot_sequence"])
                       for row in query_rows if row["status"] == "ok"}
    pending_operations = set(trace_queries) - {row["operation_id"] for row in query_rows}
    require(all(key in successful_keys or key[0] in pending_operations for key in answer_rows),
            "Answer join does not match a successful or pending query")
    require(successful_keys <= set(answer_rows), "Missing independent answer for a query")

    if counters["run_incomplete"] or counters["requires_review"] or resource_review:
        result = {
            "schema_version": 1, "protocol_version": PROTOCOL, "purpose": config["purpose"],
            "run_id": config["run_id"], "run_incomplete": counters["run_incomplete"],
            "requires_review": counters["requires_review"] or resource_review,
            "resource_evidence": resource_evidence,
            "verdict": "incomplete" if counters["run_incomplete"] else "requires_review",
            "queries": {"total": u64(config["trace"]["queries"]), "pending": missing["query"]},
            "writes": {"acknowledged": len(sequences), "uncertain": uncertain},
            "recall_at_k": {"k": k, "mean": None, "mean_ok_only": None, "per_query": []},
            "checks": {"protocol_structure": "pass", "dynamic_correctness": "not_evaluated"},
            "notes": ["Complete artifacts and resolve review reasons before scoring."],
        }
        if resource_review:
            result["notes"].append(
                "Raw Job high-water mark exceeds the limit; retain the original evidence "
                "and investigate rejected allocations before resource acceptance.")
        (run_dir / "evaluation.json").write_text(
            json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
        return result

    prefixes, updates, sequences = replay_prefixes(base, write_rows, snapshots)
    dataset = Dataset(base, updates)

    scored, aggregate, maximum_error = [], 0.0, 0.0
    for row in query_rows:
        if row["status"] != "ok":
            scored.append({"operation_id": row["operation_id"], "query_id": row["query_id"],
                           "snapshot_sequence": None, "score": 0.0,
                           "reason": row["status"]})
            continue
        index = u64(row["query_id"])
        require(index in query_vectors, "query_id absent from the query ID file")
        row["_query_vector"] = query_vectors[index]
        snapshot = u64(row["snapshot_sequence"])
        state = prefixes[snapshot]
        seen, error = check_neighbours(row, dataset, state, snapshot)
        maximum_error = max(maximum_error, error)
        truth = exact_top_k(state, row["_query_vector"], k)
        truth_ids = [identifier for _, identifier, _ in truth]
        require(len(truth_ids) == min(k, len(state)),
                "Exact answer is short of min(K, live_count)")
        key = (row["operation_id"], row["query_id"], row["snapshot_sequence"])
        require(key in answer_rows, "Missing independent answer for a query")
        require(u64(answer_rows[key]["live_vectors"]) == len(state), "Answer live count mismatch")
        require([u64(item) for item in answer_rows[key]["top_k_ids"]] == truth_ids,
                "Ground-truth file disagrees with the recomputed exact answer")
        matched = len(seen.intersection(truth_ids))
        aggregate += matched / k
        scored.append({"operation_id": row["operation_id"], "query_id": row["query_id"],
                       "snapshot_sequence": snapshot, "score": matched / k,
                       "matched": matched, "returned": len(seen)})

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
        "run_incomplete": False,
        "requires_review": False,
        "resource_evidence": resource_evidence,
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
            "publication_real_time_order": "pass",
            "independent_answer_join": "pass",
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
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    return result


def evaluate(run_dir):
    run_dir = Path(run_dir).resolve()
    report = artifact_path(run_dir, "evaluation.json")
    try:
        return _evaluate(run_dir)
    except (ContractError, OSError, ValueError, KeyError, TypeError) as error:
        if run_dir.is_dir():
            report.write_text(json.dumps({"verdict": "invalid", "diagnostic": str(error)}) + "\n",
                              encoding="utf-8", newline="\n")
        raise


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
    if result["verdict"] != "pass":
        print(f"NOT SCORED: {result['run_id']}: {result['verdict']}", file=sys.stderr)
        return 2
    print(f"PASS: {result['run_id']} {result['queries']['total']} queries, "
          f"Recall@{recall['k']} = {recall['mean']:.4f} "
          f"({result['snapshots']['distinct']} snapshots, "
          f"{result['writes']['acknowledged']} acknowledged writes)")
    if recall["mean"] < 0.85:
        print("WARNING: recall is below the 0.85 acceptance target", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
