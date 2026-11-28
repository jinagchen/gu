"""Check B-side fixtures: build config, dynamic config, trace, answers, checker.

Runs the positive fixture through evaluation.check and then mutates copies to
prove the checker rejects each class of bad input. Negatives are built under
build/contracts-tmp so nothing outside the repository is touched.
"""

import json
import math
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from evaluation import check  # noqa: E402

FIXTURE = ROOT / "evaluation/examples/stage3-b"
SCRATCH = ROOT / "build/contracts-tmp"


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


def positive(value, name, maximum=None):
    require(type(value) is int and not isinstance(value, bool) and value > 0,
            f"{name} must be a positive integer")
    if maximum is not None:
        require(value <= maximum, f"{name} out of range")
    return value


def ascii_path(value, name):
    check.text(value, 1024)
    require(value.isascii(), f"{name} must be ASCII for the native loader")
    return value


def u64_string(value, name):
    return check.u64(value, positive=True)


def validate_build_config(value, config_path=None):
    shape(value, ("schema_version", "protocol_version", "purpose", "build_id", "dataset",
                  "native", "limits", "staging", "output", "notes"), " (build-run)")
    require(value["schema_version"] == 1 and value["protocol_version"] == check.PROTOCOL,
            "Unsupported build config version")
    require(value["purpose"] == "contract_example", "Build fixture must declare its purpose")
    check.text(value["build_id"], 128)

    dataset = value["dataset"]
    shape(dataset, ("format", "vectors", "ids", "vector_count", "dimension", "metric",
                    "sha256"), " (build dataset)")
    require(dataset["format"] == "raw-f32le-v1" and dataset["metric"] == "squared_l2",
            "Unsupported build dataset")
    ascii_path(dataset["vectors"], "vectors path")
    ascii_path(dataset["ids"], "ids path")
    u64_string(dataset["vector_count"], "vector_count")
    positive(dataset["dimension"], "dimension", 65536)
    shape(dataset["sha256"], ("vectors", "ids"), " (build sha256)")
    for key in ("vectors", "ids"):
        check.checksum(dataset["sha256"][key], value["purpose"])

    native = value["native"]
    shape(native, ("dependency", "version", "native_version", "max_degree", "build_beam",
                   "pq_bytes", "parameters"), " (native)")
    check.text(native["dependency"], 64)
    check.text(native["version"], 128)
    require(native["native_version"] is None or
            type(native["native_version"]) is str, "native_version must be null or string")
    positive(native["max_degree"], "max_degree", 512)
    positive(native["build_beam"], "build_beam", 256)
    positive(native["pq_bytes"], "pq_bytes", 1024)
    check.text(native["parameters"], 512)
    require(native["build_beam"] <= native["max_degree"],
            "build_beam exceeds max_degree; the upstream build would reject it")

    limits = value["limits"]
    shape(limits, ("memory_limit_bytes", "min_free_disk_bytes", "job_object",
                   "measurement_scope", "max_peak_commit_bytes"), " (limits)")
    memory_limit = u64_string(limits["memory_limit_bytes"], "memory_limit_bytes")
    free_disk = u64_string(limits["min_free_disk_bytes"], "min_free_disk_bytes")
    peak = u64_string(limits["max_peak_commit_bytes"], "max_peak_commit_bytes")
    check.boolean(limits["job_object"])
    require(limits["job_object"], "Build must run inside a Job object")
    require(limits["measurement_scope"] == "build_process_tree",
            "Build peak must be measured over the whole process tree")
    require(peak <= memory_limit, "Declared build peak exceeds its own limit")
    require(free_disk >= (1 << 30), "Free-disk threshold is implausibly small")

    staging = value["staging"]
    shape(staging, ("directory", "same_volume", "reuse_allowed"), " (staging)")
    ascii_path(staging["directory"], "staging directory")
    check.boolean(staging["same_volume"])
    check.boolean(staging["reuse_allowed"])
    require(staging["same_volume"], "Staging must be on the same volume as the index")
    require(not staging["reuse_allowed"], "Staging must be unique per attempt")

    output = value["output"]
    shape(output, ("directory", "checkpoint_sequence", "generation", "publish_current"),
          " (output)")
    ascii_path(output["directory"], "output directory")
    require(check.u64(output["checkpoint_sequence"]) == 0,
            "First build must publish checkpoint_sequence 0")
    require(u64_string(output["generation"], "generation") == 1,
            "First build starts at manifest generation 1")
    check.boolean(output["publish_current"])
    require(output["publish_current"], "First build must publish CURRENT")

    require(type(value["notes"]) is list and value["notes"], "Build config needs notes")
    for note in value["notes"]:
        check.text(note, 512)

    if config_path is not None:
        validate_layout(config_path, staging["directory"], output["directory"])


def resolve(config_path, value, name):
    ascii_path(value, name)
    return (config_path.parent / value).resolve()


def validate_layout(config_path, staging_dir, output_dir):
    staging = resolve(config_path, staging_dir, "staging directory")
    output = resolve(config_path, output_dir, "output directory")
    require(staging != output, "Staging and output directories must differ")
    require(staging not in output.parents and output not in staging.parents,
            "Staging and output directories must not contain each other")
    require(staging.drive.upper() == output.drive.upper(),
            "Staging must be on the same volume as the index output")
    return staging, output


def validate_dynamic_config(value, config_path=None):
    shape(value, ("schema_version", "protocol_version", "purpose", "run_id", "index",
                  "dataset", "trace", "engine", "search", "workload", "measurement",
                  "notes"), " (dynamic-run)")
    require(value["schema_version"] == 1 and value["protocol_version"] == check.PROTOCOL,
            "Unsupported dynamic config version")
    require(value["purpose"] == "contract_example", "Dynamic fixture must declare its purpose")
    check.text(value["run_id"], 128)

    index = value["index"]
    shape(index, ("directory", "build_id", "manifest_generation"), " (index)")
    ascii_path(index["directory"], "index directory")
    check.text(index["build_id"], 128)
    u64_string(index["manifest_generation"], "manifest_generation")

    dataset = value["dataset"]
    shape(dataset, ("format", "vectors", "ids", "queries", "query_ids", "updates",
                    "vector_count", "query_count", "dimension", "metric", "sha256"),
          " (dataset)")
    require(dataset["format"] == "raw-f32le-v1" and dataset["metric"] == "squared_l2",
            "Unsupported dataset")
    for key in ("vectors", "ids", "queries", "query_ids", "updates"):
        ascii_path(dataset[key], f"{key} path")
    dimension = positive(dataset["dimension"], "dimension", 65536)
    u64_string(dataset["vector_count"], "vector_count")
    u64_string(dataset["query_count"], "query_count")
    shape(dataset["sha256"], ("vectors", "ids", "queries", "query_ids", "updates"),
          " (sha256)")
    for key in dataset["sha256"]:
        check.checksum(dataset["sha256"][key], value["purpose"])

    trace = value["trace"]
    shape(trace, ("file", "sha256", "operations", "queries", "seed", "update_distribution",
                  "snapshot_policy"), " (trace)")
    ascii_path(trace["file"], "trace path")
    check.checksum(trace["sha256"], value["purpose"])
    u64_string(trace["operations"], "trace operations")
    u64_string(trace["queries"], "trace queries")
    u64_string(trace["seed"], "trace seed")
    require(trace["update_distribution"] in ("uniform_existing_and_new", "zipf_existing"),
            "Unregistered update distribution")
    policy = trace["snapshot_policy"]
    shape(policy, ("mode", "min_distinct_snapshots", "max_queries_per_snapshot"),
          " (snapshot_policy)")
    require(policy["mode"] == "committed_prefix", "Unsupported snapshot policy")
    positive(policy["min_distinct_snapshots"], "min_distinct_snapshots", 1024)
    positive(policy["max_queries_per_snapshot"], "max_queries_per_snapshot", 1024)

    engine = value["engine"]
    shape(engine, ("directory", "io_backend", "direct_reads", "cache_policy", "block_bytes",
                   "memory", "rebuild_memory", "resources", "read_scheduling", "wal",
                   "max_queries", "max_pending_writes", "max_inflight_reads",
                   "max_prefetch_reads", "max_delta_segments", "group_commit_window_ms"),
          " (engine)")
    ascii_path(engine["directory"], "engine directory")
    require(engine["io_backend"] in ("synchronous", "windows_iocp"), "Invalid backend")
    require(engine["cache_policy"] in ("disabled", "clock"), "Invalid cache policy")
    check.boolean(engine["direct_reads"])
    positive(engine["block_bytes"], "block_bytes", 1 << 20)

    memory = engine["memory"]
    partitions = ("total_bytes", "cache_bytes", "delta_bytes", "metadata_bytes",
                  "workspace_bytes", "reserve_bytes")
    shape(memory, partitions, " (memory)")
    numbers = {key: check.u64(memory[key], positive=key != "cache_bytes") for key in partitions}
    require(sum(numbers[key] for key in partitions[1:]) == numbers["total_bytes"],
            "Memory partitions do not sum to the total")

    resources = engine["resources"]
    shape(resources, ("max_disk_segments", "max_retained_generations",
                      "max_retained_delta_vectors", "background_workspace_bytes",
                      "min_free_disk_bytes"), " (resources)")
    for key in ("max_disk_segments", "max_retained_generations",
                "max_retained_delta_vectors"):
        positive(resources[key], key, 1 << 20)
    require(resources["max_retained_generations"] >= 2, "Need two retained generations")
    u64_string(resources["background_workspace_bytes"], "background_workspace_bytes")
    u64_string(resources["min_free_disk_bytes"], "min_free_disk_bytes")
    retained = resources["max_retained_delta_vectors"] * (dimension * 4 + 32)
    require(retained <= numbers["delta_bytes"], "Retained delta payload exceeds the partition")

    scheduling = engine["read_scheduling"]
    shape(scheduling, ("demand_reserve", "background_reserve", "max_background_reads",
                       "demand_burst_before_background"), " (read_scheduling)")
    for key in scheduling:
        positive(scheduling[key], key, 1 << 16)
    for key in ("max_queries", "max_pending_writes", "max_inflight_reads",
                "max_delta_segments"):
        positive(engine[key], key, 1 << 20)
    available = engine["max_inflight_reads"] - scheduling["demand_reserve"]
    require(scheduling["background_reserve"] <= scheduling["max_background_reads"] <= available,
            "Invalid background reservation")
    require(u64_string(engine["group_commit_window_ms"], "group_commit_window_ms") <=
            (1 << 63) - 1, "chrono overflow")

    wal = engine["wal"]
    shape(wal, ("rotation_bytes", "max_frame_bytes"), " (wal)")
    frame_cap = u64_string(wal["max_frame_bytes"], "max_frame_bytes")
    frame = (64 + dimension * 4 + 4 + 7) // 8 * 8
    require(frame <= frame_cap, "WAL frame cannot fit the configured dimension")
    require(u64_string(wal["rotation_bytes"], "rotation_bytes") >= frame_cap + 64,
            "WAL rotation cannot fit the maximum frame")

    search = value["search"]
    shape(search, ("k", "candidate_limit", "prefetch_blocks", "timeout_ms"), " (search)")
    k = positive(search["k"], "k", 4096)
    require(positive(search["candidate_limit"], "candidate_limit", 1 << 20) >= k,
            "Candidate budget below K")
    require(check.u32(search["prefetch_blocks"]) <= engine["max_prefetch_reads"],
            "Query prefetch exceeds the engine cap")
    require(u64_string(search["timeout_ms"], "timeout_ms") <= (1 << 63) - 1, "chrono overflow")

    workload = value["workload"]
    shape(workload, ("mode", "target_ops_per_second", "workers", "client_queue_capacity",
                     "client_queue_bytes", "query_fraction", "insert_fraction",
                     "upsert_fraction", "seed"), " (workload)")
    require(workload["mode"] == "open_loop", "Fixture protocol supports open_loop")
    require(check.finite(workload["target_ops_per_second"]) > 0, "Invalid arrival rate")
    positive(workload["workers"], "workers", 1024)
    positive(workload["client_queue_capacity"], "client_queue_capacity", 1 << 20)
    fractions = [check.finite(workload[key]) for key in
                 ("query_fraction", "insert_fraction", "upsert_fraction")]
    require(all(0 <= item <= 1 for item in fractions) and
            math.isclose(sum(fractions), 1.0, rel_tol=0, abs_tol=1e-12),
            "Dynamic fractions must sum to 1")
    require(fractions[0] > 0 and (fractions[1] + fractions[2]) > 0,
            "Dynamic run must mix queries and writes")
    u64_string(workload["seed"], "workload seed")

    measurement = value["measurement"]
    shape(measurement, ("warmup_ms", "duration_ms", "drain_timeout_ms", "sample_interval_ms",
                        "result_queue_capacity", "result_queue_bytes", "max_record_bytes"),
          " (measurement)")
    positive(measurement["result_queue_capacity"], "result_queue_capacity", 1 << 20)
    for key in ("warmup_ms", "duration_ms", "drain_timeout_ms", "sample_interval_ms"):
        require(check.u64(measurement[key]) <= (1 << 63), "Millisecond overflow")
    record_cap = positive(measurement["max_record_bytes"], "max_record_bytes", 1 << 24)
    require(u64_string(measurement["result_queue_bytes"], "result_queue_bytes") >= record_cap,
            "Result queue byte cap below the record bound")

    require(type(value["notes"]) is list and value["notes"], "Dynamic config needs notes")
    check.validate_engine(engine, dimension)
    check.validate_run_settings(value)

    if config_path is not None:
        require(resolve(config_path, index["directory"], "index directory") ==
                resolve(config_path, engine["directory"], "engine directory"),
                "engine.directory must point at the index directory")


def validate_trace_contract(config, config_path):
    """Cross-check the declared trace metadata against the trace file itself."""
    trace = config["trace"]
    path = resolve(config_path, trace["file"], "trace path")
    require(path.exists(), f"Missing trace file {path}")
    rows = check.trace_rows(path, config)
    require(rows, "Trace is empty")
    declared_operations = int(check.u64(trace["operations"]))
    declared_queries = int(check.u64(trace["queries"]))
    require(declared_operations + declared_queries == len(rows),
            "trace.operations + trace.queries does not match the trace row count")

    writes = [row for row in rows if row["kind"] == "write"]
    queries = [row for row in rows if row["kind"] == "query"]
    require(len(writes) + len(queries) == len(rows), "Trace contains an unregistered kind")
    require(len(writes) == declared_operations,
            "trace.operations counts writes, so it must equal the write rows")
    require(len(queries) == declared_queries, "trace.queries must equal the query rows")

    total = len(rows)
    mix = {
        "query_fraction": len(queries) / total,
        "insert_fraction": sum(1 for row in writes if row["operation"] == "insert") / total,
        "upsert_fraction": sum(1 for row in writes if row["operation"] == "upsert") / total,
    }
    for key, expected in mix.items():
        require(math.isclose(check.finite(config["workload"][key]), expected,
                             rel_tol=0, abs_tol=1e-9),
                f"{key} disagrees with the referenced trace mix")
    return {"operations": len(writes), "queries": len(queries), "rows": total}


def expect_failure(label, mutate):
    scratch = SCRATCH / label
    if scratch.exists():
        shutil.rmtree(scratch)
    shutil.copytree(FIXTURE, scratch)
    mutate(scratch)
    try:
        check.evaluate(scratch)
    except (check.ContractError, OSError, ValueError, KeyError, TypeError):
        return
    raise ContractError(f"Checker accepted a bad run: {label}")


def expect_build_failure(label, mutate):
    value = check.load(ROOT / "configs/examples/build-run.json")
    mutate(value)
    try:
        validate_build_config(value, ROOT / "configs/examples/build-run.json")
    except (ContractError, check.ContractError):
        return
    raise ContractError(f"Accepted a bad build config: {label}")


def expect_dynamic_failure(label, mutate):
    path = ROOT / "configs/examples/dynamic-run.json"
    value = check.load(path)
    mutate(value)
    try:
        validate_dynamic_config(value, path)
        validate_trace_contract(value, path)
    except (ContractError, check.ContractError):
        return
    raise ContractError(f"Accepted a bad dynamic config: {label}")


def mutate_trace(mutator):
    def apply(scratch):
        path = scratch / "trace.jsonl"
        rows = check.load_rows(path)
        mutator(rows)
        rewrite_rows(path, rows)
        config_path = scratch / "effective-config.json"
        config = check.load(config_path)
        config["trace"]["sha256"] = check.digest(path)
        config_path.write_text(json.dumps(config, ensure_ascii=False, separators=(",", ":")) + "\n",
                               encoding="utf-8")
    return apply


def mutate_config(mutator):
    def apply(scratch):
        path = scratch / "effective-config.json"
        config = check.load(path)
        mutator(config)
        path.write_text(json.dumps(config, ensure_ascii=False, separators=(",", ":")) + "\n",
                        encoding="utf-8")
    return apply


def unknown_trace_write(rows):
    for row in rows:
        if row["kind"] == "write":
            row["operation_id"] = row["operation_id"] + "-x"
            return
    raise ContractError("Fixture has no write row to detach")


def trace_arrival_gap(rows):
    rows[1]["arrival_index"] = rows[1]["arrival_index"] + 1


def rewrite_rows(path, rows):
    path.write_text("".join(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n"
                            for row in rows), encoding="utf-8")


def mutate_queries(mutator):
    def apply(scratch):
        path = scratch / "queries.jsonl"
        rows = check.load_rows(path)
        mutator(rows)
        rewrite_rows(path, rows)
    return apply


def mutate_writes(mutator):
    def apply(scratch):
        path = scratch / "writes.jsonl"
        rows = check.load_rows(path)
        mutator(rows)
        rewrite_rows(path, rows)
    return apply


def mutate_counters(mutator):
    def apply(scratch):
        path = scratch / "counters.json"
        value = check.load(path)
        mutator(value)
        path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return apply


def stale_version(rows):
    for row in rows:
        for neighbour in row["neighbors"]:
            if int(neighbour["version"]) > 0:
                neighbour["version"] = "0"
                return
    raise ContractError("Fixture has no updated neighbour to stale-out")


def future_version(rows):
    rows[0]["neighbors"][0]["version"] = str(int(rows[0]["snapshot_sequence"]) + 1)


def duplicate_neighbour(rows):
    rows[0]["neighbors"][1] = dict(rows[0]["neighbors"][0])


def noncanonical_id(rows):
    rows[0]["neighbors"][0]["id"] = "0" + rows[0]["neighbors"][0]["id"]


def invisible_id(rows):
    rows[0]["neighbors"][0]["id"] = "999999"


def distance_drift(rows):
    rows[0]["neighbors"][0]["squared_distance"] = (
        float(rows[0]["neighbors"][0]["squared_distance"]) + 1.0)


def unsorted_neighbours(rows):
    if len(rows[0]["neighbors"]) > 1:
        rows[0]["neighbors"][0], rows[0]["neighbors"][1] = (
            rows[0]["neighbors"][1], rows[0]["neighbors"][0])


def future_snapshot(rows):
    rows[0]["snapshot_sequence"] = str(int(rows[0]["snapshot_sequence"]) + 1)
    rows[0]["view_generation"] = str(int(rows[0]["view_generation"]) + 1)


def old_snapshot(rows):
    rows[0]["snapshot_sequence"] = "0"
    rows[0]["view_generation"] = "1"


def failed_query_with_snapshot(rows):
    row = rows[0]
    row["status"] = "error"
    row["error"] = {"code": "io_error", "message": "injected", "system_error": 0,
                    "write_effect": "none", "attempted_sequence": None}


def sequence_gap(rows):
    rows[-1]["sequence"] = str(int(rows[-1]["sequence"]) + 1)


def insert_conflict(rows):
    for row in rows:
        if row["operation"] == "upsert":
            row["operation"] = "insert"
            return
    raise ContractError("Fixture has no upsert to turn into a conflicting insert")


def broken_conservation(counters):
    counters["by_type"]["query"]["succeeded"] = str(
        int(counters["by_type"]["query"]["succeeded"]) + 1)


def hidden_pending(counters):
    counters["by_type"]["query"]["pending"] = "1"


def tampered_answers(scratch):
    path = scratch / "ground-truth.jsonl"
    rows = check.load_rows(path)
    rows[0]["top_k_ids"][0] = "999999"
    rewrite_rows(path, rows)


def main():
    build_path = ROOT / "configs/examples/build-run.json"
    validate_build_config(check.load(build_path), build_path)
    dynamic_path = ROOT / "configs/examples/dynamic-run.json"
    dynamic = check.load(dynamic_path)
    validate_dynamic_config(dynamic, dynamic_path)
    mix = validate_trace_contract(dynamic, dynamic_path)

    config = check.load_config(FIXTURE)
    result = check.evaluate(FIXTURE, write_report=False)
    require(result["verdict"] == "pass", "Fixture did not pass")
    require(result["checks"]["independent_answer_join"] == "pass",
            "Fixture answers were not cross-checked")
    require(abs(result["recall_at_k"]["mean"] - 1.0) < 1e-12,
            "Exact reference fixture must score 1.0")
    require(result["distance_check"]["max_abs_error"] <= 1e-9,
            "Reference distances must match recomputation exactly")
    require(result["snapshots"]["distinct"] >= 2, "Fixture needs distinct snapshots")
    require(result["writes"]["acknowledged"] > 0, "Fixture needs acknowledged writes")
    require(config["search"]["k"] == result["recall_at_k"]["k"], "K mismatch in the report")

    SCRATCH.mkdir(parents=True, exist_ok=True)
    try:
        negatives = [
            ("stale_version", mutate_queries(stale_version)),
            ("future_version", mutate_queries(future_version)),
            ("duplicate_neighbour", mutate_queries(duplicate_neighbour)),
            ("noncanonical_id", mutate_queries(noncanonical_id)),
            ("invisible_id", mutate_queries(invisible_id)),
            ("distance_drift", mutate_queries(distance_drift)),
            ("unsorted_neighbours", mutate_queries(unsorted_neighbours)),
            ("future_snapshot", mutate_queries(future_snapshot)),
            ("old_snapshot", mutate_queries(old_snapshot)),
            ("failed_query_with_snapshot", mutate_queries(failed_query_with_snapshot)),
            ("sequence_gap", mutate_writes(sequence_gap)),
            ("insert_conflict", mutate_writes(insert_conflict)),
            ("broken_conservation", mutate_counters(broken_conservation)),
            ("hidden_pending", mutate_counters(hidden_pending)),
            ("tampered_answers", tampered_answers),
            ("trace_unknown_write", mutate_trace(unknown_trace_write)),
            ("trace_arrival_gap", mutate_trace(trace_arrival_gap)),
            ("trace_count_mismatch",
             mutate_config(lambda config: config["trace"].__setitem__("operations", "9"))),
            ("trace_checksum_tampered",
             lambda scratch: rewrite_rows(scratch / "trace.jsonl",
                                          check.load_rows(scratch / "trace.jsonl")[::-1])),
            ("query_count_mismatch",
             mutate_config(lambda config: config["dataset"].__setitem__("query_count", "5"))),
        ]
        for label, mutation in negatives:
            expect_failure(label, mutation)
            shutil.rmtree(SCRATCH / label, ignore_errors=True)

        config_negatives = [
            ("build_staging_equals_output",
             lambda value: value["staging"].__setitem__("directory", value["output"]["directory"])),
            ("build_staging_inside_output",
             lambda value: value["staging"].__setitem__(
                 "directory", value["output"]["directory"] + "/staging")),
            ("build_unknown_field",
             lambda value: value["limits"].__setitem__("memory_limit_mb", 512)),
            ("dynamic_unknown_engine_field",
             lambda value: value["engine"].__setitem__("block_size", 4096)),
            ("dynamic_unknown_top_field",
             lambda value: value.__setitem__("workloads", {})),
            ("dynamic_operation_count",
             lambda value: value["trace"].__setitem__("operations", "11")),
            ("dynamic_fraction_mismatch",
             lambda value: value["workload"].update(
                 {"query_fraction": 0.4375, "insert_fraction": 0.1875,
                  "upsert_fraction": 0.375})),
            ("dynamic_run_id_mismatch",
             lambda value: value.__setitem__("run_id", "other-run")),
            ("dynamic_missing_trace",
             lambda value: value["trace"].__setitem__("file", "../data/missing-trace.jsonl")),
            ("dynamic_engine_directory_mismatch",
             lambda value: value["engine"].__setitem__("directory", "../data/other-index")),
        ]
        for label, mutation in config_negatives:
            if label.startswith("build_"):
                expect_build_failure(label, mutation)
            else:
                expect_dynamic_failure(label, mutation)

        total_negatives = len(negatives) + len(config_negatives)
    finally:
        shutil.rmtree(SCRATCH, ignore_errors=True)

    print(f"PASS: build config, dynamic config, trace contract "
          f"({mix['operations']} writes + {mix['queries']} queries of {mix['rows']} rows), "
          f"exact answers (Recall@{result['recall_at_k']['k']}={result['recall_at_k']['mean']:.2f}), "
          f"{total_negatives} rejected negative cases")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ContractError, check.ContractError, OSError, ValueError, KeyError, TypeError) as error:
        print(f"Stage-three B contract check failed: {error}", file=sys.stderr)
        sys.exit(1)
