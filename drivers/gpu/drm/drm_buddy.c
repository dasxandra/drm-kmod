// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <linux/kmemleak.h>
#include <linux/module.h>
#include <linux/sizes.h>

#include <drm/drm_buddy.h>

static struct kmem_cache *slab_blocks;

static struct drm_buddy_block *drm_block_alloc(struct drm_buddy *mm,
					       struct drm_buddy_block *parent,
					       unsigned int order,
					       u64 offset)
{
	struct drm_buddy_block *block;

	BUG_ON(order > DRM_BUDDY_MAX_ORDER);

	block = kmem_cache_zalloc(slab_blocks, GFP_KERNEL);
	if (!block)
		return NULL;

	block->header = offset;
	block->header |= order;
	block->parent = parent;

	BUG_ON(block->header & DRM_BUDDY_HEADER_UNUSED);
	return block;
}

static void drm_block_free(struct drm_buddy *mm,
			   struct drm_buddy_block *block)
{
	kmem_cache_free(slab_blocks, block);
}

static void mark_allocated(struct drm_buddy_block *block)
{
	block->header &= ~DRM_BUDDY_HEADER_STATE;
	block->header |= DRM_BUDDY_ALLOCATED;

	list_del(&block->link);
}

static void mark_free(struct drm_buddy *mm,
		      struct drm_buddy_block *block)
{
	block->header &= ~DRM_BUDDY_HEADER_STATE;
	block->header |= DRM_BUDDY_FREE;

	list_add(&block->link,
		 &mm->free_list[drm_buddy_block_order(block)]);
}

static void mark_split(struct drm_buddy_block *block)
{
	block->header &= ~DRM_BUDDY_HEADER_STATE;
	block->header |= DRM_BUDDY_SPLIT;

	list_del(&block->link);
}

/**
 * drm_buddy_init - init memory manager
 *
 * @mm: DRM buddy manager to initialize
 * @size: size in bytes to manage
 * @chunk_size: minimum page size in bytes for our allocations
 *
 * Initializes the memory manager and its resources.
 *
 * Returns:
 * 0 on success, error code on failure.
 */
int drm_buddy_init(struct drm_buddy *mm, u64 size, u64 chunk_size)
{
	unsigned int i;
	u64 offset;

	if (size < chunk_size)
		return -EINVAL;

	if (chunk_size < PAGE_SIZE)
		return -EINVAL;

	if (!is_power_of_2(chunk_size))
		return -EINVAL;

	size = round_down(size, chunk_size);

	mm->size = size;
	mm->avail = size;
	mm->chunk_size = chunk_size;
	mm->max_order = ilog2(size) - ilog2(chunk_size);

	BUG_ON(mm->max_order > DRM_BUDDY_MAX_ORDER);

	mm->free_list = kmalloc_array(mm->max_order + 1,
				      sizeof(struct list_head),
				      GFP_KERNEL);
	if (!mm->free_list)
		return -ENOMEM;

	for (i = 0; i <= mm->max_order; ++i)
		INIT_LIST_HEAD(&mm->free_list[i]);

	mm->n_roots = hweight64(size);

	mm->roots = kmalloc_array(mm->n_roots,
				  sizeof(struct drm_buddy_block *),
				  GFP_KERNEL);
	if (!mm->roots)
		goto out_free_list;

	offset = 0;
	i = 0;

	/*
	 * Split into power-of-two blocks, in case we are given a size that is
	 * not itself a power-of-two.
	 */
	do {
		struct drm_buddy_block *root;
		unsigned int order;
		u64 root_size;

		root_size = rounddown_pow_of_two(size);
		order = ilog2(root_size) - ilog2(chunk_size);

		root = drm_block_alloc(mm, NULL, order, offset);
		if (!root)
			goto out_free_roots;

		mark_free(mm, root);

		BUG_ON(i > mm->max_order);
		BUG_ON(drm_buddy_block_size(mm, root) < chunk_size);

		mm->roots[i] = root;

		offset += root_size;
		size -= root_size;
		i++;
	} while (size);

	return 0;

out_free_roots:
	while (i--)
		drm_block_free(mm, mm->roots[i]);
	kfree(mm->roots);
out_free_list:
	kfree(mm->free_list);
	return -ENOMEM;
}
EXPORT_SYMBOL(drm_buddy_init);

/**
 * drm_buddy_fini - tear down the memory manager
 *
 * @mm: DRM buddy manager to free
 *
 * Cleanup memory manager resources and the freelist
 */
void drm_buddy_fini(struct drm_buddy *mm)
{
	int i;

	for (i = 0; i < mm->n_roots; ++i) {
		WARN_ON(!drm_buddy_block_is_free(mm->roots[i]));
		drm_block_free(mm, mm->roots[i]);
	}

	WARN_ON(mm->avail != mm->size);

	kfree(mm->roots);
	kfree(mm->free_list);
}
EXPORT_SYMBOL(drm_buddy_fini);

static int split_block(struct drm_buddy *mm,
		       struct drm_buddy_block *block)
{
	unsigned int block_order = drm_buddy_block_order(block) - 1;
	u64 offset = drm_buddy_block_offset(block);

	BUG_ON(!drm_buddy_block_is_free(block));
	BUG_ON(!drm_buddy_block_order(block));

	block->left = drm_block_alloc(mm, block, block_order, offset);
	if (!block->left)
		return -ENOMEM;

	block->right = drm_block_alloc(mm, block, block_order,
				       offset + (mm->chunk_size << block_order));
	if (!block->right) {
		drm_block_free(mm, block->left);
		return -ENOMEM;
	}

	mark_free(mm, block->left);
	mark_free(mm, block->right);

	mark_split(block);

	return 0;
}

static struct drm_buddy_block *
get_buddy(struct drm_buddy_block *block)
{
	struct drm_buddy_block *parent;

	parent = block->parent;
	if (!parent)
		return NULL;

	if (parent->left == block)
		return parent->right;

	return parent->left;
}

static void __drm_buddy_free(struct drm_buddy *mm,
			     struct drm_buddy_block *block)
{
	struct drm_buddy_block *parent;

	while ((parent = block->parent)) {
		struct drm_buddy_block *buddy;

		buddy = get_buddy(block);

		if (!drm_buddy_block_is_free(buddy))
			break;

		list_del(&buddy->link);

		drm_block_free(mm, block);
		drm_block_free(mm, buddy);

		block = parent;
	}

	mark_free(mm, block);
}

/**
 * drm_buddy_free_block - free a block
 *
 * @mm: DRM buddy manager
 * @block: block to be freed
 */
void drm_buddy_free_block(struct drm_buddy *mm,
			  struct drm_buddy_block *block)
{
	BUG_ON(!drm_buddy_block_is_allocated(block));
	mm->avail += drm_buddy_block_size(mm, block);
	__drm_buddy_free(mm, block);
}
EXPORT_SYMBOL(drm_buddy_free_block);

/**
 * drm_buddy_free_list - free blocks
 *
 * @mm: DRM buddy manager
 * @objects: input list head to free blocks
 */
void drm_buddy_free_list(struct drm_buddy *mm, struct list_head *objects)
{
	struct drm_buddy_block *block, *on;

	list_for_each_entry_safe(block, on, objects, link) {
		drm_buddy_free_block(mm, block);
		cond_resched();
	}
	INIT_LIST_HEAD(objects);
}
EXPORT_SYMBOL(drm_buddy_free_list);

/**
 * drm_buddy_alloc_blocks - allocate power-of-two blocks
 *
 * @mm: DRM buddy manager to allocate from
 * @order: size of the allocation
 *
 * The order value here translates to:
 *
 * 0 = 2^0 * mm->chunk_size
 * 1 = 2^1 * mm->chunk_size
 * 2 = 2^2 * mm->chunk_size
 *
 * Returns:
 * allocated ptr to the &drm_buddy_block on success
 */
struct drm_buddy_block *
drm_buddy_alloc_blocks(struct drm_buddy *mm, unsigned int order)
{
	struct drm_buddy_block *block = NULL;
	unsigned int i;
	int err;

	for (i = order; i <= mm->max_order; ++i) {
		block = list_first_entry_or_null(&mm->free_list[i],
						 struct drm_buddy_block,
						 link);
		if (block)
			break;
	}

	if (!block)
		return ERR_PTR(-ENOSPC);

	BUG_ON(!drm_buddy_block_is_free(block));

	while (i != order) {
		err = split_block(mm, block);
		if (unlikely(err))
			goto out_free;

		/* Go low */
		block = block->left;
		i--;
	}

	mark_allocated(block);
	mm->avail -= drm_buddy_block_size(mm, block);
	kmemleak_update_trace(block);
	return block;

out_free:
	if (i != order)
		__drm_buddy_free(mm, block);
	return ERR_PTR(err);
}
EXPORT_SYMBOL(drm_buddy_alloc_blocks);

static inline bool overlaps(u64 s1, u64 e1, u64 s2, u64 e2)
{
	return s1 <= e2 && e1 >= s2;
}

static inline bool contains(u64 s1, u64 e1, u64 s2, u64 e2)
{
	return s1 <= s2 && e1 >= e2;
}

/**
 * drm_buddy_alloc_range - allocate range
 *
 * @mm: DRM buddy manager to allocate from
 * @blocks: output list head to add allocated blocks
 * @start: start of the allowed range for this block
 * @size: size of the allocation
 *
 * Intended for pre-allocating portions of the address space, for example to
 * reserve a block for the initial framebuffer or similar, hence the expectation
 * here is that drm_buddy_alloc_blocks() is still the main vehicle for
 * allocations, so if that's not the case then the drm_mm range allocator is
 * probably a much better fit, and so you should probably go use that instead.
 *
 * Note that it's safe to chain together multiple alloc_ranges
 * with the same blocks list
 *
 * Returns:
 * 0 on success, error code on failure.
 */
int drm_buddy_alloc_range(struct drm_buddy *mm,
			  struct list_head *blocks,
			  u64 start, u64 size)
{
	struct drm_buddy_block *block;
	struct drm_buddy_block *buddy;
	LIST_HEAD(allocated);
	LIST_HEAD(dfs);
	u64 end;
	int err;
	int i;

	if (size < mm->chunk_size)
		return -EINVAL;

	if (!IS_ALIGNED(size | start, mm->chunk_size))
		return -EINVAL;

	if (range_overflows(start, size, mm->size))
		return -EINVAL;

	for (i = 0; i < mm->n_roots; ++i)
		list_add_tail(&mm->roots[i]->tmp_link, &dfs);

	end = start + size - 1;

	do {
		u64 block_start;
		u64 block_end;

		block = list_first_entry_or_null(&dfs,
						 struct drm_buddy_block,
						 tmp_link);
		if (!block)
			break;

		list_del(&block->tmp_link);

		block_start = drm_buddy_block_offset(block);
		block_end = block_start + drm_buddy_block_size(mm, block) - 1;

		if (!overlaps(start, end, block_start, block_end))
			continue;

		if (drm_buddy_block_is_allocated(block)) {
			err = -ENOSPC;
			goto err_free;
		}

		if (contains(start, end, block_start, block_end)) {
			if (!drm_buddy_block_is_free(block)) {
				err = -ENOSPC;
				goto err_free;
			}

			mark_allocated(block);
			mm->avail -= drm_buddy_block_size(mm, block);
			list_add_tail(&block->link, &allocated);
			continue;
		}

		if (!drm_buddy_block_is_split(block)) {
			err = split_block(mm, block);
			if (unlikely(err))
				goto err_undo;
		}

		list_add(&block->right->tmp_link, &dfs);
		list_add(&block->left->tmp_link, &dfs);
	} while (1);

	list_splice_tail(&allocated, blocks);
	return 0;

err_undo:
	/*
	 * We really don't want to leave around a bunch of split blocks, since
	 * bigger is better, so make sure we merge everything back before we
	 * free the allocated blocks.
	 */
	buddy = get_buddy(block);
	if (buddy &&
	    (drm_buddy_block_is_free(block) &&
	     drm_buddy_block_is_free(buddy)))
		__drm_buddy_free(mm, block);

err_free:
	drm_buddy_free_list(mm, &allocated);
	return err;
}
EXPORT_SYMBOL(drm_buddy_alloc_range);

/**
 * drm_buddy_block_print - print block information
 *
 * @mm: DRM buddy manager
 * @block: DRM buddy block
 * @p: DRM printer to use
 */
void drm_buddy_block_print(struct drm_buddy *mm,
			   struct drm_buddy_block *block,
			   struct drm_printer *p)
{
	u64 start = drm_buddy_block_offset(block);
	u64 size = drm_buddy_block_size(mm, block);

	drm_printf(p, "%#018llx-%#018llx: %llu\n", start, start + size, size);
}
EXPORT_SYMBOL(drm_buddy_block_print);

/**
 * drm_buddy_print - print allocator state
 *
 * @mm: DRM buddy manager
 * @p: DRM printer to use
 */
void drm_buddy_print(struct drm_buddy *mm, struct drm_printer *p)
{
	int order;

	drm_printf(p, "chunk_size: %lluKiB, total: %lluMiB, free: %lluMiB\n",
		   mm->chunk_size >> 10, mm->size >> 20, mm->avail >> 20);

	for (order = mm->max_order; order >= 0; order--) {
		struct drm_buddy_block *block;
		u64 count = 0, free;

		list_for_each_entry(block, &mm->free_list[order], link) {
			BUG_ON(!drm_buddy_block_is_free(block));
			count++;
		}

		drm_printf(p, "order-%d ", order);

		free = count * (mm->chunk_size << order);
		if (free < SZ_1M)
			drm_printf(p, "free: %lluKiB", free >> 10);
		else
			drm_printf(p, "free: %lluMiB", free >> 20);

		drm_printf(p, ", pages: %llu\n", count);
	}
}
EXPORT_SYMBOL(drm_buddy_print);

static void drm_buddy_module_exit(void)
{
	kmem_cache_destroy(slab_blocks);
}

static int __init drm_buddy_module_init(void)
{
	slab_blocks = KMEM_CACHE(drm_buddy_block, 0);
	if (!slab_blocks)
		return -ENOMEM;

	return 0;
}

module_init(drm_buddy_module_init);
module_exit(drm_buddy_module_exit);

MODULE_DESCRIPTION("DRM Buddy Allocator");
MODULE_LICENSE("Dual MIT/GPL");
