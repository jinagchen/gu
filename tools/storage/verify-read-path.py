"""Independently verify the read-path counters a Stage 5 run publishes.

`summary.json` is outside EVAL-1.1: `evaluation.check` never opens it, so the
cache and prefetch statistics have no consumer-side check. This script supplies
one. It reads only the published artifacts and asserts the invariants that must
hold whatever the engine does internally; it never imports engine code, so a
bug in the engine cannot also silence the check.

Invariants fall into three groups:

* budget: a statistic may not exceed the budget the configuration declared.
* conservation: prefetch and read counters must obey their own ordering.
* coupling: a disabled feature must publish zero, and read bytes must not
  exceed the logical block demands that produced them.

Exit code is 0 when every invariant holds, 1 when any fails.
"""

import argparse
import json
import sys
from pathlib import Path


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def u(value, name):
    """Counters are decimal strings; reject anything else rather than coerce."""
    if isinstance(value, bool) or not isinstance(value, (int, str)):
        raise ValueError(f"{name} has an unusable type: {type(value).__name__}")
    text = str(value)
    if not text.isdigit():
        raise ValueError(f"{name} is not an unsigned decimal: {text!r}")
    return int(text)


class Report:
    def __init__(self):
        self.checks = []
        self.failed = 0

    def require(self, name, condition, detail):
        status = "pass" if condition else "fail"
        if not condition:
            self.failed += 1
        self.checks.append({"name": name, "status": status, "detail": detail})
        return condition


def verify(run):
    config = load(run / "effective-config.json")
    counters = load(run / "counters.json")
    summary = load(run / "summary.json")
    report = Report()

    engine = config["engine"]
    memory = engine["memory"]
    search = config["search"]
    counts = counters["engine_counters"]
    path = summary["read_path"]

    cache_bytes = u(memory["cache_bytes"], "memory.cache_bytes")
    total_bytes = u(memory["total_bytes"], "memory.total_bytes")
    block_bytes = u(engine["block_bytes"], "engine.block_bytes")
    inflight_cap = u(engine["max_inflight_reads"], "engine.max_inflight_reads")
    prefetch_cap = u(engine["max_prefetch_reads"], "engine.max_prefetch_reads")
    window = u(search["prefetch_blocks"], "search.prefetch_blocks")
    policy = str(engine["cache_policy"])
    backend = str(engine["io_backend"])

    hits = u(path["cache_hits"], "read_path.cache_hits")
    misses = u(path["cache_misses"], "read_path.cache_misses")
    ready = u(path["prefetch_ready_at_demand"], "read_path.prefetch_ready_at_demand")
    resident = u(path["cache_resident_bytes"], "read_path.cache_resident_bytes")
    accounted = u(path["engine_accounted_bytes"], "read_path.engine_accounted_bytes")
    peak_inflight = u(path["peak_inflight_reads"], "read_path.peak_inflight_reads")
    peak_prefetch = u(path["peak_prefetch_reads"], "read_path.peak_prefetch_reads")

    submitted = u(counts["io_reads_submitted"], "engine_counters.io_reads_submitted")
    completed_bytes = u(counts["io_read_bytes_completed"], "engine_counters.io_read_bytes_completed")
    read_errors = u(counts["io_read_errors"], "engine_counters.io_read_errors")
    prefetch_submitted = u(counts["prefetch_submitted"], "engine_counters.prefetch_submitted")
    prefetch_consumed = u(counts["prefetch_consumed"], "engine_counters.prefetch_consumed")

    # --- budget -----------------------------------------------------------
    report.require("cache_resident_within_budget", resident <= cache_bytes,
                   f"resident {resident} <= cache_bytes {cache_bytes}")
    report.require("inflight_within_cap", peak_inflight <= inflight_cap,
                   f"peak {peak_inflight} <= max_inflight_reads {inflight_cap}")
    report.require("prefetch_within_cap", peak_prefetch <= prefetch_cap,
                   f"peak {peak_prefetch} <= max_prefetch_reads {prefetch_cap}")
    report.require("accounted_within_total", accounted <= total_bytes,
                   f"accounted {accounted} <= total_bytes {total_bytes}")

    # --- conservation -----------------------------------------------------
    report.require("prefetch_ready_le_consumed", ready <= prefetch_consumed,
                   f"ready {ready} <= consumed {prefetch_consumed}")
    report.require("prefetch_consumed_le_submitted", prefetch_consumed <= prefetch_submitted,
                   f"consumed {prefetch_consumed} <= submitted {prefetch_submitted}")
    report.require("prefetch_within_window_peak", peak_prefetch <= prefetch_submitted,
                   f"peak_prefetch {peak_prefetch} <= submitted {prefetch_submitted}")
    # A miss may be served by an in-flight read another demand already started,
    # so dispatched demand reads can be fewer than misses. Prefetch reads are
    # extra, therefore submitted can never exceed misses + prefetch_submitted.
    report.require("reads_bounded_by_misses_and_prefetch",
                   submitted <= misses + prefetch_submitted,
                   f"submitted {submitted} <= misses {misses} + prefetch {prefetch_submitted}")
    report.require("completed_bytes_bounded_by_demands",
                   completed_bytes <= (hits + misses) * block_bytes,
                   f"completed {completed_bytes} <= demands {(hits + misses) * block_bytes}")

    # --- coupling ---------------------------------------------------------
    if policy == "disabled":
        report.require("disabled_cache_publishes_no_hit", hits == 0,
                       f"cache_policy disabled but cache_hits {hits}")
        report.require("disabled_cache_publishes_no_resident", resident == 0,
                       f"cache_policy disabled but cache_resident_bytes {resident}")
    if window == 0:
        report.require("zero_window_publishes_no_prefetch", prefetch_submitted == 0,
                       f"prefetch_blocks 0 but prefetch_submitted {prefetch_submitted}")
    if backend == "synchronous":
        report.require("sync_backend_publishes_no_prefetch", prefetch_submitted == 0,
                       f"io_backend synchronous but prefetch_submitted {prefetch_submitted}")
    if str(config.get("purpose")) == "measurement":
        report.require("no_read_errors", read_errors == 0,
                       f"io_read_errors {read_errors} in a measurement run")

    # --- derived ----------------------------------------------------------
    demands = hits + misses
    hit_rate = hits / demands if demands else 0.0
    ratio = completed_bytes / (demands * block_bytes) if demands else 0.0
    report.require("demands_positive", demands > 0,
                   f"logical block demands = {demands}")

    return report, {
        "run": run.name,
        "policy": policy, "backend": backend,
        "cache_bytes": cache_bytes, "block_bytes": block_bytes,
        "hits": hits, "misses": misses, "demands": demands,
        "hit_rate": hit_rate,
        "read_calls": submitted, "completed_bytes": completed_bytes,
        "completed_to_demand_byte_ratio": ratio,
        "prefetch_submitted": prefetch_submitted,
        "prefetch_consumed": prefetch_consumed,
        "prefetch_ready_at_demand": ready,
        "peak_inflight_reads": peak_inflight,
        "peak_prefetch_reads": peak_prefetch,
        "failed_checks": report.failed,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path, action="append", required=True,
                        help="Run directory; repeat for several runs")
    parser.add_argument("--json", type=Path, help="Write the full report here")
    args = parser.parse_args()

    runs = [r.resolve() for r in args.run]
    results, checks, failures = [], [], 0
    for run in runs:
        report, row = verify(run)
        results.append(row)
        for check in report.checks:
            checks.append({"run": run.name, **check})
        failures += report.failed

    payload = {"runs": results, "checks": checks, "failed_checks": failures}
    if args.json:
        args.json.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    for row in results:
        status = "PASS" if row["failed_checks"] == 0 else "FAIL"
        print(f"{status}: {row['run']} [{row['policy']}/{row['backend']}] "
              f"hit_rate={row['hit_rate']:.4f} reads={row['read_calls']} "
              f"prefetch={row['prefetch_submitted']}")
    for check in checks:
        if check["status"] == "fail":
            print(f"  FAIL {check['run']}: {check['name']} — {check['detail']}",
                  file=sys.stderr)
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
