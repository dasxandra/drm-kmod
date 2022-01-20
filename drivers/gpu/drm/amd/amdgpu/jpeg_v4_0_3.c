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
 */

#include "amdgpu.h"
#include "amdgpu_jpeg.h"
#include "soc15.h"
#include "soc15d.h"
#include "jpeg_v4_0_3.h"

#include "vcn/vcn_4_0_3_offset.h"
#include "vcn/vcn_4_0_3_sh_mask.h"
#include "ivsrcid/vcn/irqsrcs_vcn_4_0.h"

enum jpeg_engin_status {
	UVD_PGFSM_STATUS__UVDJ_PWR_ON  = 0,
	UVD_PGFSM_STATUS__UVDJ_PWR_OFF = 2,
};

static void jpeg_v4_0_3_set_dec_ring_funcs(struct amdgpu_device *adev);
static void jpeg_v4_0_3_set_irq_funcs(struct amdgpu_device *adev);
static int jpeg_v4_0_3_set_powergating_state(void *handle,
				enum amd_powergating_state state);

static int amdgpu_ih_srcid_jpeg[] = {
	VCN_4_0__SRCID__JPEG_DECODE,
	VCN_4_0__SRCID__JPEG1_DECODE,
	VCN_4_0__SRCID__JPEG2_DECODE,
	VCN_4_0__SRCID__JPEG3_DECODE,
	VCN_4_0__SRCID__JPEG4_DECODE,
	VCN_4_0__SRCID__JPEG5_DECODE,
	VCN_4_0__SRCID__JPEG6_DECODE,
	VCN_4_0__SRCID__JPEG7_DECODE
};

/**
 * jpeg_v4_0_3_early_init - set function pointers
 *
 * @handle: amdgpu_device pointer
 *
 * Set ring and irq function pointers
 */
static int jpeg_v4_0_3_early_init(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	jpeg_v4_0_3_set_dec_ring_funcs(adev);
	jpeg_v4_0_3_set_irq_funcs(adev);

	return 0;
}

/**
 * jpeg_v4_0_3_sw_init - sw init for JPEG block
 *
 * @handle: amdgpu_device pointer
 *
 * Load firmware and sw initialization
 */
static int jpeg_v4_0_3_sw_init(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	struct amdgpu_ring *ring;
	int i, r;

	for (i = 0; i < adev->jpeg.num_jpeg_rings; ++i) {
		/* JPEG TRAP */
		r = amdgpu_irq_add_id(adev, SOC15_IH_CLIENTID_VCN,
				amdgpu_ih_srcid_jpeg[i], &adev->jpeg.inst->irq);
		if (r)
			return r;
	}

	r = amdgpu_jpeg_sw_init(adev);
	if (r)
		return r;

	r = amdgpu_jpeg_resume(adev);
	if (r)
		return r;

	for (i = 0; i < adev->jpeg.num_jpeg_rings; ++i) {
		ring = &adev->jpeg.inst->ring_dec[i];
		ring->use_doorbell = false;
		ring->vm_hub = AMDGPU_MMHUB0(0);
		sprintf(ring->name, "jpeg_dec_%d", i);
		r = amdgpu_ring_init(adev, ring, 512, &adev->jpeg.inst->irq, 0,
					AMDGPU_RING_PRIO_DEFAULT, NULL);
		if (r)
			return r;

		adev->jpeg.internal.jpeg_pitch[i] =
			regUVD_JRBC0_UVD_JRBC_SCRATCH0_INTERNAL_OFFSET;
		adev->jpeg.inst->external.jpeg_pitch[i] =
			SOC15_REG_OFFSET1(JPEG, 0, regUVD_JRBC0_UVD_JRBC_SCRATCH0,
			(i?(0x40 * i - 0xc80):0));
	}

	return 0;
}

/**
 * jpeg_v4_0_3_sw_fini - sw fini for JPEG block
 *
 * @handle: amdgpu_device pointer
 *
 * JPEG suspend and free up sw allocation
 */
static int jpeg_v4_0_3_sw_fini(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	int r;

	r = amdgpu_jpeg_suspend(adev);
	if (r)
		return r;

	r = amdgpu_jpeg_sw_fini(adev);

	return r;
}

/**
 * jpeg_v4_0_3_hw_init - start and test JPEG block
 *
 * @handle: amdgpu_device pointer
 *
 */
static int jpeg_v4_0_3_hw_init(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	struct amdgpu_ring *ring;
	int i, r;

	for (i = 0; i < adev->jpeg.num_jpeg_rings; ++i) {
		ring = &adev->jpeg.inst->ring_dec[i];
		r = amdgpu_ring_test_helper(ring);
		if (r)
			return r;
	}
	DRM_DEV_INFO(adev->dev, "JPEG decode initialized successfully.\n");

	return 0;
}

/**
 * jpeg_v4_0_3_hw_fini - stop the hardware block
 *
 * @handle: amdgpu_device pointer
 *
 * Stop the JPEG block, mark ring as not ready any more
 */
static int jpeg_v4_0_3_hw_fini(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	int ret = 0;

	cancel_delayed_work_sync(&adev->jpeg.idle_work);

	if (adev->jpeg.cur_state != AMD_PG_STATE_GATE)
		ret = jpeg_v4_0_3_set_powergating_state(adev, AMD_PG_STATE_GATE);

	return ret;
}

/**
 * jpeg_v4_0_3_suspend - suspend JPEG block
 *
 * @handle: amdgpu_device pointer
 *
 * HW fini and suspend JPEG block
 */
static int jpeg_v4_0_3_suspend(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	int r;

	r = jpeg_v4_0_3_hw_fini(adev);
	if (r)
		return r;

	r = amdgpu_jpeg_suspend(adev);

	return r;
}

/**
 * jpeg_v4_0_3_resume - resume JPEG block
 *
 * @handle: amdgpu_device pointer
 *
 * Resume firmware and hw init JPEG block
 */
static int jpeg_v4_0_3_resume(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	int r;

	r = amdgpu_jpeg_resume(adev);
	if (r)
		return r;

	r = jpeg_v4_0_3_hw_init(adev);

	return r;
}

static void jpeg_v4_0_3_disable_clock_gating(struct amdgpu_device *adev)
{
	uint32_t data;
	int i;

	data = RREG32_SOC15(JPEG, 0, regJPEG_CGC_CTRL);
	if (adev->cg_flags & AMD_CG_SUPPORT_JPEG_MGCG)
		data |= 1 << JPEG_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;
	else
		data &= ~JPEG_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;

	data |= 1 << JPEG_CGC_CTRL__CLK_GATE_DLY_TIMER__SHIFT;
	data |= 4 << JPEG_CGC_CTRL__CLK_OFF_DELAY__SHIFT;
	WREG32_SOC15(JPEG, 0, regJPEG_CGC_CTRL, data);

	data = RREG32_SOC15(JPEG, 0, regJPEG_CGC_GATE);
	data &= ~(JPEG_CGC_GATE__JMCIF_MASK | JPEG_CGC_GATE__JRBBM_MASK);
	for (i = 0; i < adev->jpeg.num_jpeg_rings; ++i)
		data &= ~(JPEG_CGC_GATE__JPEG0_DEC_MASK << i);
	WREG32_SOC15(JPEG, 0, regJPEG_CGC_GATE, data);
}

static void jpeg_v4_0_3_enable_clock_gating(struct amdgpu_device *adev)
{
	uint32_t data;
	int i;

	data = RREG32_SOC15(JPEG, 0, regJPEG_CGC_CTRL);
	if (adev->cg_flags & AMD_CG_SUPPORT_JPEG_MGCG)
		data |= 1 << JPEG_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;
	else
		data &= ~JPEG_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;

	data |= 1 << JPEG_CGC_CTRL__CLK_GATE_DLY_TIMER__SHIFT;
	data |= 4 << JPEG_CGC_CTRL__CLK_OFF_DELAY__SHIFT;
	WREG32_SOC15(JPEG, 0, regJPEG_CGC_CTRL, data);

	data = RREG32_SOC15(JPEG, 0, regJPEG_CGC_GATE);
	data |= (JPEG_CGC_GATE__JMCIF_MASK | JPEG_CGC_GATE__JRBBM_MASK);
	for (i = 0; i < adev->jpeg.num_jpeg_rings; ++i)
		data |= (JPEG_CGC_GATE__JPEG0_DEC_MASK << i);
	WREG32_SOC15(JPEG, 0, regJPEG_CGC_GATE, data);
}

/**
 * jpeg_v4_0_3_start - start JPEG block
 *
 * @adev: amdgpu_device pointer
 *
 * Setup and start the JPEG block
 */
static int jpeg_v4_0_3_start(struct amdgpu_device *adev)
{
	struct amdgpu_ring *ring = adev->jpeg.inst->ring_dec;
	int i;

	WREG32_SOC15(JPEG, 0, regUVD_PGFSM_CONFIG,
		1 << UVD_PGFSM_CONFIG__UVDJ_PWR_CONFIG__SHIFT);
	SOC15_WAIT_ON_RREG(JPEG, 0, regUVD_PGFSM_STATUS,
		UVD_PGFSM_STATUS__UVDJ_PWR_ON <<
		UVD_PGFSM_STATUS__UVDJ_PWR_STATUS__SHIFT,
		UVD_PGFSM_STATUS__UVDJ_PWR_STATUS_MASK);

	/* disable anti hang mechanism */
	WREG32_P(SOC15_REG_OFFSET(JPEG, 0, regUVD_JPEG_POWER_STATUS), 0,
		~UVD_JPEG_POWER_STATUS__JPEG_POWER_STATUS_MASK);

	/* JPEG disable CGC */
	jpeg_v4_0_3_disable_clock_gating(adev);

	/* MJPEG global tiling registers */
	WREG32_SOC15(JPEG, 0, regJPEG_DEC_GFX8_ADDR_CONFIG,
		adev->gfx.config.gb_addr_config);
	WREG32_SOC15(JPEG, 0, regJPEG_DEC_GFX10_ADDR_CONFIG,
		adev->gfx.config.gb_addr_config);

	/* enable JMI channel */
	WREG32_P(SOC15_REG_OFFSET(JPEG, 0, regUVD_JMI_CNTL), 0,
		~UVD_JMI_CNTL__SOFT_RESET_MASK);

	for (i = 0; i < adev->jpeg.num_jpeg_rings; ++i) {
		unsigned int reg_offset = (i?(0x40 * i - 0xc80):0);

		ring = &adev->jpeg.inst->ring_dec[i];

		/* enable System Interrupt for JRBC */
		WREG32_P(SOC15_REG_OFFSET(JPEG, 0, regJPEG_SYS_INT_EN),
			JPEG_SYS_INT_EN__DJRBC0_MASK << i,
			~(JPEG_SYS_INT_EN__DJRBC0_MASK << i));

		WREG32_SOC15_OFFSET(JPEG, 0, regUVD_JMI0_UVD_LMI_JRBC_RB_VMID, reg_offset, 0);
		WREG32_SOC15_OFFSET(JPEG, 0, regUVD_JRBC0_UVD_JRBC_RB_CNTL, reg_offset,
			(0x00000001L | 0x00000002L));
		WREG32_SOC15_OFFSET(JPEG, 0, regUVD_JMI0_UVD_LMI_JRBC_RB_64BIT_BAR_LOW,
			reg_offset, lower_32_bits(ring->gpu_addr));
		WREG32_SOC15_OFFSET(JPEG, 0, regUVD_JMI0_UVD_LMI_JRBC_RB_64BIT_BAR_HIGH,
			reg_offset, upper_32_bits(ring->gpu_addr));
		WREG32_SOC15_OFFSET(JPEG, 0, regUVD_JRBC0_UVD_JRBC_RB_RPTR, reg_offset, 0);
		WREG32_SOC15_OFFSET(JPEG, 0, regUVD_JRBC0_UVD_JRBC_RB_WPTR, reg_offset, 0);
		WREG32_SOC15_OFFSET(JPEG, 0, regUVD_JRBC0_UVD_JRBC_RB_CNTL, reg_offset,
			0x00000002L);
		WREG32_SOC15_OFFSET(JPEG, 0, regUVD_JRBC0_UVD_JRBC_RB_SIZE, reg_offset,
			ring->ring_size / 4);
		ring->wptr = RREG32_SOC15_OFFSET(JPEG, 0, regUVD_JRBC0_UVD_JRBC_RB_WPTR,
			reg_offset);
	}

	return 0;
}

/**
 * jpeg_v4_0_3_stop - stop JPEG block
 *
 * @adev: amdgpu_device pointer
 *
 * stop the JPEG block
 */
static int jpeg_v4_0_3_stop(struct amdgpu_device *adev)
{
	/* reset JMI */
	WREG32_P(SOC15_REG_OFFSET(JPEG, 0, regUVD_JMI_CNTL),
		UVD_JMI_CNTL__SOFT_RESET_MASK,
		~UVD_JMI_CNTL__SOFT_RESET_MASK);

	jpeg_v4_0_3_enable_clock_gating(adev);

	/* enable anti hang mechanism */
	WREG32_P(SOC15_REG_OFFSET(JPEG, 0, regUVD_JPEG_POWER_STATUS),
		UVD_JPEG_POWER_STATUS__JPEG_POWER_STATUS_MASK,
		~UVD_JPEG_POWER_STATUS__JPEG_POWER_STATUS_MASK);

	WREG32_SOC15(JPEG, 0, regUVD_PGFSM_CONFIG,
		2 << UVD_PGFSM_CONFIG__UVDJ_PWR_CONFIG__SHIFT);
	SOC15_WAIT_ON_RREG(JPEG, 0, regUVD_PGFSM_STATUS,
		UVD_PGFSM_STATUS__UVDJ_PWR_OFF <<
		UVD_PGFSM_STATUS__UVDJ_PWR_STATUS__SHIFT,
		UVD_PGFSM_STATUS__UVDJ_PWR_STATUS_MASK);

	return 0;
}

/**
 * jpeg_v4_0_3_dec_ring_get_rptr - get read pointer
 *
 * @ring: amdgpu_ring pointer
 *
 * Returns the current hardware read pointer
 */
static uint64_t jpeg_v4_0_3_dec_ring_get_rptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	return RREG32_SOC15_OFFSET(JPEG, ring->me, regUVD_JRBC0_UVD_JRBC_RB_RPTR,
			ring->pipe?(0x40 * ring->pipe - 0xc80):0);
}

/**
 * jpeg_v4_0_3_dec_ring_get_wptr - get write pointer
 *
 * @ring: amdgpu_ring pointer
 *
 * Returns the current hardware write pointer
 */
static uint64_t jpeg_v4_0_3_dec_ring_get_wptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	if (ring->use_doorbell)
		return adev->wb.wb[ring->wptr_offs];
	else
		return RREG32_SOC15_OFFSET(JPEG, ring->me, regUVD_JRBC0_UVD_JRBC_RB_WPTR,
			ring->pipe?(0x40 * ring->pipe - 0xc80):0);
}

/**
 * jpeg_v4_0_3_dec_ring_set_wptr - set write pointer
 *
 * @ring: amdgpu_ring pointer
 *
 * Commits the write pointer to the hardware
 */
static void jpeg_v4_0_3_dec_ring_set_wptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	if (ring->use_doorbell) {
		adev->wb.wb[ring->wptr_offs] = lower_32_bits(ring->wptr);
		WDOORBELL32(ring->doorbell_index, lower_32_bits(ring->wptr));
	} else {
		WREG32_SOC15_OFFSET(JPEG, ring->me, regUVD_JRBC0_UVD_JRBC_RB_WPTR,
			(ring->pipe?(0x40 * ring->pipe - 0xc80):0), lower_32_bits(ring->wptr));
	}
}

/**
 * jpeg_v4_0_3_dec_ring_insert_start - insert a start command
 *
 * @ring: amdgpu_ring pointer
 *
 * Write a start command to the ring.
 */
static void jpeg_v4_0_3_dec_ring_insert_start(struct amdgpu_ring *ring)
{
	amdgpu_ring_write(ring, PACKETJ(regUVD_JRBC_EXTERNAL_REG_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, 0x62a04); /* PCTL0_MMHUB_DEEPSLEEP_IB */

	amdgpu_ring_write(ring, PACKETJ(JRBC_DEC_EXTERNAL_REG_WRITE_ADDR,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, 0x80004000);
}

/**
 * jpeg_v4_0_3_dec_ring_insert_end - insert a end command
 *
 * @ring: amdgpu_ring pointer
 *
 * Write a end command to the ring.
 */
static void jpeg_v4_0_3_dec_ring_insert_end(struct amdgpu_ring *ring)
{
	amdgpu_ring_write(ring, PACKETJ(regUVD_JRBC_EXTERNAL_REG_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, 0x62a04);

	amdgpu_ring_write(ring, PACKETJ(JRBC_DEC_EXTERNAL_REG_WRITE_ADDR,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, 0x00004000);
}

/**
 * jpeg_v4_0_3_dec_ring_emit_fence - emit an fence & trap command
 *
 * @ring: amdgpu_ring pointer
 * @addr: address
 * @seq: sequence number
 * @flags: fence related flags
 *
 * Write a fence and a trap command to the ring.
 */
static void jpeg_v4_0_3_dec_ring_emit_fence(struct amdgpu_ring *ring, u64 addr, u64 seq,
				unsigned int flags)
{
	WARN_ON(flags & AMDGPU_FENCE_FLAG_64BIT);

	amdgpu_ring_write(ring, PACKETJ(regUVD_JPEG_GPCOM_DATA0_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, seq);

	amdgpu_ring_write(ring,	PACKETJ(regUVD_JPEG_GPCOM_DATA1_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, seq);

	amdgpu_ring_write(ring,	PACKETJ(regUVD_LMI_JRBC_RB_MEM_WR_64BIT_BAR_LOW_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, lower_32_bits(addr));

	amdgpu_ring_write(ring,	PACKETJ(regUVD_LMI_JRBC_RB_MEM_WR_64BIT_BAR_HIGH_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, upper_32_bits(addr));

	amdgpu_ring_write(ring,	PACKETJ(regUVD_JPEG_GPCOM_CMD_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, 0x8);

	amdgpu_ring_write(ring,	PACKETJ(regUVD_JPEG_GPCOM_CMD_INTERNAL_OFFSET,
		0, PACKETJ_CONDITION_CHECK0, PACKETJ_TYPE4));
	amdgpu_ring_write(ring, 0);

	amdgpu_ring_write(ring,	PACKETJ(regUVD_JRBC_EXTERNAL_REG_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, 0x3fbc);

	amdgpu_ring_write(ring, PACKETJ(JRBC_DEC_EXTERNAL_REG_WRITE_ADDR,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, 0x1);

	amdgpu_ring_write(ring, PACKETJ(0, 0, 0, PACKETJ_TYPE7));
	amdgpu_ring_write(ring, 0);
}

/**
 * jpeg_v4_0_3_dec_ring_emit_ib - execute indirect buffer
 *
 * @ring: amdgpu_ring pointer
 * @job: job to retrieve vmid from
 * @ib: indirect buffer to execute
 * @flags: unused
 *
 * Write ring commands to execute the indirect buffer.
 */
static void jpeg_v4_0_3_dec_ring_emit_ib(struct amdgpu_ring *ring,
				struct amdgpu_job *job,
				struct amdgpu_ib *ib,
				uint32_t flags)
{
	unsigned int vmid = AMDGPU_JOB_GET_VMID(job);

	amdgpu_ring_write(ring, PACKETJ(regUVD_LMI_JRBC_IB_VMID_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, (vmid | (vmid << 4)));

	amdgpu_ring_write(ring, PACKETJ(regUVD_LMI_JPEG_VMID_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, (vmid | (vmid << 4)));

	amdgpu_ring_write(ring,	PACKETJ(regUVD_LMI_JRBC_IB_64BIT_BAR_LOW_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, lower_32_bits(ib->gpu_addr));

	amdgpu_ring_write(ring,	PACKETJ(regUVD_LMI_JRBC_IB_64BIT_BAR_HIGH_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, upper_32_bits(ib->gpu_addr));

	amdgpu_ring_write(ring,	PACKETJ(regUVD_JRBC_IB_SIZE_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, ib->length_dw);

	amdgpu_ring_write(ring,	PACKETJ(regUVD_LMI_JRBC_RB_MEM_RD_64BIT_BAR_LOW_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, lower_32_bits(ring->gpu_addr));

	amdgpu_ring_write(ring,	PACKETJ(regUVD_LMI_JRBC_RB_MEM_RD_64BIT_BAR_HIGH_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, upper_32_bits(ring->gpu_addr));

	amdgpu_ring_write(ring,	PACKETJ(0, 0, PACKETJ_CONDITION_CHECK0, PACKETJ_TYPE2));
	amdgpu_ring_write(ring, 0);

	amdgpu_ring_write(ring,	PACKETJ(regUVD_JRBC_RB_COND_RD_TIMER_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, 0x01400200);

	amdgpu_ring_write(ring, PACKETJ(regUVD_JRBC_RB_REF_DATA_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, 0x2);

	amdgpu_ring_write(ring,	PACKETJ(regUVD_JRBC_STATUS_INTERNAL_OFFSET,
		0, PACKETJ_CONDITION_CHECK3, PACKETJ_TYPE3));
	amdgpu_ring_write(ring, 0x2);
}

static void jpeg_v4_0_3_dec_ring_emit_reg_wait(struct amdgpu_ring *ring, uint32_t reg,
				uint32_t val, uint32_t mask)
{
	uint32_t reg_offset = (reg << 2);

	amdgpu_ring_write(ring, PACKETJ(regUVD_JRBC_RB_COND_RD_TIMER_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, 0x01400200);

	amdgpu_ring_write(ring,	PACKETJ(regUVD_JRBC_RB_REF_DATA_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	amdgpu_ring_write(ring, val);

	amdgpu_ring_write(ring, PACKETJ(regUVD_JRBC_EXTERNAL_REG_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	if (reg_offset >= 0x10000 && reg_offset <= 0x105ff) {
		amdgpu_ring_write(ring, 0);
		amdgpu_ring_write(ring,
			PACKETJ((reg_offset >> 2), 0, 0, PACKETJ_TYPE3));
	} else {
		amdgpu_ring_write(ring, reg_offset);
		amdgpu_ring_write(ring,	PACKETJ(JRBC_DEC_EXTERNAL_REG_WRITE_ADDR,
			0, 0, PACKETJ_TYPE3));
	}
	amdgpu_ring_write(ring, mask);
}

static void jpeg_v4_0_3_dec_ring_emit_vm_flush(struct amdgpu_ring *ring,
				unsigned int vmid, uint64_t pd_addr)
{
	struct amdgpu_vmhub *hub = &ring->adev->vmhub[ring->vm_hub];
	uint32_t data0, data1, mask;

	pd_addr = amdgpu_gmc_emit_flush_gpu_tlb(ring, vmid, pd_addr);

	/* wait for register write */
	data0 = hub->ctx0_ptb_addr_lo32 + vmid * hub->ctx_addr_distance;
	data1 = lower_32_bits(pd_addr);
	mask = 0xffffffff;
	jpeg_v4_0_3_dec_ring_emit_reg_wait(ring, data0, data1, mask);
}

static void jpeg_v4_0_3_dec_ring_emit_wreg(struct amdgpu_ring *ring, uint32_t reg, uint32_t val)
{
	uint32_t reg_offset = (reg << 2);

	amdgpu_ring_write(ring,	PACKETJ(regUVD_JRBC_EXTERNAL_REG_INTERNAL_OFFSET,
		0, 0, PACKETJ_TYPE0));
	if (reg_offset >= 0x10000 && reg_offset <= 0x105ff) {
		amdgpu_ring_write(ring, 0);
		amdgpu_ring_write(ring,
			PACKETJ((reg_offset >> 2), 0, 0, PACKETJ_TYPE0));
	} else {
		amdgpu_ring_write(ring, reg_offset);
		amdgpu_ring_write(ring,	PACKETJ(JRBC_DEC_EXTERNAL_REG_WRITE_ADDR,
			0, 0, PACKETJ_TYPE0));
	}
	amdgpu_ring_write(ring, val);
}

static void jpeg_v4_0_3_dec_ring_nop(struct amdgpu_ring *ring, uint32_t count)
{
	int i;

	WARN_ON(ring->wptr % 2 || count % 2);

	for (i = 0; i < count / 2; i++) {
		amdgpu_ring_write(ring, PACKETJ(0, 0, 0, PACKETJ_TYPE6));
		amdgpu_ring_write(ring, 0);
	}
}

static bool jpeg_v4_0_3_is_idle(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	bool ret;
	int i;

	for (i = 0; i < adev->jpeg.num_jpeg_rings; ++i) {
		unsigned int reg_offset = (i?(0x40 * i - 0xc80):0);

		ret &= ((RREG32_SOC15_OFFSET(JPEG, 0,
				regUVD_JRBC0_UVD_JRBC_STATUS, reg_offset) &
				UVD_JRBC0_UVD_JRBC_STATUS__RB_JOB_DONE_MASK) ==
				UVD_JRBC0_UVD_JRBC_STATUS__RB_JOB_DONE_MASK);
	}

	return ret;
}

static int jpeg_v4_0_3_wait_for_idle(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	int ret;
	int i;

	for (i = 0; i < adev->jpeg.num_jpeg_rings; ++i) {
		unsigned int reg_offset = (i?(0x40 * i - 0xc80):0);

		ret &= SOC15_WAIT_ON_RREG_OFFSET(JPEG, 0,
			regUVD_JRBC0_UVD_JRBC_STATUS, reg_offset,
			UVD_JRBC0_UVD_JRBC_STATUS__RB_JOB_DONE_MASK,
			UVD_JRBC0_UVD_JRBC_STATUS__RB_JOB_DONE_MASK);
	}

	return ret;
}

static int jpeg_v4_0_3_set_clockgating_state(void *handle,
					  enum amd_clockgating_state state)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	bool enable = (state == AMD_CG_STATE_GATE) ? true : false;

	if (enable) {
		if (!jpeg_v4_0_3_is_idle(handle))
			return -EBUSY;
		jpeg_v4_0_3_enable_clock_gating(adev);
	} else {
		jpeg_v4_0_3_disable_clock_gating(adev);
	}

	return 0;
}

static int jpeg_v4_0_3_set_powergating_state(void *handle,
					  enum amd_powergating_state state)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	int ret;

	if (state == adev->jpeg.cur_state)
		return 0;

	if (state == AMD_PG_STATE_GATE)
		ret = jpeg_v4_0_3_stop(adev);
	else
		ret = jpeg_v4_0_3_start(adev);

	if (!ret)
		adev->jpeg.cur_state = state;

	return ret;
}

static int jpeg_v4_0_3_set_interrupt_state(struct amdgpu_device *adev,
					struct amdgpu_irq_src *source,
					unsigned int type,
					enum amdgpu_interrupt_state state)
{
	return 0;
}

static int jpeg_v4_0_3_process_interrupt(struct amdgpu_device *adev,
				      struct amdgpu_irq_src *source,
				      struct amdgpu_iv_entry *entry)
{
	DRM_DEV_DEBUG(adev->dev, "IH: JPEG TRAP\n");

	switch (entry->src_id) {
	case VCN_4_0__SRCID__JPEG_DECODE:
		amdgpu_fence_process(&adev->jpeg.inst->ring_dec[0]);
		break;
	case VCN_4_0__SRCID__JPEG1_DECODE:
		amdgpu_fence_process(&adev->jpeg.inst->ring_dec[1]);
		break;
	case VCN_4_0__SRCID__JPEG2_DECODE:
		amdgpu_fence_process(&adev->jpeg.inst->ring_dec[2]);
		break;
	case VCN_4_0__SRCID__JPEG3_DECODE:
		amdgpu_fence_process(&adev->jpeg.inst->ring_dec[3]);
		break;
	case VCN_4_0__SRCID__JPEG4_DECODE:
		amdgpu_fence_process(&adev->jpeg.inst->ring_dec[4]);
		break;
	case VCN_4_0__SRCID__JPEG5_DECODE:
		amdgpu_fence_process(&adev->jpeg.inst->ring_dec[5]);
		break;
	case VCN_4_0__SRCID__JPEG6_DECODE:
		amdgpu_fence_process(&adev->jpeg.inst->ring_dec[6]);
		break;
	case VCN_4_0__SRCID__JPEG7_DECODE:
		amdgpu_fence_process(&adev->jpeg.inst->ring_dec[7]);
		break;
	default:
		DRM_DEV_ERROR(adev->dev, "Unhandled interrupt: %d %d\n",
			  entry->src_id, entry->src_data[0]);
		break;
	}

	return 0;
}

static const struct amd_ip_funcs jpeg_v4_0_3_ip_funcs = {
	.name = "jpeg_v4_0_3",
	.early_init = jpeg_v4_0_3_early_init,
	.late_init = NULL,
	.sw_init = jpeg_v4_0_3_sw_init,
	.sw_fini = jpeg_v4_0_3_sw_fini,
	.hw_init = jpeg_v4_0_3_hw_init,
	.hw_fini = jpeg_v4_0_3_hw_fini,
	.suspend = jpeg_v4_0_3_suspend,
	.resume = jpeg_v4_0_3_resume,
	.is_idle = jpeg_v4_0_3_is_idle,
	.wait_for_idle = jpeg_v4_0_3_wait_for_idle,
	.check_soft_reset = NULL,
	.pre_soft_reset = NULL,
	.soft_reset = NULL,
	.post_soft_reset = NULL,
	.set_clockgating_state = jpeg_v4_0_3_set_clockgating_state,
	.set_powergating_state = jpeg_v4_0_3_set_powergating_state,
};

static const struct amdgpu_ring_funcs jpeg_v4_0_3_dec_ring_vm_funcs = {
	.type = AMDGPU_RING_TYPE_VCN_JPEG,
	.align_mask = 0xf,
	.get_rptr = jpeg_v4_0_3_dec_ring_get_rptr,
	.get_wptr = jpeg_v4_0_3_dec_ring_get_wptr,
	.set_wptr = jpeg_v4_0_3_dec_ring_set_wptr,
	.emit_frame_size =
		SOC15_FLUSH_GPU_TLB_NUM_WREG * 6 +
		SOC15_FLUSH_GPU_TLB_NUM_REG_WAIT * 8 +
		8 + /* jpeg_v4_0_3_dec_ring_emit_vm_flush */
		18 + 18 + /* jpeg_v4_0_3_dec_ring_emit_fence x2 vm fence */
		8 + 16,
	.emit_ib_size = 22, /* jpeg_v4_0_3_dec_ring_emit_ib */
	.emit_ib = jpeg_v4_0_3_dec_ring_emit_ib,
	.emit_fence = jpeg_v4_0_3_dec_ring_emit_fence,
	.emit_vm_flush = jpeg_v4_0_3_dec_ring_emit_vm_flush,
	.test_ring = amdgpu_jpeg_dec_ring_test_ring,
	.test_ib = amdgpu_jpeg_dec_ring_test_ib,
	.insert_nop = jpeg_v4_0_3_dec_ring_nop,
	.insert_start = jpeg_v4_0_3_dec_ring_insert_start,
	.insert_end = jpeg_v4_0_3_dec_ring_insert_end,
	.pad_ib = amdgpu_ring_generic_pad_ib,
	.begin_use = amdgpu_jpeg_ring_begin_use,
	.end_use = amdgpu_jpeg_ring_end_use,
	.emit_wreg = jpeg_v4_0_3_dec_ring_emit_wreg,
	.emit_reg_wait = jpeg_v4_0_3_dec_ring_emit_reg_wait,
	.emit_reg_write_reg_wait = amdgpu_ring_emit_reg_write_reg_wait_helper,
};

static void jpeg_v4_0_3_set_dec_ring_funcs(struct amdgpu_device *adev)
{
	int i;

	for (i = 0; i < adev->jpeg.num_jpeg_rings; ++i) {
		adev->jpeg.inst->ring_dec[i].funcs = &jpeg_v4_0_3_dec_ring_vm_funcs;
		adev->jpeg.inst->ring_dec[i].me = 0;
		adev->jpeg.inst->ring_dec[i].pipe = i;
	}
	DRM_DEV_INFO(adev->dev, "JPEG decode is enabled in VM mode\n");
}

static const struct amdgpu_irq_src_funcs jpeg_v4_0_3_irq_funcs = {
	.set = jpeg_v4_0_3_set_interrupt_state,
	.process = jpeg_v4_0_3_process_interrupt,
};

static void jpeg_v4_0_3_set_irq_funcs(struct amdgpu_device *adev)
{
	adev->jpeg.inst->irq.num_types = adev->jpeg.num_jpeg_rings;
	adev->jpeg.inst->irq.funcs = &jpeg_v4_0_3_irq_funcs;
}

const struct amdgpu_ip_block_version jpeg_v4_0_3_ip_block = {
	.type = AMD_IP_BLOCK_TYPE_JPEG,
	.major = 4,
	.minor = 0,
	.rev = 3,
	.funcs = &jpeg_v4_0_3_ip_funcs,
};
