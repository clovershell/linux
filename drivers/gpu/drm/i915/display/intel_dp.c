/*
 * Copyright © 2008 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Keith Packard <keithp@keithp.com>
 *
 */

#include <linux/export.h>
#include <linux/i2c.h>
#include <linux/log2.h>
#include <linux/math.h>
#include <linux/notifier.h>
#include <linux/seq_buf.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/string_helpers.h>
#include <linux/timekeeping.h>
#include <linux/types.h>
#include <asm/byteorder.h>

#include <drm/display/drm_dp_helper.h>
#include <drm/display/drm_dp_tunnel.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/display/drm_hdmi_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_edid.h>
#include <drm/drm_fixed.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include "g4x_dp.h"
#include "i915_utils.h"
#include "intel_alpm.h"
#include "intel_atomic.h"
#include "intel_audio.h"
#include "intel_backlight.h"
#include "intel_combo_phy_regs.h"
#include "intel_connector.h"
#include "intel_crtc.h"
#include "intel_crtc_state_dump.h"
#include "intel_cx0_phy.h"
#include "intel_ddi.h"
#include "intel_de.h"
#include "intel_display_driver.h"
#include "intel_display_regs.h"
#include "intel_display_rpm.h"
#include "intel_display_types.h"
#include "intel_dp.h"
#include "intel_dp_aux.h"
#include "intel_dp_hdcp.h"
#include "intel_dp_link_training.h"
#include "intel_dp_mst.h"
#include "intel_dp_test.h"
#include "intel_dp_tunnel.h"
#include "intel_dpio_phy.h"
#include "intel_dpll.h"
#include "intel_drrs.h"
#include "intel_encoder.h"
#include "intel_fifo_underrun.h"
#include "intel_hdcp.h"
#include "intel_hdmi.h"
#include "intel_hotplug.h"
#include "intel_hotplug_irq.h"
#include "intel_lspcon.h"
#include "intel_lvds.h"
#include "intel_modeset_lock.h"
#include "intel_panel.h"
#include "intel_pch_display.h"
#include "intel_pfit.h"
#include "intel_pps.h"
#include "intel_psr.h"
#include "intel_quirks.h"
#include "intel_tc.h"
#include "intel_vdsc.h"
#include "intel_vrr.h"

/* DP DSC throughput values used for slice count calculations KPixels/s */
#define DP_DSC_PEAK_PIXEL_RATE			2720000
#define DP_DSC_MAX_ENC_THROUGHPUT_0		340000
#define DP_DSC_MAX_ENC_THROUGHPUT_1		400000

/* Max DSC line buffer depth supported by HW. */
#define INTEL_DP_DSC_MAX_LINE_BUF_DEPTH		13

/* DP DSC FEC Overhead factor in ppm = 1/(0.972261) = 1.028530 */
#define DP_DSC_FEC_OVERHEAD_FACTOR		1028530

/* Constants for DP DSC configurations */
static const u8 valid_dsc_bpp[] = {6, 8, 10, 12, 15};

/*
 * With Single pipe configuration, HW is capable of supporting maximum of:
 * 2 slices per line for ICL, BMG
 * 4 slices per line for other platforms.
 * For now consider a max of 2 slices per line, which works for all platforms.
 * With this we can have max of 4 DSC Slices per pipe.
 *
 * For higher resolutions where 12 slice support is required with
 * ultrajoiner, only then each pipe can support 3 slices.
 *
 * #TODO Split this better to use 4 slices/dsc engine where supported.
 */
static const u8 valid_dsc_slicecount[] = {1, 2, 3, 4};

/**
 * intel_dp_is_edp - is the given port attached to an eDP panel (either CPU or PCH)
 * @intel_dp: DP struct
 *
 * If a CPU or PCH DP output is attached to an eDP panel, this function
 * will return true, and false otherwise.
 *
 * This function is not safe to use prior to encoder type being set.
 */
bool intel_dp_is_edp(struct intel_dp *intel_dp)
{
	struct intel_digital_port *dig_port = dp_to_dig_port(intel_dp);

	return dig_port->base.type == INTEL_OUTPUT_EDP;
}

static void intel_dp_unset_edid(struct intel_dp *intel_dp);

/* Is link rate UHBR and thus 128b/132b? */
bool intel_dp_is_uhbr(const struct intel_crtc_state *crtc_state)
{
	return drm_dp_is_uhbr_rate(crtc_state->port_clock);
}

/**
 * intel_dp_link_symbol_size - get the link symbol size for a given link rate
 * @rate: link rate in 10kbit/s units
 *
 * Returns the link symbol size in bits/symbol units depending on the link
 * rate -> channel coding.
 */
int intel_dp_link_symbol_size(int rate)
{
	return drm_dp_is_uhbr_rate(rate) ? 32 : 10;
}

/**
 * intel_dp_link_symbol_clock - convert link rate to link symbol clock
 * @rate: link rate in 10kbit/s units
 *
 * Returns the link symbol clock frequency in kHz units depending on the
 * link rate and channel coding.
 */
int intel_dp_link_symbol_clock(int rate)
{
	return DIV_ROUND_CLOSEST(rate * 10, intel_dp_link_symbol_size(rate));
}

static int max_dprx_rate(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_encoder *encoder = &dp_to_dig_port(intel_dp)->base;
	int max_rate;

	if (intel_dp_tunnel_bw_alloc_is_enabled(intel_dp))
		max_rate = drm_dp_tunnel_max_dprx_rate(intel_dp->tunnel);
	else
		max_rate = drm_dp_bw_code_to_link_rate(intel_dp->dpcd[DP_MAX_LINK_RATE]);

	/*
	 * Some broken eDP sinks illegally declare support for
	 * HBR3 without TPS4, and are unable to produce a stable
	 * output. Reject HBR3 when TPS4 is not available.
	 */
	if (max_rate >= 810000 && !drm_dp_tps4_supported(intel_dp->dpcd)) {
		drm_dbg_kms(display->drm,
			    "[ENCODER:%d:%s] Rejecting HBR3 due to missing TPS4 support\n",
			    encoder->base.base.id, encoder->base.name);
		max_rate = 540000;
	}

	return max_rate;
}

static int max_dprx_lane_count(struct intel_dp *intel_dp)
{
	if (intel_dp_tunnel_bw_alloc_is_enabled(intel_dp))
		return drm_dp_tunnel_max_dprx_lane_count(intel_dp->tunnel);

	return drm_dp_max_lane_count(intel_dp->dpcd);
}

static void intel_dp_set_default_sink_rates(struct intel_dp *intel_dp)
{
	intel_dp->sink_rates[0] = 162000;
	intel_dp->num_sink_rates = 1;
}

/* update sink rates from dpcd */
static void intel_dp_set_dpcd_sink_rates(struct intel_dp *intel_dp)
{
	static const int dp_rates[] = {
		162000, 270000, 540000, 810000
	};
	int i, max_rate;
	int max_lttpr_rate;

	if (drm_dp_has_quirk(&intel_dp->desc, DP_DPCD_QUIRK_CAN_DO_MAX_LINK_RATE_3_24_GBPS)) {
		/* Needed, e.g., for Apple MBP 2017, 15 inch eDP Retina panel */
		static const int quirk_rates[] = { 162000, 270000, 324000 };

		memcpy(intel_dp->sink_rates, quirk_rates, sizeof(quirk_rates));
		intel_dp->num_sink_rates = ARRAY_SIZE(quirk_rates);

		return;
	}

	/*
	 * Sink rates for 8b/10b.
	 */
	max_rate = max_dprx_rate(intel_dp);
	max_lttpr_rate = drm_dp_lttpr_max_link_rate(intel_dp->lttpr_common_caps);
	if (max_lttpr_rate)
		max_rate = min(max_rate, max_lttpr_rate);

	for (i = 0; i < ARRAY_SIZE(dp_rates); i++) {
		if (dp_rates[i] > max_rate)
			break;
		intel_dp->sink_rates[i] = dp_rates[i];
	}

	/*
	 * Sink rates for 128b/132b. If set, sink should support all 8b/10b
	 * rates and 10 Gbps.
	 */
	if (drm_dp_128b132b_supported(intel_dp->dpcd)) {
		u8 uhbr_rates = 0;

		BUILD_BUG_ON(ARRAY_SIZE(intel_dp->sink_rates) < ARRAY_SIZE(dp_rates) + 3);

		drm_dp_dpcd_readb(&intel_dp->aux,
				  DP_128B132B_SUPPORTED_LINK_RATES, &uhbr_rates);

		if (drm_dp_lttpr_count(intel_dp->lttpr_common_caps)) {
			/* We have a repeater */
			if (intel_dp->lttpr_common_caps[0] >= 0x20 &&
			    intel_dp->lttpr_common_caps[DP_MAIN_LINK_CHANNEL_CODING_PHY_REPEATER -
							DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV] &
			    DP_PHY_REPEATER_128B132B_SUPPORTED) {
				/* Repeater supports 128b/132b, valid UHBR rates */
				uhbr_rates &= intel_dp->lttpr_common_caps[DP_PHY_REPEATER_128B132B_RATES -
									  DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV];
			} else {
				/* Does not support 128b/132b */
				uhbr_rates = 0;
			}
		}

		if (uhbr_rates & DP_UHBR10)
			intel_dp->sink_rates[i++] = 1000000;
		if (uhbr_rates & DP_UHBR13_5)
			intel_dp->sink_rates[i++] = 1350000;
		if (uhbr_rates & DP_UHBR20)
			intel_dp->sink_rates[i++] = 2000000;
	}

	intel_dp->num_sink_rates = i;
}

static void intel_dp_set_sink_rates(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_connector *connector = intel_dp->attached_connector;
	struct intel_digital_port *intel_dig_port = dp_to_dig_port(intel_dp);
	struct intel_encoder *encoder = &intel_dig_port->base;

	intel_dp_set_dpcd_sink_rates(intel_dp);

	if (intel_dp->num_sink_rates)
		return;

	drm_err(display->drm,
		"[CONNECTOR:%d:%s][ENCODER:%d:%s] Invalid DPCD with no link rates, using defaults\n",
		connector->base.base.id, connector->base.name,
		encoder->base.base.id, encoder->base.name);

	intel_dp_set_default_sink_rates(intel_dp);
}

static void intel_dp_set_default_max_sink_lane_count(struct intel_dp *intel_dp)
{
	intel_dp->max_sink_lane_count = 1;
}

static void intel_dp_set_max_sink_lane_count(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_connector *connector = intel_dp->attached_connector;
	struct intel_digital_port *intel_dig_port = dp_to_dig_port(intel_dp);
	struct intel_encoder *encoder = &intel_dig_port->base;

	intel_dp->max_sink_lane_count = max_dprx_lane_count(intel_dp);

	switch (intel_dp->max_sink_lane_count) {
	case 1:
	case 2:
	case 4:
		return;
	}

	drm_err(display->drm,
		"[CONNECTOR:%d:%s][ENCODER:%d:%s] Invalid DPCD max lane count (%d), using default\n",
		connector->base.base.id, connector->base.name,
		encoder->base.base.id, encoder->base.name,
		intel_dp->max_sink_lane_count);

	intel_dp_set_default_max_sink_lane_count(intel_dp);
}

/* Get length of rates array potentially limited by max_rate. */
static int intel_dp_rate_limit_len(const int *rates, int len, int max_rate)
{
	int i;

	/* Limit results by potentially reduced max rate */
	for (i = 0; i < len; i++) {
		if (rates[len - i - 1] <= max_rate)
			return len - i;
	}

	return 0;
}

/* Get length of common rates array potentially limited by max_rate. */
static int intel_dp_common_len_rate_limit(const struct intel_dp *intel_dp,
					  int max_rate)
{
	return intel_dp_rate_limit_len(intel_dp->common_rates,
				       intel_dp->num_common_rates, max_rate);
}

int intel_dp_common_rate(struct intel_dp *intel_dp, int index)
{
	struct intel_display *display = to_intel_display(intel_dp);

	if (drm_WARN_ON(display->drm,
			index < 0 || index >= intel_dp->num_common_rates))
		return 162000;

	return intel_dp->common_rates[index];
}

/* Theoretical max between source and sink */
int intel_dp_max_common_rate(struct intel_dp *intel_dp)
{
	return intel_dp_common_rate(intel_dp, intel_dp->num_common_rates - 1);
}

int intel_dp_max_source_lane_count(struct intel_digital_port *dig_port)
{
	int vbt_max_lanes = intel_bios_dp_max_lane_count(dig_port->base.devdata);
	int max_lanes = dig_port->max_lanes;

	if (vbt_max_lanes)
		max_lanes = min(max_lanes, vbt_max_lanes);

	return max_lanes;
}

/* Theoretical max between source and sink */
int intel_dp_max_common_lane_count(struct intel_dp *intel_dp)
{
	struct intel_digital_port *dig_port = dp_to_dig_port(intel_dp);
	int source_max = intel_dp_max_source_lane_count(dig_port);
	int sink_max = intel_dp->max_sink_lane_count;
	int lane_max = intel_tc_port_max_lane_count(dig_port);
	int lttpr_max = drm_dp_lttpr_max_lane_count(intel_dp->lttpr_common_caps);

	if (lttpr_max)
		sink_max = min(sink_max, lttpr_max);

	return min3(source_max, sink_max, lane_max);
}

static int forced_lane_count(struct intel_dp *intel_dp)
{
	return clamp(intel_dp->link.force_lane_count, 1, intel_dp_max_common_lane_count(intel_dp));
}

int intel_dp_max_lane_count(struct intel_dp *intel_dp)
{
	int lane_count;

	if (intel_dp->link.force_lane_count)
		lane_count = forced_lane_count(intel_dp);
	else
		lane_count = intel_dp->link.max_lane_count;

	switch (lane_count) {
	case 1:
	case 2:
	case 4:
		return lane_count;
	default:
		MISSING_CASE(lane_count);
		return 1;
	}
}

static int intel_dp_min_lane_count(struct intel_dp *intel_dp)
{
	if (intel_dp->link.force_lane_count)
		return forced_lane_count(intel_dp);

	return 1;
}

/*
 * The required data bandwidth for a mode with given pixel clock and bpp. This
 * is the required net bandwidth independent of the data bandwidth efficiency.
 *
 * TODO: check if callers of this functions should use
 * intel_dp_effective_data_rate() instead.
 */
int
intel_dp_link_required(int pixel_clock, int bpp)
{
	/* pixel_clock is in kHz, divide bpp by 8 for bit to Byte conversion */
	return DIV_ROUND_UP(pixel_clock * bpp, 8);
}

/**
 * intel_dp_effective_data_rate - Return the pixel data rate accounting for BW allocation overhead
 * @pixel_clock: pixel clock in kHz
 * @bpp_x16: bits per pixel .4 fixed point format
 * @bw_overhead: BW allocation overhead in 1ppm units
 *
 * Return the effective pixel data rate in kB/sec units taking into account
 * the provided SSC, FEC, DSC BW allocation overhead.
 */
int intel_dp_effective_data_rate(int pixel_clock, int bpp_x16,
				 int bw_overhead)
{
	return DIV_ROUND_UP_ULL(mul_u32_u32(pixel_clock * bpp_x16, bw_overhead),
				1000000 * 16 * 8);
}

/**
 * intel_dp_max_link_data_rate: Calculate the maximum rate for the given link params
 * @intel_dp: Intel DP object
 * @max_dprx_rate: Maximum data rate of the DPRX
 * @max_dprx_lanes: Maximum lane count of the DPRX
 *
 * Calculate the maximum data rate for the provided link parameters taking into
 * account any BW limitations by a DP tunnel attached to @intel_dp.
 *
 * Returns the maximum data rate in kBps units.
 */
int intel_dp_max_link_data_rate(struct intel_dp *intel_dp,
				int max_dprx_rate, int max_dprx_lanes)
{
	int max_rate = drm_dp_max_dprx_data_rate(max_dprx_rate, max_dprx_lanes);

	if (intel_dp_tunnel_bw_alloc_is_enabled(intel_dp))
		max_rate = min(max_rate,
			       drm_dp_tunnel_available_bw(intel_dp->tunnel));

	return max_rate;
}

bool intel_dp_has_joiner(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_digital_port *intel_dig_port = dp_to_dig_port(intel_dp);
	struct intel_encoder *encoder = &intel_dig_port->base;

	/* eDP MSO is not compatible with joiner */
	if (intel_dp->mso_link_count)
		return false;

	return DISPLAY_VER(display) >= 12 ||
		(DISPLAY_VER(display) == 11 &&
		 encoder->port != PORT_A);
}

static int dg2_max_source_rate(struct intel_dp *intel_dp)
{
	return intel_dp_is_edp(intel_dp) ? 810000 : 1350000;
}

static int icl_max_source_rate(struct intel_dp *intel_dp)
{
	struct intel_encoder *encoder = &dp_to_dig_port(intel_dp)->base;

	if (intel_encoder_is_combo(encoder) && !intel_dp_is_edp(intel_dp))
		return 540000;

	return 810000;
}

static int ehl_max_source_rate(struct intel_dp *intel_dp)
{
	if (intel_dp_is_edp(intel_dp))
		return 540000;

	return 810000;
}

static int mtl_max_source_rate(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_encoder *encoder = &dp_to_dig_port(intel_dp)->base;

	if (intel_encoder_is_c10phy(encoder))
		return 810000;

	if (DISPLAY_VERx100(display) == 1401)
		return 1350000;

	return 2000000;
}

static int vbt_max_link_rate(struct intel_dp *intel_dp)
{
	struct intel_encoder *encoder = &dp_to_dig_port(intel_dp)->base;
	int max_rate;

	max_rate = intel_bios_dp_max_link_rate(encoder->devdata);

	if (intel_dp_is_edp(intel_dp)) {
		struct intel_connector *connector = intel_dp->attached_connector;
		int edp_max_rate = connector->panel.vbt.edp.max_link_rate;

		if (max_rate && edp_max_rate)
			max_rate = min(max_rate, edp_max_rate);
		else if (edp_max_rate)
			max_rate = edp_max_rate;
	}

	return max_rate;
}

static void
intel_dp_set_source_rates(struct intel_dp *intel_dp)
{
	/* The values must be in increasing order */
	static const int bmg_rates[] = {
		162000, 216000, 243000, 270000, 324000, 432000, 540000, 675000,
		810000,	1000000, 1350000,
	};
	static const int mtl_rates[] = {
		162000, 216000, 243000, 270000, 324000, 432000, 540000, 675000,
		810000,	1000000, 2000000,
	};
	static const int icl_rates[] = {
		162000, 216000, 270000, 324000, 432000, 540000, 648000, 810000,
		1000000, 1350000,
	};
	static const int bxt_rates[] = {
		162000, 216000, 243000, 270000, 324000, 432000, 540000
	};
	static const int skl_rates[] = {
		162000, 216000, 270000, 324000, 432000, 540000
	};
	static const int hsw_rates[] = {
		162000, 270000, 540000
	};
	static const int g4x_rates[] = {
		162000, 270000
	};
	struct intel_display *display = to_intel_display(intel_dp);
	const int *source_rates;
	int size, max_rate = 0, vbt_max_rate;

	/* This should only be done once */
	drm_WARN_ON(display->drm,
		    intel_dp->source_rates || intel_dp->num_source_rates);

	if (DISPLAY_VER(display) >= 14) {
		if (display->platform.battlemage) {
			source_rates = bmg_rates;
			size = ARRAY_SIZE(bmg_rates);
		} else {
			source_rates = mtl_rates;
			size = ARRAY_SIZE(mtl_rates);
		}
		max_rate = mtl_max_source_rate(intel_dp);
	} else if (DISPLAY_VER(display) >= 11) {
		source_rates = icl_rates;
		size = ARRAY_SIZE(icl_rates);
		if (display->platform.dg2)
			max_rate = dg2_max_source_rate(intel_dp);
		else if (display->platform.alderlake_p || display->platform.alderlake_s ||
			 display->platform.dg1 || display->platform.rocketlake)
			max_rate = 810000;
		else if (display->platform.jasperlake || display->platform.elkhartlake)
			max_rate = ehl_max_source_rate(intel_dp);
		else
			max_rate = icl_max_source_rate(intel_dp);
	} else if (display->platform.geminilake || display->platform.broxton) {
		source_rates = bxt_rates;
		size = ARRAY_SIZE(bxt_rates);
	} else if (DISPLAY_VER(display) == 9) {
		source_rates = skl_rates;
		size = ARRAY_SIZE(skl_rates);
	} else if ((display->platform.haswell && !display->platform.haswell_ulx) ||
		   display->platform.broadwell) {
		source_rates = hsw_rates;
		size = ARRAY_SIZE(hsw_rates);
	} else {
		source_rates = g4x_rates;
		size = ARRAY_SIZE(g4x_rates);
	}

	vbt_max_rate = vbt_max_link_rate(intel_dp);
	if (max_rate && vbt_max_rate)
		max_rate = min(max_rate, vbt_max_rate);
	else if (vbt_max_rate)
		max_rate = vbt_max_rate;

	if (max_rate)
		size = intel_dp_rate_limit_len(source_rates, size, max_rate);

	intel_dp->source_rates = source_rates;
	intel_dp->num_source_rates = size;
}

static int intersect_rates(const int *source_rates, int source_len,
			   const int *sink_rates, int sink_len,
			   int *common_rates)
{
	int i = 0, j = 0, k = 0;

	while (i < source_len && j < sink_len) {
		if (source_rates[i] == sink_rates[j]) {
			if (WARN_ON(k >= DP_MAX_SUPPORTED_RATES))
				return k;
			common_rates[k] = source_rates[i];
			++k;
			++i;
			++j;
		} else if (source_rates[i] < sink_rates[j]) {
			++i;
		} else {
			++j;
		}
	}
	return k;
}

/* return index of rate in rates array, or -1 if not found */
int intel_dp_rate_index(const int *rates, int len, int rate)
{
	int i;

	for (i = 0; i < len; i++)
		if (rate == rates[i])
			return i;

	return -1;
}

static int intel_dp_link_config_rate(struct intel_dp *intel_dp,
				     const struct intel_dp_link_config *lc)
{
	return intel_dp_common_rate(intel_dp, lc->link_rate_idx);
}

static int intel_dp_link_config_lane_count(const struct intel_dp_link_config *lc)
{
	return 1 << lc->lane_count_exp;
}

static int intel_dp_link_config_bw(struct intel_dp *intel_dp,
				   const struct intel_dp_link_config *lc)
{
	return drm_dp_max_dprx_data_rate(intel_dp_link_config_rate(intel_dp, lc),
					 intel_dp_link_config_lane_count(lc));
}

static int link_config_cmp_by_bw(const void *a, const void *b, const void *p)
{
	struct intel_dp *intel_dp = (struct intel_dp *)p;	/* remove const */
	const struct intel_dp_link_config *lc_a = a;
	const struct intel_dp_link_config *lc_b = b;
	int bw_a = intel_dp_link_config_bw(intel_dp, lc_a);
	int bw_b = intel_dp_link_config_bw(intel_dp, lc_b);

	if (bw_a != bw_b)
		return bw_a - bw_b;

	return intel_dp_link_config_rate(intel_dp, lc_a) -
	       intel_dp_link_config_rate(intel_dp, lc_b);
}

static void intel_dp_link_config_init(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_dp_link_config *lc;
	int num_common_lane_configs;
	int i;
	int j;

	if (drm_WARN_ON(display->drm, !is_power_of_2(intel_dp_max_common_lane_count(intel_dp))))
		return;

	num_common_lane_configs = ilog2(intel_dp_max_common_lane_count(intel_dp)) + 1;

	if (drm_WARN_ON(display->drm, intel_dp->num_common_rates * num_common_lane_configs >
				    ARRAY_SIZE(intel_dp->link.configs)))
		return;

	intel_dp->link.num_configs = intel_dp->num_common_rates * num_common_lane_configs;

	lc = &intel_dp->link.configs[0];
	for (i = 0; i < intel_dp->num_common_rates; i++) {
		for (j = 0; j < num_common_lane_configs; j++) {
			lc->lane_count_exp = j;
			lc->link_rate_idx = i;

			lc++;
		}
	}

	sort_r(intel_dp->link.configs, intel_dp->link.num_configs,
	       sizeof(intel_dp->link.configs[0]),
	       link_config_cmp_by_bw, NULL,
	       intel_dp);
}

void intel_dp_link_config_get(struct intel_dp *intel_dp, int idx, int *link_rate, int *lane_count)
{
	struct intel_display *display = to_intel_display(intel_dp);
	const struct intel_dp_link_config *lc;

	if (drm_WARN_ON(display->drm, idx < 0 || idx >= intel_dp->link.num_configs))
		idx = 0;

	lc = &intel_dp->link.configs[idx];

	*link_rate = intel_dp_link_config_rate(intel_dp, lc);
	*lane_count = intel_dp_link_config_lane_count(lc);
}

int intel_dp_link_config_index(struct intel_dp *intel_dp, int link_rate, int lane_count)
{
	int link_rate_idx = intel_dp_rate_index(intel_dp->common_rates, intel_dp->num_common_rates,
						link_rate);
	int lane_count_exp = ilog2(lane_count);
	int i;

	for (i = 0; i < intel_dp->link.num_configs; i++) {
		const struct intel_dp_link_config *lc = &intel_dp->link.configs[i];

		if (lc->lane_count_exp == lane_count_exp &&
		    lc->link_rate_idx == link_rate_idx)
			return i;
	}

	return -1;
}

static void intel_dp_set_common_rates(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);

	drm_WARN_ON(display->drm,
		    !intel_dp->num_source_rates || !intel_dp->num_sink_rates);

	intel_dp->num_common_rates = intersect_rates(intel_dp->source_rates,
						     intel_dp->num_source_rates,
						     intel_dp->sink_rates,
						     intel_dp->num_sink_rates,
						     intel_dp->common_rates);

	/* Paranoia, there should always be something in common. */
	if (drm_WARN_ON(display->drm, intel_dp->num_common_rates == 0)) {
		intel_dp->common_rates[0] = 162000;
		intel_dp->num_common_rates = 1;
	}

	intel_dp_link_config_init(intel_dp);
}

bool intel_dp_link_params_valid(struct intel_dp *intel_dp, int link_rate,
				u8 lane_count)
{
	/*
	 * FIXME: we need to synchronize the current link parameters with
	 * hardware readout. Currently fast link training doesn't work on
	 * boot-up.
	 */
	if (link_rate == 0 ||
	    link_rate > intel_dp->link.max_rate)
		return false;

	if (lane_count == 0 ||
	    lane_count > intel_dp_max_lane_count(intel_dp))
		return false;

	return true;
}

u32 intel_dp_mode_to_fec_clock(u32 mode_clock)
{
	return div_u64(mul_u32_u32(mode_clock, DP_DSC_FEC_OVERHEAD_FACTOR),
		       1000000U);
}

int intel_dp_bw_fec_overhead(bool fec_enabled)
{
	/*
	 * TODO: Calculate the actual overhead for a given mode.
	 * The hard-coded 1/0.972261=2.853% overhead factor
	 * corresponds (for instance) to the 8b/10b DP FEC 2.4% +
	 * 0.453% DSC overhead. This is enough for a 3840 width mode,
	 * which has a DSC overhead of up to ~0.2%, but may not be
	 * enough for a 1024 width mode where this is ~0.8% (on a 4
	 * lane DP link, with 2 DSC slices and 8 bpp color depth).
	 */
	return fec_enabled ? DP_DSC_FEC_OVERHEAD_FACTOR : 1000000;
}

static int
small_joiner_ram_size_bits(struct intel_display *display)
{
	if (DISPLAY_VER(display) >= 13)
		return 17280 * 8;
	else if (DISPLAY_VER(display) >= 11)
		return 7680 * 8;
	else
		return 6144 * 8;
}

static u32 intel_dp_dsc_nearest_valid_bpp(struct intel_display *display, u32 bpp, u32 pipe_bpp)
{
	u32 bits_per_pixel = bpp;
	int i;

	/* Error out if the max bpp is less than smallest allowed valid bpp */
	if (bits_per_pixel < valid_dsc_bpp[0]) {
		drm_dbg_kms(display->drm, "Unsupported BPP %u, min %u\n",
			    bits_per_pixel, valid_dsc_bpp[0]);
		return 0;
	}

	/* From XE_LPD onwards we support from bpc upto uncompressed bpp-1 BPPs */
	if (DISPLAY_VER(display) >= 13) {
		bits_per_pixel = min(bits_per_pixel, pipe_bpp - 1);

		/*
		 * According to BSpec, 27 is the max DSC output bpp,
		 * 8 is the min DSC output bpp.
		 * While we can still clamp higher bpp values to 27, saving bandwidth,
		 * if it is required to oompress up to bpp < 8, means we can't do
		 * that and probably means we can't fit the required mode, even with
		 * DSC enabled.
		 */
		if (bits_per_pixel < 8) {
			drm_dbg_kms(display->drm,
				    "Unsupported BPP %u, min 8\n",
				    bits_per_pixel);
			return 0;
		}
		bits_per_pixel = min_t(u32, bits_per_pixel, 27);
	} else {
		/* Find the nearest match in the array of known BPPs from VESA */
		for (i = 0; i < ARRAY_SIZE(valid_dsc_bpp) - 1; i++) {
			if (bits_per_pixel < valid_dsc_bpp[i + 1])
				break;
		}
		drm_dbg_kms(display->drm, "Set dsc bpp from %d to VESA %d\n",
			    bits_per_pixel, valid_dsc_bpp[i]);

		bits_per_pixel = valid_dsc_bpp[i];
	}

	return bits_per_pixel;
}

static int bigjoiner_interface_bits(struct intel_display *display)
{
	return DISPLAY_VER(display) >= 14 ? 36 : 24;
}

static u32 bigjoiner_bw_max_bpp(struct intel_display *display, u32 mode_clock,
				int num_joined_pipes)
{
	u32 max_bpp;
	/* With bigjoiner multiple dsc engines are used in parallel so PPC is 2 */
	int ppc = 2;
	int num_big_joiners = num_joined_pipes / 2;

	max_bpp = display->cdclk.max_cdclk_freq * ppc * bigjoiner_interface_bits(display) /
		  intel_dp_mode_to_fec_clock(mode_clock);

	max_bpp *= num_big_joiners;

	return max_bpp;

}

static u32 small_joiner_ram_max_bpp(struct intel_display *display,
				    u32 mode_hdisplay,
				    int num_joined_pipes)
{
	u32 max_bpp;

	/* Small Joiner Check: output bpp <= joiner RAM (bits) / Horiz. width */
	max_bpp = small_joiner_ram_size_bits(display) / mode_hdisplay;

	max_bpp *= num_joined_pipes;

	return max_bpp;
}

static int ultrajoiner_ram_bits(void)
{
	return 4 * 72 * 512;
}

static u32 ultrajoiner_ram_max_bpp(u32 mode_hdisplay)
{
	return ultrajoiner_ram_bits() / mode_hdisplay;
}

/* TODO: return a bpp_x16 value */
static
u32 get_max_compressed_bpp_with_joiner(struct intel_display *display,
				       u32 mode_clock, u32 mode_hdisplay,
				       int num_joined_pipes)
{
	u32 max_bpp = small_joiner_ram_max_bpp(display, mode_hdisplay, num_joined_pipes);

	if (num_joined_pipes > 1)
		max_bpp = min(max_bpp, bigjoiner_bw_max_bpp(display, mode_clock,
							    num_joined_pipes));
	if (num_joined_pipes == 4)
		max_bpp = min(max_bpp, ultrajoiner_ram_max_bpp(mode_hdisplay));

	return max_bpp;
}

/* TODO: return a bpp_x16 value */
u16 intel_dp_dsc_get_max_compressed_bpp(struct intel_display *display,
					u32 link_clock, u32 lane_count,
					u32 mode_clock, u32 mode_hdisplay,
					int num_joined_pipes,
					enum intel_output_format output_format,
					u32 pipe_bpp,
					u32 timeslots)
{
	u32 bits_per_pixel, joiner_max_bpp;

	/*
	 * Available Link Bandwidth(Kbits/sec) = (NumberOfLanes)*
	 * (LinkSymbolClock)* 8 * (TimeSlots / 64)
	 * for SST -> TimeSlots is 64(i.e all TimeSlots that are available)
	 * for MST -> TimeSlots has to be calculated, based on mode requirements
	 *
	 * Due to FEC overhead, the available bw is reduced to 97.2261%.
	 * To support the given mode:
	 * Bandwidth required should be <= Available link Bandwidth * FEC Overhead
	 * =>ModeClock * bits_per_pixel <= Available Link Bandwidth * FEC Overhead
	 * =>bits_per_pixel <= Available link Bandwidth * FEC Overhead / ModeClock
	 * =>bits_per_pixel <= (NumberOfLanes * LinkSymbolClock) * 8 (TimeSlots / 64) /
	 *		       (ModeClock / FEC Overhead)
	 * =>bits_per_pixel <= (NumberOfLanes * LinkSymbolClock * TimeSlots) /
	 *		       (ModeClock / FEC Overhead * 8)
	 */
	bits_per_pixel = ((link_clock * lane_count) * timeslots) /
			 (intel_dp_mode_to_fec_clock(mode_clock) * 8);

	/* Bandwidth required for 420 is half, that of 444 format */
	if (output_format == INTEL_OUTPUT_FORMAT_YCBCR420)
		bits_per_pixel *= 2;

	/*
	 * According to DSC 1.2a Section 4.1.1 Table 4.1 the maximum
	 * supported PPS value can be 63.9375 and with the further
	 * mention that for 420, 422 formats, bpp should be programmed double
	 * the target bpp restricting our target bpp to be 31.9375 at max.
	 */
	if (output_format == INTEL_OUTPUT_FORMAT_YCBCR420)
		bits_per_pixel = min_t(u32, bits_per_pixel, 31);

	drm_dbg_kms(display->drm, "Max link bpp is %u for %u timeslots "
				"total bw %u pixel clock %u\n",
				bits_per_pixel, timeslots,
				(link_clock * lane_count * 8),
				intel_dp_mode_to_fec_clock(mode_clock));

	joiner_max_bpp = get_max_compressed_bpp_with_joiner(display, mode_clock,
							    mode_hdisplay, num_joined_pipes);
	bits_per_pixel = min(bits_per_pixel, joiner_max_bpp);

	bits_per_pixel = intel_dp_dsc_nearest_valid_bpp(display, bits_per_pixel, pipe_bpp);

	return bits_per_pixel;
}

u8 intel_dp_dsc_get_slice_count(const struct intel_connector *connector,
				int mode_clock, int mode_hdisplay,
				int num_joined_pipes)
{
	struct intel_display *display = to_intel_display(connector);
	u8 min_slice_count, i;
	int max_slice_width;

	if (mode_clock <= DP_DSC_PEAK_PIXEL_RATE)
		min_slice_count = DIV_ROUND_UP(mode_clock,
					       DP_DSC_MAX_ENC_THROUGHPUT_0);
	else
		min_slice_count = DIV_ROUND_UP(mode_clock,
					       DP_DSC_MAX_ENC_THROUGHPUT_1);

	/*
	 * Due to some DSC engine BW limitations, we need to enable second
	 * slice and VDSC engine, whenever we approach close enough to max CDCLK
	 */
	if (mode_clock >= ((display->cdclk.max_cdclk_freq * 85) / 100))
		min_slice_count = max_t(u8, min_slice_count, 2);

	max_slice_width = drm_dp_dsc_sink_max_slice_width(connector->dp.dsc_dpcd);
	if (max_slice_width < DP_DSC_MIN_SLICE_WIDTH_VALUE) {
		drm_dbg_kms(display->drm,
			    "Unsupported slice width %d by DP DSC Sink device\n",
			    max_slice_width);
		return 0;
	}
	/* Also take into account max slice width */
	min_slice_count = max_t(u8, min_slice_count,
				DIV_ROUND_UP(mode_hdisplay,
					     max_slice_width));

	/* Find the closest match to the valid slice count values */
	for (i = 0; i < ARRAY_SIZE(valid_dsc_slicecount); i++) {
		u8 test_slice_count = valid_dsc_slicecount[i] * num_joined_pipes;

		/*
		 * 3 DSC Slices per pipe need 3 DSC engines, which is supported only
		 * with Ultrajoiner only for some platforms.
		 */
		if (valid_dsc_slicecount[i] == 3 &&
		    (!HAS_DSC_3ENGINES(display) || num_joined_pipes != 4))
			continue;

		if (test_slice_count >
		    drm_dp_dsc_sink_max_slice_count(connector->dp.dsc_dpcd, false))
			break;

		 /*
		  * Bigjoiner needs small joiner to be enabled.
		  * So there should be at least 2 dsc slices per pipe,
		  * whenever bigjoiner is enabled.
		  */
		if (num_joined_pipes > 1 && valid_dsc_slicecount[i] < 2)
			continue;

		if (mode_hdisplay % test_slice_count)
			continue;

		if (min_slice_count <= test_slice_count)
			return test_slice_count;
	}

	drm_dbg_kms(display->drm, "Unsupported Slice Count %d\n",
		    min_slice_count);
	return 0;
}

static bool source_can_output(struct intel_dp *intel_dp,
			      enum intel_output_format format)
{
	struct intel_display *display = to_intel_display(intel_dp);

	switch (format) {
	case INTEL_OUTPUT_FORMAT_RGB:
		return true;

	case INTEL_OUTPUT_FORMAT_YCBCR444:
		/*
		 * No YCbCr output support on gmch platforms.
		 * Also, ILK doesn't seem capable of DP YCbCr output.
		 * The displayed image is severely corrupted. SNB+ is fine.
		 */
		return !HAS_GMCH(display) && !display->platform.ironlake;

	case INTEL_OUTPUT_FORMAT_YCBCR420:
		/* Platform < Gen 11 cannot output YCbCr420 format */
		return DISPLAY_VER(display) >= 11;

	default:
		MISSING_CASE(format);
		return false;
	}
}

static bool
dfp_can_convert_from_rgb(struct intel_dp *intel_dp,
			 enum intel_output_format sink_format)
{
	if (!drm_dp_is_branch(intel_dp->dpcd))
		return false;

	if (sink_format == INTEL_OUTPUT_FORMAT_YCBCR444)
		return intel_dp->dfp.rgb_to_ycbcr;

	if (sink_format == INTEL_OUTPUT_FORMAT_YCBCR420)
		return intel_dp->dfp.rgb_to_ycbcr &&
			intel_dp->dfp.ycbcr_444_to_420;

	return false;
}

static bool
dfp_can_convert_from_ycbcr444(struct intel_dp *intel_dp,
			      enum intel_output_format sink_format)
{
	if (!drm_dp_is_branch(intel_dp->dpcd))
		return false;

	if (sink_format == INTEL_OUTPUT_FORMAT_YCBCR420)
		return intel_dp->dfp.ycbcr_444_to_420;

	return false;
}

static bool
dfp_can_convert(struct intel_dp *intel_dp,
		enum intel_output_format output_format,
		enum intel_output_format sink_format)
{
	switch (output_format) {
	case INTEL_OUTPUT_FORMAT_RGB:
		return dfp_can_convert_from_rgb(intel_dp, sink_format);
	case INTEL_OUTPUT_FORMAT_YCBCR444:
		return dfp_can_convert_from_ycbcr444(intel_dp, sink_format);
	default:
		MISSING_CASE(output_format);
		return false;
	}

	return false;
}

static enum intel_output_format
intel_dp_output_format(struct intel_connector *connector,
		       enum intel_output_format sink_format)
{
	struct intel_display *display = to_intel_display(connector);
	struct intel_dp *intel_dp = intel_attached_dp(connector);
	enum intel_output_format force_dsc_output_format =
		intel_dp->force_dsc_output_format;
	enum intel_output_format output_format;
	if (force_dsc_output_format) {
		if (source_can_output(intel_dp, force_dsc_output_format) &&
		    (!drm_dp_is_branch(intel_dp->dpcd) ||
		     sink_format != force_dsc_output_format ||
		     dfp_can_convert(intel_dp, force_dsc_output_format, sink_format)))
			return force_dsc_output_format;

		drm_dbg_kms(display->drm, "Cannot force DSC output format\n");
	}

	if (sink_format == INTEL_OUTPUT_FORMAT_RGB ||
	    dfp_can_convert_from_rgb(intel_dp, sink_format))
		output_format = INTEL_OUTPUT_FORMAT_RGB;

	else if (sink_format == INTEL_OUTPUT_FORMAT_YCBCR444 ||
		 dfp_can_convert_from_ycbcr444(intel_dp, sink_format))
		output_format = INTEL_OUTPUT_FORMAT_YCBCR444;

	else
		output_format = INTEL_OUTPUT_FORMAT_YCBCR420;

	drm_WARN_ON(display->drm, !source_can_output(intel_dp, output_format));

	return output_format;
}

int intel_dp_min_bpp(enum intel_output_format output_format)
{
	if (output_format == INTEL_OUTPUT_FORMAT_RGB)
		return intel_display_min_pipe_bpp();
	else
		return 8 * 3;
}

int intel_dp_output_bpp(enum intel_output_format output_format, int bpp)
{
	/*
	 * bpp value was assumed to RGB format. And YCbCr 4:2:0 output
	 * format of the number of bytes per pixel will be half the number
	 * of bytes of RGB pixel.
	 */
	if (output_format == INTEL_OUTPUT_FORMAT_YCBCR420)
		bpp /= 2;

	return bpp;
}

static enum intel_output_format
intel_dp_sink_format(struct intel_connector *connector,
		     const struct drm_display_mode *mode)
{
	const struct drm_display_info *info = &connector->base.display_info;

	if (drm_mode_is_420_only(info, mode))
		return INTEL_OUTPUT_FORMAT_YCBCR420;

	return INTEL_OUTPUT_FORMAT_RGB;
}

static int
intel_dp_mode_min_output_bpp(struct intel_connector *connector,
			     const struct drm_display_mode *mode)
{
	enum intel_output_format output_format, sink_format;

	sink_format = intel_dp_sink_format(connector, mode);

	output_format = intel_dp_output_format(connector, sink_format);

	return intel_dp_output_bpp(output_format, intel_dp_min_bpp(output_format));
}

static bool intel_dp_hdisplay_bad(struct intel_display *display,
				  int hdisplay)
{
	/*
	 * Older platforms don't like hdisplay==4096 with DP.
	 *
	 * On ILK/SNB/IVB the pipe seems to be somewhat running (scanline
	 * and frame counter increment), but we don't get vblank interrupts,
	 * and the pipe underruns immediately. The link also doesn't seem
	 * to get trained properly.
	 *
	 * On CHV the vblank interrupts don't seem to disappear but
	 * otherwise the symptoms are similar.
	 *
	 * TODO: confirm the behaviour on HSW+
	 */
	return hdisplay == 4096 && !HAS_DDI(display);
}

static int intel_dp_max_tmds_clock(struct intel_dp *intel_dp)
{
	struct intel_connector *connector = intel_dp->attached_connector;
	const struct drm_display_info *info = &connector->base.display_info;
	int max_tmds_clock = intel_dp->dfp.max_tmds_clock;

	/* Only consider the sink's max TMDS clock if we know this is a HDMI DFP */
	if (max_tmds_clock && info->max_tmds_clock)
		max_tmds_clock = min(max_tmds_clock, info->max_tmds_clock);

	return max_tmds_clock;
}

static enum drm_mode_status
intel_dp_tmds_clock_valid(struct intel_dp *intel_dp,
			  int clock, int bpc,
			  enum intel_output_format sink_format,
			  bool respect_downstream_limits)
{
	int tmds_clock, min_tmds_clock, max_tmds_clock;

	if (!respect_downstream_limits)
		return MODE_OK;

	tmds_clock = intel_hdmi_tmds_clock(clock, bpc, sink_format);

	min_tmds_clock = intel_dp->dfp.min_tmds_clock;
	max_tmds_clock = intel_dp_max_tmds_clock(intel_dp);

	if (min_tmds_clock && tmds_clock < min_tmds_clock)
		return MODE_CLOCK_LOW;

	if (max_tmds_clock && tmds_clock > max_tmds_clock)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static enum drm_mode_status
intel_dp_mode_valid_downstream(struct intel_connector *connector,
			       const struct drm_display_mode *mode,
			       int target_clock)
{
	struct intel_dp *intel_dp = intel_attached_dp(connector);
	const struct drm_display_info *info = &connector->base.display_info;
	enum drm_mode_status status;
	enum intel_output_format sink_format;

	/* If PCON supports FRL MODE, check FRL bandwidth constraints */
	if (intel_dp->dfp.pcon_max_frl_bw) {
		int target_bw;
		int max_frl_bw;
		int bpp = intel_dp_mode_min_output_bpp(connector, mode);

		target_bw = bpp * target_clock;

		max_frl_bw = intel_dp->dfp.pcon_max_frl_bw;

		/* converting bw from Gbps to Kbps*/
		max_frl_bw = max_frl_bw * 1000000;

		if (target_bw > max_frl_bw)
			return MODE_CLOCK_HIGH;

		return MODE_OK;
	}

	if (intel_dp->dfp.max_dotclock &&
	    target_clock > intel_dp->dfp.max_dotclock)
		return MODE_CLOCK_HIGH;

	sink_format = intel_dp_sink_format(connector, mode);

	/* Assume 8bpc for the DP++/HDMI/DVI TMDS clock check */
	status = intel_dp_tmds_clock_valid(intel_dp, target_clock,
					   8, sink_format, true);

	if (status != MODE_OK) {
		if (sink_format == INTEL_OUTPUT_FORMAT_YCBCR420 ||
		    !connector->base.ycbcr_420_allowed ||
		    !drm_mode_is_420_also(info, mode))
			return status;
		sink_format = INTEL_OUTPUT_FORMAT_YCBCR420;
		status = intel_dp_tmds_clock_valid(intel_dp, target_clock,
						   8, sink_format, true);
		if (status != MODE_OK)
			return status;
	}

	return MODE_OK;
}

static
bool intel_dp_needs_joiner(struct intel_dp *intel_dp,
			   struct intel_connector *connector,
			   int hdisplay, int clock,
			   int num_joined_pipes)
{
	struct intel_display *display = to_intel_display(intel_dp);
	int hdisplay_limit;

	if (!intel_dp_has_joiner(intel_dp))
		return false;

	num_joined_pipes /= 2;

	hdisplay_limit = DISPLAY_VER(display) >= 30 ? 6144 : 5120;

	return clock > num_joined_pipes * display->cdclk.max_dotclk_freq ||
	       hdisplay > num_joined_pipes * hdisplay_limit;
}

int intel_dp_num_joined_pipes(struct intel_dp *intel_dp,
			      struct intel_connector *connector,
			      int hdisplay, int clock)
{
	struct intel_display *display = to_intel_display(intel_dp);

	if (connector->force_joined_pipes)
		return connector->force_joined_pipes;

	if (HAS_ULTRAJOINER(display) &&
	    intel_dp_needs_joiner(intel_dp, connector, hdisplay, clock, 4))
		return 4;

	if ((HAS_BIGJOINER(display) || HAS_UNCOMPRESSED_JOINER(display)) &&
	    intel_dp_needs_joiner(intel_dp, connector, hdisplay, clock, 2))
		return 2;

	return 1;
}

bool intel_dp_has_dsc(const struct intel_connector *connector)
{
	struct intel_display *display = to_intel_display(connector);

	if (!HAS_DSC(display))
		return false;

	if (connector->mst.dp && !HAS_DSC_MST(display))
		return false;

	if (connector->base.connector_type == DRM_MODE_CONNECTOR_eDP &&
	    connector->panel.vbt.edp.dsc_disable)
		return false;

	if (!drm_dp_sink_supports_dsc(connector->dp.dsc_dpcd))
		return false;

	return true;
}

static enum drm_mode_status
intel_dp_mode_valid(struct drm_connector *_connector,
		    const struct drm_display_mode *mode)
{
	struct intel_display *display = to_intel_display(_connector->dev);
	struct intel_connector *connector = to_intel_connector(_connector);
	struct intel_dp *intel_dp = intel_attached_dp(connector);
	const struct drm_display_mode *fixed_mode;
	int target_clock = mode->clock;
	int max_rate, mode_rate, max_lanes, max_link_clock;
	int max_dotclk = display->cdclk.max_dotclk_freq;
	u16 dsc_max_compressed_bpp = 0;
	u8 dsc_slice_count = 0;
	enum drm_mode_status status;
	bool dsc = false;
	int num_joined_pipes;

	status = intel_cpu_transcoder_mode_valid(display, mode);
	if (status != MODE_OK)
		return status;

	if (mode->flags & DRM_MODE_FLAG_DBLCLK)
		return MODE_H_ILLEGAL;

	if (mode->clock < 10000)
		return MODE_CLOCK_LOW;

	fixed_mode = intel_panel_fixed_mode(connector, mode);
	if (intel_dp_is_edp(intel_dp) && fixed_mode) {
		status = intel_panel_mode_valid(connector, mode);
		if (status != MODE_OK)
			return status;

		target_clock = fixed_mode->clock;
	}

	num_joined_pipes = intel_dp_num_joined_pipes(intel_dp, connector,
						     mode->hdisplay, target_clock);
	max_dotclk *= num_joined_pipes;

	if (target_clock > max_dotclk)
		return MODE_CLOCK_HIGH;

	if (intel_dp_hdisplay_bad(display, mode->hdisplay))
		return MODE_H_ILLEGAL;

	max_link_clock = intel_dp_max_link_rate(intel_dp);
	max_lanes = intel_dp_max_lane_count(intel_dp);

	max_rate = intel_dp_max_link_data_rate(intel_dp, max_link_clock, max_lanes);

	mode_rate = intel_dp_link_required(target_clock,
					   intel_dp_mode_min_output_bpp(connector, mode));

	if (intel_dp_has_dsc(connector)) {
		enum intel_output_format sink_format, output_format;
		int pipe_bpp;

		sink_format = intel_dp_sink_format(connector, mode);
		output_format = intel_dp_output_format(connector, sink_format);
		/*
		 * TBD pass the connector BPC,
		 * for now U8_MAX so that max BPC on that platform would be picked
		 */
		pipe_bpp = intel_dp_dsc_compute_max_bpp(connector, U8_MAX);

		/*
		 * Output bpp is stored in 6.4 format so right shift by 4 to get the
		 * integer value since we support only integer values of bpp.
		 */
		if (intel_dp_is_edp(intel_dp)) {
			dsc_max_compressed_bpp =
				drm_edp_dsc_sink_output_bpp(connector->dp.dsc_dpcd) >> 4;
			dsc_slice_count =
				drm_dp_dsc_sink_max_slice_count(connector->dp.dsc_dpcd,
								true);
		} else if (drm_dp_sink_supports_fec(connector->dp.fec_capability)) {
			dsc_max_compressed_bpp =
				intel_dp_dsc_get_max_compressed_bpp(display,
								    max_link_clock,
								    max_lanes,
								    target_clock,
								    mode->hdisplay,
								    num_joined_pipes,
								    output_format,
								    pipe_bpp, 64);
			dsc_slice_count =
				intel_dp_dsc_get_slice_count(connector,
							     target_clock,
							     mode->hdisplay,
							     num_joined_pipes);
		}

		dsc = dsc_max_compressed_bpp && dsc_slice_count;
	}

	if (intel_dp_joiner_needs_dsc(display, num_joined_pipes) && !dsc)
		return MODE_CLOCK_HIGH;

	if (mode_rate > max_rate && !dsc)
		return MODE_CLOCK_HIGH;

	status = intel_dp_mode_valid_downstream(connector, mode, target_clock);
	if (status != MODE_OK)
		return status;

	return intel_mode_valid_max_plane_size(display, mode, num_joined_pipes);
}

bool intel_dp_source_supports_tps3(struct intel_display *display)
{
	return DISPLAY_VER(display) >= 9 ||
		display->platform.broadwell || display->platform.haswell;
}

bool intel_dp_source_supports_tps4(struct intel_display *display)
{
	return DISPLAY_VER(display) >= 10;
}

static void seq_buf_print_array(struct seq_buf *s, const int *array, int nelem)
{
	int i;

	for (i = 0; i < nelem; i++)
		seq_buf_printf(s, "%s%d", i ? ", " : "", array[i]);
}

static void intel_dp_print_rates(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	DECLARE_SEQ_BUF(s, 128); /* FIXME: too big for stack? */

	if (!drm_debug_enabled(DRM_UT_KMS))
		return;

	seq_buf_print_array(&s, intel_dp->source_rates, intel_dp->num_source_rates);
	drm_dbg_kms(display->drm, "source rates: %s\n", seq_buf_str(&s));

	seq_buf_clear(&s);
	seq_buf_print_array(&s, intel_dp->sink_rates, intel_dp->num_sink_rates);
	drm_dbg_kms(display->drm, "sink rates: %s\n", seq_buf_str(&s));

	seq_buf_clear(&s);
	seq_buf_print_array(&s, intel_dp->common_rates, intel_dp->num_common_rates);
	drm_dbg_kms(display->drm, "common rates: %s\n", seq_buf_str(&s));
}

static int forced_link_rate(struct intel_dp *intel_dp)
{
	int len = intel_dp_common_len_rate_limit(intel_dp, intel_dp->link.force_rate);

	if (len == 0)
		return intel_dp_common_rate(intel_dp, 0);

	return intel_dp_common_rate(intel_dp, len - 1);
}

int
intel_dp_max_link_rate(struct intel_dp *intel_dp)
{
	int len;

	if (intel_dp->link.force_rate)
		return forced_link_rate(intel_dp);

	len = intel_dp_common_len_rate_limit(intel_dp, intel_dp->link.max_rate);

	return intel_dp_common_rate(intel_dp, len - 1);
}

static int
intel_dp_min_link_rate(struct intel_dp *intel_dp)
{
	if (intel_dp->link.force_rate)
		return forced_link_rate(intel_dp);

	return intel_dp_common_rate(intel_dp, 0);
}

int intel_dp_rate_select(struct intel_dp *intel_dp, int rate)
{
	struct intel_display *display = to_intel_display(intel_dp);
	int i = intel_dp_rate_index(intel_dp->sink_rates,
				    intel_dp->num_sink_rates, rate);

	if (drm_WARN_ON(display->drm, i < 0))
		i = 0;

	return i;
}

void intel_dp_compute_rate(struct intel_dp *intel_dp, int port_clock,
			   u8 *link_bw, u8 *rate_select)
{
	struct intel_display *display = to_intel_display(intel_dp);

	/* FIXME g4x can't generate an exact 2.7GHz with the 96MHz non-SSC refclk */
	if (display->platform.g4x && port_clock == 268800)
		port_clock = 270000;

	/* eDP 1.4 rate select method. */
	if (intel_dp->use_rate_select) {
		*link_bw = 0;
		*rate_select =
			intel_dp_rate_select(intel_dp, port_clock);
	} else {
		*link_bw = drm_dp_link_rate_to_bw_code(port_clock);
		*rate_select = 0;
	}
}

bool intel_dp_has_hdmi_sink(struct intel_dp *intel_dp)
{
	struct intel_connector *connector = intel_dp->attached_connector;

	return connector->base.display_info.is_hdmi;
}

static bool intel_dp_source_supports_fec(struct intel_dp *intel_dp,
					 const struct intel_crtc_state *pipe_config)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_encoder *encoder = &dp_to_dig_port(intel_dp)->base;

	if (DISPLAY_VER(display) >= 12)
		return true;

	if (DISPLAY_VER(display) == 11 && encoder->port != PORT_A &&
	    !intel_crtc_has_type(pipe_config, INTEL_OUTPUT_DP_MST))
		return true;

	return false;
}

bool intel_dp_supports_fec(struct intel_dp *intel_dp,
			   const struct intel_connector *connector,
			   const struct intel_crtc_state *pipe_config)
{
	return intel_dp_source_supports_fec(intel_dp, pipe_config) &&
		drm_dp_sink_supports_fec(connector->dp.fec_capability);
}

bool intel_dp_supports_dsc(struct intel_dp *intel_dp,
			   const struct intel_connector *connector,
			   const struct intel_crtc_state *crtc_state)
{
	if (!intel_dp_has_dsc(connector))
		return false;

	if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_DP) &&
	    !intel_dp_supports_fec(intel_dp, connector, crtc_state))
		return false;

	return intel_dsc_source_support(crtc_state);
}

static int intel_dp_hdmi_compute_bpc(struct intel_dp *intel_dp,
				     const struct intel_crtc_state *crtc_state,
				     int bpc, bool respect_downstream_limits)
{
	int clock = crtc_state->hw.adjusted_mode.crtc_clock;

	/*
	 * Current bpc could already be below 8bpc due to
	 * FDI bandwidth constraints or other limits.
	 * HDMI minimum is 8bpc however.
	 */
	bpc = max(bpc, 8);

	/*
	 * We will never exceed downstream TMDS clock limits while
	 * attempting deep color. If the user insists on forcing an
	 * out of spec mode they will have to be satisfied with 8bpc.
	 */
	if (!respect_downstream_limits)
		bpc = 8;

	for (; bpc >= 8; bpc -= 2) {
		if (intel_hdmi_bpc_possible(crtc_state, bpc,
					    intel_dp_has_hdmi_sink(intel_dp)) &&
		    intel_dp_tmds_clock_valid(intel_dp, clock, bpc, crtc_state->sink_format,
					      respect_downstream_limits) == MODE_OK)
			return bpc;
	}

	return -EINVAL;
}

static int intel_dp_max_bpp(struct intel_dp *intel_dp,
			    const struct intel_crtc_state *crtc_state,
			    bool respect_downstream_limits)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_connector *connector = intel_dp->attached_connector;
	int bpp, bpc;

	bpc = crtc_state->pipe_bpp / 3;

	if (intel_dp->dfp.max_bpc)
		bpc = min_t(int, bpc, intel_dp->dfp.max_bpc);

	if (intel_dp->dfp.min_tmds_clock) {
		int max_hdmi_bpc;

		max_hdmi_bpc = intel_dp_hdmi_compute_bpc(intel_dp, crtc_state, bpc,
							 respect_downstream_limits);
		if (max_hdmi_bpc < 0)
			return 0;

		bpc = min(bpc, max_hdmi_bpc);
	}

	bpp = bpc * 3;
	if (intel_dp_is_edp(intel_dp)) {
		/* Get bpp from vbt only for panels that dont have bpp in edid */
		if (connector->base.display_info.bpc == 0 &&
		    connector->panel.vbt.edp.bpp &&
		    connector->panel.vbt.edp.bpp < bpp) {
			drm_dbg_kms(display->drm,
				    "clamping bpp for eDP panel to BIOS-provided %i\n",
				    connector->panel.vbt.edp.bpp);
			bpp = connector->panel.vbt.edp.bpp;
		}
	}

	return bpp;
}

static bool has_seamless_m_n(struct intel_connector *connector)
{
	struct intel_display *display = to_intel_display(connector);

	/*
	 * Seamless M/N reprogramming only implemented
	 * for BDW+ double buffered M/N registers so far.
	 */
	return HAS_DOUBLE_BUFFERED_M_N(display) &&
		intel_panel_drrs_type(connector) == DRRS_TYPE_SEAMLESS;
}

static int intel_dp_mode_clock(const struct intel_crtc_state *crtc_state,
			       const struct drm_connector_state *conn_state)
{
	struct intel_connector *connector = to_intel_connector(conn_state->connector);
	const struct drm_display_mode *adjusted_mode = &crtc_state->hw.adjusted_mode;

	/* FIXME a bit of a mess wrt clock vs. crtc_clock */
	if (has_seamless_m_n(connector))
		return intel_panel_highest_mode(connector, adjusted_mode)->clock;
	else
		return adjusted_mode->crtc_clock;
}

/* Optimize link config in order: max bpp, min clock, min lanes */
static int
intel_dp_compute_link_config_wide(struct intel_dp *intel_dp,
				  struct intel_crtc_state *pipe_config,
				  const struct drm_connector_state *conn_state,
				  const struct link_config_limits *limits)
{
	int bpp, i, lane_count, clock = intel_dp_mode_clock(pipe_config, conn_state);
	int mode_rate, link_rate, link_avail;

	for (bpp = fxp_q4_to_int(limits->link.max_bpp_x16);
	     bpp >= fxp_q4_to_int(limits->link.min_bpp_x16);
	     bpp -= 2 * 3) {
		int link_bpp = intel_dp_output_bpp(pipe_config->output_format, bpp);

		mode_rate = intel_dp_link_required(clock, link_bpp);

		for (i = 0; i < intel_dp->num_common_rates; i++) {
			link_rate = intel_dp_common_rate(intel_dp, i);
			if (link_rate < limits->min_rate ||
			    link_rate > limits->max_rate)
				continue;

			for (lane_count = limits->min_lane_count;
			     lane_count <= limits->max_lane_count;
			     lane_count <<= 1) {
				link_avail = intel_dp_max_link_data_rate(intel_dp,
									 link_rate,
									 lane_count);


				if (mode_rate <= link_avail) {
					pipe_config->lane_count = lane_count;
					pipe_config->pipe_bpp = bpp;
					pipe_config->port_clock = link_rate;

					return 0;
				}
			}
		}
	}

	return -EINVAL;
}

int intel_dp_dsc_max_src_input_bpc(struct intel_display *display)
{
	/* Max DSC Input BPC for ICL is 10 and for TGL+ is 12 */
	if (DISPLAY_VER(display) >= 12)
		return 12;
	if (DISPLAY_VER(display) == 11)
		return 10;

	return intel_dp_dsc_min_src_input_bpc();
}

int intel_dp_dsc_compute_max_bpp(const struct intel_connector *connector,
				 u8 max_req_bpc)
{
	struct intel_display *display = to_intel_display(connector);
	int i, num_bpc;
	u8 dsc_bpc[3] = {};
	int dsc_max_bpc;

	dsc_max_bpc = intel_dp_dsc_max_src_input_bpc(display);

	if (!dsc_max_bpc)
		return dsc_max_bpc;

	dsc_max_bpc = min(dsc_max_bpc, max_req_bpc);

	num_bpc = drm_dp_dsc_sink_supported_input_bpcs(connector->dp.dsc_dpcd,
						       dsc_bpc);
	for (i = 0; i < num_bpc; i++) {
		if (dsc_max_bpc >= dsc_bpc[i])
			return dsc_bpc[i] * 3;
	}

	return 0;
}

static int intel_dp_source_dsc_version_minor(struct intel_display *display)
{
	return DISPLAY_VER(display) >= 14 ? 2 : 1;
}

static int intel_dp_sink_dsc_version_minor(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE])
{
	return (dsc_dpcd[DP_DSC_REV - DP_DSC_SUPPORT] & DP_DSC_MINOR_MASK) >>
		DP_DSC_MINOR_SHIFT;
}

static int intel_dp_get_slice_height(int vactive)
{
	int slice_height;

	/*
	 * VDSC 1.2a spec in Section 3.8 Options for Slices implies that 108
	 * lines is an optimal slice height, but any size can be used as long as
	 * vertical active integer multiple and maximum vertical slice count
	 * requirements are met.
	 */
	for (slice_height = 108; slice_height <= vactive; slice_height += 2)
		if (vactive % slice_height == 0)
			return slice_height;

	/*
	 * Highly unlikely we reach here as most of the resolutions will end up
	 * finding appropriate slice_height in above loop but returning
	 * slice_height as 2 here as it should work with all resolutions.
	 */
	return 2;
}

static int intel_dp_dsc_compute_params(const struct intel_connector *connector,
				       struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(connector);
	struct drm_dsc_config *vdsc_cfg = &crtc_state->dsc.config;
	int ret;

	/*
	 * RC_MODEL_SIZE is currently a constant across all configurations.
	 *
	 * FIXME: Look into using sink defined DPCD DP_DSC_RC_BUF_BLK_SIZE and
	 * DP_DSC_RC_BUF_SIZE for this.
	 */
	vdsc_cfg->rc_model_size = DSC_RC_MODEL_SIZE_CONST;
	vdsc_cfg->pic_height = crtc_state->hw.adjusted_mode.crtc_vdisplay;

	vdsc_cfg->slice_height = intel_dp_get_slice_height(vdsc_cfg->pic_height);

	ret = intel_dsc_compute_params(crtc_state);
	if (ret)
		return ret;

	vdsc_cfg->dsc_version_major =
		(connector->dp.dsc_dpcd[DP_DSC_REV - DP_DSC_SUPPORT] &
		 DP_DSC_MAJOR_MASK) >> DP_DSC_MAJOR_SHIFT;
	vdsc_cfg->dsc_version_minor =
		min(intel_dp_source_dsc_version_minor(display),
		    intel_dp_sink_dsc_version_minor(connector->dp.dsc_dpcd));
	if (vdsc_cfg->convert_rgb)
		vdsc_cfg->convert_rgb =
			connector->dp.dsc_dpcd[DP_DSC_DEC_COLOR_FORMAT_CAP - DP_DSC_SUPPORT] &
			DP_DSC_RGB;

	vdsc_cfg->line_buf_depth = min(INTEL_DP_DSC_MAX_LINE_BUF_DEPTH,
				       drm_dp_dsc_sink_line_buf_depth(connector->dp.dsc_dpcd));
	if (!vdsc_cfg->line_buf_depth) {
		drm_dbg_kms(display->drm,
			    "DSC Sink Line Buffer Depth invalid\n");
		return -EINVAL;
	}

	vdsc_cfg->block_pred_enable =
		connector->dp.dsc_dpcd[DP_DSC_BLK_PREDICTION_SUPPORT - DP_DSC_SUPPORT] &
		DP_DSC_BLK_PREDICTION_IS_SUPPORTED;

	return drm_dsc_compute_rc_parameters(vdsc_cfg);
}

static bool intel_dp_dsc_supports_format(const struct intel_connector *connector,
					 enum intel_output_format output_format)
{
	struct intel_display *display = to_intel_display(connector);
	u8 sink_dsc_format;

	switch (output_format) {
	case INTEL_OUTPUT_FORMAT_RGB:
		sink_dsc_format = DP_DSC_RGB;
		break;
	case INTEL_OUTPUT_FORMAT_YCBCR444:
		sink_dsc_format = DP_DSC_YCbCr444;
		break;
	case INTEL_OUTPUT_FORMAT_YCBCR420:
		if (min(intel_dp_source_dsc_version_minor(display),
			intel_dp_sink_dsc_version_minor(connector->dp.dsc_dpcd)) < 2)
			return false;
		sink_dsc_format = DP_DSC_YCbCr420_Native;
		break;
	default:
		return false;
	}

	return drm_dp_dsc_sink_supports_format(connector->dp.dsc_dpcd, sink_dsc_format);
}

static bool is_bw_sufficient_for_dsc_config(int dsc_bpp_x16, u32 link_clock,
					    u32 lane_count, u32 mode_clock,
					    enum intel_output_format output_format,
					    int timeslots)
{
	u32 available_bw, required_bw;

	available_bw = (link_clock * lane_count * timeslots * 16)  / 8;
	required_bw = dsc_bpp_x16 * (intel_dp_mode_to_fec_clock(mode_clock));

	return available_bw > required_bw;
}

static int dsc_compute_link_config(struct intel_dp *intel_dp,
				   struct intel_crtc_state *pipe_config,
				   struct drm_connector_state *conn_state,
				   const struct link_config_limits *limits,
				   int dsc_bpp_x16,
				   int timeslots)
{
	const struct drm_display_mode *adjusted_mode = &pipe_config->hw.adjusted_mode;
	int link_rate, lane_count;
	int i;

	for (i = 0; i < intel_dp->num_common_rates; i++) {
		link_rate = intel_dp_common_rate(intel_dp, i);
		if (link_rate < limits->min_rate || link_rate > limits->max_rate)
			continue;

		for (lane_count = limits->min_lane_count;
		     lane_count <= limits->max_lane_count;
		     lane_count <<= 1) {

			/*
			 * FIXME: intel_dp_mtp_tu_compute_config() requires
			 * ->lane_count and ->port_clock set before we know
			 * they'll work. If we end up failing altogether,
			 * they'll remain in crtc state. This shouldn't matter,
			 * as we'd then bail out from compute config, but it's
			 * just ugly.
			 */
			pipe_config->lane_count = lane_count;
			pipe_config->port_clock = link_rate;

			if (drm_dp_is_uhbr_rate(link_rate)) {
				int ret;

				ret = intel_dp_mtp_tu_compute_config(intel_dp,
								     pipe_config,
								     conn_state,
								     dsc_bpp_x16,
								     dsc_bpp_x16,
								     0, true);
				if (ret)
					continue;
			} else {
				if (!is_bw_sufficient_for_dsc_config(dsc_bpp_x16, link_rate,
								     lane_count, adjusted_mode->clock,
								     pipe_config->output_format,
								     timeslots))
					continue;
			}

			return 0;
		}
	}

	return -EINVAL;
}

static
u16 intel_dp_dsc_max_sink_compressed_bppx16(const struct intel_connector *connector,
					    const struct intel_crtc_state *pipe_config,
					    int bpc)
{
	u16 max_bppx16 = drm_edp_dsc_sink_output_bpp(connector->dp.dsc_dpcd);

	if (max_bppx16)
		return max_bppx16;
	/*
	 * If support not given in DPCD 67h, 68h use the Maximum Allowed bit rate
	 * values as given in spec Table 2-157 DP v2.0
	 */
	switch (pipe_config->output_format) {
	case INTEL_OUTPUT_FORMAT_RGB:
	case INTEL_OUTPUT_FORMAT_YCBCR444:
		return (3 * bpc) << 4;
	case INTEL_OUTPUT_FORMAT_YCBCR420:
		return (3 * (bpc / 2)) << 4;
	default:
		MISSING_CASE(pipe_config->output_format);
		break;
	}

	return 0;
}

int intel_dp_dsc_sink_min_compressed_bpp(const struct intel_crtc_state *pipe_config)
{
	/* From Mandatory bit rate range Support Table 2-157 (DP v2.0) */
	switch (pipe_config->output_format) {
	case INTEL_OUTPUT_FORMAT_RGB:
	case INTEL_OUTPUT_FORMAT_YCBCR444:
		return 8;
	case INTEL_OUTPUT_FORMAT_YCBCR420:
		return 6;
	default:
		MISSING_CASE(pipe_config->output_format);
		break;
	}

	return 0;
}

int intel_dp_dsc_sink_max_compressed_bpp(const struct intel_connector *connector,
					 const struct intel_crtc_state *pipe_config,
					 int bpc)
{
	return intel_dp_dsc_max_sink_compressed_bppx16(connector,
						       pipe_config, bpc) >> 4;
}

int intel_dp_dsc_min_src_compressed_bpp(void)
{
	/* Min Compressed bpp supported by source is 8 */
	return 8;
}

static int dsc_src_max_compressed_bpp(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);

	/*
	 * Forcing DSC and using the platform's max compressed bpp is seen to cause
	 * underruns. Since DSC isn't needed in these cases, limit the
	 * max compressed bpp to 18, which is a safe value across platforms with different
	 * pipe bpps.
	 */
	if (intel_dp->force_dsc_en)
		return 18;

	/*
	 * Max Compressed bpp for Gen 13+ is 27bpp.
	 * For earlier platform is 23bpp. (Bspec:49259).
	 */
	if (DISPLAY_VER(display) < 13)
		return 23;
	else
		return 27;
}

/*
 * Note: for pre-13 display you still need to check the validity of each step.
 */
int intel_dp_dsc_bpp_step_x16(const struct intel_connector *connector)
{
	struct intel_display *display = to_intel_display(connector);
	u8 incr = drm_dp_dsc_sink_bpp_incr(connector->dp.dsc_dpcd);

	if (DISPLAY_VER(display) < 14 || !incr)
		return fxp_q4_from_int(1);

	if (connector->mst.dp &&
	    !connector->link.force_bpp_x16 && !connector->mst.dp->force_dsc_fractional_bpp_en)
		return fxp_q4_from_int(1);

	/* fxp q4 */
	return fxp_q4_from_int(1) / incr;
}

/*
 * Note: for bpp_x16 to be valid it must be also within the source/sink's
 * min..max bpp capability range.
 */
bool intel_dp_dsc_valid_compressed_bpp(struct intel_dp *intel_dp, int bpp_x16)
{
	struct intel_display *display = to_intel_display(intel_dp);
	int i;

	if (DISPLAY_VER(display) >= 13) {
		if (intel_dp->force_dsc_fractional_bpp_en && !fxp_q4_to_frac(bpp_x16))
			return false;

		return true;
	}

	if (fxp_q4_to_frac(bpp_x16))
		return false;

	for (i = 0; i < ARRAY_SIZE(valid_dsc_bpp); i++) {
		if (fxp_q4_to_int(bpp_x16) == valid_dsc_bpp[i])
			return true;
	}

	return false;
}

/*
 * Find the max compressed BPP we can find a link configuration for. The BPPs to
 * try depend on the source (platform) and sink.
 */
static int dsc_compute_compressed_bpp(struct intel_dp *intel_dp,
				      struct intel_crtc_state *pipe_config,
				      struct drm_connector_state *conn_state,
				      const struct link_config_limits *limits,
				      int pipe_bpp,
				      int timeslots)
{
	struct intel_display *display = to_intel_display(intel_dp);
	const struct intel_connector *connector = to_intel_connector(conn_state->connector);
	const struct drm_display_mode *adjusted_mode = &pipe_config->hw.adjusted_mode;
	int output_bpp;
	int min_bpp_x16, max_bpp_x16, bpp_step_x16;
	int dsc_joiner_max_bpp;
	int num_joined_pipes = intel_crtc_num_joined_pipes(pipe_config);
	int bpp_x16;
	int ret;

	dsc_joiner_max_bpp = get_max_compressed_bpp_with_joiner(display, adjusted_mode->clock,
								adjusted_mode->hdisplay,
								num_joined_pipes);
	max_bpp_x16 = min(fxp_q4_from_int(dsc_joiner_max_bpp), limits->link.max_bpp_x16);

	bpp_step_x16 = intel_dp_dsc_bpp_step_x16(connector);

	/* Compressed BPP should be less than the Input DSC bpp */
	output_bpp = intel_dp_output_bpp(pipe_config->output_format, pipe_bpp);
	max_bpp_x16 = min(max_bpp_x16, fxp_q4_from_int(output_bpp) - bpp_step_x16);

	drm_WARN_ON(display->drm, !is_power_of_2(bpp_step_x16));
	min_bpp_x16 = round_up(limits->link.min_bpp_x16, bpp_step_x16);
	max_bpp_x16 = round_down(max_bpp_x16, bpp_step_x16);

	for (bpp_x16 = max_bpp_x16; bpp_x16 >= min_bpp_x16; bpp_x16 -= bpp_step_x16) {
		if (!intel_dp_dsc_valid_compressed_bpp(intel_dp, bpp_x16))
			continue;

		ret = dsc_compute_link_config(intel_dp,
					      pipe_config,
					      conn_state,
					      limits,
					      bpp_x16,
					      timeslots);
		if (ret == 0) {
			pipe_config->dsc.compressed_bpp_x16 = bpp_x16;
			if (intel_dp->force_dsc_fractional_bpp_en &&
			    fxp_q4_to_frac(bpp_x16))
				drm_dbg_kms(display->drm,
					    "Forcing DSC fractional bpp\n");

			return 0;
		}
	}

	return -EINVAL;
}

int intel_dp_dsc_min_src_input_bpc(void)
{
	/* Min DSC Input BPC for ICL+ is 8 */
	return 8;
}

static
bool is_dsc_pipe_bpp_sufficient(const struct link_config_limits *limits,
				int pipe_bpp)
{
	return pipe_bpp >= limits->pipe.min_bpp &&
	       pipe_bpp <= limits->pipe.max_bpp;
}

static
int intel_dp_force_dsc_pipe_bpp(struct intel_dp *intel_dp,
				const struct link_config_limits *limits)
{
	struct intel_display *display = to_intel_display(intel_dp);
	int forced_bpp;

	if (!intel_dp->force_dsc_bpc)
		return 0;

	forced_bpp = intel_dp->force_dsc_bpc * 3;

	if (is_dsc_pipe_bpp_sufficient(limits, forced_bpp)) {
		drm_dbg_kms(display->drm, "Input DSC BPC forced to %d\n",
			    intel_dp->force_dsc_bpc);
		return forced_bpp;
	}

	drm_dbg_kms(display->drm,
		    "Cannot force DSC BPC:%d, due to DSC BPC limits\n",
		    intel_dp->force_dsc_bpc);

	return 0;
}

static int intel_dp_dsc_compute_pipe_bpp(struct intel_dp *intel_dp,
					 struct intel_crtc_state *pipe_config,
					 struct drm_connector_state *conn_state,
					 const struct link_config_limits *limits,
					 int timeslots)
{
	const struct intel_connector *connector =
		to_intel_connector(conn_state->connector);
	u8 dsc_bpc[3] = {};
	int forced_bpp, pipe_bpp;
	int num_bpc, i, ret;

	forced_bpp = intel_dp_force_dsc_pipe_bpp(intel_dp, limits);

	if (forced_bpp) {
		ret = dsc_compute_compressed_bpp(intel_dp, pipe_config, conn_state,
						 limits, forced_bpp, timeslots);
		if (ret == 0) {
			pipe_config->pipe_bpp = forced_bpp;
			return 0;
		}
	}

	/*
	 * Get the maximum DSC bpc that will be supported by any valid
	 * link configuration and compressed bpp.
	 */
	num_bpc = drm_dp_dsc_sink_supported_input_bpcs(connector->dp.dsc_dpcd, dsc_bpc);
	for (i = 0; i < num_bpc; i++) {
		pipe_bpp = dsc_bpc[i] * 3;
		if (pipe_bpp < limits->pipe.min_bpp || pipe_bpp > limits->pipe.max_bpp)
			continue;

		ret = dsc_compute_compressed_bpp(intel_dp, pipe_config, conn_state,
						 limits, pipe_bpp, timeslots);
		if (ret == 0) {
			pipe_config->pipe_bpp = pipe_bpp;
			return 0;
		}
	}

	return -EINVAL;
}

static int intel_edp_dsc_compute_pipe_bpp(struct intel_dp *intel_dp,
					  struct intel_crtc_state *pipe_config,
					  struct drm_connector_state *conn_state,
					  const struct link_config_limits *limits)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_connector *connector =
		to_intel_connector(conn_state->connector);
	int pipe_bpp, forced_bpp;
	int dsc_min_bpp;
	int dsc_max_bpp;

	forced_bpp = intel_dp_force_dsc_pipe_bpp(intel_dp, limits);

	if (forced_bpp) {
		pipe_bpp = forced_bpp;
	} else {
		int max_bpc = limits->pipe.max_bpp / 3;

		/* For eDP use max bpp that can be supported with DSC. */
		pipe_bpp = intel_dp_dsc_compute_max_bpp(connector, max_bpc);
		if (!is_dsc_pipe_bpp_sufficient(limits, pipe_bpp)) {
			drm_dbg_kms(display->drm,
				    "Computed BPC is not in DSC BPC limits\n");
			return -EINVAL;
		}
	}
	pipe_config->port_clock = limits->max_rate;
	pipe_config->lane_count = limits->max_lane_count;

	dsc_min_bpp = fxp_q4_to_int_roundup(limits->link.min_bpp_x16);

	dsc_max_bpp = fxp_q4_to_int(limits->link.max_bpp_x16);

	/* Compressed BPP should be less than the Input DSC bpp */
	dsc_max_bpp = min(dsc_max_bpp, pipe_bpp - 1);

	pipe_config->dsc.compressed_bpp_x16 =
		fxp_q4_from_int(max(dsc_min_bpp, dsc_max_bpp));

	pipe_config->pipe_bpp = pipe_bpp;

	return 0;
}

static void intel_dp_fec_compute_config(struct intel_dp *intel_dp,
					struct intel_crtc_state *crtc_state)
{
	if (crtc_state->fec_enable)
		return;

	/*
	 * Though eDP v1.5 supports FEC with DSC, unlike DP, it is optional.
	 * Since, FEC is a bandwidth overhead, continue to not enable it for
	 * eDP. Until, there is a good reason to do so.
	 */
	if (intel_dp_is_edp(intel_dp))
		return;

	if (intel_dp_is_uhbr(crtc_state))
		return;

	crtc_state->fec_enable = true;
}

int intel_dp_dsc_compute_config(struct intel_dp *intel_dp,
				struct intel_crtc_state *pipe_config,
				struct drm_connector_state *conn_state,
				const struct link_config_limits *limits,
				int timeslots)
{
	struct intel_display *display = to_intel_display(intel_dp);
	const struct intel_connector *connector =
		to_intel_connector(conn_state->connector);
	const struct drm_display_mode *adjusted_mode =
		&pipe_config->hw.adjusted_mode;
	int num_joined_pipes = intel_crtc_num_joined_pipes(pipe_config);
	bool is_mst = intel_crtc_has_type(pipe_config, INTEL_OUTPUT_DP_MST);
	int ret;

	intel_dp_fec_compute_config(intel_dp, pipe_config);

	if (!intel_dp_dsc_supports_format(connector, pipe_config->output_format))
		return -EINVAL;

	/*
	 * Link parameters, pipe bpp and compressed bpp have already been
	 * figured out for DP MST DSC.
	 */
	if (!is_mst) {
		if (intel_dp_is_edp(intel_dp))
			ret = intel_edp_dsc_compute_pipe_bpp(intel_dp, pipe_config,
							     conn_state, limits);
		else
			ret = intel_dp_dsc_compute_pipe_bpp(intel_dp, pipe_config,
							    conn_state, limits, timeslots);
		if (ret) {
			drm_dbg_kms(display->drm,
				    "No Valid pipe bpp for given mode ret = %d\n", ret);
			return ret;
		}
	}

	/* Calculate Slice count */
	if (intel_dp_is_edp(intel_dp)) {
		pipe_config->dsc.slice_count =
			drm_dp_dsc_sink_max_slice_count(connector->dp.dsc_dpcd,
							true);
		if (!pipe_config->dsc.slice_count) {
			drm_dbg_kms(display->drm,
				    "Unsupported Slice Count %d\n",
				    pipe_config->dsc.slice_count);
			return -EINVAL;
		}
	} else {
		u8 dsc_dp_slice_count;

		dsc_dp_slice_count =
			intel_dp_dsc_get_slice_count(connector,
						     adjusted_mode->crtc_clock,
						     adjusted_mode->crtc_hdisplay,
						     num_joined_pipes);
		if (!dsc_dp_slice_count) {
			drm_dbg_kms(display->drm,
				    "Compressed Slice Count not supported\n");
			return -EINVAL;
		}

		pipe_config->dsc.slice_count = dsc_dp_slice_count;
	}
	/*
	 * VDSC engine operates at 1 Pixel per clock, so if peak pixel rate
	 * is greater than the maximum Cdclock and if slice count is even
	 * then we need to use 2 VDSC instances.
	 * In case of Ultrajoiner along with 12 slices we need to use 3
	 * VDSC instances.
	 */
	if (pipe_config->joiner_pipes && num_joined_pipes == 4 &&
	    pipe_config->dsc.slice_count == 12)
		pipe_config->dsc.num_streams = 3;
	else if (pipe_config->joiner_pipes || pipe_config->dsc.slice_count > 1)
		pipe_config->dsc.num_streams = 2;
	else
		pipe_config->dsc.num_streams = 1;

	ret = intel_dp_dsc_compute_params(connector, pipe_config);
	if (ret < 0) {
		drm_dbg_kms(display->drm,
			    "Cannot compute valid DSC parameters for Input Bpp = %d"
			    "Compressed BPP = " FXP_Q4_FMT "\n",
			    pipe_config->pipe_bpp,
			    FXP_Q4_ARGS(pipe_config->dsc.compressed_bpp_x16));
		return ret;
	}

	pipe_config->dsc.compression_enable = true;
	drm_dbg_kms(display->drm, "DP DSC computed with Input Bpp = %d "
		    "Compressed Bpp = " FXP_Q4_FMT " Slice Count = %d\n",
		    pipe_config->pipe_bpp,
		    FXP_Q4_ARGS(pipe_config->dsc.compressed_bpp_x16),
		    pipe_config->dsc.slice_count);

	return 0;
}

/*
 * Calculate the output link min, max bpp values in limits based on the pipe bpp
 * range, crtc_state and dsc mode. Return true on success.
 */
static bool
intel_dp_compute_config_link_bpp_limits(struct intel_dp *intel_dp,
					const struct intel_connector *connector,
					const struct intel_crtc_state *crtc_state,
					bool dsc,
					struct link_config_limits *limits)
{
	struct intel_display *display = to_intel_display(intel_dp);
	const struct drm_display_mode *adjusted_mode =
		&crtc_state->hw.adjusted_mode;
	const struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	const struct intel_encoder *encoder = &dp_to_dig_port(intel_dp)->base;
	int max_link_bpp_x16;

	max_link_bpp_x16 = min(crtc_state->max_link_bpp_x16,
			       fxp_q4_from_int(limits->pipe.max_bpp));

	if (!dsc) {
		max_link_bpp_x16 = rounddown(max_link_bpp_x16, fxp_q4_from_int(2 * 3));

		if (max_link_bpp_x16 < fxp_q4_from_int(limits->pipe.min_bpp))
			return false;

		limits->link.min_bpp_x16 = fxp_q4_from_int(limits->pipe.min_bpp);
	} else {
		int dsc_src_min_bpp, dsc_sink_min_bpp, dsc_min_bpp;
		int dsc_src_max_bpp, dsc_sink_max_bpp, dsc_max_bpp;

		dsc_src_min_bpp = intel_dp_dsc_min_src_compressed_bpp();
		dsc_sink_min_bpp = intel_dp_dsc_sink_min_compressed_bpp(crtc_state);
		dsc_min_bpp = max(dsc_src_min_bpp, dsc_sink_min_bpp);
		limits->link.min_bpp_x16 = fxp_q4_from_int(dsc_min_bpp);

		dsc_src_max_bpp = dsc_src_max_compressed_bpp(intel_dp);
		dsc_sink_max_bpp = intel_dp_dsc_sink_max_compressed_bpp(connector,
									crtc_state,
									limits->pipe.max_bpp / 3);
		dsc_max_bpp = dsc_sink_max_bpp ?
			      min(dsc_sink_max_bpp, dsc_src_max_bpp) : dsc_src_max_bpp;

		max_link_bpp_x16 = min(max_link_bpp_x16, fxp_q4_from_int(dsc_max_bpp));
	}

	limits->link.max_bpp_x16 = max_link_bpp_x16;

	drm_dbg_kms(display->drm,
		    "[ENCODER:%d:%s][CRTC:%d:%s] DP link limits: pixel clock %d kHz DSC %s max lanes %d max rate %d max pipe_bpp %d max link_bpp " FXP_Q4_FMT "\n",
		    encoder->base.base.id, encoder->base.name,
		    crtc->base.base.id, crtc->base.name,
		    adjusted_mode->crtc_clock,
		    str_on_off(dsc),
		    limits->max_lane_count,
		    limits->max_rate,
		    limits->pipe.max_bpp,
		    FXP_Q4_ARGS(limits->link.max_bpp_x16));

	return true;
}

static void
intel_dp_dsc_compute_pipe_bpp_limits(struct intel_dp *intel_dp,
				     struct link_config_limits *limits)
{
	struct intel_display *display = to_intel_display(intel_dp);
	int dsc_min_bpc = intel_dp_dsc_min_src_input_bpc();
	int dsc_max_bpc = intel_dp_dsc_max_src_input_bpc(display);

	limits->pipe.max_bpp = clamp(limits->pipe.max_bpp, dsc_min_bpc * 3, dsc_max_bpc * 3);
	limits->pipe.min_bpp = clamp(limits->pipe.min_bpp, dsc_min_bpc * 3, dsc_max_bpc * 3);
}

bool
intel_dp_compute_config_limits(struct intel_dp *intel_dp,
			       struct intel_connector *connector,
			       struct intel_crtc_state *crtc_state,
			       bool respect_downstream_limits,
			       bool dsc,
			       struct link_config_limits *limits)
{
	bool is_mst = intel_crtc_has_type(crtc_state, INTEL_OUTPUT_DP_MST);

	limits->min_rate = intel_dp_min_link_rate(intel_dp);
	limits->max_rate = intel_dp_max_link_rate(intel_dp);

	limits->min_rate = min(limits->min_rate, limits->max_rate);

	limits->min_lane_count = intel_dp_min_lane_count(intel_dp);
	limits->max_lane_count = intel_dp_max_lane_count(intel_dp);

	limits->pipe.min_bpp = intel_dp_min_bpp(crtc_state->output_format);
	if (is_mst) {
		/*
		 * FIXME: If all the streams can't fit into the link with their
		 * current pipe_bpp we should reduce pipe_bpp across the board
		 * until things start to fit. Until then we limit to <= 8bpc
		 * since that's what was hardcoded for all MST streams
		 * previously. This hack should be removed once we have the
		 * proper retry logic in place.
		 */
		limits->pipe.max_bpp = min(crtc_state->pipe_bpp, 24);
	} else {
		limits->pipe.max_bpp = intel_dp_max_bpp(intel_dp, crtc_state,
							respect_downstream_limits);
	}

	if (dsc)
		intel_dp_dsc_compute_pipe_bpp_limits(intel_dp, limits);

	if (is_mst || intel_dp->use_max_params) {
		/*
		 * For MST we always configure max link bw - the spec doesn't
		 * seem to suggest we should do otherwise.
		 *
		 * Use the maximum clock and number of lanes the eDP panel
		 * advertizes being capable of in case the initial fast
		 * optimal params failed us. The panels are generally
		 * designed to support only a single clock and lane
		 * configuration, and typically on older panels these
		 * values correspond to the native resolution of the panel.
		 */
		limits->min_lane_count = limits->max_lane_count;
		limits->min_rate = limits->max_rate;
	}

	intel_dp_test_compute_config(intel_dp, crtc_state, limits);

	return intel_dp_compute_config_link_bpp_limits(intel_dp,
						       connector,
						       crtc_state,
						       dsc,
						       limits);
}

int intel_dp_config_required_rate(const struct intel_crtc_state *crtc_state)
{
	const struct drm_display_mode *adjusted_mode =
		&crtc_state->hw.adjusted_mode;
	int bpp = crtc_state->dsc.compression_enable ?
		fxp_q4_to_int_roundup(crtc_state->dsc.compressed_bpp_x16) :
		crtc_state->pipe_bpp;

	return intel_dp_link_required(adjusted_mode->crtc_clock, bpp);
}

bool intel_dp_joiner_needs_dsc(struct intel_display *display,
			       int num_joined_pipes)
{
	/*
	 * Pipe joiner needs compression up to display 12 due to bandwidth
	 * limitation. DG2 onwards pipe joiner can be enabled without
	 * compression.
	 * Ultrajoiner always needs compression.
	 */
	return (!HAS_UNCOMPRESSED_JOINER(display) && num_joined_pipes == 2) ||
		num_joined_pipes == 4;
}

static int
intel_dp_compute_link_config(struct intel_encoder *encoder,
			     struct intel_crtc_state *pipe_config,
			     struct drm_connector_state *conn_state,
			     bool respect_downstream_limits)
{
	struct intel_display *display = to_intel_display(encoder);
	struct intel_crtc *crtc = to_intel_crtc(pipe_config->uapi.crtc);
	struct intel_connector *connector =
		to_intel_connector(conn_state->connector);
	const struct drm_display_mode *adjusted_mode =
		&pipe_config->hw.adjusted_mode;
	struct intel_dp *intel_dp = enc_to_intel_dp(encoder);
	struct link_config_limits limits;
	bool dsc_needed, joiner_needs_dsc;
	int num_joined_pipes;
	int ret = 0;

	if (pipe_config->fec_enable &&
	    !intel_dp_supports_fec(intel_dp, connector, pipe_config))
		return -EINVAL;

	num_joined_pipes = intel_dp_num_joined_pipes(intel_dp, connector,
						     adjusted_mode->crtc_hdisplay,
						     adjusted_mode->crtc_clock);
	if (num_joined_pipes > 1)
		pipe_config->joiner_pipes = GENMASK(crtc->pipe + num_joined_pipes - 1, crtc->pipe);

	joiner_needs_dsc = intel_dp_joiner_needs_dsc(display, num_joined_pipes);

	dsc_needed = joiner_needs_dsc || intel_dp->force_dsc_en ||
		     !intel_dp_compute_config_limits(intel_dp, connector, pipe_config,
						     respect_downstream_limits,
						     false,
						     &limits);

	if (!dsc_needed) {
		/*
		 * Optimize for slow and wide for everything, because there are some
		 * eDP 1.3 and 1.4 panels don't work well with fast and narrow.
		 */
		ret = intel_dp_compute_link_config_wide(intel_dp, pipe_config,
							conn_state, &limits);
		if (!ret && intel_dp_is_uhbr(pipe_config))
			ret = intel_dp_mtp_tu_compute_config(intel_dp,
							     pipe_config,
							     conn_state,
							     fxp_q4_from_int(pipe_config->pipe_bpp),
							     fxp_q4_from_int(pipe_config->pipe_bpp),
							     0, false);
		if (ret)
			dsc_needed = true;
	}

	if (dsc_needed && !intel_dp_supports_dsc(intel_dp, connector, pipe_config)) {
		drm_dbg_kms(display->drm, "DSC required but not available\n");
		return -EINVAL;
	}

	if (dsc_needed) {
		drm_dbg_kms(display->drm,
			    "Try DSC (fallback=%s, joiner=%s, force=%s)\n",
			    str_yes_no(ret), str_yes_no(joiner_needs_dsc),
			    str_yes_no(intel_dp->force_dsc_en));

		if (!intel_dp_compute_config_limits(intel_dp, connector, pipe_config,
						    respect_downstream_limits,
						    true,
						    &limits))
			return -EINVAL;

		ret = intel_dp_dsc_compute_config(intel_dp, pipe_config,
						  conn_state, &limits, 64);
		if (ret < 0)
			return ret;
	}

	drm_dbg_kms(display->drm,
		    "DP lane count %d clock %d bpp input %d compressed " FXP_Q4_FMT " link rate required %d available %d\n",
		    pipe_config->lane_count, pipe_config->port_clock,
		    pipe_config->pipe_bpp,
		    FXP_Q4_ARGS(pipe_config->dsc.compressed_bpp_x16),
		    intel_dp_config_required_rate(pipe_config),
		    intel_dp_max_link_data_rate(intel_dp,
						pipe_config->port_clock,
						pipe_config->lane_count));

	return 0;
}

bool intel_dp_limited_color_range(const struct intel_crtc_state *crtc_state,
				  const struct drm_connector_state *conn_state)
{
	const struct intel_digital_connector_state *intel_conn_state =
		to_intel_digital_connector_state(conn_state);
	const struct drm_display_mode *adjusted_mode =
		&crtc_state->hw.adjusted_mode;

	/*
	 * Our YCbCr output is always limited range.
	 * crtc_state->limited_color_range only applies to RGB,
	 * and it must never be set for YCbCr or we risk setting
	 * some conflicting bits in TRANSCONF which will mess up
	 * the colors on the monitor.
	 */
	if (crtc_state->output_format != INTEL_OUTPUT_FORMAT_RGB)
		return false;

	if (intel_conn_state->broadcast_rgb == INTEL_BROADCAST_RGB_AUTO) {
		/*
		 * See:
		 * CEA-861-E - 5.1 Default Encoding Parameters
		 * VESA DisplayPort Ver.1.2a - 5.1.1.1 Video Colorimetry
		 */
		return crtc_state->pipe_bpp != 18 &&
			drm_default_rgb_quant_range(adjusted_mode) ==
			HDMI_QUANTIZATION_RANGE_LIMITED;
	} else {
		return intel_conn_state->broadcast_rgb ==
			INTEL_BROADCAST_RGB_LIMITED;
	}
}

static bool intel_dp_port_has_audio(struct intel_display *display, enum port port)
{
	if (display->platform.g4x)
		return false;
	if (DISPLAY_VER(display) < 12 && port == PORT_A)
		return false;

	return true;
}

static void intel_dp_compute_vsc_colorimetry(const struct intel_crtc_state *crtc_state,
					     const struct drm_connector_state *conn_state,
					     struct drm_dp_vsc_sdp *vsc)
{
	struct intel_display *display = to_intel_display(crtc_state);

	if (crtc_state->has_panel_replay) {
		/*
		 * Prepare VSC Header for SU as per DP 2.0 spec, Table 2-223
		 * VSC SDP supporting 3D stereo, Panel Replay, and Pixel
		 * Encoding/Colorimetry Format indication.
		 */
		vsc->revision = 0x7;
	} else {
		/*
		 * Prepare VSC Header for SU as per DP 1.4 spec, Table 2-118
		 * VSC SDP supporting 3D stereo, PSR2, and Pixel Encoding/
		 * Colorimetry Format indication.
		 */
		vsc->revision = 0x5;
	}

	vsc->length = 0x13;

	/* DP 1.4a spec, Table 2-120 */
	switch (crtc_state->output_format) {
	case INTEL_OUTPUT_FORMAT_YCBCR444:
		vsc->pixelformat = DP_PIXELFORMAT_YUV444;
		break;
	case INTEL_OUTPUT_FORMAT_YCBCR420:
		vsc->pixelformat = DP_PIXELFORMAT_YUV420;
		break;
	case INTEL_OUTPUT_FORMAT_RGB:
	default:
		vsc->pixelformat = DP_PIXELFORMAT_RGB;
	}

	switch (conn_state->colorspace) {
	case DRM_MODE_COLORIMETRY_BT709_YCC:
		vsc->colorimetry = DP_COLORIMETRY_BT709_YCC;
		break;
	case DRM_MODE_COLORIMETRY_XVYCC_601:
		vsc->colorimetry = DP_COLORIMETRY_XVYCC_601;
		break;
	case DRM_MODE_COLORIMETRY_XVYCC_709:
		vsc->colorimetry = DP_COLORIMETRY_XVYCC_709;
		break;
	case DRM_MODE_COLORIMETRY_SYCC_601:
		vsc->colorimetry = DP_COLORIMETRY_SYCC_601;
		break;
	case DRM_MODE_COLORIMETRY_OPYCC_601:
		vsc->colorimetry = DP_COLORIMETRY_OPYCC_601;
		break;
	case DRM_MODE_COLORIMETRY_BT2020_CYCC:
		vsc->colorimetry = DP_COLORIMETRY_BT2020_CYCC;
		break;
	case DRM_MODE_COLORIMETRY_BT2020_RGB:
		vsc->colorimetry = DP_COLORIMETRY_BT2020_RGB;
		break;
	case DRM_MODE_COLORIMETRY_BT2020_YCC:
		vsc->colorimetry = DP_COLORIMETRY_BT2020_YCC;
		break;
	case DRM_MODE_COLORIMETRY_DCI_P3_RGB_D65:
	case DRM_MODE_COLORIMETRY_DCI_P3_RGB_THEATER:
		vsc->colorimetry = DP_COLORIMETRY_DCI_P3_RGB;
		break;
	default:
		/*
		 * RGB->YCBCR color conversion uses the BT.709
		 * color space.
		 */
		if (crtc_state->output_format == INTEL_OUTPUT_FORMAT_YCBCR420)
			vsc->colorimetry = DP_COLORIMETRY_BT709_YCC;
		else
			vsc->colorimetry = DP_COLORIMETRY_DEFAULT;
		break;
	}

	vsc->bpc = crtc_state->pipe_bpp / 3;

	/* only RGB pixelformat supports 6 bpc */
	drm_WARN_ON(display->drm,
		    vsc->bpc == 6 && vsc->pixelformat != DP_PIXELFORMAT_RGB);

	/* all YCbCr are always limited range */
	vsc->dynamic_range = DP_DYNAMIC_RANGE_CTA;
	vsc->content_type = DP_CONTENT_TYPE_NOT_DEFINED;
}

static void intel_dp_compute_as_sdp(struct intel_dp *intel_dp,
				    struct intel_crtc_state *crtc_state)
{
	struct drm_dp_as_sdp *as_sdp = &crtc_state->infoframes.as_sdp;
	const struct drm_display_mode *adjusted_mode =
		&crtc_state->hw.adjusted_mode;

	if (!crtc_state->vrr.enable || !intel_dp->as_sdp_supported)
		return;

	crtc_state->infoframes.enable |= intel_hdmi_infoframe_enable(DP_SDP_ADAPTIVE_SYNC);

	as_sdp->sdp_type = DP_SDP_ADAPTIVE_SYNC;
	as_sdp->length = 0x9;
	as_sdp->duration_incr_ms = 0;
	as_sdp->vtotal = intel_vrr_vmin_vtotal(crtc_state);

	if (crtc_state->cmrr.enable) {
		as_sdp->mode = DP_AS_SDP_FAVT_TRR_REACHED;
		as_sdp->target_rr = drm_mode_vrefresh(adjusted_mode);
		as_sdp->target_rr_divider = true;
	} else {
		as_sdp->mode = DP_AS_SDP_AVT_DYNAMIC_VTOTAL;
		as_sdp->target_rr = 0;
	}
}

static void intel_dp_compute_vsc_sdp(struct intel_dp *intel_dp,
				     struct intel_crtc_state *crtc_state,
				     const struct drm_connector_state *conn_state)
{
	struct drm_dp_vsc_sdp *vsc;

	if ((!intel_dp->colorimetry_support ||
	     !intel_dp_needs_vsc_sdp(crtc_state, conn_state)) &&
	    !crtc_state->has_psr)
		return;

	vsc = &crtc_state->infoframes.vsc;

	crtc_state->infoframes.enable |= intel_hdmi_infoframe_enable(DP_SDP_VSC);
	vsc->sdp_type = DP_SDP_VSC;

	/* Needs colorimetry */
	if (intel_dp_needs_vsc_sdp(crtc_state, conn_state)) {
		intel_dp_compute_vsc_colorimetry(crtc_state, conn_state,
						 vsc);
	} else if (crtc_state->has_panel_replay) {
		/*
		 * [Panel Replay without colorimetry info]
		 * Prepare VSC Header for SU as per DP 2.0 spec, Table 2-223
		 * VSC SDP supporting 3D stereo + Panel Replay.
		 */
		vsc->revision = 0x6;
		vsc->length = 0x10;
	} else if (crtc_state->has_sel_update) {
		/*
		 * [PSR2 without colorimetry]
		 * Prepare VSC Header for SU as per eDP 1.4 spec, Table 6-11
		 * 3D stereo + PSR/PSR2 + Y-coordinate.
		 */
		vsc->revision = 0x4;
		vsc->length = 0xe;
	} else {
		/*
		 * [PSR1]
		 * Prepare VSC Header for SU as per DP 1.4 spec, Table 2-118
		 * VSC SDP supporting 3D stereo + PSR (applies to eDP v1.3 or
		 * higher).
		 */
		vsc->revision = 0x2;
		vsc->length = 0x8;
	}
}

static void
intel_dp_compute_hdr_metadata_infoframe_sdp(struct intel_dp *intel_dp,
					    struct intel_crtc_state *crtc_state,
					    const struct drm_connector_state *conn_state)
{
	struct intel_display *display = to_intel_display(intel_dp);
	int ret;
	struct hdmi_drm_infoframe *drm_infoframe = &crtc_state->infoframes.drm.drm;

	if (!conn_state->hdr_output_metadata)
		return;

	ret = drm_hdmi_infoframe_set_hdr_metadata(drm_infoframe, conn_state);

	if (ret) {
		drm_dbg_kms(display->drm,
			    "couldn't set HDR metadata in infoframe\n");
		return;
	}

	crtc_state->infoframes.enable |=
		intel_hdmi_infoframe_enable(HDMI_PACKET_TYPE_GAMUT_METADATA);
}

static bool can_enable_drrs(struct intel_connector *connector,
			    const struct intel_crtc_state *pipe_config,
			    const struct drm_display_mode *downclock_mode)
{
	struct intel_display *display = to_intel_display(connector);

	if (pipe_config->vrr.enable)
		return false;

	/*
	 * DRRS and PSR can't be enable together, so giving preference to PSR
	 * as it allows more power-savings by complete shutting down display,
	 * so to guarantee this, intel_drrs_compute_config() must be called
	 * after intel_psr_compute_config().
	 */
	if (pipe_config->has_psr)
		return false;

	/* FIXME missing FDI M2/N2 etc. */
	if (pipe_config->has_pch_encoder)
		return false;

	if (!intel_cpu_transcoder_has_drrs(display, pipe_config->cpu_transcoder))
		return false;

	return downclock_mode &&
		intel_panel_drrs_type(connector) == DRRS_TYPE_SEAMLESS;
}

static void
intel_dp_drrs_compute_config(struct intel_connector *connector,
			     struct intel_crtc_state *pipe_config,
			     int link_bpp_x16)
{
	struct intel_display *display = to_intel_display(connector);
	const struct drm_display_mode *downclock_mode =
		intel_panel_downclock_mode(connector, &pipe_config->hw.adjusted_mode);
	int pixel_clock;

	/*
	 * FIXME all joined pipes share the same transcoder.
	 * Need to account for that when updating M/N live.
	 */
	if (has_seamless_m_n(connector) && !pipe_config->joiner_pipes)
		pipe_config->update_m_n = true;

	if (!can_enable_drrs(connector, pipe_config, downclock_mode)) {
		if (intel_cpu_transcoder_has_m2_n2(display, pipe_config->cpu_transcoder))
			intel_zero_m_n(&pipe_config->dp_m2_n2);
		return;
	}

	if (display->platform.ironlake || display->platform.sandybridge ||
	    display->platform.ivybridge)
		pipe_config->msa_timing_delay = connector->panel.vbt.edp.drrs_msa_timing_delay;

	pipe_config->has_drrs = true;

	pixel_clock = downclock_mode->clock;
	if (pipe_config->splitter.enable)
		pixel_clock /= pipe_config->splitter.link_count;

	intel_link_compute_m_n(link_bpp_x16, pipe_config->lane_count, pixel_clock,
			       pipe_config->port_clock,
			       intel_dp_bw_fec_overhead(pipe_config->fec_enable),
			       &pipe_config->dp_m2_n2);

	/* FIXME: abstract this better */
	if (pipe_config->splitter.enable)
		pipe_config->dp_m2_n2.data_m *= pipe_config->splitter.link_count;
}

static bool intel_dp_has_audio(struct intel_encoder *encoder,
			       const struct drm_connector_state *conn_state)
{
	struct intel_display *display = to_intel_display(encoder);
	const struct intel_digital_connector_state *intel_conn_state =
		to_intel_digital_connector_state(conn_state);
	struct intel_connector *connector =
		to_intel_connector(conn_state->connector);

	if (!intel_dp_port_has_audio(display, encoder->port))
		return false;

	if (intel_conn_state->force_audio == HDMI_AUDIO_AUTO)
		return connector->base.display_info.has_audio;
	else
		return intel_conn_state->force_audio == HDMI_AUDIO_ON;
}

static int
intel_dp_compute_output_format(struct intel_encoder *encoder,
			       struct intel_crtc_state *crtc_state,
			       struct drm_connector_state *conn_state,
			       bool respect_downstream_limits)
{
	struct intel_display *display = to_intel_display(encoder);
	struct intel_dp *intel_dp = enc_to_intel_dp(encoder);
	struct intel_connector *connector = intel_dp->attached_connector;
	const struct drm_display_info *info = &connector->base.display_info;
	const struct drm_display_mode *adjusted_mode = &crtc_state->hw.adjusted_mode;
	bool ycbcr_420_only;
	int ret;

	ycbcr_420_only = drm_mode_is_420_only(info, adjusted_mode);

	if (ycbcr_420_only && !connector->base.ycbcr_420_allowed) {
		drm_dbg_kms(display->drm,
			    "YCbCr 4:2:0 mode but YCbCr 4:2:0 output not possible. Falling back to RGB.\n");
		crtc_state->sink_format = INTEL_OUTPUT_FORMAT_RGB;
	} else {
		crtc_state->sink_format = intel_dp_sink_format(connector, adjusted_mode);
	}

	crtc_state->output_format = intel_dp_output_format(connector, crtc_state->sink_format);

	ret = intel_dp_compute_link_config(encoder, crtc_state, conn_state,
					   respect_downstream_limits);
	if (ret) {
		if (crtc_state->sink_format == INTEL_OUTPUT_FORMAT_YCBCR420 ||
		    !connector->base.ycbcr_420_allowed ||
		    !drm_mode_is_420_also(info, adjusted_mode))
			return ret;

		crtc_state->sink_format = INTEL_OUTPUT_FORMAT_YCBCR420;
		crtc_state->output_format = intel_dp_output_format(connector,
								   crtc_state->sink_format);
		ret = intel_dp_compute_link_config(encoder, crtc_state, conn_state,
						   respect_downstream_limits);
	}

	return ret;
}

void
intel_dp_audio_compute_config(struct intel_encoder *encoder,
			      struct intel_crtc_state *pipe_config,
			      struct drm_connector_state *conn_state)
{
	pipe_config->has_audio =
		intel_dp_has_audio(encoder, conn_state) &&
		intel_audio_compute_config(encoder, pipe_config, conn_state);

	pipe_config->sdp_split_enable = pipe_config->has_audio &&
					intel_dp_is_uhbr(pipe_config);
}

void
intel_dp_queue_modeset_retry_for_link(struct intel_atomic_state *state,
				      struct intel_encoder *encoder,
				      const struct intel_crtc_state *crtc_state)
{
	struct intel_connector *connector;
	struct intel_digital_connector_state *conn_state;
	struct intel_dp *intel_dp = enc_to_intel_dp(encoder);
	int i;

	if (intel_dp->needs_modeset_retry)
		return;

	intel_dp->needs_modeset_retry = true;

	if (!intel_crtc_has_type(crtc_state, INTEL_OUTPUT_DP_MST)) {
		intel_connector_queue_modeset_retry_work(intel_dp->attached_connector);

		return;
	}

	for_each_new_intel_connector_in_state(state, connector, conn_state, i) {
		if (!conn_state->base.crtc)
			continue;

		if (connector->mst.dp == intel_dp)
			intel_connector_queue_modeset_retry_work(connector);
	}
}

int intel_dp_compute_min_hblank(struct intel_crtc_state *crtc_state,
				const struct drm_connector_state *conn_state)
{
	struct intel_display *display = to_intel_display(crtc_state);
	const struct drm_display_mode *adjusted_mode =
					&crtc_state->hw.adjusted_mode;
	struct intel_connector *connector = to_intel_connector(conn_state->connector);
	int symbol_size = intel_dp_is_uhbr(crtc_state) ? 32 : 8;
	/*
	 * min symbol cycles is 3(BS,VBID, BE) for 128b/132b and
	 * 5(BS, VBID, MVID, MAUD, BE) for 8b/10b
	 */
	int min_sym_cycles = intel_dp_is_uhbr(crtc_state) ? 3 : 5;
	bool is_mst = intel_crtc_has_type(crtc_state, INTEL_OUTPUT_DP_MST);
	int num_joined_pipes = intel_crtc_num_joined_pipes(crtc_state);
	int min_hblank;
	int max_lane_count = 4;
	int hactive_sym_cycles, htotal_sym_cycles;
	int dsc_slices = 0;
	int link_bpp_x16;

	if (DISPLAY_VER(display) < 30)
		return 0;

	/* MIN_HBLANK should be set only for 8b/10b MST or for 128b/132b SST/MST */
	if (!is_mst && !intel_dp_is_uhbr(crtc_state))
		return 0;

	if (crtc_state->dsc.compression_enable) {
		dsc_slices = intel_dp_dsc_get_slice_count(connector,
							  adjusted_mode->crtc_clock,
							  adjusted_mode->crtc_hdisplay,
							  num_joined_pipes);
		if (!dsc_slices) {
			drm_dbg(display->drm, "failed to calculate dsc slice count\n");
			return -EINVAL;
		}
	}

	if (crtc_state->dsc.compression_enable)
		link_bpp_x16 = crtc_state->dsc.compressed_bpp_x16;
	else
		link_bpp_x16 = fxp_q4_from_int(intel_dp_output_bpp(crtc_state->output_format,
								   crtc_state->pipe_bpp));

	/* Calculate min Hblank Link Layer Symbol Cycle Count for 8b/10b MST & 128b/132b */
	hactive_sym_cycles = drm_dp_link_symbol_cycles(max_lane_count,
						       adjusted_mode->hdisplay,
						       dsc_slices,
						       link_bpp_x16,
						       symbol_size, is_mst);
	htotal_sym_cycles = adjusted_mode->htotal * hactive_sym_cycles /
			     adjusted_mode->hdisplay;

	min_hblank = htotal_sym_cycles - hactive_sym_cycles;
	/* minimum Hblank calculation: https://groups.vesa.org/wg/DP/document/20494 */
	min_hblank = max(min_hblank, min_sym_cycles);

	/*
	 * adjust the BlankingStart/BlankingEnd framing control from
	 * the calculated value
	 */
	min_hblank = min_hblank - 2;

	min_hblank = min(10, min_hblank);
	crtc_state->min_hblank = min_hblank;

	return 0;
}

int
intel_dp_compute_config(struct intel_encoder *encoder,
			struct intel_crtc_state *pipe_config,
			struct drm_connector_state *conn_state)
{
	struct intel_display *display = to_intel_display(encoder);
	struct intel_atomic_state *state = to_intel_atomic_state(conn_state->state);
	struct drm_display_mode *adjusted_mode = &pipe_config->hw.adjusted_mode;
	struct intel_dp *intel_dp = enc_to_intel_dp(encoder);
	const struct drm_display_mode *fixed_mode;
	struct intel_connector *connector = intel_dp->attached_connector;
	int ret = 0, link_bpp_x16;

	fixed_mode = intel_panel_fixed_mode(connector, adjusted_mode);
	if (intel_dp_is_edp(intel_dp) && fixed_mode) {
		ret = intel_panel_compute_config(connector, adjusted_mode);
		if (ret)
			return ret;
	}

	if (adjusted_mode->flags & DRM_MODE_FLAG_DBLSCAN)
		return -EINVAL;

	if (!connector->base.interlace_allowed &&
	    adjusted_mode->flags & DRM_MODE_FLAG_INTERLACE)
		return -EINVAL;

	if (adjusted_mode->flags & DRM_MODE_FLAG_DBLCLK)
		return -EINVAL;

	if (intel_dp_hdisplay_bad(display, adjusted_mode->crtc_hdisplay))
		return -EINVAL;

	/*
	 * Try to respect downstream TMDS clock limits first, if
	 * that fails assume the user might know something we don't.
	 */
	ret = intel_dp_compute_output_format(encoder, pipe_config, conn_state, true);
	if (ret)
		ret = intel_dp_compute_output_format(encoder, pipe_config, conn_state, false);
	if (ret)
		return ret;

	if ((intel_dp_is_edp(intel_dp) && fixed_mode) ||
	    pipe_config->output_format == INTEL_OUTPUT_FORMAT_YCBCR420) {
		ret = intel_pfit_compute_config(pipe_config, conn_state);
		if (ret)
			return ret;
	}

	pipe_config->limited_color_range =
		intel_dp_limited_color_range(pipe_config, conn_state);

	if (intel_dp_is_uhbr(pipe_config)) {
		/* 128b/132b SST also needs this */
		pipe_config->mst_master_transcoder = pipe_config->cpu_transcoder;
	} else {
		pipe_config->enhanced_framing =
			drm_dp_enhanced_frame_cap(intel_dp->dpcd);
	}

	if (pipe_config->dsc.compression_enable)
		link_bpp_x16 = pipe_config->dsc.compressed_bpp_x16;
	else
		link_bpp_x16 = fxp_q4_from_int(intel_dp_output_bpp(pipe_config->output_format,
								   pipe_config->pipe_bpp));

	if (intel_dp->mso_link_count) {
		int n = intel_dp->mso_link_count;
		int overlap = intel_dp->mso_pixel_overlap;

		pipe_config->splitter.enable = true;
		pipe_config->splitter.link_count = n;
		pipe_config->splitter.pixel_overlap = overlap;

		drm_dbg_kms(display->drm,
			    "MSO link count %d, pixel overlap %d\n",
			    n, overlap);

		adjusted_mode->crtc_hdisplay = adjusted_mode->crtc_hdisplay / n + overlap;
		adjusted_mode->crtc_hblank_start = adjusted_mode->crtc_hblank_start / n + overlap;
		adjusted_mode->crtc_hblank_end = adjusted_mode->crtc_hblank_end / n + overlap;
		adjusted_mode->crtc_hsync_start = adjusted_mode->crtc_hsync_start / n + overlap;
		adjusted_mode->crtc_hsync_end = adjusted_mode->crtc_hsync_end / n + overlap;
		adjusted_mode->crtc_htotal = adjusted_mode->crtc_htotal / n + overlap;
		adjusted_mode->crtc_clock /= n;
	}

	intel_dp_audio_compute_config(encoder, pipe_config, conn_state);

	if (!intel_dp_is_uhbr(pipe_config)) {
		intel_link_compute_m_n(link_bpp_x16,
				       pipe_config->lane_count,
				       adjusted_mode->crtc_clock,
				       pipe_config->port_clock,
				       intel_dp_bw_fec_overhead(pipe_config->fec_enable),
				       &pipe_config->dp_m_n);
	}

	ret = intel_dp_compute_min_hblank(pipe_config, conn_state);
	if (ret)
		return ret;

	/* FIXME: abstract this better */
	if (pipe_config->splitter.enable)
		pipe_config->dp_m_n.data_m *= pipe_config->splitter.link_count;

	intel_vrr_compute_config(pipe_config, conn_state);
	intel_dp_compute_as_sdp(intel_dp, pipe_config);
	intel_psr_compute_config(intel_dp, pipe_config, conn_state);
	intel_alpm_lobf_compute_config(intel_dp, pipe_config, conn_state);
	intel_dp_drrs_compute_config(connector, pipe_config, link_bpp_x16);
	intel_dp_compute_vsc_sdp(intel_dp, pipe_config, conn_state);
	intel_dp_compute_hdr_metadata_infoframe_sdp(intel_dp, pipe_config, conn_state);

	return intel_dp_tunnel_atomic_compute_stream_bw(state, intel_dp, connector,
							pipe_config);
}

void intel_dp_set_link_params(struct intel_dp *intel_dp,
			      int link_rate, int lane_count)
{
	memset(intel_dp->train_set, 0, sizeof(intel_dp->train_set));
	intel_dp->link.active = false;
	intel_dp->needs_modeset_retry = false;
	intel_dp->link_rate = link_rate;
	intel_dp->lane_count = lane_count;
}

void intel_dp_reset_link_params(struct intel_dp *intel_dp)
{
	intel_dp->link.max_lane_count = intel_dp_max_common_lane_count(intel_dp);
	intel_dp->link.max_rate = intel_dp_max_common_rate(intel_dp);
	intel_dp->link.mst_probed_lane_count = 0;
	intel_dp->link.mst_probed_rate = 0;
	intel_dp->link.retrain_disabled = false;
	intel_dp->link.seq_train_failures = 0;
}

/* Enable backlight PWM and backlight PP control. */
void intel_edp_backlight_on(const struct intel_crtc_state *crtc_state,
			    const struct drm_connector_state *conn_state)
{
	struct intel_display *display = to_intel_display(crtc_state);
	struct intel_dp *intel_dp = enc_to_intel_dp(to_intel_encoder(conn_state->best_encoder));

	if (!intel_dp_is_edp(intel_dp))
		return;

	drm_dbg_kms(display->drm, "\n");

	intel_backlight_enable(crtc_state, conn_state);
	intel_pps_backlight_on(intel_dp);
}

/* Disable backlight PP control and backlight PWM. */
void intel_edp_backlight_off(const struct drm_connector_state *old_conn_state)
{
	struct intel_dp *intel_dp = enc_to_intel_dp(to_intel_encoder(old_conn_state->best_encoder));
	struct intel_display *display = to_intel_display(intel_dp);

	if (!intel_dp_is_edp(intel_dp))
		return;

	drm_dbg_kms(display->drm, "\n");

	intel_pps_backlight_off(intel_dp);
	intel_backlight_disable(old_conn_state);
}

static bool downstream_hpd_needs_d0(struct intel_dp *intel_dp)
{
	/*
	 * DPCD 1.2+ should support BRANCH_DEVICE_CTRL, and thus
	 * be capable of signalling downstream hpd with a long pulse.
	 * Whether or not that means D3 is safe to use is not clear,
	 * but let's assume so until proven otherwise.
	 *
	 * FIXME should really check all downstream ports...
	 */
	return intel_dp->dpcd[DP_DPCD_REV] == 0x11 &&
		drm_dp_is_branch(intel_dp->dpcd) &&
		intel_dp->downstream_ports[0] & DP_DS_PORT_HPD;
}

static int
write_dsc_decompression_flag(struct drm_dp_aux *aux, u8 flag, bool set)
{
	int err;
	u8 val;

	err = drm_dp_dpcd_readb(aux, DP_DSC_ENABLE, &val);
	if (err < 0)
		return err;

	if (set)
		val |= flag;
	else
		val &= ~flag;

	return drm_dp_dpcd_writeb(aux, DP_DSC_ENABLE, val);
}

static void
intel_dp_sink_set_dsc_decompression(struct intel_connector *connector,
				    bool enable)
{
	struct intel_display *display = to_intel_display(connector);

	if (write_dsc_decompression_flag(connector->dp.dsc_decompression_aux,
					 DP_DECOMPRESSION_EN, enable) < 0)
		drm_dbg_kms(display->drm,
			    "Failed to %s sink decompression state\n",
			    str_enable_disable(enable));
}

static void
intel_dp_sink_set_dsc_passthrough(const struct intel_connector *connector,
				  bool enable)
{
	struct intel_display *display = to_intel_display(connector);
	struct drm_dp_aux *aux = connector->mst.port ?
				 connector->mst.port->passthrough_aux : NULL;

	if (!aux)
		return;

	if (write_dsc_decompression_flag(aux,
					 DP_DSC_PASSTHROUGH_EN, enable) < 0)
		drm_dbg_kms(display->drm,
			    "Failed to %s sink compression passthrough state\n",
			    str_enable_disable(enable));
}

static int intel_dp_dsc_aux_ref_count(struct intel_atomic_state *state,
				      const struct intel_connector *connector,
				      bool for_get_ref)
{
	struct intel_display *display = to_intel_display(state);
	struct drm_connector *_connector_iter;
	struct drm_connector_state *old_conn_state;
	struct drm_connector_state *new_conn_state;
	int ref_count = 0;
	int i;

	/*
	 * On SST the decompression AUX device won't be shared, each connector
	 * uses for this its own AUX targeting the sink device.
	 */
	if (!connector->mst.dp)
		return connector->dp.dsc_decompression_enabled ? 1 : 0;

	for_each_oldnew_connector_in_state(&state->base, _connector_iter,
					   old_conn_state, new_conn_state, i) {
		const struct intel_connector *
			connector_iter = to_intel_connector(_connector_iter);

		if (connector_iter->mst.dp != connector->mst.dp)
			continue;

		if (!connector_iter->dp.dsc_decompression_enabled)
			continue;

		drm_WARN_ON(display->drm,
			    (for_get_ref && !new_conn_state->crtc) ||
			    (!for_get_ref && !old_conn_state->crtc));

		if (connector_iter->dp.dsc_decompression_aux ==
		    connector->dp.dsc_decompression_aux)
			ref_count++;
	}

	return ref_count;
}

static bool intel_dp_dsc_aux_get_ref(struct intel_atomic_state *state,
				     struct intel_connector *connector)
{
	bool ret = intel_dp_dsc_aux_ref_count(state, connector, true) == 0;

	connector->dp.dsc_decompression_enabled = true;

	return ret;
}

static bool intel_dp_dsc_aux_put_ref(struct intel_atomic_state *state,
				     struct intel_connector *connector)
{
	connector->dp.dsc_decompression_enabled = false;

	return intel_dp_dsc_aux_ref_count(state, connector, false) == 0;
}

/**
 * intel_dp_sink_enable_decompression - Enable DSC decompression in sink/last branch device
 * @state: atomic state
 * @connector: connector to enable the decompression for
 * @new_crtc_state: new state for the CRTC driving @connector
 *
 * Enable the DSC decompression if required in the %DP_DSC_ENABLE DPCD
 * register of the appropriate sink/branch device. On SST this is always the
 * sink device, whereas on MST based on each device's DSC capabilities it's
 * either the last branch device (enabling decompression in it) or both the
 * last branch device (enabling passthrough in it) and the sink device
 * (enabling decompression in it).
 */
void intel_dp_sink_enable_decompression(struct intel_atomic_state *state,
					struct intel_connector *connector,
					const struct intel_crtc_state *new_crtc_state)
{
	struct intel_display *display = to_intel_display(state);

	if (!new_crtc_state->dsc.compression_enable)
		return;

	if (drm_WARN_ON(display->drm,
			!connector->dp.dsc_decompression_aux ||
			connector->dp.dsc_decompression_enabled))
		return;

	if (!intel_dp_dsc_aux_get_ref(state, connector))
		return;

	intel_dp_sink_set_dsc_passthrough(connector, true);
	intel_dp_sink_set_dsc_decompression(connector, true);
}

/**
 * intel_dp_sink_disable_decompression - Disable DSC decompression in sink/last branch device
 * @state: atomic state
 * @connector: connector to disable the decompression for
 * @old_crtc_state: old state for the CRTC driving @connector
 *
 * Disable the DSC decompression if required in the %DP_DSC_ENABLE DPCD
 * register of the appropriate sink/branch device, corresponding to the
 * sequence in intel_dp_sink_enable_decompression().
 */
void intel_dp_sink_disable_decompression(struct intel_atomic_state *state,
					 struct intel_connector *connector,
					 const struct intel_crtc_state *old_crtc_state)
{
	struct intel_display *display = to_intel_display(state);

	if (!old_crtc_state->dsc.compression_enable)
		return;

	if (drm_WARN_ON(display->drm,
			!connector->dp.dsc_decompression_aux ||
			!connector->dp.dsc_decompression_enabled))
		return;

	if (!intel_dp_dsc_aux_put_ref(state, connector))
		return;

	intel_dp_sink_set_dsc_decompression(connector, false);
	intel_dp_sink_set_dsc_passthrough(connector, false);
}

static void
intel_dp_init_source_oui(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	u8 oui[] = { 0x00, 0xaa, 0x01 };
	u8 buf[3] = {};

	if (READ_ONCE(intel_dp->oui_valid))
		return;

	WRITE_ONCE(intel_dp->oui_valid, true);

	/*
	 * During driver init, we want to be careful and avoid changing the source OUI if it's
	 * already set to what we want, so as to avoid clearing any state by accident
	 */
	if (drm_dp_dpcd_read(&intel_dp->aux, DP_SOURCE_OUI, buf, sizeof(buf)) < 0)
		drm_dbg_kms(display->drm, "Failed to read source OUI\n");

	if (memcmp(oui, buf, sizeof(oui)) == 0) {
		/* Assume the OUI was written now. */
		intel_dp->last_oui_write = jiffies;
		return;
	}

	if (drm_dp_dpcd_write(&intel_dp->aux, DP_SOURCE_OUI, oui, sizeof(oui)) < 0) {
		drm_dbg_kms(display->drm, "Failed to write source OUI\n");
		WRITE_ONCE(intel_dp->oui_valid, false);
	}

	intel_dp->last_oui_write = jiffies;
}

void intel_dp_invalidate_source_oui(struct intel_dp *intel_dp)
{
	WRITE_ONCE(intel_dp->oui_valid, false);
}

void intel_dp_wait_source_oui(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_connector *connector = intel_dp->attached_connector;

	drm_dbg_kms(display->drm,
		    "[CONNECTOR:%d:%s] Performing OUI wait (%u ms)\n",
		    connector->base.base.id, connector->base.name,
		    connector->panel.vbt.backlight.hdr_dpcd_refresh_timeout);

	wait_remaining_ms_from_jiffies(intel_dp->last_oui_write,
				       connector->panel.vbt.backlight.hdr_dpcd_refresh_timeout);
}

/* If the device supports it, try to set the power state appropriately */
void intel_dp_set_power(struct intel_dp *intel_dp, u8 mode)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_encoder *encoder = &dp_to_dig_port(intel_dp)->base;
	int ret, i;

	/* Should have a valid DPCD by this point */
	if (intel_dp->dpcd[DP_DPCD_REV] < 0x11)
		return;

	if (mode != DP_SET_POWER_D0) {
		if (downstream_hpd_needs_d0(intel_dp))
			return;

		ret = drm_dp_dpcd_writeb(&intel_dp->aux, DP_SET_POWER, mode);
	} else {
		struct intel_digital_port *dig_port = dp_to_dig_port(intel_dp);

		intel_lspcon_resume(dig_port);

		/* Write the source OUI as early as possible */
		intel_dp_init_source_oui(intel_dp);

		/*
		 * When turning on, we need to retry for 1ms to give the sink
		 * time to wake up.
		 */
		for (i = 0; i < 3; i++) {
			ret = drm_dp_dpcd_writeb(&intel_dp->aux, DP_SET_POWER, mode);
			if (ret == 1)
				break;
			msleep(1);
		}

		if (ret == 1 && intel_lspcon_active(dig_port))
			intel_lspcon_wait_pcon_mode(dig_port);
	}

	if (ret != 1)
		drm_dbg_kms(display->drm,
			    "[ENCODER:%d:%s] Set power to %s failed\n",
			    encoder->base.base.id, encoder->base.name,
			    mode == DP_SET_POWER_D0 ? "D0" : "D3");
}

static bool
intel_dp_get_dpcd(struct intel_dp *intel_dp);

/**
 * intel_dp_sync_state - sync the encoder state during init/resume
 * @encoder: intel encoder to sync
 * @crtc_state: state for the CRTC connected to the encoder
 *
 * Sync any state stored in the encoder wrt. HW state during driver init
 * and system resume.
 */
void intel_dp_sync_state(struct intel_encoder *encoder,
			 const struct intel_crtc_state *crtc_state)
{
	struct intel_dp *intel_dp = enc_to_intel_dp(encoder);
	bool dpcd_updated = false;

	/*
	 * Don't clobber DPCD if it's been already read out during output
	 * setup (eDP) or detect.
	 */
	if (crtc_state && intel_dp->dpcd[DP_DPCD_REV] == 0) {
		intel_dp_get_dpcd(intel_dp);
		dpcd_updated = true;
	}

	intel_dp_tunnel_resume(intel_dp, crtc_state, dpcd_updated);

	if (crtc_state) {
		intel_dp_reset_link_params(intel_dp);
		intel_dp_set_link_params(intel_dp, crtc_state->port_clock, crtc_state->lane_count);
		intel_dp->link.active = true;
	}
}

bool intel_dp_initial_fastset_check(struct intel_encoder *encoder,
				    struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(encoder);
	struct intel_dp *intel_dp = enc_to_intel_dp(encoder);
	bool fastset = true;

	/*
	 * If BIOS has set an unsupported or non-standard link rate for some
	 * reason force an encoder recompute and full modeset.
	 */
	if (intel_dp_rate_index(intel_dp->source_rates, intel_dp->num_source_rates,
				crtc_state->port_clock) < 0) {
		drm_dbg_kms(display->drm,
			    "[ENCODER:%d:%s] Forcing full modeset due to unsupported link rate\n",
			    encoder->base.base.id, encoder->base.name);
		crtc_state->uapi.connectors_changed = true;
		fastset = false;
	}

	/*
	 * FIXME hack to force full modeset when DSC is being used.
	 *
	 * As long as we do not have full state readout and config comparison
	 * of crtc_state->dsc, we have no way to ensure reliable fastset.
	 * Remove once we have readout for DSC.
	 */
	if (crtc_state->dsc.compression_enable) {
		drm_dbg_kms(display->drm,
			    "[ENCODER:%d:%s] Forcing full modeset due to DSC being enabled\n",
			    encoder->base.base.id, encoder->base.name);
		crtc_state->uapi.mode_changed = true;
		fastset = false;
	}

	if (CAN_PANEL_REPLAY(intel_dp)) {
		drm_dbg_kms(display->drm,
			    "[ENCODER:%d:%s] Forcing full modeset to compute panel replay state\n",
			    encoder->base.base.id, encoder->base.name);
		crtc_state->uapi.mode_changed = true;
		fastset = false;
	}

	return fastset;
}

static void intel_dp_get_pcon_dsc_cap(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);

	/* Clear the cached register set to avoid using stale values */

	memset(intel_dp->pcon_dsc_dpcd, 0, sizeof(intel_dp->pcon_dsc_dpcd));

	if (!drm_dp_is_branch(intel_dp->dpcd))
		return;

	if (drm_dp_dpcd_read(&intel_dp->aux, DP_PCON_DSC_ENCODER,
			     intel_dp->pcon_dsc_dpcd,
			     sizeof(intel_dp->pcon_dsc_dpcd)) < 0)
		drm_err(display->drm, "Failed to read DPCD register 0x%x\n",
			DP_PCON_DSC_ENCODER);

	drm_dbg_kms(display->drm, "PCON ENCODER DSC DPCD: %*ph\n",
		    (int)sizeof(intel_dp->pcon_dsc_dpcd), intel_dp->pcon_dsc_dpcd);
}

static int intel_dp_pcon_get_frl_mask(u8 frl_bw_mask)
{
	static const int bw_gbps[] = {9, 18, 24, 32, 40, 48};
	int i;

	for (i = ARRAY_SIZE(bw_gbps) - 1; i >= 0; i--) {
		if (frl_bw_mask & (1 << i))
			return bw_gbps[i];
	}
	return 0;
}

static int intel_dp_pcon_set_frl_mask(int max_frl)
{
	switch (max_frl) {
	case 48:
		return DP_PCON_FRL_BW_MASK_48GBPS;
	case 40:
		return DP_PCON_FRL_BW_MASK_40GBPS;
	case 32:
		return DP_PCON_FRL_BW_MASK_32GBPS;
	case 24:
		return DP_PCON_FRL_BW_MASK_24GBPS;
	case 18:
		return DP_PCON_FRL_BW_MASK_18GBPS;
	case 9:
		return DP_PCON_FRL_BW_MASK_9GBPS;
	}

	return 0;
}

static int intel_dp_hdmi_sink_max_frl(struct intel_dp *intel_dp)
{
	struct intel_connector *connector = intel_dp->attached_connector;
	const struct drm_display_info *info = &connector->base.display_info;
	int max_frl_rate;
	int max_lanes, rate_per_lane;
	int max_dsc_lanes, dsc_rate_per_lane;

	max_lanes = info->hdmi.max_lanes;
	rate_per_lane = info->hdmi.max_frl_rate_per_lane;
	max_frl_rate = max_lanes * rate_per_lane;

	if (info->hdmi.dsc_cap.v_1p2) {
		max_dsc_lanes = info->hdmi.dsc_cap.max_lanes;
		dsc_rate_per_lane = info->hdmi.dsc_cap.max_frl_rate_per_lane;
		if (max_dsc_lanes && dsc_rate_per_lane)
			max_frl_rate = min(max_frl_rate, max_dsc_lanes * dsc_rate_per_lane);
	}

	return max_frl_rate;
}

static bool
intel_dp_pcon_is_frl_trained(struct intel_dp *intel_dp,
			     u8 max_frl_bw_mask, u8 *frl_trained_mask)
{
	if (drm_dp_pcon_hdmi_link_active(&intel_dp->aux) &&
	    drm_dp_pcon_hdmi_link_mode(&intel_dp->aux, frl_trained_mask) == DP_PCON_HDMI_MODE_FRL &&
	    *frl_trained_mask >= max_frl_bw_mask)
		return true;

	return false;
}

static int intel_dp_pcon_start_frl_training(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
#define TIMEOUT_FRL_READY_MS 500
#define TIMEOUT_HDMI_LINK_ACTIVE_MS 1000
	int max_frl_bw, max_pcon_frl_bw, max_edid_frl_bw, ret;
	u8 max_frl_bw_mask = 0, frl_trained_mask;
	bool is_active;

	max_pcon_frl_bw = intel_dp->dfp.pcon_max_frl_bw;
	drm_dbg(display->drm, "PCON max rate = %d Gbps\n", max_pcon_frl_bw);

	max_edid_frl_bw = intel_dp_hdmi_sink_max_frl(intel_dp);
	drm_dbg(display->drm, "Sink max rate from EDID = %d Gbps\n",
		max_edid_frl_bw);

	max_frl_bw = min(max_edid_frl_bw, max_pcon_frl_bw);

	if (max_frl_bw <= 0)
		return -EINVAL;

	max_frl_bw_mask = intel_dp_pcon_set_frl_mask(max_frl_bw);
	drm_dbg(display->drm, "MAX_FRL_BW_MASK = %u\n", max_frl_bw_mask);

	if (intel_dp_pcon_is_frl_trained(intel_dp, max_frl_bw_mask, &frl_trained_mask))
		goto frl_trained;

	ret = drm_dp_pcon_frl_prepare(&intel_dp->aux, false);
	if (ret < 0)
		return ret;
	/* Wait for PCON to be FRL Ready */
	wait_for(is_active = drm_dp_pcon_is_frl_ready(&intel_dp->aux) == true, TIMEOUT_FRL_READY_MS);

	if (!is_active)
		return -ETIMEDOUT;

	ret = drm_dp_pcon_frl_configure_1(&intel_dp->aux, max_frl_bw,
					  DP_PCON_ENABLE_SEQUENTIAL_LINK);
	if (ret < 0)
		return ret;
	ret = drm_dp_pcon_frl_configure_2(&intel_dp->aux, max_frl_bw_mask,
					  DP_PCON_FRL_LINK_TRAIN_NORMAL);
	if (ret < 0)
		return ret;
	ret = drm_dp_pcon_frl_enable(&intel_dp->aux);
	if (ret < 0)
		return ret;
	/*
	 * Wait for FRL to be completed
	 * Check if the HDMI Link is up and active.
	 */
	wait_for(is_active =
		 intel_dp_pcon_is_frl_trained(intel_dp, max_frl_bw_mask, &frl_trained_mask),
		 TIMEOUT_HDMI_LINK_ACTIVE_MS);

	if (!is_active)
		return -ETIMEDOUT;

frl_trained:
	drm_dbg(display->drm, "FRL_TRAINED_MASK = %u\n", frl_trained_mask);
	intel_dp->frl.trained_rate_gbps = intel_dp_pcon_get_frl_mask(frl_trained_mask);
	intel_dp->frl.is_trained = true;
	drm_dbg(display->drm, "FRL trained with : %d Gbps\n",
		intel_dp->frl.trained_rate_gbps);

	return 0;
}

static bool intel_dp_is_hdmi_2_1_sink(struct intel_dp *intel_dp)
{
	if (drm_dp_is_branch(intel_dp->dpcd) &&
	    intel_dp_has_hdmi_sink(intel_dp) &&
	    intel_dp_hdmi_sink_max_frl(intel_dp) > 0)
		return true;

	return false;
}

static
int intel_dp_pcon_set_tmds_mode(struct intel_dp *intel_dp)
{
	int ret;
	u8 buf = 0;

	/* Set PCON source control mode */
	buf |= DP_PCON_ENABLE_SOURCE_CTL_MODE;

	ret = drm_dp_dpcd_writeb(&intel_dp->aux, DP_PCON_HDMI_LINK_CONFIG_1, buf);
	if (ret < 0)
		return ret;

	/* Set HDMI LINK ENABLE */
	buf |= DP_PCON_ENABLE_HDMI_LINK;
	ret = drm_dp_dpcd_writeb(&intel_dp->aux, DP_PCON_HDMI_LINK_CONFIG_1, buf);
	if (ret < 0)
		return ret;

	return 0;
}

void intel_dp_check_frl_training(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);

	/*
	 * Always go for FRL training if:
	 * -PCON supports SRC_CTL_MODE (VESA DP2.0-HDMI2.1 PCON Spec Draft-1 Sec-7)
	 * -sink is HDMI2.1
	 */
	if (!(intel_dp->downstream_ports[2] & DP_PCON_SOURCE_CTL_MODE) ||
	    !intel_dp_is_hdmi_2_1_sink(intel_dp) ||
	    intel_dp->frl.is_trained)
		return;

	if (intel_dp_pcon_start_frl_training(intel_dp) < 0) {
		int ret, mode;

		drm_dbg(display->drm,
			"Couldn't set FRL mode, continuing with TMDS mode\n");
		ret = intel_dp_pcon_set_tmds_mode(intel_dp);
		mode = drm_dp_pcon_hdmi_link_mode(&intel_dp->aux, NULL);

		if (ret < 0 || mode != DP_PCON_HDMI_MODE_TMDS)
			drm_dbg(display->drm,
				"Issue with PCON, cannot set TMDS mode\n");
	} else {
		drm_dbg(display->drm, "FRL training Completed\n");
	}
}

static int
intel_dp_pcon_dsc_enc_slice_height(const struct intel_crtc_state *crtc_state)
{
	int vactive = crtc_state->hw.adjusted_mode.vdisplay;

	return intel_hdmi_dsc_get_slice_height(vactive);
}

static int
intel_dp_pcon_dsc_enc_slices(struct intel_dp *intel_dp,
			     const struct intel_crtc_state *crtc_state)
{
	struct intel_connector *connector = intel_dp->attached_connector;
	const struct drm_display_info *info = &connector->base.display_info;
	int hdmi_throughput = info->hdmi.dsc_cap.clk_per_slice;
	int hdmi_max_slices = info->hdmi.dsc_cap.max_slices;
	int pcon_max_slices = drm_dp_pcon_dsc_max_slices(intel_dp->pcon_dsc_dpcd);
	int pcon_max_slice_width = drm_dp_pcon_dsc_max_slice_width(intel_dp->pcon_dsc_dpcd);

	return intel_hdmi_dsc_get_num_slices(crtc_state, pcon_max_slices,
					     pcon_max_slice_width,
					     hdmi_max_slices, hdmi_throughput);
}

static int
intel_dp_pcon_dsc_enc_bpp(struct intel_dp *intel_dp,
			  const struct intel_crtc_state *crtc_state,
			  int num_slices, int slice_width)
{
	struct intel_connector *connector = intel_dp->attached_connector;
	const struct drm_display_info *info = &connector->base.display_info;
	int output_format = crtc_state->output_format;
	bool hdmi_all_bpp = info->hdmi.dsc_cap.all_bpp;
	int pcon_fractional_bpp = drm_dp_pcon_dsc_bpp_incr(intel_dp->pcon_dsc_dpcd);
	int hdmi_max_chunk_bytes =
		info->hdmi.dsc_cap.total_chunk_kbytes * 1024;

	return intel_hdmi_dsc_get_bpp(pcon_fractional_bpp, slice_width,
				      num_slices, output_format, hdmi_all_bpp,
				      hdmi_max_chunk_bytes);
}

void
intel_dp_pcon_dsc_configure(struct intel_dp *intel_dp,
			    const struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_connector *connector = intel_dp->attached_connector;
	const struct drm_display_info *info;
	u8 pps_param[6];
	int slice_height;
	int slice_width;
	int num_slices;
	int bits_per_pixel;
	int ret;
	bool hdmi_is_dsc_1_2;

	if (!intel_dp_is_hdmi_2_1_sink(intel_dp))
		return;

	if (!connector)
		return;

	info = &connector->base.display_info;

	hdmi_is_dsc_1_2 = info->hdmi.dsc_cap.v_1p2;

	if (!drm_dp_pcon_enc_is_dsc_1_2(intel_dp->pcon_dsc_dpcd) ||
	    !hdmi_is_dsc_1_2)
		return;

	slice_height = intel_dp_pcon_dsc_enc_slice_height(crtc_state);
	if (!slice_height)
		return;

	num_slices = intel_dp_pcon_dsc_enc_slices(intel_dp, crtc_state);
	if (!num_slices)
		return;

	slice_width = DIV_ROUND_UP(crtc_state->hw.adjusted_mode.hdisplay,
				   num_slices);

	bits_per_pixel = intel_dp_pcon_dsc_enc_bpp(intel_dp, crtc_state,
						   num_slices, slice_width);
	if (!bits_per_pixel)
		return;

	pps_param[0] = slice_height & 0xFF;
	pps_param[1] = slice_height >> 8;
	pps_param[2] = slice_width & 0xFF;
	pps_param[3] = slice_width >> 8;
	pps_param[4] = bits_per_pixel & 0xFF;
	pps_param[5] = (bits_per_pixel >> 8) & 0x3;

	ret = drm_dp_pcon_pps_override_param(&intel_dp->aux, pps_param);
	if (ret < 0)
		drm_dbg_kms(display->drm, "Failed to set pcon DSC\n");
}

void intel_dp_configure_protocol_converter(struct intel_dp *intel_dp,
					   const struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(intel_dp);
	bool ycbcr444_to_420 = false;
	bool rgb_to_ycbcr = false;
	u8 tmp;

	if (intel_dp->dpcd[DP_DPCD_REV] < 0x13)
		return;

	if (!drm_dp_is_branch(intel_dp->dpcd))
		return;

	tmp = intel_dp_has_hdmi_sink(intel_dp) ? DP_HDMI_DVI_OUTPUT_CONFIG : 0;

	if (drm_dp_dpcd_writeb(&intel_dp->aux,
			       DP_PROTOCOL_CONVERTER_CONTROL_0, tmp) != 1)
		drm_dbg_kms(display->drm,
			    "Failed to %s protocol converter HDMI mode\n",
			    str_enable_disable(intel_dp_has_hdmi_sink(intel_dp)));

	if (crtc_state->sink_format == INTEL_OUTPUT_FORMAT_YCBCR420) {
		switch (crtc_state->output_format) {
		case INTEL_OUTPUT_FORMAT_YCBCR420:
			break;
		case INTEL_OUTPUT_FORMAT_YCBCR444:
			ycbcr444_to_420 = true;
			break;
		case INTEL_OUTPUT_FORMAT_RGB:
			rgb_to_ycbcr = true;
			ycbcr444_to_420 = true;
			break;
		default:
			MISSING_CASE(crtc_state->output_format);
			break;
		}
	} else if (crtc_state->sink_format == INTEL_OUTPUT_FORMAT_YCBCR444) {
		switch (crtc_state->output_format) {
		case INTEL_OUTPUT_FORMAT_YCBCR444:
			break;
		case INTEL_OUTPUT_FORMAT_RGB:
			rgb_to_ycbcr = true;
			break;
		default:
			MISSING_CASE(crtc_state->output_format);
			break;
		}
	}

	tmp = ycbcr444_to_420 ? DP_CONVERSION_TO_YCBCR420_ENABLE : 0;

	if (drm_dp_dpcd_writeb(&intel_dp->aux,
			       DP_PROTOCOL_CONVERTER_CONTROL_1, tmp) != 1)
		drm_dbg_kms(display->drm,
			    "Failed to %s protocol converter YCbCr 4:2:0 conversion mode\n",
			    str_enable_disable(intel_dp->dfp.ycbcr_444_to_420));

	tmp = rgb_to_ycbcr ? DP_CONVERSION_BT709_RGB_YCBCR_ENABLE : 0;

	if (drm_dp_pcon_convert_rgb_to_ycbcr(&intel_dp->aux, tmp) < 0)
		drm_dbg_kms(display->drm,
			    "Failed to %s protocol converter RGB->YCbCr conversion mode\n",
			    str_enable_disable(tmp));
}

static bool intel_dp_get_colorimetry_status(struct intel_dp *intel_dp)
{
	u8 dprx = 0;

	if (drm_dp_dpcd_readb(&intel_dp->aux, DP_DPRX_FEATURE_ENUMERATION_LIST,
			      &dprx) != 1)
		return false;
	return dprx & DP_VSC_SDP_EXT_FOR_COLORIMETRY_SUPPORTED;
}

static void intel_dp_read_dsc_dpcd(struct drm_dp_aux *aux,
				   u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE])
{
	if (drm_dp_dpcd_read(aux, DP_DSC_SUPPORT, dsc_dpcd,
			     DP_DSC_RECEIVER_CAP_SIZE) < 0) {
		drm_err(aux->drm_dev,
			"Failed to read DPCD register 0x%x\n",
			DP_DSC_SUPPORT);
		return;
	}

	drm_dbg_kms(aux->drm_dev, "DSC DPCD: %*ph\n",
		    DP_DSC_RECEIVER_CAP_SIZE,
		    dsc_dpcd);
}

void intel_dp_get_dsc_sink_cap(u8 dpcd_rev, struct intel_connector *connector)
{
	struct intel_display *display = to_intel_display(connector);

	/*
	 * Clear the cached register set to avoid using stale values
	 * for the sinks that do not support DSC.
	 */
	memset(connector->dp.dsc_dpcd, 0, sizeof(connector->dp.dsc_dpcd));

	/* Clear fec_capable to avoid using stale values */
	connector->dp.fec_capability = 0;

	if (dpcd_rev < DP_DPCD_REV_14)
		return;

	intel_dp_read_dsc_dpcd(connector->dp.dsc_decompression_aux,
			       connector->dp.dsc_dpcd);

	if (drm_dp_dpcd_readb(connector->dp.dsc_decompression_aux, DP_FEC_CAPABILITY,
			      &connector->dp.fec_capability) < 0) {
		drm_err(display->drm, "Failed to read FEC DPCD register\n");
		return;
	}

	drm_dbg_kms(display->drm, "FEC CAPABILITY: %x\n",
		    connector->dp.fec_capability);
}

static void intel_edp_get_dsc_sink_cap(u8 edp_dpcd_rev, struct intel_connector *connector)
{
	if (edp_dpcd_rev < DP_EDP_14)
		return;

	intel_dp_read_dsc_dpcd(connector->dp.dsc_decompression_aux, connector->dp.dsc_dpcd);
}

static void
intel_dp_detect_dsc_caps(struct intel_dp *intel_dp, struct intel_connector *connector)
{
	struct intel_display *display = to_intel_display(intel_dp);

	/* Read DP Sink DSC Cap DPCD regs for DP v1.4 */
	if (!HAS_DSC(display))
		return;

	if (intel_dp_is_edp(intel_dp))
		intel_edp_get_dsc_sink_cap(intel_dp->edp_dpcd[0],
					   connector);
	else
		intel_dp_get_dsc_sink_cap(intel_dp->dpcd[DP_DPCD_REV],
					  connector);
}

static void intel_edp_mso_mode_fixup(struct intel_connector *connector,
				     struct drm_display_mode *mode)
{
	struct intel_display *display = to_intel_display(connector);
	struct intel_dp *intel_dp = intel_attached_dp(connector);
	int n = intel_dp->mso_link_count;
	int overlap = intel_dp->mso_pixel_overlap;

	if (!mode || !n)
		return;

	mode->hdisplay = (mode->hdisplay - overlap) * n;
	mode->hsync_start = (mode->hsync_start - overlap) * n;
	mode->hsync_end = (mode->hsync_end - overlap) * n;
	mode->htotal = (mode->htotal - overlap) * n;
	mode->clock *= n;

	drm_mode_set_name(mode);

	drm_dbg_kms(display->drm,
		    "[CONNECTOR:%d:%s] using generated MSO mode: " DRM_MODE_FMT "\n",
		    connector->base.base.id, connector->base.name,
		    DRM_MODE_ARG(mode));
}

void intel_edp_fixup_vbt_bpp(struct intel_encoder *encoder, int pipe_bpp)
{
	struct intel_display *display = to_intel_display(encoder);
	struct intel_dp *intel_dp = enc_to_intel_dp(encoder);
	struct intel_connector *connector = intel_dp->attached_connector;

	if (connector->panel.vbt.edp.bpp && pipe_bpp > connector->panel.vbt.edp.bpp) {
		/*
		 * This is a big fat ugly hack.
		 *
		 * Some machines in UEFI boot mode provide us a VBT that has 18
		 * bpp and 1.62 GHz link bandwidth for eDP, which for reasons
		 * unknown we fail to light up. Yet the same BIOS boots up with
		 * 24 bpp and 2.7 GHz link. Use the same bpp as the BIOS uses as
		 * max, not what it tells us to use.
		 *
		 * Note: This will still be broken if the eDP panel is not lit
		 * up by the BIOS, and thus we can't get the mode at module
		 * load.
		 */
		drm_dbg_kms(display->drm,
			    "pipe has %d bpp for eDP panel, overriding BIOS-provided max %d bpp\n",
			    pipe_bpp, connector->panel.vbt.edp.bpp);
		connector->panel.vbt.edp.bpp = pipe_bpp;
	}
}

static void intel_edp_mso_init(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_connector *connector = intel_dp->attached_connector;
	struct drm_display_info *info = &connector->base.display_info;
	u8 mso;

	if (intel_dp->edp_dpcd[0] < DP_EDP_14)
		return;

	if (drm_dp_dpcd_readb(&intel_dp->aux, DP_EDP_MSO_LINK_CAPABILITIES, &mso) != 1) {
		drm_err(display->drm, "Failed to read MSO cap\n");
		return;
	}

	/* Valid configurations are SST or MSO 2x1, 2x2, 4x1 */
	mso &= DP_EDP_MSO_NUMBER_OF_LINKS_MASK;
	if (mso % 2 || mso > drm_dp_max_lane_count(intel_dp->dpcd)) {
		drm_err(display->drm, "Invalid MSO link count cap %u\n", mso);
		mso = 0;
	}

	if (mso) {
		drm_dbg_kms(display->drm,
			    "Sink MSO %ux%u configuration, pixel overlap %u\n",
			    mso, drm_dp_max_lane_count(intel_dp->dpcd) / mso,
			    info->mso_pixel_overlap);
		if (!HAS_MSO(display)) {
			drm_err(display->drm,
				"No source MSO support, disabling\n");
			mso = 0;
		}
	}

	intel_dp->mso_link_count = mso;
	intel_dp->mso_pixel_overlap = mso ? info->mso_pixel_overlap : 0;
}

static void
intel_edp_set_sink_rates(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_encoder *encoder = &dp_to_dig_port(intel_dp)->base;

	intel_dp->num_sink_rates = 0;

	if (intel_dp->edp_dpcd[0] >= DP_EDP_14) {
		__le16 sink_rates[DP_MAX_SUPPORTED_RATES];
		int i;

		drm_dp_dpcd_read(&intel_dp->aux, DP_SUPPORTED_LINK_RATES,
				 sink_rates, sizeof(sink_rates));

		for (i = 0; i < ARRAY_SIZE(sink_rates); i++) {
			int rate;

			/* Value read multiplied by 200kHz gives the per-lane
			 * link rate in kHz. The source rates are, however,
			 * stored in terms of LS_Clk kHz. The full conversion
			 * back to symbols is
			 * (val * 200kHz)*(8/10 ch. encoding)*(1/8 bit to Byte)
			 */
			rate = le16_to_cpu(sink_rates[i]) * 200 / 10;

			if (rate == 0)
				break;

			/*
			 * Some broken eDP sinks illegally declare support for
			 * HBR3 without TPS4, and are unable to produce a stable
			 * output. Reject HBR3 when TPS4 is not available.
			 */
			if (rate >= 810000 && !drm_dp_tps4_supported(intel_dp->dpcd)) {
				drm_dbg_kms(display->drm,
					    "[ENCODER:%d:%s] Rejecting HBR3 due to missing TPS4 support\n",
					    encoder->base.base.id, encoder->base.name);
				break;
			}

			intel_dp->sink_rates[i] = rate;
		}
		intel_dp->num_sink_rates = i;
	}

	/*
	 * Use DP_LINK_RATE_SET if DP_SUPPORTED_LINK_RATES are available,
	 * default to DP_MAX_LINK_RATE and DP_LINK_BW_SET otherwise.
	 */
	if (intel_dp->num_sink_rates)
		intel_dp->use_rate_select = true;
	else
		intel_dp_set_sink_rates(intel_dp);
}

static bool
intel_edp_init_dpcd(struct intel_dp *intel_dp, struct intel_connector *connector)
{
	struct intel_display *display = to_intel_display(intel_dp);

	/* this function is meant to be called only once */
	drm_WARN_ON(display->drm, intel_dp->dpcd[DP_DPCD_REV] != 0);

	if (drm_dp_read_dpcd_caps(&intel_dp->aux, intel_dp->dpcd) != 0)
		return false;

	drm_dp_read_desc(&intel_dp->aux, &intel_dp->desc,
			 drm_dp_is_branch(intel_dp->dpcd));
	intel_init_dpcd_quirks(intel_dp, &intel_dp->desc.ident);

	intel_dp->colorimetry_support =
		intel_dp_get_colorimetry_status(intel_dp);

	/*
	 * Read the eDP display control registers.
	 *
	 * Do this independent of DP_DPCD_DISPLAY_CONTROL_CAPABLE bit in
	 * DP_EDP_CONFIGURATION_CAP, because some buggy displays do not have it
	 * set, but require eDP 1.4+ detection (e.g. for supported link rates
	 * method). The display control registers should read zero if they're
	 * not supported anyway.
	 */
	if (drm_dp_dpcd_read(&intel_dp->aux, DP_EDP_DPCD_REV,
			     intel_dp->edp_dpcd, sizeof(intel_dp->edp_dpcd)) ==
			     sizeof(intel_dp->edp_dpcd)) {
		drm_dbg_kms(display->drm, "eDP DPCD: %*ph\n",
			    (int)sizeof(intel_dp->edp_dpcd),
			    intel_dp->edp_dpcd);

		intel_dp->use_max_params = intel_dp->edp_dpcd[0] < DP_EDP_14;
	}

	/*
	 * If needed, program our source OUI so we can make various Intel-specific AUX services
	 * available (such as HDR backlight controls)
	 */
	intel_dp_init_source_oui(intel_dp);

	/*
	 * This has to be called after intel_dp->edp_dpcd is filled, PSR checks
	 * for SET_POWER_CAPABLE bit in intel_dp->edp_dpcd[1]
	 */
	intel_psr_init_dpcd(intel_dp);

	intel_edp_set_sink_rates(intel_dp);
	intel_dp_set_max_sink_lane_count(intel_dp);

	/* Read the eDP DSC DPCD registers */
	intel_dp_detect_dsc_caps(intel_dp, connector);

	return true;
}

static bool
intel_dp_has_sink_count(struct intel_dp *intel_dp)
{
	if (!intel_dp->attached_connector)
		return false;

	return drm_dp_read_sink_count_cap(&intel_dp->attached_connector->base,
					  intel_dp->dpcd,
					  &intel_dp->desc);
}

void intel_dp_update_sink_caps(struct intel_dp *intel_dp)
{
	intel_dp_set_sink_rates(intel_dp);
	intel_dp_set_max_sink_lane_count(intel_dp);
	intel_dp_set_common_rates(intel_dp);
}

static bool
intel_dp_get_dpcd(struct intel_dp *intel_dp)
{
	int ret;

	if (intel_dp_init_lttpr_and_dprx_caps(intel_dp) < 0)
		return false;

	/*
	 * Don't clobber cached eDP rates. Also skip re-reading
	 * the OUI/ID since we know it won't change.
	 */
	if (!intel_dp_is_edp(intel_dp)) {
		drm_dp_read_desc(&intel_dp->aux, &intel_dp->desc,
				 drm_dp_is_branch(intel_dp->dpcd));

		intel_init_dpcd_quirks(intel_dp, &intel_dp->desc.ident);

		intel_dp->colorimetry_support =
			intel_dp_get_colorimetry_status(intel_dp);

		intel_dp_update_sink_caps(intel_dp);
	}

	if (intel_dp_has_sink_count(intel_dp)) {
		ret = drm_dp_read_sink_count(&intel_dp->aux);
		if (ret < 0)
			return false;

		/*
		 * Sink count can change between short pulse hpd hence
		 * a member variable in intel_dp will track any changes
		 * between short pulse interrupts.
		 */
		intel_dp->sink_count = ret;

		/*
		 * SINK_COUNT == 0 and DOWNSTREAM_PORT_PRESENT == 1 implies that
		 * a dongle is present but no display. Unless we require to know
		 * if a dongle is present or not, we don't need to update
		 * downstream port information. So, an early return here saves
		 * time from performing other operations which are not required.
		 */
		if (!intel_dp->sink_count)
			return false;
	}

	return drm_dp_read_downstream_info(&intel_dp->aux, intel_dp->dpcd,
					   intel_dp->downstream_ports) == 0;
}

static const char *intel_dp_mst_mode_str(enum drm_dp_mst_mode mst_mode)
{
	if (mst_mode == DRM_DP_MST)
		return "MST";
	else if (mst_mode == DRM_DP_SST_SIDEBAND_MSG)
		return "SST w/ sideband messaging";
	else
		return "SST";
}

static enum drm_dp_mst_mode
intel_dp_mst_mode_choose(struct intel_dp *intel_dp,
			 enum drm_dp_mst_mode sink_mst_mode)
{
	struct intel_display *display = to_intel_display(intel_dp);

	if (!display->params.enable_dp_mst)
		return DRM_DP_SST;

	if (!intel_dp_mst_source_support(intel_dp))
		return DRM_DP_SST;

	if (sink_mst_mode == DRM_DP_SST_SIDEBAND_MSG &&
	    !(intel_dp->dpcd[DP_MAIN_LINK_CHANNEL_CODING] & DP_CAP_ANSI_128B132B))
		return DRM_DP_SST;

	return sink_mst_mode;
}

static enum drm_dp_mst_mode
intel_dp_mst_detect(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_encoder *encoder = &dp_to_dig_port(intel_dp)->base;
	enum drm_dp_mst_mode sink_mst_mode;
	enum drm_dp_mst_mode mst_detect;

	sink_mst_mode = drm_dp_read_mst_cap(&intel_dp->aux, intel_dp->dpcd);

	mst_detect = intel_dp_mst_mode_choose(intel_dp, sink_mst_mode);

	drm_dbg_kms(display->drm,
		    "[ENCODER:%d:%s] MST support: port: %s, sink: %s, modparam: %s -> enable: %s\n",
		    encoder->base.base.id, encoder->base.name,
		    str_yes_no(intel_dp_mst_source_support(intel_dp)),
		    intel_dp_mst_mode_str(sink_mst_mode),
		    str_yes_no(display->params.enable_dp_mst),
		    intel_dp_mst_mode_str(mst_detect));

	return mst_detect;
}

static void
intel_dp_mst_configure(struct intel_dp *intel_dp)
{
	if (!intel_dp_mst_source_support(intel_dp))
		return;

	intel_dp->is_mst = intel_dp->mst_detect != DRM_DP_SST;

	if (intel_dp->is_mst)
		intel_dp_mst_prepare_probe(intel_dp);

	drm_dp_mst_topology_mgr_set_mst(&intel_dp->mst.mgr, intel_dp->is_mst);

	/* Avoid stale info on the next detect cycle. */
	intel_dp->mst_detect = DRM_DP_SST;
}

static void
intel_dp_mst_disconnect(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);

	if (!intel_dp->is_mst)
		return;

	drm_dbg_kms(display->drm,
		    "MST device may have disappeared %d vs %d\n",
		    intel_dp->is_mst, intel_dp->mst.mgr.mst_state);
	intel_dp->is_mst = false;
	drm_dp_mst_topology_mgr_set_mst(&intel_dp->mst.mgr, intel_dp->is_mst);
}

static bool
intel_dp_get_sink_irq_esi(struct intel_dp *intel_dp, u8 *esi)
{
	struct intel_display *display = to_intel_display(intel_dp);

	/*
	 * Display WA for HSD #13013007775: mtl/arl/lnl
	 * Read the sink count and link service IRQ registers in separate
	 * transactions to prevent disconnecting the sink on a TBT link
	 * inadvertently.
	 */
	if (IS_DISPLAY_VER(display, 14, 20) && !display->platform.battlemage) {
		if (drm_dp_dpcd_read(&intel_dp->aux, DP_SINK_COUNT_ESI, esi, 3) != 3)
			return false;

		/* DP_SINK_COUNT_ESI + 3 == DP_LINK_SERVICE_IRQ_VECTOR_ESI0 */
		return drm_dp_dpcd_readb(&intel_dp->aux, DP_LINK_SERVICE_IRQ_VECTOR_ESI0,
					 &esi[3]) == 1;
	}

	return drm_dp_dpcd_read(&intel_dp->aux, DP_SINK_COUNT_ESI, esi, 4) == 4;
}

static bool intel_dp_ack_sink_irq_esi(struct intel_dp *intel_dp, u8 esi[4])
{
	int retry;

	for (retry = 0; retry < 3; retry++) {
		if (drm_dp_dpcd_write(&intel_dp->aux, DP_SINK_COUNT_ESI + 1,
				      &esi[1], 3) == 3)
			return true;
	}

	return false;
}

bool
intel_dp_needs_vsc_sdp(const struct intel_crtc_state *crtc_state,
		       const struct drm_connector_state *conn_state)
{
	/*
	 * As per DP 1.4a spec section 2.2.4.3 [MSA Field for Indication
	 * of Color Encoding Format and Content Color Gamut], in order to
	 * sending YCBCR 420 or HDR BT.2020 signals we should use DP VSC SDP.
	 */
	if (crtc_state->output_format == INTEL_OUTPUT_FORMAT_YCBCR420)
		return true;

	switch (conn_state->colorspace) {
	case DRM_MODE_COLORIMETRY_SYCC_601:
	case DRM_MODE_COLORIMETRY_OPYCC_601:
	case DRM_MODE_COLORIMETRY_BT2020_YCC:
	case DRM_MODE_COLORIMETRY_BT2020_RGB:
	case DRM_MODE_COLORIMETRY_BT2020_CYCC:
		return true;
	default:
		break;
	}

	return false;
}

static ssize_t intel_dp_as_sdp_pack(const struct drm_dp_as_sdp *as_sdp,
				    struct dp_sdp *sdp, size_t size)
{
	size_t length = sizeof(struct dp_sdp);

	if (size < length)
		return -ENOSPC;

	memset(sdp, 0, size);

	/* Prepare AS (Adaptive Sync) SDP Header */
	sdp->sdp_header.HB0 = 0;
	sdp->sdp_header.HB1 = as_sdp->sdp_type;
	sdp->sdp_header.HB2 = 0x02;
	sdp->sdp_header.HB3 = as_sdp->length;

	/* Fill AS (Adaptive Sync) SDP Payload */
	sdp->db[0] = as_sdp->mode;
	sdp->db[1] = as_sdp->vtotal & 0xFF;
	sdp->db[2] = (as_sdp->vtotal >> 8) & 0xFF;
	sdp->db[3] = as_sdp->target_rr & 0xFF;
	sdp->db[4] = (as_sdp->target_rr >> 8) & 0x3;

	if (as_sdp->target_rr_divider)
		sdp->db[4] |= 0x20;

	return length;
}

static ssize_t
intel_dp_hdr_metadata_infoframe_sdp_pack(struct intel_display *display,
					 const struct hdmi_drm_infoframe *drm_infoframe,
					 struct dp_sdp *sdp,
					 size_t size)
{
	size_t length = sizeof(struct dp_sdp);
	const int infoframe_size = HDMI_INFOFRAME_HEADER_SIZE + HDMI_DRM_INFOFRAME_SIZE;
	unsigned char buf[HDMI_INFOFRAME_HEADER_SIZE + HDMI_DRM_INFOFRAME_SIZE];
	ssize_t len;

	if (size < length)
		return -ENOSPC;

	memset(sdp, 0, size);

	len = hdmi_drm_infoframe_pack_only(drm_infoframe, buf, sizeof(buf));
	if (len < 0) {
		drm_dbg_kms(display->drm,
			    "buffer size is smaller than hdr metadata infoframe\n");
		return -ENOSPC;
	}

	if (len != infoframe_size) {
		drm_dbg_kms(display->drm, "wrong static hdr metadata size\n");
		return -ENOSPC;
	}

	/*
	 * Set up the infoframe sdp packet for HDR static metadata.
	 * Prepare VSC Header for SU as per DP 1.4a spec,
	 * Table 2-100 and Table 2-101
	 */

	/* Secondary-Data Packet ID, 00h for non-Audio INFOFRAME */
	sdp->sdp_header.HB0 = 0;
	/*
	 * Packet Type 80h + Non-audio INFOFRAME Type value
	 * HDMI_INFOFRAME_TYPE_DRM: 0x87
	 * - 80h + Non-audio INFOFRAME Type value
	 * - InfoFrame Type: 0x07
	 *    [CTA-861-G Table-42 Dynamic Range and Mastering InfoFrame]
	 */
	sdp->sdp_header.HB1 = drm_infoframe->type;
	/*
	 * Least Significant Eight Bits of (Data Byte Count – 1)
	 * infoframe_size - 1
	 */
	sdp->sdp_header.HB2 = 0x1D;
	/* INFOFRAME SDP Version Number */
	sdp->sdp_header.HB3 = (0x13 << 2);
	/* CTA Header Byte 2 (INFOFRAME Version Number) */
	sdp->db[0] = drm_infoframe->version;
	/* CTA Header Byte 3 (Length of INFOFRAME): HDMI_DRM_INFOFRAME_SIZE */
	sdp->db[1] = drm_infoframe->length;
	/*
	 * Copy HDMI_DRM_INFOFRAME_SIZE size from a buffer after
	 * HDMI_INFOFRAME_HEADER_SIZE
	 */
	BUILD_BUG_ON(sizeof(sdp->db) < HDMI_DRM_INFOFRAME_SIZE + 2);
	memcpy(&sdp->db[2], &buf[HDMI_INFOFRAME_HEADER_SIZE],
	       HDMI_DRM_INFOFRAME_SIZE);

	/*
	 * Size of DP infoframe sdp packet for HDR static metadata consists of
	 * - DP SDP Header(struct dp_sdp_header): 4 bytes
	 * - Two Data Blocks: 2 bytes
	 *    CTA Header Byte2 (INFOFRAME Version Number)
	 *    CTA Header Byte3 (Length of INFOFRAME)
	 * - HDMI_DRM_INFOFRAME_SIZE: 26 bytes
	 *
	 * Prior to GEN11's GMP register size is identical to DP HDR static metadata
	 * infoframe size. But GEN11+ has larger than that size, write_infoframe
	 * will pad rest of the size.
	 */
	return sizeof(struct dp_sdp_header) + 2 + HDMI_DRM_INFOFRAME_SIZE;
}

static void intel_write_dp_sdp(struct intel_encoder *encoder,
			       const struct intel_crtc_state *crtc_state,
			       unsigned int type)
{
	struct intel_display *display = to_intel_display(encoder);
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);
	struct dp_sdp sdp = {};
	ssize_t len;

	if ((crtc_state->infoframes.enable &
	     intel_hdmi_infoframe_enable(type)) == 0)
		return;

	switch (type) {
	case DP_SDP_VSC:
		len = drm_dp_vsc_sdp_pack(&crtc_state->infoframes.vsc, &sdp);
		break;
	case HDMI_PACKET_TYPE_GAMUT_METADATA:
		len = intel_dp_hdr_metadata_infoframe_sdp_pack(display,
							       &crtc_state->infoframes.drm.drm,
							       &sdp, sizeof(sdp));
		break;
	case DP_SDP_ADAPTIVE_SYNC:
		len = intel_dp_as_sdp_pack(&crtc_state->infoframes.as_sdp, &sdp,
					   sizeof(sdp));
		break;
	default:
		MISSING_CASE(type);
		return;
	}

	if (drm_WARN_ON(display->drm, len < 0))
		return;

	dig_port->write_infoframe(encoder, crtc_state, type, &sdp, len);
}

void intel_dp_set_infoframes(struct intel_encoder *encoder,
			     bool enable,
			     const struct intel_crtc_state *crtc_state,
			     const struct drm_connector_state *conn_state)
{
	struct intel_display *display = to_intel_display(encoder);
	i915_reg_t reg = HSW_TVIDEO_DIP_CTL(display, crtc_state->cpu_transcoder);
	u32 dip_enable = VIDEO_DIP_ENABLE_AVI_HSW | VIDEO_DIP_ENABLE_GCP_HSW |
			 VIDEO_DIP_ENABLE_VS_HSW | VIDEO_DIP_ENABLE_GMP_HSW |
			 VIDEO_DIP_ENABLE_SPD_HSW | VIDEO_DIP_ENABLE_DRM_GLK;

	if (HAS_AS_SDP(display))
		dip_enable |= VIDEO_DIP_ENABLE_AS_ADL;

	u32 val = intel_de_read(display, reg) & ~dip_enable;

	/* TODO: Sanitize DSC enabling wrt. intel_dsc_dp_pps_write(). */
	if (!enable && HAS_DSC(display))
		val &= ~VDIP_ENABLE_PPS;

	/*
	 * This routine disables VSC DIP if the function is called
	 * to disable SDP or if it does not have PSR
	 */
	if (!enable || !crtc_state->has_psr)
		val &= ~VIDEO_DIP_ENABLE_VSC_HSW;

	intel_de_write(display, reg, val);
	intel_de_posting_read(display, reg);

	if (!enable)
		return;

	intel_write_dp_sdp(encoder, crtc_state, DP_SDP_VSC);
	intel_write_dp_sdp(encoder, crtc_state, DP_SDP_ADAPTIVE_SYNC);

	intel_write_dp_sdp(encoder, crtc_state, HDMI_PACKET_TYPE_GAMUT_METADATA);
}

static
int intel_dp_as_sdp_unpack(struct drm_dp_as_sdp *as_sdp,
			   const void *buffer, size_t size)
{
	const struct dp_sdp *sdp = buffer;

	if (size < sizeof(struct dp_sdp))
		return -EINVAL;

	memset(as_sdp, 0, sizeof(*as_sdp));

	if (sdp->sdp_header.HB0 != 0)
		return -EINVAL;

	if (sdp->sdp_header.HB1 != DP_SDP_ADAPTIVE_SYNC)
		return -EINVAL;

	if (sdp->sdp_header.HB2 != 0x02)
		return -EINVAL;

	if ((sdp->sdp_header.HB3 & 0x3F) != 9)
		return -EINVAL;

	as_sdp->length = sdp->sdp_header.HB3 & DP_ADAPTIVE_SYNC_SDP_LENGTH;
	as_sdp->mode = sdp->db[0] & DP_ADAPTIVE_SYNC_SDP_OPERATION_MODE;
	as_sdp->vtotal = (sdp->db[2] << 8) | sdp->db[1];
	as_sdp->target_rr = (u64)sdp->db[3] | ((u64)sdp->db[4] & 0x3);
	as_sdp->target_rr_divider = sdp->db[4] & 0x20 ? true : false;

	return 0;
}

static int intel_dp_vsc_sdp_unpack(struct drm_dp_vsc_sdp *vsc,
				   const void *buffer, size_t size)
{
	const struct dp_sdp *sdp = buffer;

	if (size < sizeof(struct dp_sdp))
		return -EINVAL;

	memset(vsc, 0, sizeof(*vsc));

	if (sdp->sdp_header.HB0 != 0)
		return -EINVAL;

	if (sdp->sdp_header.HB1 != DP_SDP_VSC)
		return -EINVAL;

	vsc->sdp_type = sdp->sdp_header.HB1;
	vsc->revision = sdp->sdp_header.HB2;
	vsc->length = sdp->sdp_header.HB3;

	if ((sdp->sdp_header.HB2 == 0x2 && sdp->sdp_header.HB3 == 0x8) ||
	    (sdp->sdp_header.HB2 == 0x4 && sdp->sdp_header.HB3 == 0xe) ||
	    (sdp->sdp_header.HB2 == 0x6 && sdp->sdp_header.HB3 == 0x10)) {
		/*
		 * - HB2 = 0x2, HB3 = 0x8
		 *   VSC SDP supporting 3D stereo + PSR
		 * - HB2 = 0x4, HB3 = 0xe
		 *   VSC SDP supporting 3D stereo + PSR2 with Y-coordinate of
		 *   first scan line of the SU region (applies to eDP v1.4b
		 *   and higher).
		 * - HB2 = 0x6, HB3 = 0x10
		 *   VSC SDP supporting 3D stereo + Panel Replay.
		 */
		return 0;
	} else if (sdp->sdp_header.HB2 == 0x5 && sdp->sdp_header.HB3 == 0x13) {
		/*
		 * - HB2 = 0x5, HB3 = 0x13
		 *   VSC SDP supporting 3D stereo + PSR2 + Pixel Encoding/Colorimetry
		 *   Format.
		 */
		vsc->pixelformat = (sdp->db[16] >> 4) & 0xf;
		vsc->colorimetry = sdp->db[16] & 0xf;
		vsc->dynamic_range = (sdp->db[17] >> 7) & 0x1;

		switch (sdp->db[17] & 0x7) {
		case 0x0:
			vsc->bpc = 6;
			break;
		case 0x1:
			vsc->bpc = 8;
			break;
		case 0x2:
			vsc->bpc = 10;
			break;
		case 0x3:
			vsc->bpc = 12;
			break;
		case 0x4:
			vsc->bpc = 16;
			break;
		default:
			MISSING_CASE(sdp->db[17] & 0x7);
			return -EINVAL;
		}

		vsc->content_type = sdp->db[18] & 0x7;
	} else {
		return -EINVAL;
	}

	return 0;
}

static void
intel_read_dp_as_sdp(struct intel_encoder *encoder,
		     struct intel_crtc_state *crtc_state,
		     struct drm_dp_as_sdp *as_sdp)
{
	struct intel_display *display = to_intel_display(encoder);
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);
	unsigned int type = DP_SDP_ADAPTIVE_SYNC;
	struct dp_sdp sdp = {};
	int ret;

	if ((crtc_state->infoframes.enable &
	     intel_hdmi_infoframe_enable(type)) == 0)
		return;

	dig_port->read_infoframe(encoder, crtc_state, type, &sdp,
				 sizeof(sdp));

	ret = intel_dp_as_sdp_unpack(as_sdp, &sdp, sizeof(sdp));
	if (ret)
		drm_dbg_kms(display->drm, "Failed to unpack DP AS SDP\n");
}

static int
intel_dp_hdr_metadata_infoframe_sdp_unpack(struct hdmi_drm_infoframe *drm_infoframe,
					   const void *buffer, size_t size)
{
	int ret;

	const struct dp_sdp *sdp = buffer;

	if (size < sizeof(struct dp_sdp))
		return -EINVAL;

	if (sdp->sdp_header.HB0 != 0)
		return -EINVAL;

	if (sdp->sdp_header.HB1 != HDMI_INFOFRAME_TYPE_DRM)
		return -EINVAL;

	/*
	 * Least Significant Eight Bits of (Data Byte Count – 1)
	 * 1Dh (i.e., Data Byte Count = 30 bytes).
	 */
	if (sdp->sdp_header.HB2 != 0x1D)
		return -EINVAL;

	/* Most Significant Two Bits of (Data Byte Count – 1), Clear to 00b. */
	if ((sdp->sdp_header.HB3 & 0x3) != 0)
		return -EINVAL;

	/* INFOFRAME SDP Version Number */
	if (((sdp->sdp_header.HB3 >> 2) & 0x3f) != 0x13)
		return -EINVAL;

	/* CTA Header Byte 2 (INFOFRAME Version Number) */
	if (sdp->db[0] != 1)
		return -EINVAL;

	/* CTA Header Byte 3 (Length of INFOFRAME): HDMI_DRM_INFOFRAME_SIZE */
	if (sdp->db[1] != HDMI_DRM_INFOFRAME_SIZE)
		return -EINVAL;

	ret = hdmi_drm_infoframe_unpack_only(drm_infoframe, &sdp->db[2],
					     HDMI_DRM_INFOFRAME_SIZE);

	return ret;
}

static void intel_read_dp_vsc_sdp(struct intel_encoder *encoder,
				  struct intel_crtc_state *crtc_state,
				  struct drm_dp_vsc_sdp *vsc)
{
	struct intel_display *display = to_intel_display(encoder);
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);
	unsigned int type = DP_SDP_VSC;
	struct dp_sdp sdp = {};
	int ret;

	if ((crtc_state->infoframes.enable &
	     intel_hdmi_infoframe_enable(type)) == 0)
		return;

	dig_port->read_infoframe(encoder, crtc_state, type, &sdp, sizeof(sdp));

	ret = intel_dp_vsc_sdp_unpack(vsc, &sdp, sizeof(sdp));

	if (ret)
		drm_dbg_kms(display->drm, "Failed to unpack DP VSC SDP\n");
}

static void intel_read_dp_hdr_metadata_infoframe_sdp(struct intel_encoder *encoder,
						     struct intel_crtc_state *crtc_state,
						     struct hdmi_drm_infoframe *drm_infoframe)
{
	struct intel_display *display = to_intel_display(encoder);
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);
	unsigned int type = HDMI_PACKET_TYPE_GAMUT_METADATA;
	struct dp_sdp sdp = {};
	int ret;

	if ((crtc_state->infoframes.enable &
	    intel_hdmi_infoframe_enable(type)) == 0)
		return;

	dig_port->read_infoframe(encoder, crtc_state, type, &sdp,
				 sizeof(sdp));

	ret = intel_dp_hdr_metadata_infoframe_sdp_unpack(drm_infoframe, &sdp,
							 sizeof(sdp));

	if (ret)
		drm_dbg_kms(display->drm,
			    "Failed to unpack DP HDR Metadata Infoframe SDP\n");
}

void intel_read_dp_sdp(struct intel_encoder *encoder,
		       struct intel_crtc_state *crtc_state,
		       unsigned int type)
{
	switch (type) {
	case DP_SDP_VSC:
		intel_read_dp_vsc_sdp(encoder, crtc_state,
				      &crtc_state->infoframes.vsc);
		break;
	case HDMI_PACKET_TYPE_GAMUT_METADATA:
		intel_read_dp_hdr_metadata_infoframe_sdp(encoder, crtc_state,
							 &crtc_state->infoframes.drm.drm);
		break;
	case DP_SDP_ADAPTIVE_SYNC:
		intel_read_dp_as_sdp(encoder, crtc_state,
				     &crtc_state->infoframes.as_sdp);
		break;
	default:
		MISSING_CASE(type);
		break;
	}
}

static bool intel_dp_link_ok(struct intel_dp *intel_dp,
			     u8 link_status[DP_LINK_STATUS_SIZE])
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_encoder *encoder = &dp_to_dig_port(intel_dp)->base;
	bool uhbr = intel_dp->link_rate >= 1000000;
	bool ok;

	if (uhbr)
		ok = drm_dp_128b132b_lane_channel_eq_done(link_status,
							  intel_dp->lane_count);
	else
		ok = drm_dp_channel_eq_ok(link_status, intel_dp->lane_count);

	if (ok)
		return true;

	intel_dp_dump_link_status(intel_dp, DP_PHY_DPRX, link_status);
	drm_dbg_kms(display->drm,
		    "[ENCODER:%d:%s] %s link not ok, retraining\n",
		    encoder->base.base.id, encoder->base.name,
		    uhbr ? "128b/132b" : "8b/10b");

	return false;
}

static void
intel_dp_mst_hpd_irq(struct intel_dp *intel_dp, u8 *esi, u8 *ack)
{
	bool handled = false;

	drm_dp_mst_hpd_irq_handle_event(&intel_dp->mst.mgr, esi, ack, &handled);

	if (esi[1] & DP_CP_IRQ) {
		intel_hdcp_handle_cp_irq(intel_dp->attached_connector);
		ack[1] |= DP_CP_IRQ;
	}
}

static bool intel_dp_mst_link_status(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_encoder *encoder = &dp_to_dig_port(intel_dp)->base;
	u8 link_status[DP_LINK_STATUS_SIZE] = {};
	const size_t esi_link_status_size = DP_LINK_STATUS_SIZE - 2;

	if (drm_dp_dpcd_read(&intel_dp->aux, DP_LANE0_1_STATUS_ESI, link_status,
			     esi_link_status_size) != esi_link_status_size) {
		drm_err(display->drm,
			"[ENCODER:%d:%s] Failed to read link status\n",
			encoder->base.base.id, encoder->base.name);
		return false;
	}

	return intel_dp_link_ok(intel_dp, link_status);
}

/**
 * intel_dp_check_mst_status - service any pending MST interrupts, check link status
 * @intel_dp: Intel DP struct
 *
 * Read any pending MST interrupts, call MST core to handle these and ack the
 * interrupts. Check if the main and AUX link state is ok.
 *
 * Returns:
 * - %true if pending interrupts were serviced (or no interrupts were
 *   pending) w/o detecting an error condition.
 * - %false if an error condition - like AUX failure or a loss of link - is
 *   detected, or another condition - like a DP tunnel BW state change - needs
 *   servicing from the hotplug work.
 */
static bool
intel_dp_check_mst_status(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_digital_port *dig_port = dp_to_dig_port(intel_dp);
	struct intel_encoder *encoder = &dig_port->base;
	bool link_ok = true;
	bool reprobe_needed = false;

	for (;;) {
		u8 esi[4] = {};
		u8 ack[4] = {};

		if (!intel_dp_get_sink_irq_esi(intel_dp, esi)) {
			drm_dbg_kms(display->drm,
				    "failed to get ESI - device may have failed\n");
			link_ok = false;

			break;
		}

		drm_dbg_kms(display->drm, "DPRX ESI: %4ph\n", esi);

		if (intel_dp_mst_active_streams(intel_dp) > 0 && link_ok &&
		    esi[3] & LINK_STATUS_CHANGED) {
			if (!intel_dp_mst_link_status(intel_dp))
				link_ok = false;
			ack[3] |= LINK_STATUS_CHANGED;
		}

		intel_dp_mst_hpd_irq(intel_dp, esi, ack);

		if (esi[3] & DP_TUNNELING_IRQ) {
			if (drm_dp_tunnel_handle_irq(display->dp_tunnel_mgr,
						     &intel_dp->aux))
				reprobe_needed = true;
			ack[3] |= DP_TUNNELING_IRQ;
		}

		if (mem_is_zero(ack, sizeof(ack)))
			break;

		if (!intel_dp_ack_sink_irq_esi(intel_dp, ack))
			drm_dbg_kms(display->drm, "Failed to ack ESI\n");

		if (ack[1] & (DP_DOWN_REP_MSG_RDY | DP_UP_REQ_MSG_RDY))
			drm_dp_mst_hpd_irq_send_new_request(&intel_dp->mst.mgr);
	}

	if (!link_ok || intel_dp->link.force_retrain)
		intel_encoder_link_check_queue_work(encoder, 0);

	return !reprobe_needed;
}

static void
intel_dp_handle_hdmi_link_status_change(struct intel_dp *intel_dp)
{
	bool is_active;
	u8 buf = 0;

	is_active = drm_dp_pcon_hdmi_link_active(&intel_dp->aux);
	if (intel_dp->frl.is_trained && !is_active) {
		if (drm_dp_dpcd_readb(&intel_dp->aux, DP_PCON_HDMI_LINK_CONFIG_1, &buf) < 0)
			return;

		buf &=  ~DP_PCON_ENABLE_HDMI_LINK;
		if (drm_dp_dpcd_writeb(&intel_dp->aux, DP_PCON_HDMI_LINK_CONFIG_1, buf) < 0)
			return;

		drm_dp_pcon_hdmi_frl_link_error_count(&intel_dp->aux, &intel_dp->attached_connector->base);

		intel_dp->frl.is_trained = false;

		/* Restart FRL training or fall back to TMDS mode */
		intel_dp_check_frl_training(intel_dp);
	}
}

static bool
intel_dp_needs_link_retrain(struct intel_dp *intel_dp)
{
	u8 link_status[DP_LINK_STATUS_SIZE];

	if (!intel_dp->link.active)
		return false;

	/*
	 * While PSR source HW is enabled, it will control main-link sending
	 * frames, enabling and disabling it so trying to do a retrain will fail
	 * as the link would or not be on or it could mix training patterns
	 * and frame data at the same time causing retrain to fail.
	 * Also when exiting PSR, HW will retrain the link anyways fixing
	 * any link status error.
	 */
	if (intel_psr_enabled(intel_dp))
		return false;

	if (intel_dp->link.force_retrain)
		return true;

	if (drm_dp_dpcd_read_phy_link_status(&intel_dp->aux, DP_PHY_DPRX,
					     link_status) < 0)
		return false;

	/*
	 * Validate the cached values of intel_dp->link_rate and
	 * intel_dp->lane_count before attempting to retrain.
	 *
	 * FIXME would be nice to user the crtc state here, but since
	 * we need to call this from the short HPD handler that seems
	 * a bit hard.
	 */
	if (!intel_dp_link_params_valid(intel_dp, intel_dp->link_rate,
					intel_dp->lane_count))
		return false;

	if (intel_dp->link.retrain_disabled)
		return false;

	if (intel_dp->link.seq_train_failures)
		return true;

	/* Retrain if link not ok */
	return !intel_dp_link_ok(intel_dp, link_status) &&
		!intel_psr_link_ok(intel_dp);
}

bool intel_dp_has_connector(struct intel_dp *intel_dp,
			    const struct drm_connector_state *conn_state)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_encoder *encoder;
	enum pipe pipe;

	if (!conn_state->best_encoder)
		return false;

	/* SST */
	encoder = &dp_to_dig_port(intel_dp)->base;
	if (conn_state->best_encoder == &encoder->base)
		return true;

	/* MST */
	for_each_pipe(display, pipe) {
		encoder = &intel_dp->mst.stream_encoders[pipe]->base;
		if (conn_state->best_encoder == &encoder->base)
			return true;
	}

	return false;
}

static void wait_for_connector_hw_done(const struct drm_connector_state *conn_state)
{
	struct intel_connector *connector = to_intel_connector(conn_state->connector);
	struct intel_display *display = to_intel_display(connector);

	drm_modeset_lock_assert_held(&display->drm->mode_config.connection_mutex);

	if (!conn_state->commit)
		return;

	drm_WARN_ON(display->drm,
		    !wait_for_completion_timeout(&conn_state->commit->hw_done,
						 msecs_to_jiffies(5000)));
}

int intel_dp_get_active_pipes(struct intel_dp *intel_dp,
			      struct drm_modeset_acquire_ctx *ctx,
			      u8 *pipe_mask)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct drm_connector_list_iter conn_iter;
	struct intel_connector *connector;
	int ret = 0;

	*pipe_mask = 0;

	drm_connector_list_iter_begin(display->drm, &conn_iter);
	for_each_intel_connector_iter(connector, &conn_iter) {
		struct drm_connector_state *conn_state =
			connector->base.state;
		struct intel_crtc_state *crtc_state;
		struct intel_crtc *crtc;

		if (!intel_dp_has_connector(intel_dp, conn_state))
			continue;

		crtc = to_intel_crtc(conn_state->crtc);
		if (!crtc)
			continue;

		ret = drm_modeset_lock(&crtc->base.mutex, ctx);
		if (ret)
			break;

		crtc_state = to_intel_crtc_state(crtc->base.state);

		drm_WARN_ON(display->drm,
			    !intel_crtc_has_dp_encoder(crtc_state));

		if (!crtc_state->hw.active)
			continue;

		wait_for_connector_hw_done(conn_state);

		*pipe_mask |= BIT(crtc->pipe);
	}
	drm_connector_list_iter_end(&conn_iter);

	return ret;
}

void intel_dp_flush_connector_commits(struct intel_connector *connector)
{
	wait_for_connector_hw_done(connector->base.state);
}

static bool intel_dp_is_connected(struct intel_dp *intel_dp)
{
	struct intel_connector *connector = intel_dp->attached_connector;

	return connector->base.status == connector_status_connected ||
		intel_dp->is_mst;
}

static int intel_dp_retrain_link(struct intel_encoder *encoder,
				 struct drm_modeset_acquire_ctx *ctx)
{
	struct intel_display *display = to_intel_display(encoder);
	struct intel_dp *intel_dp = enc_to_intel_dp(encoder);
	u8 pipe_mask;
	int ret;

	if (!intel_dp_is_connected(intel_dp))
		return 0;

	ret = drm_modeset_lock(&display->drm->mode_config.connection_mutex,
			       ctx);
	if (ret)
		return ret;

	if (!intel_dp_needs_link_retrain(intel_dp))
		return 0;

	ret = intel_dp_get_active_pipes(intel_dp, ctx, &pipe_mask);
	if (ret)
		return ret;

	if (pipe_mask == 0)
		return 0;

	if (!intel_dp_needs_link_retrain(intel_dp))
		return 0;

	drm_dbg_kms(display->drm,
		    "[ENCODER:%d:%s] retraining link (forced %s)\n",
		    encoder->base.base.id, encoder->base.name,
		    str_yes_no(intel_dp->link.force_retrain));

	ret = intel_modeset_commit_pipes(display, pipe_mask, ctx);
	if (ret == -EDEADLK)
		return ret;

	intel_dp->link.force_retrain = false;

	if (ret)
		drm_dbg_kms(display->drm,
			    "[ENCODER:%d:%s] link retraining failed: %pe\n",
			    encoder->base.base.id, encoder->base.name,
			    ERR_PTR(ret));

	return ret;
}

void intel_dp_link_check(struct intel_encoder *encoder)
{
	struct drm_modeset_acquire_ctx ctx;
	int ret;

	intel_modeset_lock_ctx_retry(&ctx, NULL, 0, ret)
		ret = intel_dp_retrain_link(encoder, &ctx);
}

void intel_dp_check_link_state(struct intel_dp *intel_dp)
{
	struct intel_digital_port *dig_port = dp_to_dig_port(intel_dp);
	struct intel_encoder *encoder = &dig_port->base;

	if (!intel_dp_is_connected(intel_dp))
		return;

	if (!intel_dp_needs_link_retrain(intel_dp))
		return;

	intel_encoder_link_check_queue_work(encoder, 0);
}

static void intel_dp_check_device_service_irq(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	u8 val;

	if (intel_dp->dpcd[DP_DPCD_REV] < 0x11)
		return;

	if (drm_dp_dpcd_readb(&intel_dp->aux,
			      DP_DEVICE_SERVICE_IRQ_VECTOR, &val) != 1 || !val)
		return;

	drm_dp_dpcd_writeb(&intel_dp->aux, DP_DEVICE_SERVICE_IRQ_VECTOR, val);

	if (val & DP_AUTOMATED_TEST_REQUEST)
		intel_dp_test_request(intel_dp);

	if (val & DP_CP_IRQ)
		intel_hdcp_handle_cp_irq(intel_dp->attached_connector);

	if (val & DP_SINK_SPECIFIC_IRQ)
		drm_dbg_kms(display->drm, "Sink specific irq unhandled\n");
}

static bool intel_dp_check_link_service_irq(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	bool reprobe_needed = false;
	u8 val;

	if (intel_dp->dpcd[DP_DPCD_REV] < 0x11)
		return false;

	if (drm_dp_dpcd_readb(&intel_dp->aux,
			      DP_LINK_SERVICE_IRQ_VECTOR_ESI0, &val) != 1 || !val)
		return false;

	if ((val & DP_TUNNELING_IRQ) &&
	    drm_dp_tunnel_handle_irq(display->dp_tunnel_mgr,
				     &intel_dp->aux))
		reprobe_needed = true;

	if (drm_dp_dpcd_writeb(&intel_dp->aux,
			       DP_LINK_SERVICE_IRQ_VECTOR_ESI0, val) != 1)
		return reprobe_needed;

	if (val & HDMI_LINK_STATUS_CHANGED)
		intel_dp_handle_hdmi_link_status_change(intel_dp);

	return reprobe_needed;
}

/*
 * According to DP spec
 * 5.1.2:
 *  1. Read DPCD
 *  2. Configure link according to Receiver Capabilities
 *  3. Use Link Training from 2.5.3.3 and 3.5.1.3
 *  4. Check link status on receipt of hot-plug interrupt
 *
 * intel_dp_short_pulse -  handles short pulse interrupts
 * when full detection is not required.
 * Returns %true if short pulse is handled and full detection
 * is NOT required and %false otherwise.
 */
static bool
intel_dp_short_pulse(struct intel_dp *intel_dp)
{
	u8 old_sink_count = intel_dp->sink_count;
	bool reprobe_needed = false;
	bool ret;

	intel_dp_test_reset(intel_dp);

	/*
	 * Now read the DPCD to see if it's actually running
	 * If the current value of sink count doesn't match with
	 * the value that was stored earlier or dpcd read failed
	 * we need to do full detection
	 */
	ret = intel_dp_get_dpcd(intel_dp);

	if ((old_sink_count != intel_dp->sink_count) || !ret) {
		/* No need to proceed if we are going to do full detect */
		return false;
	}

	intel_dp_check_device_service_irq(intel_dp);
	reprobe_needed = intel_dp_check_link_service_irq(intel_dp);

	/* Handle CEC interrupts, if any */
	drm_dp_cec_irq(&intel_dp->aux);

	intel_dp_check_link_state(intel_dp);

	intel_psr_short_pulse(intel_dp);

	if (intel_alpm_get_error(intel_dp)) {
		intel_alpm_disable(intel_dp);
		intel_dp->alpm_parameters.sink_alpm_error = true;
	}

	if (intel_dp_test_short_pulse(intel_dp))
		reprobe_needed = true;

	return !reprobe_needed;
}

/* XXX this is probably wrong for multiple downstream ports */
static enum drm_connector_status
intel_dp_detect_dpcd(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_digital_port *dig_port = dp_to_dig_port(intel_dp);
	u8 *dpcd = intel_dp->dpcd;
	u8 type;

	if (drm_WARN_ON(display->drm, intel_dp_is_edp(intel_dp)))
		return connector_status_connected;

	intel_lspcon_resume(dig_port);

	if (!intel_dp_get_dpcd(intel_dp))
		return connector_status_disconnected;

	intel_dp->mst_detect = intel_dp_mst_detect(intel_dp);

	/* if there's no downstream port, we're done */
	if (!drm_dp_is_branch(dpcd))
		return connector_status_connected;

	/* If we're HPD-aware, SINK_COUNT changes dynamically */
	if (intel_dp_has_sink_count(intel_dp) &&
	    intel_dp->downstream_ports[0] & DP_DS_PORT_HPD) {
		return intel_dp->sink_count ?
		connector_status_connected : connector_status_disconnected;
	}

	if (intel_dp->mst_detect == DRM_DP_MST)
		return connector_status_connected;

	/* If no HPD, poke DDC gently */
	if (drm_probe_ddc(&intel_dp->aux.ddc))
		return connector_status_connected;

	/* Well we tried, say unknown for unreliable port types */
	if (intel_dp->dpcd[DP_DPCD_REV] >= 0x11) {
		type = intel_dp->downstream_ports[0] & DP_DS_PORT_TYPE_MASK;
		if (type == DP_DS_PORT_TYPE_VGA ||
		    type == DP_DS_PORT_TYPE_NON_EDID)
			return connector_status_unknown;
	} else {
		type = intel_dp->dpcd[DP_DOWNSTREAMPORT_PRESENT] &
			DP_DWN_STRM_PORT_TYPE_MASK;
		if (type == DP_DWN_STRM_PORT_TYPE_ANALOG ||
		    type == DP_DWN_STRM_PORT_TYPE_OTHER)
			return connector_status_unknown;
	}

	/* Anything else is out of spec, warn and ignore */
	drm_dbg_kms(display->drm, "Broken DP branch device, ignoring\n");
	return connector_status_disconnected;
}

static enum drm_connector_status
edp_detect(struct intel_dp *intel_dp)
{
	return connector_status_connected;
}

void intel_digital_port_lock(struct intel_encoder *encoder)
{
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);

	if (dig_port->lock)
		dig_port->lock(dig_port);
}

void intel_digital_port_unlock(struct intel_encoder *encoder)
{
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);

	if (dig_port->unlock)
		dig_port->unlock(dig_port);
}

/*
 * intel_digital_port_connected_locked - is the specified port connected?
 * @encoder: intel_encoder
 *
 * In cases where there's a connector physically connected but it can't be used
 * by our hardware we also return false, since the rest of the driver should
 * pretty much treat the port as disconnected. This is relevant for type-C
 * (starting on ICL) where there's ownership involved.
 *
 * The caller must hold the lock acquired by calling intel_digital_port_lock()
 * when calling this function.
 *
 * Return %true if port is connected, %false otherwise.
 */
bool intel_digital_port_connected_locked(struct intel_encoder *encoder)
{
	struct intel_display *display = to_intel_display(encoder);
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);
	bool is_glitch_free = intel_tc_port_handles_hpd_glitches(dig_port);
	bool is_connected = false;
	intel_wakeref_t wakeref;

	with_intel_display_power(display, POWER_DOMAIN_DISPLAY_CORE, wakeref) {
		unsigned long wait_expires = jiffies + msecs_to_jiffies_timeout(4);

		do {
			is_connected = dig_port->connected(encoder);
			if (is_connected || is_glitch_free)
				break;
			usleep_range(10, 30);
		} while (time_before(jiffies, wait_expires));
	}

	return is_connected;
}

bool intel_digital_port_connected(struct intel_encoder *encoder)
{
	bool ret;

	intel_digital_port_lock(encoder);
	ret = intel_digital_port_connected_locked(encoder);
	intel_digital_port_unlock(encoder);

	return ret;
}

static const struct drm_edid *
intel_dp_get_edid(struct intel_dp *intel_dp)
{
	struct intel_connector *connector = intel_dp->attached_connector;
	const struct drm_edid *fixed_edid = connector->panel.fixed_edid;

	/* Use panel fixed edid if we have one */
	if (fixed_edid) {
		/* invalid edid */
		if (IS_ERR(fixed_edid))
			return NULL;

		return drm_edid_dup(fixed_edid);
	}

	return drm_edid_read_ddc(&connector->base, &intel_dp->aux.ddc);
}

static void
intel_dp_update_dfp(struct intel_dp *intel_dp,
		    const struct drm_edid *drm_edid)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_connector *connector = intel_dp->attached_connector;

	intel_dp->dfp.max_bpc =
		drm_dp_downstream_max_bpc(intel_dp->dpcd,
					  intel_dp->downstream_ports, drm_edid);

	intel_dp->dfp.max_dotclock =
		drm_dp_downstream_max_dotclock(intel_dp->dpcd,
					       intel_dp->downstream_ports);

	intel_dp->dfp.min_tmds_clock =
		drm_dp_downstream_min_tmds_clock(intel_dp->dpcd,
						 intel_dp->downstream_ports,
						 drm_edid);
	intel_dp->dfp.max_tmds_clock =
		drm_dp_downstream_max_tmds_clock(intel_dp->dpcd,
						 intel_dp->downstream_ports,
						 drm_edid);

	intel_dp->dfp.pcon_max_frl_bw =
		drm_dp_get_pcon_max_frl_bw(intel_dp->dpcd,
					   intel_dp->downstream_ports);

	drm_dbg_kms(display->drm,
		    "[CONNECTOR:%d:%s] DFP max bpc %d, max dotclock %d, TMDS clock %d-%d, PCON Max FRL BW %dGbps\n",
		    connector->base.base.id, connector->base.name,
		    intel_dp->dfp.max_bpc,
		    intel_dp->dfp.max_dotclock,
		    intel_dp->dfp.min_tmds_clock,
		    intel_dp->dfp.max_tmds_clock,
		    intel_dp->dfp.pcon_max_frl_bw);

	intel_dp_get_pcon_dsc_cap(intel_dp);
}

static bool
intel_dp_can_ycbcr420(struct intel_dp *intel_dp)
{
	if (source_can_output(intel_dp, INTEL_OUTPUT_FORMAT_YCBCR420) &&
	    (!drm_dp_is_branch(intel_dp->dpcd) || intel_dp->dfp.ycbcr420_passthrough))
		return true;

	if (source_can_output(intel_dp, INTEL_OUTPUT_FORMAT_RGB) &&
	    dfp_can_convert_from_rgb(intel_dp, INTEL_OUTPUT_FORMAT_YCBCR420))
		return true;

	if (source_can_output(intel_dp, INTEL_OUTPUT_FORMAT_YCBCR444) &&
	    dfp_can_convert_from_ycbcr444(intel_dp, INTEL_OUTPUT_FORMAT_YCBCR420))
		return true;

	return false;
}

static void
intel_dp_update_420(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_connector *connector = intel_dp->attached_connector;

	intel_dp->dfp.ycbcr420_passthrough =
		drm_dp_downstream_420_passthrough(intel_dp->dpcd,
						  intel_dp->downstream_ports);
	/* on-board LSPCON always assumed to support 4:4:4->4:2:0 conversion */
	intel_dp->dfp.ycbcr_444_to_420 =
		intel_lspcon_active(dp_to_dig_port(intel_dp)) ||
		drm_dp_downstream_444_to_420_conversion(intel_dp->dpcd,
							intel_dp->downstream_ports);
	intel_dp->dfp.rgb_to_ycbcr =
		drm_dp_downstream_rgb_to_ycbcr_conversion(intel_dp->dpcd,
							  intel_dp->downstream_ports,
							  DP_DS_HDMI_BT709_RGB_YCBCR_CONV);

	connector->base.ycbcr_420_allowed = intel_dp_can_ycbcr420(intel_dp);

	drm_dbg_kms(display->drm,
		    "[CONNECTOR:%d:%s] RGB->YcbCr conversion? %s, YCbCr 4:2:0 allowed? %s, YCbCr 4:4:4->4:2:0 conversion? %s\n",
		    connector->base.base.id, connector->base.name,
		    str_yes_no(intel_dp->dfp.rgb_to_ycbcr),
		    str_yes_no(connector->base.ycbcr_420_allowed),
		    str_yes_no(intel_dp->dfp.ycbcr_444_to_420));
}

static void
intel_dp_set_edid(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_connector *connector = intel_dp->attached_connector;
	const struct drm_edid *drm_edid;
	bool vrr_capable;

	intel_dp_unset_edid(intel_dp);
	drm_edid = intel_dp_get_edid(intel_dp);
	connector->detect_edid = drm_edid;

	/* Below we depend on display info having been updated */
	drm_edid_connector_update(&connector->base, drm_edid);

	vrr_capable = intel_vrr_is_capable(connector);
	drm_dbg_kms(display->drm, "[CONNECTOR:%d:%s] VRR capable: %s\n",
		    connector->base.base.id, connector->base.name, str_yes_no(vrr_capable));
	drm_connector_set_vrr_capable_property(&connector->base, vrr_capable);

	intel_dp_update_dfp(intel_dp, drm_edid);
	intel_dp_update_420(intel_dp);

	drm_dp_cec_attach(&intel_dp->aux,
			  connector->base.display_info.source_physical_address);
}

static void
intel_dp_unset_edid(struct intel_dp *intel_dp)
{
	struct intel_connector *connector = intel_dp->attached_connector;

	drm_dp_cec_unset_edid(&intel_dp->aux);
	drm_edid_free(connector->detect_edid);
	connector->detect_edid = NULL;

	intel_dp->dfp.max_bpc = 0;
	intel_dp->dfp.max_dotclock = 0;
	intel_dp->dfp.min_tmds_clock = 0;
	intel_dp->dfp.max_tmds_clock = 0;

	intel_dp->dfp.pcon_max_frl_bw = 0;

	intel_dp->dfp.ycbcr_444_to_420 = false;
	connector->base.ycbcr_420_allowed = false;

	drm_connector_set_vrr_capable_property(&connector->base,
					       false);
}

static void
intel_dp_detect_sdp_caps(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);

	intel_dp->as_sdp_supported = HAS_AS_SDP(display) &&
		drm_dp_as_sdp_supported(&intel_dp->aux, intel_dp->dpcd);
}

static bool intel_dp_needs_dpcd_probe(struct intel_dp *intel_dp, bool force_on_external)
{
	struct intel_connector *connector = intel_dp->attached_connector;

	if (intel_dp_is_edp(intel_dp))
		return false;

	if (force_on_external)
		return true;

	if (intel_dp->is_mst)
		return false;

	return drm_edid_has_quirk(&connector->base, DRM_EDID_QUIRK_DP_DPCD_PROBE);
}

void intel_dp_dpcd_set_probe(struct intel_dp *intel_dp, bool force_on_external)
{
	drm_dp_dpcd_set_probe(&intel_dp->aux,
			      intel_dp_needs_dpcd_probe(intel_dp, force_on_external));
}

static int
intel_dp_detect(struct drm_connector *_connector,
		struct drm_modeset_acquire_ctx *ctx,
		bool force)
{
	struct intel_display *display = to_intel_display(_connector->dev);
	struct intel_connector *connector = to_intel_connector(_connector);
	struct intel_dp *intel_dp = intel_attached_dp(connector);
	struct intel_digital_port *dig_port = dp_to_dig_port(intel_dp);
	struct intel_encoder *encoder = &dig_port->base;
	enum drm_connector_status status;
	int ret;

	drm_dbg_kms(display->drm, "[CONNECTOR:%d:%s]\n",
		    connector->base.base.id, connector->base.name);
	drm_WARN_ON(display->drm,
		    !drm_modeset_is_locked(&display->drm->mode_config.connection_mutex));

	if (!intel_display_device_enabled(display))
		return connector_status_disconnected;

	if (!intel_display_driver_check_access(display))
		return connector->base.status;

	intel_dp_flush_connector_commits(connector);

	intel_pps_vdd_on(intel_dp);

	/* Can't disconnect eDP */
	if (intel_dp_is_edp(intel_dp))
		status = edp_detect(intel_dp);
	else if (intel_digital_port_connected(encoder))
		status = intel_dp_detect_dpcd(intel_dp);
	else
		status = connector_status_disconnected;

	if (status != connector_status_disconnected &&
	    !intel_dp_mst_verify_dpcd_state(intel_dp))
		/*
		 * This requires retrying detection for instance to re-enable
		 * the MST mode that got reset via a long HPD pulse. The retry
		 * will happen either via the hotplug handler's retry logic,
		 * ensured by setting the connector here to SST/disconnected,
		 * or via a userspace connector probing in response to the
		 * hotplug uevent sent when removing the MST connectors.
		 */
		status = connector_status_disconnected;

	if (status == connector_status_disconnected) {
		intel_dp_test_reset(intel_dp);
		memset(connector->dp.dsc_dpcd, 0, sizeof(connector->dp.dsc_dpcd));
		intel_dp->psr.sink_panel_replay_support = false;
		intel_dp->psr.sink_panel_replay_su_support = false;

		intel_dp_mst_disconnect(intel_dp);

		intel_dp_tunnel_disconnect(intel_dp);

		goto out_unset_edid;
	}

	intel_dp_init_source_oui(intel_dp);

	ret = intel_dp_tunnel_detect(intel_dp, ctx);
	if (ret == -EDEADLK) {
		status = ret;

		goto out_vdd_off;
	}

	if (ret == 1)
		connector->base.epoch_counter++;

	if (!intel_dp_is_edp(intel_dp))
		intel_psr_init_dpcd(intel_dp);

	intel_dp_detect_dsc_caps(intel_dp, connector);

	intel_dp_detect_sdp_caps(intel_dp);

	if (intel_dp->reset_link_params) {
		intel_dp_reset_link_params(intel_dp);
		intel_dp->reset_link_params = false;
	}

	intel_dp_mst_configure(intel_dp);

	intel_dp_print_rates(intel_dp);

	if (intel_dp->is_mst) {
		/*
		 * If we are in MST mode then this connector
		 * won't appear connected or have anything
		 * with EDID on it
		 */
		status = connector_status_disconnected;
		goto out_unset_edid;
	}

	/*
	 * Some external monitors do not signal loss of link synchronization
	 * with an IRQ_HPD, so force a link status check.
	 *
	 * TODO: this probably became redundant, so remove it: the link state
	 * is rechecked/recovered now after modesets, where the loss of
	 * synchronization tends to occur.
	 */
	if (!intel_dp_is_edp(intel_dp))
		intel_dp_check_link_state(intel_dp);

	/*
	 * Clearing NACK and defer counts to get their exact values
	 * while reading EDID which are required by Compliance tests
	 * 4.2.2.4 and 4.2.2.5
	 */
	intel_dp->aux.i2c_nack_count = 0;
	intel_dp->aux.i2c_defer_count = 0;

	intel_dp_set_edid(intel_dp);
	if (intel_dp_is_edp(intel_dp) || connector->detect_edid)
		status = connector_status_connected;

	intel_dp_check_device_service_irq(intel_dp);

out_unset_edid:
	if (status != connector_status_connected && !intel_dp->is_mst)
		intel_dp_unset_edid(intel_dp);

	intel_dp_dpcd_set_probe(intel_dp, false);

	if (!intel_dp_is_edp(intel_dp))
		drm_dp_set_subconnector_property(&connector->base,
						 status,
						 intel_dp->dpcd,
						 intel_dp->downstream_ports);
out_vdd_off:
	intel_pps_vdd_off(intel_dp);

	return status;
}

static void
intel_dp_force(struct drm_connector *_connector)
{
	struct intel_connector *connector = to_intel_connector(_connector);
	struct intel_display *display = to_intel_display(connector);
	struct intel_dp *intel_dp = intel_attached_dp(connector);

	drm_dbg_kms(display->drm, "[CONNECTOR:%d:%s]\n",
		    connector->base.base.id, connector->base.name);

	if (!intel_display_driver_check_access(display))
		return;

	intel_dp_unset_edid(intel_dp);

	if (connector->base.status != connector_status_connected)
		return;

	intel_dp_set_edid(intel_dp);

	intel_dp_dpcd_set_probe(intel_dp, false);
}

static int intel_dp_get_modes(struct drm_connector *_connector)
{
	struct intel_display *display = to_intel_display(_connector->dev);
	struct intel_connector *connector = to_intel_connector(_connector);
	struct intel_dp *intel_dp = intel_attached_dp(connector);
	int num_modes;

	/* drm_edid_connector_update() done in ->detect() or ->force() */
	num_modes = drm_edid_connector_add_modes(&connector->base);

	/* Also add fixed mode, which may or may not be present in EDID */
	if (intel_dp_is_edp(intel_dp))
		num_modes += intel_panel_get_modes(connector);

	if (num_modes)
		return num_modes;

	if (!connector->detect_edid) {
		struct drm_display_mode *mode;

		mode = drm_dp_downstream_mode(display->drm,
					      intel_dp->dpcd,
					      intel_dp->downstream_ports);
		if (mode) {
			drm_mode_probed_add(&connector->base, mode);
			num_modes++;
		}
	}

	return num_modes;
}

static int
intel_dp_connector_register(struct drm_connector *_connector)
{
	struct intel_connector *connector = to_intel_connector(_connector);
	struct intel_display *display = to_intel_display(connector);
	struct intel_dp *intel_dp = intel_attached_dp(connector);
	struct intel_digital_port *dig_port = dp_to_dig_port(intel_dp);
	int ret;

	ret = intel_connector_register(&connector->base);
	if (ret)
		return ret;

	drm_dbg_kms(display->drm, "registering %s bus for %s\n",
		    intel_dp->aux.name, connector->base.kdev->kobj.name);

	intel_dp->aux.dev = connector->base.kdev;
	ret = drm_dp_aux_register(&intel_dp->aux);
	if (!ret)
		drm_dp_cec_register_connector(&intel_dp->aux, &connector->base);

	if (!intel_bios_encoder_is_lspcon(dig_port->base.devdata))
		return ret;

	/*
	 * ToDo: Clean this up to handle lspcon init and resume more
	 * efficiently and streamlined.
	 */
	if (intel_lspcon_init(dig_port)) {
		if (intel_lspcon_detect_hdr_capability(dig_port))
			drm_connector_attach_hdr_output_metadata_property(&connector->base);
	}

	return ret;
}

static void
intel_dp_connector_unregister(struct drm_connector *_connector)
{
	struct intel_connector *connector = to_intel_connector(_connector);
	struct intel_dp *intel_dp = intel_attached_dp(connector);

	drm_dp_cec_unregister_connector(&intel_dp->aux);
	drm_dp_aux_unregister(&intel_dp->aux);
	intel_connector_unregister(&connector->base);
}

void intel_dp_connector_sync_state(struct intel_connector *connector,
				   const struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(connector);

	if (crtc_state && crtc_state->dsc.compression_enable) {
		drm_WARN_ON(display->drm,
			    !connector->dp.dsc_decompression_aux);
		connector->dp.dsc_decompression_enabled = true;
	} else {
		connector->dp.dsc_decompression_enabled = false;
	}
}

void intel_dp_encoder_flush_work(struct drm_encoder *_encoder)
{
	struct intel_encoder *encoder = to_intel_encoder(_encoder);
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);
	struct intel_dp *intel_dp = &dig_port->dp;

	intel_encoder_link_check_flush_work(encoder);

	intel_dp_mst_encoder_cleanup(dig_port);

	intel_dp_tunnel_destroy(intel_dp);

	intel_pps_vdd_off_sync(intel_dp);

	/*
	 * Ensure power off delay is respected on module remove, so that we can
	 * reduce delays at driver probe. See pps_init_timestamps().
	 */
	intel_pps_wait_power_cycle(intel_dp);

	intel_dp_aux_fini(intel_dp);
}

void intel_dp_encoder_suspend(struct intel_encoder *encoder)
{
	struct intel_dp *intel_dp = enc_to_intel_dp(encoder);

	intel_pps_vdd_off_sync(intel_dp);

	intel_dp_tunnel_suspend(intel_dp);
}

void intel_dp_encoder_shutdown(struct intel_encoder *encoder)
{
	struct intel_dp *intel_dp = enc_to_intel_dp(encoder);

	intel_pps_wait_power_cycle(intel_dp);
}

static int intel_modeset_tile_group(struct intel_atomic_state *state,
				    int tile_group_id)
{
	struct intel_display *display = to_intel_display(state);
	struct drm_connector_list_iter conn_iter;
	struct intel_connector *connector;
	int ret = 0;

	drm_connector_list_iter_begin(display->drm, &conn_iter);
	for_each_intel_connector_iter(connector, &conn_iter) {
		struct drm_connector_state *conn_state;
		struct intel_crtc_state *crtc_state;
		struct intel_crtc *crtc;

		if (!connector->base.has_tile ||
		    connector->base.tile_group->id != tile_group_id)
			continue;

		conn_state = drm_atomic_get_connector_state(&state->base,
							    &connector->base);
		if (IS_ERR(conn_state)) {
			ret = PTR_ERR(conn_state);
			break;
		}

		crtc = to_intel_crtc(conn_state->crtc);

		if (!crtc)
			continue;

		crtc_state = intel_atomic_get_new_crtc_state(state, crtc);
		crtc_state->uapi.mode_changed = true;

		ret = drm_atomic_add_affected_planes(&state->base, &crtc->base);
		if (ret)
			break;
	}
	drm_connector_list_iter_end(&conn_iter);

	return ret;
}

static int intel_modeset_affected_transcoders(struct intel_atomic_state *state, u8 transcoders)
{
	struct intel_display *display = to_intel_display(state);
	struct intel_crtc *crtc;

	if (transcoders == 0)
		return 0;

	for_each_intel_crtc(display->drm, crtc) {
		struct intel_crtc_state *crtc_state;
		int ret;

		crtc_state = intel_atomic_get_crtc_state(&state->base, crtc);
		if (IS_ERR(crtc_state))
			return PTR_ERR(crtc_state);

		if (!crtc_state->hw.enable)
			continue;

		if (!(transcoders & BIT(crtc_state->cpu_transcoder)))
			continue;

		crtc_state->uapi.mode_changed = true;

		ret = drm_atomic_add_affected_connectors(&state->base, &crtc->base);
		if (ret)
			return ret;

		ret = drm_atomic_add_affected_planes(&state->base, &crtc->base);
		if (ret)
			return ret;

		transcoders &= ~BIT(crtc_state->cpu_transcoder);
	}

	drm_WARN_ON(display->drm, transcoders != 0);

	return 0;
}

static int intel_modeset_synced_crtcs(struct intel_atomic_state *state,
				      struct drm_connector *_connector)
{
	struct intel_connector *connector = to_intel_connector(_connector);
	const struct drm_connector_state *old_conn_state =
		drm_atomic_get_old_connector_state(&state->base, &connector->base);
	const struct intel_crtc_state *old_crtc_state;
	struct intel_crtc *crtc;
	u8 transcoders;

	crtc = to_intel_crtc(old_conn_state->crtc);
	if (!crtc)
		return 0;

	old_crtc_state = intel_atomic_get_old_crtc_state(state, crtc);

	if (!old_crtc_state->hw.active)
		return 0;

	transcoders = old_crtc_state->sync_mode_slaves_mask;
	if (old_crtc_state->master_transcoder != INVALID_TRANSCODER)
		transcoders |= BIT(old_crtc_state->master_transcoder);

	return intel_modeset_affected_transcoders(state,
						  transcoders);
}

static int intel_dp_connector_atomic_check(struct drm_connector *_connector,
					   struct drm_atomic_state *_state)
{
	struct intel_connector *connector = to_intel_connector(_connector);
	struct intel_display *display = to_intel_display(connector);
	struct intel_atomic_state *state = to_intel_atomic_state(_state);
	struct drm_connector_state *conn_state =
		drm_atomic_get_new_connector_state(_state, &connector->base);
	struct intel_dp *intel_dp = enc_to_intel_dp(connector->encoder);
	int ret;

	ret = intel_digital_connector_atomic_check(&connector->base, &state->base);
	if (ret)
		return ret;

	if (intel_dp_mst_source_support(intel_dp)) {
		ret = drm_dp_mst_root_conn_atomic_check(conn_state, &intel_dp->mst.mgr);
		if (ret)
			return ret;
	}

	if (!intel_connector_needs_modeset(state, &connector->base))
		return 0;

	ret = intel_dp_tunnel_atomic_check_state(state,
						 intel_dp,
						 connector);
	if (ret)
		return ret;

	/*
	 * We don't enable port sync on BDW due to missing w/as and
	 * due to not having adjusted the modeset sequence appropriately.
	 */
	if (DISPLAY_VER(display) < 9)
		return 0;

	if (connector->base.has_tile) {
		ret = intel_modeset_tile_group(state, connector->base.tile_group->id);
		if (ret)
			return ret;
	}

	return intel_modeset_synced_crtcs(state, &connector->base);
}

static void intel_dp_oob_hotplug_event(struct drm_connector *_connector,
				       enum drm_connector_status hpd_state)
{
	struct intel_connector *connector = to_intel_connector(_connector);
	struct intel_display *display = to_intel_display(connector);
	struct intel_encoder *encoder = intel_attached_encoder(connector);
	bool hpd_high = hpd_state == connector_status_connected;
	unsigned int hpd_pin = encoder->hpd_pin;
	bool need_work = false;

	spin_lock_irq(&display->irq.lock);
	if (hpd_high != test_bit(hpd_pin, &display->hotplug.oob_hotplug_last_state)) {
		display->hotplug.event_bits |= BIT(hpd_pin);

		__assign_bit(hpd_pin,
			     &display->hotplug.oob_hotplug_last_state,
			     hpd_high);
		need_work = true;
	}
	spin_unlock_irq(&display->irq.lock);

	if (need_work)
		intel_hpd_schedule_detection(display);
}

static const struct drm_connector_funcs intel_dp_connector_funcs = {
	.force = intel_dp_force,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.atomic_get_property = intel_digital_connector_atomic_get_property,
	.atomic_set_property = intel_digital_connector_atomic_set_property,
	.late_register = intel_dp_connector_register,
	.early_unregister = intel_dp_connector_unregister,
	.destroy = intel_connector_destroy,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
	.atomic_duplicate_state = intel_digital_connector_duplicate_state,
	.oob_hotplug_event = intel_dp_oob_hotplug_event,
};

static const struct drm_connector_helper_funcs intel_dp_connector_helper_funcs = {
	.detect_ctx = intel_dp_detect,
	.get_modes = intel_dp_get_modes,
	.mode_valid = intel_dp_mode_valid,
	.atomic_check = intel_dp_connector_atomic_check,
};

enum irqreturn
intel_dp_hpd_pulse(struct intel_digital_port *dig_port, bool long_hpd)
{
	struct intel_display *display = to_intel_display(dig_port);
	struct intel_dp *intel_dp = &dig_port->dp;
	u8 dpcd[DP_RECEIVER_CAP_SIZE];

	if (dig_port->base.type == INTEL_OUTPUT_EDP &&
	    (long_hpd ||
	     intel_display_rpm_suspended(display) ||
	     !intel_pps_have_panel_power_or_vdd(intel_dp))) {
		/*
		 * vdd off can generate a long/short pulse on eDP which
		 * would require vdd on to handle it, and thus we
		 * would end up in an endless cycle of
		 * "vdd off -> long/short hpd -> vdd on -> detect -> vdd off -> ..."
		 */
		drm_dbg_kms(display->drm,
			    "ignoring %s hpd on eDP [ENCODER:%d:%s]\n",
			    long_hpd ? "long" : "short",
			    dig_port->base.base.base.id,
			    dig_port->base.base.name);
		return IRQ_HANDLED;
	}

	drm_dbg_kms(display->drm, "got hpd irq on [ENCODER:%d:%s] - %s\n",
		    dig_port->base.base.base.id,
		    dig_port->base.base.name,
		    long_hpd ? "long" : "short");

	/*
	 * TBT DP tunnels require the GFX driver to read out the DPRX caps in
	 * response to long HPD pulses. The DP hotplug handler does that,
	 * however the hotplug handler may be blocked by another
	 * connector's/encoder's hotplug handler. Since the TBT CM may not
	 * complete the DP tunnel BW request for the latter connector/encoder
	 * waiting for this encoder's DPRX read, perform a dummy read here.
	 */
	if (long_hpd) {
		intel_dp_dpcd_set_probe(intel_dp, true);

		intel_dp_read_dprx_caps(intel_dp, dpcd);

		intel_dp->reset_link_params = true;
		intel_dp_invalidate_source_oui(intel_dp);

		return IRQ_NONE;
	}

	if (intel_dp->is_mst) {
		if (!intel_dp_check_mst_status(intel_dp))
			return IRQ_NONE;
	} else if (!intel_dp_short_pulse(intel_dp)) {
		return IRQ_NONE;
	}

	return IRQ_HANDLED;
}

static bool _intel_dp_is_port_edp(struct intel_display *display,
				  const struct intel_bios_encoder_data *devdata,
				  enum port port)
{
	/*
	 * eDP not supported on g4x. so bail out early just
	 * for a bit extra safety in case the VBT is bonkers.
	 */
	if (DISPLAY_VER(display) < 5)
		return false;

	if (DISPLAY_VER(display) < 9 && port == PORT_A)
		return true;

	return devdata && intel_bios_encoder_supports_edp(devdata);
}

bool intel_dp_is_port_edp(struct intel_display *display, enum port port)
{
	const struct intel_bios_encoder_data *devdata =
		intel_bios_encoder_data_lookup(display, port);

	return _intel_dp_is_port_edp(display, devdata, port);
}

bool
intel_dp_has_gamut_metadata_dip(struct intel_encoder *encoder)
{
	struct intel_display *display = to_intel_display(encoder);
	enum port port = encoder->port;

	if (intel_bios_encoder_is_lspcon(encoder->devdata))
		return false;

	if (DISPLAY_VER(display) >= 11)
		return true;

	if (port == PORT_A)
		return false;

	if (display->platform.haswell || display->platform.broadwell ||
	    DISPLAY_VER(display) >= 9)
		return true;

	return false;
}

static void
intel_dp_add_properties(struct intel_dp *intel_dp, struct drm_connector *_connector)
{
	struct intel_connector *connector = to_intel_connector(_connector);
	struct intel_display *display = to_intel_display(intel_dp);
	enum port port = dp_to_dig_port(intel_dp)->base.port;

	if (!intel_dp_is_edp(intel_dp))
		drm_connector_attach_dp_subconnector_property(&connector->base);

	if (!display->platform.g4x && port != PORT_A)
		intel_attach_force_audio_property(&connector->base);

	intel_attach_broadcast_rgb_property(&connector->base);
	if (HAS_GMCH(display))
		drm_connector_attach_max_bpc_property(&connector->base, 6, 10);
	else if (DISPLAY_VER(display) >= 5)
		drm_connector_attach_max_bpc_property(&connector->base, 6, 12);

	/* Register HDMI colorspace for case of lspcon */
	if (intel_bios_encoder_is_lspcon(dp_to_dig_port(intel_dp)->base.devdata)) {
		drm_connector_attach_content_type_property(&connector->base);
		intel_attach_hdmi_colorspace_property(&connector->base);
	} else {
		intel_attach_dp_colorspace_property(&connector->base);
	}

	if (intel_dp_has_gamut_metadata_dip(&dp_to_dig_port(intel_dp)->base))
		drm_connector_attach_hdr_output_metadata_property(&connector->base);

	if (HAS_VRR(display))
		drm_connector_attach_vrr_capable_property(&connector->base);
}

static void
intel_edp_add_properties(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct intel_connector *connector = intel_dp->attached_connector;
	const struct drm_display_mode *fixed_mode =
		intel_panel_preferred_fixed_mode(connector);

	intel_attach_scaling_mode_property(&connector->base);

	drm_connector_set_panel_orientation_with_quirk(&connector->base,
						       display->vbt.orientation,
						       fixed_mode->hdisplay,
						       fixed_mode->vdisplay);
}

static void intel_edp_backlight_setup(struct intel_dp *intel_dp,
				      struct intel_connector *connector)
{
	struct intel_display *display = to_intel_display(intel_dp);
	enum pipe pipe = INVALID_PIPE;

	if (display->platform.valleyview || display->platform.cherryview)
		pipe = vlv_pps_backlight_initial_pipe(intel_dp);

	intel_backlight_setup(connector, pipe);
}

static bool intel_edp_init_connector(struct intel_dp *intel_dp,
				     struct intel_connector *connector)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct drm_display_mode *fixed_mode;
	struct intel_encoder *encoder = &dp_to_dig_port(intel_dp)->base;
	bool has_dpcd;
	const struct drm_edid *drm_edid;

	if (!intel_dp_is_edp(intel_dp))
		return true;

	/*
	 * On IBX/CPT we may get here with LVDS already registered. Since the
	 * driver uses the only internal power sequencer available for both
	 * eDP and LVDS bail out early in this case to prevent interfering
	 * with an already powered-on LVDS power sequencer.
	 */
	if (intel_get_lvds_encoder(display)) {
		drm_WARN_ON(display->drm,
			    !(HAS_PCH_IBX(display) || HAS_PCH_CPT(display)));
		drm_info(display->drm,
			 "LVDS was detected, not registering eDP\n");

		return false;
	}

	intel_bios_init_panel_early(display, &connector->panel,
				    encoder->devdata);

	if (!intel_pps_init(intel_dp)) {
		drm_info(display->drm,
			 "[ENCODER:%d:%s] unusable PPS, disabling eDP\n",
			 encoder->base.base.id, encoder->base.name);
		/*
		 * The BIOS may have still enabled VDD on the PPS even
		 * though it's unusable. Make sure we turn it back off
		 * and to release the power domain references/etc.
		 */
		goto out_vdd_off;
	}

	/*
	 * Enable HPD sense for live status check.
	 * intel_hpd_irq_setup() will turn it off again
	 * if it's no longer needed later.
	 *
	 * The DPCD probe below will make sure VDD is on.
	 */
	intel_hpd_enable_detection(encoder);

	intel_alpm_init(intel_dp);

	/* Cache DPCD and EDID for edp. */
	has_dpcd = intel_edp_init_dpcd(intel_dp, connector);

	if (!has_dpcd) {
		/* if this fails, presume the device is a ghost */
		drm_info(display->drm,
			 "[ENCODER:%d:%s] failed to retrieve link info, disabling eDP\n",
			 encoder->base.base.id, encoder->base.name);
		goto out_vdd_off;
	}

	/*
	 * VBT and straps are liars. Also check HPD as that seems
	 * to be the most reliable piece of information available.
	 *
	 * ... expect on devices that forgot to hook HPD up for eDP
	 * (eg. Acer Chromebook C710), so we'll check it only if multiple
	 * ports are attempting to use the same AUX CH, according to VBT.
	 */
	if (intel_bios_dp_has_shared_aux_ch(encoder->devdata)) {
		/*
		 * If this fails, presume the DPCD answer came
		 * from some other port using the same AUX CH.
		 *
		 * FIXME maybe cleaner to check this before the
		 * DPCD read? Would need sort out the VDD handling...
		 */
		if (!intel_digital_port_connected(encoder)) {
			drm_info(display->drm,
				 "[ENCODER:%d:%s] HPD is down, disabling eDP\n",
				 encoder->base.base.id, encoder->base.name);
			goto out_vdd_off;
		}

		/*
		 * Unfortunately even the HPD based detection fails on
		 * eg. Asus B360M-A (CFL+CNP), so as a last resort fall
		 * back to checking for a VGA branch device. Only do this
		 * on known affected platforms to minimize false positives.
		 */
		if (DISPLAY_VER(display) == 9 && drm_dp_is_branch(intel_dp->dpcd) &&
		    (intel_dp->dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DWN_STRM_PORT_TYPE_MASK) ==
		    DP_DWN_STRM_PORT_TYPE_ANALOG) {
			drm_info(display->drm,
				 "[ENCODER:%d:%s] VGA converter detected, disabling eDP\n",
				 encoder->base.base.id, encoder->base.name);
			goto out_vdd_off;
		}
	}

	mutex_lock(&display->drm->mode_config.mutex);
	drm_edid = drm_edid_read_ddc(&connector->base, connector->base.ddc);
	if (!drm_edid) {
		/* Fallback to EDID from ACPI OpRegion, if any */
		drm_edid = intel_opregion_get_edid(connector);
		if (drm_edid)
			drm_dbg_kms(display->drm,
				    "[CONNECTOR:%d:%s] Using OpRegion EDID\n",
				    connector->base.base.id, connector->base.name);
	}
	if (drm_edid) {
		if (drm_edid_connector_update(&connector->base, drm_edid) ||
		    !drm_edid_connector_add_modes(&connector->base)) {
			drm_edid_connector_update(&connector->base, NULL);
			drm_edid_free(drm_edid);
			drm_edid = ERR_PTR(-EINVAL);
		}
	} else {
		drm_edid = ERR_PTR(-ENOENT);
	}

	intel_bios_init_panel_late(display, &connector->panel, encoder->devdata,
				   IS_ERR(drm_edid) ? NULL : drm_edid);

	intel_panel_add_edid_fixed_modes(connector, true);

	/* MSO requires information from the EDID */
	intel_edp_mso_init(intel_dp);

	/* multiply the mode clock and horizontal timings for MSO */
	list_for_each_entry(fixed_mode, &connector->panel.fixed_modes, head)
		intel_edp_mso_mode_fixup(connector, fixed_mode);

	/* fallback to VBT if available for eDP */
	if (!intel_panel_preferred_fixed_mode(connector))
		intel_panel_add_vbt_lfp_fixed_mode(connector);

	mutex_unlock(&display->drm->mode_config.mutex);

	if (!intel_panel_preferred_fixed_mode(connector)) {
		drm_info(display->drm,
			 "[ENCODER:%d:%s] failed to find fixed mode for the panel, disabling eDP\n",
			 encoder->base.base.id, encoder->base.name);
		goto out_vdd_off;
	}

	intel_panel_init(connector, drm_edid);

	intel_edp_backlight_setup(intel_dp, connector);

	intel_edp_add_properties(intel_dp);

	intel_pps_init_late(intel_dp);

	return true;

out_vdd_off:
	intel_pps_vdd_off_sync(intel_dp);
	intel_bios_fini_panel(&connector->panel);

	return false;
}

bool
intel_dp_init_connector(struct intel_digital_port *dig_port,
			struct intel_connector *connector)
{
	struct intel_display *display = to_intel_display(dig_port);
	struct intel_dp *intel_dp = &dig_port->dp;
	struct intel_encoder *encoder = &dig_port->base;
	struct drm_device *dev = encoder->base.dev;
	enum port port = encoder->port;
	int type;

	if (drm_WARN(dev, dig_port->max_lanes < 1,
		     "Not enough lanes (%d) for DP on [ENCODER:%d:%s]\n",
		     dig_port->max_lanes, encoder->base.base.id,
		     encoder->base.name))
		return false;

	intel_dp->reset_link_params = true;

	/* Preserve the current hw state. */
	intel_dp->DP = intel_de_read(display, intel_dp->output_reg);
	intel_dp->attached_connector = connector;

	if (_intel_dp_is_port_edp(display, encoder->devdata, port)) {
		/*
		 * Currently we don't support eDP on TypeC ports for DISPLAY_VER < 30,
		 * although in theory it could work on TypeC legacy ports.
		 */
		drm_WARN_ON(dev, intel_encoder_is_tc(encoder) &&
			    DISPLAY_VER(display) < 30);
		type = DRM_MODE_CONNECTOR_eDP;
		encoder->type = INTEL_OUTPUT_EDP;

		/* eDP only on port B and/or C on vlv/chv */
		if (drm_WARN_ON(dev, (display->platform.valleyview ||
				      display->platform.cherryview) &&
				port != PORT_B && port != PORT_C))
			return false;
	} else {
		type = DRM_MODE_CONNECTOR_DisplayPort;
	}

	intel_dp_set_default_sink_rates(intel_dp);
	intel_dp_set_default_max_sink_lane_count(intel_dp);

	if (display->platform.valleyview || display->platform.cherryview)
		vlv_pps_pipe_init(intel_dp);

	intel_dp_aux_init(intel_dp);
	connector->dp.dsc_decompression_aux = &intel_dp->aux;

	drm_dbg_kms(display->drm,
		    "Adding %s connector on [ENCODER:%d:%s]\n",
		    type == DRM_MODE_CONNECTOR_eDP ? "eDP" : "DP",
		    encoder->base.base.id, encoder->base.name);

	drm_connector_init_with_ddc(dev, &connector->base, &intel_dp_connector_funcs,
				    type, &intel_dp->aux.ddc);
	drm_connector_helper_add(&connector->base, &intel_dp_connector_helper_funcs);

	if (!HAS_GMCH(display) && DISPLAY_VER(display) < 12)
		connector->base.interlace_allowed = true;

	if (type != DRM_MODE_CONNECTOR_eDP)
		connector->polled = DRM_CONNECTOR_POLL_HPD;
	connector->base.polled = connector->polled;

	intel_connector_attach_encoder(connector, encoder);

	if (HAS_DDI(display))
		connector->get_hw_state = intel_ddi_connector_get_hw_state;
	else
		connector->get_hw_state = intel_connector_get_hw_state;
	connector->sync_state = intel_dp_connector_sync_state;

	if (!intel_edp_init_connector(intel_dp, connector)) {
		intel_dp_aux_fini(intel_dp);
		goto fail;
	}

	intel_dp_set_source_rates(intel_dp);
	intel_dp_set_common_rates(intel_dp);
	intel_dp_reset_link_params(intel_dp);

	/* init MST on ports that can support it */
	intel_dp_mst_encoder_init(dig_port, connector->base.base.id);

	intel_dp_add_properties(intel_dp, &connector->base);

	if (is_hdcp_supported(display, port) && !intel_dp_is_edp(intel_dp)) {
		int ret = intel_dp_hdcp_init(dig_port, connector);
		if (ret)
			drm_dbg_kms(display->drm,
				    "HDCP init failed, skipping.\n");
	}

	intel_dp->frl.is_trained = false;
	intel_dp->frl.trained_rate_gbps = 0;

	intel_psr_init(intel_dp);

	return true;

fail:
	intel_display_power_flush_work(display);
	drm_connector_cleanup(&connector->base);

	return false;
}

void intel_dp_mst_suspend(struct intel_display *display)
{
	struct intel_encoder *encoder;

	if (!HAS_DISPLAY(display))
		return;

	for_each_intel_encoder(display->drm, encoder) {
		struct intel_dp *intel_dp;

		if (encoder->type != INTEL_OUTPUT_DDI)
			continue;

		intel_dp = enc_to_intel_dp(encoder);

		if (!intel_dp_mst_source_support(intel_dp))
			continue;

		if (intel_dp->is_mst)
			drm_dp_mst_topology_mgr_suspend(&intel_dp->mst.mgr);
	}
}

void intel_dp_mst_resume(struct intel_display *display)
{
	struct intel_encoder *encoder;

	if (!HAS_DISPLAY(display))
		return;

	for_each_intel_encoder(display->drm, encoder) {
		struct intel_dp *intel_dp;
		int ret;

		if (encoder->type != INTEL_OUTPUT_DDI)
			continue;

		intel_dp = enc_to_intel_dp(encoder);

		if (!intel_dp_mst_source_support(intel_dp))
			continue;

		ret = drm_dp_mst_topology_mgr_resume(&intel_dp->mst.mgr, true);
		if (ret) {
			intel_dp->is_mst = false;
			drm_dp_mst_topology_mgr_set_mst(&intel_dp->mst.mgr, false);
		}
	}
}
