"""Focused negative cases for cross-language handoff and accounting mistakes."""

from copy import deepcopy
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location(
    "stage3_a_contract", Path(__file__).with_name("check-stage3-a.py"))
contract = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(contract)
ROOT = contract.ROOT


class ContractChecks(unittest.TestCase):
    def setUp(self):
        directory = ROOT / "evaluation/examples/stage3-a/normal"
        self.queries = contract.load_rows(directory / "queries.jsonl")
        self.writes = contract.load_rows(directory / "writes.jsonl")
        self.counters = contract.load(directory / "counters.json")
        self.config = contract.load(ROOT / "configs/examples/static-run.json")

    def check_run(self):
        contract.run_artifacts(self.queries, self.writes, self.counters)

    def test_valid_fixtures(self):
        contract.check_examples()

    def test_uint64_boundaries(self):
        self.assertEqual(contract.u64(str((1 << 64) - 1)), (1 << 64) - 1)
        for value in ("01", "-1", "1.0", "1e2", str(1 << 64), 1, True):
            with self.subTest(value=value), self.assertRaises(contract.ContractError):
                contract.u64(value)

    def test_json_duplicate_and_nonfinite(self):
        for value in ('{"id":"1","id":"2"}', '{"x":NaN}', '{"x":1e999}'):
            with self.subTest(value=value), self.assertRaises(contract.ContractError):
                contract.parse_json(value)

    def test_numeric_value_cannot_overflow_double(self):
        with self.assertRaises(contract.ContractError):
            contract.finite(10 ** 400)

    def test_bool_is_not_configuration_integer(self):
        self.config["engine"]["max_queries"] = True
        with self.assertRaises(contract.ContractError):
            contract.run_config(self.config)

    def test_unknown_configuration_field(self):
        self.config["engine"]["max_query"] = 8
        with self.assertRaises(contract.ContractError):
            contract.run_config(self.config)

    def test_prefetch_cannot_take_reserved_slots(self):
        self.config["engine"]["max_prefetch_reads"] = 64
        with self.assertRaises(contract.ContractError):
            contract.run_config(self.config)

    def test_rebuild_cannot_raise_total_memory(self):
        modes = contract.load(ROOT / "configs/examples/resource-modes.json")
        modes["rebuild_memory"]["total_bytes"] = "614400001"
        modes["rebuild_memory"]["reserve_bytes"] = "33554433"
        with self.assertRaises(contract.ContractError):
            contract.resource_modes(modes)

    def test_record_buffer_must_fit_result(self):
        self.config["measurement"]["max_record_bytes"] = 1024
        with self.assertRaises(contract.ContractError):
            contract.run_config(self.config)

    def test_rejection_has_no_api_start(self):
        self.queries[2]["api_start_ns"] = self.queries[2]["scheduled_ns"]
        with self.assertRaises(contract.ContractError):
            self.check_run()

    def test_failed_query_cannot_fabricate_snapshot(self):
        self.queries[1]["snapshot_sequence"] = "1"
        with self.assertRaises(contract.ContractError):
            self.check_run()

    def test_future_neighbor_version(self):
        self.queries[0]["neighbors"][0]["version"] = "2"
        with self.assertRaises(contract.ContractError):
            self.check_run()

    def test_duplicate_operation_across_files(self):
        self.writes[0]["operation_id"] = self.queries[0]["operation_id"]
        with self.assertRaises(contract.ContractError):
            self.check_run()

    def test_payload_path_escape(self):
        self.writes[0]["payload"]["file"] = "../updates.f32"
        with self.assertRaises(contract.ContractError):
            self.check_run()

    def test_success_receipt_prefix_gap(self):
        self.writes[0]["sequence"] = "2"
        self.counters["measurement_end_sequence"] = "2"
        with self.assertRaises(contract.ContractError):
            self.check_run()

    def test_hidden_uncertain_write(self):
        self.writes[1]["error"]["write_effect"] = "may_have_applied"
        self.writes[1]["error"]["attempted_sequence"] = "2"
        with self.assertRaises(contract.ContractError):
            self.check_run()

    def test_request_conservation(self):
        self.counters["by_type"]["query"]["scheduled"] = "4"
        with self.assertRaises(contract.ContractError):
            self.check_run()

    def test_query_must_see_acknowledged_write(self):
        self.queries[0]["snapshot_sequence"] = "0"
        self.queries[0]["neighbors"][0]["version"] = "0"
        with self.assertRaises(contract.ContractError):
            self.check_run()

    def test_equal_clock_ticks_do_not_prove_happens_before(self):
        self.writes[0]["completed_ns"] = self.queries[0]["api_start_ns"]
        self.queries[0]["snapshot_sequence"] = "0"
        self.queries[0]["neighbors"][0]["version"] = "0"
        self.check_run()

    def test_response_order_need_not_follow_sequence(self):
        self.writes[1].update(
            operation="upsert", id="0", sequence="2", status="ok", error=None,
            scheduled_ns="2000", api_start_ns="3000", completed_ns="14000")
        self.writes.reverse()
        self.queries[0]["snapshot_sequence"] = "2"
        self.counters["measurement_end_sequence"] = "2"
        self.counters["by_type"]["insert"].update(
            scheduled="1", failed="0", window_completed="1")
        self.counters["by_type"]["upsert"].update(
            scheduled="1", succeeded="1", window_completed="1", window_succeeded="1")
        self.check_run()

    def test_hidden_pending_requests(self):
        self.counters["by_type"]["query"].update(scheduled="4", pending="1")
        with self.assertRaises(contract.ContractError):
            self.check_run()
        self.counters["run_incomplete"] = True
        self.check_run()

    def test_end_of_window_is_drain(self):
        self.queries[0]["completed_ns"] = self.counters["duration_ns"]
        self.counters["by_type"]["query"].update(window_completed="2", window_succeeded="0")
        self.check_run()

    def test_measurement_requires_payload_checksum(self):
        row = deepcopy(self.writes[0])
        row["purpose"] = "measurement"
        with self.assertRaises(contract.ContractError):
            contract.request(row, "insert")


if __name__ == "__main__":
    unittest.main(verbosity=2)
