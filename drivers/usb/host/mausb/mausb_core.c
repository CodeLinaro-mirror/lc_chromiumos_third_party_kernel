// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 - 2020 DisplayLink (UK) Ltd.
 */
#include <linux/module.h>

#include "utils.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DisplayLink (UK) Ltd.");

static int mausb_host_init(void)
{
	return mausb_host_dev_register();
}

static void mausb_host_exit(void)
{
	mausb_host_dev_deregister();
}

module_init(mausb_host_init);
module_exit(mausb_host_exit);
