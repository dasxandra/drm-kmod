/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/**************************************************************************
 *
 * Copyright (c) 2006-2009 VMware, Inc., Palo Alto, CA., USA
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
/*
 * Authors: Thomas Hellstrom <thellstrom-at-vmware-dot-com>
 */

#define pr_fmt(fmt) "[TTM] " fmt

#include <drm/ttm/ttm_module.h>
#include <drm/ttm/ttm_bo_driver.h>
#include <drm/ttm/ttm_placement.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/file.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/dma-resv.h>

static void ttm_bo_global_kobj_release(struct kobject *kobj);

/**
 * ttm_global_mutex - protecting the global BO state
 */
DEFINE_MUTEX(ttm_global_mutex);
unsigned ttm_bo_glob_use_count;
struct ttm_bo_global ttm_bo_glob;
EXPORT_SYMBOL(ttm_bo_glob);

static struct attribute ttm_bo_count = {
	.name = "bo_count",
	.mode = S_IRUGO
};

/* default destructor */
static void ttm_bo_default_destroy(struct ttm_buffer_object *bo)
{
	kfree(bo);
}

static inline int ttm_mem_type_from_place(const struct ttm_place *place,
					  uint32_t *mem_type)
{
	int pos;

	pos = ffs(place->flags & TTM_PL_MASK_MEM);
	if (unlikely(!pos))
		return -EINVAL;

	*mem_type = pos - 1;
	return 0;
}

void ttm_mem_type_manager_debug(struct ttm_mem_type_manager *man,
				struct drm_printer *p)
{
	drm_printf(p, "    has_type: %d\n", man->has_type);
	drm_printf(p, "    use_type: %d\n", man->use_type);
#ifdef __linux__
	drm_printf(p, "    use_tt: %d\n", man->use_tt);
	drm_printf(p, "    size: %llu\n", man->size);
#elif defined(__FreeBSD__)
	drm_printf(p, "    size: %zu\n", man->size);
#endif
	drm_printf(p, "    available_caching: 0x%08X\n", man->available_caching);
	drm_printf(p, "    default_caching: 0x%08X\n", man->default_caching);
	if (man->func && man->func->debug)
		(*man->func->debug)(man, p);
}
EXPORT_SYMBOL(ttm_mem_type_manager_debug);

static void ttm_bo_mem_space_debug(struct ttm_buffer_object *bo,
					struct ttm_placement *placement)
{
	struct drm_printer p = drm_debug_printer(TTM_PFX);
	int i, ret, mem_type;
	struct ttm_mem_type_manager *man;

	drm_printf(&p, "No space for %p (%lu pages, %luK, %luM)\n",
		   bo, bo->mem.num_pages, bo->mem.size >> 10,
		   bo->mem.size >> 20);
	for (i = 0; i < placement->num_placement; i++) {
		ret = ttm_mem_type_from_place(&placement->placement[i],
						&mem_type);
		if (ret)
			return;
		drm_printf(&p, "  placement[%d]=0x%08X (%d)\n",
			   i, placement->placement[i].flags, mem_type);
		man = ttm_manager_type(bo->bdev, mem_type);
		ttm_mem_type_manager_debug(man, &p);
	}
}

static ssize_t ttm_bo_global_show(struct kobject *kobj,
				  struct attribute *attr,
				  char *buffer)
{
	struct ttm_bo_global *glob =
		container_of(kobj, struct ttm_bo_global, kobj);

	return snprintf(buffer, PAGE_SIZE, "%d\n",
				atomic_read(&glob->bo_count));
}

static struct attribute *ttm_bo_global_attrs[] = {
	&ttm_bo_count,
	NULL
};

static const struct sysfs_ops ttm_bo_global_ops = {
	.show = &ttm_bo_global_show
};

static struct kobj_type ttm_bo_glob_kobj_type  = {
	.release = &ttm_bo_global_kobj_release,
	.sysfs_ops = &ttm_bo_global_ops,
	.default_attrs = ttm_bo_global_attrs
};


static inline uint32_t ttm_bo_type_flags(unsigned type)
{
	return 1 << (type);
}

static void ttm_bo_add_mem_to_lru(struct ttm_buffer_object *bo,
				  struct ttm_mem_reg *mem)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_mem_type_manager *man;

	if (!list_empty(&bo->lru))
		return;

	if (mem->placement & TTM_PL_FLAG_NO_EVICT)
		return;

	man = ttm_manager_type(bdev, mem->mem_type);
	list_add_tail(&bo->lru, &man->lru[bo->priority]);

	if (man->use_tt && bo->ttm &&
	    !(bo->ttm->page_flags & (TTM_PAGE_FLAG_SG |
				     TTM_PAGE_FLAG_SWAPPED))) {
		list_add_tail(&bo->swap, &ttm_bo_glob.swap_lru[bo->priority]);
	}
}

static void ttm_bo_del_from_lru(struct ttm_buffer_object *bo)
{
	struct ttm_bo_device *bdev = bo->bdev;
	bool notify = false;

	if (!list_empty(&bo->swap)) {
		list_del_init(&bo->swap);
		notify = true;
	}
	if (!list_empty(&bo->lru)) {
		list_del_init(&bo->lru);
		notify = true;
	}

	if (notify && bdev->driver->del_from_lru_notify)
		bdev->driver->del_from_lru_notify(bo);
}

static void ttm_bo_bulk_move_set_pos(struct ttm_lru_bulk_move_pos *pos,
				     struct ttm_buffer_object *bo)
{
	if (!pos->first)
		pos->first = bo;
	pos->last = bo;
}

void ttm_bo_move_to_lru_tail(struct ttm_buffer_object *bo,
			     struct ttm_lru_bulk_move *bulk)
{
	dma_resv_assert_held(bo->base.resv);

	ttm_bo_del_from_lru(bo);
	ttm_bo_add_mem_to_lru(bo, &bo->mem);

	if (bulk && !(bo->mem.placement & TTM_PL_FLAG_NO_EVICT)) {
		switch (bo->mem.mem_type) {
		case TTM_PL_TT:
			ttm_bo_bulk_move_set_pos(&bulk->tt[bo->priority], bo);
			break;

		case TTM_PL_VRAM:
			ttm_bo_bulk_move_set_pos(&bulk->vram[bo->priority], bo);
			break;
		}
		if (bo->ttm && !(bo->ttm->page_flags &
				 (TTM_PAGE_FLAG_SG | TTM_PAGE_FLAG_SWAPPED)))
			ttm_bo_bulk_move_set_pos(&bulk->swap[bo->priority], bo);
	}
}
EXPORT_SYMBOL(ttm_bo_move_to_lru_tail);

void ttm_bo_bulk_move_lru_tail(struct ttm_lru_bulk_move *bulk)
{
	unsigned i;

	for (i = 0; i < TTM_MAX_BO_PRIORITY; ++i) {
		struct ttm_lru_bulk_move_pos *pos = &bulk->tt[i];
		struct ttm_mem_type_manager *man;

		if (!pos->first)
			continue;

		dma_resv_assert_held(pos->first->base.resv);
		dma_resv_assert_held(pos->last->base.resv);

		man = ttm_manager_type(pos->first->bdev, TTM_PL_TT);
		list_bulk_move_tail(&man->lru[i], &pos->first->lru,
				    &pos->last->lru);
	}

	for (i = 0; i < TTM_MAX_BO_PRIORITY; ++i) {
		struct ttm_lru_bulk_move_pos *pos = &bulk->vram[i];
		struct ttm_mem_type_manager *man;

		if (!pos->first)
			continue;

		dma_resv_assert_held(pos->first->base.resv);
		dma_resv_assert_held(pos->last->base.resv);

		man = ttm_manager_type(pos->first->bdev, TTM_PL_VRAM);
		list_bulk_move_tail(&man->lru[i], &pos->first->lru,
				    &pos->last->lru);
	}

	for (i = 0; i < TTM_MAX_BO_PRIORITY; ++i) {
		struct ttm_lru_bulk_move_pos *pos = &bulk->swap[i];
		struct list_head *lru;

		if (!pos->first)
			continue;

		dma_resv_assert_held(pos->first->base.resv);
		dma_resv_assert_held(pos->last->base.resv);

		lru = &ttm_bo_glob.swap_lru[i];
		list_bulk_move_tail(lru, &pos->first->swap, &pos->last->swap);
	}
}
EXPORT_SYMBOL(ttm_bo_bulk_move_lru_tail);

static int ttm_bo_handle_move_mem(struct ttm_buffer_object *bo,
				  struct ttm_mem_reg *mem, bool evict,
				  struct ttm_operation_ctx *ctx)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_mem_type_manager *old_man = ttm_manager_type(bdev, bo->mem.mem_type);
	struct ttm_mem_type_manager *new_man = ttm_manager_type(bdev, mem->mem_type);
	int ret;

	ret = ttm_mem_io_lock(old_man, true);
	if (unlikely(ret != 0))
		goto out_err;
	ttm_bo_unmap_virtual_locked(bo);
	ttm_mem_io_unlock(old_man);

	/*
	 * Create and bind a ttm if required.
	 */

	if (new_man->use_tt) {
		/* Zero init the new TTM structure if the old location should
		 * have used one as well.
		 */
		ret = ttm_tt_create(bo, old_man->use_tt);
		if (ret)
			goto out_err;

		ret = ttm_tt_set_placement_caching(bo->ttm, mem->placement);
		if (ret)
			goto out_err;

		if (mem->mem_type != TTM_PL_SYSTEM) {
			ret = ttm_tt_bind(bo->ttm, mem, ctx);
			if (ret)
				goto out_err;
		}

		if (bo->mem.mem_type == TTM_PL_SYSTEM) {
			if (bdev->driver->move_notify)
				bdev->driver->move_notify(bo, evict, mem);
			bo->mem = *mem;
			goto moved;
		}
	}

	if (bdev->driver->move_notify)
		bdev->driver->move_notify(bo, evict, mem);

	if (old_man->use_tt && new_man->use_tt)
		ret = ttm_bo_move_ttm(bo, ctx, mem);
	else if (bdev->driver->move)
		ret = bdev->driver->move(bo, evict, ctx, mem);
	else
		ret = ttm_bo_move_memcpy(bo, ctx, mem);

	if (ret) {
		if (bdev->driver->move_notify) {
			swap(*mem, bo->mem);
			bdev->driver->move_notify(bo, false, mem);
			swap(*mem, bo->mem);
		}

		goto out_err;
	}

moved:
	bo->evicted = false;

	ctx->bytes_moved += bo->num_pages << PAGE_SHIFT;
	return 0;

out_err:
	new_man = ttm_manager_type(bdev, bo->mem.mem_type);
	if (!new_man->use_tt) {
		ttm_tt_destroy(bo->ttm);
		bo->ttm = NULL;
	}

	return ret;
}

/**
 * Call bo::reserved.
 * Will release GPU memory type usage on destruction.
 * This is the place to put in driver specific hooks to release
 * driver private resources.
 * Will release the bo::reserved lock.
 */

static void ttm_bo_cleanup_memtype_use(struct ttm_buffer_object *bo)
{
	if (bo->bdev->driver->move_notify)
		bo->bdev->driver->move_notify(bo, false, NULL);

	ttm_tt_destroy(bo->ttm);
	bo->ttm = NULL;
	ttm_bo_mem_put(bo, &bo->mem);
}

static int ttm_bo_individualize_resv(struct ttm_buffer_object *bo)
{
	int r;

	if (bo->base.resv == &bo->base._resv)
		return 0;

	BUG_ON(!dma_resv_trylock(&bo->base._resv));

	r = dma_resv_copy_fences(&bo->base._resv, bo->base.resv);
	dma_resv_unlock(&bo->base._resv);
	if (r)
		return r;

	if (bo->type != ttm_bo_type_sg) {
		/* This works because the BO is about to be destroyed and nobody
		 * reference it any more. The only tricky case is the trylock on
		 * the resv object while holding the lru_lock.
		 */
		spin_lock(&ttm_bo_glob.lru_lock);
		bo->base.resv = &bo->base._resv;
		spin_unlock(&ttm_bo_glob.lru_lock);
	}

	return r;
}

static void ttm_bo_flush_all_fences(struct ttm_buffer_object *bo)
{
	struct dma_resv *resv = &bo->base._resv;
	struct dma_resv_list *fobj;
	struct dma_fence *fence;
	int i;

	rcu_read_lock();
	fobj = rcu_dereference(resv->fence);
	fence = rcu_dereference(resv->fence_excl);
	if (fence && !fence->ops->signaled)
		dma_fence_enable_sw_signaling(fence);

	for (i = 0; fobj && i < fobj->shared_count; ++i) {
		fence = rcu_dereference(fobj->shared[i]);

		if (!fence->ops->signaled)
			dma_fence_enable_sw_signaling(fence);
	}
	rcu_read_unlock();
}

/**
 * function ttm_bo_cleanup_refs
 * If bo idle, remove from lru lists, and unref.
 * If not idle, block if possible.
 *
 * Must be called with lru_lock and reservation held, this function
 * will drop the lru lock and optionally the reservation lock before returning.
 *
 * @interruptible         Any sleeps should occur interruptibly.
 * @no_wait_gpu           Never wait for gpu. Return -EBUSY instead.
 * @unlock_resv           Unlock the reservation lock as well.
 */

static int ttm_bo_cleanup_refs(struct ttm_buffer_object *bo,
			       bool interruptible, bool no_wait_gpu,
			       bool unlock_resv)
{
	struct dma_resv *resv = &bo->base._resv;
	int ret;

	if (dma_resv_test_signaled_rcu(resv, true))
		ret = 0;
	else
		ret = -EBUSY;

	if (ret && !no_wait_gpu) {
		long lret;

		if (unlock_resv)
			dma_resv_unlock(bo->base.resv);
		spin_unlock(&ttm_bo_glob.lru_lock);

		lret = dma_resv_wait_timeout_rcu(resv, true, interruptible,
						 30 * HZ);

		if (lret < 0)
			return lret;
		else if (lret == 0)
			return -EBUSY;

		spin_lock(&ttm_bo_glob.lru_lock);
		if (unlock_resv && !dma_resv_trylock(bo->base.resv)) {
			/*
			 * We raced, and lost, someone else holds the reservation now,
			 * and is probably busy in ttm_bo_cleanup_memtype_use.
			 *
			 * Even if it's not the case, because we finished waiting any
			 * delayed destruction would succeed, so just return success
			 * here.
			 */
			spin_unlock(&ttm_bo_glob.lru_lock);
			return 0;
		}
		ret = 0;
	}

	if (ret || unlikely(list_empty(&bo->ddestroy))) {
		if (unlock_resv)
			dma_resv_unlock(bo->base.resv);
		spin_unlock(&ttm_bo_glob.lru_lock);
		return ret;
	}

	ttm_bo_del_from_lru(bo);
	list_del_init(&bo->ddestroy);
	spin_unlock(&ttm_bo_glob.lru_lock);
	ttm_bo_cleanup_memtype_use(bo);

	if (unlock_resv)
		dma_resv_unlock(bo->base.resv);

	ttm_bo_put(bo);

	return 0;
}

/**
 * Traverse the delayed list, and call ttm_bo_cleanup_refs on all
 * encountered buffers.
 */
static bool ttm_bo_delayed_delete(struct ttm_bo_device *bdev, bool remove_all)
{
	struct ttm_bo_global *glob = &ttm_bo_glob;
	struct list_head removed;
	bool empty;

	INIT_LIST_HEAD(&removed);

	spin_lock(&glob->lru_lock);
	while (!list_empty(&bdev->ddestroy)) {
		struct ttm_buffer_object *bo;

		bo = list_first_entry(&bdev->ddestroy, struct ttm_buffer_object,
				      ddestroy);
		list_move_tail(&bo->ddestroy, &removed);
		if (!ttm_bo_get_unless_zero(bo))
			continue;

		if (remove_all || bo->base.resv != &bo->base._resv) {
			spin_unlock(&glob->lru_lock);
			dma_resv_lock(bo->base.resv, NULL);

			spin_lock(&glob->lru_lock);
			ttm_bo_cleanup_refs(bo, false, !remove_all, true);

		} else if (dma_resv_trylock(bo->base.resv)) {
			ttm_bo_cleanup_refs(bo, false, !remove_all, true);
		} else {
			spin_unlock(&glob->lru_lock);
		}

		ttm_bo_put(bo);
		spin_lock(&glob->lru_lock);
	}
	list_splice_tail(&removed, &bdev->ddestroy);
	empty = list_empty(&bdev->ddestroy);
	spin_unlock(&glob->lru_lock);

	return empty;
}

static void ttm_bo_delayed_workqueue(struct work_struct *work)
{
	struct ttm_bo_device *bdev =
	    container_of(work, struct ttm_bo_device, wq.work);

	if (!ttm_bo_delayed_delete(bdev, false))
		schedule_delayed_work(&bdev->wq,
				      ((HZ / 100) < 1) ? 1 : HZ / 100);
}

static void ttm_bo_release(struct kref *kref)
{
	struct ttm_buffer_object *bo =
	    container_of(kref, struct ttm_buffer_object, kref);
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_mem_type_manager *man = ttm_manager_type(bdev, bo->mem.mem_type);
	size_t acc_size = bo->acc_size;
	int ret;

	if (!bo->deleted) {
		ret = ttm_bo_individualize_resv(bo);
		if (ret) {
			/* Last resort, if we fail to allocate memory for the
			 * fences block for the BO to become idle
			 */
			dma_resv_wait_timeout_rcu(bo->base.resv, true, false,
						  30 * HZ);
		}

		if (bo->bdev->driver->release_notify)
			bo->bdev->driver->release_notify(bo);

		drm_vma_offset_remove(bdev->vma_manager, &bo->base.vma_node);
		ttm_mem_io_lock(man, false);
		ttm_mem_io_free_vm(bo);
		ttm_mem_io_unlock(man);
	}

	if (!dma_resv_test_signaled_rcu(bo->base.resv, true) ||
	    !dma_resv_trylock(bo->base.resv)) {
		/* The BO is not idle, resurrect it for delayed destroy */
		ttm_bo_flush_all_fences(bo);
		bo->deleted = true;

		spin_lock(&ttm_bo_glob.lru_lock);

		/*
		 * Make NO_EVICT bos immediately available to
		 * shrinkers, now that they are queued for
		 * destruction.
		 */
		if (bo->mem.placement & TTM_PL_FLAG_NO_EVICT) {
			bo->mem.placement &= ~TTM_PL_FLAG_NO_EVICT;
			ttm_bo_del_from_lru(bo);
			ttm_bo_add_mem_to_lru(bo, &bo->mem);
		}

		kref_init(&bo->kref);
		list_add_tail(&bo->ddestroy, &bdev->ddestroy);
		spin_unlock(&ttm_bo_glob.lru_lock);

		schedule_delayed_work(&bdev->wq,
				      ((HZ / 100) < 1) ? 1 : HZ / 100);
		return;
	}

	spin_lock(&ttm_bo_glob.lru_lock);
	ttm_bo_del_from_lru(bo);
	list_del(&bo->ddestroy);
	spin_unlock(&ttm_bo_glob.lru_lock);

	ttm_bo_cleanup_memtype_use(bo);
	dma_resv_unlock(bo->base.resv);

	atomic_dec(&ttm_bo_glob.bo_count);
	dma_fence_put(bo->moving);
	if (!ttm_bo_uses_embedded_gem_object(bo))
		dma_resv_fini(&bo->base._resv);
	bo->destroy(bo);
	ttm_mem_global_free(&ttm_mem_glob, acc_size);
}

void ttm_bo_put(struct ttm_buffer_object *bo)
{
	kref_put(&bo->kref, ttm_bo_release);
}
EXPORT_SYMBOL(ttm_bo_put);

int ttm_bo_lock_delayed_workqueue(struct ttm_bo_device *bdev)
{
	return cancel_delayed_work_sync(&bdev->wq);
}
EXPORT_SYMBOL(ttm_bo_lock_delayed_workqueue);

void ttm_bo_unlock_delayed_workqueue(struct ttm_bo_device *bdev, int resched)
{
	if (resched)
		schedule_delayed_work(&bdev->wq,
				      ((HZ / 100) < 1) ? 1 : HZ / 100);
}
EXPORT_SYMBOL(ttm_bo_unlock_delayed_workqueue);

static int ttm_bo_evict(struct ttm_buffer_object *bo,
			struct ttm_operation_ctx *ctx)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_mem_reg evict_mem;
	struct ttm_placement placement;
	int ret = 0;

	dma_resv_assert_held(bo->base.resv);

	placement.num_placement = 0;
	placement.num_busy_placement = 0;
	bdev->driver->evict_flags(bo, &placement);

	if (!placement.num_placement && !placement.num_busy_placement) {
		ttm_bo_wait(bo, false, false);

		ttm_bo_cleanup_memtype_use(bo);
		return 0;
	}

	evict_mem = bo->mem;
	evict_mem.mm_node = NULL;
	evict_mem.bus.io_reserved_vm = false;
	evict_mem.bus.io_reserved_count = 0;

	ret = ttm_bo_mem_space(bo, &placement, &evict_mem, ctx);
	if (ret) {
		if (ret != -ERESTARTSYS) {
			pr_err("Failed to find memory space for buffer 0x%p eviction\n",
			       bo);
			ttm_bo_mem_space_debug(bo, &placement);
		}
		goto out;
	}

	ret = ttm_bo_handle_move_mem(bo, &evict_mem, true, ctx);
	if (unlikely(ret)) {
		if (ret != -ERESTARTSYS)
			pr_err("Buffer eviction failed\n");
		ttm_bo_mem_put(bo, &evict_mem);
		goto out;
	}
	bo->evicted = true;
out:
	return ret;
}

bool ttm_bo_eviction_valuable(struct ttm_buffer_object *bo,
			      const struct ttm_place *place)
{
	/* Don't evict this BO if it's outside of the
	 * requested placement range
	 */
	if (place->fpfn >= (bo->mem.start + bo->mem.size) ||
	    (place->lpfn && place->lpfn <= bo->mem.start))
		return false;

	return true;
}
EXPORT_SYMBOL(ttm_bo_eviction_valuable);

/**
 * Check the target bo is allowable to be evicted or swapout, including cases:
 *
 * a. if share same reservation object with ctx->resv, have assumption
 * reservation objects should already be locked, so not lock again and
 * return true directly when either the opreation allow_reserved_eviction
 * or the target bo already is in delayed free list;
 *
 * b. Otherwise, trylock it.
 */
static bool ttm_bo_evict_swapout_allowable(struct ttm_buffer_object *bo,
			struct ttm_operation_ctx *ctx, bool *locked, bool *busy)
{
	bool ret = false;

	if (bo->base.resv == ctx->resv) {
		dma_resv_assert_held(bo->base.resv);
		if (ctx->flags & TTM_OPT_FLAG_ALLOW_RES_EVICT)
			ret = true;
		*locked = false;
		if (busy)
			*busy = false;
	} else {
		ret = dma_resv_trylock(bo->base.resv);
		*locked = ret;
		if (busy)
			*busy = !ret;
	}

	return ret;
}

/**
 * ttm_mem_evict_wait_busy - wait for a busy BO to become available
 *
 * @busy_bo: BO which couldn't be locked with trylock
 * @ctx: operation context
 * @ticket: acquire ticket
 *
 * Try to lock a busy buffer object to avoid failing eviction.
 */
static int ttm_mem_evict_wait_busy(struct ttm_buffer_object *busy_bo,
				   struct ttm_operation_ctx *ctx,
				   struct ww_acquire_ctx *ticket)
{
	int r;

	if (!busy_bo || !ticket)
		return -EBUSY;

	if (ctx->interruptible)
		r = dma_resv_lock_interruptible(busy_bo->base.resv,
							  ticket);
	else
		r = dma_resv_lock(busy_bo->base.resv, ticket);

	/*
	 * TODO: It would be better to keep the BO locked until allocation is at
	 * least tried one more time, but that would mean a much larger rework
	 * of TTM.
	 */
	if (!r)
		dma_resv_unlock(busy_bo->base.resv);

	return r == -EDEADLK ? -EBUSY : r;
}

static int ttm_mem_evict_first(struct ttm_bo_device *bdev,
			       struct ttm_mem_type_manager *man,
			       const struct ttm_place *place,
			       struct ttm_operation_ctx *ctx,
			       struct ww_acquire_ctx *ticket)
{
	struct ttm_buffer_object *bo = NULL, *busy_bo = NULL;
	bool locked = false;
	unsigned i;
	int ret;

	spin_lock(&ttm_bo_glob.lru_lock);
	for (i = 0; i < TTM_MAX_BO_PRIORITY; ++i) {
		list_for_each_entry(bo, &man->lru[i], lru) {
			bool busy;

			if (!ttm_bo_evict_swapout_allowable(bo, ctx, &locked,
							    &busy)) {
				if (busy && !busy_bo && ticket !=
				    dma_resv_locking_ctx(bo->base.resv))
					busy_bo = bo;
				continue;
			}

			if (place && !bdev->driver->eviction_valuable(bo,
								      place)) {
				if (locked)
					dma_resv_unlock(bo->base.resv);
				continue;
			}
			if (!ttm_bo_get_unless_zero(bo)) {
				if (locked)
					dma_resv_unlock(bo->base.resv);
				continue;
			}
			break;
		}

		/* If the inner loop terminated early, we have our candidate */
		if (&bo->lru != &man->lru[i])
			break;

		bo = NULL;
	}

	if (!bo) {
		if (busy_bo && !ttm_bo_get_unless_zero(busy_bo))
			busy_bo = NULL;
		spin_unlock(&ttm_bo_glob.lru_lock);
		ret = ttm_mem_evict_wait_busy(busy_bo, ctx, ticket);
		if (busy_bo)
			ttm_bo_put(busy_bo);
		return ret;
	}

	if (bo->deleted) {
		ret = ttm_bo_cleanup_refs(bo, ctx->interruptible,
					  ctx->no_wait_gpu, locked);
		ttm_bo_put(bo);
		return ret;
	}

	spin_unlock(&ttm_bo_glob.lru_lock);

	ret = ttm_bo_evict(bo, ctx);
	if (locked)
		ttm_bo_unreserve(bo);

	ttm_bo_put(bo);
	return ret;
}

static int ttm_bo_mem_get(struct ttm_buffer_object *bo,
			  const struct ttm_place *place,
			  struct ttm_mem_reg *mem)
{
	struct ttm_mem_type_manager *man = ttm_manager_type(bo->bdev, mem->mem_type);

	mem->mm_node = NULL;
	if (!man->func || !man->func->get_node)
		return 0;

	return man->func->get_node(man, bo, place, mem);
}

void ttm_bo_mem_put(struct ttm_buffer_object *bo, struct ttm_mem_reg *mem)
{
	struct ttm_mem_type_manager *man = ttm_manager_type(bo->bdev, mem->mem_type);

	if (!man->func || !man->func->put_node)
		return;

	man->func->put_node(man, mem);
	mem->mm_node = NULL;
	mem->mem_type = TTM_PL_SYSTEM;
}
EXPORT_SYMBOL(ttm_bo_mem_put);

/**
 * Add the last move fence to the BO and reserve a new shared slot.
 */
static int ttm_bo_add_move_fence(struct ttm_buffer_object *bo,
				 struct ttm_mem_type_manager *man,
				 struct ttm_mem_reg *mem,
				 bool no_wait_gpu)
{
	struct dma_fence *fence;
	int ret;

	spin_lock(&man->move_lock);
	fence = dma_fence_get(man->move);
	spin_unlock(&man->move_lock);

	if (!fence)
		return 0;

	if (no_wait_gpu) {
		dma_fence_put(fence);
		return -EBUSY;
	}

	dma_resv_add_shared_fence(bo->base.resv, fence);

	ret = dma_resv_reserve_shared(bo->base.resv, 1);
	if (unlikely(ret)) {
		dma_fence_put(fence);
		return ret;
	}

	dma_fence_put(bo->moving);
	bo->moving = fence;
	return 0;
}

/**
 * Repeatedly evict memory from the LRU for @mem_type until we create enough
 * space, or we've evicted everything and there isn't enough space.
 */
static int ttm_bo_mem_force_space(struct ttm_buffer_object *bo,
				  const struct ttm_place *place,
				  struct ttm_mem_reg *mem,
				  struct ttm_operation_ctx *ctx)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_mem_type_manager *man = ttm_manager_type(bdev, mem->mem_type);
	struct ww_acquire_ctx *ticket;
	int ret;

	ticket = dma_resv_locking_ctx(bo->base.resv);
	do {
		ret = ttm_bo_mem_get(bo, place, mem);
		if (likely(!ret))
			break;
		if (unlikely(ret != -ENOSPC))
			return ret;
		ret = ttm_mem_evict_first(bdev, man, place, ctx,
					  ticket);
		if (unlikely(ret != 0))
			return ret;
	} while (1);

	return ttm_bo_add_move_fence(bo, man, mem, ctx->no_wait_gpu);
}

static uint32_t ttm_bo_select_caching(struct ttm_mem_type_manager *man,
				      uint32_t cur_placement,
				      uint32_t proposed_placement)
{
	uint32_t caching = proposed_placement & TTM_PL_MASK_CACHING;
	uint32_t result = proposed_placement & ~TTM_PL_MASK_CACHING;

	/**
	 * Keep current caching if possible.
	 */

	if ((cur_placement & caching) != 0)
		result |= (cur_placement & caching);
	else if ((man->default_caching & caching) != 0)
		result |= man->default_caching;
	else if ((TTM_PL_FLAG_CACHED & caching) != 0)
		result |= TTM_PL_FLAG_CACHED;
	else if ((TTM_PL_FLAG_WC & caching) != 0)
		result |= TTM_PL_FLAG_WC;
	else if ((TTM_PL_FLAG_UNCACHED & caching) != 0)
		result |= TTM_PL_FLAG_UNCACHED;

	return result;
}

static bool ttm_bo_mt_compatible(struct ttm_mem_type_manager *man,
				 uint32_t mem_type,
				 const struct ttm_place *place,
				 uint32_t *masked_placement)
{
	uint32_t cur_flags = ttm_bo_type_flags(mem_type);

	if ((cur_flags & place->flags & TTM_PL_MASK_MEM) == 0)
		return false;

	if ((place->flags & man->available_caching) == 0)
		return false;

	cur_flags |= (place->flags & man->available_caching);

	*masked_placement = cur_flags;
	return true;
}

/**
 * ttm_bo_mem_placement - check if placement is compatible
 * @bo: BO to find memory for
 * @place: where to search
 * @mem: the memory object to fill in
 * @ctx: operation context
 *
 * Check if placement is compatible and fill in mem structure.
 * Returns -EBUSY if placement won't work or negative error code.
 * 0 when placement can be used.
 */
static int ttm_bo_mem_placement(struct ttm_buffer_object *bo,
				const struct ttm_place *place,
				struct ttm_mem_reg *mem,
				struct ttm_operation_ctx *ctx)
{
	struct ttm_bo_device *bdev = bo->bdev;
	uint32_t mem_type = TTM_PL_SYSTEM;
	struct ttm_mem_type_manager *man;
	uint32_t cur_flags = 0;
	int ret;

	ret = ttm_mem_type_from_place(place, &mem_type);
	if (ret)
		return ret;

	man = ttm_manager_type(bdev, mem_type);
	if (!man->has_type || !man->use_type)
		return -EBUSY;

	if (!ttm_bo_mt_compatible(man, mem_type, place, &cur_flags))
		return -EBUSY;

	cur_flags = ttm_bo_select_caching(man, bo->mem.placement, cur_flags);
	/*
	 * Use the access and other non-mapping-related flag bits from
	 * the memory placement flags to the current flags
	 */
	ttm_flag_masked(&cur_flags, place->flags, ~TTM_PL_MASK_MEMTYPE);

	mem->mem_type = mem_type;
	mem->placement = cur_flags;

	spin_lock(&ttm_bo_glob.lru_lock);
	ttm_bo_del_from_lru(bo);
	ttm_bo_add_mem_to_lru(bo, mem);
	spin_unlock(&ttm_bo_glob.lru_lock);

	return 0;
}

/**
 * Creates space for memory region @mem according to its type.
 *
 * This function first searches for free space in compatible memory types in
 * the priority order defined by the driver.  If free space isn't found, then
 * ttm_bo_mem_force_space is attempted in priority order to evict and find
 * space.
 */
int ttm_bo_mem_space(struct ttm_buffer_object *bo,
			struct ttm_placement *placement,
			struct ttm_mem_reg *mem,
			struct ttm_operation_ctx *ctx)
{
	struct ttm_bo_device *bdev = bo->bdev;
	bool type_found = false;
	int i, ret;

	ret = dma_resv_reserve_shared(bo->base.resv, 1);
	if (unlikely(ret))
		return ret;

	for (i = 0; i < placement->num_placement; ++i) {
		const struct ttm_place *place = &placement->placement[i];
		struct ttm_mem_type_manager *man;

		ret = ttm_bo_mem_placement(bo, place, mem, ctx);
		if (ret == -EBUSY)
			continue;
		if (ret)
			goto error;

		type_found = true;
		ret = ttm_bo_mem_get(bo, place, mem);
		if (ret == -ENOSPC)
			continue;
		if (unlikely(ret))
			goto error;

		man = ttm_manager_type(bdev, mem->mem_type);
		ret = ttm_bo_add_move_fence(bo, man, mem, ctx->no_wait_gpu);
		if (unlikely(ret)) {
			ttm_bo_mem_put(bo, mem);
			if (ret == -EBUSY)
				continue;

			goto error;
		}
		return 0;
	}

	for (i = 0; i < placement->num_busy_placement; ++i) {
		const struct ttm_place *place = &placement->busy_placement[i];

		ret = ttm_bo_mem_placement(bo, place, mem, ctx);
		if (ret == -EBUSY)
			continue;
		if (ret)
			goto error;

		type_found = true;
		ret = ttm_bo_mem_force_space(bo, place, mem, ctx);
		if (likely(!ret))
			return 0;

		if (ret && ret != -EBUSY)
			goto error;
	}

	ret = -ENOMEM;
	if (!type_found) {
		pr_err(TTM_PFX "No compatible memory type found\n");
		ret = -EINVAL;
	}

error:
	if (bo->mem.mem_type == TTM_PL_SYSTEM && !list_empty(&bo->lru)) {
		ttm_bo_move_to_lru_tail_unlocked(bo);
	}

	return ret;
}
EXPORT_SYMBOL(ttm_bo_mem_space);

static int ttm_bo_move_buffer(struct ttm_buffer_object *bo,
			      struct ttm_placement *placement,
			      struct ttm_operation_ctx *ctx)
{
	int ret = 0;
	struct ttm_mem_reg mem;

	dma_resv_assert_held(bo->base.resv);

	mem.num_pages = bo->num_pages;
	mem.size = mem.num_pages << PAGE_SHIFT;
	mem.page_alignment = bo->mem.page_alignment;
	mem.bus.io_reserved_vm = false;
	mem.bus.io_reserved_count = 0;
	mem.mm_node = NULL;

	/*
	 * Determine where to move the buffer.
	 */
	ret = ttm_bo_mem_space(bo, placement, &mem, ctx);
	if (ret)
		goto out_unlock;
	ret = ttm_bo_handle_move_mem(bo, &mem, false, ctx);
out_unlock:
	if (ret)
		ttm_bo_mem_put(bo, &mem);
	return ret;
}

static bool ttm_bo_places_compat(const struct ttm_place *places,
				 unsigned num_placement,
				 struct ttm_mem_reg *mem,
				 uint32_t *new_flags)
{
	unsigned i;

	for (i = 0; i < num_placement; i++) {
		const struct ttm_place *heap = &places[i];

		if ((mem->start < heap->fpfn ||
		     (heap->lpfn != 0 && (mem->start + mem->num_pages) > heap->lpfn)))
			continue;

		*new_flags = heap->flags;
		if ((*new_flags & mem->placement & TTM_PL_MASK_CACHING) &&
		    (*new_flags & mem->placement & TTM_PL_MASK_MEM) &&
		    (!(*new_flags & TTM_PL_FLAG_CONTIGUOUS) ||
		     (mem->placement & TTM_PL_FLAG_CONTIGUOUS)))
			return true;
	}
	return false;
}

bool ttm_bo_mem_compat(struct ttm_placement *placement,
		       struct ttm_mem_reg *mem,
		       uint32_t *new_flags)
{
	if (ttm_bo_places_compat(placement->placement, placement->num_placement,
				 mem, new_flags))
		return true;

	if ((placement->busy_placement != placement->placement ||
	     placement->num_busy_placement > placement->num_placement) &&
	    ttm_bo_places_compat(placement->busy_placement,
				 placement->num_busy_placement,
				 mem, new_flags))
		return true;

	return false;
}
EXPORT_SYMBOL(ttm_bo_mem_compat);

int ttm_bo_validate(struct ttm_buffer_object *bo,
		    struct ttm_placement *placement,
		    struct ttm_operation_ctx *ctx)
{
	int ret;
	uint32_t new_flags;

	dma_resv_assert_held(bo->base.resv);

	/*
	 * Remove the backing store if no placement is given.
	 */
	if (!placement->num_placement && !placement->num_busy_placement) {
		ret = ttm_bo_pipeline_gutting(bo);
		if (ret)
			return ret;

		return ttm_tt_create(bo, false);
	}

	/*
	 * Check whether we need to move buffer.
	 */
	if (!ttm_bo_mem_compat(placement, &bo->mem, &new_flags)) {
		ret = ttm_bo_move_buffer(bo, placement, ctx);
		if (ret)
			return ret;
	} else {
		/*
		 * Use the access and other non-mapping-related flag bits from
		 * the compatible memory placement flags to the active flags
		 */
		ttm_flag_masked(&bo->mem.placement, new_flags,
				~TTM_PL_MASK_MEMTYPE);
	}
	/*
	 * We might need to add a TTM.
	 */
	if (bo->mem.mem_type == TTM_PL_SYSTEM && bo->ttm == NULL) {
		ret = ttm_tt_create(bo, true);
		if (ret)
			return ret;
	}
	return 0;
}
EXPORT_SYMBOL(ttm_bo_validate);

int ttm_bo_init_reserved(struct ttm_bo_device *bdev,
			 struct ttm_buffer_object *bo,
			 unsigned long size,
			 enum ttm_bo_type type,
			 struct ttm_placement *placement,
			 uint32_t page_alignment,
			 struct ttm_operation_ctx *ctx,
			 size_t acc_size,
			 struct sg_table *sg,
			 struct dma_resv *resv,
			 void (*destroy) (struct ttm_buffer_object *))
{
	struct ttm_mem_global *mem_glob = &ttm_mem_glob;
	int ret = 0;
	unsigned long num_pages;
	bool locked;

	ret = ttm_mem_global_alloc(mem_glob, acc_size, ctx);
	if (ret) {
		pr_err("Out of kernel memory\n");
		if (destroy)
			(*destroy)(bo);
		else
			kfree(bo);
		return -ENOMEM;
	}

	num_pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (num_pages == 0) {
		pr_err("Illegal buffer object size\n");
		if (destroy)
			(*destroy)(bo);
		else
			kfree(bo);
		ttm_mem_global_free(mem_glob, acc_size);
		return -EINVAL;
	}
	bo->destroy = destroy ? destroy : ttm_bo_default_destroy;

	kref_init(&bo->kref);
	INIT_LIST_HEAD(&bo->lru);
	INIT_LIST_HEAD(&bo->ddestroy);
	INIT_LIST_HEAD(&bo->swap);
	INIT_LIST_HEAD(&bo->io_reserve_lru);
	bo->bdev = bdev;
	bo->type = type;
	bo->num_pages = num_pages;
	bo->mem.size = num_pages << PAGE_SHIFT;
	bo->mem.mem_type = TTM_PL_SYSTEM;
	bo->mem.num_pages = bo->num_pages;
	bo->mem.mm_node = NULL;
	bo->mem.page_alignment = page_alignment;
	bo->mem.bus.io_reserved_vm = false;
	bo->mem.bus.io_reserved_count = 0;
	bo->moving = NULL;
	bo->mem.placement = (TTM_PL_FLAG_SYSTEM | TTM_PL_FLAG_CACHED);
	bo->acc_size = acc_size;
	bo->sg = sg;
	if (resv) {
		bo->base.resv = resv;
		dma_resv_assert_held(bo->base.resv);
	} else {
		bo->base.resv = &bo->base._resv;
	}
	if (!ttm_bo_uses_embedded_gem_object(bo)) {
		/*
		 * bo.gem is not initialized, so we have to setup the
		 * struct elements we want use regardless.
		 */
		dma_resv_init(&bo->base._resv);
		drm_vma_node_reset(&bo->base.vma_node);
	}
	atomic_inc(&ttm_bo_glob.bo_count);

	/*
	 * For ttm_bo_type_device buffers, allocate
	 * address space from the device.
	 */
	if (bo->type == ttm_bo_type_device ||
	    bo->type == ttm_bo_type_sg)
		ret = drm_vma_offset_add(bdev->vma_manager, &bo->base.vma_node,
					 bo->mem.num_pages);

	/* passed reservation objects should already be locked,
	 * since otherwise lockdep will be angered in radeon.
	 */
	if (!resv) {
		locked = dma_resv_trylock(bo->base.resv);
		WARN_ON(!locked);
	}

	if (likely(!ret))
		ret = ttm_bo_validate(bo, placement, ctx);

	if (unlikely(ret)) {
		if (!resv)
			ttm_bo_unreserve(bo);

		ttm_bo_put(bo);
		return ret;
	}

	ttm_bo_move_to_lru_tail_unlocked(bo);

	return ret;
}
EXPORT_SYMBOL(ttm_bo_init_reserved);

int ttm_bo_init(struct ttm_bo_device *bdev,
		struct ttm_buffer_object *bo,
		unsigned long size,
		enum ttm_bo_type type,
		struct ttm_placement *placement,
		uint32_t page_alignment,
		bool interruptible,
		size_t acc_size,
		struct sg_table *sg,
		struct dma_resv *resv,
		void (*destroy) (struct ttm_buffer_object *))
{
	struct ttm_operation_ctx ctx = { interruptible, false };
	int ret;

	ret = ttm_bo_init_reserved(bdev, bo, size, type, placement,
				   page_alignment, &ctx, acc_size,
				   sg, resv, destroy);
	if (ret)
		return ret;

	if (!resv)
		ttm_bo_unreserve(bo);

	return 0;
}
EXPORT_SYMBOL(ttm_bo_init);

size_t ttm_bo_acc_size(struct ttm_bo_device *bdev,
		       unsigned long bo_size,
		       unsigned struct_size)
{
	unsigned npages = (PAGE_ALIGN(bo_size)) >> PAGE_SHIFT;
	size_t size = 0;

	size += ttm_round_pot(struct_size);
	size += ttm_round_pot(npages * sizeof(void *));
	size += ttm_round_pot(sizeof(struct ttm_tt));
	return size;
}
EXPORT_SYMBOL(ttm_bo_acc_size);

size_t ttm_bo_dma_acc_size(struct ttm_bo_device *bdev,
			   unsigned long bo_size,
			   unsigned struct_size)
{
	unsigned npages = (PAGE_ALIGN(bo_size)) >> PAGE_SHIFT;
	size_t size = 0;

	size += ttm_round_pot(struct_size);
	size += ttm_round_pot(npages * (2*sizeof(void *) + sizeof(dma_addr_t)));
	size += ttm_round_pot(sizeof(struct ttm_dma_tt));
	return size;
}
EXPORT_SYMBOL(ttm_bo_dma_acc_size);

int ttm_bo_create(struct ttm_bo_device *bdev,
			unsigned long size,
			enum ttm_bo_type type,
			struct ttm_placement *placement,
			uint32_t page_alignment,
			bool interruptible,
			struct ttm_buffer_object **p_bo)
{
	struct ttm_buffer_object *bo;
	size_t acc_size;
	int ret;

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (unlikely(bo == NULL))
		return -ENOMEM;

	acc_size = ttm_bo_acc_size(bdev, size, sizeof(struct ttm_buffer_object));
	ret = ttm_bo_init(bdev, bo, size, type, placement, page_alignment,
			  interruptible, acc_size,
			  NULL, NULL, NULL);
	if (likely(ret == 0))
		*p_bo = bo;

	return ret;
}
EXPORT_SYMBOL(ttm_bo_create);

int ttm_mem_type_manager_force_list_clean(struct ttm_bo_device *bdev,
					  struct ttm_mem_type_manager *man)
{
	struct ttm_operation_ctx ctx = {
		.interruptible = false,
		.no_wait_gpu = false,
		.flags = TTM_OPT_FLAG_FORCE_ALLOC
	};
	struct ttm_bo_global *glob = &ttm_bo_glob;
	struct dma_fence *fence;
	int ret;
	unsigned i;

	/*
	 * Can't use standard list traversal since we're unlocking.
	 */

	spin_lock(&glob->lru_lock);
	for (i = 0; i < TTM_MAX_BO_PRIORITY; ++i) {
		while (!list_empty(&man->lru[i])) {
			spin_unlock(&glob->lru_lock);
			ret = ttm_mem_evict_first(bdev, man, NULL, &ctx,
						  NULL);
			if (ret)
				return ret;
			spin_lock(&glob->lru_lock);
		}
	}
	spin_unlock(&glob->lru_lock);

	spin_lock(&man->move_lock);
	fence = dma_fence_get(man->move);
	spin_unlock(&man->move_lock);

	if (fence) {
		ret = dma_fence_wait(fence, false);
		dma_fence_put(fence);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL(ttm_mem_type_manager_force_list_clean);


int ttm_bo_evict_mm(struct ttm_bo_device *bdev, unsigned mem_type)
{
	struct ttm_mem_type_manager *man = ttm_manager_type(bdev, mem_type);

	if (mem_type == 0 || mem_type >= TTM_NUM_MEM_TYPES) {
		pr_err("Illegal memory manager memory type %u\n", mem_type);
		return -EINVAL;
	}

	if (!man->has_type) {
		pr_err("Memory type %u has not been initialized\n", mem_type);
		return 0;
	}

	return ttm_mem_type_manager_force_list_clean(bdev, man);
}
EXPORT_SYMBOL(ttm_bo_evict_mm);

void ttm_mem_type_manager_init(struct ttm_mem_type_manager *man,
			       unsigned long p_size)
{
	unsigned i;

	BUG_ON(man->has_type);
	man->use_io_reserve_lru = false;
	mutex_init(&man->io_reserve_mutex);
	spin_lock_init(&man->move_lock);
	INIT_LIST_HEAD(&man->io_reserve_lru);
	man->size = p_size;

	for (i = 0; i < TTM_MAX_BO_PRIORITY; ++i)
		INIT_LIST_HEAD(&man->lru[i]);
	man->move = NULL;
}
EXPORT_SYMBOL(ttm_mem_type_manager_init);

static void ttm_bo_global_kobj_release(struct kobject *kobj)
{
	struct ttm_bo_global *glob =
		container_of(kobj, struct ttm_bo_global, kobj);

	__free_page(glob->dummy_read_page);
}

static void ttm_bo_global_release(void)
{
	struct ttm_bo_global *glob = &ttm_bo_glob;

	mutex_lock(&ttm_global_mutex);
	if (--ttm_bo_glob_use_count > 0)
		goto out;

	kobject_del(&glob->kobj);
	kobject_put(&glob->kobj);
	ttm_mem_global_release(&ttm_mem_glob);
	memset(glob, 0, sizeof(*glob));
out:
	mutex_unlock(&ttm_global_mutex);
}

static int ttm_bo_global_init(void)
{
	struct ttm_bo_global *glob = &ttm_bo_glob;
	int ret = 0;
	unsigned i;

	mutex_lock(&ttm_global_mutex);
	if (++ttm_bo_glob_use_count > 1)
		goto out;

	ret = ttm_mem_global_init(&ttm_mem_glob);
	if (ret)
		goto out;

	spin_lock_init(&glob->lru_lock);
	glob->dummy_read_page = alloc_page(__GFP_ZERO | GFP_DMA32);

	if (unlikely(glob->dummy_read_page == NULL)) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < TTM_MAX_BO_PRIORITY; ++i)
		INIT_LIST_HEAD(&glob->swap_lru[i]);
	INIT_LIST_HEAD(&glob->device_list);
	atomic_set(&glob->bo_count, 0);

	ret = kobject_init_and_add(
		&glob->kobj, &ttm_bo_glob_kobj_type, ttm_get_kobj(), "buffer_objects");
	if (unlikely(ret != 0))
		kobject_put(&glob->kobj);
out:
	mutex_unlock(&ttm_global_mutex);
	return ret;
}

int ttm_bo_device_release(struct ttm_bo_device *bdev)
{
	struct ttm_bo_global *glob = &ttm_bo_glob;
	int ret = 0;
	unsigned i;
	struct ttm_mem_type_manager *man;

	man = ttm_manager_type(bdev, TTM_PL_SYSTEM);
	ttm_mem_type_manager_disable(man);

	mutex_lock(&ttm_global_mutex);
	list_del(&bdev->device_list);
	mutex_unlock(&ttm_global_mutex);

	cancel_delayed_work_sync(&bdev->wq);

	if (ttm_bo_delayed_delete(bdev, true))
		pr_debug("Delayed destroy list was clean\n");

	spin_lock(&glob->lru_lock);
	for (i = 0; i < TTM_MAX_BO_PRIORITY; ++i)
		if (list_empty(&man->lru[0]))
			pr_debug("Swap list %d was clean\n", i);
	spin_unlock(&glob->lru_lock);

	if (!ret)
		ttm_bo_global_release();

	return ret;
}
EXPORT_SYMBOL(ttm_bo_device_release);

static void ttm_bo_init_sysman(struct ttm_bo_device *bdev)
{
	struct ttm_mem_type_manager *man = ttm_manager_type(bdev, TTM_PL_SYSTEM);

	/*
	 * Initialize the system memory buffer type.
	 * Other types need to be driver / IOCTL initialized.
	 */
	man->use_tt = true;
	man->available_caching = TTM_PL_MASK_CACHING;
	man->default_caching = TTM_PL_FLAG_CACHED;

	ttm_mem_type_manager_init(man, 0);
	ttm_mem_type_manager_set_used(man, true);
}

int ttm_bo_device_init(struct ttm_bo_device *bdev,
		       struct ttm_bo_driver *driver,
#ifdef __linux__
		       struct address_space *mapping,
#elif defined(__FreeBSD__)
		       void *dummy,
#endif
		       struct drm_vma_offset_manager *vma_manager,
		       bool need_dma32)
{
	struct ttm_bo_global *glob = &ttm_bo_glob;
	int ret;

	if (WARN_ON(vma_manager == NULL))
		return -EINVAL;

	ret = ttm_bo_global_init();
	if (ret)
		return ret;

	bdev->driver = driver;

	memset(bdev->man_priv, 0, sizeof(bdev->man_priv));

	ttm_bo_init_sysman(bdev);

	bdev->vma_manager = vma_manager;
	INIT_DELAYED_WORK(&bdev->wq, ttm_bo_delayed_workqueue);
	INIT_LIST_HEAD(&bdev->ddestroy);
#ifdef __linux__
	bdev->dev_mapping = mapping;
#endif
	bdev->need_dma32 = need_dma32;
	mutex_lock(&ttm_global_mutex);
	list_add_tail(&bdev->device_list, &glob->device_list);
	mutex_unlock(&ttm_global_mutex);

	return 0;
}
EXPORT_SYMBOL(ttm_bo_device_init);

/*
 * buffer object vm functions.
 */

void ttm_bo_unmap_virtual_locked(struct ttm_buffer_object *bo)
{
#ifdef __linux__
	struct ttm_bo_device *bdev = bo->bdev;

	drm_vma_node_unmap(&bo->base.vma_node, bdev->dev_mapping);
#elif defined(__FreeBSD__)
	drm_vma_node_unmap(&bo->base.vma_node, bo);
#endif
	ttm_mem_io_free_vm(bo);
}

void ttm_bo_unmap_virtual(struct ttm_buffer_object *bo)
{
	struct ttm_bo_device *bdev = bo->bdev;
	struct ttm_mem_type_manager *man = ttm_manager_type(bdev, bo->mem.mem_type);

	ttm_mem_io_lock(man, false);
	ttm_bo_unmap_virtual_locked(bo);
	ttm_mem_io_unlock(man);
}


EXPORT_SYMBOL(ttm_bo_unmap_virtual);

int ttm_bo_wait(struct ttm_buffer_object *bo,
		bool interruptible, bool no_wait)
{
	long timeout = 15 * HZ;

	if (no_wait) {
		if (dma_resv_test_signaled_rcu(bo->base.resv, true))
			return 0;
		else
			return -EBUSY;
	}

	timeout = dma_resv_wait_timeout_rcu(bo->base.resv, true,
						      interruptible, timeout);
	if (timeout < 0)
		return timeout;

	if (timeout == 0)
		return -EBUSY;

	dma_resv_add_excl_fence(bo->base.resv, NULL);
	return 0;
}
EXPORT_SYMBOL(ttm_bo_wait);

/**
 * A buffer object shrink method that tries to swap out the first
 * buffer object on the bo_global::swap_lru list.
 */
int ttm_bo_swapout(struct ttm_bo_global *glob, struct ttm_operation_ctx *ctx)
{
	struct ttm_buffer_object *bo;
	int ret = -EBUSY;
	bool locked;
	unsigned i;

	spin_lock(&glob->lru_lock);
	for (i = 0; i < TTM_MAX_BO_PRIORITY; ++i) {
		list_for_each_entry(bo, &glob->swap_lru[i], swap) {
			if (!ttm_bo_evict_swapout_allowable(bo, ctx, &locked,
							    NULL))
				continue;

			if (!ttm_bo_get_unless_zero(bo)) {
				if (locked)
					dma_resv_unlock(bo->base.resv);
				continue;
			}

			ret = 0;
			break;
		}
		if (!ret)
			break;
	}

	if (ret) {
		spin_unlock(&glob->lru_lock);
		return ret;
	}

	if (bo->deleted) {
		ret = ttm_bo_cleanup_refs(bo, false, false, locked);
		ttm_bo_put(bo);
		return ret;
	}

	ttm_bo_del_from_lru(bo);
	spin_unlock(&glob->lru_lock);

	/**
	 * Move to system cached
	 */

	if (bo->mem.mem_type != TTM_PL_SYSTEM ||
	    bo->ttm->caching_state != tt_cached) {
		struct ttm_operation_ctx ctx = { false, false };
		struct ttm_mem_reg evict_mem;

		evict_mem = bo->mem;
		evict_mem.mm_node = NULL;
		evict_mem.placement = TTM_PL_FLAG_SYSTEM | TTM_PL_FLAG_CACHED;
		evict_mem.mem_type = TTM_PL_SYSTEM;

		ret = ttm_bo_handle_move_mem(bo, &evict_mem, true, &ctx);
		if (unlikely(ret != 0))
			goto out;
	}

	/**
	 * Make sure BO is idle.
	 */

	ret = ttm_bo_wait(bo, false, false);
	if (unlikely(ret != 0))
		goto out;

	ttm_bo_unmap_virtual(bo);

	/**
	 * Swap out. Buffer will be swapped in again as soon as
	 * anyone tries to access a ttm page.
	 */

	if (bo->bdev->driver->swap_notify)
		bo->bdev->driver->swap_notify(bo);

	ret = ttm_tt_swapout(bo->ttm, bo->persistent_swap_storage);
out:

	/**
	 *
	 * Unreserve without putting on LRU to avoid swapping out an
	 * already swapped buffer.
	 */
	if (locked)
		dma_resv_unlock(bo->base.resv);
	ttm_bo_put(bo);
	return ret;
}
EXPORT_SYMBOL(ttm_bo_swapout);

void ttm_bo_swapout_all(void)
{
	struct ttm_operation_ctx ctx = {
		.interruptible = false,
		.no_wait_gpu = false
	};

	while (ttm_bo_swapout(&ttm_bo_glob, &ctx) == 0);
}
EXPORT_SYMBOL(ttm_bo_swapout_all);
