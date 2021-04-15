/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014-2015, The Linux Foundation. All rights reserved.
 */

#ifndef __EDP_CONNECTOR_H__
#define __EDP_CONNECTOR_H__

#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <drm/drm_bridge.h>
#include <drm/drm_crtc.h>
#include <drm/drm_dp_helper.h>

#include "msm_drv.h"
#include "msm_kms.h"
#include "dpu_io_util.h"

#define MSM_EDP_CONTROLLER_AHB_OFFSET	0x0000
#define MSM_EDP_CONTROLLER_AHB_SIZE	0x0200
#define MSM_EDP_CONTROLLER_AUX_OFFSET	0x0200
#define MSM_EDP_CONTROLLER_AUX_SIZE	0x0200
#define MSM_EDP_CONTROLLER_LINK_OFFSET	0x0400
#define MSM_EDP_CONTROLLER_LINK_SIZE	0x0C00
#define MSM_EDP_CONTROLLER_P0_OFFSET	0x1000
#define MSM_EDP_CONTROLLER_P0_SIZE	0x0400

static inline u32 edp_read_aux(void *base, u32 offset)
{
	offset += MSM_EDP_CONTROLLER_AUX_OFFSET;
	return readl_relaxed(base + offset);
}

static inline void edp_write_aux(void *base, u32 offset, u32 data)
{
	offset += MSM_EDP_CONTROLLER_AUX_OFFSET;
	/*
	 * To make sure aux reg writes happens before any other operation,
	 * this function uses writel() instread of writel_relaxed()
	 */
	writel(data, base + offset);
}

static inline u32 edp_read_ahb(void *base, u32 offset)
{
	offset += MSM_EDP_CONTROLLER_AHB_OFFSET;
	return readl_relaxed(base + offset);
}

static inline void edp_write_ahb(void *base, u32 offset, u32 data)
{
	offset += MSM_EDP_CONTROLLER_AHB_OFFSET;
	/*
	 * To make sure ahb reg writes happens before any other operation,
	 * this function uses writel() instread of writel_relaxed()
	 */
	writel(data, base + offset);
}

static inline void edp_write_p0(void *base, u32 offset, u32 data)
{
	offset += MSM_EDP_CONTROLLER_P0_OFFSET;
	/*
	 * To make sure pclk reg writes happens before any other operation,
	 * this function uses writel() instread of writel_relaxed()
	 */
	writel(data, base + offset);
}

static inline u32 edp_read_p0(void *base, u32 offset)
{
	offset += MSM_EDP_CONTROLLER_P0_OFFSET;
	return readl_relaxed(base + offset);
}

static inline u32 edp_read_link(void *base, u32 offset)
{
	offset += MSM_EDP_CONTROLLER_LINK_OFFSET;
	return readl_relaxed(base + offset);
}

static inline void edp_write_link(void *base, u32 offset, u32 data)
{
	offset += MSM_EDP_CONTROLLER_LINK_OFFSET;
	/*
	 * To make sure link reg writes happens before any other operation,
	 * this function uses writel() instread of writel_relaxed()
	 */
	writel(data, base + offset);
}

struct edp_ctrl;
struct edp_aux;
struct edp_phy;
struct edp_phy_opts {
	unsigned long link_rate;
	int lanes;
	int voltage[4];
	int pre[4];
};


struct msm_edp {
	struct drm_device *dev;
	struct platform_device *pdev;

	struct drm_connector *connector;
	struct drm_bridge *bridge;

	/* the encoder we are hooked to (outside of eDP block) */
	struct drm_encoder *encoder;

	struct edp_ctrl *ctrl;

	int irq;
	bool encoder_mode_set;
};

/* eDP bridge */
struct drm_bridge *msm_edp_bridge_init(struct msm_edp *edp);
void edp_bridge_destroy(struct drm_bridge *bridge);

/* eDP connector */
struct drm_connector *msm_edp_connector_init(struct msm_edp *edp);

/* AUX */
void *msm_edp_aux_init(struct device *dev, void __iomem *regbase,
			struct drm_dp_aux **drm_aux);
void msm_edp_aux_destroy(struct device *dev, struct edp_aux *aux);
irqreturn_t msm_edp_aux_irq(struct edp_aux *aux, u32 isr);
void msm_edp_aux_ctrl(struct edp_aux *aux, int enable);

/* Phy */
int msm_edp_phy_enable(struct edp_phy *edp_phy);
void msm_edp_phy_vm_pe_init(struct edp_phy *edp_phy, struct edp_phy_opts *opts);
void *msm_edp_phy_init(struct device *dev, void __iomem *regbase,
			struct edp_phy_opts *opts);
int msm_edp_phy_power_on(struct edp_phy *edp_phy);
void msm_edp_phy_config(struct edp_phy *edp_phy, u8 v_level, u8 p_level);

/* Ctrl */
irqreturn_t msm_edp_ctrl_irq(struct edp_ctrl *ctrl);
void msm_edp_ctrl_power(struct edp_ctrl *ctrl, bool on);
int msm_edp_ctrl_init(struct msm_edp *edp);
void msm_edp_ctrl_destroy(struct edp_ctrl *ctrl);
bool msm_edp_ctrl_panel_connected(struct edp_ctrl *ctrl);
int msm_edp_ctrl_get_panel_info(struct edp_ctrl *ctrl,
	struct drm_connector *connector, struct edid **edid);
int msm_edp_ctrl_mode_set(struct edp_ctrl *ctrl,
				const struct drm_display_mode *mode,
				const struct drm_display_info *info);
/* @pixel_rate is in kHz */
bool msm_edp_ctrl_pixel_clock_valid(struct edp_ctrl *ctrl, u32 pixel_rate);

#endif /* __EDP_CONNECTOR_H__ */
