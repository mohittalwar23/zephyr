#!/usr/bin/env python3

# Copyright 2026 NXP
# SPDX-License-Identifier: Apache-2.0

"""Verify i.MX8M M7 audio clock ownership in the compiled SoC object.

The AUDIOMIX attachment identities are encoded constants:

    id = offset | (mask << 16) | (value << 24)

Presence of the AUDIOMIX_AttachClk symbol says nothing about which clock was
attached, so this script decodes the argument of every call site and compares
the decoded (offset, mask, value) triples against the identities the active
devicetree configuration is expected to write. It also fails when two calls
program the same register field, because the second write silently replaces
the first.
"""

import argparse
import re
import subprocess
import sys

ATTACH_CALLEE = "AUDIOMIX_AttachClk"

# Identity name -> (offset, mask, value), matching fsl_audiomix.h.
IDENTITIES = {
    "sai3_mclk1_to_sai3_root": (0x308, 0x1, 0x0),
    "sai3_mclk1_to_sai3_mclk": (0x308, 0x1, 0x1),
    "pdm_root_to_ccm_pdm": (0x318, 0x3, 0x0),
    "pdm_root_to_sai_pll_div2": (0x318, 0x3, 0x1),
    "pdm_root_to_sai1_mclk": (0x318, 0x3, 0x2),
}

# Peripheral -> the single identity it is allowed to program.
PERIPHERAL_IDENTITY = {
    "sai3": "sai3_mclk1_to_sai3_root",
    "micfil": "pdm_root_to_ccm_pdm",
}

PLL_SYMBOLS = ("CLOCK_InitAudioPll1", "g_audioPll1Config")

INSN_RE = re.compile(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2,8} )+\s*(\S+)\s*(.*)$")
WORD_RE = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+\.word\s+0x([0-9a-f]+)")
RELOC_RE = re.compile(r"^\s+[0-9a-f]+:\s+R_ARM_\S+\s+(\S+)")
MOVW_RE = re.compile(r"^r1,\s*#(\d+)")
MOVT_RE = re.compile(r"^r1,\s*#(\d+)")
MOV_RE = re.compile(r"^r1,\s*#(\d+)")
R1_DEST_RE = re.compile(r"^r1\s*,")
LDR_LIT_RE = re.compile(r"^r1,\s*\[pc,\s*#\d+\]\s*[;@]\s*\(([0-9a-f]+)\s")


def decode(identity):
    return identity & 0xFFFF, (identity >> 16) & 0xFF, (identity >> 24) & 0xFF


def disassemble(objdump, obj):
    return subprocess.check_output([objdump, "-d", "-r", obj], text=True).splitlines()


def literal_words(lines):
    words = {}
    for line in lines:
        match = WORD_RE.match(line)
        if match:
            words[int(match.group(1), 16)] = int(match.group(3), 16)
    return words


def call_arguments(lines, errors):
    """Decode the r1 argument of every AUDIOMIX_AttachClk call site."""
    words = literal_words(lines)
    arguments = []
    r1 = None
    pending = None

    for line in lines:
        reloc = RELOC_RE.match(line)
        if reloc:
            if pending is not None and reloc.group(1) == ATTACH_CALLEE:
                if r1 is None:
                    errors.append(f"could not decode the argument of the call at 0x{pending:x}")
                else:
                    arguments.append((pending, r1))
            pending = None
            continue

        match = INSN_RE.match(line)
        if not match:
            continue

        address, mnemonic, operands = int(match.group(1), 16), match.group(2), match.group(3)
        pending = None

        if mnemonic == "bl":
            pending = address
            continue

        if not R1_DEST_RE.match(operands):
            # The instruction leaves r1 alone.
            continue

        if mnemonic == "movw":
            found = MOVW_RE.match(operands)
            r1 = ((r1 or 0) & ~0xFFFF) | int(found.group(1)) if found else None
        elif mnemonic == "movt":
            found = MOVT_RE.match(operands)
            r1 = ((r1 or 0) & 0xFFFF) | (int(found.group(1)) << 16) if found else None
        elif mnemonic in ("mov", "mov.w", "movs"):
            found = MOV_RE.match(operands)
            r1 = int(found.group(1)) if found else None
        elif mnemonic in ("ldr", "ldr.w"):
            found = LDR_LIT_RE.match(operands)
            r1 = words.get(int(found.group(1), 16)) if found else None
        else:
            # Any other instruction that writes r1 invalidates the tracked value.
            r1 = None

    return arguments


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nm", required=True)
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--obj", required=True)
    parser.add_argument("--owner", required=True, choices=("owned", "external"))
    parser.add_argument("--attach", default="", help="comma separated active peripherals")
    args = parser.parse_args()

    errors = []
    symbols = subprocess.check_output([args.nm, "-C", args.obj], text=True)
    expected = [name for name in args.attach.split(",") if name]

    unknown = [name for name in expected if name not in PERIPHERAL_IDENTITY]
    if unknown:
        errors.append(f"unknown peripheral(s) requested: {', '.join(unknown)}")

    if args.owner == "owned":
        for symbol in PLL_SYMBOLS:
            if symbol not in symbols:
                errors.append(f"the owned configuration is missing {symbol}")
    else:
        for symbol in PLL_SYMBOLS:
            if symbol in symbols:
                errors.append(f"the external configuration still owns {symbol}")

    if expected and ATTACH_CALLEE not in symbols:
        errors.append(f"{ATTACH_CALLEE} is not referenced at all")

    decoded = []
    if ATTACH_CALLEE in symbols:
        for address, identity in call_arguments(disassemble(args.objdump, args.obj), errors):
            decoded.append((address, decode(identity)))

    if not expected and decoded:
        errors.append(f"no peripheral is active but {len(decoded)} attachment(s) are programmed")

    wanted = {
        IDENTITIES[PERIPHERAL_IDENTITY[name]]: name
        for name in expected
        if name in PERIPHERAL_IDENTITY
    }

    seen_fields = {}
    for address, triple in decoded:
        offset, mask, value = triple
        if triple not in wanted:
            errors.append(
                f"call at 0x{address:x} programs register 0x{offset:x} mask 0x{mask:x} "
                f"value 0x{value:x}, which is not an expected identity"
            )
        field = (offset, mask)
        if field in seen_fields:
            errors.append(
                f"register 0x{offset:x} mask 0x{mask:x} is written twice "
                f"(0x{seen_fields[field]:x} and 0x{address:x}); the later write wins"
            )
        seen_fields[field] = address

    for triple, name in wanted.items():
        if triple not in [entry[1] for entry in decoded]:
            offset, mask, value = triple
            errors.append(
                f"{name} is active but register 0x{offset:x} mask 0x{mask:x} "
                f"value 0x{value:x} is never programmed"
            )

    for error in errors:
        print(f"verify_soc_symbols: {error}", file=sys.stderr)

    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
