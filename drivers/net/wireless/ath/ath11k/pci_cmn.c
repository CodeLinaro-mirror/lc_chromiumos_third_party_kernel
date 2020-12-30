// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) 2021 The Linux Foundation. All rights reserved.
 */

#include <linux/pci.h>
#include "core.h"
#include "pci_cmn.h"
#include "debug.h"

#define WINDOW_ENABLE_BIT		0x40000000
#define WINDOW_REG_ADDRESS		0x310c
#define WINDOW_VALUE_MASK		GENMASK(24, 19)
#define WINDOW_START			0x80000
#define WINDOW_RANGE_MASK		GENMASK(18, 0)

#define ATH11K_PCI_IRQ_CE0_OFFSET		3

/* BAR0 + 4k is always accessible, and no
 * need to force wakeup.
 * 4K - 32 = 0xFE0
 */
#define ACCESS_ALWAYS_OFF 0xFE0

static const char *irq_name_pci[ATH11K_IRQ_NUM_MAX] = {
	"bhi",
	"mhi-er0",
	"mhi-er1",
	"ce0",
	"ce1",
	"ce2",
	"ce3",
	"ce4",
	"ce5",
	"ce6",
	"ce7",
	"ce8",
	"ce9",
	"ce10",
	"ce11",
	"host2wbm-desc-feed",
	"host2reo-re-injection",
	"host2reo-command",
	"host2rxdma-monitor-ring3",
	"host2rxdma-monitor-ring2",
	"host2rxdma-monitor-ring1",
	"reo2ost-exception",
	"wbm2host-rx-release",
	"reo2host-status",
	"reo2host-destination-ring4",
	"reo2host-destination-ring3",
	"reo2host-destination-ring2",
	"reo2host-destination-ring1",
	"rxdma2host-monitor-destination-mac3",
	"rxdma2host-monitor-destination-mac2",
	"rxdma2host-monitor-destination-mac1",
	"ppdu-end-interrupts-mac3",
	"ppdu-end-interrupts-mac2",
	"ppdu-end-interrupts-mac1",
	"rxdma2host-monitor-status-ring-mac3",
	"rxdma2host-monitor-status-ring-mac2",
	"rxdma2host-monitor-status-ring-mac1",
	"host2rxdma-host-buf-ring-mac3",
	"host2rxdma-host-buf-ring-mac2",
	"host2rxdma-host-buf-ring-mac1",
	"rxdma2host-destination-ring-mac3",
	"rxdma2host-destination-ring-mac2",
	"rxdma2host-destination-ring-mac1",
	"host2tcl-input-ring4",
	"host2tcl-input-ring3",
	"host2tcl-input-ring2",
	"host2tcl-input-ring1",
	"wbm2host-tx-completions-ring3",
	"wbm2host-tx-completions-ring2",
	"wbm2host-tx-completions-ring1",
	"tcl2host-status-ring",
};

const struct ath11k_msi_config ath11k_msi_config[] = {
	{
		.total_vectors = 32,
		.total_users = 4,
		.users = (struct ath11k_msi_user[]) {
			{ .name = "MHI", .num_vectors = 3, .base_vector = 0 },
			{ .name = "CE", .num_vectors = 10, .base_vector = 3 },
			{ .name = "WAKE", .num_vectors = 1, .base_vector = 13 },
			{ .name = "DP", .num_vectors = 18, .base_vector = 14 },
		},
		.hw_rev = ATH11K_HW_QCA6390_HW20,
	},
	{
		.total_vectors = 16,
		.total_users = 3,
		.users = (struct ath11k_msi_user[]) {
			{ .name = "MHI", .num_vectors = 3, .base_vector = 0 },
			{ .name = "CE", .num_vectors = 5, .base_vector = 3 },
			{ .name = "DP", .num_vectors = 8, .base_vector = 8 },
		},
		.hw_rev = ATH11K_HW_QCN9074_HW10,
	},
	{
		.total_vectors = 28,
		.total_users = 2,
		.users = (struct ath11k_msi_user[]) {
			{ .name = "CE", .num_vectors = 10, .base_vector = 0 },
			{ .name = "DP", .num_vectors = 18, .base_vector = 10 },
		},
		.hw_rev = ATH11K_HW_WCN6750_HW10,
	},
};

int ath11k_pci_get_msi_config(struct ath11k_base *ab)
{
	const struct ath11k_msi_config *msi_config;
	int i;

	for (i = 0; i < ARRAY_SIZE(ath11k_msi_config); i++) {
		msi_config = &ath11k_msi_config[i];

		if (msi_config->hw_rev == ab->hw_rev)
			break;
	}

	if (i == ARRAY_SIZE(ath11k_msi_config)) {
		ath11k_err(ab, "failed to fetch msi config, unsupported hw version: 0x%x\n",
			   ab->hw_rev);
		return -EINVAL;
	}

	ab->msi.msi_config = msi_config;
	return 0;
}

static inline void ath11k_pci_select_window(struct ath11k_pci *ab_pci, u32 offset)
{
	u32 window = FIELD_GET(WINDOW_VALUE_MASK, offset);

	lockdep_assert_held(&ab_pci->window_lock);

	if (window != ab_pci->register_window) {
		iowrite32(WINDOW_ENABLE_BIT | window,
			  ab_pci->ab->mem + WINDOW_REG_ADDRESS);
		ioread32(ab_pci->ab->mem + WINDOW_REG_ADDRESS);
		ab_pci->register_window = window;
	}
}

void ath11k_pci_select_static_window(struct ath11k_base *ab)
{
	u32 umac_window = FIELD_GET(WINDOW_VALUE_MASK, HAL_SEQ_WCSS_UMAC_OFFSET);
	u32 ce_window = FIELD_GET(WINDOW_VALUE_MASK, HAL_CE_WFSS_CE_REG_BASE);
	u32 window;

	window = (umac_window << 12) | (ce_window << 6);

	iowrite32(WINDOW_ENABLE_BIT | window, ab->mem + WINDOW_REG_ADDRESS);
}

static inline u32 ath11k_pci_get_window_start(struct ath11k_base *ab,
					      u32 offset)
{
	u32 window_start = 0;

	/* If offset lies within DP register range, use 1st window */
	if ((offset ^ HAL_SEQ_WCSS_UMAC_OFFSET) < WINDOW_RANGE_MASK)
		window_start = ab->bus_params.dp_window_idx * WINDOW_START;
	/* If offset lies within CE register range, use 2nd window */
	else if ((offset ^ HAL_SEQ_WCSS_UMAC_CE0_SRC_REG(ab)) < WINDOW_RANGE_MASK)
		window_start = ab->bus_params.ce_window_idx * WINDOW_START;

	return window_start;
}

void ath11k_pci_write32(struct ath11k_base *ab, u32 offset, u32 value)
{
	struct ath11k_pci __maybe_unused *ab_pci;
	u32 window_start = WINDOW_START;

	/* for offset beyond BAR + 4K - 32, may
	 * need to wakeup the device to access.
	 */
	if (test_bit(ATH11K_FLAG_DEV_INIT_DONE, &ab->dev_flags) &&
	    offset >= ACCESS_ALWAYS_OFF && ab->bus_params.bus_wakeup)
		ab->bus_params.bus_wakeup(ab);

	if (offset < WINDOW_START) {
		iowrite32(value, ab->mem  + offset);
	} else {
		if (ab->bus_params.static_window_map) {
			window_start = ath11k_pci_get_window_start(ab, offset);
			iowrite32(value, ab->mem + window_start +
				  (offset & WINDOW_RANGE_MASK));
		} else {
			ab_pci = ath11k_pci_priv(ab);

			spin_lock_bh(&ab_pci->window_lock);
			ath11k_pci_select_window(ab_pci, offset);
			iowrite32(value, ab->mem + window_start +
				  (offset & WINDOW_RANGE_MASK));
			spin_unlock_bh(&ab_pci->window_lock);
		}
	}

	if (test_bit(ATH11K_FLAG_DEV_INIT_DONE, &ab->dev_flags) &&
	    offset >= ACCESS_ALWAYS_OFF && ab->bus_params.bus_release)
		ab->bus_params.bus_release(ab);
}

u32 ath11k_pci_read32(struct ath11k_base *ab, u32 offset)
{
	struct ath11k_pci __maybe_unused *ab_pci;
	u32 val, window_start = WINDOW_START;

	/* for offset beyond BAR + 4K - 32, may
	 * need to wakeup the device to access.
	 */
	if (test_bit(ATH11K_FLAG_DEV_INIT_DONE, &ab->dev_flags) &&
	    offset >= ACCESS_ALWAYS_OFF && ab->bus_params.bus_wakeup)
		ab->bus_params.bus_wakeup(ab);

	if (offset < WINDOW_START) {
		val = ioread32(ab->mem + offset);
	} else {
		if (ab->bus_params.static_window_map) {
			window_start = ath11k_pci_get_window_start(ab, offset);
			val = ioread32(ab->mem + window_start +
				       (offset & WINDOW_RANGE_MASK));
		} else {
			ab_pci = ath11k_pci_priv(ab);

			spin_lock_bh(&ab_pci->window_lock);
			ath11k_pci_select_window(ab_pci, offset);
			val = ioread32(ab->mem + window_start +
				       (offset & WINDOW_RANGE_MASK));
			spin_unlock_bh(&ab_pci->window_lock);
		}
	}

	if (test_bit(ATH11K_FLAG_DEV_INIT_DONE, &ab->dev_flags) &&
	    offset >= ACCESS_ALWAYS_OFF && ab->bus_params.bus_release)
		ab->bus_params.bus_release(ab);

	return val;
}

int ath11k_pci_get_msi_irq(struct ath11k_base *ab, unsigned int vector)
{
	struct pci_dev __maybe_unused *pci_dev;

	if (ab->bus_params.hybrid_bus_type)
		return ab->msi.irqs[vector];

	pci_dev = to_pci_dev(ab->dev);

	return pci_irq_vector(pci_dev, vector);
}

void ath11k_pci_get_msi_address(struct ath11k_base *ab, u32 *msi_addr_lo,
				       u32 *msi_addr_hi)
{
	*msi_addr_lo = ab->msi.msi_addr_lo;
	*msi_addr_hi = ab->msi.msi_addr_hi;
}

int ath11k_pci_get_user_msi_assignment(struct ath11k_base *ab , char *user_name,
				       int *num_vectors, u32 *user_base_data,
				       u32 *base_vector)
{
	const struct ath11k_msi_config *msi_config = ab->msi.msi_config;
	int idx;

	for (idx = 0; idx < msi_config->total_users; idx++) {
		if (strcmp(user_name, msi_config->users[idx].name) == 0) {
			*num_vectors = msi_config->users[idx].num_vectors;
			*user_base_data = msi_config->users[idx].base_vector
				+ ab->msi.msi_ep_base_data;
			*base_vector = msi_config->users[idx].base_vector;

			ath11k_dbg(ab, ATH11K_DBG_PCI, "Assign MSI to user: %s, num_vectors: %d, user_base_data: %u, base_vector: %u\n",
				   user_name, *num_vectors, *user_base_data,
				   *base_vector);

			return 0;
		}
	}

	ath11k_err(ab, "Failed to find MSI assignment for %s!\n", user_name);

	return -EINVAL;
}

void ath11k_pci_get_ce_msi_idx(struct ath11k_base *ab, u32 ce_id,
				      u32 *msi_idx)
{
	u32 i, msi_data_idx;

	for (i = 0, msi_data_idx = 0; i < ab->hw_params.ce_count; i++) {
		if (ath11k_ce_get_attr_flags(ab, i) & CE_ATTR_DIS_INTR)
			continue;

		if (ce_id == i)
			break;

		msi_data_idx++;
	}
	*msi_idx = msi_data_idx;
}

static void ath11k_pci_free_ext_irq(struct ath11k_base *ab)
{
	int i, j;

	for (i = 0; i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
		struct ath11k_ext_irq_grp *irq_grp = &ab->ext_irq_grp[i];

		for (j = 0; j < irq_grp->num_irq; j++)
			free_irq(ab->irq_num[irq_grp->irqs[j]], irq_grp);

		netif_napi_del(&irq_grp->napi);
	}
}

void ath11k_pci_free_irq(struct ath11k_base *ab)
{
	int i, irq_idx;

	for (i = 0; i < ab->hw_params.ce_count; i++) {
		if (ath11k_ce_get_attr_flags(ab, i) & CE_ATTR_DIS_INTR)
			continue;
		irq_idx = ATH11K_PCI_IRQ_CE0_OFFSET + i;
		free_irq(ab->irq_num[irq_idx], &ab->ce.ce_pipe[i]);
	}

	ath11k_pci_free_ext_irq(ab);
}

static void ath11k_pci_ce_irq_enable(struct ath11k_base *ab, u16 ce_id)
{
	u32 irq_idx;

	irq_idx = ATH11K_PCI_IRQ_CE0_OFFSET + ce_id;
	enable_irq(ab->irq_num[irq_idx]);
}

static void ath11k_pci_ce_irq_disable(struct ath11k_base *ab, u16 ce_id)
{
	u32 irq_idx;

	irq_idx = ATH11K_PCI_IRQ_CE0_OFFSET + ce_id;
	disable_irq_nosync(ab->irq_num[irq_idx]);
}

static void ath11k_pci_ce_irqs_disable(struct ath11k_base *ab)
{
	int i;

	for (i = 0; i < ab->hw_params.ce_count; i++) {
		if (ath11k_ce_get_attr_flags(ab, i) & CE_ATTR_DIS_INTR)
			continue;
		ath11k_pci_ce_irq_disable(ab, i);
	}
}

static void ath11k_pci_sync_ce_irqs(struct ath11k_base *ab)
{
	int i;
	int irq_idx;

	for (i = 0; i < ab->hw_params.ce_count; i++) {
		if (ath11k_ce_get_attr_flags(ab, i) & CE_ATTR_DIS_INTR)
			continue;

		irq_idx = ATH11K_PCI_IRQ_CE0_OFFSET + i;
		synchronize_irq(ab->irq_num[irq_idx]);
	}
}

static void ath11k_pci_ce_tasklet(unsigned long data)
{
	struct ath11k_ce_pipe *ce_pipe = (struct ath11k_ce_pipe *)data;

	ath11k_ce_per_engine_service(ce_pipe->ab, ce_pipe->pipe_num);

	ath11k_pci_ce_irq_enable(ce_pipe->ab, ce_pipe->pipe_num);
}

static irqreturn_t ath11k_pci_ce_interrupt_handler(int irq, void *arg)
{
	struct ath11k_ce_pipe *ce_pipe = arg;

	/* last interrupt received for this CE */
	ce_pipe->timestamp = jiffies;

	ath11k_pci_ce_irq_disable(ce_pipe->ab, ce_pipe->pipe_num);
	tasklet_schedule(&ce_pipe->intr_tq);

	return IRQ_HANDLED;
}

static void ath11k_pci_ext_grp_disable(struct ath11k_ext_irq_grp *irq_grp)
{
	int i;

	for (i = 0; i < irq_grp->num_irq; i++)
		disable_irq_nosync(irq_grp->ab->irq_num[irq_grp->irqs[i]]);
}

static void __ath11k_pci_ext_irq_disable(struct ath11k_base *sc)
{
	int i;

	for (i = 0; i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
		struct ath11k_ext_irq_grp *irq_grp = &sc->ext_irq_grp[i];

		ath11k_pci_ext_grp_disable(irq_grp);

		napi_synchronize(&irq_grp->napi);
		napi_disable(&irq_grp->napi);
	}
}

static void ath11k_pci_ext_grp_enable(struct ath11k_ext_irq_grp *irq_grp)
{
	int i;

	for (i = 0; i < irq_grp->num_irq; i++)
		enable_irq(irq_grp->ab->irq_num[irq_grp->irqs[i]]);
}

void ath11k_pci_ext_irq_enable(struct ath11k_base *ab)
{
	int i;

	for (i = 0; i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
		struct ath11k_ext_irq_grp *irq_grp = &ab->ext_irq_grp[i];

		napi_enable(&irq_grp->napi);
		ath11k_pci_ext_grp_enable(irq_grp);
	}
}

static void ath11k_pci_sync_ext_irqs(struct ath11k_base *ab)
{
	int i, j, irq_idx;

	for (i = 0; i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
		struct ath11k_ext_irq_grp *irq_grp = &ab->ext_irq_grp[i];

		for (j = 0; j < irq_grp->num_irq; j++) {
			irq_idx = irq_grp->irqs[j];
			synchronize_irq(ab->irq_num[irq_idx]);
		}
	}
}

void ath11k_pci_ext_irq_disable(struct ath11k_base *ab)
{
	__ath11k_pci_ext_irq_disable(ab);
	ath11k_pci_sync_ext_irqs(ab);
}

static int ath11k_pci_ext_grp_napi_poll(struct napi_struct *napi, int budget)
{
	struct ath11k_ext_irq_grp *irq_grp = container_of(napi,
						struct ath11k_ext_irq_grp,
						napi);
	struct ath11k_base *ab = irq_grp->ab;
	int work_done;

	work_done = ath11k_dp_service_srng(ab, irq_grp, budget);
	if (work_done < budget) {
		napi_complete_done(napi, work_done);
		ath11k_pci_ext_grp_enable(irq_grp);
	}

	if (work_done > budget)
		work_done = budget;

	return work_done;
}

static irqreturn_t ath11k_pci_ext_interrupt_handler(int irq, void *arg)
{
	struct ath11k_ext_irq_grp *irq_grp = arg;

	ath11k_dbg(irq_grp->ab, ATH11K_DBG_PCI, "ext irq:%d\n", irq);

	/* last interrupt received for this group */
	irq_grp->timestamp = jiffies;

	ath11k_pci_ext_grp_disable(irq_grp);

	napi_schedule(&irq_grp->napi);

	return IRQ_HANDLED;
}

static int ath11k_pci_ext_irq_config(struct ath11k_base *ab)
{
	int i, j, ret, num_vectors = 0;
	u32 user_base_data = 0, base_vector = 0, base_idx;

	base_idx = ATH11K_PCI_IRQ_CE0_OFFSET + CE_COUNT_MAX;
	ret = ath11k_pci_get_user_msi_assignment(ab, "DP",
						 &num_vectors,
						 &user_base_data,
						 &base_vector);
	if (ret < 0)
		return ret;

	for (i = 0; i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
		struct ath11k_ext_irq_grp *irq_grp = &ab->ext_irq_grp[i];
		u32 num_irq = 0;

		irq_grp->ab = ab;
		irq_grp->grp_id = i;
		init_dummy_netdev(&irq_grp->napi_ndev);
		netif_napi_add(&irq_grp->napi_ndev, &irq_grp->napi,
			       ath11k_pci_ext_grp_napi_poll, NAPI_POLL_WEIGHT);

		if (ab->hw_params.ring_mask->tx[i] ||
		    ab->hw_params.ring_mask->rx[i] ||
		    ab->hw_params.ring_mask->rx_err[i] ||
		    ab->hw_params.ring_mask->rx_wbm_rel[i] ||
		    ab->hw_params.ring_mask->reo_status[i] ||
		    ab->hw_params.ring_mask->rxdma2host[i] ||
		    ab->hw_params.ring_mask->host2rxdma[i] ||
		    ab->hw_params.ring_mask->rx_mon_status[i]) {
			num_irq = 1;
		}

		irq_grp->num_irq = num_irq;
		irq_grp->irqs[0] = base_idx + i;

		for (j = 0; j < irq_grp->num_irq; j++) {
			int irq_idx = irq_grp->irqs[j];
			int vector = (i % num_vectors) + base_vector;
			int irq = ath11k_pci_get_msi_irq(ab, vector);

			ab->irq_num[irq_idx] = irq;

			ath11k_dbg(ab, ATH11K_DBG_PCI,
				   "irq:%d group:%d\n", irq, i);

			irq_set_status_flags(irq, IRQ_DISABLE_UNLAZY);
			ret = request_irq(irq, ath11k_pci_ext_interrupt_handler,
					  IRQF_SHARED,
					  "DP_EXT_IRQ", irq_grp);
			if (ret) {
				ath11k_err(ab, "failed request irq %d: %d\n",
					   vector, ret);
				return ret;
			}

			disable_irq_nosync(ab->irq_num[irq_idx]);
		}
	}

	return 0;
}

int ath11k_pci_config_irq(struct ath11k_base *ab)
{
	struct ath11k_ce_pipe *ce_pipe;
	u32 msi_data_start;
	u32 msi_data_count, msi_data_idx;
	u32 msi_irq_start;
	unsigned int msi_data;
	int irq, i, ret, irq_idx;

	ret = ath11k_pci_get_user_msi_assignment(ab, "CE", &msi_data_count,
						 &msi_data_start, &msi_irq_start);
	if (ret)
		return ret;

	/* Configure CE irqs */

	for (i = 0, msi_data_idx = 0; i < ab->hw_params.ce_count; i++) {
		if (ath11k_ce_get_attr_flags(ab, i) & CE_ATTR_DIS_INTR)
			continue;

		msi_data = (msi_data_idx % msi_data_count) + msi_irq_start;
		irq = ath11k_pci_get_msi_irq(ab, msi_data);
		ce_pipe = &ab->ce.ce_pipe[i];

		irq_idx = ATH11K_PCI_IRQ_CE0_OFFSET + i;

		tasklet_init(&ce_pipe->intr_tq, ath11k_pci_ce_tasklet,
			     (unsigned long)ce_pipe);

		ret = request_irq(irq, ath11k_pci_ce_interrupt_handler,
				  IRQF_SHARED, irq_name_pci[irq_idx],
				  ce_pipe);
		if (ret) {
			ath11k_err(ab, "failed to request irq %d: %d\n",
				   irq_idx, ret);
			return ret;
		}

		ab->irq_num[irq_idx] = irq;
		msi_data_idx++;

		ath11k_pci_ce_irq_disable(ab, i);
	}

	ret = ath11k_pci_ext_irq_config(ab);
	if (ret)
		return ret;

	return 0;
}

void ath11k_pci_ce_irqs_enable(struct ath11k_base *ab)
{
	int i;

	for (i = 0; i < ab->hw_params.ce_count; i++) {
		if (ath11k_ce_get_attr_flags(ab, i) & CE_ATTR_DIS_INTR)
			continue;
		ath11k_pci_ce_irq_enable(ab, i);
	}
}

static void ath11k_pci_kill_tasklets(struct ath11k_base *ab)
{
	int i;

	for (i = 0; i < ab->hw_params.ce_count; i++) {
		struct ath11k_ce_pipe *ce_pipe = &ab->ce.ce_pipe[i];

		if (ath11k_ce_get_attr_flags(ab, i) & CE_ATTR_DIS_INTR)
			continue;

		tasklet_kill(&ce_pipe->intr_tq);
	}
}

void ath11k_pci_ce_irq_disable_sync(struct ath11k_base *ab)
{
	ath11k_pci_ce_irqs_disable(ab);
	ath11k_pci_sync_ce_irqs(ab);
	ath11k_pci_kill_tasklets(ab);
}

void ath11k_pci_stop(struct ath11k_base *ab)
{
	ath11k_pci_ce_irq_disable_sync(ab);
	ath11k_ce_cleanup_pipes(ab);
}

int ath11k_pci_start(struct ath11k_base *ab)
{
	set_bit(ATH11K_FLAG_DEV_INIT_DONE, &ab->dev_flags);

	ath11k_pci_ce_irqs_enable(ab);
	ath11k_ce_rx_post_buf(ab);

	return 0;
}

int ath11k_pci_map_service_to_pipe(struct ath11k_base *ab, u16 service_id,
				   u8 *ul_pipe, u8 *dl_pipe)
{
	const struct service_to_pipe *entry;
	bool ul_set = false, dl_set = false;
	int i;

	for (i = 0; i < ab->hw_params.svc_to_ce_map_len; i++) {
		entry = &ab->hw_params.svc_to_ce_map[i];

		if (__le32_to_cpu(entry->service_id) != service_id)
			continue;

		switch (__le32_to_cpu(entry->pipedir)) {
		case PIPEDIR_NONE:
			break;
		case PIPEDIR_IN:
			WARN_ON(dl_set);
			*dl_pipe = __le32_to_cpu(entry->pipenum);
			dl_set = true;
			break;
		case PIPEDIR_OUT:
			WARN_ON(ul_set);
			*ul_pipe = __le32_to_cpu(entry->pipenum);
			ul_set = true;
			break;
		case PIPEDIR_INOUT:
			WARN_ON(dl_set);
			WARN_ON(ul_set);
			*dl_pipe = __le32_to_cpu(entry->pipenum);
			*ul_pipe = __le32_to_cpu(entry->pipenum);
			dl_set = true;
			ul_set = true;
			break;
		}
	}

	if (WARN_ON(!ul_set || !dl_set))
		return -ENOENT;

	return 0;
}
