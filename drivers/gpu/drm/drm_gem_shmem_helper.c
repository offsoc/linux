// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2018 Noralf Trønnes
 */

#include <linux/dma-buf.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#ifdef CONFIG_X86
#include <asm/set_memory.h>
#endif

#include <drm/drm.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_prime.h>
#include <drm/drm_print.h>

MODULE_IMPORT_NS("DMA_BUF");

/**
 * DOC: overview
 *
 * This library provides helpers for GEM objects backed by shmem buffers
 * allocated using anonymous pageable memory.
 *
 * Functions that operate on the GEM object receive struct &drm_gem_shmem_object.
 * For GEM callback helpers in struct &drm_gem_object functions, see likewise
 * named functions with an _object_ infix (e.g., drm_gem_shmem_object_vmap() wraps
 * drm_gem_shmem_vmap()). These helpers perform the necessary type conversion.
 */

static const struct drm_gem_object_funcs drm_gem_shmem_funcs = {
	.free = drm_gem_shmem_object_free,
	.print_info = drm_gem_shmem_object_print_info,
	.pin = drm_gem_shmem_object_pin,
	.unpin = drm_gem_shmem_object_unpin,
	.get_sg_table = drm_gem_shmem_object_get_sg_table,
	.vmap = drm_gem_shmem_object_vmap,
	.vunmap = drm_gem_shmem_object_vunmap,
	.mmap = drm_gem_shmem_object_mmap,
	.vm_ops = &drm_gem_shmem_vm_ops,
};

static struct drm_gem_shmem_object *
__drm_gem_shmem_create(struct drm_device *dev, size_t size, bool private,
		       struct vfsmount *gemfs)
{
	struct drm_gem_shmem_object *shmem;
	struct drm_gem_object *obj;
	int ret = 0;

	size = PAGE_ALIGN(size);

	if (dev->driver->gem_create_object) {
		obj = dev->driver->gem_create_object(dev, size);
		if (IS_ERR(obj))
			return ERR_CAST(obj);
		shmem = to_drm_gem_shmem_obj(obj);
	} else {
		shmem = kzalloc(sizeof(*shmem), GFP_KERNEL);
		if (!shmem)
			return ERR_PTR(-ENOMEM);
		obj = &shmem->base;
	}

	if (!obj->funcs)
		obj->funcs = &drm_gem_shmem_funcs;

	if (private) {
		drm_gem_private_object_init(dev, obj, size);
		shmem->map_wc = false; /* dma-buf mappings use always writecombine */
	} else {
		ret = drm_gem_object_init_with_mnt(dev, obj, size, gemfs);
	}
	if (ret) {
		drm_gem_private_object_fini(obj);
		goto err_free;
	}

	ret = drm_gem_create_mmap_offset(obj);
	if (ret)
		goto err_release;

	INIT_LIST_HEAD(&shmem->madv_list);

	if (!private) {
		/*
		 * Our buffers are kept pinned, so allocating them
		 * from the MOVABLE zone is a really bad idea, and
		 * conflicts with CMA. See comments above new_inode()
		 * why this is required _and_ expected if you're
		 * going to pin these pages.
		 */
		mapping_set_gfp_mask(obj->filp->f_mapping, GFP_HIGHUSER |
				     __GFP_RETRY_MAYFAIL | __GFP_NOWARN);
	}

	return shmem;

err_release:
	drm_gem_object_release(obj);
err_free:
	kfree(obj);

	return ERR_PTR(ret);
}
/**
 * drm_gem_shmem_create - Allocate an object with the given size
 * @dev: DRM device
 * @size: Size of the object to allocate
 *
 * This function creates a shmem GEM object.
 *
 * Returns:
 * A struct drm_gem_shmem_object * on success or an ERR_PTR()-encoded negative
 * error code on failure.
 */
struct drm_gem_shmem_object *drm_gem_shmem_create(struct drm_device *dev, size_t size)
{
	return __drm_gem_shmem_create(dev, size, false, NULL);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_create);

/**
 * drm_gem_shmem_create_with_mnt - Allocate an object with the given size in a
 * given mountpoint
 * @dev: DRM device
 * @size: Size of the object to allocate
 * @gemfs: tmpfs mount where the GEM object will be created
 *
 * This function creates a shmem GEM object in a given tmpfs mountpoint.
 *
 * Returns:
 * A struct drm_gem_shmem_object * on success or an ERR_PTR()-encoded negative
 * error code on failure.
 */
struct drm_gem_shmem_object *drm_gem_shmem_create_with_mnt(struct drm_device *dev,
							   size_t size,
							   struct vfsmount *gemfs)
{
	return __drm_gem_shmem_create(dev, size, false, gemfs);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_create_with_mnt);

/**
 * drm_gem_shmem_free - Free resources associated with a shmem GEM object
 * @shmem: shmem GEM object to free
 *
 * This function cleans up the GEM object state and frees the memory used to
 * store the object itself.
 */
void drm_gem_shmem_free(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;

	if (drm_gem_is_imported(obj)) {
		drm_prime_gem_destroy(obj, shmem->sgt);
	} else {
		dma_resv_lock(shmem->base.resv, NULL);

		drm_WARN_ON(obj->dev, refcount_read(&shmem->vmap_use_count));

		if (shmem->sgt) {
			dma_unmap_sgtable(obj->dev->dev, shmem->sgt,
					  DMA_BIDIRECTIONAL, 0);
			sg_free_table(shmem->sgt);
			kfree(shmem->sgt);
		}
		if (shmem->pages)
			drm_gem_shmem_put_pages_locked(shmem);

		drm_WARN_ON(obj->dev, refcount_read(&shmem->pages_use_count));
		drm_WARN_ON(obj->dev, refcount_read(&shmem->pages_pin_count));

		dma_resv_unlock(shmem->base.resv);
	}

	drm_gem_object_release(obj);
	kfree(shmem);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_free);

static int drm_gem_shmem_get_pages_locked(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;
	struct page **pages;

	dma_resv_assert_held(shmem->base.resv);

	if (refcount_inc_not_zero(&shmem->pages_use_count))
		return 0;

	pages = drm_gem_get_pages(obj);
	if (IS_ERR(pages)) {
		drm_dbg_kms(obj->dev, "Failed to get pages (%ld)\n",
			    PTR_ERR(pages));
		return PTR_ERR(pages);
	}

	/*
	 * TODO: Allocating WC pages which are correctly flushed is only
	 * supported on x86. Ideal solution would be a GFP_WC flag, which also
	 * ttm_pool.c could use.
	 */
#ifdef CONFIG_X86
	if (shmem->map_wc)
		set_pages_array_wc(pages, obj->size >> PAGE_SHIFT);
#endif

	shmem->pages = pages;

	refcount_set(&shmem->pages_use_count, 1);

	return 0;
}

/*
 * drm_gem_shmem_put_pages_locked - Decrease use count on the backing pages for a shmem GEM object
 * @shmem: shmem GEM object
 *
 * This function decreases the use count and puts the backing pages when use drops to zero.
 */
void drm_gem_shmem_put_pages_locked(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;

	dma_resv_assert_held(shmem->base.resv);

	if (refcount_dec_and_test(&shmem->pages_use_count)) {
#ifdef CONFIG_X86
		if (shmem->map_wc)
			set_pages_array_wb(shmem->pages, obj->size >> PAGE_SHIFT);
#endif

		drm_gem_put_pages(obj, shmem->pages,
				  shmem->pages_mark_dirty_on_put,
				  shmem->pages_mark_accessed_on_put);
		shmem->pages = NULL;
	}
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_put_pages_locked);

int drm_gem_shmem_pin_locked(struct drm_gem_shmem_object *shmem)
{
	int ret;

	dma_resv_assert_held(shmem->base.resv);

	drm_WARN_ON(shmem->base.dev, drm_gem_is_imported(&shmem->base));

	if (refcount_inc_not_zero(&shmem->pages_pin_count))
		return 0;

	ret = drm_gem_shmem_get_pages_locked(shmem);
	if (!ret)
		refcount_set(&shmem->pages_pin_count, 1);

	return ret;
}
EXPORT_SYMBOL(drm_gem_shmem_pin_locked);

void drm_gem_shmem_unpin_locked(struct drm_gem_shmem_object *shmem)
{
	dma_resv_assert_held(shmem->base.resv);

	if (refcount_dec_and_test(&shmem->pages_pin_count))
		drm_gem_shmem_put_pages_locked(shmem);
}
EXPORT_SYMBOL(drm_gem_shmem_unpin_locked);

/**
 * drm_gem_shmem_pin - Pin backing pages for a shmem GEM object
 * @shmem: shmem GEM object
 *
 * This function makes sure the backing pages are pinned in memory while the
 * buffer is exported.
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_gem_shmem_pin(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;
	int ret;

	drm_WARN_ON(obj->dev, drm_gem_is_imported(obj));

	if (refcount_inc_not_zero(&shmem->pages_pin_count))
		return 0;

	ret = dma_resv_lock_interruptible(shmem->base.resv, NULL);
	if (ret)
		return ret;
	ret = drm_gem_shmem_pin_locked(shmem);
	dma_resv_unlock(shmem->base.resv);

	return ret;
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_pin);

/**
 * drm_gem_shmem_unpin - Unpin backing pages for a shmem GEM object
 * @shmem: shmem GEM object
 *
 * This function removes the requirement that the backing pages are pinned in
 * memory.
 */
void drm_gem_shmem_unpin(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;

	drm_WARN_ON(obj->dev, drm_gem_is_imported(obj));

	if (refcount_dec_not_one(&shmem->pages_pin_count))
		return;

	dma_resv_lock(shmem->base.resv, NULL);
	drm_gem_shmem_unpin_locked(shmem);
	dma_resv_unlock(shmem->base.resv);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_unpin);

/*
 * drm_gem_shmem_vmap_locked - Create a virtual mapping for a shmem GEM object
 * @shmem: shmem GEM object
 * @map: Returns the kernel virtual address of the SHMEM GEM object's backing
 *       store.
 *
 * This function makes sure that a contiguous kernel virtual address mapping
 * exists for the buffer backing the shmem GEM object. It hides the differences
 * between dma-buf imported and natively allocated objects.
 *
 * Acquired mappings should be cleaned up by calling drm_gem_shmem_vunmap_locked().
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_gem_shmem_vmap_locked(struct drm_gem_shmem_object *shmem,
			      struct iosys_map *map)
{
	struct drm_gem_object *obj = &shmem->base;
	int ret = 0;

	dma_resv_assert_held(obj->resv);

	if (drm_gem_is_imported(obj)) {
		ret = dma_buf_vmap(obj->import_attach->dmabuf, map);
	} else {
		pgprot_t prot = PAGE_KERNEL;

		dma_resv_assert_held(shmem->base.resv);

		if (refcount_inc_not_zero(&shmem->vmap_use_count)) {
			iosys_map_set_vaddr(map, shmem->vaddr);
			return 0;
		}

		ret = drm_gem_shmem_pin_locked(shmem);
		if (ret)
			return ret;

		if (shmem->map_wc)
			prot = pgprot_writecombine(prot);
		shmem->vaddr = vmap(shmem->pages, obj->size >> PAGE_SHIFT,
				    VM_MAP, prot);
		if (!shmem->vaddr) {
			ret = -ENOMEM;
		} else {
			iosys_map_set_vaddr(map, shmem->vaddr);
			refcount_set(&shmem->vmap_use_count, 1);
		}
	}

	if (ret) {
		drm_dbg_kms(obj->dev, "Failed to vmap pages, error %d\n", ret);
		goto err_put_pages;
	}

	return 0;

err_put_pages:
	if (!drm_gem_is_imported(obj))
		drm_gem_shmem_unpin_locked(shmem);

	return ret;
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_vmap_locked);

/*
 * drm_gem_shmem_vunmap_locked - Unmap a virtual mapping for a shmem GEM object
 * @shmem: shmem GEM object
 * @map: Kernel virtual address where the SHMEM GEM object was mapped
 *
 * This function cleans up a kernel virtual address mapping acquired by
 * drm_gem_shmem_vmap_locked(). The mapping is only removed when the use count
 * drops to zero.
 *
 * This function hides the differences between dma-buf imported and natively
 * allocated objects.
 */
void drm_gem_shmem_vunmap_locked(struct drm_gem_shmem_object *shmem,
				 struct iosys_map *map)
{
	struct drm_gem_object *obj = &shmem->base;

	dma_resv_assert_held(obj->resv);

	if (drm_gem_is_imported(obj)) {
		dma_buf_vunmap(obj->import_attach->dmabuf, map);
	} else {
		dma_resv_assert_held(shmem->base.resv);

		if (refcount_dec_and_test(&shmem->vmap_use_count)) {
			vunmap(shmem->vaddr);
			shmem->vaddr = NULL;

			drm_gem_shmem_unpin_locked(shmem);
		}
	}
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_vunmap_locked);

static int
drm_gem_shmem_create_with_handle(struct drm_file *file_priv,
				 struct drm_device *dev, size_t size,
				 uint32_t *handle)
{
	struct drm_gem_shmem_object *shmem;
	int ret;

	shmem = drm_gem_shmem_create(dev, size);
	if (IS_ERR(shmem))
		return PTR_ERR(shmem);

	/*
	 * Allocate an id of idr table where the obj is registered
	 * and handle has the id what user can see.
	 */
	ret = drm_gem_handle_create(file_priv, &shmem->base, handle);
	/* drop reference from allocate - handle holds it now. */
	drm_gem_object_put(&shmem->base);

	return ret;
}

/* Update madvise status, returns true if not purged, else
 * false or -errno.
 */
int drm_gem_shmem_madvise_locked(struct drm_gem_shmem_object *shmem, int madv)
{
	dma_resv_assert_held(shmem->base.resv);

	if (shmem->madv >= 0)
		shmem->madv = madv;

	madv = shmem->madv;

	return (madv >= 0);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_madvise_locked);

void drm_gem_shmem_purge_locked(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;
	struct drm_device *dev = obj->dev;

	dma_resv_assert_held(shmem->base.resv);

	drm_WARN_ON(obj->dev, !drm_gem_shmem_is_purgeable(shmem));

	dma_unmap_sgtable(dev->dev, shmem->sgt, DMA_BIDIRECTIONAL, 0);
	sg_free_table(shmem->sgt);
	kfree(shmem->sgt);
	shmem->sgt = NULL;

	drm_gem_shmem_put_pages_locked(shmem);

	shmem->madv = -1;

	drm_vma_node_unmap(&obj->vma_node, dev->anon_inode->i_mapping);
	drm_gem_free_mmap_offset(obj);

	/* Our goal here is to return as much of the memory as
	 * is possible back to the system as we are called from OOM.
	 * To do this we must instruct the shmfs to drop all of its
	 * backing pages, *now*.
	 */
	shmem_truncate_range(file_inode(obj->filp), 0, (loff_t)-1);

	invalidate_mapping_pages(file_inode(obj->filp)->i_mapping, 0, (loff_t)-1);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_purge_locked);

/**
 * drm_gem_shmem_dumb_create - Create a dumb shmem buffer object
 * @file: DRM file structure to create the dumb buffer for
 * @dev: DRM device
 * @args: IOCTL data
 *
 * This function computes the pitch of the dumb buffer and rounds it up to an
 * integer number of bytes per pixel. Drivers for hardware that doesn't have
 * any additional restrictions on the pitch can directly use this function as
 * their &drm_driver.dumb_create callback.
 *
 * For hardware with additional restrictions, drivers can adjust the fields
 * set up by userspace before calling into this function.
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_gem_shmem_dumb_create(struct drm_file *file, struct drm_device *dev,
			      struct drm_mode_create_dumb *args)
{
	u32 min_pitch = DIV_ROUND_UP(args->width * args->bpp, 8);

	if (!args->pitch || !args->size) {
		args->pitch = min_pitch;
		args->size = PAGE_ALIGN((u64)args->pitch * args->height);
	} else {
		/* ensure sane minimum values */
		if (args->pitch < min_pitch)
			args->pitch = min_pitch;
		if (args->size < (u64)args->pitch * args->height)
			args->size = PAGE_ALIGN((u64)args->pitch * args->height);
	}

	return drm_gem_shmem_create_with_handle(file, dev, args->size, &args->handle);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_dumb_create);

static vm_fault_t drm_gem_shmem_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct drm_gem_object *obj = vma->vm_private_data;
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);
	loff_t num_pages = obj->size >> PAGE_SHIFT;
	vm_fault_t ret;
	struct page *page;
	pgoff_t page_offset;

	/* We don't use vmf->pgoff since that has the fake offset */
	page_offset = (vmf->address - vma->vm_start) >> PAGE_SHIFT;

	dma_resv_lock(shmem->base.resv, NULL);

	if (page_offset >= num_pages ||
	    drm_WARN_ON_ONCE(obj->dev, !shmem->pages) ||
	    shmem->madv < 0) {
		ret = VM_FAULT_SIGBUS;
	} else {
		page = shmem->pages[page_offset];

		ret = vmf_insert_pfn(vma, vmf->address, page_to_pfn(page));
	}

	dma_resv_unlock(shmem->base.resv);

	return ret;
}

static void drm_gem_shmem_vm_open(struct vm_area_struct *vma)
{
	struct drm_gem_object *obj = vma->vm_private_data;
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	drm_WARN_ON(obj->dev, drm_gem_is_imported(obj));

	dma_resv_lock(shmem->base.resv, NULL);

	/*
	 * We should have already pinned the pages when the buffer was first
	 * mmap'd, vm_open() just grabs an additional reference for the new
	 * mm the vma is getting copied into (ie. on fork()).
	 */
	drm_WARN_ON_ONCE(obj->dev,
			 !refcount_inc_not_zero(&shmem->pages_use_count));

	dma_resv_unlock(shmem->base.resv);

	drm_gem_vm_open(vma);
}

static void drm_gem_shmem_vm_close(struct vm_area_struct *vma)
{
	struct drm_gem_object *obj = vma->vm_private_data;
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	dma_resv_lock(shmem->base.resv, NULL);
	drm_gem_shmem_put_pages_locked(shmem);
	dma_resv_unlock(shmem->base.resv);

	drm_gem_vm_close(vma);
}

const struct vm_operations_struct drm_gem_shmem_vm_ops = {
	.fault = drm_gem_shmem_fault,
	.open = drm_gem_shmem_vm_open,
	.close = drm_gem_shmem_vm_close,
};
EXPORT_SYMBOL_GPL(drm_gem_shmem_vm_ops);

/**
 * drm_gem_shmem_mmap - Memory-map a shmem GEM object
 * @shmem: shmem GEM object
 * @vma: VMA for the area to be mapped
 *
 * This function implements an augmented version of the GEM DRM file mmap
 * operation for shmem objects.
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_gem_shmem_mmap(struct drm_gem_shmem_object *shmem, struct vm_area_struct *vma)
{
	struct drm_gem_object *obj = &shmem->base;
	int ret;

	if (drm_gem_is_imported(obj)) {
		/* Reset both vm_ops and vm_private_data, so we don't end up with
		 * vm_ops pointing to our implementation if the dma-buf backend
		 * doesn't set those fields.
		 */
		vma->vm_private_data = NULL;
		vma->vm_ops = NULL;

		ret = dma_buf_mmap(obj->dma_buf, vma, 0);

		/* Drop the reference drm_gem_mmap_obj() acquired.*/
		if (!ret)
			drm_gem_object_put(obj);

		return ret;
	}

	if (is_cow_mapping(vma->vm_flags))
		return -EINVAL;

	dma_resv_lock(shmem->base.resv, NULL);
	ret = drm_gem_shmem_get_pages_locked(shmem);
	dma_resv_unlock(shmem->base.resv);

	if (ret)
		return ret;

	vm_flags_set(vma, VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	if (shmem->map_wc)
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	return 0;
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_mmap);

/**
 * drm_gem_shmem_print_info() - Print &drm_gem_shmem_object info for debugfs
 * @shmem: shmem GEM object
 * @p: DRM printer
 * @indent: Tab indentation level
 */
void drm_gem_shmem_print_info(const struct drm_gem_shmem_object *shmem,
			      struct drm_printer *p, unsigned int indent)
{
	if (drm_gem_is_imported(&shmem->base))
		return;

	drm_printf_indent(p, indent, "pages_pin_count=%u\n", refcount_read(&shmem->pages_pin_count));
	drm_printf_indent(p, indent, "pages_use_count=%u\n", refcount_read(&shmem->pages_use_count));
	drm_printf_indent(p, indent, "vmap_use_count=%u\n", refcount_read(&shmem->vmap_use_count));
	drm_printf_indent(p, indent, "vaddr=%p\n", shmem->vaddr);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_print_info);

/**
 * drm_gem_shmem_get_sg_table - Provide a scatter/gather table of pinned
 *                              pages for a shmem GEM object
 * @shmem: shmem GEM object
 *
 * This function exports a scatter/gather table suitable for PRIME usage by
 * calling the standard DMA mapping API.
 *
 * Drivers who need to acquire an scatter/gather table for objects need to call
 * drm_gem_shmem_get_pages_sgt() instead.
 *
 * Returns:
 * A pointer to the scatter/gather table of pinned pages or error pointer on failure.
 */
struct sg_table *drm_gem_shmem_get_sg_table(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;

	drm_WARN_ON(obj->dev, drm_gem_is_imported(obj));

	return drm_prime_pages_to_sg(obj->dev, shmem->pages, obj->size >> PAGE_SHIFT);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_get_sg_table);

static struct sg_table *drm_gem_shmem_get_pages_sgt_locked(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;
	int ret;
	struct sg_table *sgt;

	if (shmem->sgt)
		return shmem->sgt;

	drm_WARN_ON(obj->dev, drm_gem_is_imported(obj));

	ret = drm_gem_shmem_get_pages_locked(shmem);
	if (ret)
		return ERR_PTR(ret);

	sgt = drm_gem_shmem_get_sg_table(shmem);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto err_put_pages;
	}
	/* Map the pages for use by the h/w. */
	ret = dma_map_sgtable(obj->dev->dev, sgt, DMA_BIDIRECTIONAL, 0);
	if (ret)
		goto err_free_sgt;

	shmem->sgt = sgt;

	return sgt;

err_free_sgt:
	sg_free_table(sgt);
	kfree(sgt);
err_put_pages:
	drm_gem_shmem_put_pages_locked(shmem);
	return ERR_PTR(ret);
}

/**
 * drm_gem_shmem_get_pages_sgt - Pin pages, dma map them, and return a
 *				 scatter/gather table for a shmem GEM object.
 * @shmem: shmem GEM object
 *
 * This function returns a scatter/gather table suitable for driver usage. If
 * the sg table doesn't exist, the pages are pinned, dma-mapped, and a sg
 * table created.
 *
 * This is the main function for drivers to get at backing storage, and it hides
 * and difference between dma-buf imported and natively allocated objects.
 * drm_gem_shmem_get_sg_table() should not be directly called by drivers.
 *
 * Returns:
 * A pointer to the scatter/gather table of pinned pages or errno on failure.
 */
struct sg_table *drm_gem_shmem_get_pages_sgt(struct drm_gem_shmem_object *shmem)
{
	int ret;
	struct sg_table *sgt;

	ret = dma_resv_lock_interruptible(shmem->base.resv, NULL);
	if (ret)
		return ERR_PTR(ret);
	sgt = drm_gem_shmem_get_pages_sgt_locked(shmem);
	dma_resv_unlock(shmem->base.resv);

	return sgt;
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_get_pages_sgt);

/**
 * drm_gem_shmem_prime_import_sg_table - Produce a shmem GEM object from
 *                 another driver's scatter/gather table of pinned pages
 * @dev: Device to import into
 * @attach: DMA-BUF attachment
 * @sgt: Scatter/gather table of pinned pages
 *
 * This function imports a scatter/gather table exported via DMA-BUF by
 * another driver. Drivers that use the shmem helpers should set this as their
 * &drm_driver.gem_prime_import_sg_table callback.
 *
 * Returns:
 * A pointer to a newly created GEM object or an ERR_PTR-encoded negative
 * error code on failure.
 */
struct drm_gem_object *
drm_gem_shmem_prime_import_sg_table(struct drm_device *dev,
				    struct dma_buf_attachment *attach,
				    struct sg_table *sgt)
{
	size_t size = PAGE_ALIGN(attach->dmabuf->size);
	struct drm_gem_shmem_object *shmem;

	shmem = __drm_gem_shmem_create(dev, size, true, NULL);
	if (IS_ERR(shmem))
		return ERR_CAST(shmem);

	shmem->sgt = sgt;

	drm_dbg_prime(dev, "size = %zu\n", size);

	return &shmem->base;
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_prime_import_sg_table);

/**
 * drm_gem_shmem_prime_import_no_map - Import dmabuf without mapping its sg_table
 * @dev: Device to import into
 * @dma_buf: dma-buf object to import
 *
 * Drivers that use the shmem helpers but also wants to import dmabuf without
 * mapping its sg_table can use this as their &drm_driver.gem_prime_import
 * implementation.
 */
struct drm_gem_object *drm_gem_shmem_prime_import_no_map(struct drm_device *dev,
							 struct dma_buf *dma_buf)
{
	struct dma_buf_attachment *attach;
	struct drm_gem_shmem_object *shmem;
	struct drm_gem_object *obj;
	size_t size;
	int ret;

	if (drm_gem_is_prime_exported_dma_buf(dev, dma_buf)) {
		/*
		 * Importing dmabuf exported from our own gem increases
		 * refcount on gem itself instead of f_count of dmabuf.
		 */
		obj = dma_buf->priv;
		drm_gem_object_get(obj);
		return obj;
	}

	attach = dma_buf_attach(dma_buf, dev->dev);
	if (IS_ERR(attach))
		return ERR_CAST(attach);

	get_dma_buf(dma_buf);

	size = PAGE_ALIGN(attach->dmabuf->size);

	shmem = __drm_gem_shmem_create(dev, size, true, NULL);
	if (IS_ERR(shmem)) {
		ret = PTR_ERR(shmem);
		goto fail_detach;
	}

	drm_dbg_prime(dev, "size = %zu\n", size);

	shmem->base.import_attach = attach;
	shmem->base.resv = dma_buf->resv;

	return &shmem->base;

fail_detach:
	dma_buf_detach(dma_buf, attach);
	dma_buf_put(dma_buf);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_prime_import_no_map);

MODULE_DESCRIPTION("DRM SHMEM memory-management helpers");
MODULE_IMPORT_NS("DMA_BUF");
MODULE_LICENSE("GPL v2");
