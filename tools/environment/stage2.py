"""Native Windows feasibility evidence; not the stage-four engine benchmark."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
COMMIT = "4c31367b6c3df5395bf8b786299988a5d001ec5a"


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")


def sha256(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def run(command, log, *, success=True, timeout=240):
    with log.open("wb") as output:
        result = subprocess.run([str(item) for item in command], stdout=output,
                                stderr=subprocess.STDOUT, timeout=timeout, check=False)
    if (result.returncode == 0) != success:
        raise RuntimeError(f"Unexpected exit {result.returncode}: {command}; see {log}")
    return result.returncode


def build_command(binary, base, prefix):
    return [binary, "--data_type", "float", "--dist_fn", "l2",
            "--data_path", base, "--index_path_prefix", prefix,
            "-R", "32", "-L", "64", "-B", "0.0001", "-M", "0.25", "-T", "2"]


def binfile(path, values):
    with path.open("wb") as output:
        output.write(struct.pack("<II", *values.shape))
        output.write(values.astype("<f4", copy=False).tobytes())


def validate_results(path, ground_truth, count):
    rows = [json.loads(line) for line in path.read_text().splitlines()]
    if len(rows) != count or [row["query_id"] for row in rows] != list(range(count)):
        raise RuntimeError("Missing, duplicate, or unordered query records")
    for row in rows:
        if len(row["ids"]) != 10 or len(set(row["ids"])) != 10:
            raise RuntimeError("Invalid result cardinality")
    recall = sum(len(set(row["ids"]) & set(ground_truth[i]))
                 for i, row in enumerate(rows)) / (count * 10)
    if recall < 0.85:
        raise RuntimeError(f"Small-data recall gate failed: {recall}")
    return recall


def rebuild_worker(args):
    directory = args.directory
    binaries = ROOT / "build/windows/Release"
    builder = ROOT / "build/deps/DiskANN/x64/Release/build_disk_index.exe"
    old_results = directory / "resident-old.jsonl"
    ready = Path(str(old_results) + ".ready")
    release = Path(str(old_results) + ".release")
    for marker in (ready, release):
        marker.unlink(missing_ok=True)
    with (directory / "resident-old.log").open("wb") as log:
        old = subprocess.Popen([str(binaries / "diskann_probe.exe"),
                                str(directory / "index"), str(directory / "queries.bin"),
                                str(old_results), "hold"], stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 30
            while not ready.exists():
                if old.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("Old index did not become resident")
                time.sleep(0.01)
            run(build_command(builder, directory / "combined.bin", directory / "index-new"),
                directory / "rebuild.log")
        finally:
            release.write_text("1")
            try:
                code = old.wait(timeout=10)
            except subprocess.TimeoutExpired:
                old.kill()
                old.wait()
                raise
        if code != 0:
            raise RuntimeError("Resident old query process failed")


def main(args):
    import numpy as np

    directory = args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    if not str(directory).isascii():
        raise RuntimeError("DiskANN 0.6.1 fixture paths must be ASCII")
    if shutil.disk_usage(directory).free < 2 * 1024**3:
        raise RuntimeError("Need at least 2 GiB free for probes")
    actual = subprocess.check_output(
        ["git", "-C", ROOT / "build/deps/DiskANN", "rev-parse", "HEAD"], text=True).strip()
    if actual != COMMIT:
        raise RuntimeError("DiskANN commit does not match lock")
    subprocess.run(["git", "-C", ROOT / "build/deps/DiskANN", "apply",
                    "--reverse", "--check", ROOT / "tools/environment/diskann-0.6.1.patch"],
                   check=True)
    plan = json.loads((ROOT / "configs/stage2-capacity.json").read_text())
    payload = plan["vector_count"] * plan["dimension"] * plan["element_bytes"]
    for mode in ("query_memory_bytes", "rebuild_memory_bytes"):
        if sum(plan[mode].values()) != plan["total_memory_bytes"]:
            raise RuntimeError(f"Capacity budget does not sum: {mode}")
    if plan["total_memory_bytes"] != int(payload * plan["memory_fraction"]):
        raise RuntimeError("Memory fraction mismatch")
    if shutil.disk_usage(directory).free < plan["required_free_disk_bytes"]:
        raise RuntimeError("Selected volume cannot hold planned data and rebuild reserve")
    runner = ROOT / "build/windows/Release/job_runner.exe"
    probe = ROOT / "build/windows/Release/diskann_probe.exe"
    builder = ROOT / "build/deps/DiskANN/x64/Release/build_disk_index.exe"
    environment = ROOT / "build/windows/Release/windows_environment_probe.exe"
    run([environment, directory], directory / "volume-probe.log")
    rng = np.random.default_rng(20261110)
    combined = rng.normal(size=(8192, 32)).astype("<f4")
    base = combined[:4096]
    queries = rng.normal(size=(128, 32)).astype("<f4")
    for name, values in (("base.bin", base), ("combined.bin", combined),
                         ("queries.bin", queries)):
        binfile(directory / name, values)
    # Independent float64 exact answers; tie break by original row/ID.
    truth = np.asarray([np.argsort(np.sum(
        (base.astype(np.float64) - query.astype(np.float64))**2, axis=1),
        kind="stable")[:10] for query in queries])
    write_json(directory / "ground-truth.json", truth.tolist())
    run([runner, 512, 180, directory / "build-memory.json",
         *build_command(builder, directory / "base.bin", directory / "index")],
        directory / "build.log")
    recalls = []
    for iteration in range(2):
        results = directory / f"results-{iteration}.jsonl"
        run([runner, 128, 60, directory / f"query-memory-{iteration}.json",
             probe, directory / "index", directory / "queries.bin", results],
            directory / f"query-{iteration}.log")
        recalls.append(validate_results(results, truth, len(queries)))
    import matplotlib
    matplotlib.use("Agg")
    from matplotlib import pyplot as plt

    figure, axis = plt.subplots(figsize=(4, 2.5))
    axis.bar(["load 1", "load 2"], recalls, color=["#167d8d", "#a63246"])
    axis.set(ylabel="Recall@10", ylim=(0, 1.05), title="Synthetic feasibility fixture")
    figure.tight_layout()
    figure.savefig(directory / "recall-smoke.png", dpi=120)
    plt.close(figure)
    if (directory / "results-0.jsonl").read_bytes() != (directory / "results-1.jsonl").read_bytes():
        raise RuntimeError("Reload changed query results")
    run([runner, 128, 30, directory / "injected-failure-memory.json",
         probe, directory / "index", directory / "queries.bin",
         directory / "injected-failure.jsonl", "fail"],
        directory / "injected-failure.log", success=False)
    if "Injected reader submission failure" not in (directory / "injected-failure.log").read_text():
        raise RuntimeError("Fault did not propagate through the reader")
    run([runner, 512, 180, directory / "rebuild-memory.json", sys.executable,
         Path(__file__), "--directory", directory, "--rebuild-worker"],
        directory / "rebuild-worker.log")
    # Real nonsparse allocation and flush on the selected data volume.
    capacity_path = directory / "capacity-512MiB.bin"
    started = time.monotonic()
    with capacity_path.open("wb", buffering=0) as output:
        block = rng.integers(0, 256, size=1024 * 1024, dtype=np.uint8).tobytes()
        for _ in range(512):
            output.write(block)
        os.fsync(output.fileno())
    with capacity_path.open("rb", buffering=0) as source:
        for offset in (0, 255 * 1024**2, 511 * 1024**2):
            source.seek(offset)
            if source.read(len(block)) != block:
                raise RuntimeError("Capacity file content mismatch")
    capacity_size = capacity_path.stat().st_size
    capacity_path.unlink()
    memory_files = ["build-memory.json", "query-memory-0.json",
                    "query-memory-1.json", "rebuild-memory.json"]
    memory = {name: json.loads((directory / name).read_text()) for name in memory_files}
    for name, metrics in memory.items():
        if metrics["exit_code"] or metrics["timed_out"]:
            raise RuntimeError(f"Resource run failed: {name}")
        if metrics["kernel_peak_job_commit_bytes"] > metrics["limit_commit_bytes"]:
            raise RuntimeError(f"Unexpected non-fault peak above job limit: {name}")
    artifacts = sorted(path for path in directory.iterdir()
                       if path.is_file() and path.name != "summary.json")
    summary = {
        "schema_version": 1, "purpose": "stage2_feasibility_only",
        "diskann_commit": COMMIT, "directory": str(directory),
        "python_version": sys.version,
        "numpy_version": np.__version__, "matplotlib_version": matplotlib.__version__,
        "source_sha256": {
            str(path.relative_to(ROOT)): sha256(path)
            for path in [
                ROOT / "CMakeLists.txt", ROOT / "configs/dependencies.lock.json",
                ROOT / "evaluation/requirements.lock.txt",
                *sorted((ROOT / "tools/environment").glob("*"))
            ] if path.is_file()
        },
        "binary_sha256": {
            path.name: sha256(path) for path in (
                runner, probe, environment, builder,
                ROOT / "build/windows/Release/diskann.dll")
        },
        "seed": 20261110, "vector_count": 4096, "dimension": 32,
        "query_count": 128, "search_k": 10, "search_list": 128, "beam_width": 4,
        "recall_at_10": recalls, "reload_identical": True,
        "reader_failure_propagated": True,
        "old_index_resident_during_rebuild": True,
        "capacity_probe_bytes_written_flushed_checked": capacity_size,
        "capacity_probe_seconds": time.monotonic() - started,
        "volume_free_bytes_after": shutil.disk_usage(directory).free,
        "capacity_plan": {
            "config_sha256": sha256(ROOT / "configs/stage2-capacity.json"),
            "raw_payload_bytes_estimated": payload,
            "total_memory_bytes_planned": plan["total_memory_bytes"],
            "required_free_disk_bytes_planned": plan["required_free_disk_bytes"],
            "budget_arithmetic_checked": True,
            "large_dataset_executed": False,
        },
        "memory": memory,
        "index_files": {path.name: path.stat().st_size
                        for path in directory.glob("index*") if path.is_file()},
        "artifacts_sha256": {path.name: sha256(path) for path in artifacts},
        "limits": [
            "Synthetic small data, not official performance or constrained-memory recall acceptance.",
            "Rebuild is upstream full rebuild while old reader remains resident, not Engine merge.",
            "No-buffering graph reads; PQ and metadata loads still use upstream buffered streams.",
            "5ms private/working-set samples are lower bounds; kernel job peak reported separately.",
            "Job commit limit does not bound shared pages, OS cache, or physical RAM.",
        ],
    }
    write_json(directory / "summary.json", summary)
    print(json.dumps({key: summary[key] for key in (
        "recall_at_10", "reload_identical", "reader_failure_propagated",
        "old_index_resident_during_rebuild",
        "capacity_probe_bytes_written_flushed_checked")}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=Path, default=Path("D:/agent-io-stage2/20261110"))
    parser.add_argument("--rebuild-worker", action="store_true")
    arguments = parser.parse_args()
    if arguments.rebuild_worker:
        rebuild_worker(arguments)
    else:
        main(arguments)
