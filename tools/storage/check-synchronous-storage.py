"""Small-data integration check for A's storage, not the formal EVAL runner."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import numpy as np

ROOT = Path(__file__).resolve().parents[2]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def vectors(path):
    with path.open("rb") as stream:
        header = np.fromfile(stream, dtype="<u4", count=2)
        if len(header) != 2 or not all(header):
            raise ValueError(f"Invalid vector header: {path}")
        count, dimension = map(int, header)
        if path.stat().st_size != 8 + count * dimension * 4:
            raise ValueError(f"Vector file size mismatch: {path}")
        data = np.fromfile(stream, dtype="<f4").reshape(count, dimension)
        if not np.isfinite(data).all():
            raise ValueError(f"Nonfinite vectors: {path}")
        return data.astype(np.float64)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--summary", type=Path)
    arguments = parser.parse_args()
    fixture = arguments.fixture.resolve()
    output = arguments.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    binaries = ROOT / "build/windows/Release"
    probe = binaries / "diskann_probe.exe"
    job = binaries / "job_runner.exe"
    base = vectors(fixture / "base.bin")
    queries = vectors(fixture / "queries.bin")
    if base.shape[1] != queries.shape[1] or len(base) < 10:
        raise ValueError("Incompatible smoke fixture")
    truth = []
    for query in queries:
        distances = np.sum((base - query) ** 2, axis=1)
        truth.append(np.lexsort((np.arange(len(base)), distances))[:10].tolist())
    recalls = []
    memories = []
    for iteration in range(2):
        results = output / f"results-{iteration}.jsonl"
        memory = output / f"memory-{iteration}.json"
        with (output / f"query-{iteration}.log").open("w", encoding="utf-8") as log:
            subprocess.run([str(job), "128", "30", str(memory), str(probe),
                            str(fixture / "index"), str(fixture / "queries.bin"),
                            str(results)], stdout=log, stderr=subprocess.STDOUT,
                           timeout=40, check=True)
        rows = [json.loads(line) for line in results.read_text().splitlines()]
        if len(rows) != len(queries):
            raise ValueError("Incomplete result file")
        total = 0
        for index, row in enumerate(rows):
            ids = row["ids"]
            if row["query_id"] != index or len(ids) != 10 or len(set(ids)) != 10:
                raise ValueError("Invalid query result")
            if any(type(identifier) is not int or not 0 <= identifier < len(base)
                   for identifier in ids):
                raise ValueError("Invalid result ID")
            total += len(set(ids) & set(truth[index]))
        recall = total / (10 * len(queries))
        if recall < 0.85:
            raise ValueError(f"Recall smoke gate failed: {recall}")
        recalls.append(recall)
        memories.append(json.loads(memory.read_text()))
        log_text = (output / f"query-{iteration}.log").read_text()
        if "storage_implementation=agent_io_storage" not in log_text:
            raise ValueError("Probe did not identify the implemented storage path")
    if (output / "results-0.jsonl").read_bytes() != (output / "results-1.jsonl").read_bytes():
        raise ValueError("Repeated loads changed results")
    with (output / "injected-failure.log").open("w", encoding="utf-8") as log:
        failure = subprocess.run([str(job), "128", "30", str(output / "failure-memory.json"),
                                  str(probe), str(fixture / "index"),
                                  str(fixture / "queries.bin"),
                                  str(output / "failure.jsonl"), "fail"],
                                 stdout=log, stderr=subprocess.STDOUT, timeout=40)
    if failure.returncode == 0 or "Injected reader submission failure" not in (
            output / "injected-failure.log").read_text():
        raise ValueError("Injected adapter failure was hidden")
    source_paths = [
        "CMakeLists.txt", "include/agent_io/synchronous_storage.hpp",
        "src/storage/synchronous_storage.cpp", "src/storage/synchronous_block_reader.cpp",
        "tests/storage/synchronous-storage-test.cpp",
        "tools/environment/diskann-probe.cpp", "tools/storage/check-synchronous-storage.py",
    ]
    summary = {
        "schema_version": 1,
        "purpose": "stage4_a_storage_integration_only",
        "fixture_directory": str(fixture),
        "output_directory": str(output),
        "vector_count": len(base), "dimension": base.shape[1], "query_count": len(queries),
        "recall_at_10": recalls, "reload_identical": True,
        "adapter_failure_propagated": True,
        "prior_probe_results_identical": (
            (fixture / "results-0.jsonl").read_bytes() ==
            (output / "results-0.jsonl").read_bytes()
        ) if (fixture / "results-0.jsonl").exists() else None,
        "memory": memories,
        "input_sha256": {name: sha256(fixture / name)
                         for name in ("base.bin", "queries.bin", "index_disk.index")},
        "source_sha256": {name: sha256(ROOT / name) for name in source_paths},
        "binary_sha256": {path.name: sha256(path) for path in (probe, job)},
        "artifact_sha256": {path.name: sha256(path) for path in output.iterdir()
                            if path.is_file()},
        "limits": [
            "Synthetic small-data storage integration, not formal Engine/EVAL acceptance.",
            "PQ and upstream scratch allocations are outside this adapter's internal governor.",
            "Job commit and sampled working set do not prove the official physical-memory rule.",
            "Adapter failure injection is separate from the C++ test's real ReadFile failure.",
        ],
    }
    text = json.dumps(summary, ensure_ascii=True, indent=2) + "\n"
    (output / "summary.json").write_text(text, encoding="utf-8")
    if arguments.summary:
        arguments.summary.parent.mkdir(parents=True, exist_ok=True)
        if arguments.summary.exists():
            raise ValueError("Summary target already exists")
        arguments.summary.write_text(text, encoding="utf-8")
    print(json.dumps({key: summary[key] for key in (
        "recall_at_10", "reload_identical", "adapter_failure_propagated",
        "prior_probe_results_identical", "output_directory")}, indent=2))


if __name__ == "__main__":
    main()
