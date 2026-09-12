# Copyright (c) 2026 Mohit Talwar
# SPDX-License-Identifier: Apache-2.0

"""Tests for the golden-vector header generator."""

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
    main,
    render_header,
)

GENERATION = 0x0000A11C


def header(**overrides: int) -> dict[str, int]:
    base = {"cmd": (0x0B << 16) | 1, "generation": GENERATION}
    base.update(overrides)
    return base


def document(**overrides: object) -> dict[str, object]:
    base: dict[str, object] = {
        "frames": [{"name": "heartbeat", "header": header(), "payload_hex": ""}],
    }
    base.update(overrides)
    return base


class FrameTest(unittest.TestCase):
    def test_header_only_frame_is_exactly_twelve_bytes(self):
        frame = build_frame({"name": "bare", "header": header(), "payload_hex": ""})

        self.assertEqual(len(frame.frame), HEADER_LENGTH)
        self.assertEqual(frame.frame[:4], (12).to_bytes(4, "little"))

    def test_size_field_counts_header_plus_payload(self):
        payload = bytes(range(12))
        frame = build_frame(
            {"name": "config", "header": header(), "payload_hex": payload.hex()}
        )

        self.assertEqual(len(frame.frame), HEADER_LENGTH + len(payload))
        self.assertEqual(
            int.from_bytes(frame.frame[0:4], "little"), HEADER_LENGTH + len(payload)
        )
        self.assertEqual(frame.payload, payload)

    def test_fields_are_serialized_little_endian(self):
        frame = build_frame(
            {"name": "hb", "header": header(generation=0xAABBCCDD), "payload_hex": ""}
        )

        self.assertEqual(frame.frame[8:12], bytes([0xDD, 0xCC, 0xBB, 0xAA]))

    def test_rejects_zero_generation(self):
        """A generation of zero means "not yet published" and is never on the wire."""

        with self.assertRaisesRegex(VectorError, "generation must be nonzero"):
            build_frame(
                {"name": "bad", "header": header(generation=0), "payload_hex": ""}
            )

    def test_rejects_odd_length_hex(self):
        with self.assertRaisesRegex(VectorError, "even number of hex digits"):
            build_frame({"name": "bad", "header": header(), "payload_hex": "0"})

    def test_rejects_missing_required_field(self):
        fields = header()
        del fields["cmd"]
        with self.assertRaisesRegex(VectorError, "cmd"):
            build_frame({"name": "bad", "header": fields, "payload_hex": ""})

    def test_rejects_value_too_wide_for_its_field(self):
        with self.assertRaisesRegex(VectorError, "does not fit"):
            build_frame(
                {"name": "bad", "header": header(cmd=1 << 32), "payload_hex": ""}
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
                {"name": "alpha", "header": header(cmd=2), "payload_hex": ""},
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

    def test_rendered_header_carries_no_crc(self):
        """The v1 control format has no checksum; the generator must not invent one."""

        rendered = render_header(document())

        self.assertNotIn("crc", rendered.lower())

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

            self.assertEqual(main(["--input", str(source), "--output", str(target)]), 0)

            text = target.read_text(encoding="utf-8")
            self.assertIn("MPIPE_IPC_VECTOR_COUNT", text)
            self.assertIn("0x0c, 0x00, 0x00, 0x00,", text, "size must lead the header")

    def test_reports_failure_for_a_bad_document(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "vectors.json"
            source.write_text("{not json", encoding="utf-8")
            target = Path(tmp) / "out.h"

            self.assertEqual(main(["--input", str(source), "--output", str(target)]), 1)
            self.assertFalse(target.exists())


class RealVectorsTest(unittest.TestCase):
    """The checked-in vectors must satisfy their own contract."""

    def test_shipped_vectors_build(self):
        source = Path(__file__).resolve().parent.parent / "vectors" / "mpipe-ipc-v1.json"
        doc = json.loads(source.read_text(encoding="utf-8"))

        frames = build_frames(doc)

        self.assertGreaterEqual(len(frames), 12)
        for frame in frames:
            self.assertEqual(
                int.from_bytes(frame.frame[0:4], "little"),
                len(frame.frame),
                f"{frame.name}: size field must equal the real length",
            )
            self.assertLessEqual(
                len(frame.frame), 256, f"{frame.name}: exceeds the control ceiling"
            )


if __name__ == "__main__":
    unittest.main()
