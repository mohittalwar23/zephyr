# Copyright (c) 2026 Mohit Talwar
# SPDX-License-Identifier: Apache-2.0

"""Tests for the MIPC golden-vector header generator."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from vector_to_header import (
    HEADER_LENGTH,
    VectorError,
    build_frame,
    build_frames,
    check_output_path,
    crc32c,
    main,
    render_header,
)


def header(**overrides: int) -> dict[str, int]:
    base = {
        "major": 1,
        "minor": 0,
        "type": 0x0C,
        "header_length": HEADER_LENGTH,
        "flags": 1,
        "session_id": 0x11223344,
        "stream_id": 0,
        "sequence": 0,
        "payload_length": 0,
        "timestamp_us": 0,
        "format_generation": 0,
    }
    base.update(overrides)
    return base


def document(**overrides: object) -> dict[str, object]:
    base: dict[str, object] = {
        "crc32c_check": {"input_ascii": "123456789", "expected": "e3069283"},
        "frames": [{"name": "heartbeat", "header": header(), "payload_hex": ""}],
    }
    base.update(overrides)
    return base


class Crc32cTest(unittest.TestCase):
    def test_matches_the_standard_check_value(self):
        self.assertEqual(crc32c(b"123456789"), 0xE3069283)

    def test_empty_input_is_zero(self):
        self.assertEqual(crc32c(b""), 0x00000000)


class FrameTest(unittest.TestCase):
    def test_frame_is_header_plus_payload_with_crc_filled_in(self):
        payload = bytes(range(16))
        frame = build_frame(
            {
                "name": "hello",
                "header": header(type=0x01, payload_length=len(payload)),
                "payload_hex": payload.hex(),
            }
        )

        self.assertEqual(len(frame.frame), HEADER_LENGTH + len(payload))
        self.assertEqual(frame.frame[:4], b"MIPC")
        self.assertEqual(frame.payload, payload)

    def test_crc_is_computed_over_the_header_with_its_own_field_zeroed(self):
        frame = build_frame(
            {"name": "heartbeat", "header": header(), "payload_hex": ""}
        )

        zeroed = bytearray(frame.frame)
        zeroed[36:40] = b"\x00\x00\x00\x00"

        self.assertEqual(frame.crc32c, crc32c(bytes(zeroed)))

    def test_fields_are_serialized_little_endian(self):
        frame = build_frame(
            {
                "name": "heartbeat",
                "header": header(session_id=0xAABBCCDD),
                "payload_hex": "",
            }
        )

        self.assertEqual(frame.frame[12:16], bytes([0xDD, 0xCC, 0xBB, 0xAA]))

    def test_rejects_declared_length_that_disagrees_with_payload(self):
        with self.assertRaisesRegex(VectorError, "payload_length"):
            build_frame(
                {
                    "name": "bad",
                    "header": header(payload_length=8),
                    "payload_hex": "0011",
                }
            )

    def test_rejects_odd_length_hex(self):
        with self.assertRaisesRegex(VectorError, "even number of hex digits"):
            build_frame(
                {"name": "bad", "header": header(payload_length=1), "payload_hex": "0"}
            )

    def test_rejects_missing_required_field(self):
        fields = header()
        del fields["sequence"]
        with self.assertRaisesRegex(VectorError, "sequence"):
            build_frame({"name": "bad", "header": fields, "payload_hex": ""})

    def test_rejects_value_too_wide_for_its_field(self):
        with self.assertRaisesRegex(VectorError, "does not fit"):
            build_frame({"name": "bad", "header": header(major=256), "payload_hex": ""})

    def test_rejects_wrong_header_length(self):
        with self.assertRaisesRegex(VectorError, "header_length"):
            build_frame(
                {"name": "bad", "header": header(header_length=44), "payload_hex": ""}
            )

    def test_rejects_duplicate_frame_names(self):
        entry = {"name": "same", "header": header(), "payload_hex": ""}
        with self.assertRaisesRegex(VectorError, "duplicate frame name"):
            build_frames({"frames": [entry, dict(entry)]})


class RenderTest(unittest.TestCase):
    def test_output_is_deterministic_and_ordered(self):
        doc = document(
            frames=[
                {"name": "zulu", "header": header(), "payload_hex": ""},
                {"name": "alpha", "header": header(sequence=1), "payload_hex": ""},
            ]
        )

        first = render_header(doc)
        second = render_header(doc)

        self.assertEqual(first, second, "identical input must render identically")
        self.assertLess(
            first.index('"zulu"'),
            first.index('"alpha"'),
            "frames must keep declaration order, not be sorted",
        )

    def test_rejects_a_document_whose_declared_check_value_is_wrong(self):
        doc = document(crc32c_check={"input_ascii": "123456789", "expected": "deadbeef"})

        with self.assertRaisesRegex(VectorError, "crc32c_check mismatch"):
            render_header(doc)

    def test_rejects_empty_frame_list(self):
        with self.assertRaisesRegex(VectorError, "non-empty list"):
            render_header(document(frames=[]))


class OutputPathTest(unittest.TestCase):
    def test_refuses_to_write_into_the_checked_in_test_tree(self):
        inside = Path(__file__).resolve().parent / "generated.h"

        with self.assertRaisesRegex(VectorError, "source tree"):
            check_output_path(inside)

    def test_allows_a_build_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "include" / "mpipe_ipc_vectors.h"
            self.assertEqual(check_output_path(target), target.resolve())


class MainTest(unittest.TestCase):
    def test_writes_a_header_and_creates_missing_directories(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "vectors.json"
            source.write_text(json.dumps(document()), encoding="utf-8")
            target = Path(tmp) / "build" / "include" / "mpipe_ipc_vectors.h"

            self.assertEqual(
                main(["--input", str(source), "--output", str(target)]), 0
            )

            text = target.read_text(encoding="utf-8")
            self.assertIn("MPIPE_IPC_VECTOR_COUNT", text)
            self.assertIn("0x4d, 0x49, 0x50, 0x43,", text, "magic must appear as bytes")

    def test_reports_failure_for_a_bad_document(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "vectors.json"
            source.write_text("{not json", encoding="utf-8")
            target = Path(tmp) / "out.h"

            self.assertEqual(
                main(["--input", str(source), "--output", str(target)]), 1
            )
            self.assertFalse(target.exists())


if __name__ == "__main__":
    unittest.main()
