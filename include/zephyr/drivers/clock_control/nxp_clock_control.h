/*
 * Copyright 2025 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Clock control configuration helpers for NXP devices.
 * @ingroup clock_control_nxp
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_CLOCK_CONTROL_NXP_CLOCK_CONTROL_H_
#define ZEPHYR_INCLUDE_DRIVERS_CLOCK_CONTROL_NXP_CLOCK_CONTROL_H_

#include <zephyr/device.h>
#include <zephyr/devicetree/clocks.h>
#include <zephyr/drivers/clock_control.h>

/**
 * @defgroup clock_control_nxp NXP
 * @ingroup clock_control_interface_ext
 * @{
 */

/** Clock controller and NXP `name` subsystem from one devicetree clock specifier. */
struct nxp_clock_dt_spec {
	/** Clock controller device. */
	const struct device *dev;
	/** NXP clock subsystem identifier. */
	clock_control_subsys_t subsys;
};

/**
 * @brief Initialize an NXP clock specification from a devicetree clock index.
 *
 * NXP clock-control consumers represent the provider's `name` cell as the
 * opaque clock subsystem. Providers without that cell use subsystem zero.
 *
 * @param node_id Devicetree node identifier.
 * @param idx Logical index in the node's `clocks` property.
 */
#define NXP_CLOCK_DT_SPEC_GET_BY_IDX(node_id, idx) \
	{ \
		.dev = DEVICE_DT_GET(DT_CLOCKS_CTLR_BY_IDX(node_id, idx)), \
		.subsys = UINT_TO_POINTER(DT_PHA_BY_IDX_OR(node_id, clocks, idx, name, 0U)), \
	}

/** @brief Initialize the first NXP clock specification for a devicetree node. */
#define NXP_CLOCK_DT_SPEC_GET(node_id) NXP_CLOCK_DT_SPEC_GET_BY_IDX(node_id, 0)

/** @brief Instance form of NXP_CLOCK_DT_SPEC_GET_BY_IDX(). */
#define NXP_CLOCK_DT_SPEC_INST_GET_BY_IDX(inst, idx) \
	NXP_CLOCK_DT_SPEC_GET_BY_IDX(DT_DRV_INST(inst), idx)

/** @brief Instance form of NXP_CLOCK_DT_SPEC_GET(). */
#define NXP_CLOCK_DT_SPEC_INST_GET(inst) NXP_CLOCK_DT_SPEC_GET(DT_DRV_INST(inst))

/**
 * @brief Initialize an NXP clock specification or use a default value.
 *
 * @param node_id Devicetree node identifier.
 * @param idx Logical index in the node's `clocks` property.
 * @param default_value Value used when the index does not exist.
 */
#define NXP_CLOCK_DT_SPEC_GET_BY_IDX_OR(node_id, idx, default_value) \
	COND_CODE_1(DT_CLOCKS_HAS_IDX(node_id, idx), \
		    (NXP_CLOCK_DT_SPEC_GET_BY_IDX(node_id, idx)), (default_value))

/**
 * @brief Check whether an NXP clock specification's provider is ready.
 *
 * @param spec NXP clock specification to check.
 *
 * @retval true The specification has a ready provider.
 * @retval false The specification is NULL, has no provider, or its provider
 *               is not ready.
 */
static inline bool nxp_clock_is_ready_dt(const struct nxp_clock_dt_spec *spec)
{
	return spec != NULL && spec->dev != NULL && device_is_ready(spec->dev);
}

/**
 * @brief Enable an NXP clock described by a devicetree specification.
 *
 * @param spec NXP clock specification to enable.
 *
 * @retval 0 The clock was enabled.
 * @retval -ENODEV @p spec is NULL, has no provider, or its provider is not ready.
 * @return A nonzero backend error unchanged.
 */
static inline int nxp_clock_control_on_dt(const struct nxp_clock_dt_spec *spec)
{
	if (!nxp_clock_is_ready_dt(spec)) {
		return -ENODEV;
	}

	return clock_control_on(spec->dev, spec->subsys);
}

/**
 * @brief Disable an NXP clock described by a devicetree specification.
 *
 * @param spec NXP clock specification to disable.
 *
 * @retval 0 The clock was disabled.
 * @retval -ENODEV @p spec is NULL, has no provider, or its provider is not ready.
 * @return A nonzero backend error unchanged.
 */
static inline int nxp_clock_control_off_dt(const struct nxp_clock_dt_spec *spec)
{
	if (!nxp_clock_is_ready_dt(spec)) {
		return -ENODEV;
	}

	return clock_control_off(spec->dev, spec->subsys);
}

/**
 * @brief Read the rate of an NXP clock described by a devicetree specification.
 *
 * @param spec NXP clock specification to query.
 * @param rate Where to store the clock rate on backend success.
 *
 * @retval 0 The backend stored the clock rate in @p rate.
 * @retval -ENODEV @p spec is NULL, has no provider, or its provider is not ready.
 * @return A nonzero backend error unchanged; @p rate is then as left by the backend.
 */
static inline int nxp_clock_control_get_rate_dt(const struct nxp_clock_dt_spec *spec,
						 uint32_t *rate)
{
	if (!nxp_clock_is_ready_dt(spec)) {
		return -ENODEV;
	}

	return clock_control_get_rate(spec->dev, spec->subsys, rate);
}

#if DT_NODE_HAS_COMPAT_STATUS(DT_NODELABEL(mc_cgm), nxp_mc_cgm, okay)
#include <zephyr/dt-bindings/clock/nxp_mc_cgm.h>
#endif

#if DT_NODE_HAS_COMPAT_STATUS(DT_NODELABEL(firc), nxp_firc, okay)
/** @brief FIRC output divider selection index. */
#define NXP_FIRC_DIV DT_ENUM_IDX(DT_NODELABEL(firc), firc_div)
#endif

#if DT_NODE_HAS_COMPAT_STATUS(DT_NODELABEL(fxosc), nxp_fxosc, okay)
/** @brief FXOSC frequency, in Hz. */
#define NXP_FXOSC_FREQ DT_PROP(DT_NODELABEL(fxosc), freq)
/** @brief FXOSC work mode (crystal or bypass). */
#define NXP_FXOSC_WORKMODE                                                                         \
	(DT_ENUM_IDX(DT_NODELABEL(fxosc), workmode) == 0 ? kFXOSC_ModeCrystal : kFXOSC_ModeBypass)
/** @brief FXOSC startup delay. */
#define NXP_FXOSC_DELAY     DT_PROP(DT_NODELABEL(fxosc), delay)
/** @brief FXOSC overdrive setting. */
#define NXP_FXOSC_OVERDRIVE DT_PROP(DT_NODELABEL(fxosc), overdrive)
#endif

#if DT_NODE_HAS_COMPAT_STATUS(DT_NODELABEL(pll), nxp_plldig, okay)
/** @brief PLL work mode selection index. */
#define NXP_PLL_WORKMODE       DT_ENUM_IDX(DT_NODELABEL(pll), workmode)
/** @brief PLL pre-divider. */
#define NXP_PLL_PREDIV         DT_PROP(DT_NODELABEL(pll), prediv)
/** @brief PLL post-divider. */
#define NXP_PLL_POSTDIV        DT_PROP(DT_NODELABEL(pll), postdiv)
/** @brief PLL loop multiplier. */
#define NXP_PLL_MULTIPLIER     DT_PROP(DT_NODELABEL(pll), multiplier)
/** @brief PLL fractional loop divider. */
#define NXP_PLL_FRACLOOPDIV    DT_PROP(DT_NODELABEL(pll), fracloopdiv)
/** @brief PLL modulation step size. */
#define NXP_PLL_STEPSIZE       DT_PROP(DT_NODELABEL(pll), stepsize)
/** @brief PLL modulation step count. */
#define NXP_PLL_STEPNUM        DT_PROP(DT_NODELABEL(pll), stepnum)
/** @brief PLL accuracy selection index. */
#define NXP_PLL_ACCURACY       DT_ENUM_IDX(DT_NODELABEL(pll), accuracy)
/** @brief PLL output divider table pointer. */
#define NXP_PLL_OUTDIV_POINTER DT_PROP(DT_NODELABEL(pll), outdiv)
#endif

#if DT_NODE_HAS_COMPAT_STATUS(DT_NODELABEL(mc_cgm), nxp_mc_cgm, okay)
/** @brief Maximum input duty-cycle change for the PLL. */
#define NXP_PLL_MAXIDOCHANGE   DT_PROP(DT_NODELABEL(mc_cgm), max_ido_change)
/** @brief PLL step duration. */
#define NXP_PLL_STEPDURATION   DT_PROP(DT_NODELABEL(mc_cgm), step_duration)
/** @brief Clock source frequency feeding the MC_CGM, in Hz. */
#define NXP_PLL_CLKSRCFREQ     DT_PROP(DT_NODELABEL(mc_cgm), clk_src_freq)
/** @brief MC_CGM mux 0 divider 0 value. */
#define NXP_PLL_MUX_0_DC_0_DIV DT_PROP(DT_NODELABEL(mc_cgm), mux_0_dc_0_div)
/** @brief MC_CGM mux 0 divider 1 value. */
#define NXP_PLL_MUX_0_DC_1_DIV DT_PROP(DT_NODELABEL(mc_cgm), mux_0_dc_1_div)
/** @brief MC_CGM mux 0 divider 2 value. */
#define NXP_PLL_MUX_0_DC_2_DIV DT_PROP(DT_NODELABEL(mc_cgm), mux_0_dc_2_div)
/** @brief MC_CGM mux 0 divider 3 value. */
#define NXP_PLL_MUX_0_DC_3_DIV DT_PROP(DT_NODELABEL(mc_cgm), mux_0_dc_3_div)
/** @brief MC_CGM mux 0 divider 4 value. */
#define NXP_PLL_MUX_0_DC_4_DIV DT_PROP(DT_NODELABEL(mc_cgm), mux_0_dc_4_div)
/** @brief MC_CGM mux 0 divider 5 value. */
#define NXP_PLL_MUX_0_DC_5_DIV DT_PROP(DT_NODELABEL(mc_cgm), mux_0_dc_5_div)
/** @brief MC_CGM mux 0 divider 6 value. */
#define NXP_PLL_MUX_0_DC_6_DIV DT_PROP(DT_NODELABEL(mc_cgm), mux_0_dc_6_div)
#endif

/** @} */

#endif /* ZEPHYR_INCLUDE_DRIVERS_CLOCK_CONTROL_NXP_CLOCK_CONTROL_H_ */
