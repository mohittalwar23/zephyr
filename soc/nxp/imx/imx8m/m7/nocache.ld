/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Injected via zephyr_linker_sources(NOINIT).
 *
 * The MCUX SDK drivers put data that must not be cached -- SDMA buffer
 * descriptors and channel context, above all -- in a section named
 * NonCacheable. No Cortex-M linker script collects it, so on this core it has
 * been an orphan: the linker still places it, immediately after noinit, but
 * nothing says so, and a link-order change could move it without warning.
 *
 * This states that placement rather than changing it. The section lands exactly
 * where the orphan already did, and, as before, its contents are not zeroed at
 * startup -- every current user writes a descriptor before the DMA controller
 * reads it.
 *
 * Whether RAMABLE_REGION is genuinely uncached is a separate question this does
 * not answer: it is, on a build running from TCM, and mpu_regions.c re-marks
 * text and data cacheable on a build running from DDR. A driver that needs the
 * guarantee rather than the name should say so through CONFIG_NOCACHE_MEMORY.
 */

*(NonCacheable)
*(NonCacheable.init)
