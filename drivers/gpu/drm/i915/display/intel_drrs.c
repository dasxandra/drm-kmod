// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include "i915_drv.h"
#include "intel_atomic.h"
#include "intel_de.h"
#include "intel_display_types.h"
#include "intel_drrs.h"
#include "intel_panel.h"

/**
 * DOC: Display Refresh Rate Switching (DRRS)
 *
 * Display Refresh Rate Switching (DRRS) is a power conservation feature
 * which enables swtching between low and high refresh rates,
 * dynamically, based on the usage scenario. This feature is applicable
 * for internal panels.
 *
 * Indication that the panel supports DRRS is given by the panel EDID, which
 * would list multiple refresh rates for one resolution.
 *
 * DRRS is of 2 types - static and seamless.
 * Static DRRS involves changing refresh rate (RR) by doing a full modeset
 * (may appear as a blink on screen) and is used in dock-undock scenario.
 * Seamless DRRS involves changing RR without any visual effect to the user
 * and can be used during normal system usage. This is done by programming
 * certain registers.
 *
 * Support for static/seamless DRRS may be indicated in the VBT based on
 * inputs from the panel spec.
 *
 * DRRS saves power by switching to low RR based on usage scenarios.
 *
 * The implementation is based on frontbuffer tracking implementation.  When
 * there is a disturbance on the screen triggered by user activity or a periodic
 * system activity, DRRS is disabled (RR is changed to high RR).  When there is
 * no movement on screen, after a timeout of 1 second, a switch to low RR is
 * made.
 *
 * For integration with frontbuffer tracking code, intel_drrs_invalidate()
 * and intel_drrs_flush() are called.
 *
 * DRRS can be further extended to support other internal panels and also
 * the scenario of video playback wherein RR is set based on the rate
 * requested by userspace.
 */

static bool can_enable_drrs(struct intel_connector *connector,
			    const struct intel_crtc_state *pipe_config)
{
	const struct drm_i915_private *i915 = to_i915(connector->base.dev);

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

	return connector->panel.downclock_mode &&
		i915->drrs.type == DRRS_TYPE_SEAMLESS;
}

void
intel_drrs_compute_config(struct intel_dp *intel_dp,
			  struct intel_crtc_state *pipe_config,
			  int output_bpp, bool constant_n)
{
	struct intel_connector *connector = intel_dp->attached_connector;
	struct drm_i915_private *i915 = to_i915(connector->base.dev);
	int pixel_clock;

	if (!can_enable_drrs(connector, pipe_config)) {
		if (intel_cpu_transcoder_has_m2_n2(i915, pipe_config->cpu_transcoder))
			intel_zero_m_n(&pipe_config->dp_m2_n2);
		return;
	}

	if (IS_IRONLAKE(i915) || IS_SANDYBRIDGE(i915) || IS_IVYBRIDGE(i915))
		pipe_config->msa_timing_delay = i915->vbt.edp.drrs_msa_timing_delay;

	pipe_config->has_drrs = true;

	pixel_clock = connector->panel.downclock_mode->clock;
	if (pipe_config->splitter.enable)
		pixel_clock /= pipe_config->splitter.link_count;

	intel_link_compute_m_n(output_bpp, pipe_config->lane_count, pixel_clock,
			       pipe_config->port_clock, &pipe_config->dp_m2_n2,
			       constant_n, pipe_config->fec_enable);

	/* FIXME: abstract this better */
	if (pipe_config->splitter.enable)
		pipe_config->dp_m2_n2.data_m *= pipe_config->splitter.link_count;
}

static void
intel_drrs_set_refresh_rate_pipeconf(const struct intel_crtc_state *crtc_state,
				     enum drrs_refresh_rate refresh_rate)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;
	u32 val, bit;

	if (IS_VALLEYVIEW(dev_priv) || IS_CHERRYVIEW(dev_priv))
		bit = PIPECONF_REFRESH_RATE_ALT_VLV;
	else
		bit = PIPECONF_REFRESH_RATE_ALT_ILK;

	val = intel_de_read(dev_priv, PIPECONF(cpu_transcoder));

	if (refresh_rate == DRRS_REFRESH_RATE_LOW)
		val |= bit;
	else
		val &= ~bit;

	intel_de_write(dev_priv, PIPECONF(cpu_transcoder), val);
}

static void
intel_drrs_set_refresh_rate_m_n(const struct intel_crtc_state *crtc_state,
				enum drrs_refresh_rate refresh_rate)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);

	intel_cpu_transcoder_set_m1_n1(crtc, crtc_state->cpu_transcoder,
				       refresh_rate == DRRS_REFRESH_RATE_LOW ?
				       &crtc_state->dp_m2_n2 : &crtc_state->dp_m_n);
}

static void intel_drrs_set_state(struct drm_i915_private *dev_priv,
				 const struct intel_crtc_state *crtc_state,
				 enum drrs_refresh_rate refresh_rate)
{
	struct intel_dp *intel_dp = dev_priv->drrs.dp;
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);

	if (!intel_dp) {
		drm_dbg_kms(&dev_priv->drm, "DRRS not supported.\n");
		return;
	}

	if (!crtc) {
		drm_dbg_kms(&dev_priv->drm,
			    "DRRS: intel_crtc not initialized\n");
		return;
	}

	if (dev_priv->drrs.type != DRRS_TYPE_SEAMLESS) {
		drm_dbg_kms(&dev_priv->drm, "Only Seamless DRRS supported.\n");
		return;
	}

	if (refresh_rate == dev_priv->drrs.refresh_rate)
		return;

	if (!crtc_state->hw.active) {
		drm_dbg_kms(&dev_priv->drm,
			    "eDP encoder disabled. CRTC not Active\n");
		return;
	}

	if (DISPLAY_VER(dev_priv) >= 8 && !IS_CHERRYVIEW(dev_priv))
		intel_drrs_set_refresh_rate_m_n(crtc_state, refresh_rate);
	else if (DISPLAY_VER(dev_priv) > 6)
		intel_drrs_set_refresh_rate_pipeconf(crtc_state, refresh_rate);

	dev_priv->drrs.refresh_rate = refresh_rate;

	drm_dbg_kms(&dev_priv->drm, "eDP Refresh Rate set to : %s\n",
		    refresh_rate == DRRS_REFRESH_RATE_LOW ? "low" : "high");
}

static void
intel_drrs_enable_locked(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);

	dev_priv->drrs.busy_frontbuffer_bits = 0;
	dev_priv->drrs.dp = intel_dp;
}

/**
 * intel_drrs_enable - init drrs struct if supported
 * @intel_dp: DP struct
 * @crtc_state: A pointer to the active crtc state.
 *
 * Initializes frontbuffer_bits and drrs.dp
 */
void intel_drrs_enable(struct intel_dp *intel_dp,
		       const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);

	if (!crtc_state->has_drrs)
		return;

	drm_dbg_kms(&dev_priv->drm, "Enabling DRRS\n");

	mutex_lock(&dev_priv->drrs.mutex);

	if (dev_priv->drrs.dp) {
		drm_warn(&dev_priv->drm, "DRRS already enabled\n");
		goto unlock;
	}

	intel_drrs_enable_locked(intel_dp);

unlock:
	mutex_unlock(&dev_priv->drrs.mutex);
}

static void
intel_drrs_disable_locked(struct intel_dp *intel_dp,
			  const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);

	intel_drrs_set_state(dev_priv, crtc_state, DRRS_REFRESH_RATE_HIGH);
	dev_priv->drrs.dp = NULL;
}

/**
 * intel_drrs_disable - Disable DRRS
 * @intel_dp: DP struct
 * @old_crtc_state: Pointer to old crtc_state.
 *
 */
void intel_drrs_disable(struct intel_dp *intel_dp,
			const struct intel_crtc_state *old_crtc_state)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);

	if (!old_crtc_state->has_drrs)
		return;

	mutex_lock(&dev_priv->drrs.mutex);
	if (!dev_priv->drrs.dp) {
		mutex_unlock(&dev_priv->drrs.mutex);
		return;
	}

	intel_drrs_disable_locked(intel_dp, old_crtc_state);
	mutex_unlock(&dev_priv->drrs.mutex);

	cancel_delayed_work_sync(&dev_priv->drrs.work);
}

/**
 * intel_drrs_update - Update DRRS state
 * @intel_dp: Intel DP
 * @crtc_state: new CRTC state
 *
 * This function will update DRRS states, disabling or enabling DRRS when
 * executing fastsets. For full modeset, intel_drrs_disable() and
 * intel_drrs_enable() should be called instead.
 */
void
intel_drrs_update(struct intel_dp *intel_dp,
		  const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);

	if (dev_priv->drrs.type != DRRS_TYPE_SEAMLESS)
		return;

	mutex_lock(&dev_priv->drrs.mutex);

	/* New state matches current one? */
	if (crtc_state->has_drrs == !!dev_priv->drrs.dp)
		goto unlock;

	if (crtc_state->has_drrs)
		intel_drrs_enable_locked(intel_dp);
	else
		intel_drrs_disable_locked(intel_dp, crtc_state);

unlock:
	mutex_unlock(&dev_priv->drrs.mutex);
}

static void intel_drrs_downclock_work(struct work_struct *work)
{
	struct drm_i915_private *dev_priv =
		container_of(work, typeof(*dev_priv), drrs.work.work);
	struct intel_dp *intel_dp;

	mutex_lock(&dev_priv->drrs.mutex);

	intel_dp = dev_priv->drrs.dp;

	if (!intel_dp)
		goto unlock;

	/*
	 * The delayed work can race with an invalidate hence we need to
	 * recheck.
	 */

	if (!dev_priv->drrs.busy_frontbuffer_bits) {
		struct intel_crtc *crtc =
			to_intel_crtc(dp_to_dig_port(intel_dp)->base.base.crtc);

		intel_drrs_set_state(dev_priv, crtc->config,
				     DRRS_REFRESH_RATE_LOW);
	}

unlock:
	mutex_unlock(&dev_priv->drrs.mutex);
}

static void intel_drrs_frontbuffer_update(struct drm_i915_private *dev_priv,
					  unsigned int frontbuffer_bits,
					  bool invalidate)
{
	struct intel_dp *intel_dp;
	struct drm_crtc *crtc;
	enum pipe pipe;

	if (dev_priv->drrs.type != DRRS_TYPE_SEAMLESS)
		return;

	cancel_delayed_work(&dev_priv->drrs.work);

	mutex_lock(&dev_priv->drrs.mutex);

	intel_dp = dev_priv->drrs.dp;
	if (!intel_dp) {
		mutex_unlock(&dev_priv->drrs.mutex);
		return;
	}

	crtc = dp_to_dig_port(intel_dp)->base.base.crtc;
	pipe = to_intel_crtc(crtc)->pipe;

	frontbuffer_bits &= INTEL_FRONTBUFFER_ALL_MASK(pipe);
	if (invalidate)
		dev_priv->drrs.busy_frontbuffer_bits |= frontbuffer_bits;
	else
		dev_priv->drrs.busy_frontbuffer_bits &= ~frontbuffer_bits;

	/* flush/invalidate means busy screen hence upclock */
	if (frontbuffer_bits)
		intel_drrs_set_state(dev_priv, to_intel_crtc(crtc)->config,
				     DRRS_REFRESH_RATE_HIGH);

	/*
	 * flush also means no more activity hence schedule downclock, if all
	 * other fbs are quiescent too
	 */
	if (!invalidate && !dev_priv->drrs.busy_frontbuffer_bits)
		schedule_delayed_work(&dev_priv->drrs.work,
				      msecs_to_jiffies(1000));
	mutex_unlock(&dev_priv->drrs.mutex);
}

/**
 * intel_drrs_invalidate - Disable Idleness DRRS
 * @dev_priv: i915 device
 * @frontbuffer_bits: frontbuffer plane tracking bits
 *
 * This function gets called everytime rendering on the given planes start.
 * Hence DRRS needs to be Upclocked, i.e. (LOW_RR -> HIGH_RR).
 *
 * Dirty frontbuffers relevant to DRRS are tracked in busy_frontbuffer_bits.
 */
void intel_drrs_invalidate(struct drm_i915_private *dev_priv,
			   unsigned int frontbuffer_bits)
{
	intel_drrs_frontbuffer_update(dev_priv, frontbuffer_bits, true);
}

/**
 * intel_drrs_flush - Restart Idleness DRRS
 * @dev_priv: i915 device
 * @frontbuffer_bits: frontbuffer plane tracking bits
 *
 * This function gets called every time rendering on the given planes has
 * completed or flip on a crtc is completed. So DRRS should be upclocked
 * (LOW_RR -> HIGH_RR). And also Idleness detection should be started again,
 * if no other planes are dirty.
 *
 * Dirty frontbuffers relevant to DRRS are tracked in busy_frontbuffer_bits.
 */
void intel_drrs_flush(struct drm_i915_private *dev_priv,
		      unsigned int frontbuffer_bits)
{
	intel_drrs_frontbuffer_update(dev_priv, frontbuffer_bits, false);
}

void intel_drrs_page_flip(struct intel_atomic_state *state,
			  struct intel_crtc *crtc)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	unsigned int frontbuffer_bits = INTEL_FRONTBUFFER_ALL_MASK(crtc->pipe);

	intel_drrs_frontbuffer_update(dev_priv, frontbuffer_bits, false);
}

/**
 * intel_drrs_init - Init basic DRRS work and mutex.
 * @connector: eDP connector
 * @fixed_mode: preferred mode of panel
 *
 * This function is  called only once at driver load to initialize basic
 * DRRS stuff.
 *
 * Returns:
 * Downclock mode if panel supports it, else return NULL.
 * DRRS support is determined by the presence of downclock mode (apart
 * from VBT setting).
 */
struct drm_display_mode *
intel_drrs_init(struct intel_connector *connector,
		const struct drm_display_mode *fixed_mode)
{
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct intel_encoder *encoder = connector->encoder;
	struct drm_display_mode *downclock_mode = NULL;

	INIT_DELAYED_WORK(&dev_priv->drrs.work, intel_drrs_downclock_work);
	mutex_init(&dev_priv->drrs.mutex);

	if (DISPLAY_VER(dev_priv) <= 6) {
		drm_dbg_kms(&dev_priv->drm,
			    "[CONNECTOR:%d:%s] DRRS not supported on platform\n",
			    connector->base.base.id, connector->base.name);
		return NULL;
	}

	if ((DISPLAY_VER(dev_priv) < 8 && !HAS_GMCH(dev_priv)) &&
	    encoder->port != PORT_A) {
		drm_dbg_kms(&dev_priv->drm,
			    "[CONNECTOR:%d:%s] DRRS not supported on [ENCODER:%d:%s]\n",
			    connector->base.base.id, connector->base.name,
			    encoder->base.base.id, encoder->base.name);
		return NULL;
	}

	if (dev_priv->vbt.drrs_type != DRRS_TYPE_SEAMLESS) {
		drm_dbg_kms(&dev_priv->drm,
			    "[CONNECTOR:%d:%s] DRRS not supported according to VBT\n",
			    connector->base.base.id, connector->base.name);
		return NULL;
	}

	downclock_mode = intel_panel_edid_downclock_mode(connector, fixed_mode);
	if (!downclock_mode) {
		drm_dbg_kms(&dev_priv->drm,
			    "[CONNECTOR:%d:%s] DRRS not supported due to lack of downclock mode\n",
			    connector->base.base.id, connector->base.name);
		return NULL;
	}

	dev_priv->drrs.type = dev_priv->vbt.drrs_type;

	dev_priv->drrs.refresh_rate = DRRS_REFRESH_RATE_HIGH;
	drm_dbg_kms(&dev_priv->drm,
		    "[CONNECTOR:%d:%s] seamless DRRS supported\n",
		    connector->base.base.id, connector->base.name);

	return downclock_mode;
}
