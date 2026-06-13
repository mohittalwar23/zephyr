#!/usr/bin/env python3
# Copyright 2025-2026 NXP
# SPDX-License-Identifier: Apache-2.0
"""Generate a raw PCM test tone for the libMP audio pipeline native_sim sample.

The native_sim DMIC reads raw little-endian PCM from a host file. This helper
produces a stereo (interleaved L/R), 16-bit, 16 kHz sine tone suitable as the
``dmic_in.pcm`` capture source.

Example:
    python3 gen_input.py --frames 150 --out dmic_in.pcm
"""

import argparse
import math
import struct

SAMPLE_RATE = 16000
CHANNELS = 2
SAMPLES_PER_FRAME = 160  # 10 ms at 16 kHz


def main():
    p = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    p.add_argument("--out", default="dmic_in.pcm", help="output file path")
    p.add_argument("--frames", type=int, default=150, help="number of 10 ms frames")
    p.add_argument("--freq", type=float, default=440.0, help="tone frequency (Hz)")
    p.add_argument("--amp", type=int, default=10000, help="amplitude (max 32767)")
    args = p.parse_args()

    n = 0
    with open(args.out, "wb") as f:
        for _ in range(args.frames):
            for _ in range(SAMPLES_PER_FRAME):
                t = n / SAMPLE_RATE
                left = int(args.amp * math.sin(2 * math.pi * args.freq * t))
                # Distinct right-channel tone (a higher harmonic) so a channel
                # swap or loss of per-channel independence is detectable; both
                # channels keep the same amplitude so clipping behaviour is
                # unchanged for the gain tests.
                right = int(args.amp * math.sin(2 * math.pi * args.freq * 1.5 * t))
                n += 1
                f.write(struct.pack("<hh", left, right))

    print(
        f"wrote {args.out}: {args.frames} frames "
        f"({args.frames * SAMPLES_PER_FRAME * CHANNELS * 2} bytes)"
    )


if __name__ == "__main__":
    main()
