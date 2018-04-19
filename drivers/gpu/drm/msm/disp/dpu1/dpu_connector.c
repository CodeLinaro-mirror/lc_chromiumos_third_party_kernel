/* Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt)	"[drm:%s:%d] " fmt, __func__, __LINE__
#include "msm_drv.h"
#include "dpu_dbg.h"

#include "dpu_kms.h"
#include "dpu_connector.h"
#ifdef CONFIG_DRM_MSM_DSI_STAGING
#include <linux/backlight.h>
#include "dsi_drm.h"
#include "dsi_display.h"
#endif

#define BL_NODE_NAME_SIZE 32

/* Autorefresh will occur after FRAME_CNT frames. Large values are unlikely */
#define AUTOREFRESH_MAX_FRAME_CNT 6

#define DPU_DEBUG_CONN(c, fmt, ...) DPU_DEBUG("conn%d " fmt,\
		(c) ? (c)->base.base.id : -1, ##__VA_ARGS__)

#define DPU_ERROR_CONN(c, fmt, ...) DPU_ERROR("conn%d " fmt,\
		(c) ? (c)->base.base.id : -1, ##__VA_ARGS__)

static const struct drm_prop_enum_list e_topology_name[] = {
	{DPU_RM_TOPOLOGY_NONE,	"dpu_none"},
	{DPU_RM_TOPOLOGY_SINGLEPIPE,	"dpu_singlepipe"},
	{DPU_RM_TOPOLOGY_DUALPIPE,	"dpu_dualpipe"},
	{DPU_RM_TOPOLOGY_DUALPIPE_3DMERGE,	"dpu_dualpipemerge"},
	{DPU_RM_TOPOLOGY_PPSPLIT,	"dpu_ppsplit"},
};
static const struct drm_prop_enum_list e_topology_control[] = {
	{DPU_RM_TOPCTL_RESERVE_LOCK,	"reserve_lock"},
	{DPU_RM_TOPCTL_RESERVE_CLEAR,	"reserve_clear"},
	{DPU_RM_TOPCTL_DSPP,		"dspp"},
};
static const struct drm_prop_enum_list e_power_mode[] = {
	{DPU_MODE_DPMS_ON,	"ON"},
	{DPU_MODE_DPMS_LP1,	"LP1"},
	{DPU_MODE_DPMS_LP2,	"LP2"},
	{DPU_MODE_DPMS_OFF,	"OFF"},
};

#ifdef CONFIG_DRM_MSM_DSI_STAGING
static int dpu_backlight_device_update_status(struct backlight_device *bd)
{
	int brightness;
	struct dsi_display *display;
	struct dpu_connector *c_conn;
	int bl_lvl;
	struct drm_event event;

	brightness = bd->props.brightness;

	if ((bd->props.power != FB_BLANK_UNBLANK) ||
			(bd->props.state & BL_CORE_FBBLANK) ||
			(bd->props.state & BL_CORE_SUSPENDED))
		brightness = 0;

	c_conn = bl_get_data(bd);
	display = (struct dsi_display *) c_conn->display;
	if (brightness > display->panel->bl_config.bl_max_level)
		brightness = display->panel->bl_config.bl_max_level;

	/* map UI brightness into driver backlight level with rounding */
	bl_lvl = mult_frac(brightness, display->panel->bl_config.bl_max_level,
			display->panel->bl_config.brightness_max_level);

	if (!bl_lvl && brightness)
		bl_lvl = 1;

	if (c_conn->ops.set_backlight) {
		event.type = DRM_EVENT_SYS_BACKLIGHT;
		event.length = sizeof(u32);
		msm_mode_object_event_notify(&c_conn->base.base,
				c_conn->base.dev, &event, (u8 *)&brightness);
		c_conn->ops.set_backlight(c_conn->display, bl_lvl);
	}

	return 0;
}

static int dpu_backlight_device_get_brightness(struct backlight_device *bd)
{
	return 0;
}

static const struct backlight_ops dpu_backlight_device_ops = {
	.update_status = dpu_backlight_device_update_status,
	.get_brightness = dpu_backlight_device_get_brightness,
};

static int dpu_backlight_setup(struct dpu_connector *c_conn,
					struct drm_device *dev)
{
	struct backlight_properties props;
	struct dsi_display *display;
	struct dsi_backlight_config *bl_config;
	static int display_count;
	char bl_node_name[BL_NODE_NAME_SIZE];

	if (!c_conn || !dev || !dev->dev) {
		DPU_ERROR("invalid param\n");
		return -EINVAL;
	} else if (c_conn->connector_type != DRM_MODE_CONNECTOR_DSI) {
		return 0;
	}

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.power = FB_BLANK_UNBLANK;

	display = (struct dsi_display *) c_conn->display;
	bl_config = &display->panel->bl_config;
	props.max_brightness = bl_config->brightness_max_level;
	props.brightness = bl_config->brightness_max_level;
	snprintf(bl_node_name, BL_NODE_NAME_SIZE, "panel%u-backlight",
							display_count);
	c_conn->bl_device = backlight_device_register(bl_node_name, dev->dev,
			c_conn, &dpu_backlight_device_ops, &props);
	if (IS_ERR_OR_NULL(c_conn->bl_device)) {
		DPU_ERROR("Failed to register backlight: %ld\n",
				    PTR_ERR(c_conn->bl_device));
		c_conn->bl_device = NULL;
		return -ENODEV;
	}
	display_count++;

	return 0;
}
#endif

int dpu_connector_trigger_event(void *drm_connector,
		uint32_t event_idx, uint32_t instance_idx,
		uint32_t data0, uint32_t data1,
		uint32_t data2, uint32_t data3)
{
	struct dpu_connector *c_conn;
	unsigned long irq_flags;
	void (*cb_func)(uint32_t event_idx,
			uint32_t instance_idx, void *usr,
			uint32_t data0, uint32_t data1,
			uint32_t data2, uint32_t data3);
	void *usr;
	int rc = 0;

	/*
	 * This function may potentially be called from an ISR context, so
	 * avoid excessive logging/etc.
	 */
	if (!drm_connector)
		return -EINVAL;
	else if (event_idx >= DPU_CONN_EVENT_COUNT)
		return -EINVAL;
	c_conn = to_dpu_connector(drm_connector);

	spin_lock_irqsave(&c_conn->event_lock, irq_flags);
	cb_func = c_conn->event_table[event_idx].cb_func;
	usr = c_conn->event_table[event_idx].usr;
	spin_unlock_irqrestore(&c_conn->event_lock, irq_flags);

	if (cb_func)
		cb_func(event_idx, instance_idx, usr,
			data0, data1, data2, data3);
	else
		rc = -EAGAIN;

	return rc;
}

int dpu_connector_register_event(struct drm_connector *connector,
		uint32_t event_idx,
		void (*cb_func)(uint32_t event_idx,
			uint32_t instance_idx, void *usr,
			uint32_t data0, uint32_t data1,
			uint32_t data2, uint32_t data3),
		void *usr)
{
	struct dpu_connector *c_conn;
	unsigned long irq_flags;

	if (!connector) {
		DPU_ERROR("invalid connector\n");
		return -EINVAL;
	} else if (event_idx >= DPU_CONN_EVENT_COUNT) {
		DPU_ERROR("conn%d, invalid event %d\n",
				connector->base.id, event_idx);
		return -EINVAL;
	}
	c_conn = to_dpu_connector(connector);

	spin_lock_irqsave(&c_conn->event_lock, irq_flags);
	c_conn->event_table[event_idx].cb_func = cb_func;
	c_conn->event_table[event_idx].usr = usr;
	spin_unlock_irqrestore(&c_conn->event_lock, irq_flags);

	/* optionally notify display of event registration */
	if (c_conn->ops.enable_event && c_conn->display)
		c_conn->ops.enable_event(connector, event_idx,
				cb_func != NULL, c_conn->display);
	return 0;
}

void dpu_connector_unregister_event(struct drm_connector *connector,
		uint32_t event_idx)
{
	(void)dpu_connector_register_event(connector, event_idx, 0, 0);
}

#ifdef CONFIG_DRM_MSM_DSI_STAGING
static u32 dither_matrix[DITHER_MATRIX_SZ] = {
	15, 7, 13, 5, 3, 11, 1, 9, 12, 4, 14, 6, 0, 8, 2, 10
};

static int _dpu_connector_get_default_dither_cfg_v1(
		struct dpu_connector *c_conn, void *cfg)
{
	struct drm_msm_dither *dither_cfg = (struct drm_msm_dither *)cfg;
	enum dsi_pixel_format dst_format = DSI_PIXEL_FORMAT_MAX;

	if (!c_conn || !cfg) {
		DPU_ERROR("invalid argument(s), c_conn %pK, cfg %pK\n",
				c_conn, cfg);
		return -EINVAL;
	}

	if (!c_conn->ops.get_dst_format) {
		DPU_DEBUG("get_dst_format is unavailable\n");
		return 0;
	}

	dst_format = c_conn->ops.get_dst_format(c_conn->display);
	switch (dst_format) {
	case DSI_PIXEL_FORMAT_RGB888:
		dither_cfg->c0_bitdepth = 8;
		dither_cfg->c1_bitdepth = 8;
		dither_cfg->c2_bitdepth = 8;
		dither_cfg->c3_bitdepth = 8;
		break;
	case DSI_PIXEL_FORMAT_RGB666:
	case DSI_PIXEL_FORMAT_RGB666_LOOSE:
		dither_cfg->c0_bitdepth = 6;
		dither_cfg->c1_bitdepth = 6;
		dither_cfg->c2_bitdepth = 6;
		dither_cfg->c3_bitdepth = 6;
		break;
	default:
		DPU_DEBUG("no default dither config for dst_format %d\n",
			dst_format);
		return -ENODATA;
	}

	memcpy(&dither_cfg->matrix, dither_matrix,
			sizeof(u32) * DITHER_MATRIX_SZ);
	dither_cfg->temporal_en = 0;
	return 0;
}

static void _dpu_connector_install_dither_property(struct drm_device *dev,
		struct dpu_kms *dpu_kms, struct dpu_connector *c_conn)
{
	char prop_name[DRM_PROP_NAME_LEN];
	struct dpu_mdss_cfg *catalog = NULL;
	struct drm_property_blob *blob_ptr;
	void *cfg;
	int ret = 0;
	u32 version = 0, len = 0;
	bool defalut_dither_needed = false;

	if (!dev || !dpu_kms || !c_conn) {
		DPU_ERROR("invld args (s), dev %pK, dpu_kms %pK, c_conn %pK\n",
				dev, dpu_kms, c_conn);
		return;
	}

	catalog = dpu_kms->catalog;
	version = DPU_COLOR_PROCESS_MAJOR(
			catalog->pingpong[0].sblk->dither.version);
	snprintf(prop_name, ARRAY_SIZE(prop_name), "%s%d",
			"DPU_PP_DITHER_V", version);
	switch (version) {
	case 1:
		msm_property_install_blob(&c_conn->property_info, prop_name,
			DRM_MODE_PROP_BLOB,
			CONNECTOR_PROP_PP_DITHER);
		len = sizeof(struct drm_msm_dither);
		cfg = kzalloc(len, GFP_KERNEL);
		if (!cfg)
			return;

		ret = _dpu_connector_get_default_dither_cfg_v1(c_conn, cfg);
		if (!ret)
			defalut_dither_needed = true;
		break;
	default:
		DPU_ERROR("unsupported dither version %d\n", version);
		return;
	}

	if (defalut_dither_needed) {
		blob_ptr = drm_property_create_blob(dev, len, cfg);
		if (IS_ERR_OR_NULL(blob_ptr))
			goto exit;
		c_conn->blob_dither = blob_ptr;
	}
exit:
	kfree(cfg);
}
#endif

int dpu_connector_get_dither_cfg(struct drm_connector *conn,
			struct drm_connector_state *state, void **cfg,
			size_t *len)
{
	struct dpu_connector *c_conn = NULL;
	struct dpu_connector_state *c_state = NULL;
	size_t dither_sz = 0;

	if (!conn || !state || !(*cfg))
		return -EINVAL;

	c_conn = to_dpu_connector(conn);
	c_state = to_dpu_connector_state(state);

	/* try to get user config data first */
	*cfg = msm_property_get_blob(&c_conn->property_info,
					&c_state->property_state,
					&dither_sz,
					CONNECTOR_PROP_PP_DITHER);
	/* if user config data doesn't exist, use default dither blob */
	if (*cfg == NULL && c_conn->blob_dither) {
		*cfg = &c_conn->blob_dither->data;
		dither_sz = c_conn->blob_dither->length;
	}
	*len = dither_sz;
	return 0;
}

int dpu_connector_get_info(struct drm_connector *connector,
		struct msm_display_info *info)
{
	struct dpu_connector *c_conn;

	if (!connector || !info) {
		DPU_ERROR("invalid argument(s), conn %pK, info %pK\n",
				connector, info);
		return -EINVAL;
	}

	c_conn = to_dpu_connector(connector);

	if (!c_conn->display || !c_conn->ops.get_info) {
		DPU_ERROR("display info not supported for %pK\n",
				c_conn->display);
		return -EINVAL;
	}

	return c_conn->ops.get_info(info, c_conn->display);
}

static int _dpu_connector_update_power_locked(struct dpu_connector *c_conn)
{
	struct drm_connector *connector;
	void *display;
	int (*set_power)(struct drm_connector *, int, void *);
	int mode, rc = 0;

	if (!c_conn)
		return -EINVAL;
	connector = &c_conn->base;

	switch (c_conn->dpms_mode) {
	case DRM_MODE_DPMS_ON:
		mode = c_conn->lp_mode;
		break;
	case DRM_MODE_DPMS_STANDBY:
		mode = DPU_MODE_DPMS_STANDBY;
		break;
	case DRM_MODE_DPMS_SUSPEND:
		mode = DPU_MODE_DPMS_SUSPEND;
		break;
	case DRM_MODE_DPMS_OFF:
		mode = DPU_MODE_DPMS_OFF;
		break;
	default:
		mode = c_conn->lp_mode;
		DPU_ERROR("conn %d dpms set to unrecognized mode %d\n",
				connector->base.id, mode);
		break;
	}

	DPU_EVT32(connector->base.id, c_conn->dpms_mode, c_conn->lp_mode, mode);
	DPU_DEBUG("conn %d - dpms %d, lp %d, panel %d\n", connector->base.id,
			c_conn->dpms_mode, c_conn->lp_mode, mode);

	if (mode != c_conn->last_panel_power_mode && c_conn->ops.set_power) {
		display = c_conn->display;
		set_power = c_conn->ops.set_power;

		mutex_unlock(&c_conn->lock);
		rc = set_power(connector, mode, display);
		mutex_lock(&c_conn->lock);
	}
	c_conn->last_panel_power_mode = mode;

	return rc;
}

int dpu_connector_pre_kickoff(struct drm_connector *connector)
{
	struct dpu_connector *c_conn;
	struct dpu_connector_state *c_state;
	int idx, rc = 0;

	if (!connector) {
		DPU_ERROR("invalid argument\n");
		return -EINVAL;
	}

	c_conn = to_dpu_connector(connector);
	c_state = to_dpu_connector_state(connector->state);

	if (!c_conn->display) {
		DPU_ERROR("invalid argument\n");
		return -EINVAL;
	}
#ifdef CONFIG_DRM_MSM_DSI_STAGING
	if (c_conn->ops.set_backlight) {
		DPU_DEBUG("conn%d: Set backlight to 255\n", connector->base.id);
		c_conn->ops.set_backlight(c_conn->display, 255);
	}
#endif

	while ((idx = msm_property_pop_dirty(&c_conn->property_info,
					&c_state->property_state)) >= 0) {
		switch (idx) {
		case CONNECTOR_PROP_LP:
			mutex_lock(&c_conn->lock);
			c_conn->lp_mode = dpu_connector_get_property(
					connector->state, CONNECTOR_PROP_LP);
			_dpu_connector_update_power_locked(c_conn);
			mutex_unlock(&c_conn->lock);
			break;
		default:
			/* nothing to do for most properties */
			break;
		}
	}

	DPU_EVT32_VERBOSE(connector->base.id);

	return rc;
}

void dpu_connector_clk_ctrl(struct drm_connector *connector, bool enable)
{
#ifdef CONFIG_DRM_MSM_DSI_STAGING
	struct dpu_connector *c_conn;
	struct dsi_display *display;
	u32 state = enable ? DSI_CLK_ON : DSI_CLK_OFF;

	if (!connector) {
		DPU_ERROR("invalid connector\n");
		return;
	}

	c_conn = to_dpu_connector(connector);
	display = (struct dsi_display *) c_conn->display;

	if (display && c_conn->ops.clk_ctrl)
		c_conn->ops.clk_ctrl(display->mdp_clk_handle,
				DSI_ALL_CLKS, state);
#endif
}

static void dpu_connector_destroy(struct drm_connector *connector)
{
	struct dpu_connector *c_conn;

	if (!connector) {
		DPU_ERROR("invalid connector\n");
		return;
	}

	c_conn = to_dpu_connector(connector);

	if (c_conn->ops.put_modes)
		c_conn->ops.put_modes(connector, c_conn->display);

	if (c_conn->blob_caps)
		drm_property_blob_put(c_conn->blob_caps);
	if (c_conn->blob_hdr)
		drm_property_blob_put(c_conn->blob_hdr);
	if (c_conn->blob_dither)
		drm_property_blob_put(c_conn->blob_dither);
	msm_property_destroy(&c_conn->property_info);

	if (c_conn->bl_device)
		backlight_device_unregister(c_conn->bl_device);
	drm_connector_unregister(connector);
	mutex_destroy(&c_conn->lock);
	drm_connector_cleanup(connector);
	kfree(c_conn);
}

/**
 * _dpu_connector_destroy_fb - clean up connector state's out_fb buffer
 * @c_conn: Pointer to dpu connector structure
 * @c_state: Pointer to dpu connector state structure
 */
static void _dpu_connector_destroy_fb(struct dpu_connector *c_conn,
		struct dpu_connector_state *c_state)
{
	if (!c_state || !c_state->out_fb) {
		DPU_ERROR("invalid state %pK\n", c_state);
		return;
	}

	drm_framebuffer_put(c_state->out_fb);
	c_state->out_fb = NULL;

	if (c_conn)
		c_state->property_values[CONNECTOR_PROP_OUT_FB].value =
			msm_property_get_default(&c_conn->property_info,
					CONNECTOR_PROP_OUT_FB);
	else
		c_state->property_values[CONNECTOR_PROP_OUT_FB].value = ~0;
}

static void dpu_connector_atomic_destroy_state(struct drm_connector *connector,
		struct drm_connector_state *state)
{
	struct dpu_connector *c_conn = NULL;
	struct dpu_connector_state *c_state = NULL;

	if (!state) {
		DPU_ERROR("invalid state\n");
		return;
	}

	/*
	 * The base DRM framework currently always passes in a NULL
	 * connector pointer. This is not correct, but attempt to
	 * handle that case as much as possible.
	 */
	if (connector)
		c_conn = to_dpu_connector(connector);
	c_state = to_dpu_connector_state(state);

	if (c_state->out_fb)
		_dpu_connector_destroy_fb(c_conn, c_state);

	if (!c_conn) {
		kfree(c_state);
	} else {
		/* destroy value helper */
		msm_property_destroy_state(&c_conn->property_info, c_state,
				&c_state->property_state);
	}
}

static void dpu_connector_atomic_reset(struct drm_connector *connector)
{
	struct dpu_connector *c_conn;
	struct dpu_connector_state *c_state;

	if (!connector) {
		DPU_ERROR("invalid connector\n");
		return;
	}

	c_conn = to_dpu_connector(connector);

	if (connector->state) {
		dpu_connector_atomic_destroy_state(connector, connector->state);
		connector->state = 0;
	}

	c_state = msm_property_alloc_state(&c_conn->property_info);
	if (!c_state) {
		DPU_ERROR("state alloc failed\n");
		return;
	}

	/* reset value helper, zero out state structure and reset properties */
	msm_property_reset_state(&c_conn->property_info, c_state,
			&c_state->property_state,
			c_state->property_values);

	c_state->base.connector = connector;
	connector->state = &c_state->base;
}

static struct drm_connector_state *
dpu_connector_atomic_duplicate_state(struct drm_connector *connector)
{
	struct dpu_connector *c_conn;
	struct dpu_connector_state *c_state, *c_oldstate;

	if (!connector || !connector->state) {
		DPU_ERROR("invalid connector %pK\n", connector);
		return NULL;
	}

	c_conn = to_dpu_connector(connector);
	c_oldstate = to_dpu_connector_state(connector->state);
	c_state = msm_property_alloc_state(&c_conn->property_info);
	if (!c_state) {
		DPU_ERROR("state alloc failed\n");
		return NULL;
	}

	/* duplicate value helper */
	msm_property_duplicate_state(&c_conn->property_info,
			c_oldstate, c_state,
			&c_state->property_state, c_state->property_values);

	/* additional handling for drm framebuffer objects */
	if (c_state->out_fb)
		drm_framebuffer_get(c_state->out_fb);

	return &c_state->base;
}

static int _dpu_connector_update_bl_scale(struct dpu_connector *c_conn,
		int idx,
		uint64_t value)
{
#ifdef CONFIG_DRM_MSM_DSI_STAGING
	struct dsi_display *dsi_display = c_conn->display;
	struct dsi_backlight_config *bl_config;
	int rc = 0;

	if (!dsi_display || !dsi_display->panel) {
		pr_err("Invalid params(s) dsi_display %pK, panel %pK\n",
			dsi_display,
			((dsi_display) ? dsi_display->panel : NULL));
		return -EINVAL;
	}

	bl_config = &dsi_display->panel->bl_config;
	if (idx == CONNECTOR_PROP_BL_SCALE) {
		bl_config->bl_scale = value;
		if (value > MAX_BL_SCALE_LEVEL)
			bl_config->bl_scale = MAX_BL_SCALE_LEVEL;
		DPU_DEBUG("set to panel: bl_scale = %u, bl_level = %u\n",
			bl_config->bl_scale, bl_config->bl_level);
		rc = c_conn->ops.set_backlight(dsi_display,
					       bl_config->bl_level);
	} else if (idx == CONNECTOR_PROP_AD_BL_SCALE) {
		bl_config->bl_scale_ad = value;
		if (value > MAX_AD_BL_SCALE_LEVEL)
			bl_config->bl_scale_ad = MAX_AD_BL_SCALE_LEVEL;
		DPU_DEBUG("set to panel: bl_scale_ad = %u, bl_level = %u\n",
			bl_config->bl_scale_ad, bl_config->bl_level);
		rc = c_conn->ops.set_backlight(dsi_display,
					       bl_config->bl_level);
	}
	return rc;
#else
	return 0;
#endif
}

static int dpu_connector_atomic_set_property(struct drm_connector *connector,
		struct drm_connector_state *state,
		struct drm_property *property,
		uint64_t val)
{
	struct dpu_connector *c_conn;
	struct dpu_connector_state *c_state;
	int idx, rc;

	if (!connector || !state || !property) {
		DPU_ERROR("invalid argument(s), conn %pK, state %pK, prp %pK\n",
				connector, state, property);
		return -EINVAL;
	}

	c_conn = to_dpu_connector(connector);
	c_state = to_dpu_connector_state(state);

	/* generic property handling */
	rc = msm_property_atomic_set(&c_conn->property_info,
			&c_state->property_state, property, val);
	if (rc)
		goto end;

	/* connector-specific property handling */
	idx = msm_property_index(&c_conn->property_info, property);
	switch (idx) {
	case CONNECTOR_PROP_OUT_FB:
		/* clear old fb, if present */
		if (c_state->out_fb)
			_dpu_connector_destroy_fb(c_conn, c_state);

		/* convert fb val to drm framebuffer and prepare it */
		c_state->out_fb =
			drm_framebuffer_lookup(connector->dev, NULL, val);
		if (!c_state->out_fb && val) {
			DPU_ERROR("failed to look up fb %lld\n", val);
			rc = -EFAULT;
		} else if (!c_state->out_fb && !val) {
			DPU_DEBUG("cleared fb_id\n");
			rc = 0;
		}
		break;
	case CONNECTOR_PROP_BL_SCALE:
	case CONNECTOR_PROP_AD_BL_SCALE:
		rc = _dpu_connector_update_bl_scale(c_conn, idx, val);
		break;
	default:
		break;
	}

	/* check for custom property handling */
	if (!rc && c_conn->ops.set_property) {
		rc = c_conn->ops.set_property(connector,
				state,
				idx,
				val,
				c_conn->display);

		/* potentially clean up out_fb if rc != 0 */
		if ((idx == CONNECTOR_PROP_OUT_FB) && rc)
			_dpu_connector_destroy_fb(c_conn, c_state);
	}
end:
	return rc;
}

static int dpu_connector_set_property(struct drm_connector *connector,
		struct drm_property *property,
		uint64_t val)
{
	if (!connector) {
		DPU_ERROR("invalid connector\n");
		return -EINVAL;
	}

	return dpu_connector_atomic_set_property(connector,
			connector->state, property, val);
}

static int dpu_connector_atomic_get_property(struct drm_connector *connector,
		const struct drm_connector_state *state,
		struct drm_property *property,
		uint64_t *val)
{
	struct dpu_connector *c_conn;
	struct dpu_connector_state *c_state;
	int idx, rc = -EINVAL;

	if (!connector || !state) {
		DPU_ERROR("invalid argument(s), conn %pK, state %pK\n",
				connector, state);
		return -EINVAL;
	}

	c_conn = to_dpu_connector(connector);
	c_state = to_dpu_connector_state(state);

	idx = msm_property_index(&c_conn->property_info, property);
	/* get cached property value */
	rc = msm_property_atomic_get(&c_conn->property_info,
			&c_state->property_state, property, val);

	/* allow for custom override */
	if (c_conn->ops.get_property)
		rc = c_conn->ops.get_property(connector,
				(struct drm_connector_state *)state,
				idx,
				val,
				c_conn->display);
	return rc;
}

static enum drm_connector_status
dpu_connector_detect(struct drm_connector *connector, bool force)
{
	enum drm_connector_status status = connector_status_unknown;
	struct dpu_connector *c_conn;

	if (!connector) {
		DPU_ERROR("invalid connector\n");
		return status;
	}

	c_conn = to_dpu_connector(connector);

	if (c_conn->ops.detect)
		status = c_conn->ops.detect(connector,
				force,
				c_conn->display);

	return status;
}

int dpu_connector_get_dpms(struct drm_connector *connector)
{
	struct dpu_connector *c_conn;
	int rc;

	if (!connector) {
		DPU_DEBUG("invalid connector\n");
		return DRM_MODE_DPMS_OFF;
	}

	c_conn = to_dpu_connector(connector);

	mutex_lock(&c_conn->lock);
	rc = c_conn->dpms_mode;
	mutex_unlock(&c_conn->lock);

	return rc;
}

int dpu_connector_set_property_for_commit(struct drm_connector *connector,
		struct drm_atomic_state *atomic_state,
		uint32_t property_idx, uint64_t value)
{
	struct drm_connector_state *state;
	struct drm_property *property;
	struct dpu_connector *c_conn;

	if (!connector || !atomic_state) {
		DPU_ERROR("invalid argument(s), conn %d, state %d\n",
				connector != NULL, atomic_state != NULL);
		return -EINVAL;
	}

	c_conn = to_dpu_connector(connector);
	property = msm_property_index_to_drm_property(
			&c_conn->property_info, property_idx);
	if (!property) {
		DPU_ERROR("invalid property index %d\n", property_idx);
		return -EINVAL;
	}

	state = drm_atomic_get_connector_state(atomic_state, connector);
	if (IS_ERR_OR_NULL(state)) {
		DPU_ERROR("failed to get conn %d state\n",
				connector->base.id);
		return -EINVAL;
	}

	return dpu_connector_atomic_set_property(
			connector, state, property, value);
}

#ifdef CONFIG_DEBUG_FS
/**
 * dpu_connector_init_debugfs - initialize connector debugfs
 * @connector: Pointer to drm connector
 */
static int dpu_connector_init_debugfs(struct drm_connector *connector)
{
	struct dpu_connector *dpu_connector;

	if (!connector || !connector->debugfs_entry) {
		DPU_ERROR("invalid connector\n");
		return -EINVAL;
	}

	dpu_connector = to_dpu_connector(connector);

	if (!debugfs_create_bool("fb_kmap", 0600, connector->debugfs_entry,
			&dpu_connector->fb_kmap)) {
		DPU_ERROR("failed to create connector fb_kmap\n");
		return -ENOMEM;
	}

	return 0;
}
#else
static int dpu_connector_init_debugfs(struct drm_connector *connector)
{
	return 0;
}
#endif

static int dpu_connector_late_register(struct drm_connector *connector)
{
	return dpu_connector_init_debugfs(connector);
}

static void dpu_connector_early_unregister(struct drm_connector *connector)
{
	/* debugfs under connector->debugfs are deleted by drm_debugfs */
}

static const struct drm_connector_funcs dpu_connector_ops = {
	.reset =                  dpu_connector_atomic_reset,
	.detect =                 dpu_connector_detect,
	.destroy =                dpu_connector_destroy,
	.fill_modes =             drm_helper_probe_single_connector_modes,
	.atomic_duplicate_state = dpu_connector_atomic_duplicate_state,
	.atomic_destroy_state =   dpu_connector_atomic_destroy_state,
	.atomic_set_property =    dpu_connector_atomic_set_property,
	.atomic_get_property =    dpu_connector_atomic_get_property,
	.set_property =           dpu_connector_set_property,
	.late_register =          dpu_connector_late_register,
	.early_unregister =       dpu_connector_early_unregister,
};

static int dpu_connector_get_modes(struct drm_connector *connector)
{
	struct dpu_connector *c_conn;

	if (!connector) {
		DPU_ERROR("invalid connector\n");
		return 0;
	}

	c_conn = to_dpu_connector(connector);
	if (!c_conn->ops.get_modes) {
		DPU_DEBUG("missing get_modes callback\n");
		return 0;
	}

	return c_conn->ops.get_modes(connector, c_conn->display);
}

static enum drm_mode_status
dpu_connector_mode_valid(struct drm_connector *connector,
		struct drm_display_mode *mode)
{
	struct dpu_connector *c_conn;

	if (!connector || !mode) {
		DPU_ERROR("invalid argument(s), conn %pK, mode %pK\n",
				connector, mode);
		return MODE_ERROR;
	}

	c_conn = to_dpu_connector(connector);

	if (c_conn->ops.mode_valid)
		return c_conn->ops.mode_valid(connector, mode, c_conn->display);

	/* assume all modes okay by default */
	return MODE_OK;
}

static struct drm_encoder *
dpu_connector_best_encoder(struct drm_connector *connector)
{
	struct dpu_connector *c_conn = to_dpu_connector(connector);

	if (!connector) {
		DPU_ERROR("invalid connector\n");
		return NULL;
	}

	/*
	 * This is true for now, revisit this code when multiple encoders are
	 * supported.
	 */
	return c_conn->encoder;
}

static const struct drm_connector_helper_funcs dpu_connector_helper_ops = {
	.get_modes =    dpu_connector_get_modes,
	.mode_valid =   dpu_connector_mode_valid,
	.best_encoder = dpu_connector_best_encoder,
};

struct drm_connector *dpu_connector_init(struct drm_device *dev,
		struct drm_encoder *encoder,
		struct drm_panel *panel,
		void *display,
		const struct dpu_connector_ops *ops,
		int connector_poll,
		int connector_type)
{
	struct msm_drm_private *priv;
	struct dpu_kms *dpu_kms;
	struct dpu_kms_info *info;
	struct dpu_connector *c_conn = NULL;
	struct msm_display_info display_info;
	int rc;

	if (!dev || !dev->dev_private || !encoder) {
		DPU_ERROR("invalid argument(s), dev %pK, enc %pK\n",
				dev, encoder);
		return ERR_PTR(-EINVAL);
	}

	priv = dev->dev_private;
	if (!priv->kms) {
		DPU_ERROR("invalid kms reference\n");
		return ERR_PTR(-EINVAL);
	}

	c_conn = kzalloc(sizeof(*c_conn), GFP_KERNEL);
	if (!c_conn) {
		DPU_ERROR("failed to alloc dpu connector\n");
		return ERR_PTR(-ENOMEM);
	}

	memset(&display_info, 0, sizeof(display_info));

	rc = drm_connector_init(dev,
			&c_conn->base,
			&dpu_connector_ops,
			connector_type);
	if (rc)
		goto error_free_conn;

	spin_lock_init(&c_conn->event_lock);

	c_conn->connector_type = connector_type;
	c_conn->encoder = encoder;
	c_conn->panel = panel;
	c_conn->display = display;

	c_conn->dpms_mode = DRM_MODE_DPMS_ON;
	c_conn->lp_mode = 0;
	c_conn->last_panel_power_mode = DPU_MODE_DPMS_ON;

	dpu_kms = to_dpu_kms(priv->kms);

	if (ops)
		c_conn->ops = *ops;

	c_conn->base.helper_private = &dpu_connector_helper_ops;
	c_conn->base.polled = connector_poll;
	c_conn->base.interlace_allowed = 0;
	c_conn->base.doublescan_allowed = 0;

	snprintf(c_conn->name,
			DPU_CONNECTOR_NAME_SIZE,
			"conn%u",
			c_conn->base.base.id);

	mutex_init(&c_conn->lock);

	rc = drm_mode_connector_attach_encoder(&c_conn->base, encoder);
	if (rc) {
		DPU_ERROR("failed to attach encoder to connector, %d\n", rc);
		goto error_cleanup_conn;
	}

#ifdef CONFIG_DRM_MSM_DSI_STAGING
	rc = dpu_backlight_setup(c_conn, dev);
	if (rc) {
		DPU_ERROR("failed to setup backlight, rc=%d\n", rc);
		goto error_cleanup_conn;
	}
#endif

	/* create properties */
	msm_property_init(&c_conn->property_info, &c_conn->base.base, dev,
			priv->conn_property, c_conn->property_data,
			CONNECTOR_PROP_COUNT, CONNECTOR_PROP_BLOBCOUNT,
			sizeof(struct dpu_connector_state));

	if (c_conn->ops.post_init) {
		info = kmalloc(sizeof(*info), GFP_KERNEL);
		if (!info) {
			DPU_ERROR("failed to allocate info buffer\n");
			rc = -ENOMEM;
			goto error_cleanup_conn;
		}

		dpu_kms_info_reset(info);
		rc = c_conn->ops.post_init(&c_conn->base, info, display);
		if (rc) {
			DPU_ERROR("post-init failed, %d\n", rc);
			kfree(info);
			goto error_cleanup_conn;
		}

		msm_property_install_blob(&c_conn->property_info,
				"capabilities",
				DRM_MODE_PROP_IMMUTABLE,
				CONNECTOR_PROP_DPU_INFO);

		msm_property_set_blob(&c_conn->property_info,
				&c_conn->blob_caps,
				DPU_KMS_INFO_DATA(info),
				DPU_KMS_INFO_DATALEN(info),
				CONNECTOR_PROP_DPU_INFO);
		kfree(info);
	}

#ifdef CONFIG_DRM_MSM_DSI_STAGING
	if (connector_type == DRM_MODE_CONNECTOR_DSI) {
		struct dsi_display *dsi_display;

		dsi_display = (struct dsi_display *)(display);
		if (dsi_display && dsi_display->panel &&
			dsi_display->panel->hdr_props.hdr_enabled == true) {
			msm_property_install_blob(&c_conn->property_info,
				"hdr_properties",
				DRM_MODE_PROP_IMMUTABLE,
				CONNECTOR_PROP_HDR_INFO);

			msm_property_set_blob(&c_conn->property_info,
				&c_conn->blob_hdr,
				&dsi_display->panel->hdr_props,
				sizeof(dsi_display->panel->hdr_props),
				CONNECTOR_PROP_HDR_INFO);
		}
	}

	/* install PP_DITHER properties */
	_dpu_connector_install_dither_property(dev, dpu_kms, c_conn);

	msm_property_install_range(&c_conn->property_info, "bl_scale",
		0x0, 0, MAX_BL_SCALE_LEVEL, MAX_BL_SCALE_LEVEL,
		CONNECTOR_PROP_BL_SCALE);

	msm_property_install_range(&c_conn->property_info, "ad_bl_scale",
		0x0, 0, MAX_AD_BL_SCALE_LEVEL, MAX_AD_BL_SCALE_LEVEL,
		CONNECTOR_PROP_AD_BL_SCALE);
#endif

	msm_property_install_range(&c_conn->property_info, "autorefresh",
			0x0, 0, AUTOREFRESH_MAX_FRAME_CNT, 0,
			CONNECTOR_PROP_AUTOREFRESH);

	/* enum/bitmask properties */
	msm_property_install_enum(&c_conn->property_info, "topology_name",
			DRM_MODE_PROP_IMMUTABLE, 0, e_topology_name,
			ARRAY_SIZE(e_topology_name),
			CONNECTOR_PROP_TOPOLOGY_NAME);
	msm_property_install_enum(&c_conn->property_info, "topology_control",
			0, 1, e_topology_control,
			ARRAY_SIZE(e_topology_control),
			CONNECTOR_PROP_TOPOLOGY_CONTROL);
	msm_property_install_enum(&c_conn->property_info, "LP",
			0, 0, e_power_mode,
			ARRAY_SIZE(e_power_mode),
			CONNECTOR_PROP_LP);

	rc = msm_property_install_get_status(&c_conn->property_info);
	if (rc) {
		DPU_ERROR("failed to create one or more properties\n");
		goto error_destroy_property;
	}

	DPU_DEBUG("connector %d attach encoder %d\n",
			c_conn->base.base.id, encoder->base.id);

	priv->connectors[priv->num_connectors++] = &c_conn->base;

	return &c_conn->base;

error_destroy_property:
	if (c_conn->blob_caps)
		drm_property_blob_put(c_conn->blob_caps);
	if (c_conn->blob_hdr)
		drm_property_blob_put(c_conn->blob_hdr);
	if (c_conn->blob_dither)
		drm_property_blob_put(c_conn->blob_dither);

	msm_property_destroy(&c_conn->property_info);
error_cleanup_conn:
	mutex_destroy(&c_conn->lock);
	drm_connector_cleanup(&c_conn->base);
error_free_conn:
	kfree(c_conn);

	return ERR_PTR(rc);
}

int dpu_connector_register_custom_event(struct dpu_kms *kms,
		struct drm_connector *conn_drm, u32 event, bool val)
{
	int ret = -EINVAL;

	switch (event) {
	case DRM_EVENT_SYS_BACKLIGHT:
		ret = 0;
		break;
	default:
		break;
	}
	return ret;
}
