/*
 * Copyright 2015 Advanced Micro Devices, Inc.
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

#ifndef __DC_LINK_DP_H__
#define __DC_LINK_DP_H__

#define LINK_TRAINING_ATTEMPTS 4
#define LINK_TRAINING_RETRY_DELAY 50 /* ms */
#define LINK_AUX_DEFAULT_LTTPR_TIMEOUT_PERIOD 3200 /*us*/
#define LINK_AUX_DEFAULT_TIMEOUT_PERIOD 552 /*us*/
#define MAX_MTP_SLOT_COUNT 64
#define TRAINING_AUX_RD_INTERVAL 100 //us
#define LINK_AUX_WAKE_TIMEOUT_MS 1500 // Timeout when trying to wake unresponsive DPRX.

struct dc_link;
struct dc_stream_state;
struct dc_link_settings;

enum {
	/*
	 * Some receivers fail to train on first try and are good
	 * on subsequent tries. 2 retries should be plenty. If we
	 * don't have a successful training then we don't expect to
	 * ever get one.
	 */
	LINK_TRAINING_MAX_VERIFY_RETRY = 2,
	PEAK_FACTOR_X1000 = 1006,
};

struct dc_link_settings dp_get_max_link_cap(struct dc_link *link);

bool dp_verify_link_cap_with_retries(
	struct dc_link *link,
	struct dc_link_settings *known_limit_link_setting,
	int attempts);

bool dp_validate_mode_timing(
	struct dc_link *link,
	const struct dc_crtc_timing *timing);

bool decide_edp_link_settings(struct dc_link *link,
		struct dc_link_settings *link_setting,
		uint32_t req_bw);

bool decide_link_settings(
	struct dc_stream_state *stream,
	struct dc_link_settings *link_setting);

bool hpd_rx_irq_check_link_loss_status(struct dc_link *link,
				       union hpd_irq_data *hpd_irq_dpcd_data);

bool is_mst_supported(struct dc_link *link);

bool detect_dp_sink_caps(struct dc_link *link);

void detect_edp_sink_caps(struct dc_link *link);

bool is_dp_active_dongle(const struct dc_link *link);

bool is_dp_branch_device(const struct dc_link *link);

bool is_edp_ilr_optimization_required(struct dc_link *link, struct dc_crtc_timing *crtc_timing);

void dp_enable_mst_on_sink(struct dc_link *link, bool enable);

enum dp_panel_mode dp_get_panel_mode(struct dc_link *link);
void dp_set_panel_mode(struct dc_link *link, enum dp_panel_mode panel_mode);

bool dp_overwrite_extended_receiver_cap(struct dc_link *link);

void dpcd_set_source_specific_data(struct dc_link *link);

void dpcd_write_cable_id_to_dprx(struct dc_link *link);

enum dc_status dp_set_fec_ready(struct dc_link *link, const struct link_resource *link_res, bool ready);
void dp_set_fec_enable(struct dc_link *link, bool enable);
bool dp_set_dsc_enable(struct pipe_ctx *pipe_ctx, bool enable);
bool dp_set_dsc_pps_sdp(struct pipe_ctx *pipe_ctx, bool enable, bool immediate_update);
void dp_set_dsc_on_stream(struct pipe_ctx *pipe_ctx, bool enable);
bool dp_update_dsc_config(struct pipe_ctx *pipe_ctx);
bool dp_set_dsc_on_rx(struct pipe_ctx *pipe_ctx, bool enable);

/* Initialize output parameter lt_settings. */
void dp_decide_training_settings(
	struct dc_link *link,
	const struct dc_link_settings *link_setting,
	struct link_training_settings *lt_settings);

/* Convert PHY repeater count read from DPCD uint8_t. */
uint8_t dp_convert_to_count(uint8_t lttpr_repeater_count);

enum dp_link_encoding dp_get_link_encoding_format(const struct dc_link_settings *link_settings);
enum dc_status dp_retrieve_lttpr_cap(struct dc_link *link);
bool dp_is_lttpr_present(struct dc_link *link);
bool dpcd_write_128b_132b_sst_payload_allocation_table(
		const struct dc_stream_state *stream,
		struct dc_link *link,
		struct link_mst_stream_allocation_table *proposed_table,
		bool allocate);

bool dpcd_poll_for_allocation_change_trigger(struct dc_link *link);

struct fixed31_32 calculate_sst_avg_time_slots_per_mtp(
		const struct dc_stream_state *stream,
		const struct dc_link *link);
void enable_dp_hpo_output(struct dc_link *link,
		const struct link_resource *link_res,
		const struct dc_link_settings *link_settings);
void disable_dp_hpo_output(struct dc_link *link,
		const struct link_resource *link_res,
		enum signal_type signal);
void setup_dp_hpo_stream(struct pipe_ctx *pipe_ctx, bool enable);
bool is_dp_128b_132b_signal(struct pipe_ctx *pipe_ctx);
void edp_panel_backlight_power_on(struct dc_link *link, bool wait_for_hpd);
void dp_receiver_power_ctrl(struct dc_link *link, bool on);
void dp_source_sequence_trace(struct dc_link *link, uint8_t dp_test_mode);
void dp_enable_link_phy(
	struct dc_link *link,
	const struct link_resource *link_res,
	enum signal_type signal,
	enum clock_source_id clock_source,
	const struct dc_link_settings *link_settings);
void edp_add_delay_for_T9(struct dc_link *link);
bool edp_receiver_ready_T9(struct dc_link *link);
bool edp_receiver_ready_T7(struct dc_link *link);

void dp_disable_link_phy(struct dc_link *link, const struct link_resource *link_res,
		enum signal_type signal);

void dp_disable_link_phy_mst(struct dc_link *link, const struct link_resource *link_res,
		enum signal_type signal);

void dp_set_hw_lane_settings(
		struct dc_link *link,
		const struct link_resource *link_res,
		const struct link_training_settings *link_settings,
		uint32_t offset);

void dp_retrain_link_dp_test(struct dc_link *link,
		struct dc_link_settings *link_setting,
		bool skip_video_pattern);

bool decide_fallback_link_setting(
		struct dc_link *link,
		struct dc_link_settings *max,
		struct dc_link_settings *cur,
		enum link_training_result training_result);

#endif /* __DC_LINK_DP_H__ */
