# ESP32 PDM microphone bring-up (Adafruit 3492) on Zephyr

Working notes for capturing a PDM MEMS microphone (Adafruit 3492) on a classic
ESP32 (esp32_devkitc) under Zephyr, and playing it back through a MAX98357A I2S
amplifier. Companion to `GSOC_MP_FIXES_NOTES.md` and `GSOC_HW_COMPAT_RESEARCH.md`.

## Status: WORKING (loopback)

Mic -> ESP32 hardware PDM->PCM -> loopback pipeline -> MAX98357A speaker.
Validated with the `zephyr-audio-demos/audio_pipeline` loopback sample. You can
hear yourself with correct pitch/rate.

## The key finding

The classic ESP32 (and S3) has a HARDWARE PDM-to-PCM filter in the I2S
peripheral. soc_caps: `SOC_I2S_SUPPORTS_PDM_RX=1`, `SOC_I2S_SUPPORTS_PDM2PCM=1`.
So a PDM mic needs NO software CIC/FIR decimator: enable PDM RX, and `i2s_read()`
returns finished 16-bit PCM. The ESP-IDF HAL bundled in Zephyr already exposes
this; Zephyr's `i2s_esp32.c` simply never called it (it only did standard/TDM).

## What was changed (driver)

`drivers/i2s/i2s_esp32.c` - a PDM RX branch in `i2s_esp32_configure()`, gated on
`CONFIG_I2S_ESP32_PDM_RX` (new, `drivers/i2s/Kconfig.esp32`). When enabled, the RX
channel is set up in PDM mode instead of standard:

- PDM interface clock recomputed as `frame_clk_freq * I2S_LL_PDM_BCK_FACTOR (64)`
  (the standard I2S bit-clock is wrong for PDM and caused an 8x-slow capture).
- `slot_mode = STEREO`, `pdm_rx.slot_mask = BOTH`, `pdm_rx.data_fmt = PCM`
  (present the single mono mic as stereo frames so downstream rate accounting and
  a stereo I2S sink stay consistent).
- HAL calls: `i2s_hal_pdm_set_rx_slot()`, `i2s_hal_set_rx_clock()`,
  `i2s_ll_rx_set_pdm_dsr(I2S_PDM_DSR_8S)`, `i2s_hal_pdm_enable_rx_channel(true)`.

## Debugging log (how we converged, for reference)

The demo primes a 250-block delay; its prime TIME is a precise rate ruler
(250 blocks should take ~1 s at 16 kHz).
- First try (reused standard clock, DSR_16, mono): prime took 8 s -> 8x too slow.
  Cause = wrong PDM clock (rate*32 vs needed rate*64) x DSR mismatch x mono/stereo.
- Fixed PDM clock (rate*64) + DSR_8: 8 s -> 2 s (4x better).
- Presented as stereo (slot_mode STEREO, mask BOTH): 2 s -> ~1 s, TX starvation
  errors (i2s_write -5/-11) cleared. The earlier -5/-11 were the slow mic starving
  the stereo sink, not a separate bug.

## Wiring (esp32_devkitc)

Adafruit 3492 PDM mic:  CLK->GPIO18 (WS line), DAT->GPIO19 (SD line), SEL->GND
(LEFT slot), 3V->3V3, GND->GND.
MAX98357A amp:  BCLK->GPIO26, LRC->GPIO25, DIN->GPIO22, VIN->5V, GND->GND,
SD floating (enabled), GAIN->GND for +12 dB. Speaker +/- to the terminals.
Common ground between mic, amp and ESP32 is required.

## Build / run (in zephyr-audio-demos/audio_pipeline)

    west build -b esp32_devkitc/esp32/procpu . -- \
        -DPIPELINE=loopback -DOVERLAY_CONFIG=pdm.conf
    west flash
    west espressif monitor

Board files (in the demo repo): `boards/esp32_devkitc_esp32_procpu.overlay`
(i2s0 pinmux switched to PDM WS=18/SD=19, no BCK) and `pdm.conf`
(`CONFIG_I2S_ESP32_PDM_RX=y`).

## Is it "proper" or a shortcut?

Mechanism = proper (real HAL, hardware PDM2PCM, no bypass). Integration = bring-up
shortcut, NOT upstream-PR-ready as-is:
1. Gated on a GLOBAL Kconfig -> makes all I2S RX on the board PDM. Proper: a
   per-node devicetree property (e.g. a `pdm` bool on the i2s node) or a dedicated
   compatible.
2. Exposed via the I2S API. In Zephyr, PDM mics properly belong to the DMIC API
   (`dmic_configure/trigger/read/get_caps`). This is why libMP `dmic_src` cannot
   use it yet.
3. DSR/clock hardcoded for the 16 kHz path (not parameterized for arbitrary rates).
4. slot_mask=BOTH with one mic fills stereo pragmatically, not a true mono capture.

## Why the libMP dmic_i2s sample does NOT run on ESP32 yet

- `dmic_src` uses the DMIC API; our hack is on the I2S API -> not visible to it.
- `i2s_codec_sink` needs `i2s_get_caps` (ESP32 i2s still lacks get_caps).

## Next steps (to run libMP on ESP32)

1. `dmic_esp32_pdm` - a real DMIC driver wrapping the same HAL PDM calls proven
   here, implementing dmic_configure/trigger/read/get_caps. Reference:
   dmic_nrfx_pdm.c (565 L) / dmic_ambiq_pdm.c (356 L). Makes `dmic_src` work.
2. `get_caps` on i2s_esp32 (for `i2s_codec_sink`).
3. A `zephyr,dummy-codec` node (MAX98357A needs no I2C control).
Then the libMP dmic_i2s sample can run on ESP32 with this mic + MAX98357A.
