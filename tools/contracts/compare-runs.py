"""Compare two EVAL-1.1 run directories produced by the same command.

Stage 4 requires that repeating one command is explainable. That is a stronger
statement than "the numbers are close": a re-run must either reproduce a value
exactly, or differ for a reason the protocol already allows. This module encodes
that split so the claim is checkable instead of asserted.

Three classes of state are distinguished:

``identical``   Bit-for-bit reproducibility is required. These are the inputs,
                plan and semantic outputs. A difference here is a defect.
``derived``     Bytes are expected to differ, but the *content digest* the
                protocol pins must match. A first build mints a fresh segment
                UUID, so the file name and the JSON that records it change while
                ``graph.bin``/``versions.bin`` stay byte-identical.
``variable``    Wall-clock derived. Allowed to move, and each movement is
                reported with the tolerance it was judged against.

Exit codes follow the rest of the evaluation suite: 0 clean, 1 divergence,
2 usage or input error.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

SCHEMA_VERSION = 1
PROTOCOL = "EVAL-1.1"

# A re-run on an idle machine still sees scheduler jitter. These bounds are wide
# enough to absorb that and narrow enough to catch a real behaviour change.
QPS_TOLERANCE = 0.10
SERVICE_MODE_BIN_TOLERANCE = 1


class Divergence(Exception):
    """A difference the protocol does not explain."""


def load_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def load_rows(path: Path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip()]


def plan_signature(run: Path):
    """The query plan: which query is issued, when, and in which order."""
    return [(row["arrival_index"], row["operation_id"], row["query_id"], row["query_row"])
            for row in load_rows(run / "trace.jsonl")]


def segment_content(run: Path):
    """Map logical file name -> content digest recorded in segment.json.

    The digest, not the outer file hash, is what the manifest pins. Reading it
    from the segment metadata keeps the comparison valid across segment UUIDs.
    """
    segments = run / "index" / "segments"
    if not segments.is_dir():
        return {}
    digests = {}
    for segment in sorted(segments.iterdir()):
        if not segment.is_dir():
            continue
        metadata = load_json(segment / "segment.json")
        for entry in metadata.get("files", []):
            digests[entry["path"]] = entry["sha256"]
    return digests


def histogram_mode(bins):
    """Index of the most populated latency bin; ties resolve to the lower bin."""
    best_index, best_count = 0, -1
    for index, value in enumerate(bins):
        count = int(value)
        if count > best_count:
            best_index, best_count = index, count
    return best_index


def latency_mode(summary, stage):
    try:
        return histogram_mode(summary["latencies"]["ok"][stage]["power_of_two_ns_bins"])
    except (KeyError, TypeError):
        return None


def compare(left: Path, right: Path):
    findings = {"identical": [], "derived": [], "variable": [], "divergences": []}

    def same(group, name, a, b, note=""):
        if a == b:
            findings[group].append({"item": name, "status": "reproduced", "note": note})
        else:
            findings["divergences"].append({"item": name, "left": a, "right": b,
                                            "note": note})

    def free(name, a, b, judged, note=""):
        findings["variable"].append({"item": name, "left": a, "right": b,
                                     "judgement": judged, "note": note})

    # --- inputs and plan: must be reproduced exactly -----------------------
    same("identical", "trace.plan", plan_signature(left), plan_signature(right),
         "arrival_index/operation_id/query_id/query_row sequence")

    counters_l, counters_r = load_json(left / "counters.json"), load_json(right / "counters.json")
    for key in ("purpose", "duration_ns", "measurement_start_sequence",
                "measurement_end_sequence", "run_incomplete", "requires_review"):
        same("identical", f"counters.{key}", counters_l.get(key), counters_r.get(key))
    for kind in ("query", "insert", "upsert"):
        for field in ("scheduled", "succeeded", "failed", "rejected"):
            same("identical", f"counters.by_type.{kind}.{field}",
                 counters_l["by_type"][kind][field], counters_r["by_type"][kind][field])
    same("identical", "counters.engine_counters",
         counters_l.get("engine_counters"), counters_r.get("engine_counters"),
         "logical IO and write counters are deterministic for a static index")

    # --- answers: recall must land on the same per-query result ------------
    verdict_l = load_json(left / "evaluation.json")
    verdict_r = load_json(right / "evaluation.json")
    per_query = lambda doc: [(row["operation_id"], row["query_id"], row["snapshot_sequence"],
                              row["matched"], row["returned"])
                             for row in doc["recall_at_k"]["per_query"]]
    same("identical", "evaluation.recall.per_query", per_query(verdict_l), per_query(verdict_r),
         "independent answers are recomputed from the same base vectors")
    same("identical", "evaluation.recall.mean",
         verdict_l["recall_at_k"]["mean"], verdict_r["recall_at_k"]["mean"])
    same("identical", "evaluation.checks", verdict_l.get("checks"), verdict_r.get("checks"),
         "every EVAL-1.1 check must reach the same conclusion")
    same("identical", "evaluation.verdict", verdict_l.get("verdict"), verdict_r.get("verdict"))

    # --- index content: digests pinned by the manifest ---------------------
    same("derived", "index.segment_content_digests", segment_content(left),
         segment_content(right),
         "segment UUID and its JSON record differ per build; the pinned content digests must not")

    manifests = lambda run: sorted(p.name for p in (run / "index" / "manifests").iterdir())
    same("identical", "index.manifest_generations", manifests(left), manifests(right))

    # --- wall clock: allowed to move, but bounded --------------------------
    summary_l, summary_r = load_json(left / "summary.json"), load_json(right / "summary.json")
    # window_completed_qps is the rate the index sustained inside the measurement
    # window. drained_qps divides by an open-ended drain interval, so it moves
    # whenever the last in-flight request happens to land late; it is reported
    # but never used as the reproducibility criterion.
    qps_l, qps_r = summary_l.get("window_completed_qps"), summary_r.get("window_completed_qps")
    if isinstance(qps_l, (int, float)) and isinstance(qps_r, (int, float)) and qps_l > 0:
        drift = abs(qps_r - qps_l) / qps_l
        free("summary.window_completed_qps", qps_l, qps_r,
             "within tolerance" if drift <= QPS_TOLERANCE else "exceeds tolerance",
             f"relative drift {drift:.1%}, tolerance {QPS_TOLERANCE:.0%}")
        if drift > QPS_TOLERANCE:
            findings["divergences"].append({"item": "summary.window_completed_qps",
                                            "left": qps_l, "right": qps_r,
                                            "note": f"drift {drift:.1%} exceeds {QPS_TOLERANCE:.0%}"})
    free("summary.drained_qps", summary_l.get("drained_qps"), summary_r.get("drained_qps"),
         "informational", "divides by an open-ended drain interval; not a measurement")

    # A saturated run measures the machine, not the index. If the index cannot
    # sustain the configured arrival rate, comparing two runs is meaningless and
    # the configuration has to be fixed before the numbers mean anything.
    for side, summary in (("left", summary_l), ("right", summary_r)):
        try:
            effective = load_json((left if side == "left" else right) / "effective-config.json")
            target = effective["workload"]["target_ops_per_second"]
        except (KeyError, TypeError):
            continue
        achieved = summary.get("window_completed_qps")
        if not isinstance(achieved, (int, float)) or target <= 0:
            continue
        if achieved < 0.90 * target:
            findings["divergences"].append({
                "item": f"workload.saturation.{side}",
                "left": target, "right": achieved,
                "note": f"index sustained {achieved:.1f} ops/s against a configured "
                        f"{target:.1f}; the run is saturated and its rate is not a property of the index",
            })
    for stage in ("service", "queue", "end_to_end"):
        mode_l, mode_r = latency_mode(summary_l, stage), latency_mode(summary_r, stage)
        if mode_l is None or mode_r is None:
            continue
        delta = abs(mode_r - mode_l)
        free(f"summary.latency.{stage}.mode_bin", mode_l, mode_r,
             "within tolerance" if delta <= SERVICE_MODE_BIN_TOLERANCE else "exceeds tolerance",
             f"modal bin delta {delta}, tolerance {SERVICE_MODE_BIN_TOLERANCE}")
        if delta > SERVICE_MODE_BIN_TOLERANCE:
            findings["divergences"].append({"item": f"summary.latency.{stage}.mode_bin",
                                            "left": mode_l, "right": mode_r,
                                            "note": f"modal bin moved {delta} buckets"})
    free("counters.drain_ns", counters_l.get("drain_ns"), counters_r.get("drain_ns"),
         "informational", "time to drain in-flight work; not a measured quantity")

    findings["verdict"] = "pass" if not findings["divergences"] else "fail"
    return findings


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--left", required=True, type=Path)
    parser.add_argument("--right", required=True, type=Path)
    parser.add_argument("--write", type=Path, help="optional comparison.json path")
    arguments = parser.parse_args(argv)

    for path in (arguments.left, arguments.right):
        if not (path / "counters.json").is_file():
            print(f"not a run directory: {path}", file=sys.stderr)
            return 2

    findings = compare(arguments.left, arguments.right)
    findings.update({
        "schema_version": SCHEMA_VERSION,
        "protocol_version": PROTOCOL,
        "left": str(arguments.left),
        "right": str(arguments.right),
    })

    counts = {group: len(findings[group]) for group in ("identical", "derived", "variable")}
    print(f"identical={counts['identical']} derived={counts['derived']} "
          f"variable={counts['variable']} divergences={len(findings['divergences'])}")
    for entry in findings["variable"]:
        print(f"  ~ {entry['item']}: {entry['left']} -> {entry['right']} [{entry['judgement']}]")
    for entry in findings["divergences"]:
        print(f"  ! {entry['item']}: {entry['note']}")
    print(f"verdict={findings['verdict']}")

    if arguments.write:
        arguments.write.parent.mkdir(parents=True, exist_ok=True)
        arguments.write.write_text(json.dumps(findings, ensure_ascii=False, indent=2) + "\n",
                                   encoding="utf-8", newline="\n")
    return 0 if findings["verdict"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
