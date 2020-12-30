// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) 2019-2020 The Linux Foundation. All rights reserved.
 */

#include <linux/module.h>
#include <linux/msi.h>
#include <linux/pci.h>

#include "pci.h"
#include "core.h"
#include "hif.h"
#include "mhi.h"
#include "debug.h"
#include "pci_cmn.h"

#define ATH11K_PCI_BAR_NUM		0
#define ATH11K_PCI_DMA_MASK		32

#define TCSR_SOC_HW_VERSION		0x0224
#define TCSR_SOC_HW_VERSION_MAJOR_MASK	GENMASK(16, 8)
#define TCSR_SOC_HW_VERSION_MINOR_MASK	GENMASK(7, 0)

#define QCA6390_DEVICE_ID              0x1101
#define QCN9074_DEVICE_ID              0x1104

static const struct pci_device_id ath11k_pci_id_table[] = {
	{ PCI_VDEVICE(QCOM, QCA6390_DEVICE_ID) },
	{ PCI_VDEVICE(QCOM, QCN9074_DEVICE_ID) },
	{0}
};

MODULE_DEVICE_TABLE(pci, ath11k_pci_id_table);

static void ath11k_pci_bus_wake_up(struct ath11k_base *ab)
{
	struct ath11k_pci *ab_pci = ath11k_pci_priv(ab);

	mhi_device_get_sync(ab_pci->mhi_ctrl->mhi_dev);
}

static void ath11k_pci_bus_release(struct ath11k_base *ab)
{
	struct ath11k_pci *ab_pci = ath11k_pci_priv(ab);

	mhi_device_put(ab_pci->mhi_ctrl->mhi_dev);
}

static const struct ath11k_bus_params ath11k_pci_bus_params = {
	.mhi_support = true,
	.m3_fw_support = true,
	.fixed_bdf_addr = false,
	.fixed_mem_region = false,
	.bus_wakeup = ath11k_pci_bus_wake_up,
	.bus_release = ath11k_pci_bus_release,
};

static void ath11k_pci_soc_global_reset(struct ath11k_base *ab)
{
	u32 val, delay;

	val = ath11k_pci_read32(ab, PCIE_SOC_GLOBAL_RESET);

	val |= PCIE_SOC_GLOBAL_RESET_V;

	ath11k_pci_write32(ab, PCIE_SOC_GLOBAL_RESET, val);

	/* TODO: exact time to sleep is uncertain */
	delay = 10;
	mdelay(delay);

	/* Need to toggle V bit back otherwise stuck in reset status */
	val &= ~PCIE_SOC_GLOBAL_RESET_V;

	ath11k_pci_write32(ab, PCIE_SOC_GLOBAL_RESET, val);

	mdelay(delay);

	val = ath11k_pci_read32(ab, PCIE_SOC_GLOBAL_RESET);
	if (val == 0xffffffff)
		ath11k_warn(ab, "link down error during global reset\n");
}

static void ath11k_pci_clear_dbg_registers(struct ath11k_base *ab)
{
	u32 val;

	/* read cookie */
	val = ath11k_pci_read32(ab, PCIE_Q6_COOKIE_ADDR);
	ath11k_dbg(ab, ATH11K_DBG_PCI, "cookie:0x%x\n", val);

	val = ath11k_pci_read32(ab, WLAON_WARM_SW_ENTRY);
	ath11k_dbg(ab, ATH11K_DBG_PCI, "WLAON_WARM_SW_ENTRY 0x%x\n", val);

	/* TODO: exact time to sleep is uncertain */
	mdelay(10);

	/* write 0 to WLAON_WARM_SW_ENTRY to prevent Q6 from
	 * continuing warm path and entering dead loop.
	 */
	ath11k_pci_write32(ab, WLAON_WARM_SW_ENTRY, 0);
	mdelay(10);

	val = ath11k_pci_read32(ab, WLAON_WARM_SW_ENTRY);
	ath11k_dbg(ab, ATH11K_DBG_PCI, "WLAON_WARM_SW_ENTRY 0x%x\n", val);

	/* A read clear register. clear the register to prevent
	 * Q6 from entering wrong code path.
	 */
	val = ath11k_pci_read32(ab, WLAON_SOC_RESET_CAUSE_REG);
	ath11k_dbg(ab, ATH11K_DBG_PCI, "soc reset cause:%d\n", val);
}

static int ath11k_pci_set_link_reg(struct ath11k_base *ab,
				   u32 offset, u32 value, u32 mask)
{
	u32 v;
	int i;

	v = ath11k_pci_read32(ab, offset);
	if ((v & mask) == value)
		return 0;

	for (i = 0; i < 10; i++) {
		ath11k_pci_write32(ab, offset, (v & ~mask) | value);

		v = ath11k_pci_read32(ab, offset);
		if ((v & mask) == value)
			return 0;

		mdelay(2);
	}

	ath11k_warn(ab, "failed to set pcie link register 0x%08x: 0x%08x != 0x%08x\n",
		    offset, v & mask, value);

	return -ETIMEDOUT;
}

static int ath11k_pci_fix_l1ss(struct ath11k_base *ab)
{
	int ret;

	ret = ath11k_pci_set_link_reg(ab,
				      PCIE_QSERDES_COM_SYSCLK_EN_SEL_REG,
				      PCIE_QSERDES_COM_SYSCLK_EN_SEL_VAL,
				      PCIE_QSERDES_COM_SYSCLK_EN_SEL_MSK);
	if (!ret) {
		ath11k_warn(ab, "failed to set sysclk: %d\n", ret);
		return ret;
	}

	ret = ath11k_pci_set_link_reg(ab,
				      PCIE_USB3_PCS_MISC_OSC_DTCT_CONFIG1_REG,
				      PCIE_USB3_PCS_MISC_OSC_DTCT_CONFIG1_VAL,
				      PCIE_USB3_PCS_MISC_OSC_DTCT_CONFIG_MSK);
	if (!ret) {
		ath11k_warn(ab, "failed to set dtct config1 error: %d\n", ret);
		return ret;
	}

	ret = ath11k_pci_set_link_reg(ab,
				      PCIE_USB3_PCS_MISC_OSC_DTCT_CONFIG2_REG,
				      PCIE_USB3_PCS_MISC_OSC_DTCT_CONFIG2_VAL,
				      PCIE_USB3_PCS_MISC_OSC_DTCT_CONFIG_MSK);
	if (!ret) {
		ath11k_warn(ab, "failed to set dtct config2: %d\n", ret);
		return ret;
	}

	ret = ath11k_pci_set_link_reg(ab,
				      PCIE_USB3_PCS_MISC_OSC_DTCT_CONFIG4_REG,
				      PCIE_USB3_PCS_MISC_OSC_DTCT_CONFIG4_VAL,
				      PCIE_USB3_PCS_MISC_OSC_DTCT_CONFIG_MSK);
	if (!ret) {
		ath11k_warn(ab, "failed to set dtct config4: %d\n", ret);
		return ret;
	}

	return 0;
}

static void ath11k_pci_enable_ltssm(struct ath11k_base *ab)
{
	u32 val;
	int i;

	val = ath11k_pci_read32(ab, PCIE_PCIE_PARF_LTSSM);

	/* PCIE link seems very unstable after the Hot Reset*/
	for (i = 0; val != PARM_LTSSM_VALUE && i < 5; i++) {
		if (val == 0xffffffff)
			mdelay(5);

		ath11k_pci_write32(ab, PCIE_PCIE_PARF_LTSSM, PARM_LTSSM_VALUE);
		val = ath11k_pci_read32(ab, PCIE_PCIE_PARF_LTSSM);
	}

	ath11k_dbg(ab, ATH11K_DBG_PCI, "pci ltssm 0x%x\n", val);

	val = ath11k_pci_read32(ab, GCC_GCC_PCIE_HOT_RST);
	val |= GCC_GCC_PCIE_HOT_RST_VAL | 0x10;
	ath11k_pci_write32(ab, GCC_GCC_PCIE_HOT_RST, val);
	val = ath11k_pci_read32(ab, GCC_GCC_PCIE_HOT_RST);

	ath11k_dbg(ab, ATH11K_DBG_PCI, "pci pcie_hot_rst 0x%x\n", val);

	mdelay(5);
}

static void ath11k_pci_clear_all_intrs(struct ath11k_base *ab)
{
	/* This is a WAR for PCIE Hotreset.
	 * When target receive Hotreset, but will set the interrupt.
	 * So when download SBL again, SBL will open Interrupt and
	 * receive it, and crash immediately.
	 */
	ath11k_pci_write32(ab, PCIE_PCIE_INT_ALL_CLEAR, PCIE_INT_CLEAR_ALL);
}

static void ath11k_pci_set_wlaon_pwr_ctrl(struct ath11k_base *ab)
{
	u32 val;

	val = ath11k_pci_read32(ab, WLAON_QFPROM_PWR_CTRL_REG);
	val &= ~QFPROM_PWR_CTRL_VDD4BLOW_MASK;
	ath11k_pci_write32(ab, WLAON_QFPROM_PWR_CTRL_REG, val);
}

static void ath11k_pci_force_wake(struct ath11k_base *ab)
{
	ath11k_pci_write32(ab, PCIE_SOC_WAKE_PCIE_LOCAL_REG, 1);
	mdelay(5);
}

static void ath11k_pci_sw_reset(struct ath11k_base *ab, bool power_on)
{
	if (power_on) {
		ath11k_pci_enable_ltssm(ab);
		ath11k_pci_clear_all_intrs(ab);
		ath11k_pci_set_wlaon_pwr_ctrl(ab);
		ath11k_pci_fix_l1ss(ab);
	}

	ath11k_mhi_clear_vector(ab);
	ath11k_pci_soc_global_reset(ab);
	ath11k_mhi_set_mhictrl_reset(ab);
	ath11k_pci_clear_dbg_registers(ab);
}

static int ath11k_pci_enable_msi(struct ath11k_pci *ab_pci)
{
	struct ath11k_base *ab = ab_pci->ab;
	const struct ath11k_msi_config *msi_config = ab->msi.msi_config;
	struct pci_dev *pci_dev = ab_pci->pdev;
	struct msi_desc *msi_desc;
	int num_vectors;
	int ret;

	num_vectors = pci_alloc_irq_vectors(pci_dev,
					    msi_config->total_vectors,
					    msi_config->total_vectors,
					    PCI_IRQ_MSI);
	if (num_vectors != msi_config->total_vectors) {
		ath11k_err(ab, "failed to get %d MSI vectors, only %d available",
			   msi_config->total_vectors, num_vectors);

		if (num_vectors >= 0)
			return -EINVAL;
		else
			return num_vectors;
	}

	msi_desc = irq_get_msi_desc(ab_pci->pdev->irq);
	if (!msi_desc) {
		ath11k_err(ab, "msi_desc is NULL!\n");
		ret = -EINVAL;
		goto free_msi_vector;
	}

	ab->msi.msi_ep_base_data = msi_desc->msg.data;

	pci_read_config_dword(pci_dev, pci_dev->msi_cap + PCI_MSI_ADDRESS_LO,
			      &ab->msi.msi_addr_lo);

	if (msi_desc->msi_attrib.is_64) {
		pci_read_config_dword(pci_dev, pci_dev->msi_cap + PCI_MSI_ADDRESS_HI,
				      &ab->msi.msi_addr_hi);
	} else {
		ab->msi.msi_addr_hi = 0;
	}

	ath11k_dbg(ab, ATH11K_DBG_PCI, "msi base data is %d\n", ab->msi.msi_ep_base_data);

	return 0;

free_msi_vector:
	pci_free_irq_vectors(ab_pci->pdev);

	return ret;
}

static void ath11k_pci_disable_msi(struct ath11k_pci *ab_pci)
{
	pci_free_irq_vectors(ab_pci->pdev);
}

static int ath11k_pci_claim(struct ath11k_pci *ab_pci, struct pci_dev *pdev)
{
	struct ath11k_base *ab = ab_pci->ab;
	u16 device_id;
	int ret = 0;

	pci_read_config_word(pdev, PCI_DEVICE_ID, &device_id);
	if (device_id != ab_pci->dev_id)  {
		ath11k_err(ab, "pci device id mismatch: 0x%x 0x%x\n",
			   device_id, ab_pci->dev_id);
		ret = -EIO;
		goto out;
	}

	ret = pci_assign_resource(pdev, ATH11K_PCI_BAR_NUM);
	if (ret) {
		ath11k_err(ab, "failed to assign pci resource: %d\n", ret);
		goto out;
	}

	ret = pci_enable_device(pdev);
	if (ret) {
		ath11k_err(ab, "failed to enable pci device: %d\n", ret);
		goto out;
	}

	ret = pci_request_region(pdev, ATH11K_PCI_BAR_NUM, "ath11k_pci");
	if (ret) {
		ath11k_err(ab, "failed to request pci region: %d\n", ret);
		goto disable_device;
	}

	ret = pci_set_dma_mask(pdev, DMA_BIT_MASK(ATH11K_PCI_DMA_MASK));
	if (ret) {
		ath11k_err(ab, "failed to set pci dma mask to %d: %d\n",
			   ATH11K_PCI_DMA_MASK, ret);
		goto release_region;
	}

	ret = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(ATH11K_PCI_DMA_MASK));
	if (ret) {
		ath11k_err(ab, "failed to set pci consistent dma mask to %d: %d\n",
			   ATH11K_PCI_DMA_MASK, ret);
		goto release_region;
	}

	pci_set_master(pdev);

	ab->mem_len = pci_resource_len(pdev, ATH11K_PCI_BAR_NUM);
	ab->mem = pci_iomap(pdev, ATH11K_PCI_BAR_NUM, 0);
	if (!ab->mem) {
		ath11k_err(ab, "failed to map pci bar %d\n", ATH11K_PCI_BAR_NUM);
		ret = -EIO;
		goto clear_master;
	}

	ath11k_dbg(ab, ATH11K_DBG_BOOT, "boot pci_mem 0x%pK\n", ab->mem);
	return 0;

clear_master:
	pci_clear_master(pdev);
release_region:
	pci_release_region(pdev, ATH11K_PCI_BAR_NUM);
disable_device:
	pci_disable_device(pdev);
out:
	return ret;
}

static void ath11k_pci_free_region(struct ath11k_pci *ab_pci)
{
	struct ath11k_base *ab = ab_pci->ab;
	struct pci_dev *pci_dev = ab_pci->pdev;

	pci_iounmap(pci_dev, ab->mem);
	ab->mem = NULL;
	pci_clear_master(pci_dev);
	pci_release_region(pci_dev, ATH11K_PCI_BAR_NUM);
	if (pci_is_enabled(pci_dev))
		pci_disable_device(pci_dev);
}

static int ath11k_pci_power_up(struct ath11k_base *ab)
{
	struct ath11k_pci *ab_pci = ath11k_pci_priv(ab);
	int ret;

	ab_pci->register_window = 0;
	clear_bit(ATH11K_FLAG_DEV_INIT_DONE, &ab->dev_flags);
	ath11k_pci_sw_reset(ab_pci->ab, true);

	ret = ath11k_mhi_start(ab_pci);
	if (ret) {
		ath11k_err(ab, "failed to start mhi: %d\n", ret);
		return ret;
	}

	if (ab->bus_params.static_window_map)
		ath11k_pci_select_static_window(ab);

	return 0;
}

static void ath11k_pci_power_down(struct ath11k_base *ab)
{
	struct ath11k_pci *ab_pci = ath11k_pci_priv(ab);

	ath11k_pci_force_wake(ab_pci->ab);
	ath11k_mhi_stop(ab_pci);
	clear_bit(ATH11K_FLAG_DEV_INIT_DONE, &ab->dev_flags);
	ath11k_pci_sw_reset(ab_pci->ab, false);
}

static int ath11k_pci_hif_suspend(struct ath11k_base *ab)
{
	struct ath11k_pci *ar_pci = ath11k_pci_priv(ab);

	ath11k_mhi_suspend(ar_pci);

	return 0;
}

static int ath11k_pci_hif_resume(struct ath11k_base *ab)
{
	struct ath11k_pci *ar_pci = ath11k_pci_priv(ab);

	ath11k_mhi_resume(ar_pci);

	return 0;
}

static void ath11k_pci_hif_ce_irq_enable(struct ath11k_base *ab)
{
	ath11k_pci_ce_irqs_enable(ab);
}

static void ath11k_pci_hif_ce_irq_disable(struct ath11k_base *ab)
{
	ath11k_pci_ce_irq_disable_sync(ab);
}

static const struct ath11k_hif_ops ath11k_pci_hif_ops = {
	.start = ath11k_pci_start,
	.stop = ath11k_pci_stop,
	.read32 = ath11k_pci_read32,
	.write32 = ath11k_pci_write32,
	.power_down = ath11k_pci_power_down,
	.power_up = ath11k_pci_power_up,
	.suspend = ath11k_pci_hif_suspend,
	.resume = ath11k_pci_hif_resume,
	.irq_enable = ath11k_pci_ext_irq_enable,
	.irq_disable = ath11k_pci_ext_irq_disable,
	.get_msi_address =  ath11k_pci_get_msi_address,
	.get_user_msi_vector = ath11k_pci_get_user_msi_assignment,
	.map_service_to_pipe = ath11k_pci_map_service_to_pipe,
	.ce_irq_enable = ath11k_pci_hif_ce_irq_enable,
	.ce_irq_disable = ath11k_pci_hif_ce_irq_disable,
	.get_ce_msi_idx = ath11k_pci_get_ce_msi_idx,
};

static int ath11k_pci_probe(struct pci_dev *pdev,
			    const struct pci_device_id *pci_dev)
{
	struct ath11k_base *ab;
	struct ath11k_pci *ab_pci;
	u32 soc_hw_version, soc_hw_version_major, soc_hw_version_minor;
	int ret;

	dev_warn(&pdev->dev, "WARNING: ath11k PCI support is experimental!\n");

	ab = ath11k_core_alloc(&pdev->dev, sizeof(*ab_pci), ATH11K_BUS_PCI,
			       &ath11k_pci_bus_params);
	if (!ab) {
		dev_err(&pdev->dev, "failed to allocate ath11k base\n");
		return -ENOMEM;
	}

	ab->dev = &pdev->dev;
	pci_set_drvdata(pdev, ab);
	ab_pci = ath11k_pci_priv(ab);
	ab_pci->dev_id = pci_dev->device;
	ab_pci->ab = ab;
	ab_pci->pdev = pdev;
	ab->hif.ops = &ath11k_pci_hif_ops;
	pci_set_drvdata(pdev, ab);
	spin_lock_init(&ab_pci->window_lock);

	ret = ath11k_pci_claim(ab_pci, pdev);
	if (ret) {
		ath11k_err(ab, "failed to claim device: %d\n", ret);
		goto err_free_core;
	}

	switch (pci_dev->device) {
	case QCA6390_DEVICE_ID:
		soc_hw_version = ath11k_pci_read32(ab, TCSR_SOC_HW_VERSION);
		soc_hw_version_major = FIELD_GET(TCSR_SOC_HW_VERSION_MAJOR_MASK,
						 soc_hw_version);
		soc_hw_version_minor = FIELD_GET(TCSR_SOC_HW_VERSION_MINOR_MASK,
						 soc_hw_version);

		ath11k_dbg(ab, ATH11K_DBG_PCI, "pci tcsr_soc_hw_version major %d minor %d\n",
			   soc_hw_version_major, soc_hw_version_minor);

		switch (soc_hw_version_major) {
		case 2:
			ab->hw_rev = ATH11K_HW_QCA6390_HW20;
			break;
		default:
			dev_err(&pdev->dev, "Unsupported QCA6390 SOC hardware version: %d %d\n",
				soc_hw_version_major, soc_hw_version_minor);
			ret = -EOPNOTSUPP;
			goto err_pci_free_region;
		}
		break;
	case QCN9074_DEVICE_ID:
		ab->bus_params.static_window_map = true;
		/* For QCN9074, CE: 2nd window, UMAC: 3rd window */
		ab->bus_params.ce_window_idx = 2;
		ab->bus_params.dp_window_idx = 3;
		ab->hw_rev = ATH11K_HW_QCN9074_HW10;
		break;
	default:
		dev_err(&pdev->dev, "Unknown PCI device found: 0x%x\n",
			pci_dev->device);
		ret = -EOPNOTSUPP;
		goto err_pci_free_region;
	}

	ret = ath11k_pci_get_msi_config(ab);
	if (ret) {
		ath11k_err(ab, "failed to fetch msi config: %d\n", ret);
		goto err_pci_free_region;
	}

	ret = ath11k_pci_enable_msi(ab_pci);
	if (ret) {
		ath11k_err(ab, "failed to enable msi: %d\n", ret);
		goto err_pci_free_region;
	}

	ret = ath11k_core_pre_init(ab);
	if (ret)
		goto err_pci_disable_msi;

	ret = ath11k_mhi_register(ab_pci);
	if (ret) {
		ath11k_err(ab, "failed to register mhi: %d\n", ret);
		goto err_pci_disable_msi;
	}

	ret = ath11k_hal_srng_init(ab);
	if (ret)
		goto err_mhi_unregister;

	ret = ath11k_ce_alloc_pipes(ab);
	if (ret) {
		ath11k_err(ab, "failed to allocate ce pipes: %d\n", ret);
		goto err_hal_srng_deinit;
	}

	ath11k_ce_init_qmi_ce_config(ab);

	ret = ath11k_pci_config_irq(ab);
	if (ret) {
		ath11k_err(ab, "failed to config irq: %d\n", ret);
		goto err_ce_free;
	}

	ret = ath11k_core_init(ab);
	if (ret) {
		ath11k_err(ab, "failed to init core: %d\n", ret);
		goto err_free_irq;
	}
	return 0;

err_free_irq:
	ath11k_pci_free_irq(ab);

err_ce_free:
	ath11k_ce_free_pipes(ab);

err_hal_srng_deinit:
	ath11k_hal_srng_deinit(ab);

err_mhi_unregister:
	ath11k_mhi_unregister(ab_pci);

err_pci_disable_msi:
	ath11k_pci_disable_msi(ab_pci);

err_pci_free_region:
	ath11k_pci_free_region(ab_pci);

err_free_core:
	ath11k_core_free(ab);

	return ret;
}

static void ath11k_pci_remove(struct pci_dev *pdev)
{
	struct ath11k_base *ab = pci_get_drvdata(pdev);
	struct ath11k_pci *ab_pci = ath11k_pci_priv(ab);

	if (test_bit(ATH11K_FLAG_QMI_FAIL, &ab->dev_flags)) {
		ath11k_pci_power_down(ab);
		ath11k_debugfs_soc_destroy(ab);
		ath11k_qmi_deinit_service(ab);
		goto qmi_fail;
	}

	set_bit(ATH11K_FLAG_UNREGISTERING, &ab->dev_flags);

	ath11k_core_deinit(ab);

qmi_fail:
	ath11k_mhi_unregister(ab_pci);

	ath11k_pci_free_irq(ab);
	ath11k_pci_disable_msi(ab_pci);
	ath11k_pci_free_region(ab_pci);

	ath11k_hal_srng_deinit(ab);
	ath11k_ce_free_pipes(ab);
	ath11k_core_free(ab);
}

static void ath11k_pci_shutdown(struct pci_dev *pdev)
{
	struct ath11k_base *ab = pci_get_drvdata(pdev);

	ath11k_pci_power_down(ab);
}

static __maybe_unused int ath11k_pci_pm_suspend(struct device *dev)
{
	struct ath11k_base *ab = dev_get_drvdata(dev);
	int ret;

	ret = ath11k_core_suspend(ab);
	if (ret)
		ath11k_warn(ab, "failed to suspend core: %d\n", ret);

	return ret;
}

static __maybe_unused int ath11k_pci_pm_resume(struct device *dev)
{
	struct ath11k_base *ab = dev_get_drvdata(dev);
	int ret;

	ret = ath11k_core_resume(ab);
	if (ret)
		ath11k_warn(ab, "failed to resume core: %d\n", ret);

	return ret;
}

static SIMPLE_DEV_PM_OPS(ath11k_pci_pm_ops,
			 ath11k_pci_pm_suspend,
			 ath11k_pci_pm_resume);

static struct pci_driver ath11k_pci_driver = {
	.name = "ath11k_pci",
	.id_table = ath11k_pci_id_table,
	.probe = ath11k_pci_probe,
	.remove = ath11k_pci_remove,
	.shutdown = ath11k_pci_shutdown,
#ifdef CONFIG_PM
	.driver.pm = &ath11k_pci_pm_ops,
#endif
};

static int ath11k_pci_init(void)
{
	int ret;

	ret = pci_register_driver(&ath11k_pci_driver);
	if (ret)
		pr_err("failed to register ath11k pci driver: %d\n",
		       ret);

	return ret;
}
module_init(ath11k_pci_init);

static void ath11k_pci_exit(void)
{
	pci_unregister_driver(&ath11k_pci_driver);
}

module_exit(ath11k_pci_exit);

MODULE_DESCRIPTION("Driver support for Qualcomm Technologies 802.11ax WLAN PCIe devices");
MODULE_LICENSE("Dual BSD/GPL");

/* QCA639x 2.0 firmware files */
MODULE_FIRMWARE(ATH11K_FW_DIR "/QCA6390/hw2.0/" ATH11K_BOARD_API2_FILE);
MODULE_FIRMWARE(ATH11K_FW_DIR "/QCA6390/hw2.0/" ATH11K_AMSS_FILE);
MODULE_FIRMWARE(ATH11K_FW_DIR "/QCA6390/hw2.0/" ATH11K_M3_FILE);
