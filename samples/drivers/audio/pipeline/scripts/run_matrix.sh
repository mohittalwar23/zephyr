#!/usr/bin/env bash
# Copyright 2025-2026 NXP
# SPDX-License-Identifier: Apache-2.0
#
# Long-running soak / stress validation for the libMP audio pipeline.
#
# The Twister matrix (sample.yaml) already covers the fast functional checks
# (gain sweep, missing-input silence, short-stream flush, sanitizers) in CI.
# This script covers the rows that are too slow for CI: a many-frame soak that
# asserts the output stays byte-exact to the Q16.16 reference, the frame count
# is exact, and the simulated clock shows zero drift at end-of-stream.
#
# Usage:
#   scripts/run_matrix.sh [--frames N] [--gain PCT] [--rt] [--build-dir DIR]
#
#   --frames N        frames to soak (default 100000 = 1000 s of audio)
#   --gain PCT        gain percent (default 90)
#   --rt              run in host real time (default: fast-forward simulated
#                     time). Real time makes a 100k soak take ~16m41s.
#   --build-dir DIR   build directory (default build-soak)
#
# Exit status is non-zero if any soak assertion fails.

set -euo pipefail

FRAMES=100000
GAIN=90
RT=0
BUILD_DIR=build-soak

while [ $# -gt 0 ]; do
	case "$1" in
	--frames) FRAMES=$2; shift 2 ;;
	--gain) GAIN=$2; shift 2 ;;
	--rt) RT=1; shift ;;
	--build-dir) BUILD_DIR=$2; shift 2 ;;
	-h | --help) sed -n '6,24p' "$0"; exit 0 ;;
	*) echo "unknown arg: $1" >&2; exit 2 ;;
	esac
done

SAMPLE_DIR=$(cd "$(dirname "$0")/.." && pwd)
SCRIPTS=$SAMPLE_DIR/scripts

# Frame geometry: 10 ms @ 16 kHz, stereo, 16-bit -> 640 bytes/frame.
FRAME_BYTES=640
FRAME_MS=10

IN=$BUILD_DIR/dmic_in.pcm
OUT=$BUILD_DIR/i2s_out.pcm
RUN_LOG=$BUILD_DIR/soak_run.log
TIME_LOG=$BUILD_DIR/soak_time.log

echo "=== build (frames=$FRAMES gain=$GAIN%) ==="
west build -b native_sim/native/64 -d "$BUILD_DIR" "$SAMPLE_DIR" -- \
	-DCONFIG_AUDIO_PIPELINE_NUM_FRAMES="$FRAMES" \
	-DCONFIG_AUDIO_PIPELINE_GAIN_PERCENT="$GAIN" >/dev/null

echo "=== generate $((FRAMES + 16)) frame input tone ==="
python3 "$SCRIPTS/gen_input.py" --frames $((FRAMES + 16)) --out "$IN" >/dev/null

# The native_sim binary idles after main() returns, so bound the run with
# --stop_at one second of simulated time past the expected EOS + drain.
STOP_AT=$(awk "BEGIN { print $FRAMES * $FRAME_MS / 1000 + 1 }")
RTFLAG=--no-rt; [ "$RT" = 1 ] && RTFLAG=--rt

echo "=== run ($RTFLAG, stop_at=${STOP_AT}s) ==="
TIME_BIN=$(command -v time || true)
if [ -x /usr/bin/time ]; then
	/usr/bin/time -v "$BUILD_DIR/zephyr/zephyr.exe" "$RTFLAG" \
		--stop_at="$STOP_AT" --dmic0_file="$IN" --i2s_rxtx_tx="$OUT" \
		>"$RUN_LOG" 2>"$TIME_LOG"
else
	"$BUILD_DIR/zephyr/zephyr.exe" "$RTFLAG" \
		--stop_at="$STOP_AT" --dmic0_file="$IN" --i2s_rxtx_tx="$OUT" >"$RUN_LOG" 2>&1
	: >"$TIME_LOG"
fi

echo "=== verify output (exact gain math + frame count) ==="
python3 "$SCRIPTS/verify_output.py" --in "$IN" --out "$OUT" \
	--gain "$GAIN" --frames "$FRAMES" --frame-bytes "$FRAME_BYTES"

echo "=== drift check (EOS must land at frames x ${FRAME_MS}ms) ==="
EXPECT_S=$(awk "BEGIN { print $FRAMES * $FRAME_MS / 1000 }")
EOS_LINE=$(grep "EOS received" "$RUN_LOG" || true)
echo "  $EOS_LINE"
python3 - "$EOS_LINE" "$EXPECT_S" <<'PY'
import re, sys
line, expect = sys.argv[1], float(sys.argv[2])
m = re.search(r"\[(\d+):(\d+):(\d+)\.(\d+),", line)
if not m:
    sys.exit("FAIL: no EOS timestamp found in run log")
h, mi, s, ms = (int(x) for x in m.groups())
got = h * 3600 + mi * 60 + s + ms / 1000.0
drift = got - expect
print(f"  EOS at {got:.3f}s, expected {expect:.3f}s, drift {drift * 1000:+.0f} ms")
if abs(drift) > 1e-3:
    sys.exit("FAIL: clock drift at EOS")
PY

if [ -s "$TIME_LOG" ]; then
	RSS=$(grep -i "Maximum resident" "$TIME_LOG" | grep -oE "[0-9]+" | tail -1 || true)
	WALL=$(grep -i "wall clock" "$TIME_LOG" | sed -E 's/.*: //' || true)
	echo "=== resources: peak RSS ${RSS:-?} kB, wall ${WALL:-?} ==="
fi

MODE=fast-forward; [ "$RT" = 1 ] && MODE=real-time
echo
echo "SOAK PASS: $FRAMES frames, gain ${GAIN}%, $MODE"
