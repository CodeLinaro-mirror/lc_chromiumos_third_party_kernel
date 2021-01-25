// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

static const char * const regulator_names[] = {
	"vdda",
	"vdispp",
	"vdispn",
};

static unsigned long const regulator_enable_loads[] = {
	62000,
	100000,
	100000,
};

static unsigned long const regulator_disable_loads[] = {
	80,
	100,
	100,
};

struct truly_nt36672e {
	struct device *dev;
	struct drm_panel panel;

	struct regulator_bulk_data supplies[ARRAY_SIZE(regulator_names)];

	struct gpio_desc *reset_gpio;

	struct backlight_device *backlight;

	struct mipi_dsi_device *dsi;

	const struct panel_desc *desc;
	bool prepared;
	bool enabled;
};

struct panel_desc {
	const struct drm_display_mode *display_mode;
	u32 bpc;
	u32 width_mm;
	u32 height_mm;
	unsigned long mode_flags;
	enum mipi_dsi_pixel_format format;
	unsigned int lanes;
	const char *panel_name;
	int (*init_sequence)(struct mipi_dsi_device *dsi);
};

static inline struct truly_nt36672e *panel_to_ctx(struct drm_panel *panel)
{
	return container_of(panel, struct truly_nt36672e, panel);
}

#define dsi_dcs_write_seq(dsi, seq...) do {                             \
		static const u8 d[] = { seq };                          \
		int ret;                                                \
		ret = mipi_dsi_dcs_write_buffer(dsi, d, ARRAY_SIZE(d)); \
		if (ret < 0)                                            \
			return ret;                                     \
	} while (0)

static int truly_nt36672e_1080x2408_60hz_init(struct mipi_dsi_device *dsi)
{
	dsi_dcs_write_seq(dsi, 0xff, 0x10);
	dsi_dcs_write_seq(dsi, 0xfb, 0x01);
	dsi_dcs_write_seq(dsi, 0xb0, 0x00);
	dsi_dcs_write_seq(dsi, 0xc0,0x00);
	dsi_dcs_write_seq(dsi, 0xc1, 0x89, 0x28, 0x00, 0x08, 0x00, 0xaa, 0x02,
				0x0e, 0x00, 0x2b, 0x00, 0x07, 0x0d, 0xb7, 0x0c,
				0xb7);

	dsi_dcs_write_seq(dsi, 0xc2, 0x1b, 0xa0);
	dsi_dcs_write_seq(dsi, 0xff, 0x20);
	dsi_dcs_write_seq(dsi, 0xfb, 0x01);
	dsi_dcs_write_seq(dsi, 0x01, 0x66);
	dsi_dcs_write_seq(dsi, 0x06, 0x40);
	dsi_dcs_write_seq(dsi, 0x07, 0x38);
	dsi_dcs_write_seq(dsi, 0x2f, 0x83);
	dsi_dcs_write_seq(dsi, 0x69, 0x91);
	dsi_dcs_write_seq(dsi, 0x95, 0xd1);
	dsi_dcs_write_seq(dsi, 0x96, 0xd1);
	dsi_dcs_write_seq(dsi, 0xf2, 0x64);
	dsi_dcs_write_seq(dsi, 0xf3, 0x54);
	dsi_dcs_write_seq(dsi, 0xf4, 0x64);
	dsi_dcs_write_seq(dsi, 0xf5, 0x54);
	dsi_dcs_write_seq(dsi, 0xf6, 0x64);
	dsi_dcs_write_seq(dsi, 0xf7, 0x54);
	dsi_dcs_write_seq(dsi, 0xf8, 0x64);
	dsi_dcs_write_seq(dsi, 0xf9, 0x54);
	dsi_dcs_write_seq(dsi, 0xff, 0x24);
	dsi_dcs_write_seq(dsi, 0xfb, 0x01);
	dsi_dcs_write_seq(dsi, 0x01, 0x0f);
	dsi_dcs_write_seq(dsi, 0x03, 0x0c);
	dsi_dcs_write_seq(dsi, 0x05, 0x1d);
	dsi_dcs_write_seq(dsi, 0x08, 0x2f);
	dsi_dcs_write_seq(dsi, 0x09, 0x2e);
	dsi_dcs_write_seq(dsi, 0x0a, 0x2d);
	dsi_dcs_write_seq(dsi, 0x0b, 0x2c);
	dsi_dcs_write_seq(dsi, 0x11, 0x17);
	dsi_dcs_write_seq(dsi, 0x12, 0x13);
	dsi_dcs_write_seq(dsi, 0x13, 0x15);
	dsi_dcs_write_seq(dsi, 0x15, 0x14);
	dsi_dcs_write_seq(dsi, 0x16, 0x16);
	dsi_dcs_write_seq(dsi, 0x17, 0x18);
	dsi_dcs_write_seq(dsi, 0x1b, 0x01);
	dsi_dcs_write_seq(dsi, 0x1d, 0x1d);
	dsi_dcs_write_seq(dsi, 0x20, 0x2f);
	dsi_dcs_write_seq(dsi, 0x21, 0x2e);
	dsi_dcs_write_seq(dsi, 0x22, 0x2d);
	dsi_dcs_write_seq(dsi, 0x23, 0x2c);
	dsi_dcs_write_seq(dsi, 0x29, 0x17);
	dsi_dcs_write_seq(dsi, 0x2a, 0x13);
	dsi_dcs_write_seq(dsi, 0x2b, 0x15);
	dsi_dcs_write_seq(dsi, 0x2f, 0x14);
	dsi_dcs_write_seq(dsi, 0x30, 0x16);
	dsi_dcs_write_seq(dsi, 0x31, 0x18);
	dsi_dcs_write_seq(dsi, 0x32, 0x04);
	dsi_dcs_write_seq(dsi, 0x34, 0x10);
	dsi_dcs_write_seq(dsi, 0x35, 0x1f);
	dsi_dcs_write_seq(dsi, 0x36, 0x1f);
	dsi_dcs_write_seq(dsi, 0x4d, 0x14);
	dsi_dcs_write_seq(dsi, 0x4e, 0x36);
	dsi_dcs_write_seq(dsi, 0x4f, 0x36);
	dsi_dcs_write_seq(dsi, 0x53, 0x36);
	dsi_dcs_write_seq(dsi, 0x71, 0x30);
	dsi_dcs_write_seq(dsi, 0x79, 0x11);
	dsi_dcs_write_seq(dsi, 0x7a, 0x82);
	dsi_dcs_write_seq(dsi, 0x7b, 0x8f);
	dsi_dcs_write_seq(dsi, 0x7d, 0x04);
	dsi_dcs_write_seq(dsi, 0x80, 0x04);
	dsi_dcs_write_seq(dsi, 0x81, 0x04);
	dsi_dcs_write_seq(dsi, 0x82, 0x13);
	dsi_dcs_write_seq(dsi, 0x84, 0x31);
	dsi_dcs_write_seq(dsi, 0x85, 0x00);
	dsi_dcs_write_seq(dsi, 0x86, 0x00);
	dsi_dcs_write_seq(dsi, 0x87, 0x00);
	dsi_dcs_write_seq(dsi, 0x90, 0x13);
	dsi_dcs_write_seq(dsi, 0x92, 0x31);
	dsi_dcs_write_seq(dsi, 0x93, 0x00);
	dsi_dcs_write_seq(dsi, 0x94, 0x00);
	dsi_dcs_write_seq(dsi, 0x95, 0x00);
	dsi_dcs_write_seq(dsi, 0x9c, 0xf4);
	dsi_dcs_write_seq(dsi, 0x9d, 0x01);
	dsi_dcs_write_seq(dsi, 0xa0, 0x0f);
	dsi_dcs_write_seq(dsi, 0xa2, 0x0f);
	dsi_dcs_write_seq(dsi, 0xa3, 0x02);
	dsi_dcs_write_seq(dsi, 0xa4, 0x04);
	dsi_dcs_write_seq(dsi, 0xa5, 0x04);
	dsi_dcs_write_seq(dsi, 0xc6, 0xc0);
	dsi_dcs_write_seq(dsi, 0xc9, 0x00);
	dsi_dcs_write_seq(dsi, 0xd9, 0x80);
	dsi_dcs_write_seq(dsi, 0xe9, 0x02);
	dsi_dcs_write_seq(dsi, 0xff, 0x25);
	dsi_dcs_write_seq(dsi, 0xfb, 0x01);
	dsi_dcs_write_seq(dsi, 0x18, 0x22);
	dsi_dcs_write_seq(dsi, 0x19, 0xe4);
	dsi_dcs_write_seq(dsi, 0x21, 0x40);
	dsi_dcs_write_seq(dsi, 0x66, 0xd8);
	dsi_dcs_write_seq(dsi, 0x68, 0x50);
	dsi_dcs_write_seq(dsi, 0x69, 0x10);
	dsi_dcs_write_seq(dsi, 0x6b, 0x00);
	dsi_dcs_write_seq(dsi, 0x6d, 0x0d);
	dsi_dcs_write_seq(dsi, 0x6e, 0x48);
	dsi_dcs_write_seq(dsi, 0x72, 0x41);
	dsi_dcs_write_seq(dsi, 0x73, 0x4a);
	dsi_dcs_write_seq(dsi, 0x74, 0xd0);
	dsi_dcs_write_seq(dsi, 0x77, 0x62);
	dsi_dcs_write_seq(dsi, 0x79, 0x7e);
	dsi_dcs_write_seq(dsi, 0x7d, 0x03);
	dsi_dcs_write_seq(dsi, 0x7e, 0x15);
	dsi_dcs_write_seq(dsi, 0x7f, 0x00);
	dsi_dcs_write_seq(dsi, 0x84, 0x4d);
	dsi_dcs_write_seq(dsi, 0xcf, 0x80);
	dsi_dcs_write_seq(dsi, 0xd6, 0x80);
	dsi_dcs_write_seq(dsi, 0xd7, 0x80);
	dsi_dcs_write_seq(dsi, 0xef, 0x20);
	dsi_dcs_write_seq(dsi, 0xf0, 0x84);
	dsi_dcs_write_seq(dsi, 0xff, 0x26);
	dsi_dcs_write_seq(dsi, 0xfb, 0x01);
	dsi_dcs_write_seq(dsi, 0x81, 0x0f);
	dsi_dcs_write_seq(dsi, 0x83, 0x01);
	dsi_dcs_write_seq(dsi, 0x84, 0x03);
	dsi_dcs_write_seq(dsi, 0x85, 0x01);
	dsi_dcs_write_seq(dsi, 0x86, 0x03);
	dsi_dcs_write_seq(dsi, 0x87, 0x01);
	dsi_dcs_write_seq(dsi, 0x88, 0x05);
	dsi_dcs_write_seq(dsi, 0x8a, 0x1a);
	dsi_dcs_write_seq(dsi, 0x8b, 0x11);
	dsi_dcs_write_seq(dsi, 0x8c, 0x24);
	dsi_dcs_write_seq(dsi, 0x8e, 0x42);
	dsi_dcs_write_seq(dsi, 0x8f, 0x11);
	dsi_dcs_write_seq(dsi, 0x90, 0x11);
	dsi_dcs_write_seq(dsi, 0x91, 0x11);
	dsi_dcs_write_seq(dsi, 0x9a, 0x80);
	dsi_dcs_write_seq(dsi, 0x9b, 0x04);
	dsi_dcs_write_seq(dsi, 0x9c, 0x00);
	dsi_dcs_write_seq(dsi, 0x9d, 0x00);
	dsi_dcs_write_seq(dsi, 0x9e, 0x00);
	dsi_dcs_write_seq(dsi, 0xff, 0x27);
	dsi_dcs_write_seq(dsi, 0xfb, 0x01);
	dsi_dcs_write_seq(dsi, 0x01, 0x68);
	dsi_dcs_write_seq(dsi, 0x20, 0x81);
	dsi_dcs_write_seq(dsi, 0x21, 0x6a);
	dsi_dcs_write_seq(dsi, 0x25, 0x81);
	dsi_dcs_write_seq(dsi, 0x26, 0x94);
	dsi_dcs_write_seq(dsi, 0x6e, 0x00);
	dsi_dcs_write_seq(dsi, 0x6f, 0x00);
	dsi_dcs_write_seq(dsi, 0x70, 0x00);
	dsi_dcs_write_seq(dsi, 0x71, 0x00);
	dsi_dcs_write_seq(dsi, 0x72, 0x00);
	dsi_dcs_write_seq(dsi, 0x75, 0x00);
	dsi_dcs_write_seq(dsi, 0x76, 0x00);
	dsi_dcs_write_seq(dsi, 0x77, 0x00);
	dsi_dcs_write_seq(dsi, 0x7d, 0x09);
	dsi_dcs_write_seq(dsi, 0x7e, 0x67);
	dsi_dcs_write_seq(dsi, 0x80, 0x23);
	dsi_dcs_write_seq(dsi, 0x82, 0x09);
	dsi_dcs_write_seq(dsi, 0x83, 0x67);
	dsi_dcs_write_seq(dsi, 0x88, 0x01);
	dsi_dcs_write_seq(dsi, 0x89, 0x10);
	dsi_dcs_write_seq(dsi, 0xa5, 0x10);
	dsi_dcs_write_seq(dsi, 0xa6, 0x23);
	dsi_dcs_write_seq(dsi, 0xa7, 0x01);
	dsi_dcs_write_seq(dsi, 0xb6, 0x40);
	dsi_dcs_write_seq(dsi, 0xe5, 0x02);
	dsi_dcs_write_seq(dsi, 0xe6, 0xd3);
	dsi_dcs_write_seq(dsi, 0xeb, 0x03);
	dsi_dcs_write_seq(dsi, 0xec, 0x28);
	dsi_dcs_write_seq(dsi, 0xff, 0x2a);
	dsi_dcs_write_seq(dsi, 0xfb, 0x01);
	dsi_dcs_write_seq(dsi, 0x00, 0x91);
	dsi_dcs_write_seq(dsi, 0x03, 0x20);
	dsi_dcs_write_seq(dsi, 0x07, 0x50);
	dsi_dcs_write_seq(dsi, 0x0a, 0x70);
	dsi_dcs_write_seq(dsi, 0x0c, 0x04);
	dsi_dcs_write_seq(dsi, 0x0d, 0x40);
	dsi_dcs_write_seq(dsi, 0x0f, 0x01);
	dsi_dcs_write_seq(dsi, 0x11, 0xe0);
	dsi_dcs_write_seq(dsi, 0x15, 0x0f);
	dsi_dcs_write_seq(dsi, 0x16, 0xa4);
	dsi_dcs_write_seq(dsi, 0x19, 0x0f);
	dsi_dcs_write_seq(dsi, 0x1a, 0x78);
	dsi_dcs_write_seq(dsi, 0x1b, 0x23);
	dsi_dcs_write_seq(dsi, 0x1d, 0x36);
	dsi_dcs_write_seq(dsi, 0x1e, 0x3e);
	dsi_dcs_write_seq(dsi, 0x1f, 0x3e);
	dsi_dcs_write_seq(dsi, 0x20, 0x3e);
	dsi_dcs_write_seq(dsi, 0x28, 0xfd);
	dsi_dcs_write_seq(dsi, 0x29, 0x12);
	dsi_dcs_write_seq(dsi, 0x2a, 0xe1);
	dsi_dcs_write_seq(dsi, 0x2d, 0x0a);
	dsi_dcs_write_seq(dsi, 0x30, 0x49);
	dsi_dcs_write_seq(dsi, 0x33, 0x96);
	dsi_dcs_write_seq(dsi, 0x34, 0xff);
	dsi_dcs_write_seq(dsi, 0x35, 0x40);
	dsi_dcs_write_seq(dsi, 0x36, 0xde);
	dsi_dcs_write_seq(dsi, 0x37, 0xf9);
	dsi_dcs_write_seq(dsi, 0x38, 0x45);
	dsi_dcs_write_seq(dsi, 0x39, 0xd9);
	dsi_dcs_write_seq(dsi, 0x3a, 0x49);
	dsi_dcs_write_seq(dsi, 0x4a, 0xf0);
	dsi_dcs_write_seq(dsi, 0x7a, 0x09);
	dsi_dcs_write_seq(dsi, 0x7b, 0x40);
	dsi_dcs_write_seq(dsi, 0x7f, 0xf0);
	dsi_dcs_write_seq(dsi, 0x83, 0x0f);
	dsi_dcs_write_seq(dsi, 0x84, 0xa4);
	dsi_dcs_write_seq(dsi, 0x87, 0x0f);
	dsi_dcs_write_seq(dsi, 0x88, 0x78);
	dsi_dcs_write_seq(dsi, 0x89, 0x23);
	dsi_dcs_write_seq(dsi, 0x8b, 0x36);
	dsi_dcs_write_seq(dsi, 0x8c, 0x7d);
	dsi_dcs_write_seq(dsi, 0x8d, 0x7d);
	dsi_dcs_write_seq(dsi, 0x8e, 0x7d);
	dsi_dcs_write_seq(dsi, 0xff, 0x20);
	dsi_dcs_write_seq(dsi, 0xfb, 0x01);
	dsi_dcs_write_seq(dsi, 0xb0, 0x00, 0x00, 0x00, 0x17, 0x00, 0x49, 0x00,
				0x6a, 0x00, 0x89, 0x00, 0x9f, 0x00, 0xb6, 0x00,
				0xc8);
	dsi_dcs_write_seq(dsi, 0xb1, 0x00, 0xd9, 0x01, 0x10, 0x01, 0x3a, 0x01,
				0x7a, 0x01, 0xa9, 0x01, 0xf2, 0x02, 0x2d, 0x02,
				0x2e);
	dsi_dcs_write_seq(dsi, 0xb2, 0x02, 0x64, 0x02, 0xa3, 0x02, 0xca, 0x03,
				0x00, 0x03, 0x1e, 0x03, 0x4a, 0x03, 0x59, 0x03,
				0x6a);
	dsi_dcs_write_seq(dsi, 0xb3, 0x03, 0x7d, 0x03, 0x93, 0x03, 0xab, 0x03,
				0xc8, 0x03, 0xec, 0x03, 0xfe, 0x00, 0x00);
	dsi_dcs_write_seq(dsi, 0xb4, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x51, 0x00,
				0x71, 0x00, 0x90, 0x00, 0xa7, 0x00, 0xbf, 0x00,
				0xd1);
	dsi_dcs_write_seq(dsi, 0xb5, 0x00, 0xe2, 0x01, 0x1a, 0x01, 0x43, 0x01,
				0x83, 0x01, 0xb2, 0x01, 0xfa, 0x02, 0x34, 0x02,
				0x36);
	dsi_dcs_write_seq(dsi, 0xb6, 0x02, 0x6b, 0x02, 0xa8, 0x02, 0xd0, 0x03,
				0x03, 0x03, 0x21, 0x03, 0x4d, 0x03, 0x5b, 0x03,
				0x6b);
	dsi_dcs_write_seq(dsi, 0xb7, 0x03, 0x7e, 0x03, 0x94, 0x03, 0xac, 0x03,
				0xc8, 0x03, 0xec, 0x03, 0xfe, 0x00, 0x00);
	dsi_dcs_write_seq(dsi, 0xb8, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x51, 0x00,
				0x72, 0x00, 0x92, 0x00, 0xa8, 0x00, 0xbf, 0x00,
				0xd1);
	dsi_dcs_write_seq(dsi, 0xb9, 0x00, 0xe2, 0x01, 0x18, 0x01, 0x42, 0x01,
				0x81, 0x01, 0xaf, 0x01, 0xf5, 0x02, 0x2f, 0x02,
				0x31);
	dsi_dcs_write_seq(dsi, 0xba, 0x02, 0x68, 0x02, 0xa6, 0x02, 0xcd, 0x03,
				0x01, 0x03, 0x1f, 0x03, 0x4a, 0x03, 0x59, 0x03,
				0x6a);
	dsi_dcs_write_seq(dsi, 0xbb, 0x03, 0x7d, 0x03, 0x93, 0x03, 0xab, 0x03,
				0xc8, 0x03, 0xec, 0x03, 0xfe, 0x00, 0x00);
	dsi_dcs_write_seq(dsi, 0xff, 0x21);
	dsi_dcs_write_seq(dsi, 0xfb, 0x01);
	dsi_dcs_write_seq(dsi, 0xb0, 0x00, 0x00, 0x00, 0x17, 0x00, 0x49, 0x00,
				0x6a, 0x00, 0x89, 0x00, 0x9f, 0x00, 0xb6, 0x00,
				0xc8);
	dsi_dcs_write_seq(dsi, 0xb1, 0x00, 0xd9, 0x01, 0x10, 0x01, 0x3a, 0x01,
				0x7a, 0x01, 0xa9, 0x01, 0xf2, 0x02, 0x2d, 0x02,
				0x2e);
	dsi_dcs_write_seq(dsi, 0xb2, 0x02, 0x64, 0x02, 0xa3, 0x02, 0xca, 0x03,
				0x00, 0x03, 0x1e, 0x03, 0x4a, 0x03, 0x59, 0x03,
				0x6a);
	dsi_dcs_write_seq(dsi, 0xb3, 0x03, 0x7d, 0x03, 0x93, 0x03, 0xab, 0x03,
				0xc8, 0x03, 0xec, 0x03, 0xfe, 0x00, 0x00);
	dsi_dcs_write_seq(dsi, 0xb4, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x51, 0x00,
				0x71, 0x00, 0x90, 0x00, 0xa7, 0x00, 0xbf, 0x00,
				0xd1);
	dsi_dcs_write_seq(dsi, 0xb5, 0x00, 0xe2, 0x01, 0x1a, 0x01, 0x43, 0x01,
				0x83, 0x01, 0xb2, 0x01, 0xfa, 0x02, 0x34, 0x02,
				0x36);
	dsi_dcs_write_seq(dsi, 0xb6, 0x02, 0x6b, 0x02, 0xa8, 0x02, 0xd0, 0x03,
				0x03, 0x03, 0x21, 0x03, 0x4d, 0x03, 0x5b, 0x03,
				0x6b);
	dsi_dcs_write_seq(dsi, 0xb7, 0x03, 0x7e, 0x03, 0x94, 0x03, 0xac, 0x03,
				0xc8, 0x03, 0xec, 0x03, 0xfe, 0x00, 0x00);
	dsi_dcs_write_seq(dsi, 0xb8, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x51, 0x00,
				0x72, 0x00, 0x92, 0x00, 0xa8, 0x00, 0xbf, 0x00,
				0xd1);
	dsi_dcs_write_seq(dsi, 0xb9, 0x00, 0xe2, 0x01, 0x18, 0x01, 0x42, 0x01,
				0x81, 0x01, 0xaf, 0x01, 0xf5, 0x02, 0x2f, 0x02,
				0x31);
	dsi_dcs_write_seq(dsi, 0xba, 0x02, 0x68, 0x02, 0xa6, 0x02, 0xcd, 0x03,
				0x01, 0x03, 0x1f, 0x03, 0x4a, 0x03, 0x59, 0x03,
				0x6a);
	dsi_dcs_write_seq(dsi, 0xbb, 0x03, 0x7d, 0x03, 0x93, 0x03, 0xab, 0x03,
				0xc8, 0x03, 0xec, 0x03, 0xfe, 0x00, 0x00);
	dsi_dcs_write_seq(dsi, 0xff, 0x2c);
	dsi_dcs_write_seq(dsi, 0xfb, 0x01);
	dsi_dcs_write_seq(dsi, 0x61, 0x1f);
	dsi_dcs_write_seq(dsi, 0x62, 0x1f);
	dsi_dcs_write_seq(dsi, 0x7e, 0x03);
	dsi_dcs_write_seq(dsi, 0x6a, 0x14);
	dsi_dcs_write_seq(dsi, 0x6b, 0x36);
	dsi_dcs_write_seq(dsi, 0x6c, 0x36);
	dsi_dcs_write_seq(dsi, 0x6d, 0x36);
	dsi_dcs_write_seq(dsi, 0x53, 0x04);
	dsi_dcs_write_seq(dsi, 0x54, 0x04);
	dsi_dcs_write_seq(dsi, 0x55, 0x04);
	dsi_dcs_write_seq(dsi, 0x56, 0x0f);
	dsi_dcs_write_seq(dsi, 0x58, 0x0f);
	dsi_dcs_write_seq(dsi, 0x59, 0x0f);
	dsi_dcs_write_seq(dsi, 0xff, 0xf0);
	dsi_dcs_write_seq(dsi, 0xfb, 0x01);
	dsi_dcs_write_seq(dsi, 0x5a, 0x00);

	dsi_dcs_write_seq(dsi, 0xff, 0x10);
	dsi_dcs_write_seq(dsi, 0xfb, 0x01);
	dsi_dcs_write_seq(dsi, 0x51, 0xff);
	dsi_dcs_write_seq(dsi, 0x53, 0x24);
	dsi_dcs_write_seq(dsi, 0x55, 0x01);

	return 0;
}

static int truly_dcs_write(struct drm_panel *panel, u32 command)
{
	struct truly_nt36672e *ctx = panel_to_ctx(panel);
	int ret = 0;

	ret = mipi_dsi_dcs_write(ctx->dsi, command, NULL, 0);
	if (ret < 0)
		dev_err(ctx->dev, "cmd 0x%x failed, ret %d\n", command, ret);

	return ret;
}

static int truly_nt36672e_power_on(struct truly_nt36672e *ctx)
{
	int ret, i;

	for (i = 0; i < ARRAY_SIZE(ctx->supplies); i++) {
		ret = regulator_set_load(ctx->supplies[i].consumer,
					regulator_enable_loads[i]);
		if (ret)
			return ret;
	}

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0)
		return ret;

	/*
	 * Reset sequence of truly panel requires the panel to be
	 * out of reset for 10ms, followed by being held in reset
	 * for 10ms and then out again
	 */
	gpiod_set_value(ctx->reset_gpio, 1);
	usleep_range(10000, 20000);
	gpiod_set_value(ctx->reset_gpio, 0);
	usleep_range(10000, 20000);
	gpiod_set_value(ctx->reset_gpio, 1);
	usleep_range(10000, 20000);

	return 0;
}

static int truly_nt36672e_power_off(struct truly_nt36672e *ctx)
{
	int ret = 0;
	int i;

	gpiod_set_value(ctx->reset_gpio, 1);

	for (i = 0; i < ARRAY_SIZE(ctx->supplies); i++) {
		ret = regulator_set_load(ctx->supplies[i].consumer,
				regulator_disable_loads[i]);
		if (ret) {
			dev_err(ctx->dev, "regulator_set_load failed, ret %d\n", ret);
			return ret;
		}
	}

	ret = regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret) {
		dev_err(ctx->dev, "regulator_bulk_disable failed, ret %d\n", ret);
	}
	return ret;
}

static int truly_nt36672e_disable(struct drm_panel *panel)
{
	struct truly_nt36672e *ctx = panel_to_ctx(panel);
	int ret;

	if (!ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ret = backlight_disable(ctx->backlight);
		if (ret < 0)
			dev_err(ctx->dev, "backlight disable failed, ret %d\n", ret);
	}

	ctx->enabled = false;
	return 0;
}

static int truly_nt36672e_unprepare(struct drm_panel *panel)
{
	struct truly_nt36672e *ctx = panel_to_ctx(panel);
	int ret = 0;

	if (!ctx->prepared)
		return 0;

	ctx->dsi->mode_flags = 0;

	ret = truly_dcs_write(panel, MIPI_DCS_SET_DISPLAY_OFF);
	if (ret < 0) {
		dev_err(ctx->dev, "set_display_off cmd failed, ret %d\n", ret);
	}

	/* 120ms delay required here as per DCS spec */
	msleep(120);

	ret = truly_dcs_write(panel, MIPI_DCS_ENTER_SLEEP_MODE);
	if (ret < 0) {
		dev_err(ctx->dev, "enter_sleep cmd failed, ret %d\n", ret);
	}

	ret = truly_nt36672e_power_off(ctx);
	if (ret < 0)
		dev_err(ctx->dev, "power_off failed, ret %d\n", ret);

	ctx->prepared = false;
	return ret;
}

static int truly_nt36672e_prepare(struct drm_panel *panel)
{
	struct truly_nt36672e *ctx = panel_to_ctx(panel);
	int ret = 0;
	const struct panel_desc *desc;

	if (ctx->prepared)
		return 0;

	ret = truly_nt36672e_power_on(ctx);
	if (ret < 0)
		return ret;

	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	desc = ctx->desc;

	if (desc->init_sequence) {
		ret = desc->init_sequence(ctx->dsi);
		if (ret < 0) {
			dev_err(ctx->dev, "panel init sequence failed, ret %d\n", ret);
			goto power_off;
		}
	}

	ret = truly_dcs_write(panel, MIPI_DCS_EXIT_SLEEP_MODE);
	if (ret < 0) {
		dev_err(ctx->dev, "exit_sleep_mode cmd failed, ret = %d\n", ret);
		goto power_off;
	}

	/* Per DSI spec wait 120ms after sending exit sleep DCS command */
	msleep(120);

	ret = truly_dcs_write(panel, MIPI_DCS_SET_DISPLAY_ON);
	if (ret < 0) {
		dev_err(ctx->dev, "set_display_on cmd failed, ret %d\n", ret);
		goto power_off;
	}

	/* Per DSI spec wait 120ms after sending set_display_on DCS command */
	msleep(120);

	ctx->prepared = true;

	return 0;

power_off:
	if (truly_nt36672e_power_off(ctx))
		dev_err(ctx->dev, "power_off failed\n");
	return ret;
}

static int truly_nt36672e_enable(struct drm_panel *panel)
{
	struct truly_nt36672e *ctx = panel_to_ctx(panel);
	int ret;

	if (ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ret = backlight_enable(ctx->backlight);
		if (ret < 0)
			dev_err(ctx->dev, "backlight enable failed, ret %d\n", ret);
	}

	ctx->enabled = true;

	return 0;
}

static int truly_nt36672e_get_modes(struct drm_panel *panel,
				   struct drm_connector *connector)
{
	struct truly_nt36672e *ctx = panel_to_ctx(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_create(connector->dev);
	if (!mode) {
		dev_err(ctx->dev, "failed to create a new display mode\n");
		return 0;
	}

	connector->display_info.width_mm = ctx->desc->width_mm;
	connector->display_info.height_mm = ctx->desc->height_mm;
	connector->display_info.bpc = ctx->desc->bpc;
	drm_mode_copy(mode, ctx->desc->display_mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs truly_nt36672e_drm_funcs = {
	.disable = truly_nt36672e_disable,
	.unprepare = truly_nt36672e_unprepare,
	.prepare = truly_nt36672e_prepare,
	.enable = truly_nt36672e_enable,
	.get_modes = truly_nt36672e_get_modes,
};



static const struct drm_display_mode truly_nt36672e_1080x2408_60hz = {
	.name = "1080x2408",
	.clock = 181690,
	.hdisplay = 1080,
	.hsync_start = 1080 + 76,
	.hsync_end = 1080 + 76 + 12,
	.htotal = 1080 + 76 + 12 + 56,
	.vdisplay = 2408,
	.vsync_start = 2408 + 46,
	.vsync_end = 2408 + 46 + 10,
	.vtotal = 2408 + 46 + 10 + 10,
	.flags = 0,
};

static const struct panel_desc truly_nt36672e_panel_desc = {
	.display_mode = &truly_nt36672e_1080x2408_60hz,
	.bpc = 8,
	.width_mm = 74,
	.height_mm = 131,
	.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
	.panel_name = "truly nt36672e fhd plus panel",
	.init_sequence = truly_nt36672e_1080x2408_60hz_init,
};

static int truly_nt36672e_probe(struct mipi_dsi_device *dsi)
{
	struct device_node *backlight;
	struct device *dev = &dsi->dev;
	struct truly_nt36672e *ctx;
	int ret = 0;
	int i;


	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->desc = of_device_get_match_data(dev);
	if (!ctx->desc) {
		dev_err(dev, "missing device configuration\n");
		return -ENODEV;
	}

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;
	ctx->dsi = dsi;

	for (i = 0; i < ARRAY_SIZE(ctx->supplies); i++)
		ctx->supplies[i].supply = regulator_names[i];

	ret = devm_regulator_bulk_get(ctx->dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0)
		return ret;

	ctx->reset_gpio = devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(dev, "cannot get reset gpio %ld\n", PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}

	backlight = of_parse_phandle(dev->of_node, "backlight", 0);
	if (backlight) {
		ctx->backlight = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (!ctx->backlight)
			return -EPROBE_DEFER;
	}

	drm_panel_init(&ctx->panel, dev, &truly_nt36672e_drm_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	ctx->panel.funcs = &truly_nt36672e_drm_funcs;
	drm_panel_add(&ctx->panel);

	dsi->lanes = ctx->desc->lanes;
	dsi->format = ctx->desc->format;
	dsi->mode_flags = ctx->desc->mode_flags;
	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "dsi attach failed, ret %d\n", ret);
		goto err_dsi_attach;
	}

	return 0;

err_dsi_attach:
	drm_panel_remove(&ctx->panel);

	if (ctx->backlight)
		put_device(&ctx->backlight->dev);

	return ret;
}

static int truly_nt36672e_remove(struct mipi_dsi_device *dsi)
{
	struct truly_nt36672e *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(ctx->dsi);
	mipi_dsi_device_unregister(ctx->dsi);

	drm_panel_remove(&ctx->panel);

	if (ctx->backlight)
		put_device(&ctx->backlight->dev);

	return 0;
}

static const struct of_device_id truly_nt36672e_of_match[] = {
	{
		.compatible = "truly,nt36672e-fhd-plus-display",
		.data = &truly_nt36672e_panel_desc,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, truly_nt36672e_of_match);

static struct mipi_dsi_driver truly_nt36672e_driver = {
	.driver = {
		.name = "panel-truly-nt36672e",
		.of_match_table = truly_nt36672e_of_match,
	},
	.probe = truly_nt36672e_probe,
	.remove = truly_nt36672e_remove,
};
module_mipi_dsi_driver(truly_nt36672e_driver);

MODULE_DESCRIPTION("Truly NT36672e DSI Panel Driver");
MODULE_LICENSE("GPL v2");
