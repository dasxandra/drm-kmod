/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef _INTEL_GSC_UC_HECI_CMD_SUBMIT_H_
#define _INTEL_GSC_UC_HECI_CMD_SUBMIT_H_

#include <linux/types.h>

struct intel_gsc_uc;
struct intel_gsc_mtl_header {
	u32 validity_marker;
#define GSC_HECI_VALIDITY_MARKER 0xA578875A

	u8 heci_client_id;
#define HECI_MEADDRESS_PROXY 10
#define HECI_MEADDRESS_PXP 17
#define HECI_MEADDRESS_HDCP 18

	u8 reserved1;

	u16 header_version;
#define MTL_GSC_HEADER_VERSION 1

	/*
	 * FW allows host to decide host_session handle
	 * as it sees fit.
	 * For intertracebility reserving select bits(60-63)
	 * to differentiate caller-target subsystem
	 * 0000 - HDCP
	 * 0001 - PXP Single Session
	 */
	u64 host_session_handle;
#define HOST_SESSION_MASK	REG_GENMASK64(63, 60)
#define HOST_SESSION_PXP_SINGLE BIT_ULL(60)
	u64 gsc_message_handle;

	u32 message_size; /* lower 20 bits only, upper 12 are reserved */

	/*
	 * Flags mask:
	 * Bit 0: Pending
	 * Bit 1: Session Cleanup;
	 * Bits 2-15: Flags
	 * Bits 16-31: Extension Size
	 * According to internal spec flags are either input or output
	 * we distinguish the flags using OUTFLAG or INFLAG
	 */
	u32 flags;
#define GSC_OUTFLAG_MSG_PENDING	1

	u32 status;
} __packed;

int intel_gsc_uc_heci_cmd_submit_packet(struct intel_gsc_uc *gsc,
					u64 addr_in, u32 size_in,
					u64 addr_out, u32 size_out);
void intel_gsc_uc_heci_cmd_emit_mtl_header(struct intel_gsc_mtl_header *header,
					   u8 heci_client_id, u32 message_size,
					   u64 host_session_id);
#endif
