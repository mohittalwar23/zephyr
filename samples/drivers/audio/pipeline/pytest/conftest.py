# Copyright 2025-2026 NXP
# SPDX-License-Identifier: Apache-2.0

"""Pytest options for the libMP audio pipeline Twister harness."""


def pytest_addoption(parser):
    parser.addoption(
        "--audio-mode",
        action="store",
        default="gain",
        choices=("gain", "silence"),
        help="Verification mode: 'gain' generates a tone and checks the Q16.16 "
        "gain math; 'silence' withholds the input file and asserts the output "
        "is all-zero (missing-input behaviour).",
    )
