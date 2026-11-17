"""FORMAT-1 manifest contract examples; this does not implement the C++ codec."""

import copy
import re
import sys
import unittest
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from evaluation import check

FIXTURE = ROOT / "configs/examples/manifest.json"
MAX_BYTES = 65536


def bounded_text(value, maximum):
    check.text(value, maximum)
    check.require(all(ord(character) >= 32 for character in value),
                  "Manifest text contains a control character")


def canonical_uuid(value):
    bounded_text(value, 36)
    try:
        parsed = uuid.UUID(value)
    except ValueError as error:
        raise check.ContractError("Invalid UUID") from error
    check.require(str(parsed) == value and parsed.int != 0, "Noncanonical or zero UUID")


def validate_manifest(value):
    check.shape(value, ("format_version", "instance_id", "generation",
                       "checkpoint_sequence", "dimension", "metric", "block_bytes",
                       "live_count", "adapter", "layout", "segments", "wal_replay_after"))
    check.require(type(value["format_version"]) is int and value["format_version"] == 1,
                  "Unsupported manifest format")
    canonical_uuid(value["instance_id"])
    generation = check.u64(value["generation"], positive=True)
    checkpoint = check.u64(value["checkpoint_sequence"])
    check.require(check.u64(value["wal_replay_after"]) == checkpoint, "WAL replay boundary")
    dimension = check.u32(value["dimension"], positive=True)
    check.require(dimension <= 65536 and value["metric"] == "squared_l2",
                  "Unsupported geometry")
    block = check.u32(value["block_bytes"], positive=True)
    check.require(block & (block - 1) == 0, "Block size must be a power of two")
    live = check.u64(value["live_count"])

    adapter = value["adapter"]
    check.shape(adapter, ("name", "commit", "native_version", "parameters"))
    for name in ("name", "commit", "native_version"):
        bounded_text(adapter[name], 128)
    check.require(re.fullmatch(r"[0-9a-f]{40}", adapter["commit"]) is not None,
                  "Adapter needs an exact commit")
    bounded_text(adapter["parameters"], 512)

    layout = value["layout"]
    check.shape(layout, ("dimension", "max_degree", "row_bytes", "vector_offset",
                        "neighbor_offset", "metadata_offset"))
    for number in layout.values():
        check.u32(number)
    check.require(layout["dimension"] == dimension and 0 < layout["max_degree"] <= 512,
                  "Layout geometry")
    row = layout["row_bytes"]
    check.require(row > 0 and row % block == 0, "Row alignment")
    # Neighbor records contain a uint32 count plus max_degree uint32 node IDs.
    ranges = sorted((layout[offset], layout[offset] + length) for offset, length in (
        ("vector_offset", dimension * 4),
        ("neighbor_offset", (layout["max_degree"] + 1) * 4),
        ("metadata_offset", 16)))
    check.require(all(end <= row for _, end in ranges), "Layout field exceeds row")
    check.require(all(left[1] <= right[0] for left, right in zip(ranges, ranges[1:])),
                  "Layout fields overlap")

    segments = value["segments"]
    check.require(type(segments) is list and len(segments) <= 16, "Segment count")
    ids, uuids, total_nodes = set(), set(), 0
    for segment in segments:
        check.shape(segment, ("id", "uuid", "generation", "kind", "covered_sequence",
                              "node_count", "file_bytes", "content_sha256", "metadata"))
        identifier = check.u64(segment["id"], positive=True)
        canonical_uuid(segment["uuid"])
        check.require(identifier not in ids and segment["uuid"] not in uuids,
                      "Duplicate segment identity")
        ids.add(identifier)
        uuids.add(segment["uuid"])
        check.require(0 < check.u64(segment["generation"]) <= generation,
                      "Segment generation")
        covered = check.u64(segment["covered_sequence"])
        check.require(covered <= checkpoint, "Segment above checkpoint")
        check.require(segment["kind"] in ("base", "merged", "flushed_delta"), "Segment kind")
        check.require(segment["kind"] != "base" or covered == 0, "Base sequence")
        nodes = check.u64(segment["node_count"], positive=True)
        check.require(nodes <= (1 << 32), "Native node address range")
        total_nodes += nodes
        file_bytes = check.u64(segment["file_bytes"], positive=True)
        check.require(nodes * row <= file_bytes and file_bytes % block == 0,
                      "Segment length")
        check.checksum(segment["content_sha256"], "measurement")
        metadata = segment["metadata"]
        check.shape(metadata, ("path", "logical_bytes", "physical_bytes", "sha256"))
        check.relative_path(metadata["path"])
        check.require(metadata["path"] == f"segments/{segment['uuid']}/segment.json",
                      "Metadata path must match the segment UUID")
        logical = check.u64(metadata["logical_bytes"], positive=True)
        physical = check.u64(metadata["physical_bytes"], positive=True)
        check.require(logical <= physical, "Metadata lengths")
        check.checksum(metadata["sha256"], "measurement")
    check.require(total_nodes <= check.UINT64_MAX and live <= total_nodes, "Live count")
    check.require(bool(segments) or (live == 0 and checkpoint == 0), "Empty durable prefix")


class ManifestContractChecks(unittest.TestCase):
    def setUp(self):
        self.manifest = check.load(FIXTURE)

    def rejects(self, mutation, reason):
        mutation(self.manifest)
        with self.assertRaisesRegex(check.ContractError, reason):
            validate_manifest(self.manifest)

    def test_valid_example_and_wire_bound(self):
        validate_manifest(self.manifest)
        self.assertLessEqual(len(FIXTURE.read_bytes()), MAX_BYTES)

    def test_empty_initial_index(self):
        self.manifest.update(segments=[], live_count="0")
        validate_manifest(self.manifest)

    def test_multiple_blocks_per_row(self):
        self.manifest["layout"]["row_bytes"] = 8192
        self.manifest["segments"][0]["file_bytes"] = str(24 * 8192)
        validate_manifest(self.manifest)

    def test_unknown_field(self):
        self.rejects(lambda value: value.update(wal_replay_aftr="0"), "Fields mismatch")

    def test_missing_adapter(self):
        self.rejects(lambda value: value.pop("adapter"), "Fields mismatch")

    def test_noncanonical_sequence(self):
        self.rejects(lambda value: value.update(checkpoint_sequence="00"), "canonical")

    def test_boolean_format(self):
        self.rejects(lambda value: value.update(format_version=True), "format")

    def test_bad_uuid(self):
        self.rejects(lambda value: value.update(instance_id="bad"), "UUID")

    def test_replay_boundary(self):
        self.rejects(lambda value: value.update(wal_replay_after="1"), "WAL replay")

    def test_abbreviated_commit(self):
        self.rejects(lambda value: value["adapter"].update(commit="4c31367"), "exact commit")

    def test_unbounded_parameters(self):
        self.rejects(lambda value: value["adapter"].update(parameters="x" * 513), "byte limit")

    def test_duplicate_segments(self):
        self.rejects(lambda value: value["segments"].append(copy.deepcopy(value["segments"][0])),
                     "Duplicate segment")

    def test_segment_capacity(self):
        self.rejects(lambda value: value.update(segments=value["segments"] * 17), "Segment count")

    def test_future_segment(self):
        self.rejects(lambda value: value["segments"][0].update(covered_sequence="1"),
                     "above checkpoint")

    def test_unsafe_metadata_path(self):
        self.rejects(lambda value: value["segments"][0]["metadata"].update(path="../outside"),
                     "Path must stay")

    def test_wrong_metadata_uuid(self):
        self.rejects(lambda value: value["segments"][0]["metadata"].update(
            path="segments/another/segment.json"), "must match")

    def test_missing_digest(self):
        self.rejects(lambda value: value["segments"][0]["metadata"].update(sha256=None),
                     "checksum")

    def test_metadata_lengths(self):
        self.rejects(lambda value: value["segments"][0]["metadata"].update(physical_bytes="1"),
                     "Metadata lengths")

    def test_layout_overlap(self):
        self.rejects(lambda value: value["layout"].update(neighbor_offset=0), "overlap")

    def test_short_row(self):
        self.rejects(lambda value: value["layout"].update(metadata_offset=4090), "exceeds row")

    def test_short_segment(self):
        self.rejects(lambda value: value["segments"][0].update(file_bytes="4096"),
                     "Segment length")

    def test_live_count(self):
        self.rejects(lambda value: value.update(live_count="25"), "Live count")


if __name__ == "__main__":
    unittest.main(verbosity=2)
