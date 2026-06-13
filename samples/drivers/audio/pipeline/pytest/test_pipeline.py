# Copyright 2025-2026 NXP
# SPDX-License-Identifier: Apache-2.0

"""Twister pytest harness for the libMP audio pipeline native_sim sample.

One parametrised test drives the whole Section-7 matrix. Each Twister scenario
picks gain / frame count via ``extra_configs`` (read back here from the built
``.config``) and the verification mode via ``--audio-mode`` (see ``conftest.py``).

Flow per scenario:

  1. (gain mode) generate a stereo PCM tone with ``scripts/gen_input.py``;
     (silence mode) withhold the input entirely.
  2. launch the native_sim binary, overriding the DMIC input / I2S output file
     paths on the command line (``--dmic0_file=`` / ``--i2s_rxtx_tx=``) so the
     run is hermetic and does not depend on the build CWD.
  3. wait for the deterministic "Pipeline stopped" log line.
  4. run ``scripts/verify_output.py`` against the captured output and assert it
     reports PASS (exact gain math, frame count, mute/unity, clipping, silence).
"""

import logging
import pathlib
import subprocess
import sys

from twister_harness import DeviceAdapter
from twister_harness.helpers.utils import find_in_config

logger = logging.getLogger(__name__)

HERE = pathlib.Path(__file__).parent
SCRIPTS = HERE.parent / "scripts"
GEN_INPUT = SCRIPTS / "gen_input.py"
VERIFY_OUTPUT = SCRIPTS / "verify_output.py"

# 10 ms @ 16 kHz, stereo, 16-bit: 160 samples * 2 ch * 2 bytes.
FRAME_BYTES = 640
# Generate a few extra input frames so every output frame has a matching input
# sample to compare against.
INPUT_MARGIN_FRAMES = 16


def _config_int(build_dir: pathlib.Path, name: str, default: int) -> int:
    """Read an int Kconfig value from the built .config, with a fallback."""
    value = find_in_config(build_dir / "zephyr" / ".config", name)
    return int(value) if value not in (None, "") else default


def test_pipeline(unlaunched_dut: DeviceAdapter, request):
    dut = unlaunched_dut
    mode = request.config.getoption("--audio-mode")
    build_dir = pathlib.Path(dut.device_config.build_dir)

    gain = _config_int(build_dir, "CONFIG_AUDIO_PIPELINE_GAIN_PERCENT", 90)
    frames = _config_int(build_dir, "CONFIG_AUDIO_PIPELINE_NUM_FRAMES", 100)
    logger.info("scenario: mode=%s gain=%d%% frames=%d", mode, gain, frames)

    in_pcm = build_dir / "dmic_in.pcm"
    out_pcm = build_dir / "i2s_out.pcm"
    # Start from a clean slate so a stale output from a previous run can never
    # masquerade as a pass.
    out_pcm.unlink(missing_ok=True)

    if mode == "gain":
        subprocess.run(
            [
                sys.executable,
                str(GEN_INPUT),
                "--frames",
                str(frames + INPUT_MARGIN_FRAMES),
                "--out",
                str(in_pcm),
            ],
            check=True,
        )
        dmic_path = in_pcm
    else:  # silence: point the DMIC at a file that does not exist
        in_pcm.unlink(missing_ok=True)
        dmic_path = build_dir / "__no_such_input.pcm"

    # Populate the launch command, then inject the file overrides before running.
    dut.generate_command()
    dut.command += [f"--dmic0_file={dmic_path}", f"--i2s_rxtx_tx={out_pcm}"]
    dut.launch()
    dut.readlines_until(regex="Pipeline stopped", timeout=30.0)

    assert out_pcm.exists(), f"pipeline produced no output file: {out_pcm}"

    cmd = [
        sys.executable,
        str(VERIFY_OUTPUT),
        "--in",
        str(dmic_path if mode == "gain" else in_pcm),
        "--out",
        str(out_pcm),
        "--gain",
        str(gain),
        "--frames",
        str(frames),
        "--frame-bytes",
        str(FRAME_BYTES),
    ]
    if mode == "silence":
        cmd.append("--expect-silence")

    result = subprocess.run(cmd, capture_output=True, text=True)
    logger.info("verify_output:\n%s", result.stdout)
    if result.returncode != 0:
        logger.error("verify_output stderr:\n%s", result.stderr)
    assert result.returncode == 0, "verify_output.py reported a failure"
