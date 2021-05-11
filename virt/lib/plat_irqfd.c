// SPDX-License-Identifier: GPL-2.0
/*
 * Experimental driver for user space interrupt handler.
 *
 * Copyright 2020 Google LLC
 *
 */

#include <linux/file.h>
#include <linux/vfio.h>
#include <linux/eventfd.h>
#include <linux/slab.h>
#include <uapi/linux/platirqforward.h>
#include <linux/plat_irqfd.h>

static DEFINE_SPINLOCK(plat_irqfd_lock);

static void plat_irqfd_deactivate(struct plat_irq_forward_irqfd *plat_irqfd)
{
	schedule_work(&plat_irqfd->shutdown);
}

static int plat_irqfd_wakeup(wait_queue_entry_t *wait, unsigned int mode,
			     int sync, void *key)
{
	struct plat_irq_forward_irqfd *plat_irqfd =
			container_of(wait, struct plat_irq_forward_irqfd, wait);
	__poll_t flags = key_to_poll(key);

	if (flags & EPOLLIN) {
		/* An event has been signaled, call function */
		if (!plat_irqfd->handler ||
		    plat_irqfd->handler(-1, plat_irqfd->data))
			pr_emerg("handler failed\n");
	}

	if (flags & EPOLLHUP) {
		unsigned long flags;

		spin_lock_irqsave(&plat_irqfd_lock, flags);

		/*
		 * The eventfd is closing, if the plat_irqfd has not yet been
		 * queued for release, as determined by testing whether the
		 * plat_irqfd pointer to it is still valid, queue it now.  As
		 * with kvm irqfds, we know we won't race against the plat_irqfd
		 * going away because we hold the lock to get here.
		 */
		if (*(plat_irqfd->pirqfd) == plat_irqfd) {
			*(plat_irqfd->pirqfd) = NULL;
			plat_irqfd_deactivate(plat_irqfd);
		}

		spin_unlock_irqrestore(&plat_irqfd_lock, flags);
	}

	return 0;
}


static void plat_irqfd_ptable_queue_proc(struct file *file,
					 wait_queue_head_t *wqh, poll_table *pt)
{
	struct plat_irq_forward_irqfd *plat_irqfd =
			container_of(pt, struct plat_irq_forward_irqfd, pt);
	add_wait_queue(wqh, &plat_irqfd->wait);
}

static void plat_irqfd_shutdown(struct work_struct *work)
{
	struct plat_irq_forward_irqfd *plat_irqfd = container_of(work,
			struct plat_irq_forward_irqfd, shutdown);
	u64 cnt;

	eventfd_ctx_remove_wait_queue(plat_irqfd->eventfd, &plat_irqfd->wait,
				      &cnt);
	eventfd_ctx_put(plat_irqfd->eventfd);

	kfree(plat_irqfd);
}

int plat_irq_forward_irqfd_enable(int (*handler)(int, void *), void *data,
		struct plat_irq_forward_irqfd **pirqfd, int fd)
{
	struct fd irqfd;
	struct eventfd_ctx *ctx;
	struct plat_irq_forward_irqfd *plat_irqfd;
	int ret = 0;
	unsigned int events;

	plat_irqfd = kzalloc(sizeof(*plat_irqfd), GFP_KERNEL);
	if (!plat_irqfd)
		return -ENOMEM;

	plat_irqfd->pirqfd = pirqfd;
	plat_irqfd->handler = handler;
	plat_irqfd->data = data;

	// shutdown causes crash
	INIT_WORK(&plat_irqfd->shutdown, plat_irqfd_shutdown);

	irqfd = fdget(fd);
	if (!irqfd.file) {
		ret = -EBADF;
		goto err_fd;
	}

	ctx = eventfd_ctx_fileget(irqfd.file);
	if (IS_ERR(ctx)) {
		ret = PTR_ERR(ctx);
		goto err_ctx;
	}

	plat_irqfd->eventfd = ctx;

	// plat_irqfds can be released by closing the eventfd or directly
	// through ioctl.  These are both done through a workqueue, so
	// we update the pointer to the plat_irqfd under lock to avoid
	// pushing multiple jobs to release the same plat_irqfd.
	spin_lock_irq(&plat_irqfd_lock);

	if (*pirqfd) {
		pr_emerg("pirqfd should be NULL. BUG!\n");
		spin_unlock_irq(&plat_irqfd_lock);
		ret = -EBUSY;
		goto err_busy;
	}
	*pirqfd = plat_irqfd;

	spin_unlock_irq(&plat_irqfd_lock);

	// Install our own custom wake-up handling so we are notified via
	// a callback whenever someone signals the underlying eventfd.
	init_waitqueue_func_entry(&plat_irqfd->wait, plat_irqfd_wakeup);
	init_poll_funcptr(&plat_irqfd->pt, plat_irqfd_ptable_queue_proc);

	events = irqfd.file->f_op->poll(irqfd.file, &plat_irqfd->pt);

	// Check if there was an event already pending on the eventfd
	// before we registered and trigger it as if we didn't miss it.
	if (events & POLLIN) {
		if (!handler || handler(-1, data))
			pr_emerg("handler failed\n");
	}

	// Do not drop the file until the irqfd is fully initialized,
	// otherwise we might race against the POLLHUP.
	fdput(irqfd);

	return 0;
err_busy:
	eventfd_ctx_put(ctx);
err_ctx:
	fdput(irqfd);
err_fd:
	kfree(plat_irqfd);

	return ret;
}
EXPORT_SYMBOL_GPL(plat_irq_forward_plat_irqfd_enable);
