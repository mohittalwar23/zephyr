#!/usr/bin/env python3

# Copyright 2026 NXP
# SPDX-License-Identifier: Apache-2.0

"""Verify the SAI clock initializer and the driver's clock seam.

Build-time assertions can only check devicetree macros. This script checks the
object the compiler actually emitted: the provider pointers inside the
`sai_clock_data` initializer must be the same devicetree devices the node names,
in the same order, and the SAI driver must reach the clocks through the atomic
helpers rather than calling the clock-control wrappers itself.
"""

import argparse
import re
import subprocess
import sys

# Helpers the driver must go through, and wrappers it must no longer call.
REQUIRED_SEAMS = ("dai_nxp_sai_clocks_enable", "dai_nxp_sai_clocks_release")
FORBIDDEN_DIRECT = ("nxp_clock_control_on_dt", "nxp_clock_control_off_dt")


def relocation_symbols(readelf: str, obj: str, section: str) -> list[str]:
    output = subprocess.check_output([readelf, "-rW", obj], text=True)
    match = re.search(
        rf"Relocation section '{re.escape(section)}'.*?(?=\nRelocation section|\Z)",
        output,
        re.DOTALL,
    )
    if match is None:
        raise RuntimeError(f"missing relocation section {section}")

    return re.findall(r"\s(__device_dts_ord_\d+)\s*$", match.group(0), re.MULTILINE)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--readelf", required=True)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--app-obj", required=True)
    parser.add_argument("--sai-obj", required=True)
    args = parser.parse_args()

    errors = []

    expected = [
        relocation_symbols(args.readelf, args.app_obj, ".rel.rodata.expected_sai_provider_0")[0],
        relocation_symbols(args.readelf, args.app_obj, ".rel.rodata.expected_sai_provider_1")[0],
    ]
    actual = relocation_symbols(args.readelf, args.app_obj, ".rel.data.__compound_literal.0")

    if actual != expected:
        errors.append(
            f"the emitted SAI initializer points at {actual} but the devicetree names {expected}"
        )

    symbols = subprocess.check_output([args.nm, "-C", args.sai_obj], text=True)
    undefined = set(re.findall(r"^\s+U (\S+)$", symbols, re.MULTILINE))

    for seam in REQUIRED_SEAMS:
        if seam not in undefined:
            errors.append(f"the SAI driver does not use {seam}()")

    for wrapper in FORBIDDEN_DIRECT:
        if wrapper in undefined:
            errors.append(f"the SAI driver calls {wrapper}() instead of the atomic helpers")

    for error in errors:
        print(f"verify_providers: {error}", file=sys.stderr)

    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
