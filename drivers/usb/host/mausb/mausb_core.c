// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 - 2020 DisplayLink (UK) Ltd.
 */
#include <linux/module.h>

#include "hcd.h"
#include "utils.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DisplayLink (UK) Ltd.");

static int mausb_host_init(void)
{
	int status = mausb_host_dev_register();

	if (status < 0)
		return status;

	status = mausb_host_driver_init();
	if (status < 0)
		goto cleanup_dev;

	return 0;

cleanup_dev:
	mausb_host_dev_deregister();
	return status;
}

static void mausb_host_exit(void)
{
	mausb_host_driver_deinit();
	mausb_host_dev_deregister();
}

module_init(mausb_host_init);
module_exit(mausb_host_exit);
