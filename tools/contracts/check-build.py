"""Verify the first-build producer independently of the Engine that consumes it.

The stage 4 acceptance condition is "an independent basis for correctness". For
the build side that means: re-derive every published fact from the bytes on
disk instead of trusting ``build-report.json``. This script re-hashes the
segment files, walks CURRENT -> manifest -> segment.json itself, and checks the
publication invariants the storage protocol claims.

It also drives the refusal matrix. A first build is the one operation that can
destroy an index, so every rejected configuration is checked twice: the process
must fail, and the previously published index must be byte-identical
afterwards. A build that refuses but leaves a half-written segment behind has
still failed the contract.

Exit code 0 means every case behaved; 1 means at least one assertion failed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from copy import deepcopy
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCHEMA_VERSION = 1
PROTOCOL = "EVAL-1.1"
VECTOR_COUNT = 64
DIMENSION = 8
JOB_LIMIT_MB = 256
JOB_TIMEOUT_S = 60

F32 = struct.Struct("<f")
U64 = struct.Struct("<Q")


class Assertion(Exception):
    pass


def require(condition, message):
    if not condition:
        raise Assertion(message)


def sha256_bytes(payload):
    return hashlib.sha256(payload).hexdigest()


def sha256_file(path):
    return sha256_bytes(path.read_bytes())


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def dump(path, value):
    # newline="\n" keeps the fixture identical across platforms, which matters
    # because the configuration's own hash is part of the evidence chain.
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8", newline="\n")


def is_ascii_path(path):
    """FORMAT-1 refuses non-ASCII dataset paths, so a workspace has to be ASCII."""
    try:
        str(path).encode("ascii")
    except UnicodeEncodeError:
        return False
    return True


def temporary_workspace():
    """Return an ASCII scratch directory even when TEMP itself is not ASCII."""
    candidate = Path(tempfile.mkdtemp(prefix="build-contract-")).resolve()
    if is_ascii_path(candidate):
        return candidate
    shutil.rmtree(candidate, ignore_errors=True)
    root = Path(os.environ.get("SystemDrive", "C:") + os.sep) / "agent-io-build-contract"
    root.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix="run-", dir=root)).resolve()


def write_vectors(path, count, dimension, seed):
    """Deterministic float32 matrix plus its uint64 id column."""
    generator = random.Random(seed)
    rows = bytearray()
    for index in range(count):
        base = float(index) / max(count, 1)
        for axis in range(dimension):
            rows += F32.pack(base + generator.uniform(-0.25, 0.25))
    path.write_bytes(bytes(rows))
    ids = b"".join(U64.pack(10_000 + 7 * index) for index in range(count))
    (path.parent / "base.ids.u64").write_bytes(ids)


def build_config(dataset_dir, vectors, ids, vectors_sha, ids_sha, **overrides):
    config = {
        "schema_version": 1,
        "protocol_version": PROTOCOL,
        "purpose": "measurement",
        "build_id": "contract-build",
        "dataset": {
            "format": "raw-f32le-v1",
            "vectors": str(vectors),
            "ids": str(ids),
            "vector_count": str(VECTOR_COUNT),
            "dimension": DIMENSION,
            "metric": "squared_l2",
            "sha256": {"vectors": vectors_sha, "ids": ids_sha},
        },
        "native": {
            "dependency": "diskann",
            "version": "0.6.1+4c31367",
            "native_version": "0.6.1+4c31367",
            "max_degree": 16,
            "build_beam": 4,
            "pq_bytes": 32,
            "parameters": "contract fixture: exact knn plus xor long range",
        },
        "limits": {
            "memory_limit_bytes": str(JOB_LIMIT_MB * 1024 * 1024),
            "min_free_disk_bytes": "0",
            "job_object": True,
            "measurement_scope": "build_process_tree",
            "max_peak_commit_bytes": str(JOB_LIMIT_MB * 1024 * 1024),
        },
        "staging": {"directory": "index.staging", "same_volume": True,
                    "reuse_allowed": False},
        "output": {"directory": "index", "checkpoint_sequence": "0",
                   "generation": "1", "publish_current": True},
        "notes": ["Contract fixture for the first-build refusal matrix."],
    }
    config.update(overrides)
    return config


def verify_published(index_dir, config):
    """Re-derive every published claim from the bytes under index_dir."""
    current_path = index_dir / "CURRENT"
    require(current_path.is_file(), "CURRENT was not published")
    current = load(current_path)
    require(current["format_version"] == 1, "CURRENT format_version must be 1")
    manifest_relative = current["manifest"]
    require(manifest_relative == f"manifests/manifest-{config['output']['generation']}.json",
            f"CURRENT names an unexpected manifest: {manifest_relative}")
    manifest_path = index_dir / manifest_relative
    require(manifest_path.is_file(), "the manifest CURRENT names does not exist")
    # The digest in CURRENT must cover the manifest bytes, not the path.
    require(sha256_file(manifest_path) == current["sha256"],
            "CURRENT.sha256 does not match the manifest content")

    manifest = load(manifest_path)
    require(manifest["instance_id"] == current["instance_id"],
            "instance_id differs between CURRENT and the manifest")
    require(manifest["generation"] == config["output"]["generation"],
            "manifest generation differs from the requested one")
    require(manifest["checkpoint_sequence"] == config["output"]["checkpoint_sequence"],
            "manifest checkpoint differs from the requested one")
    require(manifest["dimension"] == config["dataset"]["dimension"],
            "manifest dimension differs from the dataset")
    require(manifest["metric"] == config["dataset"]["metric"],
            "manifest metric differs from the dataset")
    # This is the regression that motivated the check: the manifest must name
    # the native library version, not the adapter's own version.
    require(manifest["adapter"]["native_version"] == config["native"]["native_version"],
            "manifest adapter.native_version must record the native dependency version, "
            f"got {manifest['adapter']['native_version']!r}")

    segments = manifest["segments"]
    require(len(segments) == 1, f"a first build publishes exactly one segment, got {len(segments)}")
    segment = segments[0]
    require(segment["kind"] == "base", "the first segment must be a base segment")
    require(segment["covered_sequence"] == config["output"]["checkpoint_sequence"],
            "segment covered_sequence must equal the published checkpoint")
    require(segment["node_count"] == config["dataset"]["vector_count"],
            "segment node_count differs from the vector count")
    require(manifest["live_count"] == segment["node_count"],
            "live_count must equal the single segment's node_count")

    segment_dir = index_dir / "segments" / segment["uuid"]
    require(segment_dir.is_dir(), "the segment directory named by the manifest is missing")
    metadata_path = segment_dir / "segment.json"
    require(metadata_path.is_file(), "segment.json is missing")
    require(sha256_file(metadata_path) == segment["metadata"]["sha256"],
            "segment.json digest differs from the manifest")
    require(segment["metadata"]["path"] ==
            f"segments/{segment['uuid']}/segment.json",
            "manifest metadata path must be relative and name the segment")

    metadata = load(metadata_path)
    require(metadata["segment_uuid"] == segment["uuid"],
            "segment.json uuid differs from its directory and the manifest")
    require(metadata["node_count"] == segment["node_count"],
            "segment.json node_count differs from the manifest")
    require(metadata["layout"] == manifest["layout"],
            "segment layout differs from the manifest layout")

    files = {entry["path"]: entry for entry in metadata["files"]}
    require(set(files) == {"graph.bin", "versions.bin"},
            f"unexpected segment file set: {sorted(files)}")
    for name, entry in files.items():
        path = segment_dir / name
        require(path.is_file(), f"{name} is missing from the segment")
        payload = path.read_bytes()
        require(sha256_bytes(payload) == entry["sha256"],
                f"{name} digest does not match segment.json")
        # physical_bytes is what occupies the disk; logical_bytes is what the
        # reader consumes. They diverge when the payload is not block aligned,
        # which is the normal case for a small version directory.
        require(len(payload) == int(entry["physical_bytes"]),
                f"{name} physical_bytes does not match its size on disk")
        require(int(entry["logical_bytes"]) <= int(entry["physical_bytes"]),
                f"{name} logical_bytes exceeds physical_bytes")
        require(int(entry["physical_bytes"]) % metadata["block_bytes"] == 0,
                f"{name} physical size is not block aligned")
    require(files["graph.bin"]["sha256"] == segment["content_sha256"],
            "manifest content_sha256 must be graph.bin's digest")

    version_directory = metadata["version_directory"]
    require(version_directory["file"] == "versions.bin",
            "version directory must live in versions.bin")
    require(version_directory["entry_count"] == metadata["node_count"],
            "version entry count must cover every node")
    require(version_directory["record_bytes"] == 32,
            "version records are fixed at 32 bytes")
    require(int(files["versions.bin"]["logical_bytes"]) ==
            int(version_directory["entry_count"]) * 32,
            "versions.bin logical size must be entry_count * 32")
    require(files["graph.bin"]["logical_bytes"] == files["graph.bin"]["physical_bytes"],
            "graph.bin is row aligned, so its logical and physical sizes must agree")

    # Walk the version directory independently. Records are ordered by external
    # id, not by graph node, so the node column must be a permutation of the
    # node range while the id column must ascend.
    records = segment_dir / "versions.bin"
    raw = records.read_bytes()
    logical_size = int(version_directory["entry_count"]) * 32
    require(len(raw) >= logical_size, "versions.bin is shorter than its logical size")
    previous_id = -1
    node_ids = []
    for offset in range(0, logical_size, 32):
        record_id, version, node, reserved = struct.unpack_from("<QQQQ", raw, offset)
        require(record_id > previous_id, "version directory is not ordered by id")
        require(version == 0, "a first build must publish version 0 for every id")
        require(reserved == 0, "version record reserved bytes must be zero")
        require(node < int(metadata["node_count"]), "version record node is out of range")
        previous_id = record_id
        node_ids.append(node)
    require(sorted(node_ids) == list(range(int(metadata["node_count"]))),
            "version directory must map every node exactly once")
    require(all(byte == 0 for byte in raw[logical_size:]),
            "versions.bin padding must be zero")
    return current, manifest, metadata


def run_build(job_runner, binary, config_path, report_path, workdir):
    metrics = workdir / "job.json"
    command = [str(job_runner), str(JOB_LIMIT_MB), str(JOB_TIMEOUT_S), str(metrics),
               str(binary), "--config", str(config_path), "--report", str(report_path)]
    completed = subprocess.run(command, capture_output=True, timeout=JOB_TIMEOUT_S + 30)
    log = (completed.stdout + completed.stderr).decode("utf-8", "replace")
    record = load(metrics) if metrics.is_file() else {}
    return record.get("exit_code"), log, record


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path,
                        default=ROOT / "build/windows/Release/vector_build.exe")
    parser.add_argument("--job-runner", type=Path,
                        default=ROOT / "build/windows/Release/job_runner.exe")
    parser.add_argument("--output", type=Path, help="evidence directory to keep")
    arguments = parser.parse_args(argv)

    binary = arguments.binary.resolve()
    job_runner = arguments.job_runner.resolve()
    for path in (binary, job_runner):
        if not path.is_file():
            print(f"missing executable: {path}", file=sys.stderr)
            return 2

    evidence_dir = None
    if arguments.output:
        evidence_dir = arguments.output.resolve()
        # Re-running the suite must be idempotent, but a build refuses to write
        # into an index that already exists. Clear our own previous workspace
        # and nothing else: refuse to touch a directory that does not carry the
        # markers this script writes.
        if evidence_dir.exists() and any(evidence_dir.iterdir()):
            markers = ("build-contract.json", "published")
            if not any((evidence_dir / marker).exists() for marker in markers):
                print(f"refusing to clear {evidence_dir}: it is not a contract workspace",
                      file=sys.stderr)
                return 2
            shutil.rmtree(evidence_dir)
        evidence_dir.mkdir(parents=True, exist_ok=True)

    # ctest asks for the evidence directory next to the build tree, so the path
    # inherits whatever the checkout is called. The producer rejects non-ASCII
    # dataset paths by design, which would make this suite pass or fail on the
    # name of the parent folder rather than on the contract. Run the cases in an
    # ASCII workspace and copy the report back instead.
    if evidence_dir is not None and is_ascii_path(evidence_dir):
        workspace = evidence_dir
    else:
        workspace = temporary_workspace()
        if evidence_dir is not None:
            print(f"note: {evidence_dir} is not an ASCII path, so the cases run in "
                  f"{workspace} and the report is copied back", file=sys.stderr)
    dataset = workspace / "dataset"
    dataset.mkdir(exist_ok=True)
    vectors = dataset / "base.f32"
    write_vectors(vectors, VECTOR_COUNT, DIMENSION, seed=7)
    ids = dataset / "base.ids.u64"
    digests = {"vectors": sha256_file(vectors), "ids": sha256_file(ids)}

    cases = {}

    # --- case 1: a clean build, verified from the bytes ----------------------
    good = workspace / "published"
    good.mkdir(exist_ok=True)
    config = build_config(good, vectors.resolve(), ids.resolve(),
                          digests["vectors"], digests["ids"])
    dump(good / "build.json", config)
    code, log, record = run_build(job_runner, binary, good / "build.json",
                                  good / "build-report.json", good)
    require(code == 0, f"a valid first build must succeed, got exit {code}: {log[-800:]}")
    # The kernel, not the child, is the authority on the Job's peak commit.
    peak = int(record.get("kernel_peak_job_commit_bytes", 0))
    require(peak > 0, "the Job peak commit was not measured")
    require(int(record["limit_commit_bytes"]) == JOB_LIMIT_MB * 1024 * 1024,
            "the child did not inherit the Job commit limit")
    current, manifest, metadata = verify_published(good / "index", config)
    report = load(good / "build-report.json")
    require(report["published"] is True, "the report must claim publication")
    require(report["node_count"] == config["dataset"]["vector_count"],
            "the report node_count differs from the dataset")
    require(report["graph_sha256"] == sha256_file(
        good / "index" / "segments" / manifest["segments"][0]["uuid"] / "graph.bin"),
        "the report graph digest differs from the published file")
    require(report["native_version"] == config["native"]["native_version"],
            "the report must name the native dependency version")
    require(not (good / "index.staging").exists(), "staging must not survive a publish")
    cases["valid-build"] = {"exit_code": code, "kernel_peak_job_commit_bytes": peak,
                            "elapsed_ms": record.get("elapsed_ms")}

    published_current = sha256_file(good / "index" / "CURRENT")
    published_manifest = {"generation": manifest["generation"],
                          "instance_id": manifest["instance_id"],
                          "live_count": manifest["live_count"]}

    # --- case 2: refusals must leave the published index untouched ----------
    def refusal(name, mutate, expected, dataset_override=None):
        case = workspace / f"refused-{name}"
        case.mkdir(exist_ok=True)
        broken = deepcopy(config)
        mutate(broken)
        dump(case / "build.json", broken)
        if dataset_override:
            dataset_override(case)
        code, log, _ = run_build(job_runner, binary, case / "build.json",
                                 case / "build-report.json", case)
        require(code == expected,
                f"{name}: expected exit {expected}, got {code}: {log[-400:]}")
        # The build points at its own output directory, so nothing it does can
        # touch the published index. Re-check anyway: the guarantee must be
        # observed, not assumed.
        require(sha256_file(good / "index" / "CURRENT") == published_current,
                f"{name}: the published CURRENT changed")
        require(not (case / "index").exists() or not (case / "index" / "CURRENT").exists(),
                f"{name}: a refused build published CURRENT")
        require(not (case / "index.staging").exists(),
                f"{name}: a refused build left its staging directory behind")
        cases[name] = {"exit_code": code}

    def set_field(path, value):
        def mutate(config):
            target = config
            for key in path[:-1]:
                target = target[key]
            target[path[-1]] = value
        return mutate

    def drop_field(path):
        def mutate(config):
            target = config
            for key in path[:-1]:
                target = target[key]
            del target[path[-1]]
        return mutate

    # Configuration-level refusals: the parser rejects them before any IO.
    refusal("unknown-field", set_field(("extra",), True), 2)
    refusal("missing-notes", drop_field(("notes",)), 2)
    refusal("schema-version", set_field(("schema_version",), 2), 2)
    refusal("protocol-version", set_field(("protocol_version",), "EVAL-1.0"), 2)
    refusal("bad-purpose", set_field(("purpose",), "benchmark"), 2)
    refusal("noncanonical-u64", set_field(("dataset", "vector_count"), "064"), 2)
    refusal("boolean-as-integer", set_field(("native", "max_degree"), True), 2)
    refusal("bad-metric", set_field(("dataset", "metric"), "cosine"), 2)
    refusal("bad-format", set_field(("dataset", "format"), "text"), 2)
    refusal("beam-exceeds-degree",
            lambda config: config["native"].update(build_beam=64), 2)
    refusal("generation-not-one", set_field(("output", "generation"), "2"), 2)
    refusal("checkpoint-not-zero", set_field(("output", "checkpoint_sequence"), "5"), 2)
    refusal("publish-disabled", set_field(("output", "publish_current"), False), 2)
    refusal("job-less-limits", set_field(("limits", "job_object"), False), 2)
    refusal("staging-reuse", set_field(("staging", "reuse_allowed"), True), 2)
    # The two native version fields must agree; disagreeing means the manifest
    # would name a version no artifact was built against.
    refusal("native-version-disagreement",
            set_field(("native", "native_version"), "0.5.0+0000000"), 2)

    # Data-level refusals. Exit 2 means the request contradicts itself and is
    # rejected before any file is touched; exit 4 means the request was coherent
    # but the data on disk did not match it. The split is part of the contract,
    # so it is asserted rather than tolerated.
    refusal("vector-count-mismatch",
            set_field(("dataset", "vector_count"), str(VECTOR_COUNT + 1)), 2)
    refusal("dimension-mismatch",
            set_field(("dataset", "dimension"), DIMENSION + 1), 2)
    refusal("vector-hash-mismatch",
            set_field(("dataset", "sha256", "vectors"), "0" * 64), 4)
    refusal("id-hash-mismatch",
            set_field(("dataset", "sha256", "ids"), "0" * 64), 4)
    refusal("missing-vectors", set_field(("dataset", "vectors"),
                                        str(workspace / "absent.f32")), 4)

    # --- case 3: a self-declared peak ceiling is detected after publication -
    # Peak commit is only knowable once the process has run, so this refusal
    # cannot precede the publish. The contract is therefore: exit 4, the
    # publication stands, and the report must expose the overrun instead of
    # hiding it. Treating it like the other refusals would be a lie.
    overrun = workspace / "peak-overrun"
    overrun.mkdir(exist_ok=True)
    broken = deepcopy(config)
    broken["limits"]["max_peak_commit_bytes"] = "1"
    dump(overrun / "build.json", broken)
    code, log, _ = run_build(job_runner, binary, overrun / "build.json",
                             overrun / "build-report.json", overrun)
    require(code == 4, f"peak-overrun: expected exit 4, got {code}: {log[-300:]}")
    require((overrun / "index" / "CURRENT").is_file(),
            "peak-overrun: the build had already published, CURRENT must exist")
    overrun_report = load(overrun / "build-report.json")
    require(overrun_report["published"] is True,
            "peak-overrun: the report must state that the index is published")
    require(int(overrun_report["peak_commit_bytes"]) > 1,
            "peak-overrun: the report must expose the measured peak")
    require(not (overrun / "index.staging").exists(),
            "peak-overrun: staging must not survive a completed publish")
    cases["peak-overrun-after-publish"] = {"exit_code": code,
                                           "published": True,
                                           "peak_commit_bytes": overrun_report["peak_commit_bytes"]}

    # --- case 4: a second build must not overwrite a live index ------------
    second = workspace / "second-build"
    second.mkdir(exist_ok=True)
    existing = deepcopy(config)
    existing["output"]["directory"] = str((good / "index").resolve())
    # dataset paths stay absolute, staging moves next to the second config
    existing["staging"]["directory"] = str((second / "index.staging").resolve())
    dump(second / "build.json", existing)
    code, log, _ = run_build(job_runner, binary, second / "build.json",
                             second / "build-report.json", second)
    require(code != 0, "a second build into a live index must be refused")
    require(sha256_file(good / "index" / "CURRENT") == published_current,
            "the refused second build modified CURRENT")
    require(load(good / "index" / "CURRENT") == current,
            "the refused second build replaced the manifest pointer")
    require(not (second / "index.staging").exists(),
            "the refused second build left staging behind")
    cases["second-build-refused"] = {"exit_code": code}

    manifest_now = load(good / "index" / f"manifests/manifest-{published_manifest['generation']}.json")
    require(manifest_now["instance_id"] == published_manifest["instance_id"],
            "the refused second build changed the instance identity")

    # --- case 5: malformed JSON never reaches the parser's happy path ------
    raw = json.dumps(config, separators=(",", ":"))
    malformed = {
        "duplicate-key": raw.replace('"schema_version":1', '"schema_version":1,"schema_version":1'),
        "trailing-content": raw + "{}",
        "nan": raw.replace('"max_degree":16', '"max_degree":NaN'),
        "unterminated": raw[:-1],
    }
    for name, text in malformed.items():
        case = workspace / f"refused-{name}"
        case.mkdir(exist_ok=True)
        path = case / "build.json"
        path.write_text(text, encoding="utf-8", newline="\n")
        code, log, _ = run_build(job_runner, binary, path, case / "report.json", case)
        require(code == 2, f"{name}: malformed JSON must exit 2, got {code}: {log[-300:]}")
        cases[name] = {"exit_code": code}

    evidence = {
        "schema_version": SCHEMA_VERSION,
        "protocol_version": PROTOCOL,
        "purpose": "stage4_b_first_build_contract",
        "checked_at": datetime.now().astimezone().isoformat(),
        "workspace": str(workspace),
        "cases": cases,
        "cases_passed": len(cases),
        "published_index": published_manifest,
        "binary_sha256": {binary.name: sha256_file(binary),
                          job_runner.name: sha256_file(job_runner)},
        "dataset_sha256": digests,
        "limitations": [
            "The Job commit limit is a scheduling limit, not a physical memory measurement.",
            "The fixture is small on purpose: this checks the protocol, not build throughput.",
            "Graph quality is checked by index-engine-test, not by this script.",
        ],
    }
    dump(workspace / "build-contract.json", evidence)
    if evidence_dir is not None and evidence_dir != workspace:
        shutil.copyfile(workspace / "build-contract.json",
                        evidence_dir / "build-contract.json")
    print(json.dumps({"cases_passed": len(cases), "workspace": str(workspace)},
                     ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Assertion as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        raise SystemExit(1)
