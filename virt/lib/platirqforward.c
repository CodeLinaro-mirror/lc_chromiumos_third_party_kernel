// SPDX-License-Identifier: GPL-2.0
/*
 * Experimental driver for user space interrupt handler.
 *
 * Copyright 2020 Google LLC
 *
 */

#include <linux/acpi.h>
#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/sched.h>
#include <linux/vfio.h>
#include <linux/eventfd.h>
#include <linux/delay.h>
#include <uapi/linux/platirqforward.h>
#include <linux/plat_irqfd.h>

#define VERSION	"0.1"
#define AUTHOR	"Micah Morton <mortonm@chromium.org>"
#define DESC	"Platform IRQ Forwarding"

MODULE_VERSION(VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(AUTHOR);
MODULE_DESCRIPTION(DESC);
MODULE_ALIAS("devname:plat-irq-forward");

static LIST_HEAD(level_triggered_irqs);
static LIST_HEAD(edge_triggered_irqs);


static int plat_irq_forward_unmask_handler_level(int irq, void *data)
{
	unsigned long flags;
	struct plat_irq_forward_level_triggered *l =
			(struct plat_irq_forward_level_triggered *) data;

	spin_lock_irqsave(&(l->spinlock), flags);
	if (l->is_masked) {
		enable_irq(l->irq_num);
		l->is_masked = false;
	}
	spin_unlock_irqrestore(&(l->spinlock), flags);
	return 0;
}


static irqreturn_t plat_irq_forward_handler_level(int irq, void *data)
{
	unsigned long flags;
	int ret = IRQ_NONE;
	struct plat_irq_forward_level_triggered *l =
			(struct plat_irq_forward_level_triggered *) data;
	spin_lock_irqsave(&(l->spinlock), flags);

	disable_irq_nosync(irq);
	l->is_masked = true;
	ret = IRQ_HANDLED;

	spin_unlock_irqrestore(&(l->spinlock), flags);

	if (ret == IRQ_HANDLED)
		eventfd_signal(l->trigger, 1);

	return ret;
}

static irqreturn_t plat_irq_forward_handler_edge(int irq, void *data)
{
	struct plat_irq_forward_edge_triggered *e =
			(struct plat_irq_forward_edge_triggered *) data;
	eventfd_signal(e->trigger, 1);

	return IRQ_HANDLED;
}

static int plat_irq_forward_set_level_trigger(void *data,
		uint32_t irq_number_host,
		struct plat_irq_forward_level_triggered *level)
{
	int32_t fd = *(int32_t *)data;
	struct eventfd_ctx *trigger;
	int ret;

	if (fd < 0) /* Disable only */
		return 0;

	trigger = eventfd_ctx_fdget(fd);
	if (IS_ERR(trigger))
		return PTR_ERR(trigger);

	level->trigger = trigger;
	spin_lock_init(&(level->spinlock));

	ret = request_irq(irq_number_host, plat_irq_forward_handler_level,
			  IRQF_SHARED, "level-triggered-irq", level);
	if (ret == -EINVAL) {
		ret = acpi_register_gsi(NULL, irq_number_host,
					ACPI_LEVEL_SENSITIVE, ACPI_ACTIVE_LOW);
		if (ret < 0) {
			level->trigger = NULL;
			eventfd_ctx_put(trigger);
			return ret;
		}
		ret = request_irq(irq_number_host,
				  plat_irq_forward_handler_level,
				  IRQF_SHARED, "level-triggered-irq", level);
	}
	if (ret) {
		level->trigger = NULL;
		eventfd_ctx_put(trigger);
		return ret;
	}

	return 0;
}

static int plat_irq_forward_set_level_unmask(void *data,
		struct plat_irq_forward_level_triggered *level)
{
	int32_t fd = *(int32_t *)data;

	if (fd >= 0)
		return plat_irq_forward_irqfd_enable(
				plat_irq_forward_unmask_handler_level,
				level, &(level->unmask), fd);
	return -1;
}

static int plat_irq_forward_set_edge_trigger(void *data,
		uint32_t irq_number_host,
		struct plat_irq_forward_edge_triggered *edge)
{
	int32_t fd = *(int32_t *)data;
	struct eventfd_ctx *trigger;
	int ret;

	if (fd < 0) /* Disable only */
		return 0;

	trigger = eventfd_ctx_fdget(fd);
	if (IS_ERR(trigger))
		return PTR_ERR(trigger);

	edge->trigger = trigger;

	ret = request_irq(irq_number_host, plat_irq_forward_handler_edge,
			  IRQF_SHARED, "edge-triggered-irq", edge);

	/* no irq descriptor initialized yet. allocate one owned by
	 * irq-forwarder module.
	 */
	if (ret == -EINVAL) {
		ret = acpi_register_gsi(NULL, irq_number_host,
					ACPI_EDGE_SENSITIVE, ACPI_ACTIVE_LOW);
		if (ret < 0) {
			edge->trigger = NULL;
			eventfd_ctx_put(trigger);
			return ret;
		}
		ret = request_irq(irq_number_host,
				  plat_irq_forward_handler_edge,
				  IRQF_SHARED, "edge-triggered-irq", edge);
	}
	if (ret) {
		edge->trigger = NULL;
		eventfd_ctx_put(trigger);
		return ret;
	}

	return 0;
}


int platform_set_irqs_ioctl_level_trigger(uint32_t irq_number_host, void *data)
{
	struct plat_irq_forward_level_triggered *level_irq = kzalloc(
		sizeof(struct plat_irq_forward_level_triggered), GFP_KERNEL);
	if (!level_irq)
		return -ENOMEM;
	level_irq->trigger = NULL;
	level_irq->irq_num = irq_number_host;
	level_irq->unmask = NULL;
	level_irq->is_masked = true;
	list_add(&(level_irq->list), &level_triggered_irqs);

	return plat_irq_forward_set_level_trigger(data, irq_number_host,
						  level_irq);
}

int platform_set_irqs_ioctl_level_unmask(uint32_t irq_number_host, void *data)
{
	struct list_head *position = NULL;
	struct plat_irq_forward_level_triggered *level_irq = NULL;

	// We must already have a trigger for the IRQ before we add an unmask
	list_for_each(position, &level_triggered_irqs) {
		level_irq = list_entry(position,
				       struct plat_irq_forward_level_triggered,
				       list);
		if (level_irq->irq_num == irq_number_host)
			return plat_irq_forward_set_level_unmask(data,
								 level_irq);
	}

	return -1;
}

int platform_set_irqs_ioctl_edge_trigger(uint32_t irq_number_host, void *data)
{
	struct plat_irq_forward_edge_triggered *edge_irq = kzalloc(
		sizeof(struct plat_irq_forward_edge_triggered), GFP_KERNEL);
	if (!edge_irq)
		return -ENOMEM;
	edge_irq->trigger = NULL;
	edge_irq->irq_num = irq_number_host;
	list_add(&(edge_irq->list), &edge_triggered_irqs);

	return plat_irq_forward_set_edge_trigger(data, irq_number_host,
						 edge_irq);
}

int plat_irq_forward_ioctl(void *device_data, unsigned long arg)
{
	u8 *data = NULL;
	unsigned long minsz;
	struct plat_irq_forward_set hdr;

	minsz = offsetofend(struct plat_irq_forward_set, count);

	if (copy_from_user(&hdr, (void __user *)arg, minsz))
		return -EFAULT;

	data = memdup_user((void __user *)(arg + minsz), sizeof(int32_t));
	if (IS_ERR(data))
		return PTR_ERR(data);

	switch (hdr.action_flags) {
	case PLAT_IRQ_FORWARD_SET_LEVEL_TRIGGER_EVENTFD:
		return platform_set_irqs_ioctl_level_trigger(
				hdr.irq_number_host, data);
	case PLAT_IRQ_FORWARD_SET_LEVEL_UNMASK_EVENTFD:
		return platform_set_irqs_ioctl_level_unmask(
				hdr.irq_number_host, data);
	case PLAT_IRQ_FORWARD_SET_EDGE_TRIGGER:
		return platform_set_irqs_ioctl_edge_trigger(
				hdr.irq_number_host, data);
	default:
		return -EINVAL;
	}

	kfree(data);
	return 0;
}

/**
 * Platform IRQ Forwarding fd, /dev/plat-irq-forward
 */
static long plat_irq_forward_fops_unl_ioctl(struct file *filep,
		unsigned int cmd, unsigned long arg)
{
	long ret = -EINVAL;

	switch (cmd) {
	case PLAT_IRQ_FORWARD_SET:
		ret = (long) plat_irq_forward_ioctl(filep, arg);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
static long plat_irq_forward_fops_compat_ioctl(struct file *filep,
		unsigned int cmd, unsigned long arg)
{
	arg = (unsigned long)compat_ptr(arg);
	return plat_irq_forward_fops_unl_ioctl(filep, cmd, arg);
}
#endif	/* CONFIG_COMPAT */

static int plat_irq_forward_fops_open(struct inode *inode, struct file *filep)
{
	return 0;
}

static int plat_irq_forward_fops_release(struct inode *inode,
					 struct file *filep)
{
	return 0;
}

static const struct file_operations plat_irq_forward_fops = {
	.owner		= THIS_MODULE,
	.open		= plat_irq_forward_fops_open,
	.release	= plat_irq_forward_fops_release,
	.unlocked_ioctl	= plat_irq_forward_fops_unl_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= plat_irq_forward_fops_compat_ioctl,
#endif
};

static struct miscdevice plat_irq_forward_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "plat-irq-forward",
	.fops = &plat_irq_forward_fops,
	.nodename = "plat-irq-forward",
	.mode = 0660,
};

static int __init plat_irq_forward_init(void)
{
	int ret;

	ret = misc_register(&plat_irq_forward_dev);
	if (ret) {
		pr_err("plat-irq-forward: misc device register failed\n");
		return ret;
	}

	pr_info(DESC " version: " VERSION "\n");

	return 0;
}

// TODO: cleanup/free/disconnect stuff
static void __exit plat_irq_forward_cleanup(void)
{
	misc_deregister(&plat_irq_forward_dev);
}

module_init(plat_irq_forward_init);
module_exit(plat_irq_forward_cleanup);
