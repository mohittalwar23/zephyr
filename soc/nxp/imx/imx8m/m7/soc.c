/*
 * Copyright (c) 2021, Laird Connectivity
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <fsl_audiomix.h>
#include <fsl_clock.h>
#include <fsl_common.h>
#include <fsl_rdc.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <soc.h>
#include <zephyr/cache.h>

#include <zephyr/dt-bindings/rdc/imx_rdc.h>

/*
 * The audio peripherals all live behind the AUDIOMIX power domain, which only
 * boot firmware can power up and hand to the M7. Enabling any of them without
 * that handoff faults on the first AUDIOMIX access, so it is refused here.
 */
#define IMX8M_M7_SAI3_ACTIVE DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(sai3))
#define IMX8M_M7_SDMA3_ACTIVE DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(sdma3))
#define IMX8M_M7_MICFIL_ACTIVE DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(micfil))
#define IMX8M_M7_AUDIOMIX_ACTIVE                                                \
	(IMX8M_M7_SAI3_ACTIVE || IMX8M_M7_SDMA3_ACTIVE || IMX8M_M7_MICFIL_ACTIVE)

BUILD_ASSERT(!IMX8M_M7_AUDIOMIX_ACTIVE ||
	     IS_ENABLED(CONFIG_IMX8M_M7_AUDIOMIX_BOOT_HANDOFF),
	     "SAI3, SDMA3, and MICFIL require boot-firmware AUDIOMIX power handoff");

/*
 * AUDIO PLL1 is a fractional PLL:
 *
 *   Fout = (mainDiv + dsm / 65536) * refSel / (preDiv * 2^postDiv)
 *
 * The coefficients below are the ones handed to CLOCK_InitAudioPll1(), so the
 * static assertions fail if either a PLL coefficient or a root divider stops
 * producing the intended audio rates.
 */
#define IMX8M_M7_AUDIO_PLL1_REF_SEL kANALOG_PllRefOsc24M
#define IMX8M_M7_AUDIO_PLL1_REF_RATE 24000000ULL
#define IMX8M_M7_AUDIO_PLL1_MAIN_DIV 262ULL
#define IMX8M_M7_AUDIO_PLL1_DSM 9437ULL
#define IMX8M_M7_AUDIO_PLL1_PRE_DIV 2ULL
#define IMX8M_M7_AUDIO_PLL1_POST_DIV 3ULL
#define IMX8M_M7_AUDIO_PLL1_DSM_SCALE 65536ULL

/* Scaled numerator/denominator keep the derivation exact in integer math. */
#define IMX8M_M7_AUDIO_PLL1_NUM							\
	((IMX8M_M7_AUDIO_PLL1_MAIN_DIV * IMX8M_M7_AUDIO_PLL1_DSM_SCALE +	\
	  IMX8M_M7_AUDIO_PLL1_DSM) * IMX8M_M7_AUDIO_PLL1_REF_RATE)
#define IMX8M_M7_AUDIO_PLL1_DEN							\
	(IMX8M_M7_AUDIO_PLL1_DSM_SCALE * IMX8M_M7_AUDIO_PLL1_PRE_DIV *		\
	 BIT64(IMX8M_M7_AUDIO_PLL1_POST_DIV))

#define IMX8M_M7_AUDIO_PLL1_RESIDUE 8ULL

#define IMX8M_M7_SAI3_ROOT_DIVIDER 32ULL
#define IMX8M_M7_PDM_ROOT_DIVIDER 2ULL
#define IMX8M_M7_PDM_ROOT_NOMINAL 196608000ULL

BUILD_ASSERT(IMX8M_M7_AUDIO_PLL1_REF_SEL == kANALOG_PllRefOsc24M,
	     "AUDIO PLL1 rate derivation assumes the 24 MHz reference");

/*
 * A single-step error in any PLL coefficient moves the rate far more than the
 * fractional residue, so this catches a coefficient typo without asserting the
 * PLL lands exactly on the nominal rate, which it cannot.
 */
BUILD_ASSERT(IMX8M_M7_AUDIO_PLL1_NUM / IMX8M_M7_AUDIO_PLL1_DEN <=
		     IMX8M_M7_AUDIO_PLL1_NOMINAL_RATE &&
	     IMX8M_M7_AUDIO_PLL1_NUM / IMX8M_M7_AUDIO_PLL1_DEN >=
		     IMX8M_M7_AUDIO_PLL1_NOMINAL_RATE - IMX8M_M7_AUDIO_PLL1_RESIDUE,
	     "AUDIO PLL1 coefficients must produce 393.216 MHz within the fractional residue");

/*
 * The dividers the roots are programmed with must derive the audio rates
 * exactly from the nominal PLL rate, since that is what the clock driver
 * reports. PDM_SetSampleRateConfig() consumes the PDM root rather than the PLL
 * rate, and MICFIL divides that root again, so the root is left at the
 * 196.608 MHz that Linux programs on i.MX8MP. A lower root would still divide
 * exactly, but it leaves the PDM clock divider too small for the lower quality
 * modes with many channels.
 */
BUILD_ASSERT(IMX8M_M7_AUDIO_PLL1_NOMINAL_RATE / IMX8M_M7_SAI3_ROOT_DIVIDER == 12288000ULL,
	     "SAI3 MCLK must be 12.288 MHz");
BUILD_ASSERT(IMX8M_M7_AUDIO_PLL1_NOMINAL_RATE / IMX8M_M7_PDM_ROOT_DIVIDER ==
		     IMX8M_M7_PDM_ROOT_NOMINAL,
	     "PDM root must be 196.608 MHz");

/* OSC/PLL is already initialized by ROM and Cortex-A53 (u-boot) */
static void SOC_RdcInit(void)
{
	/* Move M7 core to specific RDC domain 1 */
	rdc_domain_assignment_t assignment = {0};
	uint8_t domainId                   = 0U;

	domainId = RDC_GetCurrentMasterDomainId(RDC);
	/* Only configure the RDC if RDC peripheral write access allowed. */
	if ((0x1U & RDC_GetPeriphAccessPolicy(RDC, kRDC_Periph_RDC, domainId)) != 0U) {
		assignment.domainId = M7_DOMAIN_ID;
		RDC_SetMasterDomainAssignment(RDC, kRDC_Master_M7, &assignment);
	}

	/*
	 * The M7 core is running at domain 1, now enable the clock gate of the following IP/BUS/PLL
	 * in domain 1 in the CCM. In this way, to ensure the clock of the peripherals used by M
	 * core not be affected by A core which is running at domain 0.
	 */
	CLOCK_EnableClock(kCLOCK_Iomux);

	CLOCK_EnableClock(kCLOCK_Ipmux1);
	CLOCK_EnableClock(kCLOCK_Ipmux2);
	CLOCK_EnableClock(kCLOCK_Ipmux3);

#if defined(FLASH_TARGET)
	CLOCK_EnableClock(kCLOCK_Qspi);
#endif

	/* Enable the CCGR gate for SysPLL1 in Domain 1 */
	CLOCK_ControlGate(kCLOCK_SysPll1Gate, kCLOCK_ClockNeededAll);
	/* Enable the CCGR gate for SysPLL2 in Domain 1 */
	CLOCK_ControlGate(kCLOCK_SysPll2Gate, kCLOCK_ClockNeededAll);
	/* Enable the CCGR gate for SysPLL3 in Domain 1 */
	CLOCK_ControlGate(kCLOCK_SysPll3Gate, kCLOCK_ClockNeededAll);
#ifdef CONFIG_INIT_VIDEO_PLL
	/* Enable the CCGR gate for VideoPLL1 in Domain 1 */
	CLOCK_ControlGate(kCLOCK_VideoPll1Gate, kCLOCK_ClockNeededAll);
#endif
}

/* Integer PLLs: Fout = (mainDiv * refSel) / (preDiv * 2^ postDiv) */
/* SYSTEM PLL1 configuration */
const ccm_analog_integer_pll_config_t g_sysPll1Config = {
	.refSel  = kANALOG_PllRefOsc24M, /*!< PLL reference OSC24M */
	.mainDiv = 400U,
	.preDiv  = 3U,
	.postDiv = 2U, /*!< SYSTEM PLL1 frequency  = 800MHZ */
};

/* SYSTEM PLL2 configuration */
const ccm_analog_integer_pll_config_t g_sysPll2Config = {
	.refSel  = kANALOG_PllRefOsc24M, /*!< PLL reference OSC24M */
	.mainDiv = 250U,
	.preDiv  = 3U,
	.postDiv = 1U, /*!< SYSTEM PLL2 frequency  = 1000MHZ */
};

/* SYSTEM PLL3 configuration */
const ccm_analog_integer_pll_config_t g_sysPll3Config = {
	.refSel  = kANALOG_PllRefOsc24M, /*!< PLL reference OSC24M */
	.mainDiv = 300,
	.preDiv  = 3U,
	.postDiv = 2U, /*!< SYSTEM PLL3 frequency  = 600MHZ */
};

#if defined(CONFIG_IMX8M_M7_AUDIO_PLL1) && IMX8M_M7_AUDIOMIX_ACTIVE
/* Fractional PLLs: Fout = (mainDiv + dsm / 65536) * refSel / (preDiv * 2^postDiv) */
/* AUDIO PLL1 configuration */
const ccm_analog_frac_pll_config_t g_audioPll1Config = {
	.refSel = IMX8M_M7_AUDIO_PLL1_REF_SEL, /*!< PLL reference OSC24M */
	.mainDiv = IMX8M_M7_AUDIO_PLL1_MAIN_DIV,
	.dsm = IMX8M_M7_AUDIO_PLL1_DSM,
	.preDiv = IMX8M_M7_AUDIO_PLL1_PRE_DIV,
	.postDiv = IMX8M_M7_AUDIO_PLL1_POST_DIV, /*!< AUDIO PLL1 frequency = 393216000HZ */
};
#endif

__weak void SOC_ClockInit(void)
{
	/*
	 * The following steps just show how to configure the PLL clock sources using the clock
	 * driver on M7 core side . Please note that the ROM has already configured the SYSTEM PLL1
	 * to 800Mhz when power up the SOC, meanwhile A core would enable SYSTEM PLL1, SYSTEM PLL2
	 * and SYSTEM PLL3 by U-Boot. Therefore, there is no need to configure the system PLL again
	 * on M7 side, otherwise it would have a risk to make the SOC hang.
	 */

	/* switch AHB NOC root to 24M first in order to configure the SYSTEM PLL1. */
	CLOCK_SetRootMux(kCLOCK_RootAhb, kCLOCK_AhbRootmuxOsc24M);

	/* switch AXI M7 root to 24M first in order to configure the SYSTEM PLL2. */
	CLOCK_SetRootMux(kCLOCK_RootM7, kCLOCK_M7RootmuxOsc24M);

	/* Set root clock to 800M */
	CLOCK_SetRootDivider(kCLOCK_RootM7, 1U, 1U);
	/* switch cortex-m7 to SYSTEM PLL1 */
	CLOCK_SetRootMux(kCLOCK_RootM7, kCLOCK_M7RootmuxSysPll1);

	/* Set root clock freq to 133M / 1= 133MHZ */
	CLOCK_SetRootDivider(kCLOCK_RootAhb, 1U, 1U);
	/* switch AHB to SYSTEM PLL1 DIV6 */
	CLOCK_SetRootMux(kCLOCK_RootAhb, kCLOCK_AhbRootmuxSysPll1Div6);

#if defined(CONFIG_UART_MCUX_IUART)
#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(uart1))
	/* Set UART source to SysPLL1 Div10 80MHZ */
	CLOCK_SetRootMux(kCLOCK_RootUart1, kCLOCK_UartRootmuxSysPll1Div10);
	/* Set root clock to 80MHZ/ 1= 80MHZ */
	CLOCK_SetRootDivider(kCLOCK_RootUart1, 1U, 1U);
#endif
#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(uart2))
	/* Set UART source to SysPLL1 Div10 80MHZ */
	CLOCK_SetRootMux(kCLOCK_RootUart2, kCLOCK_UartRootmuxSysPll1Div10);
	/* Set root clock to 80MHZ/ 1= 80MHZ */
	CLOCK_SetRootDivider(kCLOCK_RootUart2, 1U, 1U);
#endif
#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(uart3))
	/* Set UART source to SysPLL1 Div10 80MHZ */
	CLOCK_SetRootMux(kCLOCK_RootUart3, kCLOCK_UartRootmuxSysPll1Div10);
	/* Set root clock to 80MHZ/ 1= 80MHZ */
	CLOCK_SetRootDivider(kCLOCK_RootUart3, 1U, 1U);
#endif
#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(uart4))
	/* Set UART source to SysPLL1 Div10 80MHZ */
	CLOCK_SetRootMux(kCLOCK_RootUart4, kCLOCK_UartRootmuxSysPll1Div10);
	/* Set root clock to 80MHZ/ 1= 80MHZ */
	CLOCK_SetRootDivider(kCLOCK_RootUart4, 1U, 1U);
#endif
#endif

#if defined(CONFIG_SPI_MCUX_ECSPI)
#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(ecspi1))
	/* Set ECSPI1 source to SYSTEM PLL1 800MHZ */
	CLOCK_SetRootMux(kCLOCK_RootEcspi1, kCLOCK_EcspiRootmuxSysPll1);
	/* Set root clock to 800MHZ / 10 = 80MHZ */
	CLOCK_SetRootDivider(kCLOCK_RootEcspi1, 2U, 5U);
#endif

#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(ecspi2))
	/* Set ECSPI2 source to SYSTEM PLL1 800MHZ */
	CLOCK_SetRootMux(kCLOCK_RootEcspi2, kCLOCK_EcspiRootmuxSysPll1);
	/* Set root clock to 800MHZ / 10 = 80MHZ */
	CLOCK_SetRootDivider(kCLOCK_RootEcspi2, 2U, 5U);
#endif

#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(ecspi3))
	/* Set ECSPI3 source to SYSTEM PLL1 800MHZ */
	CLOCK_SetRootMux(kCLOCK_RootEcspi3, kCLOCK_EcspiRootmuxSysPll1);
	/* Set root clock to 800MHZ / 10 = 80MHZ */
	CLOCK_SetRootDivider(kCLOCK_RootEcspi3, 2U, 5U);
#endif
#endif

#if defined(CONFIG_IMX8M_M7_AUDIO_PLL1) && IMX8M_M7_AUDIOMIX_ACTIVE
	/* Enable the CCGR gate for AudioPLL1 in Domain 1 */
	CLOCK_ControlGate(kCLOCK_AudioPll1Gate, kCLOCK_ClockNeededAll);
	/* Init AUDIO PLL1 to 393216000HZ for the 48 kHz sample rate family */
	CLOCK_InitAudioPll1(&g_audioPll1Config);

	/*
	 * AUDIO AHB is the bus clock root shared by the AUDIOMIX peripherals
	 * (SAI, SDMA2/3, MICFIL). Set it to SYSTEM PLL1 800MHZ / 2 = 400MHZ.
	 * The AUDIOMIX CCGR gate is enabled below whoever owns the PLL, and the
	 * per-IP gates inside AUDIOMIX are managed by the clock control driver.
	 */
	CLOCK_SetRootMux(kCLOCK_RootAudioAhb, kCLOCK_AudioAhbRootmuxSysPll1);
	CLOCK_SetRootDivider(kCLOCK_RootAudioAhb, 1U, 2U);
#endif

#if defined(CONFIG_IMX8M_M7_AUDIO_PLL1) && IMX8M_M7_SAI3_ACTIVE
	/* Set SAI3 source to AUDIO PLL1 393216000HZ / 32 = 12288000HZ (MCLK) */
	CLOCK_SetRootMux(kCLOCK_RootSai3, kCLOCK_SaiRootmuxAudioPll1);
	CLOCK_SetRootDivider(kCLOCK_RootSai3, 1U, IMX8M_M7_SAI3_ROOT_DIVIDER);
	/*
	 * The AUDIOMIX gates don't manage this root (see kCLOCK_Sai3),
	 * so it must be enabled explicitly.
	 */
	CLOCK_EnableRoot(kCLOCK_RootSai3);
#endif

#if defined(CONFIG_IMX8M_M7_AUDIO_PLL1) && IMX8M_M7_MICFIL_ACTIVE
	/*
	 * Set PDM source to AUDIO PLL1 393216000HZ / 2 = 196608000HZ, the rate
	 * Linux uses for MICFIL on i.MX8MP. PDM_SetSampleRateConfig() divides
	 * this root, not the PLL.
	 */
	CLOCK_SetRootMux(kCLOCK_RootPdm, kCLOCK_PdmRootmuxAudioPll1);
	CLOCK_SetRootDivider(kCLOCK_RootPdm, 1U, IMX8M_M7_PDM_ROOT_DIVIDER);
	CLOCK_EnableRoot(kCLOCK_RootPdm);
#endif

#if IMX8M_M7_AUDIOMIX_ACTIVE
	/*
	 * The AUDIOMIX CCGR gate and the clock attachments are needed whoever
	 * owns Audio PLL1: without them the M7 cannot reach the roots that boot
	 * firmware handed over. Each attachment identity is one register field,
	 * so exactly one identity per field is written; a second call to the
	 * same field would silently replace the first.
	 */
	CLOCK_EnableClock(kCLOCK_Audio);
#endif

#if IMX8M_M7_SAI3_ACTIVE
	/* AUDIOMIX 0x308 bit 0: SAI3 MCLK1 follows the CCM SAI3 root. */
	AUDIOMIX_AttachClk(AUDIOMIX, kAUDIOMIX_Attach_SAI3_MCLK1_To_SAI3_ROOT);
#endif

#if IMX8M_M7_MICFIL_ACTIVE
	/* AUDIOMIX 0x318 bits 1:0: the PDM root follows the CCM PDM clock. */
	AUDIOMIX_AttachClk(AUDIOMIX, kAUDIOMIX_Attach_PDM_Root_to_CCM_PDM);
#endif

	CLOCK_EnableClock(kCLOCK_Rdc);   /* Enable RDC clock */
	CLOCK_EnableClock(kCLOCK_Ocram); /* Enable Ocram clock */

	/* The purpose to enable the following modules clock is to make sure the M7 core could work
	 * normally when A53 core enters the low power status.
	 */
	CLOCK_EnableClock(kCLOCK_Sim_m);
	CLOCK_EnableClock(kCLOCK_Sim_main);
	CLOCK_EnableClock(kCLOCK_Sim_s);
	CLOCK_EnableClock(kCLOCK_Sim_wakeup);
	CLOCK_EnableClock(kCLOCK_Debug);
	CLOCK_EnableClock(kCLOCK_Dram);
	CLOCK_EnableClock(kCLOCK_Sec_Debug);
}

static void gpio_init(void)
{

#if defined(CONFIG_GPIO_MCUX_IGPIO)
#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(gpio1))

	CLOCK_EnableClock(kCLOCK_Gpio1);

#endif
#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(gpio2))

	CLOCK_EnableClock(kCLOCK_Gpio2);

#endif
#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(gpio3))

	CLOCK_EnableClock(kCLOCK_Gpio3);

#endif
#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(gpio4))

	CLOCK_EnableClock(kCLOCK_Gpio4);

#endif
#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(gpio5))

	CLOCK_EnableClock(kCLOCK_Gpio5);

#endif
#endif
}

void soc_early_init_hook(void)
{

#ifdef CONFIG_CACHE_MANAGEMENT
	sys_cache_data_enable();
	sys_cache_instr_enable();
#endif /* CONFIG_CACHE_MANAGEMENT */

	/* SoC specific RDC settings */
	SOC_RdcInit();

	/* SoC specific Clock settings */
	SOC_ClockInit();

	gpio_init();
}
