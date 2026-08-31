/*
 * Copyright (c) 2021, Laird Connectivity
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SOC__H_
#define _SOC__H_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _ASMLANGUAGE


#include <fsl_device_registers.h>
#include <soc_common.h>

/*
 * Nominal AUDIO PLL1 rate. The PLL is programmed from fractional coefficients
 * that settle a few Hz below this, and the HAL's measured root rates truncate
 * a further Hz below the audio targets. Audio consumers derive the MCLK to
 * frame-clock ratio with exact integer division and reject a ratio that does
 * not land on a supported value, so the audio roots are reported from this
 * nominal rate rather than the measured one. Shared with the CCM clock driver
 * so the two cannot drift; soc.c asserts the PLL coefficients against it.
 */
#define IMX8M_M7_AUDIO_PLL1_NOMINAL_RATE 393216000ULL

#endif /* !_ASMLANGUAGE */

#ifdef __cplusplus
}
#endif

#endif /* _SOC__H_ */
