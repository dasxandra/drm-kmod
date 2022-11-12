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
#include "mmhub_v1_8.h"

#include "mmhub/mmhub_1_8_0_offset.h"
#include "mmhub/mmhub_1_8_0_sh_mask.h"
#include "vega10_enum.h"

#include "soc15_common.h"
#include "soc15.h"

#define regVM_L2_CNTL3_DEFAULT	0x80100007
#define regVM_L2_CNTL4_DEFAULT	0x000000c1

static u64 mmhub_v1_8_get_fb_location(struct amdgpu_device *adev)
{
	u64 base = RREG32_SOC15(MMHUB, 0, regMC_VM_FB_LOCATION_BASE);
	u64 top = RREG32_SOC15(MMHUB, 0, regMC_VM_FB_LOCATION_TOP);

	base &= MC_VM_FB_LOCATION_BASE__FB_BASE_MASK;
	base <<= 24;

	top &= MC_VM_FB_LOCATION_TOP__FB_TOP_MASK;
	top <<= 24;

	adev->gmc.fb_start = base;
	adev->gmc.fb_end = top;

	return base;
}

static void mmhub_v1_8_setup_vm_pt_regs(struct amdgpu_device *adev, uint32_t vmid,
				uint64_t page_table_base)
{
	struct amdgpu_vmhub *hub;
	int i;

	for (i = 0; i < adev->num_aid; i++) {
		hub = &adev->vmhub[AMDGPU_MMHUB0(i)];
		WREG32_SOC15_OFFSET(MMHUB, i,
				    regVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32,
				    hub->ctx_addr_distance * vmid,
				    lower_32_bits(page_table_base));

		WREG32_SOC15_OFFSET(MMHUB, i,
				    regVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32,
				    hub->ctx_addr_distance * vmid,
				    upper_32_bits(page_table_base));
	}
}

static void mmhub_v1_8_init_gart_aperture_regs(struct amdgpu_device *adev)
{
	uint64_t pt_base;
	int i;

	if (adev->gmc.pdb0_bo)
		pt_base = amdgpu_gmc_pd_addr(adev->gmc.pdb0_bo);
	else
		pt_base = amdgpu_gmc_pd_addr(adev->gart.bo);

	mmhub_v1_8_setup_vm_pt_regs(adev, 0, pt_base);

	/* If use GART for FB translation, vmid0 page table covers both
	 * vram and system memory (gart)
	 */
	for (i = 0; i < adev->num_aid; i++) {
		if (adev->gmc.pdb0_bo) {
			WREG32_SOC15(MMHUB, i,
				     regVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32,
				     (u32)(adev->gmc.fb_start >> 12));
			WREG32_SOC15(MMHUB, i,
				     regVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32,
				     (u32)(adev->gmc.fb_start >> 44));

			WREG32_SOC15(MMHUB, i,
				     regVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32,
				     (u32)(adev->gmc.gart_end >> 12));
			WREG32_SOC15(MMHUB, i,
				     regVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32,
				     (u32)(adev->gmc.gart_end >> 44));

		} else {
			WREG32_SOC15(MMHUB, i,
				     regVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32,
				     (u32)(adev->gmc.gart_start >> 12));
			WREG32_SOC15(MMHUB, i,
				     regVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32,
				     (u32)(adev->gmc.gart_start >> 44));

			WREG32_SOC15(MMHUB, i,
				     regVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32,
				     (u32)(adev->gmc.gart_end >> 12));
			WREG32_SOC15(MMHUB, i,
				     regVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32,
				     (u32)(adev->gmc.gart_end >> 44));
		}
	}
}

static void mmhub_v1_8_init_system_aperture_regs(struct amdgpu_device *adev)
{
	uint64_t value;
	uint32_t tmp;
	int i;

	for (i = 0; i < adev->num_aid; i++) {
		/* Program the AGP BAR */
		WREG32_SOC15(MMHUB, i, regMC_VM_AGP_BASE, 0);
		WREG32_SOC15(MMHUB, i, regMC_VM_AGP_BOT,
			     adev->gmc.agp_start >> 24);
		WREG32_SOC15(MMHUB, i, regMC_VM_AGP_TOP,
			     adev->gmc.agp_end >> 24);

		if (amdgpu_sriov_vf(adev))
			return;

		/* Program the system aperture low logical page number. */
		WREG32_SOC15(MMHUB, i, regMC_VM_SYSTEM_APERTURE_LOW_ADDR,
			min(adev->gmc.fb_start, adev->gmc.agp_start) >> 18);

		WREG32_SOC15(MMHUB, i, regMC_VM_SYSTEM_APERTURE_HIGH_ADDR,
			max(adev->gmc.fb_end, adev->gmc.agp_end) >> 18);

		/* In the case squeezing vram into GART aperture, we don't use
		 * FB aperture and AGP aperture. Disable them.
		 */
		if (adev->gmc.pdb0_bo) {
			WREG32_SOC15(MMHUB, i, regMC_VM_AGP_BOT, 0xFFFFFF);
			WREG32_SOC15(MMHUB, i, regMC_VM_AGP_TOP, 0);
			WREG32_SOC15(MMHUB, i, regMC_VM_FB_LOCATION_TOP, 0);
			WREG32_SOC15(MMHUB, i, regMC_VM_FB_LOCATION_BASE,
				     0x00FFFFFF);
			WREG32_SOC15(MMHUB, i,
				     regMC_VM_SYSTEM_APERTURE_LOW_ADDR,
				     0x3FFFFFFF);
			WREG32_SOC15(MMHUB, i,
				     regMC_VM_SYSTEM_APERTURE_HIGH_ADDR, 0);
		}

		/* Set default page address. */
		value = amdgpu_gmc_vram_mc2pa(adev, adev->mem_scratch.gpu_addr);
		WREG32_SOC15(MMHUB, i, regMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB,
			     (u32)(value >> 12));
		WREG32_SOC15(MMHUB, i, regMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB,
			     (u32)(value >> 44));

		/* Program "protection fault". */
		WREG32_SOC15(MMHUB, i,
			     regVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32,
			     (u32)(adev->dummy_page_addr >> 12));
		WREG32_SOC15(MMHUB, i,
			     regVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32,
			     (u32)((u64)adev->dummy_page_addr >> 44));

		tmp = RREG32_SOC15(MMHUB, i, regVM_L2_PROTECTION_FAULT_CNTL2);
		tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL2,
				    ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY, 1);
		WREG32_SOC15(MMHUB, i, regVM_L2_PROTECTION_FAULT_CNTL2, tmp);
	}
}

static void mmhub_v1_8_init_tlb_regs(struct amdgpu_device *adev)
{
	uint32_t tmp;
	int i;

	/* Setup TLB control */
	for (i = 0; i < adev->num_aid; i++) {
		tmp = RREG32_SOC15(MMHUB, i, regMC_VM_MX_L1_TLB_CNTL);

		tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, ENABLE_L1_TLB,
				    1);
		tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL,
				    SYSTEM_ACCESS_MODE, 3);
		tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL,
				    ENABLE_ADVANCED_DRIVER_MODEL, 1);
		tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL,
				    SYSTEM_APERTURE_UNMAPPED_ACCESS, 0);
		tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL,
				    MTYPE, MTYPE_UC);/* XXX for emulation. */
		tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, ATC_EN, 1);

		WREG32_SOC15(MMHUB, i, regMC_VM_MX_L1_TLB_CNTL, tmp);
	}
}

static void mmhub_v1_8_init_cache_regs(struct amdgpu_device *adev)
{
	uint32_t tmp;
	int i;

	if (amdgpu_sriov_vf(adev))
		return;

	/* Setup L2 cache */
	for (i = 0; i < adev->num_aid; i++) {
		tmp = RREG32_SOC15(MMHUB, i, regVM_L2_CNTL);
		tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, ENABLE_L2_CACHE, 1);
		tmp = REG_SET_FIELD(tmp, VM_L2_CNTL,
				    ENABLE_L2_FRAGMENT_PROCESSING, 1);
		/* XXX for emulation, Refer to closed source code.*/
		tmp = REG_SET_FIELD(tmp, VM_L2_CNTL,
				    L2_PDE0_CACHE_TAG_GENERATION_MODE, 0);
		tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, PDE_FAULT_CLASSIFICATION,
				    0);
		tmp = REG_SET_FIELD(tmp, VM_L2_CNTL,
				    CONTEXT1_IDENTITY_ACCESS_MODE, 1);
		tmp = REG_SET_FIELD(tmp, VM_L2_CNTL,
				    IDENTITY_MODE_FRAGMENT_SIZE, 0);
		WREG32_SOC15(MMHUB, i, regVM_L2_CNTL, tmp);

		tmp = RREG32_SOC15(MMHUB, i, regVM_L2_CNTL2);
		tmp = REG_SET_FIELD(tmp, VM_L2_CNTL2, INVALIDATE_ALL_L1_TLBS,
				    1);
		tmp = REG_SET_FIELD(tmp, VM_L2_CNTL2, INVALIDATE_L2_CACHE, 1);
		WREG32_SOC15(MMHUB, i, regVM_L2_CNTL2, tmp);

		tmp = regVM_L2_CNTL3_DEFAULT;
		if (adev->gmc.translate_further) {
			tmp = REG_SET_FIELD(tmp, VM_L2_CNTL3, BANK_SELECT, 12);
			tmp = REG_SET_FIELD(tmp, VM_L2_CNTL3,
					    L2_CACHE_BIGK_FRAGMENT_SIZE, 9);
		} else {
			tmp = REG_SET_FIELD(tmp, VM_L2_CNTL3, BANK_SELECT, 9);
			tmp = REG_SET_FIELD(tmp, VM_L2_CNTL3,
					    L2_CACHE_BIGK_FRAGMENT_SIZE, 6);
		}
		WREG32_SOC15(MMHUB, i, regVM_L2_CNTL3, tmp);

		tmp = regVM_L2_CNTL4_DEFAULT;
		if (adev->gmc.xgmi.connected_to_cpu) {
			tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4,
					    VMC_TAP_PDE_REQUEST_PHYSICAL, 1);
			tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4,
					    VMC_TAP_PTE_REQUEST_PHYSICAL, 1);
		} else {
			tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4,
					    VMC_TAP_PDE_REQUEST_PHYSICAL, 0);
			tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4,
					    VMC_TAP_PTE_REQUEST_PHYSICAL, 0);
		}
		WREG32_SOC15(MMHUB, i, regVM_L2_CNTL4, tmp);
 	}
}

static void mmhub_v1_8_enable_system_domain(struct amdgpu_device *adev)
{
	uint32_t tmp;
	int i;

	for (i = 0; i < adev->num_aid; i++) {
		tmp = RREG32_SOC15(MMHUB, i, regVM_CONTEXT0_CNTL);
		tmp = REG_SET_FIELD(tmp, VM_CONTEXT0_CNTL, ENABLE_CONTEXT, 1);
		tmp = REG_SET_FIELD(tmp, VM_CONTEXT0_CNTL, PAGE_TABLE_DEPTH,
				adev->gmc.vmid0_page_table_depth);
		tmp = REG_SET_FIELD(tmp,
				    VM_CONTEXT0_CNTL, PAGE_TABLE_BLOCK_SIZE,
				    adev->gmc.vmid0_page_table_block_size);
		tmp = REG_SET_FIELD(tmp, VM_CONTEXT0_CNTL,
				    RETRY_PERMISSION_OR_INVALID_PAGE_FAULT, 0);
		WREG32_SOC15(MMHUB, i, regVM_CONTEXT0_CNTL, tmp);
	}
}

static void mmhub_v1_8_disable_identity_aperture(struct amdgpu_device *adev)
{
	int i;

	if (amdgpu_sriov_vf(adev))
		return;

	for (i = 0; i < adev->num_aid; i++) {
		WREG32_SOC15(MMHUB, i,
			     regVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_LO32,
			     0XFFFFFFFF);
		WREG32_SOC15(MMHUB, i,
			     regVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_HI32,
			     0x0000000F);

		WREG32_SOC15(MMHUB, i,
			     regVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_LO32,
			     0);
		WREG32_SOC15(MMHUB, i,
			     regVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_HI32,
			     0);

		WREG32_SOC15(MMHUB, i,
			     regVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_LO32, 0);
		WREG32_SOC15(MMHUB, i,
			     regVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_HI32, 0);
	}
}

static void mmhub_v1_8_setup_vmid_config(struct amdgpu_device *adev)
{
	struct amdgpu_vmhub *hub;
	unsigned num_level, block_size;
	uint32_t tmp;
	int i, j;

	num_level = adev->vm_manager.num_level;
	block_size = adev->vm_manager.block_size;
	if (adev->gmc.translate_further)
		num_level -= 1;
	else
		block_size -= 9;

	for (j = 0; j < adev->num_aid; j++) {
		hub = &adev->vmhub[AMDGPU_MMHUB0(j)];
		for (i = 0; i <= 14; i++) {
			tmp = RREG32_SOC15_OFFSET(MMHUB, j, regVM_CONTEXT1_CNTL,
						  i);
			tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL,
					    ENABLE_CONTEXT, 1);
			tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL,
					    PAGE_TABLE_DEPTH, num_level);
			tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL,
				RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
			tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL,
				DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
			tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL,
				PDE0_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
			tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL,
				VALID_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
			tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL,
				READ_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
			tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL,
				WRITE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
			tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL,
				EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
			tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL,
					    PAGE_TABLE_BLOCK_SIZE,
					    block_size);
			/* On 9.4.3, XNACK can be enabled in the SQ
			 * per-process. Retry faults need to be enabled for
			 * that to work.
			 */
			tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL,
				RETRY_PERMISSION_OR_INVALID_PAGE_FAULT, 1);
			WREG32_SOC15_OFFSET(MMHUB, j, regVM_CONTEXT1_CNTL,
					    i * hub->ctx_distance, tmp);
			WREG32_SOC15_OFFSET(MMHUB, j,
				regVM_CONTEXT1_PAGE_TABLE_START_ADDR_LO32,
				i * hub->ctx_addr_distance, 0);
			WREG32_SOC15_OFFSET(MMHUB, j,
				regVM_CONTEXT1_PAGE_TABLE_START_ADDR_HI32,
				i * hub->ctx_addr_distance, 0);
			WREG32_SOC15_OFFSET(MMHUB, j,
				regVM_CONTEXT1_PAGE_TABLE_END_ADDR_LO32,
				i * hub->ctx_addr_distance,
				lower_32_bits(adev->vm_manager.max_pfn - 1));
			WREG32_SOC15_OFFSET(MMHUB, j,
				regVM_CONTEXT1_PAGE_TABLE_END_ADDR_HI32,
				i * hub->ctx_addr_distance,
				upper_32_bits(adev->vm_manager.max_pfn - 1));
		}
	}
}

static void mmhub_v1_8_program_invalidation(struct amdgpu_device *adev)
{
	struct amdgpu_vmhub *hub;
	unsigned i, j;

	for (j = 0; j < adev->num_aid; j++) {
		hub = &adev->vmhub[AMDGPU_MMHUB0(j)];
		for (i = 0; i < 18; ++i) {
			WREG32_SOC15_OFFSET(MMHUB, j,
					regVM_INVALIDATE_ENG0_ADDR_RANGE_LO32,
					i * hub->eng_addr_distance, 0xffffffff);
			WREG32_SOC15_OFFSET(MMHUB, j,
					regVM_INVALIDATE_ENG0_ADDR_RANGE_HI32,
					i * hub->eng_addr_distance, 0x1f);
		}
	}
}

static int mmhub_v1_8_gart_enable(struct amdgpu_device *adev)
{
	if (amdgpu_sriov_vf(adev)) {
		/*
		 * MC_VM_FB_LOCATION_BASE/TOP is NULL for VF, becuase they are
		 * VF copy registers so vbios post doesn't program them, for
		 * SRIOV driver need to program them
		 */
		WREG32_SOC15(MMHUB, 0, regMC_VM_FB_LOCATION_BASE,
			     adev->gmc.vram_start >> 24);
		WREG32_SOC15(MMHUB, 0, regMC_VM_FB_LOCATION_TOP,
			     adev->gmc.vram_end >> 24);
	}

	/* GART Enable. */
	mmhub_v1_8_init_gart_aperture_regs(adev);
	mmhub_v1_8_init_system_aperture_regs(adev);
	mmhub_v1_8_init_tlb_regs(adev);
	mmhub_v1_8_init_cache_regs(adev);

	mmhub_v1_8_enable_system_domain(adev);
	mmhub_v1_8_disable_identity_aperture(adev);
	mmhub_v1_8_setup_vmid_config(adev);
	mmhub_v1_8_program_invalidation(adev);

	return 0;
}

static void mmhub_v1_8_gart_disable(struct amdgpu_device *adev)
{
	struct amdgpu_vmhub *hub;
	u32 tmp;
	u32 i, j;

	/* Disable all tables */
	for (j = 0; j < adev->num_aid; j++) {
		hub = &adev->vmhub[AMDGPU_MMHUB0(j)];
		for (i = 0; i < 16; i++)
			WREG32_SOC15_OFFSET(MMHUB, j, regVM_CONTEXT0_CNTL,
					    i * hub->ctx_distance, 0);

		/* Setup TLB control */
		tmp = RREG32_SOC15(MMHUB, j, regMC_VM_MX_L1_TLB_CNTL);
		tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, ENABLE_L1_TLB,
				    0);
		tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL,
				    ENABLE_ADVANCED_DRIVER_MODEL, 0);
		WREG32_SOC15(MMHUB, j, regMC_VM_MX_L1_TLB_CNTL, tmp);

		if (!amdgpu_sriov_vf(adev)) {
			/* Setup L2 cache */
			tmp = RREG32_SOC15(MMHUB, j, regVM_L2_CNTL);
			tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, ENABLE_L2_CACHE,
					    0);
			WREG32_SOC15(MMHUB, j, regVM_L2_CNTL, tmp);
			WREG32_SOC15(MMHUB, j, regVM_L2_CNTL3, 0);
		}
	}
}

/**
 * mmhub_v1_8_set_fault_enable_default - update GART/VM fault handling
 *
 * @adev: amdgpu_device pointer
 * @value: true redirects VM faults to the default page
 */
static void mmhub_v1_8_set_fault_enable_default(struct amdgpu_device *adev, bool value)
{
	u32 tmp;
	int i;

	if (amdgpu_sriov_vf(adev))
		return;

	for (i = 0; i < adev->num_aid; i++) {
		tmp = RREG32_SOC15(MMHUB, i, regVM_L2_PROTECTION_FAULT_CNTL);
		tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL,
				RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
		tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL,
				PDE0_PROTECTION_FAULT_ENABLE_DEFAULT, value);
		tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL,
				PDE1_PROTECTION_FAULT_ENABLE_DEFAULT, value);
		tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL,
				PDE2_PROTECTION_FAULT_ENABLE_DEFAULT, value);
		tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL,
			TRANSLATE_FURTHER_PROTECTION_FAULT_ENABLE_DEFAULT,
			value);
		tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL,
				NACK_PROTECTION_FAULT_ENABLE_DEFAULT, value);
		tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL,
				DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
		tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL,
				VALID_PROTECTION_FAULT_ENABLE_DEFAULT, value);
		tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL,
				READ_PROTECTION_FAULT_ENABLE_DEFAULT, value);
		tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL,
				WRITE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
		tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL,
				EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
		if (!value) {
			tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL,
					    CRASH_ON_NO_RETRY_FAULT, 1);
			tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL,
					    CRASH_ON_RETRY_FAULT, 1);
		}

		WREG32_SOC15(MMHUB, i, regVM_L2_PROTECTION_FAULT_CNTL, tmp);
	}
}

static void mmhub_v1_8_init(struct amdgpu_device *adev)
{
	struct amdgpu_vmhub *hub;
	int i;

	for (i = 0; i < adev->num_aid; i++) {
		hub = &adev->vmhub[AMDGPU_MMHUB0(i)];

		hub->ctx0_ptb_addr_lo32 = SOC15_REG_OFFSET(MMHUB, i,
			regVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32);
		hub->ctx0_ptb_addr_hi32 = SOC15_REG_OFFSET(MMHUB, i,
			regVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32);
		hub->vm_inv_eng0_req =
			SOC15_REG_OFFSET(MMHUB, i, regVM_INVALIDATE_ENG0_REQ);
		hub->vm_inv_eng0_ack =
			SOC15_REG_OFFSET(MMHUB, i, regVM_INVALIDATE_ENG0_ACK);
		hub->vm_context0_cntl =
			SOC15_REG_OFFSET(MMHUB, i, regVM_CONTEXT0_CNTL);
		hub->vm_l2_pro_fault_status = SOC15_REG_OFFSET(MMHUB, i,
			regVM_L2_PROTECTION_FAULT_STATUS);
		hub->vm_l2_pro_fault_cntl = SOC15_REG_OFFSET(MMHUB, i,
			regVM_L2_PROTECTION_FAULT_CNTL);

		hub->ctx_distance = regVM_CONTEXT1_CNTL - regVM_CONTEXT0_CNTL;
		hub->ctx_addr_distance =
			regVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32 -
			regVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32;
		hub->eng_distance = regVM_INVALIDATE_ENG1_REQ -
			regVM_INVALIDATE_ENG0_REQ;
		hub->eng_addr_distance = regVM_INVALIDATE_ENG1_ADDR_RANGE_LO32 -
			regVM_INVALIDATE_ENG0_ADDR_RANGE_LO32;
	}
}

static int mmhub_v1_8_set_clockgating(struct amdgpu_device *adev,
				      enum amd_clockgating_state state)
{
	return 0;
}

static void mmhub_v1_8_get_clockgating(struct amdgpu_device *adev, u64 *flags)
{

}

const struct amdgpu_mmhub_funcs mmhub_v1_8_funcs = {
	.get_fb_location = mmhub_v1_8_get_fb_location,
	.init = mmhub_v1_8_init,
	.gart_enable = mmhub_v1_8_gart_enable,
	.set_fault_enable_default = mmhub_v1_8_set_fault_enable_default,
	.gart_disable = mmhub_v1_8_gart_disable,
	.setup_vm_pt_regs = mmhub_v1_8_setup_vm_pt_regs,
	.set_clockgating = mmhub_v1_8_set_clockgating,
	.get_clockgating = mmhub_v1_8_get_clockgating,
};
