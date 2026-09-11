#!/usr/bin/env python3
# Copyright (c) 2026 Mohit Talwar
# SPDX-License-Identifier: Apache-2.0

"""Turn the checked-in MIPC golden vectors into a C header of exact bytes.

The JSON file is the single cross-language source of truth for the version 1
wire format. This script computes each frame's CRC32C with an implementation
independent of Zephyr's C one and emits explicit byte arrays, so the C tests
and any later Python peer consume identical bytes and disagree loudly if either
side grows a host endianness or structure padding assumption.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

# CRC32C (Castagnoli), reflected form of polynomial 0x1edc6f41.
_CRC32C_POLY_REFLECTED = 0x82F63B78
_CRC32C_INIT = 0xFFFFFFFF
_CRC32C_XOR_OUT = 0xFFFFFFFF

#: Fixed v1 header length in bytes.
HEADER_LENGTH = 40
#: Offset of the CRC field within the header.
CRC_OFFSET = 36

#: Header fields in wire order, with their width in bytes.
_HEADER_FIELDS: tuple[tuple[str, int], ...] = (
    ("major", 1),
    ("minor", 1),
    ("type", 1),
    ("header_length", 1),
    ("flags", 4),
    ("session_id", 4),
    ("stream_id", 4),
    ("sequence", 4),
    ("payload_length", 4),
    ("timestamp_us", 4),
    ("format_generation", 4),
)


class VectorError(ValueError):
    """The vector document does not satisfy the golden-vector contract."""


def crc32c(data: bytes) -> int:
    """Return the CRC32C of @p data using the spec's exact parameters."""

    crc = _CRC32C_INIT
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ _CRC32C_POLY_REFLECTED
            else:
                crc >>= 1
    return crc ^ _CRC32C_XOR_OUT


@dataclass(frozen=True)
class Frame:
    """One golden frame: its decoded fields and its exact wire bytes."""

    name: str
    header: dict[str, int]
    payload: bytes
    frame: bytes

    @property
    def crc32c(self) -> int:
        return int.from_bytes(
            self.frame[CRC_OFFSET : CRC_OFFSET + 4], "little", signed=False
        )


def _require(mapping: dict[str, Any], key: str, where: str) -> Any:
    if key not in mapping:
        raise VectorError(f"{where} is missing required field {key!r}")
    return mapping[key]


def _parse_hex(value: str, where: str) -> bytes:
    if not isinstance(value, str):
        raise VectorError(f"{where} must be a hex string")
    if len(value) % 2 != 0:
        raise VectorError(f"{where} must have an even number of hex digits")
    try:
        return bytes.fromhex(value)
    except ValueError as exc:
        raise VectorError(f"{where} is not valid hex: {exc}") from exc


def _encode_header(header: dict[str, int], name: str) -> bytearray:
    buf = bytearray(HEADER_LENGTH)
    buf[0:4] = b"MIPC"
    offset = 4
    for field, width in _HEADER_FIELDS:
        value = _require(header, field, f"frame {name!r} header")
        if not isinstance(value, int) or isinstance(value, bool):
            raise VectorError(f"frame {name!r} field {field!r} must be an integer")
        if value < 0 or value >= (1 << (8 * width)):
            raise VectorError(
                f"frame {name!r} field {field!r} does not fit in {width} bytes"
            )
        buf[offset : offset + width] = value.to_bytes(width, "little")
        offset += width
    if offset != CRC_OFFSET:
        raise VectorError("internal header layout error")
    return buf


def build_frame(entry: dict[str, Any]) -> Frame:
    """Validate one vector entry and compute its complete wire bytes."""

    name = _require(entry, "name", "frame")
    if not isinstance(name, str) or not name:
        raise VectorError("frame name must be a non-empty string")

    header = dict(_require(entry, "header", f"frame {name!r}"))
    payload = _parse_hex(entry.get("payload_hex", ""), f"frame {name!r} payload_hex")

    declared = _require(header, "payload_length", f"frame {name!r} header")
    if declared != len(payload):
        raise VectorError(
            f"frame {name!r} declares payload_length {declared} "
            f"but payload_hex holds {len(payload)} bytes"
        )
    if header.get("header_length") != HEADER_LENGTH:
        raise VectorError(f"frame {name!r} header_length must be {HEADER_LENGTH}")

    buf = _encode_header(header, name)
    # The CRC covers the header with its own field zeroed, then the payload.
    frame = bytes(buf) + payload
    value = crc32c(frame)
    buf[CRC_OFFSET : CRC_OFFSET + 4] = value.to_bytes(4, "little")

    return Frame(name=name, header=header, payload=payload, frame=bytes(buf) + payload)


def build_frames(document: dict[str, Any]) -> list[Frame]:
    """Validate the document and return its frames in declaration order."""

    entries = _require(document, "frames", "document")
    if not isinstance(entries, list) or not entries:
        raise VectorError("document frames must be a non-empty list")

    frames = [build_frame(entry) for entry in entries]

    seen: set[str] = set()
    for frame in frames:
        if frame.name in seen:
            raise VectorError(f"duplicate frame name {frame.name!r}")
        seen.add(frame.name)

    return frames


def _c_array(data: bytes) -> str:
    lines = []
    for start in range(0, len(data), 12):
        chunk = data[start : start + 12]
        lines.append("\t" + " ".join(f"0x{byte:02x}," for byte in chunk))
    return "\n".join(lines)


def render_header(document: dict[str, Any]) -> str:
    """Render the generated C header for @p document."""

    frames = build_frames(document)
    check = _require(document, "crc32c_check", "document")
    check_input = _require(check, "input_ascii", "crc32c_check").encode("ascii")
    check_expected = int(_require(check, "expected", "crc32c_check"), 16)

    computed = crc32c(check_input)
    if computed != check_expected:
        raise VectorError(
            f"crc32c_check mismatch: computed 0x{computed:08x}, "
            f"document declares 0x{check_expected:08x}"
        )

    out: list[str] = [
        "/*",
        " * Generated by tests/subsys/mpipe/ipc_protocol/scripts/vector_to_header.py",
        " * from vectors/mpipe-ipc-v1.json. Do not edit.",
        " *",
        " * SPDX-License-Identifier: Apache-2.0",
        " */",
        "",
        "#ifndef MPIPE_IPC_VECTORS_H_",
        "#define MPIPE_IPC_VECTORS_H_",
        "",
        "#include <stdint.h>",
        "#include <stddef.h>",
        "",
        f"#define MPIPE_IPC_VECTOR_CRC_CHECK_INPUT \"{check_input.decode('ascii')}\"",
        f"#define MPIPE_IPC_VECTOR_CRC_CHECK_EXPECTED 0x{check_expected:08x}U",
        "",
        "struct mpipe_ipc_vector {",
        "\tconst char *name;",
        "\tconst uint8_t *frame;",
        "\tsize_t frame_length;",
        "\tsize_t payload_length;",
        "\tuint8_t major;",
        "\tuint8_t minor;",
        "\tuint8_t type;",
        "\tuint8_t header_length;",
        "\tuint32_t flags;",
        "\tuint32_t session_id;",
        "\tuint32_t stream_id;",
        "\tuint32_t sequence;",
        "\tuint32_t timestamp_us;",
        "\tuint32_t format_generation;",
        "\tuint32_t crc32c;",
        "};",
        "",
    ]

    for index, frame in enumerate(frames):
        out.append(f"static const uint8_t mpipe_ipc_vector_bytes_{index}[] = {{")
        out.append(_c_array(frame.frame))
        out.append("};")
        out.append("")

    out.append("static const struct mpipe_ipc_vector mpipe_ipc_vectors[] = {")
    for index, frame in enumerate(frames):
        header = frame.header
        out.append("\t{")
        out.append(f"\t\t.name = \"{frame.name}\",")
        out.append(f"\t\t.frame = mpipe_ipc_vector_bytes_{index},")
        out.append(f"\t\t.frame_length = {len(frame.frame)}U,")
        out.append(f"\t\t.payload_length = {len(frame.payload)}U,")
        for field, _width in _HEADER_FIELDS:
            if field == "payload_length":
                continue
            out.append(f"\t\t.{field} = {header[field]}U,")
        out.append(f"\t\t.crc32c = 0x{frame.crc32c:08x}U,")
        out.append("\t},")
    out.append("};")
    out.append("")
    out.append(
        "#define MPIPE_IPC_VECTOR_COUNT "
        "(sizeof(mpipe_ipc_vectors) / sizeof(mpipe_ipc_vectors[0]))"
    )
    out.append("")
    out.append("#endif /* MPIPE_IPC_VECTORS_H_ */")
    out.append("")

    return "\n".join(out)


def _source_root() -> Path:
    """Directory tree that generated output must never be written into."""

    return Path(__file__).resolve().parent.parent


def check_output_path(output: Path) -> Path:
    """Return @p output, or raise if it lies inside the checked-in test tree."""

    resolved = output.resolve()
    root = _source_root()
    if resolved == root or root in resolved.parents:
        raise VectorError(
            f"refusing to write generated output into the source tree: {resolved}"
        )
    return resolved


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate the MIPC golden-vector C header"
    )
    parser.add_argument("--input", type=Path, required=True, help="vector JSON file")
    parser.add_argument("--output", type=Path, required=True, help="header to write")
    args = parser.parse_args(argv)

    try:
        destination = check_output_path(args.output)
        document = json.loads(args.input.read_text(encoding="utf-8"))
        rendered = render_header(document)
    except (OSError, json.JSONDecodeError, VectorError) as exc:
        print(f"vector_to_header: {exc}", file=sys.stderr)
        return 1

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(rendered, encoding="utf-8")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
