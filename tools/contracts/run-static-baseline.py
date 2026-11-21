"""Run the static baseline end to end: fixture, first build, measured run, check.

Usage:
    python tools/contracts/run-static-baseline.py --run-id stage4-static-001
    python tools/contracts/run-static-baseline.py --run-id repeat --repeat-of stage4-static-001

Every stage runs inside the Job commit limit that the configuration declares, so
the recorded peaks are comparable across runs. The script never rewrites an
emitted artifact; it only launches the two binaries and the EVAL-1.1 checker.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
RELEASE = ROOT / "build/windows/Release"
BUILD_LIMIT_MIB = 512
RUN_LIMIT_MIB = 384


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def launch(command, log_path):
    completed = subprocess.run(command, capture_output=True, text=True, errors="replace")
    log_path.write_text((completed.stdout or "") + (completed.stderr or ""),
                        encoding="utf-8", newline="\n")
    return completed.returncode, completed.stdout or ""


def require(condition, message):
    if not condition:
        raise SystemExit(message)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--root", type=Path, default=ROOT / "runs")
    parser.add_argument("--vectors", type=int, default=512)
    parser.add_argument("--dimension", type=int, default=16)
    parser.add_argument("--duration-ms", type=int, default=6000)
    parser.add_argument("--ops-per-second", type=int, default=10)
    parser.add_argument("--seed", type=int, default=20261121)
    args = parser.parse_args(argv)

    run = (args.root / args.run_id).resolve()
    require(not run.exists() or not any(run.iterdir()),
            f"run directory already holds data: {run}")
    run.mkdir(parents=True, exist_ok=True)

    fixture = [sys.executable, "-m", "evaluation.static_fixture", "--output", str(run),
               "--vectors", str(args.vectors), "--dimension", str(args.dimension),
               "--seed", str(args.seed),
               "--duration-ms", str(args.duration_ms),
               "--ops-per-second", str(args.ops_per_second), "--run-id", args.run_id]
    code, _ = launch(fixture, run / "fixture.log")
    require(code == 0, f"fixture generation failed, see {run / 'fixture.log'}")

    build = [str(RELEASE / "job_runner.exe"), str(BUILD_LIMIT_MIB), "120",
             str(run / "build-job.json"), str(RELEASE / "vector_build.exe"),
             "--config", str(run / "build.json"), "--report", str(run / "build-report.json")]
    code, output = launch(build, run / "build.log")
    require(code == 0, f"first build failed: {output.strip()}")

    bench = [str(RELEASE / "job_runner.exe"), str(RUN_LIMIT_MIB), "300",
             str(run / "job.json"), str(RELEASE / "vector_bench.exe"),
             "--config", str(run / "run-config.json"), "--output", str(run),
             "--target", "engine", "--job-metrics", str(run / "job.json")]
    code, output = launch(bench, run / "bench.log")
    require(code == 0, f"measured run failed: {output.strip()}")

    check = [sys.executable, "-m", "evaluation.check", "--run", str(run)]
    code, output = launch(check, run / "check.log")
    require(code in (0, 2), f"checker rejected the run, see {run / 'check.log'}")

    evaluation = json.loads((run / "evaluation.json").read_text(encoding="utf-8"))
    build_report = json.loads((run / "build-report.json").read_text(encoding="utf-8"))
    environment = json.loads((run / "environment.json").read_text(encoding="utf-8"))
    counters = json.loads((run / "counters.json").read_text(encoding="utf-8"))
    summary = {
        "run_directory": str(run),
        "verdict": evaluation["verdict"],
        "recall_at_k": {"k": evaluation["recall_at_k"]["k"],
                        "mean": evaluation["recall_at_k"]["mean"]},
        "queries": evaluation["queries"],
        "build": {"node_count": build_report.get("node_count"),
                  "file_bytes": build_report.get("file_bytes"),
                  "peak_commit_bytes": build_report.get("peak_commit_bytes"),
                  "job_limit_mib": BUILD_LIMIT_MIB},
        "environment": {
            "target": environment["target"],
            "formal_engine_measurement": environment["formal_engine_measurement"],
            "code_commit_at_configure": environment["code_commit_at_configure"],
            "job_limit_commit_bytes": environment["job_limit_commit_bytes"],
            "unbudgeted_metadata_bytes": environment["unbudgeted_metadata_bytes"],
            "io_backend": environment["io_backend"], "direct_reads": environment["direct_reads"],
        },
        "engine_counters": counters["engine_counters"],
    }
    artifacts = {path.name: digest(path) for path in sorted(run.iterdir()) if path.is_file()}
    segments = run / "index" / "segments"
    if segments.is_dir():
        for segment in sorted(segments.iterdir()):
            for path in sorted(segment.iterdir()):
                artifacts[f"index/segments/{segment.name}/{path.name}"] = digest(path)
    summary["artifact_sha256"] = artifacts
    (run / "baseline-summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps(summary, indent=2))
    return 0 if evaluation["verdict"] == "pass" else 2


if __name__ == "__main__":
    sys.exit(main())
