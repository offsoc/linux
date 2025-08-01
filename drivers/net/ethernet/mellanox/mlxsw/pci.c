// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/* Copyright (c) 2015-2018 Mellanox Technologies. All rights reserved */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/export.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/if_vlan.h>
#include <linux/log2.h>
#include <linux/string.h>
#include <net/page_pool/helpers.h>

#include "pci_hw.h"
#include "pci.h"
#include "core.h"
#include "cmd.h"
#include "port.h"
#include "resources.h"
#include "txheader.h"

#define mlxsw_pci_write32(mlxsw_pci, reg, val) \
	iowrite32be(val, (mlxsw_pci)->hw_addr + (MLXSW_PCI_ ## reg))
#define mlxsw_pci_read32(mlxsw_pci, reg) \
	ioread32be((mlxsw_pci)->hw_addr + (MLXSW_PCI_ ## reg))

enum mlxsw_pci_queue_type {
	MLXSW_PCI_QUEUE_TYPE_SDQ,
	MLXSW_PCI_QUEUE_TYPE_RDQ,
	MLXSW_PCI_QUEUE_TYPE_CQ,
	MLXSW_PCI_QUEUE_TYPE_EQ,
};

#define MLXSW_PCI_QUEUE_TYPE_COUNT	4

enum mlxsw_pci_cq_type {
	MLXSW_PCI_CQ_SDQ,
	MLXSW_PCI_CQ_RDQ,
};

static const u16 mlxsw_pci_doorbell_type_offset[] = {
	MLXSW_PCI_DOORBELL_SDQ_OFFSET,	/* for type MLXSW_PCI_QUEUE_TYPE_SDQ */
	MLXSW_PCI_DOORBELL_RDQ_OFFSET,	/* for type MLXSW_PCI_QUEUE_TYPE_RDQ */
	MLXSW_PCI_DOORBELL_CQ_OFFSET,	/* for type MLXSW_PCI_QUEUE_TYPE_CQ */
	MLXSW_PCI_DOORBELL_EQ_OFFSET,	/* for type MLXSW_PCI_QUEUE_TYPE_EQ */
};

static const u16 mlxsw_pci_doorbell_arm_type_offset[] = {
	0, /* unused */
	0, /* unused */
	MLXSW_PCI_DOORBELL_ARM_CQ_OFFSET, /* for type MLXSW_PCI_QUEUE_TYPE_CQ */
	MLXSW_PCI_DOORBELL_ARM_EQ_OFFSET, /* for type MLXSW_PCI_QUEUE_TYPE_EQ */
};

struct mlxsw_pci_mem_item {
	char *buf;
	dma_addr_t mapaddr;
	size_t size;
};

struct mlxsw_pci_queue_elem_info {
	struct page *pages[MLXSW_PCI_WQE_SG_ENTRIES];
	char *elem; /* pointer to actual dma mapped element mem chunk */
	struct {
		struct sk_buff *skb;
	} sdq;
};

struct mlxsw_pci_queue {
	spinlock_t lock; /* for queue accesses */
	struct mlxsw_pci_mem_item mem_item;
	struct mlxsw_pci_queue_elem_info *elem_info;
	u16 producer_counter;
	u16 consumer_counter;
	u16 count; /* number of elements in queue */
	u8 num; /* queue number */
	u8 elem_size; /* size of one element */
	enum mlxsw_pci_queue_type type;
	struct mlxsw_pci *pci;
	union {
		struct {
			enum mlxsw_pci_cqe_v v;
			struct mlxsw_pci_queue *dq;
			struct napi_struct napi;
			struct page_pool *page_pool;
		} cq;
		struct {
			struct tasklet_struct tasklet;
		} eq;
		struct {
			struct mlxsw_pci_queue *cq;
		} rdq;
	} u;
};

struct mlxsw_pci_queue_type_group {
	struct mlxsw_pci_queue *q;
	u8 count; /* number of queues in group */
};

struct mlxsw_pci {
	struct pci_dev *pdev;
	u8 __iomem *hw_addr;
	u64 free_running_clock_offset;
	u64 utc_sec_offset;
	u64 utc_nsec_offset;
	bool lag_mode_support;
	bool cff_support;
	enum mlxsw_cmd_mbox_config_profile_lag_mode lag_mode;
	enum mlxsw_cmd_mbox_config_profile_flood_mode flood_mode;
	u8 num_sg_entries; /* Number of scatter/gather entries for packets. */
	struct mlxsw_pci_queue_type_group queues[MLXSW_PCI_QUEUE_TYPE_COUNT];
	u32 doorbell_offset;
	struct mlxsw_core *core;
	struct {
		struct mlxsw_pci_mem_item *items;
		unsigned int count;
	} fw_area;
	struct {
		struct mlxsw_pci_mem_item out_mbox;
		struct mlxsw_pci_mem_item in_mbox;
		struct mutex lock; /* Lock access to command registers */
		struct {
			u8 status;
			u64 out_param;
		} comp;
	} cmd;
	struct mlxsw_bus_info bus_info;
	const struct pci_device_id *id;
	enum mlxsw_pci_cqe_v max_cqe_ver; /* Maximal supported CQE version */
	u8 num_cqs; /* Number of CQs */
	u8 num_sdqs; /* Number of SDQs */
	bool skip_reset;
	struct net_device *napi_dev_tx;
	struct net_device *napi_dev_rx;
};

static int mlxsw_pci_napi_devs_init(struct mlxsw_pci *mlxsw_pci)
{
	int err;

	mlxsw_pci->napi_dev_tx = alloc_netdev_dummy(0);
	if (!mlxsw_pci->napi_dev_tx)
		return -ENOMEM;
	strscpy(mlxsw_pci->napi_dev_tx->name, "mlxsw_tx",
		sizeof(mlxsw_pci->napi_dev_tx->name));

	mlxsw_pci->napi_dev_rx = alloc_netdev_dummy(0);
	if (!mlxsw_pci->napi_dev_rx) {
		err = -ENOMEM;
		goto err_alloc_rx;
	}
	strscpy(mlxsw_pci->napi_dev_rx->name, "mlxsw_rx",
		sizeof(mlxsw_pci->napi_dev_rx->name));
	netif_threaded_enable(mlxsw_pci->napi_dev_rx);

	return 0;

err_alloc_rx:
	free_netdev(mlxsw_pci->napi_dev_tx);
	return err;
}

static void mlxsw_pci_napi_devs_fini(struct mlxsw_pci *mlxsw_pci)
{
	free_netdev(mlxsw_pci->napi_dev_rx);
	free_netdev(mlxsw_pci->napi_dev_tx);
}

static char *__mlxsw_pci_queue_elem_get(struct mlxsw_pci_queue *q,
					size_t elem_size, int elem_index)
{
	return q->mem_item.buf + (elem_size * elem_index);
}

static struct mlxsw_pci_queue_elem_info *
mlxsw_pci_queue_elem_info_get(struct mlxsw_pci_queue *q, int elem_index)
{
	return &q->elem_info[elem_index];
}

static struct mlxsw_pci_queue_elem_info *
mlxsw_pci_queue_elem_info_producer_get(struct mlxsw_pci_queue *q)
{
	int index = q->producer_counter & (q->count - 1);

	if ((u16) (q->producer_counter - q->consumer_counter) == q->count)
		return NULL;
	return mlxsw_pci_queue_elem_info_get(q, index);
}

static struct mlxsw_pci_queue_elem_info *
mlxsw_pci_queue_elem_info_consumer_get(struct mlxsw_pci_queue *q)
{
	int index = q->consumer_counter & (q->count - 1);

	return mlxsw_pci_queue_elem_info_get(q, index);
}

static char *mlxsw_pci_queue_elem_get(struct mlxsw_pci_queue *q, int elem_index)
{
	return mlxsw_pci_queue_elem_info_get(q, elem_index)->elem;
}

static bool mlxsw_pci_elem_hw_owned(struct mlxsw_pci_queue *q, bool owner_bit)
{
	return owner_bit != !!(q->consumer_counter & q->count);
}

static struct mlxsw_pci_queue_type_group *
mlxsw_pci_queue_type_group_get(struct mlxsw_pci *mlxsw_pci,
			       enum mlxsw_pci_queue_type q_type)
{
	return &mlxsw_pci->queues[q_type];
}

static struct mlxsw_pci_queue *
__mlxsw_pci_queue_get(struct mlxsw_pci *mlxsw_pci,
		      enum mlxsw_pci_queue_type q_type, u8 q_num)
{
	return &mlxsw_pci->queues[q_type].q[q_num];
}

static struct mlxsw_pci_queue *mlxsw_pci_sdq_get(struct mlxsw_pci *mlxsw_pci,
						 u8 q_num)
{
	return __mlxsw_pci_queue_get(mlxsw_pci,
				     MLXSW_PCI_QUEUE_TYPE_SDQ, q_num);
}

static struct mlxsw_pci_queue *mlxsw_pci_cq_get(struct mlxsw_pci *mlxsw_pci,
						u8 q_num)
{
	return __mlxsw_pci_queue_get(mlxsw_pci, MLXSW_PCI_QUEUE_TYPE_CQ, q_num);
}

static struct mlxsw_pci_queue *mlxsw_pci_eq_get(struct mlxsw_pci *mlxsw_pci)
{
	/* There is only one EQ at index 0. */
	return __mlxsw_pci_queue_get(mlxsw_pci, MLXSW_PCI_QUEUE_TYPE_EQ, 0);
}

static void __mlxsw_pci_queue_doorbell_set(struct mlxsw_pci *mlxsw_pci,
					   struct mlxsw_pci_queue *q,
					   u16 val)
{
	mlxsw_pci_write32(mlxsw_pci,
			  DOORBELL(mlxsw_pci->doorbell_offset,
				   mlxsw_pci_doorbell_type_offset[q->type],
				   q->num), val);
}

static void __mlxsw_pci_queue_doorbell_arm_set(struct mlxsw_pci *mlxsw_pci,
					       struct mlxsw_pci_queue *q,
					       u16 val)
{
	mlxsw_pci_write32(mlxsw_pci,
			  DOORBELL(mlxsw_pci->doorbell_offset,
				   mlxsw_pci_doorbell_arm_type_offset[q->type],
				   q->num), val);
}

static void mlxsw_pci_queue_doorbell_producer_ring(struct mlxsw_pci *mlxsw_pci,
						   struct mlxsw_pci_queue *q)
{
	wmb(); /* ensure all writes are done before we ring a bell */
	__mlxsw_pci_queue_doorbell_set(mlxsw_pci, q, q->producer_counter);
}

static void mlxsw_pci_queue_doorbell_consumer_ring(struct mlxsw_pci *mlxsw_pci,
						   struct mlxsw_pci_queue *q)
{
	wmb(); /* ensure all writes are done before we ring a bell */
	__mlxsw_pci_queue_doorbell_set(mlxsw_pci, q,
				       q->consumer_counter + q->count);
}

static void
mlxsw_pci_queue_doorbell_arm_consumer_ring(struct mlxsw_pci *mlxsw_pci,
					   struct mlxsw_pci_queue *q)
{
	wmb(); /* ensure all writes are done before we ring a bell */
	__mlxsw_pci_queue_doorbell_arm_set(mlxsw_pci, q, q->consumer_counter);
}

static dma_addr_t __mlxsw_pci_queue_page_get(struct mlxsw_pci_queue *q,
					     int page_index)
{
	return q->mem_item.mapaddr + MLXSW_PCI_PAGE_SIZE * page_index;
}

static int mlxsw_pci_sdq_init(struct mlxsw_pci *mlxsw_pci, char *mbox,
			      struct mlxsw_pci_queue *q)
{
	struct mlxsw_pci_queue *cq;
	int tclass;
	u8 cq_num;
	int lp;
	int i;
	int err;

	q->producer_counter = 0;
	q->consumer_counter = 0;
	tclass = q->num == MLXSW_PCI_SDQ_EMAD_INDEX ? MLXSW_PCI_SDQ_EMAD_TC :
						      MLXSW_PCI_SDQ_CTL_TC;
	lp = q->num == MLXSW_PCI_SDQ_EMAD_INDEX ? MLXSW_CMD_MBOX_SW2HW_DQ_SDQ_LP_IGNORE_WQE :
						  MLXSW_CMD_MBOX_SW2HW_DQ_SDQ_LP_WQE;

	/* Set CQ of same number of this SDQ. */
	cq_num = q->num;
	mlxsw_cmd_mbox_sw2hw_dq_cq_set(mbox, cq_num);
	mlxsw_cmd_mbox_sw2hw_dq_sdq_lp_set(mbox, lp);
	mlxsw_cmd_mbox_sw2hw_dq_sdq_tclass_set(mbox, tclass);
	mlxsw_cmd_mbox_sw2hw_dq_log2_dq_sz_set(mbox, 3); /* 8 pages */
	for (i = 0; i < MLXSW_PCI_AQ_PAGES; i++) {
		dma_addr_t mapaddr = __mlxsw_pci_queue_page_get(q, i);

		mlxsw_cmd_mbox_sw2hw_dq_pa_set(mbox, i, mapaddr);
	}

	err = mlxsw_cmd_sw2hw_sdq(mlxsw_pci->core, mbox, q->num);
	if (err)
		return err;

	cq = mlxsw_pci_cq_get(mlxsw_pci, cq_num);
	cq->u.cq.dq = q;
	mlxsw_pci_queue_doorbell_producer_ring(mlxsw_pci, q);
	return 0;
}

static void mlxsw_pci_sdq_fini(struct mlxsw_pci *mlxsw_pci,
			       struct mlxsw_pci_queue *q)
{
	mlxsw_cmd_hw2sw_sdq(mlxsw_pci->core, q->num);
}

#define MLXSW_PCI_SKB_HEADROOM (NET_SKB_PAD + NET_IP_ALIGN)

#define MLXSW_PCI_RX_BUF_SW_OVERHEAD		\
		(MLXSW_PCI_SKB_HEADROOM +	\
		SKB_DATA_ALIGN(sizeof(struct skb_shared_info)))

static void
mlxsw_pci_wqe_rx_frag_set(struct mlxsw_pci *mlxsw_pci, struct page *page,
			  char *wqe, int index, size_t frag_len)
{
	dma_addr_t mapaddr;

	mapaddr = page_pool_get_dma_addr(page);

	if (index == 0) {
		mapaddr += MLXSW_PCI_SKB_HEADROOM;
		frag_len = frag_len - MLXSW_PCI_RX_BUF_SW_OVERHEAD;
	}

	mlxsw_pci_wqe_address_set(wqe, index, mapaddr);
	mlxsw_pci_wqe_byte_count_set(wqe, index, frag_len);
}

static int mlxsw_pci_wqe_frag_map(struct mlxsw_pci *mlxsw_pci, char *wqe,
				  int index, char *frag_data, size_t frag_len,
				  int direction)
{
	struct pci_dev *pdev = mlxsw_pci->pdev;
	dma_addr_t mapaddr;

	mapaddr = dma_map_single(&pdev->dev, frag_data, frag_len, direction);
	if (unlikely(dma_mapping_error(&pdev->dev, mapaddr))) {
		dev_err_ratelimited(&pdev->dev, "failed to dma map tx frag\n");
		return -EIO;
	}
	mlxsw_pci_wqe_address_set(wqe, index, mapaddr);
	mlxsw_pci_wqe_byte_count_set(wqe, index, frag_len);
	return 0;
}

static void mlxsw_pci_wqe_frag_unmap(struct mlxsw_pci *mlxsw_pci, char *wqe,
				     int index, int direction)
{
	struct pci_dev *pdev = mlxsw_pci->pdev;
	size_t frag_len = mlxsw_pci_wqe_byte_count_get(wqe, index);
	dma_addr_t mapaddr = mlxsw_pci_wqe_address_get(wqe, index);

	if (!frag_len)
		return;
	dma_unmap_single(&pdev->dev, mapaddr, frag_len, direction);
}

static struct sk_buff *mlxsw_pci_rdq_build_skb(struct mlxsw_pci_queue *q,
					       struct page *pages[],
					       u16 byte_count)
{
	struct mlxsw_pci_queue *cq = q->u.rdq.cq;
	unsigned int linear_data_size;
	struct page_pool *page_pool;
	struct sk_buff *skb;
	int page_index = 0;
	bool linear_only;
	void *data;

	linear_only = byte_count + MLXSW_PCI_RX_BUF_SW_OVERHEAD <= PAGE_SIZE;
	linear_data_size = linear_only ? byte_count :
					 PAGE_SIZE -
					 MLXSW_PCI_RX_BUF_SW_OVERHEAD;

	page_pool = cq->u.cq.page_pool;
	page_pool_dma_sync_for_cpu(page_pool, pages[page_index],
				   MLXSW_PCI_SKB_HEADROOM, linear_data_size);

	data = page_address(pages[page_index]);
	net_prefetch(data);

	skb = napi_build_skb(data, PAGE_SIZE);
	if (unlikely(!skb))
		return ERR_PTR(-ENOMEM);

	skb_reserve(skb, MLXSW_PCI_SKB_HEADROOM);
	skb_put(skb, linear_data_size);

	if (linear_only)
		return skb;

	byte_count -= linear_data_size;
	page_index++;

	while (byte_count > 0) {
		unsigned int frag_size;
		struct page *page;

		page = pages[page_index];
		frag_size = min(byte_count, PAGE_SIZE);
		page_pool_dma_sync_for_cpu(page_pool, page, 0, frag_size);
		skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags,
				page, 0, frag_size, PAGE_SIZE);
		byte_count -= frag_size;
		page_index++;
	}

	return skb;
}

static int mlxsw_pci_rdq_page_alloc(struct mlxsw_pci_queue *q,
				    struct mlxsw_pci_queue_elem_info *elem_info,
				    int index)
{
	struct mlxsw_pci_queue *cq = q->u.rdq.cq;
	char *wqe = elem_info->elem;
	struct page *page;

	page = page_pool_dev_alloc_pages(cq->u.cq.page_pool);
	if (unlikely(!page))
		return -ENOMEM;

	mlxsw_pci_wqe_rx_frag_set(q->pci, page, wqe, index, PAGE_SIZE);
	elem_info->pages[index] = page;
	return 0;
}

static void mlxsw_pci_rdq_page_free(struct mlxsw_pci_queue *q,
				    struct mlxsw_pci_queue_elem_info *elem_info,
				    int index)
{
	struct mlxsw_pci_queue *cq = q->u.rdq.cq;

	page_pool_put_page(cq->u.cq.page_pool, elem_info->pages[index], -1,
			   false);
}

static u8 mlxsw_pci_num_sg_entries_get(u16 byte_count)
{
	return DIV_ROUND_UP(byte_count + MLXSW_PCI_RX_BUF_SW_OVERHEAD,
			    PAGE_SIZE);
}

static int
mlxsw_pci_elem_info_pages_ref_store(const struct mlxsw_pci_queue *q,
				    const struct mlxsw_pci_queue_elem_info *el,
				    u16 byte_count, struct page *pages[],
				    u8 *p_num_sg_entries)
{
	u8 num_sg_entries;
	int i;

	num_sg_entries = mlxsw_pci_num_sg_entries_get(byte_count);
	if (WARN_ON_ONCE(num_sg_entries > q->pci->num_sg_entries))
		return -EINVAL;

	for (i = 0; i < num_sg_entries; i++)
		pages[i] = el->pages[i];

	*p_num_sg_entries = num_sg_entries;
	return 0;
}

static int
mlxsw_pci_rdq_pages_alloc(struct mlxsw_pci_queue *q,
			  struct mlxsw_pci_queue_elem_info *elem_info,
			  u8 num_sg_entries)
{
	struct page *old_pages[MLXSW_PCI_WQE_SG_ENTRIES];
	struct mlxsw_pci_queue *cq = q->u.rdq.cq;
	int i, err;

	for (i = 0; i < num_sg_entries; i++) {
		old_pages[i] = elem_info->pages[i];
		err = mlxsw_pci_rdq_page_alloc(q, elem_info, i);
		if (err) {
			dev_err_ratelimited(&q->pci->pdev->dev, "Failed to alloc page\n");
			goto err_page_alloc;
		}
	}

	return 0;

err_page_alloc:
	for (i--; i >= 0; i--)
		page_pool_recycle_direct(cq->u.cq.page_pool, old_pages[i]);

	return err;
}

static void
mlxsw_pci_rdq_pages_recycle(struct mlxsw_pci_queue *q, struct page *pages[],
			    u8 num_sg_entries)
{
	struct mlxsw_pci_queue *cq = q->u.rdq.cq;
	int i;

	for (i = 0; i < num_sg_entries; i++)
		page_pool_recycle_direct(cq->u.cq.page_pool, pages[i]);
}

static int mlxsw_pci_rdq_init(struct mlxsw_pci *mlxsw_pci, char *mbox,
			      struct mlxsw_pci_queue *q)
{
	struct mlxsw_pci_queue_elem_info *elem_info;
	u8 sdq_count = mlxsw_pci->num_sdqs;
	struct mlxsw_pci_queue *cq;
	u8 cq_num;
	int i, j;
	int err;

	q->producer_counter = 0;
	q->consumer_counter = 0;

	/* Set CQ of same number of this RDQ with base
	 * above SDQ count as the lower ones are assigned to SDQs.
	 */
	cq_num = sdq_count + q->num;
	mlxsw_cmd_mbox_sw2hw_dq_cq_set(mbox, cq_num);
	mlxsw_cmd_mbox_sw2hw_dq_log2_dq_sz_set(mbox, 3); /* 8 pages */
	for (i = 0; i < MLXSW_PCI_AQ_PAGES; i++) {
		dma_addr_t mapaddr = __mlxsw_pci_queue_page_get(q, i);

		mlxsw_cmd_mbox_sw2hw_dq_pa_set(mbox, i, mapaddr);
	}

	err = mlxsw_cmd_sw2hw_rdq(mlxsw_pci->core, mbox, q->num);
	if (err)
		return err;

	cq = mlxsw_pci_cq_get(mlxsw_pci, cq_num);
	cq->u.cq.dq = q;
	q->u.rdq.cq = cq;

	mlxsw_pci_queue_doorbell_producer_ring(mlxsw_pci, q);

	for (i = 0; i < q->count; i++) {
		elem_info = mlxsw_pci_queue_elem_info_producer_get(q);
		BUG_ON(!elem_info);

		for (j = 0; j < mlxsw_pci->num_sg_entries; j++) {
			err = mlxsw_pci_rdq_page_alloc(q, elem_info, j);
			if (err)
				goto rollback;
		}
		/* Everything is set up, ring doorbell to pass elem to HW */
		q->producer_counter++;
		mlxsw_pci_queue_doorbell_producer_ring(mlxsw_pci, q);
	}

	return 0;

rollback:
	for (i--; i >= 0; i--) {
		elem_info = mlxsw_pci_queue_elem_info_get(q, i);
		for (j--; j >= 0; j--)
			mlxsw_pci_rdq_page_free(q, elem_info, j);
		j = mlxsw_pci->num_sg_entries;
	}
	q->u.rdq.cq = NULL;
	cq->u.cq.dq = NULL;
	mlxsw_cmd_hw2sw_rdq(mlxsw_pci->core, q->num);

	return err;
}

static void mlxsw_pci_rdq_fini(struct mlxsw_pci *mlxsw_pci,
			       struct mlxsw_pci_queue *q)
{
	struct mlxsw_pci_queue_elem_info *elem_info;
	int i, j;

	mlxsw_cmd_hw2sw_rdq(mlxsw_pci->core, q->num);
	for (i = 0; i < q->count; i++) {
		elem_info = mlxsw_pci_queue_elem_info_get(q, i);
		for (j = 0; j < mlxsw_pci->num_sg_entries; j++)
			mlxsw_pci_rdq_page_free(q, elem_info, j);
	}
}

static void mlxsw_pci_cq_pre_init(struct mlxsw_pci *mlxsw_pci,
				  struct mlxsw_pci_queue *q)
{
	q->u.cq.v = mlxsw_pci->max_cqe_ver;

	if (q->u.cq.v == MLXSW_PCI_CQE_V2 &&
	    q->num < mlxsw_pci->num_sdqs &&
	    !mlxsw_core_sdq_supports_cqe_v2(mlxsw_pci->core))
		q->u.cq.v = MLXSW_PCI_CQE_V1;
}

static unsigned int mlxsw_pci_read32_off(struct mlxsw_pci *mlxsw_pci,
					 ptrdiff_t off)
{
	return ioread32be(mlxsw_pci->hw_addr + off);
}

static void mlxsw_pci_skb_cb_ts_set(struct mlxsw_pci *mlxsw_pci,
				    struct sk_buff *skb,
				    enum mlxsw_pci_cqe_v cqe_v, char *cqe)
{
	u8 ts_type;

	if (cqe_v != MLXSW_PCI_CQE_V2)
		return;

	ts_type = mlxsw_pci_cqe2_time_stamp_type_get(cqe);

	if (ts_type != MLXSW_PCI_CQE_TIME_STAMP_TYPE_UTC &&
	    ts_type != MLXSW_PCI_CQE_TIME_STAMP_TYPE_MIRROR_UTC)
		return;

	mlxsw_skb_cb(skb)->cqe_ts.sec = mlxsw_pci_cqe2_time_stamp_sec_get(cqe);
	mlxsw_skb_cb(skb)->cqe_ts.nsec =
		mlxsw_pci_cqe2_time_stamp_nsec_get(cqe);
}

static void mlxsw_pci_cqe_sdq_handle(struct mlxsw_pci *mlxsw_pci,
				     struct mlxsw_pci_queue *q,
				     u16 consumer_counter_limit,
				     enum mlxsw_pci_cqe_v cqe_v,
				     char *cqe, int budget)
{
	struct pci_dev *pdev = mlxsw_pci->pdev;
	struct mlxsw_pci_queue_elem_info *elem_info;
	struct mlxsw_tx_info tx_info;
	char *wqe;
	struct sk_buff *skb;
	int i;

	spin_lock(&q->lock);
	elem_info = mlxsw_pci_queue_elem_info_consumer_get(q);
	tx_info = mlxsw_skb_cb(elem_info->sdq.skb)->tx_info;
	skb = elem_info->sdq.skb;
	wqe = elem_info->elem;
	for (i = 0; i < MLXSW_PCI_WQE_SG_ENTRIES; i++)
		mlxsw_pci_wqe_frag_unmap(mlxsw_pci, wqe, i, DMA_TO_DEVICE);

	if (unlikely(!tx_info.is_emad &&
		     skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP)) {
		mlxsw_pci_skb_cb_ts_set(mlxsw_pci, skb, cqe_v, cqe);
		mlxsw_core_ptp_transmitted(mlxsw_pci->core, skb,
					   tx_info.local_port);
		skb = NULL;
	}

	if (skb)
		napi_consume_skb(skb, budget);
	elem_info->sdq.skb = NULL;

	if (q->consumer_counter++ != consumer_counter_limit)
		dev_dbg_ratelimited(&pdev->dev, "Consumer counter does not match limit in SDQ\n");
	spin_unlock(&q->lock);
}

static void mlxsw_pci_cqe_rdq_md_tx_port_init(struct sk_buff *skb,
					      const char *cqe)
{
	struct mlxsw_skb_cb *cb = mlxsw_skb_cb(skb);

	if (mlxsw_pci_cqe2_tx_lag_get(cqe)) {
		cb->rx_md_info.tx_port_is_lag = true;
		cb->rx_md_info.tx_lag_id = mlxsw_pci_cqe2_tx_lag_id_get(cqe);
		cb->rx_md_info.tx_lag_port_index =
			mlxsw_pci_cqe2_tx_lag_subport_get(cqe);
	} else {
		cb->rx_md_info.tx_port_is_lag = false;
		cb->rx_md_info.tx_sys_port =
			mlxsw_pci_cqe2_tx_system_port_get(cqe);
	}

	if (cb->rx_md_info.tx_sys_port != MLXSW_PCI_CQE2_TX_PORT_MULTI_PORT &&
	    cb->rx_md_info.tx_sys_port != MLXSW_PCI_CQE2_TX_PORT_INVALID)
		cb->rx_md_info.tx_port_valid = 1;
	else
		cb->rx_md_info.tx_port_valid = 0;
}

static void mlxsw_pci_cqe_rdq_md_init(struct sk_buff *skb, const char *cqe)
{
	struct mlxsw_skb_cb *cb = mlxsw_skb_cb(skb);

	cb->rx_md_info.tx_congestion = mlxsw_pci_cqe2_mirror_cong_get(cqe);
	if (cb->rx_md_info.tx_congestion != MLXSW_PCI_CQE2_MIRROR_CONG_INVALID)
		cb->rx_md_info.tx_congestion_valid = 1;
	else
		cb->rx_md_info.tx_congestion_valid = 0;
	cb->rx_md_info.tx_congestion <<= MLXSW_PCI_CQE2_MIRROR_CONG_SHIFT;

	cb->rx_md_info.latency = mlxsw_pci_cqe2_mirror_latency_get(cqe);
	if (cb->rx_md_info.latency != MLXSW_PCI_CQE2_MIRROR_LATENCY_INVALID)
		cb->rx_md_info.latency_valid = 1;
	else
		cb->rx_md_info.latency_valid = 0;

	cb->rx_md_info.tx_tc = mlxsw_pci_cqe2_mirror_tclass_get(cqe);
	if (cb->rx_md_info.tx_tc != MLXSW_PCI_CQE2_MIRROR_TCLASS_INVALID)
		cb->rx_md_info.tx_tc_valid = 1;
	else
		cb->rx_md_info.tx_tc_valid = 0;

	mlxsw_pci_cqe_rdq_md_tx_port_init(skb, cqe);
}

static void mlxsw_pci_cqe_rdq_handle(struct mlxsw_pci *mlxsw_pci,
				     struct napi_struct *napi,
				     struct mlxsw_pci_queue *q,
				     u16 consumer_counter_limit,
				     enum mlxsw_pci_cqe_v cqe_v, char *cqe)
{
	struct pci_dev *pdev = mlxsw_pci->pdev;
	struct page *pages[MLXSW_PCI_WQE_SG_ENTRIES];
	struct mlxsw_pci_queue_elem_info *elem_info;
	struct mlxsw_rx_info rx_info = {};
	struct sk_buff *skb;
	u8 num_sg_entries;
	u16 byte_count;
	int err;

	elem_info = mlxsw_pci_queue_elem_info_consumer_get(q);

	if (q->consumer_counter++ != consumer_counter_limit)
		dev_dbg_ratelimited(&pdev->dev, "Consumer counter does not match limit in RDQ\n");

	byte_count = mlxsw_pci_cqe_byte_count_get(cqe);
	if (mlxsw_pci_cqe_crc_get(cqe_v, cqe))
		byte_count -= ETH_FCS_LEN;

	err = mlxsw_pci_elem_info_pages_ref_store(q, elem_info, byte_count,
						  pages, &num_sg_entries);
	if (err)
		goto out;

	err = mlxsw_pci_rdq_pages_alloc(q, elem_info, num_sg_entries);
	if (err)
		goto out;

	skb = mlxsw_pci_rdq_build_skb(q, pages, byte_count);
	if (IS_ERR(skb)) {
		dev_err_ratelimited(&pdev->dev, "Failed to build skb for RDQ\n");
		mlxsw_pci_rdq_pages_recycle(q, pages, num_sg_entries);
		goto out;
	}

	skb_mark_for_recycle(skb);

	if (mlxsw_pci_cqe_lag_get(cqe_v, cqe)) {
		rx_info.is_lag = true;
		rx_info.u.lag_id = mlxsw_pci_cqe_lag_id_get(cqe_v, cqe);
		rx_info.lag_port_index =
			mlxsw_pci_cqe_lag_subport_get(cqe_v, cqe);
	} else {
		rx_info.is_lag = false;
		rx_info.u.sys_port = mlxsw_pci_cqe_system_port_get(cqe);
	}

	rx_info.trap_id = mlxsw_pci_cqe_trap_id_get(cqe);

	if (rx_info.trap_id == MLXSW_TRAP_ID_DISCARD_INGRESS_ACL ||
	    rx_info.trap_id == MLXSW_TRAP_ID_DISCARD_EGRESS_ACL) {
		u32 cookie_index = 0;

		if (mlxsw_pci->max_cqe_ver >= MLXSW_PCI_CQE_V2)
			cookie_index = mlxsw_pci_cqe2_user_def_val_orig_pkt_len_get(cqe);
		mlxsw_skb_cb(skb)->rx_md_info.cookie_index = cookie_index;
	} else if (rx_info.trap_id >= MLXSW_TRAP_ID_MIRROR_SESSION0 &&
		   rx_info.trap_id <= MLXSW_TRAP_ID_MIRROR_SESSION7 &&
		   mlxsw_pci->max_cqe_ver >= MLXSW_PCI_CQE_V2) {
		rx_info.mirror_reason = mlxsw_pci_cqe2_mirror_reason_get(cqe);
		mlxsw_pci_cqe_rdq_md_init(skb, cqe);
	} else if (rx_info.trap_id == MLXSW_TRAP_ID_PKT_SAMPLE &&
		   mlxsw_pci->max_cqe_ver >= MLXSW_PCI_CQE_V2) {
		mlxsw_pci_cqe_rdq_md_tx_port_init(skb, cqe);
	}

	mlxsw_pci_skb_cb_ts_set(mlxsw_pci, skb, cqe_v, cqe);
	mlxsw_skb_cb(skb)->rx_md_info.napi = napi;

	mlxsw_core_skb_receive(mlxsw_pci->core, skb, &rx_info);

out:
	q->producer_counter++;
	return;
}

static char *mlxsw_pci_cq_sw_cqe_get(struct mlxsw_pci_queue *q)
{
	struct mlxsw_pci_queue_elem_info *elem_info;
	char *elem;
	bool owner_bit;

	elem_info = mlxsw_pci_queue_elem_info_consumer_get(q);
	elem = elem_info->elem;
	owner_bit = mlxsw_pci_cqe_owner_get(q->u.cq.v, elem);
	if (mlxsw_pci_elem_hw_owned(q, owner_bit))
		return NULL;
	q->consumer_counter++;
	rmb(); /* make sure we read owned bit before the rest of elem */
	return elem;
}

static bool mlxsw_pci_cq_cqe_to_handle(struct mlxsw_pci_queue *q)
{
	struct mlxsw_pci_queue_elem_info *elem_info;
	bool owner_bit;

	elem_info = mlxsw_pci_queue_elem_info_consumer_get(q);
	owner_bit = mlxsw_pci_cqe_owner_get(q->u.cq.v, elem_info->elem);
	return !mlxsw_pci_elem_hw_owned(q, owner_bit);
}

static int mlxsw_pci_napi_poll_cq_rx(struct napi_struct *napi, int budget)
{
	struct mlxsw_pci_queue *q = container_of(napi, struct mlxsw_pci_queue,
						 u.cq.napi);
	struct mlxsw_pci_queue *rdq = q->u.cq.dq;
	struct mlxsw_pci *mlxsw_pci = q->pci;
	int work_done = 0;
	char *cqe;

	/* If the budget is 0, Rx processing should be skipped. */
	if (unlikely(!budget))
		return 0;

	while ((cqe = mlxsw_pci_cq_sw_cqe_get(q))) {
		u16 wqe_counter = mlxsw_pci_cqe_wqe_counter_get(cqe);
		u8 sendq = mlxsw_pci_cqe_sr_get(q->u.cq.v, cqe);
		u8 dqn = mlxsw_pci_cqe_dqn_get(q->u.cq.v, cqe);

		if (unlikely(sendq)) {
			WARN_ON_ONCE(1);
			continue;
		}

		if (unlikely(dqn != rdq->num)) {
			WARN_ON_ONCE(1);
			continue;
		}

		mlxsw_pci_cqe_rdq_handle(mlxsw_pci, napi, rdq,
					 wqe_counter, q->u.cq.v, cqe);

		if (++work_done == budget)
			break;
	}

	mlxsw_pci_queue_doorbell_consumer_ring(mlxsw_pci, q);
	mlxsw_pci_queue_doorbell_producer_ring(mlxsw_pci, rdq);

	if (work_done < budget)
		goto processing_completed;

	/* The driver still has outstanding work to do, budget was exhausted.
	 * Return exactly budget. In that case, the NAPI instance will be polled
	 * again.
	 */
	if (mlxsw_pci_cq_cqe_to_handle(q))
		goto out;

	/* The driver processed all the completions and handled exactly
	 * 'budget'. Return 'budget - 1' to distinguish from the case that
	 * driver still has completions to handle.
	 */
	if (work_done == budget)
		work_done--;

processing_completed:
	if (napi_complete_done(napi, work_done))
		mlxsw_pci_queue_doorbell_arm_consumer_ring(mlxsw_pci, q);
out:
	return work_done;
}

static int mlxsw_pci_napi_poll_cq_tx(struct napi_struct *napi, int budget)
{
	struct mlxsw_pci_queue *q = container_of(napi, struct mlxsw_pci_queue,
						 u.cq.napi);
	struct mlxsw_pci_queue *sdq = q->u.cq.dq;
	struct mlxsw_pci *mlxsw_pci = q->pci;
	int work_done = 0;
	char *cqe;

	while ((cqe = mlxsw_pci_cq_sw_cqe_get(q))) {
		u16 wqe_counter = mlxsw_pci_cqe_wqe_counter_get(cqe);
		u8 sendq = mlxsw_pci_cqe_sr_get(q->u.cq.v, cqe);
		u8 dqn = mlxsw_pci_cqe_dqn_get(q->u.cq.v, cqe);
		char ncqe[MLXSW_PCI_CQE_SIZE_MAX];

		if (unlikely(!sendq)) {
			WARN_ON_ONCE(1);
			continue;
		}

		if (unlikely(dqn != sdq->num)) {
			WARN_ON_ONCE(1);
			continue;
		}

		memcpy(ncqe, cqe, q->elem_size);
		mlxsw_pci_queue_doorbell_consumer_ring(mlxsw_pci, q);

		mlxsw_pci_cqe_sdq_handle(mlxsw_pci, sdq,
					 wqe_counter, q->u.cq.v, ncqe, budget);

		work_done++;
	}

	/* If the budget is 0 napi_complete_done() should never be called. */
	if (unlikely(!budget))
		goto processing_completed;

	work_done = min(work_done, budget - 1);
	if (unlikely(!napi_complete_done(napi, work_done)))
		goto out;

processing_completed:
	mlxsw_pci_queue_doorbell_arm_consumer_ring(mlxsw_pci, q);
out:
	return work_done;
}

static enum mlxsw_pci_cq_type
mlxsw_pci_cq_type(const struct mlxsw_pci *mlxsw_pci,
		  const struct mlxsw_pci_queue *q)
{
	/* Each CQ is mapped to one DQ. The first 'num_sdqs' queues are used
	 * for SDQs and the rest are used for RDQs.
	 */
	if (q->num < mlxsw_pci->num_sdqs)
		return MLXSW_PCI_CQ_SDQ;

	return MLXSW_PCI_CQ_RDQ;
}

static void mlxsw_pci_cq_napi_setup(struct mlxsw_pci_queue *q,
				    enum mlxsw_pci_cq_type cq_type)
{
	struct mlxsw_pci *mlxsw_pci = q->pci;

	switch (cq_type) {
	case MLXSW_PCI_CQ_SDQ:
		netif_napi_add(mlxsw_pci->napi_dev_tx, &q->u.cq.napi,
			       mlxsw_pci_napi_poll_cq_tx);
		break;
	case MLXSW_PCI_CQ_RDQ:
		netif_napi_add(mlxsw_pci->napi_dev_rx, &q->u.cq.napi,
			       mlxsw_pci_napi_poll_cq_rx);
		break;
	}
}

static void mlxsw_pci_cq_napi_teardown(struct mlxsw_pci_queue *q)
{
	netif_napi_del(&q->u.cq.napi);
}

static int mlxsw_pci_cq_page_pool_init(struct mlxsw_pci_queue *q,
				       enum mlxsw_pci_cq_type cq_type)
{
	struct page_pool_params pp_params = {};
	struct mlxsw_pci *mlxsw_pci = q->pci;
	struct page_pool *page_pool;

	if (cq_type != MLXSW_PCI_CQ_RDQ)
		return 0;

	pp_params.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV;
	pp_params.pool_size = MLXSW_PCI_WQE_COUNT * mlxsw_pci->num_sg_entries;
	pp_params.nid = dev_to_node(&mlxsw_pci->pdev->dev);
	pp_params.dev = &mlxsw_pci->pdev->dev;
	pp_params.napi = &q->u.cq.napi;
	pp_params.dma_dir = DMA_FROM_DEVICE;
	pp_params.max_len = PAGE_SIZE;

	page_pool = page_pool_create(&pp_params);
	if (IS_ERR(page_pool))
		return PTR_ERR(page_pool);

	q->u.cq.page_pool = page_pool;
	return 0;
}

static void mlxsw_pci_cq_page_pool_fini(struct mlxsw_pci_queue *q,
					enum mlxsw_pci_cq_type cq_type)
{
	if (cq_type != MLXSW_PCI_CQ_RDQ)
		return;

	page_pool_destroy(q->u.cq.page_pool);
}

static int mlxsw_pci_cq_init(struct mlxsw_pci *mlxsw_pci, char *mbox,
			     struct mlxsw_pci_queue *q)
{
	enum mlxsw_pci_cq_type cq_type = mlxsw_pci_cq_type(mlxsw_pci, q);
	int i;
	int err;

	q->consumer_counter = 0;

	for (i = 0; i < q->count; i++) {
		char *elem = mlxsw_pci_queue_elem_get(q, i);

		mlxsw_pci_cqe_owner_set(q->u.cq.v, elem, 1);
	}

	if (q->u.cq.v == MLXSW_PCI_CQE_V1)
		mlxsw_cmd_mbox_sw2hw_cq_cqe_ver_set(mbox,
				MLXSW_CMD_MBOX_SW2HW_CQ_CQE_VER_1);
	else if (q->u.cq.v == MLXSW_PCI_CQE_V2)
		mlxsw_cmd_mbox_sw2hw_cq_cqe_ver_set(mbox,
				MLXSW_CMD_MBOX_SW2HW_CQ_CQE_VER_2);

	mlxsw_cmd_mbox_sw2hw_cq_c_eqn_set(mbox, MLXSW_PCI_EQ_COMP_NUM);
	mlxsw_cmd_mbox_sw2hw_cq_st_set(mbox, 0);
	mlxsw_cmd_mbox_sw2hw_cq_log_cq_size_set(mbox, ilog2(q->count));
	for (i = 0; i < MLXSW_PCI_AQ_PAGES; i++) {
		dma_addr_t mapaddr = __mlxsw_pci_queue_page_get(q, i);

		mlxsw_cmd_mbox_sw2hw_cq_pa_set(mbox, i, mapaddr);
	}
	err = mlxsw_cmd_sw2hw_cq(mlxsw_pci->core, mbox, q->num);
	if (err)
		return err;
	mlxsw_pci_cq_napi_setup(q, cq_type);

	err = mlxsw_pci_cq_page_pool_init(q, cq_type);
	if (err)
		goto err_page_pool_init;

	napi_enable(&q->u.cq.napi);
	mlxsw_pci_queue_doorbell_consumer_ring(mlxsw_pci, q);
	mlxsw_pci_queue_doorbell_arm_consumer_ring(mlxsw_pci, q);
	return 0;

err_page_pool_init:
	mlxsw_pci_cq_napi_teardown(q);
	return err;
}

static void mlxsw_pci_cq_fini(struct mlxsw_pci *mlxsw_pci,
			      struct mlxsw_pci_queue *q)
{
	enum mlxsw_pci_cq_type cq_type = mlxsw_pci_cq_type(mlxsw_pci, q);

	napi_disable(&q->u.cq.napi);
	mlxsw_pci_cq_page_pool_fini(q, cq_type);
	mlxsw_pci_cq_napi_teardown(q);
	mlxsw_cmd_hw2sw_cq(mlxsw_pci->core, q->num);
}

static u16 mlxsw_pci_cq_elem_count(const struct mlxsw_pci_queue *q)
{
	return q->u.cq.v == MLXSW_PCI_CQE_V2 ? MLXSW_PCI_CQE2_COUNT :
					     MLXSW_PCI_CQE01_COUNT;
}

static u8 mlxsw_pci_cq_elem_size(const struct mlxsw_pci_queue *q)
{
	return q->u.cq.v == MLXSW_PCI_CQE_V2 ? MLXSW_PCI_CQE2_SIZE :
					       MLXSW_PCI_CQE01_SIZE;
}

static char *mlxsw_pci_eq_sw_eqe_get(struct mlxsw_pci_queue *q)
{
	struct mlxsw_pci_queue_elem_info *elem_info;
	char *elem;
	bool owner_bit;

	elem_info = mlxsw_pci_queue_elem_info_consumer_get(q);
	elem = elem_info->elem;
	owner_bit = mlxsw_pci_eqe_owner_get(elem);
	if (mlxsw_pci_elem_hw_owned(q, owner_bit))
		return NULL;
	q->consumer_counter++;
	rmb(); /* make sure we read owned bit before the rest of elem */
	return elem;
}

static void mlxsw_pci_eq_tasklet(struct tasklet_struct *t)
{
	unsigned long active_cqns[BITS_TO_LONGS(MLXSW_PCI_CQS_MAX)];
	struct mlxsw_pci_queue *q = from_tasklet(q, t, u.eq.tasklet);
	struct mlxsw_pci *mlxsw_pci = q->pci;
	int credits = q->count >> 1;
	u8 cqn, cq_count;
	int items = 0;
	char *eqe;

	memset(&active_cqns, 0, sizeof(active_cqns));

	while ((eqe = mlxsw_pci_eq_sw_eqe_get(q))) {
		cqn = mlxsw_pci_eqe_cqn_get(eqe);
		set_bit(cqn, active_cqns);

		if (++items == credits)
			break;
	}

	if (!items)
		return;

	mlxsw_pci_queue_doorbell_consumer_ring(mlxsw_pci, q);
	mlxsw_pci_queue_doorbell_arm_consumer_ring(mlxsw_pci, q);

	cq_count = mlxsw_pci->num_cqs;
	for_each_set_bit(cqn, active_cqns, cq_count) {
		q = mlxsw_pci_cq_get(mlxsw_pci, cqn);
		napi_schedule(&q->u.cq.napi);
	}
}

static int mlxsw_pci_eq_init(struct mlxsw_pci *mlxsw_pci, char *mbox,
			     struct mlxsw_pci_queue *q)
{
	int i;
	int err;

	/* We expect to initialize only one EQ, which gets num=0 as it is
	 * located at index zero. We use the EQ as EQ1, so set the number for
	 * future use.
	 */
	WARN_ON_ONCE(q->num);
	q->num = MLXSW_PCI_EQ_COMP_NUM;

	q->consumer_counter = 0;

	for (i = 0; i < q->count; i++) {
		char *elem = mlxsw_pci_queue_elem_get(q, i);

		mlxsw_pci_eqe_owner_set(elem, 1);
	}

	mlxsw_cmd_mbox_sw2hw_eq_int_msix_set(mbox, 1); /* MSI-X used */
	mlxsw_cmd_mbox_sw2hw_eq_st_set(mbox, 1); /* armed */
	mlxsw_cmd_mbox_sw2hw_eq_log_eq_size_set(mbox, ilog2(q->count));
	for (i = 0; i < MLXSW_PCI_AQ_PAGES; i++) {
		dma_addr_t mapaddr = __mlxsw_pci_queue_page_get(q, i);

		mlxsw_cmd_mbox_sw2hw_eq_pa_set(mbox, i, mapaddr);
	}
	err = mlxsw_cmd_sw2hw_eq(mlxsw_pci->core, mbox, q->num);
	if (err)
		return err;
	tasklet_setup(&q->u.eq.tasklet, mlxsw_pci_eq_tasklet);
	mlxsw_pci_queue_doorbell_consumer_ring(mlxsw_pci, q);
	mlxsw_pci_queue_doorbell_arm_consumer_ring(mlxsw_pci, q);
	return 0;
}

static void mlxsw_pci_eq_fini(struct mlxsw_pci *mlxsw_pci,
			      struct mlxsw_pci_queue *q)
{
	mlxsw_cmd_hw2sw_eq(mlxsw_pci->core, q->num);
}

struct mlxsw_pci_queue_ops {
	const char *name;
	enum mlxsw_pci_queue_type type;
	void (*pre_init)(struct mlxsw_pci *mlxsw_pci,
			 struct mlxsw_pci_queue *q);
	int (*init)(struct mlxsw_pci *mlxsw_pci, char *mbox,
		    struct mlxsw_pci_queue *q);
	void (*fini)(struct mlxsw_pci *mlxsw_pci,
		     struct mlxsw_pci_queue *q);
	u16 (*elem_count_f)(const struct mlxsw_pci_queue *q);
	u8 (*elem_size_f)(const struct mlxsw_pci_queue *q);
	u16 elem_count;
	u8 elem_size;
};

static const struct mlxsw_pci_queue_ops mlxsw_pci_sdq_ops = {
	.type		= MLXSW_PCI_QUEUE_TYPE_SDQ,
	.init		= mlxsw_pci_sdq_init,
	.fini		= mlxsw_pci_sdq_fini,
	.elem_count	= MLXSW_PCI_WQE_COUNT,
	.elem_size	= MLXSW_PCI_WQE_SIZE,
};

static const struct mlxsw_pci_queue_ops mlxsw_pci_rdq_ops = {
	.type		= MLXSW_PCI_QUEUE_TYPE_RDQ,
	.init		= mlxsw_pci_rdq_init,
	.fini		= mlxsw_pci_rdq_fini,
	.elem_count	= MLXSW_PCI_WQE_COUNT,
	.elem_size	= MLXSW_PCI_WQE_SIZE
};

static const struct mlxsw_pci_queue_ops mlxsw_pci_cq_ops = {
	.type		= MLXSW_PCI_QUEUE_TYPE_CQ,
	.pre_init	= mlxsw_pci_cq_pre_init,
	.init		= mlxsw_pci_cq_init,
	.fini		= mlxsw_pci_cq_fini,
	.elem_count_f	= mlxsw_pci_cq_elem_count,
	.elem_size_f	= mlxsw_pci_cq_elem_size
};

static const struct mlxsw_pci_queue_ops mlxsw_pci_eq_ops = {
	.type		= MLXSW_PCI_QUEUE_TYPE_EQ,
	.init		= mlxsw_pci_eq_init,
	.fini		= mlxsw_pci_eq_fini,
	.elem_count	= MLXSW_PCI_EQE_COUNT,
	.elem_size	= MLXSW_PCI_EQE_SIZE
};

static int mlxsw_pci_queue_init(struct mlxsw_pci *mlxsw_pci, char *mbox,
				const struct mlxsw_pci_queue_ops *q_ops,
				struct mlxsw_pci_queue *q, u8 q_num)
{
	struct mlxsw_pci_mem_item *mem_item = &q->mem_item;
	int i;
	int err;

	q->num = q_num;
	if (q_ops->pre_init)
		q_ops->pre_init(mlxsw_pci, q);

	spin_lock_init(&q->lock);
	q->count = q_ops->elem_count_f ? q_ops->elem_count_f(q) :
					 q_ops->elem_count;
	q->elem_size = q_ops->elem_size_f ? q_ops->elem_size_f(q) :
					    q_ops->elem_size;
	q->type = q_ops->type;
	q->pci = mlxsw_pci;

	mem_item->size = MLXSW_PCI_AQ_SIZE;
	mem_item->buf = dma_alloc_coherent(&mlxsw_pci->pdev->dev,
					   mem_item->size, &mem_item->mapaddr,
					   GFP_KERNEL);
	if (!mem_item->buf)
		return -ENOMEM;

	q->elem_info = kcalloc(q->count, sizeof(*q->elem_info), GFP_KERNEL);
	if (!q->elem_info) {
		err = -ENOMEM;
		goto err_elem_info_alloc;
	}

	/* Initialize dma mapped elements info elem_info for
	 * future easy access.
	 */
	for (i = 0; i < q->count; i++) {
		struct mlxsw_pci_queue_elem_info *elem_info;

		elem_info = mlxsw_pci_queue_elem_info_get(q, i);
		elem_info->elem =
			__mlxsw_pci_queue_elem_get(q, q->elem_size, i);
	}

	mlxsw_cmd_mbox_zero(mbox);
	err = q_ops->init(mlxsw_pci, mbox, q);
	if (err)
		goto err_q_ops_init;
	return 0;

err_q_ops_init:
	kfree(q->elem_info);
err_elem_info_alloc:
	dma_free_coherent(&mlxsw_pci->pdev->dev, mem_item->size,
			  mem_item->buf, mem_item->mapaddr);
	return err;
}

static void mlxsw_pci_queue_fini(struct mlxsw_pci *mlxsw_pci,
				 const struct mlxsw_pci_queue_ops *q_ops,
				 struct mlxsw_pci_queue *q)
{
	struct mlxsw_pci_mem_item *mem_item = &q->mem_item;

	q_ops->fini(mlxsw_pci, q);
	kfree(q->elem_info);
	dma_free_coherent(&mlxsw_pci->pdev->dev, mem_item->size,
			  mem_item->buf, mem_item->mapaddr);
}

static int mlxsw_pci_queue_group_init(struct mlxsw_pci *mlxsw_pci, char *mbox,
				      const struct mlxsw_pci_queue_ops *q_ops,
				      u8 num_qs)
{
	struct mlxsw_pci_queue_type_group *queue_group;
	int i;
	int err;

	queue_group = mlxsw_pci_queue_type_group_get(mlxsw_pci, q_ops->type);
	queue_group->q = kcalloc(num_qs, sizeof(*queue_group->q), GFP_KERNEL);
	if (!queue_group->q)
		return -ENOMEM;

	for (i = 0; i < num_qs; i++) {
		err = mlxsw_pci_queue_init(mlxsw_pci, mbox, q_ops,
					   &queue_group->q[i], i);
		if (err)
			goto err_queue_init;
	}
	queue_group->count = num_qs;

	return 0;

err_queue_init:
	for (i--; i >= 0; i--)
		mlxsw_pci_queue_fini(mlxsw_pci, q_ops, &queue_group->q[i]);
	kfree(queue_group->q);
	return err;
}

static void mlxsw_pci_queue_group_fini(struct mlxsw_pci *mlxsw_pci,
				       const struct mlxsw_pci_queue_ops *q_ops)
{
	struct mlxsw_pci_queue_type_group *queue_group;
	int i;

	queue_group = mlxsw_pci_queue_type_group_get(mlxsw_pci, q_ops->type);
	for (i = 0; i < queue_group->count; i++)
		mlxsw_pci_queue_fini(mlxsw_pci, q_ops, &queue_group->q[i]);
	kfree(queue_group->q);
}

static int mlxsw_pci_aqs_init(struct mlxsw_pci *mlxsw_pci, char *mbox)
{
	struct pci_dev *pdev = mlxsw_pci->pdev;
	u8 num_sdqs;
	u8 sdq_log2sz;
	u8 num_rdqs;
	u8 rdq_log2sz;
	u8 num_cqs;
	u8 cq_log2sz;
	u8 cqv2_log2sz;
	u8 num_eqs;
	u8 eq_log2sz;
	int err;

	mlxsw_cmd_mbox_zero(mbox);
	err = mlxsw_cmd_query_aq_cap(mlxsw_pci->core, mbox);
	if (err)
		return err;

	num_sdqs = mlxsw_cmd_mbox_query_aq_cap_max_num_sdqs_get(mbox);
	sdq_log2sz = mlxsw_cmd_mbox_query_aq_cap_log_max_sdq_sz_get(mbox);
	num_rdqs = mlxsw_cmd_mbox_query_aq_cap_max_num_rdqs_get(mbox);
	rdq_log2sz = mlxsw_cmd_mbox_query_aq_cap_log_max_rdq_sz_get(mbox);
	num_cqs = mlxsw_cmd_mbox_query_aq_cap_max_num_cqs_get(mbox);
	cq_log2sz = mlxsw_cmd_mbox_query_aq_cap_log_max_cq_sz_get(mbox);
	cqv2_log2sz = mlxsw_cmd_mbox_query_aq_cap_log_max_cqv2_sz_get(mbox);
	num_eqs = mlxsw_cmd_mbox_query_aq_cap_max_num_eqs_get(mbox);
	eq_log2sz = mlxsw_cmd_mbox_query_aq_cap_log_max_eq_sz_get(mbox);

	if (num_sdqs + num_rdqs > num_cqs ||
	    num_sdqs < MLXSW_PCI_SDQS_MIN ||
	    num_cqs > MLXSW_PCI_CQS_MAX || num_eqs != MLXSW_PCI_EQS_MAX) {
		dev_err(&pdev->dev, "Unsupported number of queues\n");
		return -EINVAL;
	}

	if ((1 << sdq_log2sz != MLXSW_PCI_WQE_COUNT) ||
	    (1 << rdq_log2sz != MLXSW_PCI_WQE_COUNT) ||
	    (1 << cq_log2sz != MLXSW_PCI_CQE01_COUNT) ||
	    (mlxsw_pci->max_cqe_ver == MLXSW_PCI_CQE_V2 &&
	     (1 << cqv2_log2sz != MLXSW_PCI_CQE2_COUNT)) ||
	    (1 << eq_log2sz != MLXSW_PCI_EQE_COUNT)) {
		dev_err(&pdev->dev, "Unsupported number of async queue descriptors\n");
		return -EINVAL;
	}

	mlxsw_pci->num_cqs = num_cqs;
	mlxsw_pci->num_sdqs = num_sdqs;

	err = mlxsw_pci_queue_group_init(mlxsw_pci, mbox, &mlxsw_pci_eq_ops,
					 MLXSW_PCI_EQS_COUNT);
	if (err) {
		dev_err(&pdev->dev, "Failed to initialize event queues\n");
		return err;
	}

	err = mlxsw_pci_queue_group_init(mlxsw_pci, mbox, &mlxsw_pci_cq_ops,
					 num_cqs);
	if (err) {
		dev_err(&pdev->dev, "Failed to initialize completion queues\n");
		goto err_cqs_init;
	}

	err = mlxsw_pci_queue_group_init(mlxsw_pci, mbox, &mlxsw_pci_sdq_ops,
					 num_sdqs);
	if (err) {
		dev_err(&pdev->dev, "Failed to initialize send descriptor queues\n");
		goto err_sdqs_init;
	}

	err = mlxsw_pci_queue_group_init(mlxsw_pci, mbox, &mlxsw_pci_rdq_ops,
					 num_rdqs);
	if (err) {
		dev_err(&pdev->dev, "Failed to initialize receive descriptor queues\n");
		goto err_rdqs_init;
	}

	return 0;

err_rdqs_init:
	mlxsw_pci_queue_group_fini(mlxsw_pci, &mlxsw_pci_sdq_ops);
err_sdqs_init:
	mlxsw_pci_queue_group_fini(mlxsw_pci, &mlxsw_pci_cq_ops);
err_cqs_init:
	mlxsw_pci_queue_group_fini(mlxsw_pci, &mlxsw_pci_eq_ops);
	return err;
}

static void mlxsw_pci_aqs_fini(struct mlxsw_pci *mlxsw_pci)
{
	mlxsw_pci_queue_group_fini(mlxsw_pci, &mlxsw_pci_rdq_ops);
	mlxsw_pci_queue_group_fini(mlxsw_pci, &mlxsw_pci_sdq_ops);
	mlxsw_pci_queue_group_fini(mlxsw_pci, &mlxsw_pci_cq_ops);
	mlxsw_pci_queue_group_fini(mlxsw_pci, &mlxsw_pci_eq_ops);
}

static void
mlxsw_pci_config_profile_swid_config(struct mlxsw_pci *mlxsw_pci,
				     char *mbox, int index,
				     const struct mlxsw_swid_config *swid)
{
	u8 mask = 0;

	if (swid->used_type) {
		mlxsw_cmd_mbox_config_profile_swid_config_type_set(
			mbox, index, swid->type);
		mask |= 1;
	}
	if (swid->used_properties) {
		mlxsw_cmd_mbox_config_profile_swid_config_properties_set(
			mbox, index, swid->properties);
		mask |= 2;
	}
	mlxsw_cmd_mbox_config_profile_swid_config_mask_set(mbox, index, mask);
}

static int
mlxsw_pci_profile_get_kvd_sizes(const struct mlxsw_pci *mlxsw_pci,
				const struct mlxsw_config_profile *profile,
				struct mlxsw_res *res)
{
	u64 single_size, double_size, linear_size;
	int err;

	err = mlxsw_core_kvd_sizes_get(mlxsw_pci->core, profile,
				       &single_size, &double_size,
				       &linear_size);
	if (err)
		return err;

	MLXSW_RES_SET(res, KVD_SINGLE_SIZE, single_size);
	MLXSW_RES_SET(res, KVD_DOUBLE_SIZE, double_size);
	MLXSW_RES_SET(res, KVD_LINEAR_SIZE, linear_size);

	return 0;
}

static int mlxsw_pci_config_profile(struct mlxsw_pci *mlxsw_pci, char *mbox,
				    const struct mlxsw_config_profile *profile,
				    struct mlxsw_res *res)
{
	int i;
	int err;

	mlxsw_cmd_mbox_zero(mbox);

	if (profile->used_max_vepa_channels) {
		mlxsw_cmd_mbox_config_profile_set_max_vepa_channels_set(
			mbox, 1);
		mlxsw_cmd_mbox_config_profile_max_vepa_channels_set(
			mbox, profile->max_vepa_channels);
	}
	if (profile->used_max_lag) {
		mlxsw_cmd_mbox_config_profile_set_max_lag_set(mbox, 1);
		mlxsw_cmd_mbox_config_profile_max_lag_set(mbox,
							  profile->max_lag);
	}
	if (profile->used_max_mid) {
		mlxsw_cmd_mbox_config_profile_set_max_mid_set(
			mbox, 1);
		mlxsw_cmd_mbox_config_profile_max_mid_set(
			mbox, profile->max_mid);
	}
	if (profile->used_max_pgt) {
		mlxsw_cmd_mbox_config_profile_set_max_pgt_set(
			mbox, 1);
		mlxsw_cmd_mbox_config_profile_max_pgt_set(
			mbox, profile->max_pgt);
	}
	if (profile->used_max_system_port) {
		mlxsw_cmd_mbox_config_profile_set_max_system_port_set(
			mbox, 1);
		mlxsw_cmd_mbox_config_profile_max_system_port_set(
			mbox, profile->max_system_port);
	}
	if (profile->used_max_vlan_groups) {
		mlxsw_cmd_mbox_config_profile_set_max_vlan_groups_set(
			mbox, 1);
		mlxsw_cmd_mbox_config_profile_max_vlan_groups_set(
			mbox, profile->max_vlan_groups);
	}
	if (profile->used_max_regions) {
		mlxsw_cmd_mbox_config_profile_set_max_regions_set(
			mbox, 1);
		mlxsw_cmd_mbox_config_profile_max_regions_set(
			mbox, profile->max_regions);
	}
	if (profile->used_flood_tables) {
		mlxsw_cmd_mbox_config_profile_set_flood_tables_set(
			mbox, 1);
		mlxsw_cmd_mbox_config_profile_max_flood_tables_set(
			mbox, profile->max_flood_tables);
		mlxsw_cmd_mbox_config_profile_max_vid_flood_tables_set(
			mbox, profile->max_vid_flood_tables);
		mlxsw_cmd_mbox_config_profile_max_fid_offset_flood_tables_set(
			mbox, profile->max_fid_offset_flood_tables);
		mlxsw_cmd_mbox_config_profile_fid_offset_flood_table_size_set(
			mbox, profile->fid_offset_flood_table_size);
		mlxsw_cmd_mbox_config_profile_max_fid_flood_tables_set(
			mbox, profile->max_fid_flood_tables);
		mlxsw_cmd_mbox_config_profile_fid_flood_table_size_set(
			mbox, profile->fid_flood_table_size);
	}
	if (profile->flood_mode_prefer_cff && mlxsw_pci->cff_support) {
		enum mlxsw_cmd_mbox_config_profile_flood_mode flood_mode =
			MLXSW_CMD_MBOX_CONFIG_PROFILE_FLOOD_MODE_CFF;

		mlxsw_cmd_mbox_config_profile_set_flood_mode_set(mbox, 1);
		mlxsw_cmd_mbox_config_profile_flood_mode_set(mbox, flood_mode);
		mlxsw_pci->flood_mode = flood_mode;
	} else if (profile->used_flood_mode) {
		mlxsw_cmd_mbox_config_profile_set_flood_mode_set(
			mbox, 1);
		mlxsw_cmd_mbox_config_profile_flood_mode_set(
			mbox, profile->flood_mode);
		mlxsw_pci->flood_mode = profile->flood_mode;
	} else {
		WARN_ON(1);
		return -EINVAL;
	}
	if (profile->used_max_ib_mc) {
		mlxsw_cmd_mbox_config_profile_set_max_ib_mc_set(
			mbox, 1);
		mlxsw_cmd_mbox_config_profile_max_ib_mc_set(
			mbox, profile->max_ib_mc);
	}
	if (profile->used_max_pkey) {
		mlxsw_cmd_mbox_config_profile_set_max_pkey_set(
			mbox, 1);
		mlxsw_cmd_mbox_config_profile_max_pkey_set(
			mbox, profile->max_pkey);
	}
	if (profile->used_ar_sec) {
		mlxsw_cmd_mbox_config_profile_set_ar_sec_set(
			mbox, 1);
		mlxsw_cmd_mbox_config_profile_ar_sec_set(
			mbox, profile->ar_sec);
	}
	if (profile->used_adaptive_routing_group_cap) {
		mlxsw_cmd_mbox_config_profile_set_adaptive_routing_group_cap_set(
			mbox, 1);
		mlxsw_cmd_mbox_config_profile_adaptive_routing_group_cap_set(
			mbox, profile->adaptive_routing_group_cap);
	}
	if (profile->used_ubridge) {
		mlxsw_cmd_mbox_config_profile_set_ubridge_set(mbox, 1);
		mlxsw_cmd_mbox_config_profile_ubridge_set(mbox,
							  profile->ubridge);
	}
	if (profile->used_kvd_sizes && MLXSW_RES_VALID(res, KVD_SIZE)) {
		err = mlxsw_pci_profile_get_kvd_sizes(mlxsw_pci, profile, res);
		if (err)
			return err;

		mlxsw_cmd_mbox_config_profile_set_kvd_linear_size_set(mbox, 1);
		mlxsw_cmd_mbox_config_profile_kvd_linear_size_set(mbox,
					MLXSW_RES_GET(res, KVD_LINEAR_SIZE));
		mlxsw_cmd_mbox_config_profile_set_kvd_hash_single_size_set(mbox,
									   1);
		mlxsw_cmd_mbox_config_profile_kvd_hash_single_size_set(mbox,
					MLXSW_RES_GET(res, KVD_SINGLE_SIZE));
		mlxsw_cmd_mbox_config_profile_set_kvd_hash_double_size_set(
								mbox, 1);
		mlxsw_cmd_mbox_config_profile_kvd_hash_double_size_set(mbox,
					MLXSW_RES_GET(res, KVD_DOUBLE_SIZE));
	}

	for (i = 0; i < MLXSW_CONFIG_PROFILE_SWID_COUNT; i++)
		mlxsw_pci_config_profile_swid_config(mlxsw_pci, mbox, i,
						     &profile->swid_config[i]);

	if (mlxsw_pci->max_cqe_ver > MLXSW_PCI_CQE_V0) {
		mlxsw_cmd_mbox_config_profile_set_cqe_version_set(mbox, 1);
		mlxsw_cmd_mbox_config_profile_cqe_version_set(mbox, 1);
	}

	if (profile->used_cqe_time_stamp_type) {
		mlxsw_cmd_mbox_config_profile_set_cqe_time_stamp_type_set(mbox,
									  1);
		mlxsw_cmd_mbox_config_profile_cqe_time_stamp_type_set(mbox,
					profile->cqe_time_stamp_type);
	}

	if (profile->lag_mode_prefer_sw && mlxsw_pci->lag_mode_support) {
		enum mlxsw_cmd_mbox_config_profile_lag_mode lag_mode =
			MLXSW_CMD_MBOX_CONFIG_PROFILE_LAG_MODE_SW;

		mlxsw_cmd_mbox_config_profile_set_lag_mode_set(mbox, 1);
		mlxsw_cmd_mbox_config_profile_lag_mode_set(mbox, lag_mode);
		mlxsw_pci->lag_mode = lag_mode;
	} else {
		mlxsw_pci->lag_mode = MLXSW_CMD_MBOX_CONFIG_PROFILE_LAG_MODE_FW;
	}
	return mlxsw_cmd_config_profile_set(mlxsw_pci->core, mbox);
}

static int mlxsw_pci_boardinfo(struct mlxsw_pci *mlxsw_pci, char *mbox)
{
	struct mlxsw_bus_info *bus_info = &mlxsw_pci->bus_info;
	int err;

	mlxsw_cmd_mbox_zero(mbox);
	err = mlxsw_cmd_boardinfo(mlxsw_pci->core, mbox);
	if (err)
		return err;
	mlxsw_cmd_mbox_boardinfo_vsd_memcpy_from(mbox, bus_info->vsd);
	mlxsw_cmd_mbox_boardinfo_psid_memcpy_from(mbox, bus_info->psid);
	return 0;
}

static int mlxsw_pci_fw_area_init(struct mlxsw_pci *mlxsw_pci, char *mbox,
				  u16 num_pages)
{
	struct mlxsw_pci_mem_item *mem_item;
	int nent = 0;
	int i;
	int err;

	mlxsw_pci->fw_area.items = kcalloc(num_pages, sizeof(*mem_item),
					   GFP_KERNEL);
	if (!mlxsw_pci->fw_area.items)
		return -ENOMEM;
	mlxsw_pci->fw_area.count = num_pages;

	mlxsw_cmd_mbox_zero(mbox);
	for (i = 0; i < num_pages; i++) {
		mem_item = &mlxsw_pci->fw_area.items[i];

		mem_item->size = MLXSW_PCI_PAGE_SIZE;
		mem_item->buf = dma_alloc_coherent(&mlxsw_pci->pdev->dev,
						   mem_item->size,
						   &mem_item->mapaddr, GFP_KERNEL);
		if (!mem_item->buf) {
			err = -ENOMEM;
			goto err_alloc;
		}
		mlxsw_cmd_mbox_map_fa_pa_set(mbox, nent, mem_item->mapaddr);
		mlxsw_cmd_mbox_map_fa_log2size_set(mbox, nent, 0); /* 1 page */
		if (++nent == MLXSW_CMD_MAP_FA_VPM_ENTRIES_MAX) {
			err = mlxsw_cmd_map_fa(mlxsw_pci->core, mbox, nent);
			if (err)
				goto err_cmd_map_fa;
			nent = 0;
			mlxsw_cmd_mbox_zero(mbox);
		}
	}

	if (nent) {
		err = mlxsw_cmd_map_fa(mlxsw_pci->core, mbox, nent);
		if (err)
			goto err_cmd_map_fa;
	}

	return 0;

err_cmd_map_fa:
err_alloc:
	for (i--; i >= 0; i--) {
		mem_item = &mlxsw_pci->fw_area.items[i];

		dma_free_coherent(&mlxsw_pci->pdev->dev, mem_item->size,
				  mem_item->buf, mem_item->mapaddr);
	}
	kfree(mlxsw_pci->fw_area.items);
	return err;
}

static void mlxsw_pci_fw_area_fini(struct mlxsw_pci *mlxsw_pci)
{
	struct mlxsw_pci_mem_item *mem_item;
	int i;

	mlxsw_cmd_unmap_fa(mlxsw_pci->core);

	for (i = 0; i < mlxsw_pci->fw_area.count; i++) {
		mem_item = &mlxsw_pci->fw_area.items[i];

		dma_free_coherent(&mlxsw_pci->pdev->dev, mem_item->size,
				  mem_item->buf, mem_item->mapaddr);
	}
	kfree(mlxsw_pci->fw_area.items);
}

static irqreturn_t mlxsw_pci_eq_irq_handler(int irq, void *dev_id)
{
	struct mlxsw_pci *mlxsw_pci = dev_id;
	struct mlxsw_pci_queue *q;

	q = mlxsw_pci_eq_get(mlxsw_pci);
	tasklet_schedule(&q->u.eq.tasklet);
	return IRQ_HANDLED;
}

static int mlxsw_pci_mbox_alloc(struct mlxsw_pci *mlxsw_pci,
				struct mlxsw_pci_mem_item *mbox)
{
	struct pci_dev *pdev = mlxsw_pci->pdev;
	int err = 0;

	mbox->size = MLXSW_CMD_MBOX_SIZE;
	mbox->buf = dma_alloc_coherent(&pdev->dev, MLXSW_CMD_MBOX_SIZE,
				       &mbox->mapaddr, GFP_KERNEL);
	if (!mbox->buf) {
		dev_err(&pdev->dev, "Failed allocating memory for mailbox\n");
		err = -ENOMEM;
	}

	return err;
}

static void mlxsw_pci_mbox_free(struct mlxsw_pci *mlxsw_pci,
				struct mlxsw_pci_mem_item *mbox)
{
	struct pci_dev *pdev = mlxsw_pci->pdev;

	dma_free_coherent(&pdev->dev, MLXSW_CMD_MBOX_SIZE, mbox->buf,
			  mbox->mapaddr);
}

static int mlxsw_pci_sys_ready_wait(struct mlxsw_pci *mlxsw_pci,
				    const struct pci_device_id *id,
				    u32 *p_sys_status)
{
	unsigned long end;
	u32 val;

	/* We must wait for the HW to become responsive. */
	msleep(MLXSW_PCI_SW_RESET_WAIT_MSECS);

	end = jiffies + msecs_to_jiffies(MLXSW_PCI_SW_RESET_TIMEOUT_MSECS);
	do {
		val = mlxsw_pci_read32(mlxsw_pci, FW_READY);
		if ((val & MLXSW_PCI_FW_READY_MASK) == MLXSW_PCI_FW_READY_MAGIC)
			return 0;
		cond_resched();
	} while (time_before(jiffies, end));

	*p_sys_status = val & MLXSW_PCI_FW_READY_MASK;

	return -EBUSY;
}

static int mlxsw_pci_reset_at_pci_disable(struct mlxsw_pci *mlxsw_pci,
					  bool pci_reset_sbr_supported)
{
	struct pci_dev *pdev = mlxsw_pci->pdev;
	char mrsr_pl[MLXSW_REG_MRSR_LEN];
	struct pci_dev *bridge;
	int err;

	if (!pci_reset_sbr_supported) {
		pci_dbg(pdev, "Performing PCI hot reset instead of \"all reset\"\n");
		goto sbr;
	}

	mlxsw_reg_mrsr_pack(mrsr_pl,
			    MLXSW_REG_MRSR_COMMAND_RESET_AT_PCI_DISABLE);
	err = mlxsw_reg_write(mlxsw_pci->core, MLXSW_REG(mrsr), mrsr_pl);
	if (err)
		return err;

sbr:
	device_lock_assert(&pdev->dev);

	bridge = pci_upstream_bridge(pdev);
	if (bridge)
		pci_cfg_access_lock(bridge);
	pci_cfg_access_lock(pdev);
	pci_save_state(pdev);

	err = __pci_reset_function_locked(pdev);
	if (err)
		pci_err(pdev, "PCI function reset failed with %d\n", err);

	pci_restore_state(pdev);
	pci_cfg_access_unlock(pdev);
	if (bridge)
		pci_cfg_access_unlock(bridge);

	return err;
}

static int mlxsw_pci_reset_sw(struct mlxsw_pci *mlxsw_pci)
{
	char mrsr_pl[MLXSW_REG_MRSR_LEN];

	mlxsw_reg_mrsr_pack(mrsr_pl, MLXSW_REG_MRSR_COMMAND_SOFTWARE_RESET);
	return mlxsw_reg_write(mlxsw_pci->core, MLXSW_REG(mrsr), mrsr_pl);
}

static int
mlxsw_pci_reset(struct mlxsw_pci *mlxsw_pci, const struct pci_device_id *id)
{
	struct pci_dev *pdev = mlxsw_pci->pdev;
	bool pci_reset_sbr_supported = false;
	char mcam_pl[MLXSW_REG_MCAM_LEN];
	bool pci_reset_supported = false;
	u32 sys_status;
	int err;

	err = mlxsw_pci_sys_ready_wait(mlxsw_pci, id, &sys_status);
	if (err) {
		dev_err(&pdev->dev, "Failed to reach system ready status before reset. Status is 0x%x\n",
			sys_status);
		return err;
	}

	/* PCI core already issued a PCI reset, do not issue another reset. */
	if (mlxsw_pci->skip_reset)
		return 0;

	mlxsw_reg_mcam_pack(mcam_pl,
			    MLXSW_REG_MCAM_FEATURE_GROUP_ENHANCED_FEATURES);
	err = mlxsw_reg_query(mlxsw_pci->core, MLXSW_REG(mcam), mcam_pl);
	if (!err) {
		mlxsw_reg_mcam_unpack(mcam_pl, MLXSW_REG_MCAM_PCI_RESET,
				      &pci_reset_supported);
		mlxsw_reg_mcam_unpack(mcam_pl, MLXSW_REG_MCAM_PCI_RESET_SBR,
				      &pci_reset_sbr_supported);
	}

	if (pci_reset_supported) {
		pci_dbg(pdev, "Starting PCI reset flow\n");
		err = mlxsw_pci_reset_at_pci_disable(mlxsw_pci,
						     pci_reset_sbr_supported);
	} else {
		pci_dbg(pdev, "Starting software reset flow\n");
		err = mlxsw_pci_reset_sw(mlxsw_pci);
	}
	if (err)
		return err;

	err = mlxsw_pci_sys_ready_wait(mlxsw_pci, id, &sys_status);
	if (err) {
		dev_err(&pdev->dev, "Failed to reach system ready status after reset. Status is 0x%x\n",
			sys_status);
		return err;
	}

	return 0;
}

static int mlxsw_pci_alloc_irq_vectors(struct mlxsw_pci *mlxsw_pci)
{
	int err;

	err = pci_alloc_irq_vectors(mlxsw_pci->pdev, 1, 1, PCI_IRQ_MSIX);
	if (err < 0)
		dev_err(&mlxsw_pci->pdev->dev, "MSI-X init failed\n");
	return err;
}

static void mlxsw_pci_free_irq_vectors(struct mlxsw_pci *mlxsw_pci)
{
	pci_free_irq_vectors(mlxsw_pci->pdev);
}

static void mlxsw_pci_num_sg_entries_set(struct mlxsw_pci *mlxsw_pci)
{
	u8 num_sg_entries;

	num_sg_entries = mlxsw_pci_num_sg_entries_get(MLXSW_PORT_MAX_MTU);
	mlxsw_pci->num_sg_entries = min(num_sg_entries,
					MLXSW_PCI_WQE_SG_ENTRIES);

	WARN_ON(num_sg_entries > MLXSW_PCI_WQE_SG_ENTRIES);
}

static int mlxsw_pci_init(void *bus_priv, struct mlxsw_core *mlxsw_core,
			  const struct mlxsw_config_profile *profile,
			  struct mlxsw_res *res)
{
	struct mlxsw_pci *mlxsw_pci = bus_priv;
	struct pci_dev *pdev = mlxsw_pci->pdev;
	char *mbox;
	u16 num_pages;
	int err;

	mlxsw_pci->core = mlxsw_core;

	mbox = mlxsw_cmd_mbox_alloc();
	if (!mbox)
		return -ENOMEM;

	err = mlxsw_pci_reset(mlxsw_pci, mlxsw_pci->id);
	if (err)
		goto err_reset;

	err = mlxsw_pci_alloc_irq_vectors(mlxsw_pci);
	if (err < 0) {
		dev_err(&pdev->dev, "MSI-X init failed\n");
		goto err_alloc_irq;
	}

	err = mlxsw_cmd_query_fw(mlxsw_core, mbox);
	if (err)
		goto err_query_fw;

	mlxsw_pci->bus_info.fw_rev.major =
		mlxsw_cmd_mbox_query_fw_fw_rev_major_get(mbox);
	mlxsw_pci->bus_info.fw_rev.minor =
		mlxsw_cmd_mbox_query_fw_fw_rev_minor_get(mbox);
	mlxsw_pci->bus_info.fw_rev.subminor =
		mlxsw_cmd_mbox_query_fw_fw_rev_subminor_get(mbox);

	if (mlxsw_cmd_mbox_query_fw_cmd_interface_rev_get(mbox) != 1) {
		dev_err(&pdev->dev, "Unsupported cmd interface revision ID queried from hw\n");
		err = -EINVAL;
		goto err_iface_rev;
	}
	if (mlxsw_cmd_mbox_query_fw_doorbell_page_bar_get(mbox) != 0) {
		dev_err(&pdev->dev, "Unsupported doorbell page bar queried from hw\n");
		err = -EINVAL;
		goto err_doorbell_page_bar;
	}

	mlxsw_pci->doorbell_offset =
		mlxsw_cmd_mbox_query_fw_doorbell_page_offset_get(mbox);

	if (mlxsw_cmd_mbox_query_fw_fr_rn_clk_bar_get(mbox) != 0) {
		dev_err(&pdev->dev, "Unsupported free running clock BAR queried from hw\n");
		err = -EINVAL;
		goto err_fr_rn_clk_bar;
	}

	mlxsw_pci->free_running_clock_offset =
		mlxsw_cmd_mbox_query_fw_free_running_clock_offset_get(mbox);

	if (mlxsw_cmd_mbox_query_fw_utc_sec_bar_get(mbox) != 0) {
		dev_err(&pdev->dev, "Unsupported UTC sec BAR queried from hw\n");
		err = -EINVAL;
		goto err_utc_sec_bar;
	}

	mlxsw_pci->utc_sec_offset =
		mlxsw_cmd_mbox_query_fw_utc_sec_offset_get(mbox);

	if (mlxsw_cmd_mbox_query_fw_utc_nsec_bar_get(mbox) != 0) {
		dev_err(&pdev->dev, "Unsupported UTC nsec BAR queried from hw\n");
		err = -EINVAL;
		goto err_utc_nsec_bar;
	}

	mlxsw_pci->utc_nsec_offset =
		mlxsw_cmd_mbox_query_fw_utc_nsec_offset_get(mbox);

	mlxsw_pci->lag_mode_support =
		mlxsw_cmd_mbox_query_fw_lag_mode_support_get(mbox);
	mlxsw_pci->cff_support =
		mlxsw_cmd_mbox_query_fw_cff_support_get(mbox);

	num_pages = mlxsw_cmd_mbox_query_fw_fw_pages_get(mbox);
	err = mlxsw_pci_fw_area_init(mlxsw_pci, mbox, num_pages);
	if (err)
		goto err_fw_area_init;

	err = mlxsw_pci_boardinfo(mlxsw_pci, mbox);
	if (err)
		goto err_boardinfo;

	err = mlxsw_core_resources_query(mlxsw_core, mbox, res);
	if (err)
		goto err_query_resources;

	if (MLXSW_CORE_RES_VALID(mlxsw_core, CQE_V2) &&
	    MLXSW_CORE_RES_GET(mlxsw_core, CQE_V2))
		mlxsw_pci->max_cqe_ver = MLXSW_PCI_CQE_V2;
	else if (MLXSW_CORE_RES_VALID(mlxsw_core, CQE_V1) &&
		 MLXSW_CORE_RES_GET(mlxsw_core, CQE_V1))
		mlxsw_pci->max_cqe_ver = MLXSW_PCI_CQE_V1;
	else if ((MLXSW_CORE_RES_VALID(mlxsw_core, CQE_V0) &&
		  MLXSW_CORE_RES_GET(mlxsw_core, CQE_V0)) ||
		 !MLXSW_CORE_RES_VALID(mlxsw_core, CQE_V0)) {
		mlxsw_pci->max_cqe_ver = MLXSW_PCI_CQE_V0;
	} else {
		dev_err(&pdev->dev, "Invalid supported CQE version combination reported\n");
		goto err_cqe_v_check;
	}

	err = mlxsw_pci_config_profile(mlxsw_pci, mbox, profile, res);
	if (err)
		goto err_config_profile;

	/* Some resources depend on details of config_profile, such as unified
	 * bridge model. Query the resources again to get correct values.
	 */
	err = mlxsw_core_resources_query(mlxsw_core, mbox, res);
	if (err)
		goto err_requery_resources;

	mlxsw_pci_num_sg_entries_set(mlxsw_pci);

	err = mlxsw_pci_napi_devs_init(mlxsw_pci);
	if (err)
		goto err_napi_devs_init;

	err = mlxsw_pci_aqs_init(mlxsw_pci, mbox);
	if (err)
		goto err_aqs_init;

	err = request_irq(pci_irq_vector(pdev, 0),
			  mlxsw_pci_eq_irq_handler, 0,
			  mlxsw_pci->bus_info.device_kind, mlxsw_pci);
	if (err) {
		dev_err(&pdev->dev, "IRQ request failed\n");
		goto err_request_eq_irq;
	}

	goto mbox_put;

err_request_eq_irq:
	mlxsw_pci_aqs_fini(mlxsw_pci);
err_aqs_init:
	mlxsw_pci_napi_devs_fini(mlxsw_pci);
err_napi_devs_init:
err_requery_resources:
err_config_profile:
err_cqe_v_check:
err_query_resources:
err_boardinfo:
	mlxsw_pci_fw_area_fini(mlxsw_pci);
err_fw_area_init:
err_utc_nsec_bar:
err_utc_sec_bar:
err_fr_rn_clk_bar:
err_doorbell_page_bar:
err_iface_rev:
err_query_fw:
	mlxsw_pci_free_irq_vectors(mlxsw_pci);
err_alloc_irq:
err_reset:
mbox_put:
	mlxsw_cmd_mbox_free(mbox);
	return err;
}

static void mlxsw_pci_fini(void *bus_priv)
{
	struct mlxsw_pci *mlxsw_pci = bus_priv;

	free_irq(pci_irq_vector(mlxsw_pci->pdev, 0), mlxsw_pci);
	mlxsw_pci_aqs_fini(mlxsw_pci);
	mlxsw_pci_napi_devs_fini(mlxsw_pci);
	mlxsw_pci_fw_area_fini(mlxsw_pci);
	mlxsw_pci_free_irq_vectors(mlxsw_pci);
}

static int mlxsw_pci_txhdr_construct(struct sk_buff *skb,
				     const struct mlxsw_txhdr_info *txhdr_info)
{
	const struct mlxsw_tx_info tx_info = txhdr_info->tx_info;
	char *txhdr;

	if (skb_cow_head(skb, MLXSW_TXHDR_LEN))
		return -ENOMEM;

	txhdr = skb_push(skb, MLXSW_TXHDR_LEN);
	memset(txhdr, 0, MLXSW_TXHDR_LEN);

	mlxsw_tx_hdr_version_set(txhdr, MLXSW_TXHDR_VERSION_1);
	mlxsw_tx_hdr_proto_set(txhdr, MLXSW_TXHDR_PROTO_ETH);
	mlxsw_tx_hdr_swid_set(txhdr, 0);

	if (unlikely(txhdr_info->data)) {
		u16 fid = txhdr_info->max_fid + tx_info.local_port - 1;

		mlxsw_tx_hdr_rx_is_router_set(txhdr, true);
		mlxsw_tx_hdr_fid_valid_set(txhdr, true);
		mlxsw_tx_hdr_fid_set(txhdr, fid);
		mlxsw_tx_hdr_type_set(txhdr, MLXSW_TXHDR_TYPE_DATA);
	} else {
		mlxsw_tx_hdr_ctl_set(txhdr, MLXSW_TXHDR_ETH_CTL);
		mlxsw_tx_hdr_control_tclass_set(txhdr, 1);
		mlxsw_tx_hdr_port_mid_set(txhdr, tx_info.local_port);
		mlxsw_tx_hdr_type_set(txhdr, MLXSW_TXHDR_TYPE_CONTROL);
	}

	return 0;
}

static struct mlxsw_pci_queue *
mlxsw_pci_sdq_pick(struct mlxsw_pci *mlxsw_pci,
		   const struct mlxsw_tx_info *tx_info)
{
	u8 ctl_sdq_count = mlxsw_pci->num_sdqs - 1;
	u8 sdqn;

	if (tx_info->is_emad) {
		sdqn = MLXSW_PCI_SDQ_EMAD_INDEX;
	} else {
		BUILD_BUG_ON(MLXSW_PCI_SDQ_EMAD_INDEX != 0);
		sdqn = 1 + (tx_info->local_port % ctl_sdq_count);
	}

	return mlxsw_pci_sdq_get(mlxsw_pci, sdqn);
}

static bool mlxsw_pci_skb_transmit_busy(void *bus_priv,
					const struct mlxsw_tx_info *tx_info)
{
	struct mlxsw_pci *mlxsw_pci = bus_priv;
	struct mlxsw_pci_queue *q = mlxsw_pci_sdq_pick(mlxsw_pci, tx_info);

	return !mlxsw_pci_queue_elem_info_producer_get(q);
}

static int mlxsw_pci_skb_transmit(void *bus_priv, struct sk_buff *skb,
				  const struct mlxsw_txhdr_info *txhdr_info)
{
	struct mlxsw_pci *mlxsw_pci = bus_priv;
	struct mlxsw_pci_queue *q;
	struct mlxsw_pci_queue_elem_info *elem_info;
	char *wqe;
	int i;
	int err;

	err = mlxsw_pci_txhdr_construct(skb, txhdr_info);
	if (err)
		return err;

	if (skb_shinfo(skb)->nr_frags > MLXSW_PCI_WQE_SG_ENTRIES - 1) {
		err = skb_linearize(skb);
		if (err)
			return err;
	}

	q = mlxsw_pci_sdq_pick(mlxsw_pci, &txhdr_info->tx_info);
	spin_lock_bh(&q->lock);
	elem_info = mlxsw_pci_queue_elem_info_producer_get(q);
	if (!elem_info) {
		/* queue is full */
		err = -EAGAIN;
		goto unlock;
	}
	mlxsw_skb_cb(skb)->tx_info = txhdr_info->tx_info;
	elem_info->sdq.skb = skb;

	wqe = elem_info->elem;
	mlxsw_pci_wqe_c_set(wqe, 1); /* always report completion */
	mlxsw_pci_wqe_lp_set(wqe, 0);
	mlxsw_pci_wqe_type_set(wqe, MLXSW_PCI_WQE_TYPE_ETHERNET);

	err = mlxsw_pci_wqe_frag_map(mlxsw_pci, wqe, 0, skb->data,
				     skb_headlen(skb), DMA_TO_DEVICE);
	if (err)
		goto unlock;

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		const skb_frag_t *frag = &skb_shinfo(skb)->frags[i];

		err = mlxsw_pci_wqe_frag_map(mlxsw_pci, wqe, i + 1,
					     skb_frag_address(frag),
					     skb_frag_size(frag),
					     DMA_TO_DEVICE);
		if (err)
			goto unmap_frags;
	}

	if (unlikely(skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP))
		skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;

	/* Set unused sq entries byte count to zero. */
	for (i++; i < MLXSW_PCI_WQE_SG_ENTRIES; i++)
		mlxsw_pci_wqe_byte_count_set(wqe, i, 0);

	mlxsw_pci_wqe_ipcs_set(wqe, skb->ip_summed == CHECKSUM_PARTIAL);

	/* Everything is set up, ring producer doorbell to get HW going */
	q->producer_counter++;
	mlxsw_pci_queue_doorbell_producer_ring(mlxsw_pci, q);

	goto unlock;

unmap_frags:
	for (; i >= 0; i--)
		mlxsw_pci_wqe_frag_unmap(mlxsw_pci, wqe, i, DMA_TO_DEVICE);
unlock:
	spin_unlock_bh(&q->lock);
	return err;
}

static int mlxsw_pci_cmd_exec(void *bus_priv, u16 opcode, u8 opcode_mod,
			      u32 in_mod, bool out_mbox_direct,
			      char *in_mbox, size_t in_mbox_size,
			      char *out_mbox, size_t out_mbox_size,
			      u8 *p_status)
{
	struct mlxsw_pci *mlxsw_pci = bus_priv;
	dma_addr_t in_mapaddr = 0, out_mapaddr = 0;
	unsigned long timeout = msecs_to_jiffies(MLXSW_PCI_CIR_TIMEOUT_MSECS);
	unsigned long end;
	bool wait_done;
	int err;

	*p_status = MLXSW_CMD_STATUS_OK;

	err = mutex_lock_interruptible(&mlxsw_pci->cmd.lock);
	if (err)
		return err;

	if (in_mbox) {
		memcpy(mlxsw_pci->cmd.in_mbox.buf, in_mbox, in_mbox_size);
		in_mapaddr = mlxsw_pci->cmd.in_mbox.mapaddr;
	}
	mlxsw_pci_write32(mlxsw_pci, CIR_IN_PARAM_HI, upper_32_bits(in_mapaddr));
	mlxsw_pci_write32(mlxsw_pci, CIR_IN_PARAM_LO, lower_32_bits(in_mapaddr));

	if (out_mbox)
		out_mapaddr = mlxsw_pci->cmd.out_mbox.mapaddr;
	mlxsw_pci_write32(mlxsw_pci, CIR_OUT_PARAM_HI, upper_32_bits(out_mapaddr));
	mlxsw_pci_write32(mlxsw_pci, CIR_OUT_PARAM_LO, lower_32_bits(out_mapaddr));

	mlxsw_pci_write32(mlxsw_pci, CIR_IN_MODIFIER, in_mod);
	mlxsw_pci_write32(mlxsw_pci, CIR_TOKEN, 0);

	wait_done = false;

	wmb(); /* all needs to be written before we write control register */
	mlxsw_pci_write32(mlxsw_pci, CIR_CTRL,
			  MLXSW_PCI_CIR_CTRL_GO_BIT |
			  (opcode_mod << MLXSW_PCI_CIR_CTRL_OPCODE_MOD_SHIFT) |
			  opcode);

	end = jiffies + timeout;
	do {
		u32 ctrl = mlxsw_pci_read32(mlxsw_pci, CIR_CTRL);

		if (!(ctrl & MLXSW_PCI_CIR_CTRL_GO_BIT)) {
			wait_done = true;
			*p_status = ctrl >> MLXSW_PCI_CIR_CTRL_STATUS_SHIFT;
			break;
		}
		cond_resched();
	} while (time_before(jiffies, end));

	err = 0;
	if (wait_done) {
		if (*p_status)
			err = -EIO;
	} else {
		err = -ETIMEDOUT;
	}

	if (!err && out_mbox && out_mbox_direct) {
		/* Some commands don't use output param as address to mailbox
		 * but they store output directly into registers. In that case,
		 * copy registers into mbox buffer.
		 */
		__be32 tmp;

		tmp = cpu_to_be32(mlxsw_pci_read32(mlxsw_pci,
						   CIR_OUT_PARAM_HI));
		memcpy(out_mbox, &tmp, sizeof(tmp));
		tmp = cpu_to_be32(mlxsw_pci_read32(mlxsw_pci,
						   CIR_OUT_PARAM_LO));
		memcpy(out_mbox + sizeof(tmp), &tmp, sizeof(tmp));
	} else if (!err && out_mbox) {
		memcpy(out_mbox, mlxsw_pci->cmd.out_mbox.buf, out_mbox_size);
	}

	mutex_unlock(&mlxsw_pci->cmd.lock);

	return err;
}

static u32 mlxsw_pci_read_frc_h(void *bus_priv)
{
	struct mlxsw_pci *mlxsw_pci = bus_priv;
	u64 frc_offset_h;

	frc_offset_h = mlxsw_pci->free_running_clock_offset;
	return mlxsw_pci_read32_off(mlxsw_pci, frc_offset_h);
}

static u32 mlxsw_pci_read_frc_l(void *bus_priv)
{
	struct mlxsw_pci *mlxsw_pci = bus_priv;
	u64 frc_offset_l;

	frc_offset_l = mlxsw_pci->free_running_clock_offset + 4;
	return mlxsw_pci_read32_off(mlxsw_pci, frc_offset_l);
}

static u32 mlxsw_pci_read_utc_sec(void *bus_priv)
{
	struct mlxsw_pci *mlxsw_pci = bus_priv;

	return mlxsw_pci_read32_off(mlxsw_pci, mlxsw_pci->utc_sec_offset);
}

static u32 mlxsw_pci_read_utc_nsec(void *bus_priv)
{
	struct mlxsw_pci *mlxsw_pci = bus_priv;

	return mlxsw_pci_read32_off(mlxsw_pci, mlxsw_pci->utc_nsec_offset);
}

static enum mlxsw_cmd_mbox_config_profile_lag_mode
mlxsw_pci_lag_mode(void *bus_priv)
{
	struct mlxsw_pci *mlxsw_pci = bus_priv;

	return mlxsw_pci->lag_mode;
}

static enum mlxsw_cmd_mbox_config_profile_flood_mode
mlxsw_pci_flood_mode(void *bus_priv)
{
	struct mlxsw_pci *mlxsw_pci = bus_priv;

	return mlxsw_pci->flood_mode;
}

static const struct mlxsw_bus mlxsw_pci_bus = {
	.kind			= "pci",
	.init			= mlxsw_pci_init,
	.fini			= mlxsw_pci_fini,
	.skb_transmit_busy	= mlxsw_pci_skb_transmit_busy,
	.skb_transmit		= mlxsw_pci_skb_transmit,
	.cmd_exec		= mlxsw_pci_cmd_exec,
	.read_frc_h		= mlxsw_pci_read_frc_h,
	.read_frc_l		= mlxsw_pci_read_frc_l,
	.read_utc_sec		= mlxsw_pci_read_utc_sec,
	.read_utc_nsec		= mlxsw_pci_read_utc_nsec,
	.lag_mode		= mlxsw_pci_lag_mode,
	.flood_mode		= mlxsw_pci_flood_mode,
	.features		= MLXSW_BUS_F_TXRX | MLXSW_BUS_F_RESET,
};

static int mlxsw_pci_cmd_init(struct mlxsw_pci *mlxsw_pci)
{
	int err;

	mutex_init(&mlxsw_pci->cmd.lock);

	err = mlxsw_pci_mbox_alloc(mlxsw_pci, &mlxsw_pci->cmd.in_mbox);
	if (err)
		goto err_in_mbox_alloc;

	err = mlxsw_pci_mbox_alloc(mlxsw_pci, &mlxsw_pci->cmd.out_mbox);
	if (err)
		goto err_out_mbox_alloc;

	return 0;

err_out_mbox_alloc:
	mlxsw_pci_mbox_free(mlxsw_pci, &mlxsw_pci->cmd.in_mbox);
err_in_mbox_alloc:
	mutex_destroy(&mlxsw_pci->cmd.lock);
	return err;
}

static void mlxsw_pci_cmd_fini(struct mlxsw_pci *mlxsw_pci)
{
	mlxsw_pci_mbox_free(mlxsw_pci, &mlxsw_pci->cmd.out_mbox);
	mlxsw_pci_mbox_free(mlxsw_pci, &mlxsw_pci->cmd.in_mbox);
	mutex_destroy(&mlxsw_pci->cmd.lock);
}

static int mlxsw_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	const char *driver_name = dev_driver_string(&pdev->dev);
	struct mlxsw_pci *mlxsw_pci;
	int err;

	mlxsw_pci = kzalloc(sizeof(*mlxsw_pci), GFP_KERNEL);
	if (!mlxsw_pci)
		return -ENOMEM;

	err = pci_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "pci_enable_device failed\n");
		goto err_pci_enable_device;
	}

	err = pci_request_regions(pdev, driver_name);
	if (err) {
		dev_err(&pdev->dev, "pci_request_regions failed\n");
		goto err_pci_request_regions;
	}

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (err) {
		err = dma_set_mask(&pdev->dev, DMA_BIT_MASK(32));
		if (err) {
			dev_err(&pdev->dev, "dma_set_mask failed\n");
			goto err_pci_set_dma_mask;
		}
	}

	if (pci_resource_len(pdev, 0) < MLXSW_PCI_BAR0_SIZE) {
		dev_err(&pdev->dev, "invalid PCI region size\n");
		err = -EINVAL;
		goto err_pci_resource_len_check;
	}

	mlxsw_pci->hw_addr = ioremap(pci_resource_start(pdev, 0),
				     pci_resource_len(pdev, 0));
	if (!mlxsw_pci->hw_addr) {
		dev_err(&pdev->dev, "ioremap failed\n");
		err = -EIO;
		goto err_ioremap;
	}
	pci_set_master(pdev);

	mlxsw_pci->pdev = pdev;
	pci_set_drvdata(pdev, mlxsw_pci);

	err = mlxsw_pci_cmd_init(mlxsw_pci);
	if (err)
		goto err_pci_cmd_init;

	mlxsw_pci->bus_info.device_kind = driver_name;
	mlxsw_pci->bus_info.device_name = pci_name(mlxsw_pci->pdev);
	mlxsw_pci->bus_info.dev = &pdev->dev;
	mlxsw_pci->bus_info.read_clock_capable = true;
	mlxsw_pci->id = id;

	err = mlxsw_core_bus_device_register(&mlxsw_pci->bus_info,
					     &mlxsw_pci_bus, mlxsw_pci, false,
					     NULL, NULL);
	if (err) {
		dev_err(&pdev->dev, "cannot register bus device\n");
		goto err_bus_device_register;
	}

	return 0;

err_bus_device_register:
	mlxsw_pci_cmd_fini(mlxsw_pci);
err_pci_cmd_init:
	iounmap(mlxsw_pci->hw_addr);
err_ioremap:
err_pci_resource_len_check:
err_pci_set_dma_mask:
	pci_release_regions(pdev);
err_pci_request_regions:
	pci_disable_device(pdev);
err_pci_enable_device:
	kfree(mlxsw_pci);
	return err;
}

static void mlxsw_pci_remove(struct pci_dev *pdev)
{
	struct mlxsw_pci *mlxsw_pci = pci_get_drvdata(pdev);

	mlxsw_core_bus_device_unregister(mlxsw_pci->core, false);
	mlxsw_pci_cmd_fini(mlxsw_pci);
	iounmap(mlxsw_pci->hw_addr);
	pci_release_regions(mlxsw_pci->pdev);
	pci_disable_device(mlxsw_pci->pdev);
	kfree(mlxsw_pci);
}

static void mlxsw_pci_reset_prepare(struct pci_dev *pdev)
{
	struct mlxsw_pci *mlxsw_pci = pci_get_drvdata(pdev);

	mlxsw_core_bus_device_unregister(mlxsw_pci->core, false);
}

static void mlxsw_pci_reset_done(struct pci_dev *pdev)
{
	struct mlxsw_pci *mlxsw_pci = pci_get_drvdata(pdev);

	mlxsw_pci->skip_reset = true;
	mlxsw_core_bus_device_register(&mlxsw_pci->bus_info, &mlxsw_pci_bus,
				       mlxsw_pci, false, NULL, NULL);
	mlxsw_pci->skip_reset = false;
}

static const struct pci_error_handlers mlxsw_pci_err_handler = {
	.reset_prepare = mlxsw_pci_reset_prepare,
	.reset_done = mlxsw_pci_reset_done,
};

int mlxsw_pci_driver_register(struct pci_driver *pci_driver)
{
	pci_driver->probe = mlxsw_pci_probe;
	pci_driver->remove = mlxsw_pci_remove;
	pci_driver->shutdown = mlxsw_pci_remove;
	pci_driver->err_handler = &mlxsw_pci_err_handler;
	return pci_register_driver(pci_driver);
}
EXPORT_SYMBOL(mlxsw_pci_driver_register);

void mlxsw_pci_driver_unregister(struct pci_driver *pci_driver)
{
	pci_unregister_driver(pci_driver);
}
EXPORT_SYMBOL(mlxsw_pci_driver_unregister);

static int __init mlxsw_pci_module_init(void)
{
	return 0;
}

static void __exit mlxsw_pci_module_exit(void)
{
}

module_init(mlxsw_pci_module_init);
module_exit(mlxsw_pci_module_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Jiri Pirko <jiri@mellanox.com>");
MODULE_DESCRIPTION("Mellanox switch PCI interface driver");
