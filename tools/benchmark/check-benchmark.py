"""Verify the native benchmark producer, never retrieval quality or performance."""

import argparse
from copy import deepcopy
from datetime import datetime
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "producer_contract", ROOT / "tools/contracts/check-stage3-a.py")
contract = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(contract)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path):
    return contract.load(path)


def check_artifacts(directory):
    effective = load(directory / "effective-config.json")
    contract.run_config(effective)
    queries = contract.load_rows(directory / "queries.jsonl")
    writes = contract.load_rows(directory / "writes.jsonl")
    counters = load(directory / "counters.json")
    contract.run_artifacts(queries, writes, counters)
    samples = contract.load_rows(directory / "samples.jsonl")
    previous = -1
    for row in samples:
        contract.sample(row)
        sampled = int(row["sampled_ns"])
        require(previous <= sampled <= int(counters["duration_ns"]) + int(counters["drain_ns"]),
                "Sample outside measurement and drain interval")
        require(all(row[key] == counters[key] for key in ("run_id", "purpose", "protocol_version")),
                "Sample belongs to another run")
        previous = sampled
    environment = load(directory / "environment.json")
    require(environment["target"] == "contract-test"
            and environment["formal_engine_measurement"] is False
            and environment["input_content_used"] is False
            and environment["input_hashes_verified"] is False,
            "Synthetic target concealed its scope")
    summary = load(directory / "summary.json")
    require(int(summary["query_log_bytes"]) == (directory / "queries.jsonl").stat().st_size,
            "Query log byte counter mismatch")
    require(int(summary["sample_log_bytes"]) == (directory / "samples.jsonl").stat().st_size,
            "Sample log byte counter mismatch")
    counts = counters["by_type"]["query"]
    duration = int(counters["duration_ns"])
    require(math.isclose(summary["window_completed_qps"],
                         int(counts["window_succeeded"]) * 1e9 / duration),
            "Window QPS denominator or numerator mismatch")
    require(math.isclose(summary["drained_qps"], int(counts["succeeded"]) * 1e9 /
                         (duration + int(counters["drain_ns"]))),
            "Drained QPS denominator or numerator mismatch")
    for status in ("ok", "error", "rejected"):
        selected = [row for row in queries if row["status"] == status]
        histograms = summary["latencies"][status]
        require(int(histograms["end_to_end"]["count"]) == len(selected),
                "Status latency count mismatch")
        for kind in ("end_to_end", "queue", "service"):
            values = []
            for row in selected:
                if kind != "end_to_end" and row["api_start_ns"] is None:
                    continue
                if kind == "end_to_end":
                    value = int(row["completed_ns"]) - int(row["scheduled_ns"])
                elif kind == "queue":
                    value = int(row["api_start_ns"]) - int(row["scheduled_ns"])
                else:
                    value = int(row["completed_ns"]) - int(row["api_start_ns"])
                values.append(value)
            histogram = histograms[kind]
            bins = [0] * 65
            for value in values:
                bins[value.bit_length()] += 1
            require([int(value) for value in histogram["power_of_two_ns_bins"]] == bins,
                    "C++ latency histogram differs from raw timings")
            require(int(histogram["count"]) == len(values), "Histogram count mismatch")
            require(histogram["min_ns"] == (str(min(values)) if values else None)
                    and histogram["max_ns"] == (str(max(values)) if values else None),
                    "Latency bounds mismatch")
    errors = {code: 0 for code in contract.ERROR_CODES}
    for row in queries:
        if row["status"] == "error":
            errors[row["error"]["code"]] += 1
    require(summary["errors_by_code"] == {code: str(count) for code, count in errors.items()},
            "Error classification differs from raw records")
    timeout_rows = [row for row in queries if row["status"] == "error"
                    and row["error"]["code"] == "deadline_exceeded"]
    for kind in ("end_to_end", "queue", "service"):
        values = []
        for row in timeout_rows:
            first = row["api_start_ns"] if kind == "service" else row["scheduled_ns"]
            last = row["api_start_ns"] if kind == "queue" else row["completed_ns"]
            values.append(int(last) - int(first))
        bins = [0] * 65
        for value in values:
            bins[value.bit_length()] += 1
        histogram = summary["api_timeouts"][kind]
        require(int(histogram["count"]) == len(values)
                and [int(value) for value in histogram["power_of_two_ns_bins"]] == bins,
                "API timeout latency distribution differs from raw timings")
    require((directory / "stdout.log").is_file() and (directory / "stderr.log").is_file(),
            "Process logs missing")
    return {"counts": counts, "exit_code": summary["exit_code"],
            "resources": summary["resources"],
            "window_completed_qps": summary["window_completed_qps"],
            "drained_qps": summary["drained_qps"],
            "job_limit_commit_bytes": environment["job_limit_commit_bytes"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/windows/Release/vector_bench.exe")
    parser.add_argument("--job-runner", type=Path, default=ROOT / "build/windows/Release/job_runner.exe")
    output = parser.add_mutually_exclusive_group(required=True)
    output.add_argument("--output", type=Path)
    output.add_argument("--output-parent", type=Path)
    args = parser.parse_args()
    if args.output_parent:
        args.output_parent.mkdir(parents=True, exist_ok=True)
        directory = Path(tempfile.mkdtemp(prefix="producer-", dir=args.output_parent)).resolve()
    else:
        directory = args.output.resolve()
        directory.mkdir(parents=True, exist_ok=False)
    binary, job_runner = args.binary.resolve(), args.job_runner.resolve()
    template = load(ROOT / "configs/examples/benchmark-contract-test.json")
    outcomes = {}

    def run_case(name, config, flags=(), expected=0, target="contract-test"):
        case = directory / name
        case.mkdir()
        config_path = case / "input.json"
        config_path.write_text(json.dumps(config, ensure_ascii=True), encoding="utf-8")
        run = case / "run"
        metrics = case / "job-memory.json"
        command = [str(job_runner), "128", "15", str(metrics), str(binary),
                   "--config", str(config_path), "--output", str(run),
                   "--target", target, "--job-metrics", str(metrics), *flags]
        with (case / "process.log").open("w", encoding="utf-8") as log:
            completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=20)
        memory = load(metrics)
        require(memory["exit_code"] == expected and not memory["timed_out"],
                f"{name}: expected child exit {expected}, got {memory}")
        require(completed.returncode == (0 if expected == 0 else 1),
                f"{name}: Job launcher concealed child failure")
        if expected in (0, 4) and (run / "counters.json").exists():
            result = check_artifacts(run)
            require(result["exit_code"] == expected, f"{name}: wrong report exit code")
            require(result["job_limit_commit_bytes"] == str(128 * 1024 * 1024),
                    f"{name}: inherited Job limit was not measured")
            result["job_memory"] = memory
            outcomes[name] = result
        else:
            outcomes[name] = {"expected_exit_code": expected}
        return run, memory

    for name in ("normal", "repeat"):
        config = deepcopy(template)
        run_case(name, config, ("--test-delay-ms", "2"))
        require(outcomes[name]["counts"]["scheduled"] == "20"
                and outcomes[name]["counts"]["succeeded"] == "20",
                "Normal run missing measured requests")
    normal_rows = contract.load_rows(directory / "normal/run/queries.jsonl")
    repeat_rows = contract.load_rows(directory / "repeat/run/queries.jsonl")
    stable = lambda rows: sorted((row["operation_id"], row["query_id"], row["neighbors"])
                                 for row in rows)
    require(stable(normal_rows) == stable(repeat_rows),
            "Repeated config changed query mapping or synthetic answers")

    config = deepcopy(template)
    config["measurement"]["warmup_ms"] = "0"
    run_case("api-errors", config, ("--test-error-every", "3"))
    require(outcomes["api-errors"]["counts"]["failed"] == "6", "API errors were omitted")

    config = deepcopy(template)
    config["measurement"].update(warmup_ms="0", duration_ms="100")
    config["search"]["timeout_ms"] = "1"
    run_case("api-timeouts", config, ("--test-delay-ms", "50"))
    require(outcomes["api-timeouts"]["counts"]["failed"] == "10",
            "API timeouts were omitted")

    config = deepcopy(template)
    config["measurement"].update(warmup_ms="0", duration_ms="300")
    config["workload"].update(workers=1, client_queue_capacity=1, target_ops_per_second=20)
    run_case("client-overload", config, ("--test-delay-ms", "160"))
    require(int(outcomes["client-overload"]["counts"]["rejected"]) > 0,
            "Overload did not produce explicit client rejections")

    config = deepcopy(template)
    config["measurement"].update(warmup_ms="0", duration_ms="20", drain_timeout_ms="10")
    config["workload"].update(workers=1, target_ops_per_second=1000)
    run_case("drain-timeout", config, ("--test-delay-ms", "100"), expected=4)
    require(int(outcomes["drain-timeout"]["counts"]["pending"]) > 0,
            "Drain timeout hid pending requests")

    config = deepcopy(template)
    run_case("engine-unavailable", config, expected=3, target="engine")
    config["purpose"] = "measurement"
    config["dataset"]["sha256"] = dict.fromkeys(config["dataset"]["sha256"], "0" * 64)
    run_case("measurement-refused", config, expected=3)

    cases = []
    config = deepcopy(template)
    config["unknown"] = True
    cases.append(("unknown-field", config))
    config = deepcopy(template)
    config["workload"]["workers"] = True
    cases.append(("boolean-integer", config))
    config = deepcopy(template)
    config["workload"]["seed"] = "01"
    cases.append(("noncanonical-u64", config))
    config = deepcopy(template)
    config["measurement"]["result_queue_capacity"] = config["workload"]["workers"]
    cases.append(("missing-rejection-slot", config))
    config = deepcopy(template)
    config["measurement"]["max_record_bytes"] = 1024
    cases.append(("undersized-record", config))
    for name, config in cases:
        run_case(name, config, expected=2)

    raw = json.dumps(template, separators=(",", ":"))
    malformed = {
        "duplicate-key": raw.replace('"schema_version":1', '"schema_version":1,"schema_version":1'),
        "escaped-duplicate": raw.replace('"schema_version":1',
                                        '"schema_version":1,"schema_versio\\u006e":1'),
        "nested-duplicate": raw.replace('"workers":3', '"workers":3,"workers":3'),
        "nan": raw.replace('"target_ops_per_second":100', '"target_ops_per_second":NaN'),
        "infinity": raw.replace('"target_ops_per_second":100', '"target_ops_per_second":1e999'),
        "trailing-content": raw + "{}",
    }
    for name, text in malformed.items():
        path = directory / f"{name}.json"
        path.write_text(text, encoding="utf-8")
        completed = subprocess.run([str(binary), "--config", str(path),
                                    "--output", str(directory / f"{name}-run"),
                                    "--target", "contract-test"], capture_output=True, timeout=5)
        require(completed.returncode == 2, f"{name}: malformed config was accepted")
        outcomes[name] = {"expected_exit_code": 2}

    existing = directory / "normal/run"
    original = sha256(existing / "counters.json")
    completed = subprocess.run([str(binary), "--config",
                                str(ROOT / "configs/examples/benchmark-contract-test.json"),
                                "--output", str(existing), "--target", "contract-test"],
                               capture_output=True, timeout=5)
    require(completed.returncode == 2 and original == sha256(existing / "counters.json"),
            "Existing output was overwritten")
    outcomes["existing-output-protected"] = {"expected_exit_code": 2}

    source_paths = (
        "CMakeLists.txt", "include/agent_io/benchmark.hpp",
        "include/agent_io/benchmark_runner.hpp", "src/benchmark/runner.cpp",
        "tools/benchmark/main.cpp", "tools/benchmark/config.hpp",
        "tools/benchmark/config.cpp", "tools/benchmark/boost-json.cpp",
        "tests/benchmark/benchmark-runner-test.cpp", "tools/benchmark/check-benchmark.py",
        "configs/examples/benchmark-contract-test.json",
    )
    summary = {
        "schema_version": 1, "purpose": "stage4_a_benchmark_producer_only",
        "checked_at": datetime.now().astimezone().isoformat(),
        "output_directory": str(directory), "cases": outcomes,
        "source_sha256": {name: sha256(ROOT / name) for name in source_paths},
        "binary_sha256": {binary.name: sha256(binary), job_runner.name: sha256(job_runner)},
        "limitations": [
            "Synthetic target validates the producer, not retrieval, recall or Engine integration.",
            "The existing EVAL-1.1 checker validates actual emitted query/counter/sample artifacts.",
            "Job commit limits are not an official physical-memory compliance result.",
        ],
    }
    (directory / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"cases_passed": len(outcomes), "output_directory": str(directory)},
                     ensure_ascii=True, indent=2))


if __name__ == "__main__":
    main()
