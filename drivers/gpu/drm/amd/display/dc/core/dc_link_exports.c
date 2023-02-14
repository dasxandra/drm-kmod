/*
 * Copyright 2023 Advanced Micro Devices, Inc.
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

/* FILE POLICY AND INTENDED USAGE:
 * This file provides single entrance to link functionality declared in dc
 * public headers. The file is intended to be used as a thin translation layer
 * that directly calls link internal functions without adding new functional
 * behavior.
 *
 * When exporting a new link related dc function, add function declaration in
 * dc.h with detail interface documentation, then add function implementation
 * in this file which calls link functions.
 */
#include "link.h"
#include "dce/dce_i2c.h"
struct dc_link *dc_get_link_at_index(struct dc *dc, uint32_t link_index)
{
	return dc->links[link_index];
}

void dc_get_edp_links(const struct dc *dc,
		struct dc_link **edp_links,
		int *edp_num)
{
	int i;

	*edp_num = 0;
	for (i = 0; i < dc->link_count; i++) {
		// report any eDP links, even unconnected DDI's
		if (!dc->links[i])
			continue;
		if (dc->links[i]->connector_signal == SIGNAL_TYPE_EDP) {
			edp_links[*edp_num] = dc->links[i];
			if (++(*edp_num) == MAX_NUM_EDP)
				return;
		}
	}
}

bool dc_get_edp_link_panel_inst(const struct dc *dc,
		const struct dc_link *link,
		unsigned int *inst_out)
{
	struct dc_link *edp_links[MAX_NUM_EDP];
	int edp_num, i;

	*inst_out = 0;
	if (link->connector_signal != SIGNAL_TYPE_EDP)
		return false;
	dc_get_edp_links(dc, edp_links, &edp_num);
	for (i = 0; i < edp_num; i++) {
		if (link == edp_links[i])
			break;
		(*inst_out)++;
	}
	return true;
}

bool dc_link_detect(struct dc_link *link, enum dc_detect_reason reason)
{
	return link_detect(link, reason);
}

bool dc_link_detect_connection_type(struct dc_link *link,
		enum dc_connection_type *type)
{
	return link_detect_connection_type(link, type);
}

const struct dc_link_status *dc_link_get_status(const struct dc_link *link)
{
	return link_get_status(link);
}

/* return true if the connected receiver supports the hdcp version */
bool dc_link_is_hdcp14(struct dc_link *link, enum signal_type signal)
{
	return link_is_hdcp14(link, signal);
}

bool dc_link_is_hdcp22(struct dc_link *link, enum signal_type signal)
{
	return link_is_hdcp22(link, signal);
}

void dc_link_clear_dprx_states(struct dc_link *link)
{
	link_clear_dprx_states(link);
}

bool dc_link_reset_cur_dp_mst_topology(struct dc_link *link)
{
	return link_reset_cur_dp_mst_topology(link);
}

uint32_t dc_link_bandwidth_kbps(
	const struct dc_link *link,
	const struct dc_link_settings *link_settings)
{
	return dp_link_bandwidth_kbps(link, link_settings);
}

uint32_t dc_bandwidth_in_kbps_from_timing(
	const struct dc_crtc_timing *timing)
{
	return link_timing_bandwidth_kbps(timing);
}

void dc_get_cur_link_res_map(const struct dc *dc, uint32_t *map)
{
	link_get_cur_res_map(dc, map);
}

void dc_restore_link_res_map(const struct dc *dc, uint32_t *map)
{
	link_restore_res_map(dc, map);
}

bool dc_link_update_dsc_config(struct pipe_ctx *pipe_ctx)
{
	return link_update_dsc_config(pipe_ctx);
}

bool dc_is_oem_i2c_device_present(
	struct dc *dc,
	size_t slave_address)
{
	if (dc->res_pool->oem_device)
		return dce_i2c_oem_device_present(
			dc->res_pool,
			dc->res_pool->oem_device,
			slave_address);

	return false;
}

bool dc_submit_i2c(
		struct dc *dc,
		uint32_t link_index,
		struct i2c_command *cmd)
{

	struct dc_link *link = dc->links[link_index];
	struct ddc_service *ddc = link->ddc;

	return dce_i2c_submit_command(
		dc->res_pool,
		ddc->ddc_pin,
		cmd);
}

bool dc_submit_i2c_oem(
		struct dc *dc,
		struct i2c_command *cmd)
{
	struct ddc_service *ddc = dc->res_pool->oem_device;

	if (ddc)
		return dce_i2c_submit_command(
			dc->res_pool,
			ddc->ddc_pin,
			cmd);

	return false;
}

void dc_link_dp_handle_automated_test(struct dc_link *link)
{
	link->dc->link_srv->dp_handle_automated_test(link);
}

bool dc_link_dp_set_test_pattern(
	struct dc_link *link,
	enum dp_test_pattern test_pattern,
	enum dp_test_pattern_color_space test_pattern_color_space,
	const struct link_training_settings *p_link_settings,
	const unsigned char *p_custom_pattern,
	unsigned int cust_pattern_size)
{
	return link->dc->link_srv->dp_set_test_pattern(link, test_pattern,
			test_pattern_color_space, p_link_settings,
			p_custom_pattern, cust_pattern_size);
}

void dc_link_set_drive_settings(struct dc *dc,
				struct link_training_settings *lt_settings,
				struct dc_link *link)
{
	struct link_resource link_res;

	link_get_cur_link_res(link, &link_res);
	dp_set_drive_settings(link, &link_res, lt_settings);
}

void dc_link_set_preferred_link_settings(struct dc *dc,
					 struct dc_link_settings *link_setting,
					 struct dc_link *link)
{
	dc->link_srv->dp_set_preferred_link_settings(dc, link_setting, link);
}

void dc_link_set_preferred_training_settings(struct dc *dc,
		struct dc_link_settings *link_setting,
		struct dc_link_training_overrides *lt_overrides,
		struct dc_link *link,
		bool skip_immediate_retrain)
{
	dc->link_srv->dp_set_preferred_training_settings(dc, link_setting,
			lt_overrides, link, skip_immediate_retrain);
}

bool dc_dp_trace_is_initialized(struct dc_link *link)
{
	return link->dc->link_srv->dp_trace_is_initialized(link);
}

void dc_dp_trace_set_is_logged_flag(struct dc_link *link,
		bool in_detection,
		bool is_logged)
{
	link->dc->link_srv->dp_trace_set_is_logged_flag(link, in_detection, is_logged);
}

bool dc_dp_trace_is_logged(struct dc_link *link, bool in_detection)
{
	return link->dc->link_srv->dp_trace_is_logged(link, in_detection);
}

unsigned long long dc_dp_trace_get_lt_end_timestamp(struct dc_link *link,
		bool in_detection)
{
	return link->dc->link_srv->dp_trace_get_lt_end_timestamp(link, in_detection);
}

const struct dp_trace_lt_counts *dc_dp_trace_get_lt_counts(struct dc_link *link,
		bool in_detection)
{
	return link->dc->link_srv->dp_trace_get_lt_counts(link, in_detection);
}

unsigned int dc_dp_trace_get_link_loss_count(struct dc_link *link)
{
	return link->dc->link_srv->dp_trace_get_link_loss_count(link);
}
