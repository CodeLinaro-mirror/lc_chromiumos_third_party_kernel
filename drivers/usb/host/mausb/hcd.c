// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 - 2020 DisplayLink (UK) Ltd.
 */
#include "hcd.h"

#include "utils.h"

static int mausb_bus_match(struct device *dev, struct device_driver *drv);

struct mausb_hcd	*mhcd;

static struct bus_type	mausb_bus_type = {
	.name	= DRIVER_NAME,
	.match	= mausb_bus_match,
};

static struct device_driver mausb_driver = {
	.name	= DRIVER_NAME,
	.owner	= THIS_MODULE,
	.bus	= &mausb_bus_type,
};

static struct device *mausb_device;

static int mausb_bus_match(struct device *dev, struct device_driver *drv)
{
	return !strncmp(dev->bus->name, drv->name, strlen(drv->name));
}

int mausb_host_driver_init(void)
{
	int retval = bus_register(&mausb_bus_type);

	if (retval)
		return retval;

	retval = driver_register(&mausb_driver);
	if (retval)
		goto cleanup_bus;

	mausb_device = kzalloc(sizeof(struct device), GFP_KERNEL);
	if (!mausb_device) {
		retval = -ENOMEM;
		goto cleanup_driver;
	}

	dev_set_name(mausb_device, DEVICE_NAME);
	mausb_device->bus = &mausb_bus_type;
	mausb_device->release = (void (*)(struct device *))kfree;

	retval = device_register(mausb_device);
	if (retval) {
		put_device(mausb_device);
		goto cleanup_driver;
	}

	return retval;

cleanup_driver:
	driver_unregister(&mausb_driver);
cleanup_bus:
	bus_unregister(&mausb_bus_type);
	return retval;
}

void mausb_host_driver_deinit(void)
{
	device_unregister(mausb_device);
	driver_unregister(&mausb_driver);
	bus_unregister(&mausb_bus_type);
}
