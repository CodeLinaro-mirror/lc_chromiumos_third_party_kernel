// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 - 2020 DisplayLink (UK) Ltd.
 */
#include "utils.h"

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>

#define MAUSB_KERNEL_DEV_NAME "mausb_host"
#define MAUSB_READ_DEVICE_TIMEOUT_MS 500

struct miscdevice mausb_host_dev;

static int mausb_host_dev_open(struct inode *inode, struct file *filp)
{
	filp->private_data = NULL;

	return 0;
}

static int mausb_host_dev_release(struct inode *inode, struct file *filp)
{
	kfree(filp->private_data);
	filp->private_data = NULL;

	return 0;
}

static const struct file_operations mausb_host_dev_fops = {
	.open	 = mausb_host_dev_open,
	.release = mausb_host_dev_release,
};

int mausb_host_dev_register(void)
{
	mausb_host_dev.minor = MISC_DYNAMIC_MINOR;
	mausb_host_dev.name = MAUSB_KERNEL_DEV_NAME;
	mausb_host_dev.fops = &mausb_host_dev_fops;
	return misc_register(&mausb_host_dev);
}

void mausb_host_dev_deregister(void)
{
	misc_deregister(&mausb_host_dev);
}
