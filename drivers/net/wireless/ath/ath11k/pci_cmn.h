// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) 2021 The Linux Foundation. All rights reserved.
 */

#ifndef _ATH11K_PCI_CMN_H
#define _ATH11K_PCI_CMN_H

#include "core.h"

struct ath11k_pci {
	struct pci_dev *pdev;
	struct ath11k_base *ab;
	u16 dev_id;
	char amss_path[100];
	struct mhi_controller *mhi_ctrl;
	unsigned long mhi_state;
	u32 register_window;

	/* protects register_window above */
	spinlock_t window_lock;
};

static inline struct ath11k_pci *ath11k_pci_priv(struct ath11k_base *ab)
{
	return (struct ath11k_pci *)ab->drv_priv;
}

void ath11k_pci_write32(struct ath11k_base *ab, u32 offset, u32 value);
u32 ath11k_pci_read32(struct ath11k_base *ab, u32 offset);
int ath11k_pci_get_msi_irq(struct ath11k_base *ab, unsigned int vector);
void ath11k_pci_get_msi_address(struct ath11k_base *ab, u32 *msi_addr_lo,
				       u32 *msi_addr_hi);
int ath11k_pci_get_user_msi_assignment(struct ath11k_base *ab , char *user_name,
				       int *num_vectors, u32 *user_base_data,
				       u32 *base_vector);
void ath11k_pci_get_ce_msi_idx(struct ath11k_base *ab, u32 ce_id,
				      u32 *msi_idx);
void ath11k_pci_free_irq(struct ath11k_base *ab);
int ath11k_pci_config_irq(struct ath11k_base *ab);
void ath11k_pci_ext_irq_enable(struct ath11k_base *ab);
void ath11k_pci_ext_irq_disable(struct ath11k_base *ab);
void ath11k_pci_stop(struct ath11k_base *ab);
int ath11k_pci_start(struct ath11k_base *ab);
int ath11k_pci_map_service_to_pipe(struct ath11k_base *ab, u16 service_id,
				   u8 *ul_pipe, u8 *dl_pipe);
int ath11k_pci_get_msi_config(struct ath11k_base *ab);
void ath11k_pci_select_static_window(struct ath11k_base *ab);
void ath11k_pci_ce_irqs_enable(struct ath11k_base *ab);
void ath11k_pci_ce_irq_disable_sync(struct ath11k_base *ab);
#endif
