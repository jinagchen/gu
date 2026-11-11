"""Check A-side design fixtures, not engine performance or vector correctness."""

import json
import math
from pathlib import Path, PureWindowsPath
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
UINT64_MAX = (1 << 64) - 1
UINT32_MAX = (1 << 32) - 1
COUNT_FIELDS = (
    "scheduled", "succeeded", "failed", "rejected", "pending",
    "window_completed", "window_succeeded",
)
ENGINE_COUNTERS = (
    "io_reads_submitted", "io_read_bytes_completed", "io_read_errors",
    "prefetch_submitted", "prefetch_consumed", "wal_bytes_written",
    "checkpoint_bytes_written", "merge_bytes_written",
)
ERROR_CODES = (
    "invalid_argument", "not_found", "already_exists", "resource_exhausted",
    "deadline_exceeded", "search_incomplete", "io_error", "corruption",
    "unsupported", "closed", "recovery_required",
)
COMMON_FIELDS = (
    "schema_version", "protocol_version", "purpose", "run_id", "operation_id",
    "status", "scheduled_ns", "api_start_ns", "completed_ns", "error",
)


class ContractError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ContractError(message)


def shape(value, fields):
    require(type(value) is dict, "Expected object")
    expected = set(fields)
    require(set(value) == expected,
            f"Fields mismatch: missing={expected - set(value)}, extra={set(value) - expected}")


def text(value, maximum=None):
    require(type(value) is str and bool(value), "Expected nonempty string")
    if maximum is not None:
        require(len(value.encode("utf-8")) <= maximum, "String exceeds byte limit")
    return value


def u64(value, positive=False):
    require(type(value) is str and
            re.fullmatch(r"0|[1-9][0-9]{0,19}", value) is not None,
            "Expected canonical uint64 decimal string")
    result = int(value)
    require(result <= UINT64_MAX and (not positive or result > 0), "uint64 range")
    return result


def u32(value, positive=False):
    require(type(value) is int and 0 <= value <= UINT32_MAX and
            (not positive or value > 0), "Expected bounded JSON integer, not bool")
    return value


def boolean(value):
    require(type(value) is bool, "Expected JSON boolean")


def finite(value):
    require(type(value) in (int, float) and abs(value) <= sys.float_info.max and
            math.isfinite(value), "Expected finite number")
    return value


def header(value):
    require(type(value["schema_version"]) is int and value["schema_version"] == 1,
            "Unsupported schema")
    require(value["protocol_version"] == "EVAL-1.1", "Unsupported protocol")
    require(value["purpose"] in ("contract_example", "measurement"), "Invalid purpose")
    text(value["run_id"], 128)


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
            ":" not in value and path.parts, "Payload path must stay in run directory")


def memory(value):
    fields = ("total_bytes", "cache_bytes", "delta_bytes", "metadata_bytes",
              "workspace_bytes", "reserve_bytes")
    shape(value, fields)
    numbers = {key: u64(value[key], positive=key != "cache_bytes") for key in fields}
    require(sum(numbers[key] for key in fields[1:]) == numbers["total_bytes"],
            "Memory partitions do not sum")
    return numbers


def resource_modes(value):
    query = memory(value["memory"])
    rebuild = None if value["rebuild_memory"] is None else memory(value["rebuild_memory"])
    resources = value["resources"]
    shape(resources, ("max_disk_segments", "max_retained_generations",
                      "max_retained_delta_vectors", "background_workspace_bytes",
                      "min_free_disk_bytes"))
    for key in ("max_disk_segments", "max_retained_generations",
                "max_retained_delta_vectors"):
        u32(resources[key], positive=True)
    require(resources["max_retained_generations"] >= 2, "Need two retained generations")
    workspace = u64(resources["background_workspace_bytes"], positive=True)
    u64(resources["min_free_disk_bytes"])
    if rebuild is not None:
        require(query["total_bytes"] == rebuild["total_bytes"], "Rebuild changes total limit")
        require(workspace < rebuild["workspace_bytes"], "No foreground rebuild workspace")
    return query, rebuild


def run_config(value):
    shape(value, ("schema_version", "protocol_version", "purpose", "run_id", "dataset",
                  "engine", "search", "workload", "measurement"))
    header(value)
    dataset = value["dataset"]
    shape(dataset, ("format", "vectors", "ids", "queries", "query_ids", "vector_count",
                    "query_count", "dimension", "metric", "sha256"))
    require(dataset["format"] == "raw-f32le-v1" and dataset["metric"] == "squared_l2",
            "Unsupported dataset format")
    dimension = u32(dataset["dimension"], positive=True)
    u64(dataset["vector_count"], positive=True)
    u64(dataset["query_count"], positive=True)
    shape(dataset["sha256"], ("vectors", "ids", "queries", "query_ids"))
    for key in dataset["sha256"]:
        text(dataset[key])
        checksum(dataset["sha256"][key], value["purpose"])
    engine = value["engine"]
    shape(engine, ("directory", "io_backend", "direct_reads", "cache_policy", "block_bytes",
                   "memory", "rebuild_memory", "resources", "read_scheduling", "wal",
                   "max_queries", "max_pending_writes", "max_inflight_reads",
                   "max_prefetch_reads", "max_delta_segments", "group_commit_window_ms"))
    text(engine["directory"])
    require(engine["io_backend"] in ("synchronous", "windows_iocp"), "Invalid backend")
    require(engine["cache_policy"] in ("disabled", "clock"), "Invalid cache policy")
    boolean(engine["direct_reads"])
    u32(engine["block_bytes"], positive=True)
    query, rebuild = resource_modes(engine)
    for budget in (query, rebuild):
        if budget is not None:
            retained = engine["resources"]["max_retained_delta_vectors"] * (dimension * 4 + 32)
            require(retained <= budget["delta_bytes"], "Retained delta payload exceeds budget")
    for key in ("max_queries", "max_pending_writes", "max_inflight_reads", "max_delta_segments"):
        u32(engine[key], positive=True)
    u32(engine["max_prefetch_reads"])
    require(u64(engine["group_commit_window_ms"]) <= (1 << 63) - 1, "chrono overflow")
    scheduling = engine["read_scheduling"]
    shape(scheduling, ("demand_reserve", "background_reserve", "max_background_reads",
                       "demand_burst_before_background"))
    for key in scheduling:
        u32(scheduling[key], positive=True)
    available = engine["max_inflight_reads"] - scheduling["demand_reserve"]
    require(scheduling["background_reserve"] <= scheduling["max_background_reads"] <= available,
            "Invalid background reservation")
    require(engine["max_prefetch_reads"] <= available - scheduling["background_reserve"],
            "Prefetch consumes reserved demand/background slots")
    shape(engine["wal"], ("rotation_bytes", "max_frame_bytes"))
    frame = (64 + dimension * 4 + 4 + 7) // 8 * 8
    frame_cap = u64(engine["wal"]["max_frame_bytes"], positive=True)
    require(frame <= frame_cap <= UINT32_MAX, "WAL frame cannot fit")
    require(u64(engine["wal"]["rotation_bytes"], positive=True) >= frame_cap + 64,
            "WAL rotation cannot fit maximum frame")
    search = value["search"]
    shape(search, ("k", "candidate_limit", "prefetch_blocks", "timeout_ms"))
    u32(search["k"], positive=True)
    u32(search["candidate_limit"], positive=True)
    u32(search["prefetch_blocks"])
    require(search["candidate_limit"] >= search["k"], "Candidate budget below K")
    require(search["prefetch_blocks"] <= engine["max_prefetch_reads"], "Query prefetch exceeds cap")
    require(u64(search["timeout_ms"], positive=True) <= (1 << 63) - 1, "chrono overflow")
    workload = value["workload"]
    shape(workload, ("mode", "target_ops_per_second", "workers", "client_queue_capacity",
                     "client_queue_bytes", "query_fraction", "insert_fraction",
                     "upsert_fraction", "seed"))
    require(workload["mode"] == "open_loop", "Fixture protocol supports open_loop")
    require(finite(workload["target_ops_per_second"]) > 0, "Invalid arrival rate")
    for key in ("workers", "client_queue_capacity"):
        u32(workload[key], positive=True)
    fractions = [finite(workload[key]) for key in
                 ("query_fraction", "insert_fraction", "upsert_fraction")]
    require(all(0 <= item <= 1 for item in fractions) and
            math.isclose(sum(fractions), 1, rel_tol=0, abs_tol=1e-12), "Invalid fractions")
    u64(workload["seed"])
    measurement = value["measurement"]
    shape(measurement, ("warmup_ms", "duration_ms", "drain_timeout_ms", "sample_interval_ms",
                        "result_queue_capacity", "result_queue_bytes", "max_record_bytes"))
    for key in ("warmup_ms", "duration_ms", "drain_timeout_ms", "sample_interval_ms"):
        require(u64(measurement[key], positive=key != "warmup_ms") <= UINT64_MAX // 1000000,
                "Millisecond to nanosecond overflow")
    u32(measurement["result_queue_capacity"], positive=True)
    record_cap = u32(measurement["max_record_bytes"], positive=True)
    require(record_cap >= 16384 + 6 * 4096 + 128 * search["k"], "Result record bound below K")
    require(u64(measurement["result_queue_bytes"], positive=True) >= record_cap and
            u64(workload["client_queue_bytes"], positive=True) >=
            max(record_cap, dimension * 4 + 1024), "Queue byte cap too small")


def request(row, kind):
    extra = ("query_id", "snapshot_sequence", "view_generation", "k", "neighbors") \
        if kind == "query" else ("operation", "id", "payload", "sequence")
    shape(row, (*COMMON_FIELDS, *extra))
    header(row)
    text(row["operation_id"], 128)
    status = row["status"]
    require(status in ("ok", "error", "rejected"), "Invalid request status")
    scheduled, completed = u64(row["scheduled_ns"]), u64(row["completed_ns"])
    require(scheduled <= completed, "Completion before arrival")
    if status == "rejected":
        require(row["api_start_ns"] is None, "Rejected request entered API")
    else:
        require(scheduled <= u64(row["api_start_ns"]) <= completed, "Invalid API timing")
    error = row["error"]
    if status == "ok":
        require(error is None, "Success includes error")
    else:
        shape(error, ("code", "message", "system_error", "write_effect", "attempted_sequence"))
        require(error["code"] in ERROR_CODES, "Unknown error code")
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
    if kind == "query":
        u64(row["query_id"])
        u32(row["k"], positive=True)
        require(type(row["neighbors"]) is list, "Expected neighbors array")
        if status != "ok":
            require(row["snapshot_sequence"] is None and row["view_generation"] is None and
                    not row["neighbors"], "Failed query fabricates results or snapshot")
            require(error["write_effect"] == "none" and error["attempted_sequence"] is None,
                    "Query has write effect")
        else:
            snapshot = u64(row["snapshot_sequence"])
            u64(row["view_generation"], positive=True)
            require(len(row["neighbors"]) <= row["k"], "Too many neighbors")
            ordering, ids = [], set()
            for neighbor in row["neighbors"]:
                shape(neighbor, ("id", "version", "squared_distance"))
                identifier = u64(neighbor["id"])
                require(identifier not in ids, "Duplicate neighbor ID")
                ids.add(identifier)
                require(u64(neighbor["version"]) <= snapshot, "Future version")
                distance = finite(neighbor["squared_distance"])
                require(distance >= 0, "Negative squared distance")
                ordering.append((distance, identifier))
            require(ordering == sorted(ordering), "Neighbor order mismatch")
    else:
        require(row["operation"] in ("insert", "upsert"), "Invalid write operation")
        u64(row["id"])
        payload = row["payload"]
        shape(payload, ("file", "row", "dimension", "sha256"))
        relative_path(payload["file"])
        u64(payload["row"])
        u32(payload["dimension"], positive=True)
        checksum(payload["sha256"], row["purpose"])
        if status == "ok":
            u64(row["sequence"], positive=True)
        else:
            require(row["sequence"] is None, "Failed write has success receipt")


def sample(row):
    shape(row, ("schema_version", "protocol_version", "purpose", "run_id", "sampled_ns",
                "private_bytes", "working_set_bytes", "kernel_peak_job_commit_bytes",
                "unavailable_reason"))
    header(row)
    u64(row["sampled_ns"])
    missing = False
    for key in ("private_bytes", "working_set_bytes", "kernel_peak_job_commit_bytes"):
        if row[key] is None:
            missing = True
        else:
            u64(row[key])
    if missing:
        text(row["unavailable_reason"])
    else:
        require(row["unavailable_reason"] is None, "Unexpected unavailable reason")


def run_artifacts(queries, writes, counters):
    shape(counters, ("schema_version", "protocol_version", "purpose", "run_id", "duration_ns",
                     "drain_ns", "run_incomplete", "requires_review",
                     "measurement_start_sequence", "measurement_end_sequence",
                     "by_type", "engine_counters"))
    header(counters)
    duration = u64(counters["duration_ns"], positive=True)
    end_ns = duration + u64(counters["drain_ns"])
    require(end_ns <= UINT64_MAX, "End time overflow")
    boolean(counters["run_incomplete"])
    boolean(counters["requires_review"])
    start = u64(counters["measurement_start_sequence"])
    end = u64(counters["measurement_end_sequence"])
    require(start <= end, "Sequence regression")
    shape(counters["by_type"], ("query", "insert", "upsert"))
    shape(counters["engine_counters"], ENGINE_COUNTERS)
    for value in counters["engine_counters"].values():
        u64(value)
    observed = {kind: dict.fromkeys(COUNT_FIELDS, 0) for kind in counters["by_type"]}
    operation_ids, sequences, query_rows = set(), [], []
    uncertain = False
    for row, kind in [(row, "query") for row in queries] + [
            (row, row["operation"]) for row in writes]:
        request(row, kind)
        require(all(row[key] == counters[key] for key in
                    ("run_id", "purpose", "protocol_version")), "Mixed run artifacts")
        require(row["operation_id"] not in operation_ids, "Duplicate operation ID")
        operation_ids.add(row["operation_id"])
        require(u64(row["scheduled_ns"]) < duration, "Arrival outside measurement window")
        require(u64(row["completed_ns"]) <= end_ns, "Completion beyond drain interval")
        counts = observed[kind]
        counts["scheduled"] += 1
        counts[{"ok": "succeeded", "error": "failed", "rejected": "rejected"}[row["status"]]] += 1
        if u64(row["completed_ns"]) < duration:
            counts["window_completed"] += 1
            counts["window_succeeded"] += row["status"] == "ok"
        if kind != "query":
            if row["status"] == "ok":
                sequence = u64(row["sequence"])
                require(start < sequence <= end, "Receipt outside measurement sequence interval")
                sequences.append(sequence)
            else:
                uncertain |= row["error"]["write_effect"] == "may_have_applied"
        elif row["status"] == "ok":
            require(start <= u64(row["snapshot_sequence"]) <= end, "Snapshot outside run")
            query_rows.append(row)
    require(not uncertain or counters["requires_review"], "Uncertain write hidden")
    require(len(set(sequences)) == len(sequences), "Duplicate success sequence")
    for kind, counts in counters["by_type"].items():
        shape(counts, COUNT_FIELDS)
        numbers = {key: u64(value) for key, value in counts.items()}
        require(numbers["scheduled"] == sum(numbers[key] for key in
                ("succeeded", "failed", "rejected", "pending")), "Request conservation failed")
        for key in COUNT_FIELDS:
            if key not in ("pending", "scheduled"):
                require(numbers[key] == observed[kind][key], f"Counter mismatch: {kind}.{key}")
        require(numbers["scheduled"] == observed[kind]["scheduled"] + numbers["pending"],
                "Missing terminal records")
        require(not numbers["pending"] or counters["run_incomplete"], "Pending requests hidden")
    if not counters["run_incomplete"] and not counters["requires_review"]:
        ordered = sorted(sequences)
        require(len(ordered) == end - start and
                all(sequence == start + offset + 1 for offset, sequence in enumerate(ordered)),
                "Success write sequence prefix missing")
        for query in query_rows:
            snapshot = u64(query["snapshot_sequence"])
            for write in writes:
                if write["status"] != "ok":
                    continue
                sequence = u64(write["sequence"])
                if u64(write["completed_ns"]) < u64(query["api_start_ns"]):
                    require(sequence <= snapshot, "Query misses acknowledged write")
                if u64(write["api_start_ns"]) > u64(query["completed_ns"]):
                    require(sequence > snapshot, "Query sees future write")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def parse_json(text_value):
    def parse_float(value):
        return finite(float(value))

    def reject_constant(value):
        raise ContractError(f"Nonfinite JSON constant: {value}")

    return json.loads(text_value, object_pairs_hook=unique_object,
                      parse_float=parse_float, parse_constant=reject_constant)


def load(path):
    return parse_json(path.read_text(encoding="utf-8"))


def load_rows(path):
    return [parse_json(line) for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip()]


def check_examples():
    config = load(ROOT / "configs/examples/static-run.json")
    run_config(config)
    modes = load(ROOT / "configs/examples/resource-modes.json")
    shape(modes, ("schema_version", "purpose", "memory", "rebuild_memory", "resources"))
    require(type(modes["schema_version"]) is int and modes["schema_version"] == 1 and
            modes["purpose"] == "contract_example", "Invalid resource example")
    resource_modes(modes)
    plan = load(ROOT / "configs/stage2-capacity.json")
    for mode, plan_key in (("memory", "query_memory_bytes"),
                           ("rebuild_memory", "rebuild_memory_bytes")):
        budget = memory(modes[mode])
        require(budget["total_bytes"] == plan["total_memory_bytes"], "Stage-two total drift")
        for region, value in plan[plan_key].items():
            require(budget[region + "_bytes"] == value, "Stage-two partition drift")
        minimum_delta = plan["max_retained_delta_vectors"] * (
            plan["dimension"] * plan["element_bytes"] + plan["delta_record_overhead_bytes"])
        require(minimum_delta <= budget["delta_bytes"], "Delta estimate cannot fit")
    resources = modes["resources"]
    for key in ("max_disk_segments", "max_retained_generations", "max_retained_delta_vectors"):
        require(resources[key] == plan[key], "Stage-two retained resource drift")
    require(u64(resources["background_workspace_bytes"]) ==
            plan["background_build_workspace_cap_bytes"] and
            u64(resources["min_free_disk_bytes"]) == plan["required_free_disk_bytes"],
            "Stage-two maintenance reservation drift")
    queries = load_rows(ROOT / "evaluation/examples/queries.jsonl")
    answers = load_rows(ROOT / "evaluation/examples/ground-truth.jsonl")
    require(len(queries) == len(answers), "Static answer count")
    for query, answer in zip(queries, answers):
        request(query, "query")
        shape(answer, ("schema_version", "protocol_version", "purpose", "operation_id",
                       "query_id", "snapshot_sequence", "live_vectors", "metric", "top10_ids"))
        require(type(answer["schema_version"]) is int and answer["schema_version"] == 1 and
                answer["protocol_version"] == "EVAL-1.1" and
                answer["purpose"] == "contract_example" and
                answer["metric"] == "squared_l2", "Static answer version")
        u64(answer["live_vectors"])
        require(all(query[key] == answer[key] for key in
                    ("operation_id", "query_id", "snapshot_sequence")), "Static answer join")
        require([neighbor["id"] for neighbor in query["neighbors"]] == answer["top10_ids"],
                "Static contract answer IDs")
    for name in ("normal", "uncertain"):
        directory = ROOT / "evaluation/examples/stage3-a" / name
        counters = load(directory / "counters.json")
        run_artifacts(load_rows(directory / "queries.jsonl"),
                      load_rows(directory / "writes.jsonl"), counters)
        samples_path = directory / "samples.jsonl"
        if samples_path.exists():
            previous = -1
            for row in load_rows(samples_path):
                sample(row)
                require(row["run_id"] == counters["run_id"] and
                        row["purpose"] == counters["purpose"], "Mixed resource samples")
                current = u64(row["sampled_ns"])
                require(previous <= current <=
                        u64(counters["duration_ns"]) + u64(counters["drain_ns"]),
                        "Resource sample time mismatch")
                previous = current
    print("PASS: static config, resource modes, static answer join, normal/uncertain outputs")


if __name__ == "__main__":
    try:
        check_examples()
    except (ContractError, OSError, ValueError, KeyError, TypeError) as error:
        print(f"Stage-three A contract check failed: {error}", file=sys.stderr)
        sys.exit(1)
