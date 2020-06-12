/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2020 Google LLC
 */

#ifndef PLAT_IRQFD_H
#define PLAT_IRQFD_H

#ifdef CONFIG_PLAT_IRQ_FORWARD

struct plat_irq_forward_irqfd {
	struct eventfd_ctx	*eventfd;
	int			(*handler)(int irq, void *data);
	void			*data;
	wait_queue_entry_t	wait;
	poll_table		pt;
	struct work_struct	shutdown;
	struct plat_irq_forward_irqfd	**pirqfd;
};

struct plat_irq_forward_edge_triggered {
	struct eventfd_ctx	*trigger;
	uint32_t		irq_num;
	struct list_head	list;
};

struct plat_irq_forward_level_triggered {
	struct eventfd_ctx	*trigger;
	uint32_t		irq_num;
	struct list_head	list;
	struct plat_irq_forward_irqfd	*unmask;
	bool			is_masked;
	spinlock_t		spinlock;
};

int plat_irq_forward_irqfd_enable(int (*handler)(int irq, void *data),
		void *data, struct plat_irq_forward_irqfd **pirqfd, int fd);

#endif /* CONFIG_PLAT_IRQ_FORWARD */
#endif /* PLAT_IRQFD_H */
