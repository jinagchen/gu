"""Regression tests for the Stage 5 verifier CLI and its JSON report."""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


VERIFIER = Path(__file__).with_name("verify-read-path.py")
BLOCK = 4096


class VerifyReadPathTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.run_dir = Path(temp.name)
        self.config = {
            "purpose": "measurement",
            "engine": {
                "memory": {"cache_bytes": "16384", "total_bytes": "65536"},
                "block_bytes": BLOCK,
                "max_inflight_reads": 4,
                "max_prefetch_reads": 2,
                "cache_policy": "clock",
                "io_backend": "windows_iocp",
            },
            "search": {"prefetch_blocks": 0},
        }
        self.counts = {
            "io_reads_submitted": "2",
            "io_read_bytes_completed": str(2 * BLOCK),
            "io_read_errors": "0",
            "prefetch_submitted": "0",
            "prefetch_consumed": "0",
        }
        self.counters = {"run_incomplete": False, "engine_counters": self.counts}
        self.path = {
            "cache_hits": "0",
            "cache_misses": "3",
            "prefetch_ready_at_demand": "0",
            "cache_resident_bytes": str(2 * BLOCK),
            "engine_accounted_bytes": "32768",
            "peak_inflight_reads": 2,
            "peak_prefetch_reads": 0,
        }

    def assert_checks(self, *expected_failures):
        files = {
            "effective-config.json": self.config,
            "counters.json": self.counters,
            "summary.json": {"read_path": self.path},
        }
        for name, data in files.items():
            (self.run_dir / name).write_text(json.dumps(data), encoding="utf-8")
        report = self.run_dir / "verification.json"
        result = subprocess.run(
            [sys.executable, "-X", "utf8", str(VERIFIER), "--run", str(self.run_dir),
             "--json", str(report)],
            capture_output=True, encoding="utf-8", timeout=10,
        )
        self.assertEqual(result.returncode, int(bool(expected_failures)),
                         result.stdout + result.stderr)
        payload = json.loads(report.read_text(encoding="utf-8"))
        failed = [row["name"] for row in payload["checks"] if row["status"] == "fail"]
        self.assertCountEqual(failed, expected_failures)
        self.assertEqual(payload["failed_checks"], len(expected_failures))

    def test_completed_fixed_size_reads(self):
        self.assert_checks()

    def test_missing_partial_and_extra_bytes_fail_for_a_complete_run(self):
        for completed in (0, 2 * BLOCK - 1, 2 * BLOCK + 1):
            with self.subTest(completed=completed):
                self.counts["io_read_bytes_completed"] = str(completed)
                self.assert_checks("completed_bytes_match_submitted_blocks")

    def test_no_io_when_all_demands_are_cache_hits(self):
        self.path.update(cache_hits="3", cache_misses="0")
        self.counts.update(io_reads_submitted="0", io_read_bytes_completed="0")
        self.assert_checks()

    def test_unused_prefetch_can_complete_more_bytes_than_demanded(self):
        self.config["search"]["prefetch_blocks"] = 2
        self.path.update(cache_misses="1", peak_prefetch_reads=1)
        self.counts["prefetch_submitted"] = "1"
        self.assert_checks()

    def test_incomplete_run_allows_unfinished_reads_but_not_extra_bytes(self):
        self.counters["run_incomplete"] = True
        self.counts["io_read_bytes_completed"] = str(BLOCK)
        self.assert_checks()
        self.counts["io_read_bytes_completed"] = str(3 * BLOCK)
        self.assert_checks("completed_bytes_bounded_by_submitted_blocks")

    def test_measurement_still_rejects_read_errors(self):
        self.counts.update(io_read_bytes_completed=str(BLOCK + 1), io_read_errors="1")
        self.assert_checks("no_read_errors")


if __name__ == "__main__":
    unittest.main()
