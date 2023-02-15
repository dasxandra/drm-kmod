/*
 * Copyright 2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */

#ifndef __DC_LINK_EDP_PANEL_CONTROL_H__
#define __DC_LINK_EDP_PANEL_CONTROL_H__
#include "link.h"

enum dp_panel_mode dp_get_panel_mode(struct dc_link *link);
void dp_set_panel_mode(struct dc_link *link, enum dp_panel_mode panel_mode);
bool set_default_brightness_aux(struct dc_link *link);
void edp_panel_backlight_power_on(struct dc_link *link, bool wait_for_hpd);
int edp_get_backlight_level(const struct dc_link *link);
bool edp_get_backlight_level_nits(struct dc_link *link,
		uint32_t *backlight_millinits_avg,
		uint32_t *backlight_millinits_peak);
bool edp_set_backlight_level(const struct dc_link *link,
		uint32_t backlight_pwm_u16_16,
		uint32_t frame_ramp);
bool edp_set_backlight_level_nits(struct dc_link *link,
		bool isHDR,
		uint32_t backlight_millinits,
		uint32_t transition_time_in_ms);
int edp_get_target_backlight_pwm(const struct dc_link *link);
bool edp_get_psr_state(const struct dc_link *link, enum dc_psr_state *state);
bool edp_set_psr_allow_active(struct dc_link *link, const bool *allow_active,
		bool wait, bool force_static, const unsigned int *power_opts);
bool edp_setup_psr(struct dc_link *link,
		const struct dc_stream_state *stream, struct psr_config *psr_config,
		struct psr_context *psr_context);
bool edp_wait_for_t12(struct dc_link *link);
#endif /* __DC_LINK_EDP_POWER_CONTROL_H__ */
