/* SPDX-License-Identifier: GPL-2.0 */
/*
 * include/linux/backing-dev.h
 *
 * low-level device information and state which is propagated up through
 * to high-level code.
 */

#ifndef _LINUX_BACKING_DEV_H
#define _LINUX_BACKING_DEV_H

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/writeback.h>
#include <linux/backing-dev-defs.h>
#include <linux/slab.h>

static inline struct backing_dev_info *bdi_get(struct backing_dev_info *bdi)
{
	kref_get(&bdi->refcnt);
	return bdi;
}

struct backing_dev_info *bdi_get_by_id(u64 id);
void bdi_put(struct backing_dev_info *bdi);

__printf(2, 3)
int bdi_register(struct backing_dev_info *bdi, const char *fmt, ...);
__printf(2, 0)
int bdi_register_va(struct backing_dev_info *bdi, const char *fmt,
		    va_list args);
void bdi_set_owner(struct backing_dev_info *bdi, struct device *owner);
void bdi_unregister(struct backing_dev_info *bdi);

struct backing_dev_info *bdi_alloc(int node_id);

void wb_start_background_writeback(struct bdi_writeback *wb);
void wb_workfn(struct work_struct *work);

void wb_wait_for_completion(struct wb_completion *done);

extern spinlock_t bdi_lock;
extern struct list_head bdi_list;

extern struct workqueue_struct *bdi_wq;

static inline bool wb_has_dirty_io(struct bdi_writeback *wb)
{
	return test_bit(WB_has_dirty_io, &wb->state);
}

static inline bool bdi_has_dirty_io(struct backing_dev_info *bdi)
{
	/*
	 * @bdi->tot_write_bandwidth is guaranteed to be > 0 if there are
	 * any dirty wbs.  See wb_update_write_bandwidth().
	 */
	return atomic_long_read(&bdi->tot_write_bandwidth);
}

static inline void wb_stat_mod(struct bdi_writeback *wb,
				 enum wb_stat_item item, s64 amount)
{
	percpu_counter_add_batch(&wb->stat[item], amount, WB_STAT_BATCH);
}

static inline void inc_wb_stat(struct bdi_writeback *wb, enum wb_stat_item item)
{
	wb_stat_mod(wb, item, 1);
}

static inline void dec_wb_stat(struct bdi_writeback *wb, enum wb_stat_item item)
{
	wb_stat_mod(wb, item, -1);
}

static inline s64 wb_stat(struct bdi_writeback *wb, enum wb_stat_item item)
{
	return percpu_counter_read_positive(&wb->stat[item]);
}

static inline s64 wb_stat_sum(struct bdi_writeback *wb, enum wb_stat_item item)
{
	return percpu_counter_sum_positive(&wb->stat[item]);
}

extern void wb_writeout_inc(struct bdi_writeback *wb);

/*
 * maximal error of a stat counter.
 */
static inline unsigned long wb_stat_error(void)
{
#ifdef CONFIG_SMP
	return (unsigned long)nr_cpu_ids * WB_STAT_BATCH;
#else
	return 1;
#endif
}

/* BDI ratio is expressed as part per 1000000 for finer granularity. */
#define BDI_RATIO_SCALE 10000

u64 bdi_get_min_bytes(struct backing_dev_info *bdi);
u64 bdi_get_max_bytes(struct backing_dev_info *bdi);
int bdi_set_min_ratio(struct backing_dev_info *bdi, unsigned int min_ratio);
int bdi_set_max_ratio(struct backing_dev_info *bdi, unsigned int max_ratio);
int bdi_set_min_ratio_no_scale(struct backing_dev_info *bdi, unsigned int min_ratio);
int bdi_set_max_ratio_no_scale(struct backing_dev_info *bdi, unsigned int max_ratio);
int bdi_set_min_bytes(struct backing_dev_info *bdi, u64 min_bytes);
int bdi_set_max_bytes(struct backing_dev_info *bdi, u64 max_bytes);
int bdi_set_strict_limit(struct backing_dev_info *bdi, unsigned int strict_limit);

/*
 * Flags in backing_dev_info::capability
 *
 * BDI_CAP_WRITEBACK:		Supports dirty page writeback, and dirty pages
 *				should contribute to accounting
 * BDI_CAP_WRITEBACK_ACCT:	Automatically account writeback pages
 * BDI_CAP_STRICTLIMIT:		Keep number of dirty pages below bdi threshold
 */
#define BDI_CAP_WRITEBACK		(1 << 0)
#define BDI_CAP_WRITEBACK_ACCT		(1 << 1)
#define BDI_CAP_STRICTLIMIT		(1 << 2)

extern struct backing_dev_info noop_backing_dev_info;

int bdi_init(struct backing_dev_info *bdi);

/**
 * writeback_in_progress - determine whether there is writeback in progress
 * @wb: bdi_writeback of interest
 *
 * Determine whether there is writeback waiting to be handled against a
 * bdi_writeback.
 */
static inline bool writeback_in_progress(struct bdi_writeback *wb)
{
	return test_bit(WB_writeback_running, &wb->state);
}

struct backing_dev_info *inode_to_bdi(struct inode *inode);

static inline bool mapping_can_writeback(struct address_space *mapping)
{
	return inode_to_bdi(mapping->host)->capabilities & BDI_CAP_WRITEBACK;
}

#ifdef CONFIG_CGROUP_WRITEBACK

struct bdi_writeback *wb_get_lookup(struct backing_dev_info *bdi,
				    struct cgroup_subsys_state *memcg_css);
struct bdi_writeback *wb_get_create(struct backing_dev_info *bdi,
				    struct cgroup_subsys_state *memcg_css,
				    gfp_t gfp);
void wb_memcg_offline(struct mem_cgroup *memcg);
void wb_blkcg_offline(struct cgroup_subsys_state *css);

/**
 * inode_cgwb_enabled - test whether cgroup writeback is enabled on an inode
 * @inode: inode of interest
 *
 * Cgroup writeback requires support from the filesystem.  Also, both memcg and
 * iocg have to be on the default hierarchy.  Test whether all conditions are
 * met.
 *
 * Note that the test result may change dynamically on the same inode
 * depending on how memcg and iocg are configured.
 */
static inline bool inode_cgwb_enabled(struct inode *inode)
{
	struct backing_dev_info *bdi = inode_to_bdi(inode);

	return cgroup_subsys_on_dfl(memory_cgrp_subsys) &&
		cgroup_subsys_on_dfl(io_cgrp_subsys) &&
		(bdi->capabilities & BDI_CAP_WRITEBACK) &&
		(inode->i_sb->s_iflags & SB_I_CGROUPWB);
}

/**
 * wb_find_current - find wb for %current on a bdi
 * @bdi: bdi of interest
 *
 * Find the wb of @bdi which matches both the memcg and blkcg of %current.
 * Must be called under rcu_read_lock() which protects the returend wb.
 * NULL if not found.
 */
static inline struct bdi_writeback *wb_find_current(struct backing_dev_info *bdi)
{
	struct cgroup_subsys_state *memcg_css;
	struct bdi_writeback *wb;

	memcg_css = task_css(current, memory_cgrp_id);
	if (!memcg_css->parent)
		return &bdi->wb;

	wb = radix_tree_lookup(&bdi->cgwb_tree, memcg_css->id);

	/*
	 * %current's blkcg equals the effective blkcg of its memcg.  No
	 * need to use the relatively expensive cgroup_get_e_css().
	 */
	if (likely(wb && wb->blkcg_css == task_css(current, io_cgrp_id)))
		return wb;
	return NULL;
}

/**
 * wb_get_create_current - get or create wb for %current on a bdi
 * @bdi: bdi of interest
 * @gfp: allocation mask
 *
 * Equivalent to wb_get_create() on %current's memcg.  This function is
 * called from a relatively hot path and optimizes the common cases using
 * wb_find_current().
 */
static inline struct bdi_writeback *
wb_get_create_current(struct backing_dev_info *bdi, gfp_t gfp)
{
	struct bdi_writeback *wb;

	rcu_read_lock();
	wb = wb_find_current(bdi);
	if (wb && unlikely(!wb_tryget(wb)))
		wb = NULL;
	rcu_read_unlock();

	if (unlikely(!wb)) {
		struct cgroup_subsys_state *memcg_css;

		memcg_css = task_get_css(current, memory_cgrp_id);
		wb = wb_get_create(bdi, memcg_css, gfp);
		css_put(memcg_css);
	}
	return wb;
}

/**
 * inode_to_wb - determine the wb of an inode
 * @inode: inode of interest
 *
 * Returns the wb @inode is currently associated with.  The caller must be
 * holding either @inode->i_lock, the i_pages lock, or the
 * associated wb's list_lock.
 */
static inline struct bdi_writeback *inode_to_wb(const struct inode *inode)
{
#ifdef CONFIG_LOCKDEP
	WARN_ON_ONCE(debug_locks &&
		     (inode->i_sb->s_iflags & SB_I_CGROUPWB) &&
		     (!lockdep_is_held(&inode->i_lock) &&
		      !lockdep_is_held(&inode->i_mapping->i_pages.xa_lock) &&
		      !lockdep_is_held(&inode->i_wb->list_lock)));
#endif
	return inode->i_wb;
}

static inline struct bdi_writeback *inode_to_wb_wbc(
				struct inode *inode,
				struct writeback_control *wbc)
{
	/*
	 * If wbc does not have inode attached, it means cgroup writeback was
	 * disabled when wbc started. Just use the default wb in that case.
	 */
	return wbc->wb ? wbc->wb : &inode_to_bdi(inode)->wb;
}

/**
 * unlocked_inode_to_wb_begin - begin unlocked inode wb access transaction
 * @inode: target inode
 * @cookie: output param, to be passed to the end function
 *
 * The caller wants to access the wb associated with @inode but isn't
 * holding inode->i_lock, the i_pages lock or wb->list_lock.  This
 * function determines the wb associated with @inode and ensures that the
 * association doesn't change until the transaction is finished with
 * unlocked_inode_to_wb_end().
 *
 * The caller must call unlocked_inode_to_wb_end() with *@cookie afterwards and
 * can't sleep during the transaction.  IRQs may or may not be disabled on
 * return.
 */
static inline struct bdi_writeback *
unlocked_inode_to_wb_begin(struct inode *inode, struct wb_lock_cookie *cookie)
{
	rcu_read_lock();

	/*
	 * Paired with store_release in inode_switch_wbs_work_fn() and
	 * ensures that we see the new wb if we see cleared I_WB_SWITCH.
	 */
	cookie->locked = smp_load_acquire(&inode->i_state) & I_WB_SWITCH;

	if (unlikely(cookie->locked))
		xa_lock_irqsave(&inode->i_mapping->i_pages, cookie->flags);

	/*
	 * Protected by either !I_WB_SWITCH + rcu_read_lock() or the i_pages
	 * lock.  inode_to_wb() will bark.  Deref directly.
	 */
	return inode->i_wb;
}

/**
 * unlocked_inode_to_wb_end - end inode wb access transaction
 * @inode: target inode
 * @cookie: @cookie from unlocked_inode_to_wb_begin()
 */
static inline void unlocked_inode_to_wb_end(struct inode *inode,
					    struct wb_lock_cookie *cookie)
{
	if (unlikely(cookie->locked))
		xa_unlock_irqrestore(&inode->i_mapping->i_pages, cookie->flags);

	rcu_read_unlock();
}

#else	/* CONFIG_CGROUP_WRITEBACK */

static inline bool inode_cgwb_enabled(struct inode *inode)
{
	return false;
}

static inline struct bdi_writeback *wb_find_current(struct backing_dev_info *bdi)
{
	return &bdi->wb;
}

static inline struct bdi_writeback *
wb_get_create_current(struct backing_dev_info *bdi, gfp_t gfp)
{
	return &bdi->wb;
}

static inline struct bdi_writeback *inode_to_wb(struct inode *inode)
{
	return &inode_to_bdi(inode)->wb;
}

static inline struct bdi_writeback *inode_to_wb_wbc(
				struct inode *inode,
				struct writeback_control *wbc)
{
	return inode_to_wb(inode);
}


static inline struct bdi_writeback *
unlocked_inode_to_wb_begin(struct inode *inode, struct wb_lock_cookie *cookie)
{
	return inode_to_wb(inode);
}

static inline void unlocked_inode_to_wb_end(struct inode *inode,
					    struct wb_lock_cookie *cookie)
{
}

static inline void wb_memcg_offline(struct mem_cgroup *memcg)
{
}

static inline void wb_blkcg_offline(struct cgroup_subsys_state *css)
{
}

#endif	/* CONFIG_CGROUP_WRITEBACK */

const char *bdi_dev_name(struct backing_dev_info *bdi);

#endif	/* _LINUX_BACKING_DEV_H */
