/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 - 2020 DisplayLink (UK) Ltd.
 */
#ifndef __MAUSB_HCD_H__
#define __MAUSB_HCD_H__

#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>

#define DRIVER_NAME "mausb_host"
#define DEVICE_NAME "mausb_host_hcd"

#define NUMBER_OF_PORTS		15
/* Response Timeout in ms (MA-USB v1.0a, Table 43) */
#define RESPONSE_TIMEOUT_MS	5000

/* MA-USB v1.0a, Table 12 */
enum mausb_device_type {
	USBDEVICE = 0,
	USB20HUB  = 1,
	USB30HUB  = 2,
};

/* MA-USB v1.0a, Table 15 */
enum mausb_device_speed {
	LOW_SPEED	 = 0,
	FULL_SPEED	 = 1,
	HIGH_SPEED	 = 2,
	SUPER_SPEED	 = 3,
	SUPER_SPEED_PLUS = 4,
};

struct mausb_hcd {
	spinlock_t	lock;	/* Protect HCD during URB processing */
	u8		connected_ports;

	struct rb_root	mausb_urbs;
	struct hub_ctx	*hcd_ss_ctx;
	struct hub_ctx	*hcd_hs_ctx;
	struct notifier_block power_state_listener;
};

struct mausb_dev {
	u32		port_status;
	struct rb_root	usb_devices;
	u8		dev_speed;
	void		*ma_dev;
};

struct hub_ctx {
	struct usb_hcd	 *hcd;
	struct mausb_dev ma_devs[NUMBER_OF_PORTS];
};

int mausb_host_driver_init(void);
void mausb_host_driver_deinit(void);

#endif /* __MAUSB_HCD_H__ */
