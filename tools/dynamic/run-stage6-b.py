"""Stage 6 B-side driver: write acknowledgement, visibility and recovery.

The engine tests prove the protocol holds; this script measures what it costs
and then checks the numbers against the invariants the protocol promises. Each
write count gets its own fixture so a longer history cannot hide a leak in a
shorter one.

Every run produces three artifacts:

* ``probe.json``  - acknowledgement and visibility latency, sequence range,
* ``reopen.json`` - recovery time and the state seen by a second process,
* ``probe.log``   - the raw probe transcript, kept so a failed check is
  reproducible without guessing which argument produced it.

The script refuses to overwrite a run directory: evidence that can be rewritten
silently is not evidence.
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RELEASE = ROOT / "build/release"
BUILD_LIMIT_MIB = 512


def launch(command, log_path):
    completed = subprocess.run(command, capture_output=True, text=True, errors="replace")
    log_path.write_text((completed.stdout or "") + (completed.stderr or ""),
                        encoding="utf-8", newline="\n")
    return completed.returncode, (completed.stdout or "")


def require(condition, message, failures):
    if not condition:
        failures.append(message)


def frame_bytes(dimension):
    """FORMAT-1 frame: 64-byte header, payload, CRC32C, padded to eight bytes."""
    return ((64 + dimension * 4 + 4) + 7) // 8 * 8


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True,
                        help="ASCII output directory for this sweep")
    parser.add_argument("--vectors", type=int, default=4096)
    parser.add_argument("--dimension", type=int, default=16)
    parser.add_argument("--write-counts", type=int, nargs="+", default=[50, 200, 800])
    parser.add_argument("--queries", type=int, default=10)
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--seed", type=int, default=20261125)
    args = parser.parse_args(argv)

    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        raise SystemExit(f"refusing to reuse a non-empty output directory: {output}")
    output.mkdir(parents=True, exist_ok=True)

    summary = {
        "schema_version": 1,
        "purpose": "stage6_b_dynamic_closure",
        "fixture": {
            "vectors": args.vectors,
            "dimension": args.dimension,
            "queries_per_run": args.queries,
            "k": args.k,
            "seed": args.seed,
            "frame_bytes": frame_bytes(args.dimension),
        },
        "runs": [],
        "checks": [],
        "failed_checks": 0,
    }
    failures = []

    for count in args.write_counts:
        run = output / f"writes-{count:05d}"
        run.mkdir()
        fixture = [
            sys.executable, "-m", "evaluation.static_fixture",
            "--output", str(run),
            "--vectors", str(args.vectors),
            "--dimension", str(args.dimension),
            "--seed", str(args.seed),
            "--duration-ms", "3000",
            "--ops-per-second", "20",
            "--run-id", run.name,
        ]
        code, _ = launch(fixture, run / "fixture.log")
        require(code == 0, f"{run.name}: fixture generation failed", failures)
        if code != 0:
            continue

        build = [
            str(RELEASE / "job_runner.exe"), str(BUILD_LIMIT_MIB), "300",
            str(run / "build-job.json"), str(RELEASE / "vector_build.exe"),
            "--config", str(run / "build.json"),
            "--report", str(run / "build-report.json"),
        ]
        code, text = launch(build, run / "build.log")
        require(code == 0, f"{run.name}: first build failed: {text.strip()[:200]}", failures)
        if code != 0:
            continue

        probe_command = [
            str(RELEASE / "write_probe.exe"),
            "--index", str(run / "index"),
            "--dimension", str(args.dimension),
            "--writes", str(count),
            "--queries", str(args.queries),
            "--k", str(args.k),
            "--seed", str(args.seed),
        ]
        code, text = launch(probe_command, run / "probe.log")
        require(code == 0, f"{run.name}: write probe failed: {text.strip()[:200]}", failures)
        if code != 0:
            continue
        probe = json.loads(text)
        (run / "probe.json").write_text(json.dumps(probe, indent=2) + "\n",
                                        encoding="utf-8", newline="\n")

        reopen_command = [
            str(RELEASE / "write_probe.exe"),
            "--index", str(run / "index"),
            "--dimension", str(args.dimension),
            "--reopen-only",
        ]
        code, text = launch(reopen_command, run / "reopen.log")
        require(code == 0, f"{run.name}: reopen probe failed: {text.strip()[:200]}", failures)
        if code != 0:
            continue
        reopen = json.loads(text)
        (run / "reopen.json").write_text(json.dumps(reopen, indent=2) + "\n",
                                         encoding="utf-8", newline="\n")

        expected_frame = frame_bytes(args.dimension)
        require(probe["confirmed_writes"] == count,
                f"{run.name}: only {probe['confirmed_writes']} of {count} writes were confirmed",
                failures)
        require(probe["first_sequence"] == 1,
                f"{run.name}: the first write took sequence {probe['first_sequence']}",
                failures)
        require(probe["last_sequence"] - probe["first_sequence"] + 1 == count,
                f"{run.name}: the sequence range is not contiguous", failures)
        require(probe["published_sequence"] == probe["last_sequence"],
                f"{run.name}: the published sequence lags the last receipt", failures)
        require(probe["immediately_visible"] == count,
                f"{run.name}: only {probe['immediately_visible']} writes were visible immediately",
                failures)
        require(probe["visibility_mismatches"] == 0,
                f"{run.name}: {probe['visibility_mismatches']} reads returned the wrong version",
                failures)
        require(probe["survived_reopen"] == count,
                f"{run.name}: only {probe['survived_reopen']} writes survived the restart", failures)
        require(probe["sequence_after_reopen"] == probe["last_sequence"],
                f"{run.name}: the reopened sequence differs from the last receipt", failures)
        require(probe["wal_bytes_written"] == count * expected_frame,
                f"{run.name}: WAL wrote {probe['wal_bytes_written']} bytes, "
                f"expected {count * expected_frame} for {count} frames", failures)
        require(reopen["recovered_sequence"] == probe["last_sequence"],
                f"{run.name}: a second process recovered a different sequence", failures)
        require(reopen["recovered_live"] == probe["live_vectors"],
                f"{run.name}: a second process recovered a different live count", failures)

        summary["runs"].append({
            "run": run.name,
            "writes": count,
            "write_acknowledge_ns": probe["write_acknowledge_ns"],
            "visible_read_ns": probe["visible_read_ns"],
            "query_ns": probe["query_ns"],
            "open_ns": probe["open_ns"],
            "reopen_ns": probe["reopen_ns"],
            "second_process_open_ns": reopen["open_ns"],
            "first_sequence": probe["first_sequence"],
            "last_sequence": probe["last_sequence"],
            "wal_bytes_written": probe["wal_bytes_written"],
            "wal_bytes_per_write": probe["wal_bytes_written"] / count,
            "delta_resident_bytes": probe["delta_resident_bytes"],
            "live_vectors": probe["live_vectors"],
        })

    for run in summary["runs"]:
        result = {
            "run": run["run"],
            "writes": run["writes"],
            "ack_p50_ms": run["write_acknowledge_ns"]["p50"] / 1e6,
            "ack_p95_ms": run["write_acknowledge_ns"]["p95"] / 1e6,
            "ack_p99_ms": run["write_acknowledge_ns"]["p99"] / 1e6,
            "visible_p50_ms": run["visible_read_ns"]["p50"] / 1e6,
            "reopen_ms": run["reopen_ns"] / 1e6,
        }
        summary["checks"].append(result)

    summary["failed_checks"] = len(failures)
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n",
                                         encoding="utf-8", newline="\n")

    for run in summary["runs"]:
        print(f"{run['run']}: writes={run['writes']} "
              f"ack_p50={run['write_acknowledge_ns']['p50'] / 1e6:.3f}ms "
              f"ack_p99={run['write_acknowledge_ns']['p99'] / 1e6:.3f}ms "
              f"reopen={run['reopen_ns'] / 1e6:.1f}ms "
              f"wal={run['wal_bytes_written']}")
    if failures:
        for message in failures:
            print(f"FAIL: {message}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
