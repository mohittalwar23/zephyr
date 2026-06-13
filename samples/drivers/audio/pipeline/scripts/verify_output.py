#!/usr/bin/env python3
# Copyright 2025-2026 NXP
# SPDX-License-Identifier: Apache-2.0
"""Verify the native_sim audio pipeline output (i2s_out.pcm) against the input.

Replicates the zaud gain element's Q16.16 fixed-point math exactly and checks:
  * output frame/byte count (when --frames given)
  * per-sample gain correctness (out == clamp((in * gain_fixed) >> 16))
  * mute (gain 0) -> all zeros
  * unity (gain 100) -> byte-exact pass-through
  * silence input -> silence output

Exit status is non-zero on any check failure.

Example:
    verify_output.py --in dmic_in.pcm --out i2s_out.pcm --gain 90 --frames 100
"""

import argparse
import struct
import sys

INT16_MIN, INT16_MAX = -32768, 32767


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def expected_sample(s, gain_pct):
    """Mathematically-correct zaud gain (matches the fixed int64 plugin)."""
    gain_pct = clamp(gain_pct, 0, 1000)
    if gain_pct == 0:
        return 0
    if gain_pct == 100:
        return s
    gain_fixed = (gain_pct * (1 << 16)) // 100
    # Arithmetic right shift; Python >> floors negatives, matching C int64 >>.
    return clamp((s * gain_fixed) >> 16, INT16_MIN, INT16_MAX)


def read_s16(path):
    with open(path, "rb") as f:
        data = f.read()
    n = len(data) // 2
    return list(struct.unpack(f"<{n}h", data[: n * 2])), len(data)


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        allow_abbrev=False,
    )
    p.add_argument("--in", dest="inp", required=True)
    p.add_argument("--out", dest="out", required=True)
    p.add_argument("--gain", type=int, default=90, help="gain percent applied (default 90)")
    p.add_argument("--frames", type=int, default=None, help="expected output frame count")
    p.add_argument("--frame-bytes", type=int, default=640, help="bytes per frame (default 640)")
    p.add_argument("--tol", type=int, default=0, help="abs sample tolerance (default 0 = exact)")
    p.add_argument(
        "--expect-silence",
        action="store_true",
        help="assert output is all zeros (e.g. missing input file)",
    )
    args = p.parse_args()

    failures = []

    inp, _ = read_s16(args.inp) if not args.expect_silence else ([], 0)
    out, out_bytes = read_s16(args.out)

    # 1. Output size / frame count
    if args.frames is not None:
        want = args.frames * args.frame_bytes
        if out_bytes != want:
            failures.append(
                f"output bytes {out_bytes} != expected {want} ({args.frames} x {args.frame_bytes})"
            )
        else:
            print(f"[ok] output size {out_bytes} B = {args.frames} frames")
    else:
        print(f"[info] output size {out_bytes} B ({out_bytes // args.frame_bytes} frames)")

    # 2. Content
    if args.expect_silence:
        nz = sum(1 for s in out if s != 0)
        if nz:
            failures.append(f"expected silence but {nz}/{len(out)} samples are non-zero")
        else:
            print(f"[ok] output is silence ({len(out)} samples all zero)")
    else:
        n = min(len(inp), len(out))
        if n == 0:
            failures.append("no overlapping samples to compare")
        else:
            max_err = 0
            bad = 0
            first_bad = None
            for i in range(n):
                exp = expected_sample(inp[i], args.gain)
                err = abs(out[i] - exp)
                if err > max_err:
                    max_err = err
                if err > args.tol:
                    bad += 1
                    if first_bad is None:
                        first_bad = (i, inp[i], exp, out[i])
            if bad:
                failures.append(
                    f"gain mismatch: {bad}/{n} samples off (max_err={max_err}); "
                    f"first bad idx {first_bad[0]}: in={first_bad[1]} "
                    f"exp={first_bad[2]} got={first_bad[3]}"
                )
            else:
                label = (
                    "byte-exact pass-through"
                    if args.gain == 100
                    else "mute (all zero)"
                    if args.gain == 0
                    else f"gain {args.gain}%"
                )
                print(f"[ok] {label}: {n} samples match (max_err={max_err}, tol={args.tol})")
            # clipping report
            clipped = sum(1 for s in out if s in (INT16_MIN, INT16_MAX))
            if clipped:
                print(f"[info] {clipped} output samples at full-scale (clipping)")

    if failures:
        print("\nFAIL:")
        for f in failures:
            print("  - " + f)
        sys.exit(1)
    print("\nPASS")


if __name__ == "__main__":
    main()
