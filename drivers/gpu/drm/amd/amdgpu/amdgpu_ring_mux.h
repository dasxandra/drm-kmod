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

#ifndef __AMDGPU_RING_MUX__
#define __AMDGPU_RING_MUX__

#include <linux/timer.h>
#include <linux/spinlock.h>
#include "amdgpu_ring.h"

struct amdgpu_ring;
/**
 * struct amdgpu_mux_entry - the entry recording software rings copying information.
 * @ring: the pointer to the software ring.
 * @start_ptr_in_hw_ring: last start location copied to in the hardware ring.
 * @end_ptr_in_hw_ring: last end location copied to in the hardware ring.
 * @sw_cptr: the position of the copy pointer in the sw ring.
 * @sw_rptr: the read pointer in software ring.
 * @sw_wptr: the write pointer in software ring.
 */
struct amdgpu_mux_entry {
	struct amdgpu_ring      *ring;
	u64                     start_ptr_in_hw_ring;
	u64                     end_ptr_in_hw_ring;
	u64                     sw_cptr;
	u64                     sw_rptr;
	u64                     sw_wptr;
};

struct amdgpu_ring_mux {
	struct amdgpu_ring      *real_ring;

	struct amdgpu_mux_entry *ring_entry;
	unsigned int            num_ring_entries;
	unsigned int            ring_entry_size;
	/*the lock for copy data from different software rings*/
	spinlock_t              lock;
};

int amdgpu_ring_mux_init(struct amdgpu_ring_mux *mux, struct amdgpu_ring *ring,
			 unsigned int entry_size);
void amdgpu_ring_mux_fini(struct amdgpu_ring_mux *mux);
int amdgpu_ring_mux_add_sw_ring(struct amdgpu_ring_mux *mux, struct amdgpu_ring *ring);
void amdgpu_ring_mux_set_wptr(struct amdgpu_ring_mux *mux, struct amdgpu_ring *ring, u64 wptr);
u64 amdgpu_ring_mux_get_wptr(struct amdgpu_ring_mux *mux, struct amdgpu_ring *ring);
u64 amdgpu_ring_mux_get_rptr(struct amdgpu_ring_mux *mux, struct amdgpu_ring *ring);

u64 amdgpu_sw_ring_get_rptr_gfx(struct amdgpu_ring *ring);
u64 amdgpu_sw_ring_get_wptr_gfx(struct amdgpu_ring *ring);
void amdgpu_sw_ring_set_wptr_gfx(struct amdgpu_ring *ring);

void amdgpu_sw_ring_insert_nop(struct amdgpu_ring *ring, uint32_t count);
void amdgpu_sw_ring_ib_begin(struct amdgpu_ring *ring);
void amdgpu_sw_ring_ib_end(struct amdgpu_ring *ring);

const char *amdgpu_sw_ring_name(int idx);
unsigned int amdgpu_sw_ring_priority(int idx);
#endif
