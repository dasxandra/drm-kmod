/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_DMC_H__
#define __INTEL_DMC_H__

#include "i915_reg_defs.h"
#include "intel_wakeref.h"
#include <linux/workqueue.h>

struct drm_i915_error_state_buf;
struct drm_i915_private;

enum pipe;

enum intel_dmc_id {
	DMC_FW_MAIN = 0,
	DMC_FW_PIPEA,
	DMC_FW_PIPEB,
	DMC_FW_PIPEC,
	DMC_FW_PIPED,
	DMC_FW_MAX
};

struct intel_dmc {
	struct work_struct work;
	const char *fw_path;
	u32 max_fw_size; /* bytes */
	u32 version;
	struct dmc_fw_info {
		u32 mmio_count;
		i915_reg_t mmioaddr[20];
		u32 mmiodata[20];
		u32 dmc_offset;
		u32 start_mmioaddr;
		u32 dmc_fw_size; /*dwords */
		u32 *payload;
		bool present;
	} dmc_info[DMC_FW_MAX];

	u32 dc_state;
	u32 target_dc_state;
	u32 allowed_dc_mask;
	intel_wakeref_t wakeref;
};

void intel_dmc_init(struct drm_i915_private *i915);
void intel_dmc_load_program(struct drm_i915_private *i915);
void intel_dmc_disable_program(struct drm_i915_private *i915);
void intel_dmc_enable_pipe(struct drm_i915_private *i915, enum pipe pipe);
void intel_dmc_disable_pipe(struct drm_i915_private *i915, enum pipe pipe);
void intel_dmc_fini(struct drm_i915_private *i915);
void intel_dmc_suspend(struct drm_i915_private *i915);
void intel_dmc_resume(struct drm_i915_private *i915);
bool intel_dmc_has_payload(struct drm_i915_private *i915);
void intel_dmc_debugfs_register(struct drm_i915_private *i915);
void intel_dmc_print_error_state(struct drm_i915_error_state_buf *m,
				 struct drm_i915_private *i915);

void assert_dmc_loaded(struct drm_i915_private *i915);

#endif /* __INTEL_DMC_H__ */
