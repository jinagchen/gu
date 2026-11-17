"""Regression checks for INDEX-B-1.1 and dynamic EVAL-1.1 consumption."""

import contextlib
import io
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from evaluation import check, trace

FIXTURE = ROOT / "evaluation/examples/stage3-b"


class DynamicChecks(unittest.TestCase):
    def setUp(self):
        parent = ROOT / "build"
        parent.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="stage3-b-test-", dir=parent)
        self.addCleanup(self.temporary.cleanup)
        self.run = Path(self.temporary.name) / "run"
        shutil.copytree(FIXTURE, self.run)

    def change(self, name, mutate):
        path = self.run / name
        if name.endswith(".jsonl"):
            value = check.load_rows(path)
            mutate(value)
            trace.dump_rows(path, value)
        else:
            value = check.load(path)
            mutate(value)
            trace.dump(path, value)
        if name == "trace.jsonl":
            self.change("effective-config.json", lambda config: config["trace"].update(
                sha256=check.digest(path)))

    def rejects(self, message):
        with self.assertRaisesRegex(check.ContractError, message):
            check.evaluate(self.run)

    def counters(self):
        """Update only derived counts after a deliberately changed terminal status."""
        queries = check.load_rows(self.run / "queries.jsonl")
        writes = check.load_rows(self.run / "writes.jsonl")

        def update(value):
            duration = int(value["duration_ns"])
            for kind in ("query", "insert", "upsert"):
                records = queries if kind == "query" else [
                    row for row in writes if row["operation"] == kind]
                counts = dict.fromkeys(check.COUNT_FIELDS, 0)
                counts["scheduled"] = len(records)
                for row in records:
                    counts[{"ok": "succeeded", "error": "failed",
                            "rejected": "rejected"}[row["status"]]] += 1
                    if int(row["completed_ns"]) < duration:
                        counts["window_completed"] += 1
                        counts["window_succeeded"] += row["status"] == "ok"
                value["by_type"][kind] = {key: str(count) for key, count in counts.items()}
        self.change("counters.json", update)

    def answers(self):
        """Independent fixture oracle, including upserts that introduce new IDs."""
        config = check.load(self.run / "effective-config.json")
        dimension = config["dataset"]["dimension"]
        base_vectors = check.read_f32_rows(self.run / "inputs/base.f32", dimension)
        base_ids = check.read_u64_rows(self.run / "inputs/base.ids.u64")
        versions = [0] * len(base_ids)
        if "baseline" in config:
            versions = check.read_u64_rows(self.run / config["baseline"]["versions"])
        query_vectors = check.read_f32_rows(self.run / "inputs/queries.f32", dimension)
        query_ids = check.read_u64_rows(self.run / "inputs/query.ids.u64")
        queries_by_id = dict(zip(query_ids, query_vectors))
        updates = check.read_f32_rows(self.run / "inputs/updates.f32", dimension)
        writes = check.load_rows(self.run / "writes.jsonl")
        output = []
        records = check.load_rows(self.run / "queries.jsonl")
        for row in records:
            if row["status"] != "ok":
                continue
            state = {identifier: (version, vector) for identifier, version, vector in
                     zip(base_ids, versions, base_vectors)}
            for write in sorted(writes, key=lambda item: int(item["sequence"] or 0)):
                if write["status"] == "ok" and int(write["sequence"]) <= int(row["snapshot_sequence"]):
                    state[int(write["id"])] = (
                        int(write["sequence"]), updates[int(write["payload"]["row"])])
            truth = trace.exact_top_k(state, queries_by_id[int(row["query_id"])], row["k"])
            row["neighbors"] = [{"id": str(identifier), "version": str(version),
                                 "squared_distance": distance}
                                for distance, identifier, version in truth]
            output.append({
                **{key: row[key] for key in
                   ("schema_version", "protocol_version", "purpose", "run_id",
                    "operation_id", "query_id", "snapshot_sequence", "k")},
                "live_vectors": str(len(state)), "metric": "squared_l2",
                "method": "exact_scan_float64",
                "top_k_ids": [str(identifier) for _, identifier, _ in truth]})
        trace.dump_rows(self.run / "queries.jsonl", records)
        trace.dump_rows(self.run / "ground-truth.jsonl", output)

    def fail_write(self, row, status="error", effect="none"):
        row.update(status=status, sequence=None, error={
            "code": "resource_exhausted" if status == "rejected" else "io_error",
            "message": "injected", "system_error": 0, "write_effect": effect,
            "attempted_sequence": "10" if effect == "may_have_applied" else None})
        if status == "rejected":
            row["api_start_ns"] = None

    def measurement(self):
        """Synthetic evidence exercises validation, never a real measurement."""
        for name in ("queries.jsonl", "writes.jsonl", "trace.jsonl", "ground-truth.jsonl"):
            self.change(name, lambda rows: [row.update(purpose="measurement") for row in rows])
        for name in ("effective-config.json", "counters.json", "environment.json"):
            self.change(name, lambda value: value.update(purpose="measurement"))
        self.change("effective-config.json", lambda config: config.update(
            unbudgeted_metadata_bytes="0"))
        self.change("environment.json", lambda value: value.update(
            formal_engine_measurement=True, engine_options_applied=True,
            input_content_used=True, input_hashes_verified=True,
            code_commit_at_configure="a" * 40, build_config="Release",
            measurement_domain="synthetic process tree", participating_processes="one",
            log_volume="C:", log_filesystem="NTFS", job_limit_commit_bytes="268435456",
            direct_reads=True,
            sample_interval_ms="5", workers=1, runtime_threads=3,
            unbudgeted_metadata_bytes="0", external_job_metrics="job.json"))
        trace.dump(self.run / "job.json", {
            "schema_version": 1, "limit_commit_bytes": 268435456,
            "kernel_peak_job_commit_bytes": 100000000,
            "sampled_peak_private_bytes": 90000000,
            "sampled_peak_working_set_bytes": 110000000, "total_processes": 1,
            "sample_interval_ms": 5, "elapsed_ms": 20, "timed_out": False, "exit_code": 0})
        trace.dump_rows(self.run / "samples.jsonl", [{
            "schema_version": 1, "protocol_version": check.PROTOCOL,
            "purpose": "measurement", "run_id": "stage3-b-exact", "sampled_ns": "0",
            "private_bytes": "90000000", "working_set_bytes": "110000000",
            "kernel_peak_job_commit_bytes": "100000000", "unavailable_reason": None}])

    def test_baseline(self):
        result = check.evaluate(self.run)
        self.assertEqual(result["verdict"], "pass")
        self.assertEqual(result["recall_at_k"]["mean"], 1.0)

    def test_other_run(self):
        self.change("queries.jsonl", lambda rows: rows[0].update(run_id="other"))
        self.rejects("Mixed run")
        self.assertEqual(check.load(self.run / "evaluation.json")["verdict"], "invalid")

    def test_other_purpose(self):
        self.change("queries.jsonl", lambda rows: rows[0].update(purpose="measurement"))
        self.rejects("Mixed run")

    def test_duplicate_operation(self):
        self.change("queries.jsonl", lambda rows: rows[1].update(operation_id=rows[0]["operation_id"]))
        self.rejects("Duplicate operation ID")

    def test_cross_kind_duplicate(self):
        self.change("queries.jsonl", lambda rows: rows[0].update(operation_id="w-001"))
        self.rejects("Duplicate operation ID")

    def test_duplicate_trace(self):
        self.change("trace.jsonl", lambda rows: rows[6].update(operation_id=rows[5]["operation_id"]))
        self.rejects("Duplicate operation ID in trace")

    def test_wrong_trace_operation(self):
        self.change("trace.jsonl", lambda rows: rows[0].update(operation="insert"))
        self.rejects("Trace disagrees with write")

    def test_wrong_trace_payload(self):
        self.change("trace.jsonl", lambda rows: rows[0].update(payload_row="9"))
        self.rejects("Trace disagrees with write")

    def test_wrong_trace_query_row(self):
        self.change("trace.jsonl", lambda rows: rows[5].update(query_row="1"))
        self.rejects("query_id does not match query_row")

    def test_trace_does_not_accept_predicted_sequence(self):
        self.change("trace.jsonl", lambda rows: rows[0].update(sequence="1"))
        self.rejects("Fields mismatch")

    def test_trace_does_not_accept_predicted_snapshot(self):
        self.change("trace.jsonl", lambda rows: rows[5].update(committed_prefix_at_arrival="5"))
        self.rejects("Fields mismatch")

    def test_trace_arrival_is_not_bool(self):
        self.change("trace.jsonl", lambda rows: rows[0].update(arrival_index=False))
        self.rejects("bounded JSON integer")

    def test_short_success(self):
        self.change("queries.jsonl", lambda rows: rows[0]["neighbors"].pop())
        self.rejects("short of min")

    def test_bad_window(self):
        self.change("counters.json", lambda value: value["by_type"]["query"].update(
            window_completed="999", window_succeeded="999"))
        self.rejects("Invalid window counters")

    def test_plausible_but_wrong_window(self):
        self.change("counters.json", lambda value: value["by_type"]["query"].update(
            window_completed="5", window_succeeded="5"))
        self.rejects("Counter mismatch")

    def test_wrong_measurement_end(self):
        self.change("counters.json", lambda value: value.update(measurement_end_sequence="999"))
        self.rejects("prefix missing")

    def test_start_regression(self):
        self.change("counters.json", lambda value: value.update(
            measurement_start_sequence="11"))
        self.rejects("Sequence regression")

    def test_missing_environment(self):
        (self.run / "environment.json").unlink()
        self.rejects("Missing environment")

    def test_missing_answers(self):
        (self.run / "ground-truth.jsonl").unlink()
        self.rejects("Missing independent answers")

    def test_empty_answers(self):
        trace.dump_rows(self.run / "ground-truth.jsonl", [])
        self.rejects("Missing independent answer for")

    def test_bad_answer_header(self):
        self.change("ground-truth.jsonl", lambda rows: rows[0].update(run_id="other"))
        self.rejects("Mixed run")

    def test_bad_answer_id_type(self):
        self.change("ground-truth.jsonl", lambda rows: rows[0]["top_k_ids"].__setitem__(0, 15))
        self.rejects("canonical uint64")

    def test_distance_cannot_overflow_double(self):
        self.change("queries.jsonl", lambda rows: rows[0]["neighbors"][0].update(
            squared_distance=10 ** 500))
        self.rejects("Expected finite number")

    def test_missing_hashes(self):
        self.change("effective-config.json", lambda config: config["dataset"].pop("sha256"))
        self.rejects("Fields mismatch")

    def test_missing_trace_hash(self):
        self.change("effective-config.json", lambda config: config["trace"].pop("sha256"))
        self.rejects("Invalid trace fields")

    def test_updates_hash(self):
        path = self.run / "inputs/updates.f32"
        path.write_bytes(path.read_bytes()[::-1])
        self.rejects("Checksum mismatch")

    def test_payload_hash_each_record(self):
        self.change("writes.jsonl", lambda rows: rows[1]["payload"].update(sha256="0" * 64))
        self.rejects("Payload checksum differs")

    def test_query_id_file_is_used(self):
        path = self.run / "inputs/query.ids.u64"
        trace.write_u64(path, [100, 101, 102, 103, 104, 105])
        self.change("effective-config.json", lambda config: config["dataset"]["sha256"].update(
            query_ids=check.digest(path)))
        self.rejects("query_id does not match query_row")

    def test_arbitrary_query_ids(self):
        path = self.run / "inputs/query.ids.u64"
        trace.write_u64(path, [100, 101, 102, 103, 104, 105])
        self.change("effective-config.json", lambda config: config["dataset"]["sha256"].update(
            query_ids=check.digest(path)))
        for name in ("trace.jsonl", "queries.jsonl", "ground-truth.jsonl"):
            self.change(name, lambda rows: [
                row.update(query_id=str(int(row["query_id"]) + 100))
                for row in rows if "query_id" in row])
        self.assertEqual(check.evaluate(self.run)["recall_at_k"]["mean"], 1.0)

    def test_repeated_query_id_with_distinct_operations(self):
        self.change("queries.jsonl", lambda rows: rows[1].update(query_id="0"))
        self.change("trace.jsonl", lambda rows: rows[6].update(query_id="0", query_row="0"))
        self.answers()
        self.assertEqual(check.evaluate(self.run)["queries"]["total"], 6)

    def test_unknown_engine_field(self):
        self.change("effective-config.json", lambda config: config["engine"].update(block_size=4096))
        self.rejects("Fields mismatch")

    def test_readview_capacity(self):
        self.change("effective-config.json", lambda config: config["engine"]["resources"].update(
            max_disk_segments=17))
        self.rejects("segment capacity exceeded")

    def test_prefetch_respects_reservations(self):
        self.change("effective-config.json", lambda config: config["engine"].update(
            max_prefetch_reads=64))
        self.rejects("Prefetch consumes reserved slots")

    def test_zero_cache_budget(self):
        def update(config):
            budget = config["engine"]["memory"]
            budget["workspace_bytes"] = str(int(budget["workspace_bytes"]) +
                                            int(budget["cache_bytes"]))
            budget["cache_bytes"] = "0"
        self.change("effective-config.json", update)
        self.assertEqual(check.evaluate(self.run)["verdict"], "pass")

    def test_exact_wal_frame_capacity(self):
        self.change("effective-config.json", lambda config: config["engine"]["wal"].update(
            max_frame_bytes="88", rotation_bytes="152"))
        self.assertEqual(check.evaluate(self.run)["verdict"], "pass")

    def test_measurement_cannot_omit_evidence(self):
        for name in ("queries.jsonl", "writes.jsonl", "trace.jsonl", "ground-truth.jsonl"):
            self.change(name, lambda rows: [row.update(purpose="measurement") for row in rows])
        for name in ("effective-config.json", "counters.json", "environment.json"):
            self.change(name, lambda value: value.update(purpose="measurement"))
        self.rejects("Measurement environment must confirm")

    def test_valid_failed_query(self):
        def update(rows):
            row = rows[0]
            row.update(status="error", snapshot_sequence=None, view_generation=None,
                       neighbors=[], error={"code": "io_error", "message": "injected",
                       "system_error": 1117, "write_effect": "none", "attempted_sequence": None})
        self.change("queries.jsonl", update)
        self.counters()
        self.answers()
        result = check.evaluate(self.run)
        self.assertEqual(result["queries"]["failed"], 1)
        self.assertAlmostEqual(result["recall_at_k"]["mean"], 5 / 6)

    def test_valid_rejected_query(self):
        def update(rows):
            row = rows[0]
            row.update(status="rejected", api_start_ns=None, snapshot_sequence=None,
                       view_generation=None, neighbors=[], error={
                       "code": "resource_exhausted", "message": "injected", "system_error": 0,
                       "write_effect": "none", "attempted_sequence": None})
        self.change("queries.jsonl", update)
        self.counters()
        self.answers()
        self.assertEqual(check.evaluate(self.run)["queries"]["rejected"], 1)

    def test_overlapping_write_old_snapshot(self):
        self.change("queries.jsonl", lambda rows: rows[0].update(
            snapshot_sequence="4", view_generation="5"))
        queries = check.load_rows(self.run / "queries.jsonl")
        self.change("writes.jsonl", lambda rows: rows[4].update(
            completed_ns=str(int(queries[0]["completed_ns"]) + 1)))
        self.answers()
        self.assertEqual(check.evaluate(self.run)["verdict"], "pass")

    def test_arrival_is_not_api_capture(self):
        writes = check.load_rows(self.run / "writes.jsonl")
        completed = int(writes[5]["completed_ns"])
        self.change("queries.jsonl", lambda rows: [
            row.update(api_start_ns=str(completed + 1000),
                       completed_ns=str(completed + 100000), snapshot_sequence="6")
            for row in rows[:2]])
        self.answers()
        self.assertEqual(check.evaluate(self.run)["verdict"], "pass")

    def test_upsert_inserts_unknown_id(self):
        self.change("writes.jsonl", lambda rows: rows[-1].update(id="99999", operation="upsert"))
        self.change("trace.jsonl", lambda rows: rows[13].update(id="99999", operation="upsert"))
        self.counters()
        self.answers()
        self.assertEqual(check.evaluate(self.run)["verdict"], "pass")

    def test_failed_write_has_no_receipt(self):
        self.change("writes.jsonl", lambda rows: self.fail_write(rows[-1]))
        self.change("queries.jsonl", lambda rows: [
            row.update(snapshot_sequence="9") for row in rows if row["snapshot_sequence"] == "10"])
        self.change("counters.json", lambda value: value.update(measurement_end_sequence="9"))
        self.counters()
        self.answers()
        self.assertEqual(check.evaluate(self.run)["writes"]["acknowledged"], 9)

    def test_rejected_write_has_no_receipt(self):
        self.change("writes.jsonl", lambda rows: self.fail_write(rows[-1], status="rejected"))
        self.change("queries.jsonl", lambda rows: [
            row.update(snapshot_sequence="9") for row in rows if row["snapshot_sequence"] == "10"])
        self.change("counters.json", lambda value: value.update(measurement_end_sequence="9"))
        self.counters()
        self.answers()
        self.assertEqual(check.evaluate(self.run)["writes"]["acknowledged"], 9)

    def test_response_order_does_not_determine_sequence(self):
        self.change("writes.jsonl", lambda rows: rows.reverse())
        self.assertEqual(check.evaluate(self.run)["verdict"], "pass")

    def test_requires_review_is_not_scored(self):
        self.change("counters.json", lambda value: value.update(requires_review=True))
        result = check.evaluate(self.run)
        self.assertEqual(result["verdict"], "requires_review")
        self.assertIsNone(result["recall_at_k"]["mean"])
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(check.main(["--run", str(self.run)]), 2)

    def test_uncertain_write_not_replayed_as_success(self):
        self.change("writes.jsonl", lambda rows: self.fail_write(rows[-1], effect="may_have_applied"))
        self.change("counters.json", lambda value: value.update(requires_review=True))
        self.counters()
        result = check.evaluate(self.run)
        self.assertEqual(result["verdict"], "requires_review")
        self.assertTrue(result["writes"]["uncertain"])
        self.assertIsNone(result["recall_at_k"]["mean"])

    def test_uncertain_write_cannot_be_hidden(self):
        self.change("writes.jsonl", lambda rows: self.fail_write(rows[-1], effect="may_have_applied"))
        self.counters()
        self.rejects("Uncertain write hidden")

    def test_incomplete_with_real_pending(self):
        self.change("queries.jsonl", lambda rows: rows.pop())
        self.counters()
        self.change("counters.json", lambda value: (
            value.update(run_incomplete=True),
            value["by_type"]["query"].update(scheduled="6", pending="1")))
        result = check.evaluate(self.run)
        self.assertEqual(result["verdict"], "incomplete")
        self.assertEqual(result["queries"]["pending"], 1)
        self.assertIsNone(result["recall_at_k"]["mean"])

    def test_pending_cannot_be_hidden(self):
        self.change("queries.jsonl", lambda rows: rows.pop())
        self.counters()
        self.rejects("Missing terminal records")

    def test_nonzero_baseline_with_versions(self):
        path = self.run / "inputs/base.versions.u64"
        trace.write_u64(path, [0] * 22 + [7, 100])
        self.change("effective-config.json", lambda config: config.update(baseline={
            "sequence": "100", "versions": "inputs/base.versions.u64", "sha256": check.digest(path)}))
        self.change("writes.jsonl", lambda rows: [
            row.update(sequence=str(int(row["sequence"]) + 100)) for row in rows])
        self.change("queries.jsonl", lambda rows: [
            row.update(snapshot_sequence=str(int(row["snapshot_sequence"]) + 100)) for row in rows])
        self.change("counters.json", lambda value: value.update(
            measurement_start_sequence="100", measurement_end_sequence="110"))
        self.answers()
        self.assertEqual(check.evaluate(self.run)["writes"]["last_sequence"], 110)

    def test_nonzero_start_needs_baseline(self):
        self.change("counters.json", lambda value: value.update(measurement_start_sequence="1"))
        self.rejects("requires an explicit baseline")

    def test_noncanonical_answer_version(self):
        self.change("ground-truth.jsonl", lambda rows: rows[0].update(snapshot_sequence="05"))
        self.rejects("canonical uint64")

    def test_time_overflow(self):
        self.change("counters.json", lambda value: value.update(drain_ns=str(check.UINT64_MAX)))
        self.rejects("End time overflow")

    def test_generator_preserves_existing_directory(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            trace.main(["--output", str(self.run)])
        self.assertEqual(raised.exception.code, 2)

    def test_nonoverlapping_writes_cannot_reverse_sequences(self):
        self.change("writes.jsonl", lambda rows: (
            rows[0].update(sequence="2"), rows[1].update(sequence="1")))
        self.answers()
        self.rejects("Write sequence contradicts")

    def test_overlapping_writes_can_reverse_sequences(self):
        def update(rows):
            rows[0].update(sequence="2", completed_ns=rows[1]["completed_ns"])
            rows[1].update(sequence="1")
        self.change("writes.jsonl", update)
        self.answers()
        self.assertEqual(check.evaluate(self.run)["verdict"], "pass")

    def test_equal_clock_ticks_do_not_prove_write_order(self):
        def update(rows):
            rows[0].update(sequence="2", completed_ns=rows[1]["api_start_ns"])
            rows[1].update(sequence="1")
        self.change("writes.jsonl", update)
        self.answers()
        self.assertEqual(check.evaluate(self.run)["verdict"], "pass")

    def test_query_snapshot_cannot_regress_after_completed_query(self):
        queries = check.load_rows(self.run / "queries.jsonl")
        self.change("writes.jsonl", lambda rows: rows[4].update(
            completed_ns=str(int(queries[1]["completed_ns"]) + 1)))
        self.change("queries.jsonl", lambda rows: rows[1].update(
            snapshot_sequence="4", view_generation="5"))
        self.answers()
        self.rejects("Query snapshot contradicts")

    def test_overlapping_queries_can_complete_in_reverse_snapshot_order(self):
        queries = check.load_rows(self.run / "queries.jsonl")
        self.change("writes.jsonl", lambda rows: rows[4].update(
            completed_ns=str(int(queries[1]["completed_ns"]) + 1)))
        self.change("queries.jsonl", lambda rows: (
            rows[0].update(completed_ns=str(int(queries[1]["completed_ns"]) + 2)),
            rows[1].update(snapshot_sequence="4", view_generation="5")))
        self.answers()
        self.assertEqual(check.evaluate(self.run)["verdict"], "pass")

    def test_uncertain_write_stops_later_successes(self):
        self.change("writes.jsonl", lambda rows: self.fail_write(
            rows[0], effect="may_have_applied"))
        self.change("counters.json", lambda value: value.update(requires_review=True))
        self.counters()
        self.rejects("Successful write after uncertain")

    def test_incomplete_still_checks_neighbour_types(self):
        self.change("counters.json", lambda value: value.update(run_incomplete=True))
        self.change("queries.jsonl", lambda rows: rows[0]["neighbors"][0].update(id=15))
        self.rejects("canonical uint64")

    def test_review_still_checks_neighbour_versions(self):
        self.change("counters.json", lambda value: value.update(requires_review=True))
        self.change("queries.jsonl", lambda rows: rows[0]["neighbors"][0].update(version="6"))
        self.rejects("newer than the snapshot")

    def test_wrong_kind_cannot_be_disguised_as_pending(self):
        self.change("writes.jsonl", lambda rows: rows[0].update(operation_id="q-000"))
        self.change("queries.jsonl", lambda rows: rows.pop(0))
        self.counters()
        def update(value):
            value["run_incomplete"] = True
            for kind in ("query", "upsert"):
                counts = value["by_type"][kind]
                counts.update(pending="1", scheduled=str(int(counts["scheduled"]) + 1))
        self.change("counters.json", update)
        self.rejects("Result kind differs")

    def test_extra_answer_operation_is_rejected(self):
        self.change("ground-truth.jsonl", lambda rows: rows.append(
            dict(rows[0], operation_id="phantom")))
        self.rejects("Answer operation")

    def test_extra_answer_snapshot_is_rejected(self):
        self.change("ground-truth.jsonl", lambda rows: rows.append(
            dict(rows[0], snapshot_sequence="4")))
        self.rejects("Answer join")

    def test_baseline_versions_must_be_possible_global_sequences(self):
        self.test_nonzero_baseline_with_versions()
        path = self.run / "inputs/base.versions.u64"
        trace.write_u64(path, [0] * 21 + [7, 7, 100])
        self.change("effective-config.json", lambda config: config["baseline"].update(
            sha256=check.digest(path)))
        self.rejects("Duplicate nonzero baseline version")

    def test_baseline_must_contain_latest_sequence(self):
        self.test_nonzero_baseline_with_versions()
        path = self.run / "inputs/base.versions.u64"
        trace.write_u64(path, [0] * 23 + [99])
        self.change("effective-config.json", lambda config: config["baseline"].update(
            sha256=check.digest(path)))
        self.rejects("Baseline omits the latest")

    def test_typed_measurement_evidence(self):
        self.measurement()
        self.assertEqual(check.evaluate(self.run)["verdict"], "pass")

    def test_empty_job_evidence(self):
        self.measurement()
        trace.dump(self.run / "job.json", {})
        self.rejects("Fields mismatch")

    def test_job_limit_mismatch(self):
        self.measurement()
        self.change("job.json", lambda value: value.update(limit_commit_bytes=134217728))
        self.rejects("Job limit differs")

    def test_job_timeout(self):
        self.measurement()
        self.change("job.json", lambda value: value.update(timed_out=True))
        self.rejects("Job did not complete successfully")

    def test_job_peak_above_limit_preserves_evidence_for_review(self):
        self.measurement()
        self.change("job.json", lambda value: value.update(kernel_peak_job_commit_bytes=268435457))
        result = check.evaluate(self.run)
        self.assertEqual(result["verdict"], "requires_review")
        self.assertTrue(result["requires_review"])
        self.assertEqual(result["resource_evidence"]["kernel_peak_job_commit_bytes"], "268435457")
        self.assertTrue(result["resource_evidence"]["kernel_peak_exceeds_limit"])
        self.assertIsNone(result["recall_at_k"]["mean"])
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(check.main(["--run", str(self.run)]), 2)

    def test_measurement_direct_reads_must_match_configuration(self):
        self.measurement()
        self.change("environment.json", lambda value: value.update(direct_reads=False))
        self.rejects("Measured direct_reads differs")

    def test_measurement_direct_reads_must_be_boolean(self):
        self.measurement()
        self.change("environment.json", lambda value: value.update(direct_reads="true"))
        self.rejects("Expected JSON boolean")

    def test_resource_peak_cannot_regress(self):
        self.measurement()
        self.change("samples.jsonl", lambda rows: rows.append(
            dict(rows[0], sampled_ns="1", kernel_peak_job_commit_bytes="99999999")))
        self.rejects("Resource Job peak regressed")

    def test_resource_peak_can_grow_after_unavailable_sample(self):
        self.measurement()
        def update(rows):
            rows[0]["kernel_peak_job_commit_bytes"] = "80000000"
            rows.append(dict(rows[0], sampled_ns="1", kernel_peak_job_commit_bytes=None,
                             unavailable_reason="injected observation failure"))
            rows.append(dict(rows[0], sampled_ns="2", kernel_peak_job_commit_bytes="90000000"))
        self.change("samples.jsonl", update)
        self.assertEqual(check.evaluate(self.run)["verdict"], "pass")

    def test_resource_peak_cannot_regress_across_unavailable_sample(self):
        self.measurement()
        def update(rows):
            rows.append(dict(rows[0], sampled_ns="1", kernel_peak_job_commit_bytes=None,
                             unavailable_reason="injected observation failure"))
            rows.append(dict(rows[0], sampled_ns="2", kernel_peak_job_commit_bytes="99999999"))
        self.change("samples.jsonl", update)
        self.rejects("Resource Job peak regressed")

    def test_measurement_samples_are_required(self):
        self.measurement()
        (self.run / "samples.jsonl").unlink()
        self.rejects("Missing samples")

    def test_measurement_samples_cannot_belong_to_other_run(self):
        self.measurement()
        self.change("samples.jsonl", lambda rows: rows[0].update(run_id="other"))
        self.rejects("Mixed run")

    def test_measurement_metadata_disclosure_must_match(self):
        self.measurement()
        self.change("environment.json", lambda value: value.update(unbudgeted_metadata_bytes="1"))
        self.rejects("Unbudgeted metadata disclosure differs")

    def test_optional_workload_cannot_be_ignored(self):
        self.change("effective-config.json", lambda config: config.update(workload={"typo": 1}))
        self.rejects("Fields mismatch")

    def test_optional_snapshot_policy_cannot_be_ignored(self):
        self.change("effective-config.json", lambda config: config["trace"].update(
            snapshot_policy={"mode": "arrival_prefix", "min_distinct_snapshots": 2,
                             "max_queries_per_snapshot": 4}))
        self.rejects("Unsupported snapshot policy")

    def test_optional_measurement_cannot_be_ignored(self):
        self.change("effective-config.json", lambda config: config.update(measurement={"typo": 1}))
        self.rejects("Fields mismatch")

    def test_generator_direct_call_preserves_existing_directory(self):
        path = self.run / "inputs/base.f32"
        before = check.digest(path)
        with self.assertRaises(FileExistsError):
            trace.build_fixture(self.run, 123, 24, 4, 10, 6, 10)
        self.assertEqual(check.digest(path), before)

    def test_generator_supports_one_write_and_one_query(self):
        output = self.run.parent / "tiny"
        summary = trace.build_fixture(output, 1, 4, 2, 1, 1, 2)
        self.assertEqual(summary["snapshots"], [1])
        self.assertEqual(check.evaluate(output)["verdict"], "pass")

    def test_generator_ensures_snapshot_coverage_for_small_seeds(self):
        for seed in range(16):
            with self.subTest(seed=seed):
                output = self.run.parent / f"seed-{seed}"
                summary = trace.build_fixture(output, seed, 4, 2, 2, 2, 2)
                self.assertEqual(len(summary["snapshots"]), 2)
                self.assertEqual(check.evaluate(output)["verdict"], "pass")

    def test_generator_rejects_invalid_counts_before_creating_directory(self):
        output = self.run.parent / "invalid"
        with self.assertRaisesRegex(ValueError, "positive"):
            trace.build_fixture(output, 1, 4, 0, 2, 2, 2)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
