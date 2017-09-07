/* Copyright (c) 2016-2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) "%s " fmt, KBUILD_MODNAME

#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <asm-generic/io.h>
#include <soc/qcom/tcs.h>
#include <dt-bindings/soc/qcom,rpmh-rsc.h>
#include "mailbox.h"

#define CREATE_TRACE_POINTS
#include <trace/events/rpmh.h>

#define MAX_CMDS_PER_TCS		16
#define MAX_TCS_PER_TYPE		3
#define MAX_TCS_SLOTS			MAX_CMDS_PER_TCS * MAX_TCS_PER_TYPE

#define RSC_DRV_TCS_OFFSET		672
#define RSC_DRV_CMD_OFFSET		20

/* DRV Configuration Information Register */
#define DRV_PRNT_CHLD_CONFIG		0x0C
#define DRV_NUM_TCS_MASK		0x3F
#define DRV_NUM_TCS_SHIFT		6
#define DRV_NCPT_MASK			0x1F
#define DRV_NCPT_SHIFT			27

/* Register offsets */
#define RSC_DRV_IRQ_ENABLE		0x00
#define RSC_DRV_IRQ_STATUS		0x04
#define RSC_DRV_IRQ_CLEAR		0x08
#define RSC_DRV_CMD_WAIT_FOR_CMPL	0x10
#define RSC_DRV_CONTROL			0x14
#define RSC_DRV_STATUS			0x18
#define RSC_DRV_CMD_ENABLE		0x1C
#define RSC_DRV_CMD_MSGID		0x30
#define RSC_DRV_CMD_ADDR		0x34
#define RSC_DRV_CMD_DATA		0x38
#define RSC_DRV_CMD_STATUS		0x3C
#define RSC_DRV_CMD_RESP_DATA		0x40

#define TCS_AMC_MODE_ENABLE		BIT(16)
#define TCS_AMC_MODE_TRIGGER		BIT(24)

/* TCS CMD register bit mask */
#define CMD_MSGID_LEN			8
#define CMD_MSGID_RESP_REQ		BIT(8)
#define CMD_MSGID_WRITE			BIT(16)
#define CMD_STATUS_ISSUED		BIT(8)
#define CMD_STATUS_COMPL		BIT(16)

#define TCS_TYPE_NR			4
#define MAX_TCS_NR			(MAX_TCS_PER_TYPE * TCS_TYPE_NR)

struct rsc_drv;

struct tcs_response {
	struct rsc_drv *drv;
	struct mbox_chan *chan;
	struct tcs_mbox_msg *msg;
	u32 m; /* m-th TCS */
	int err;
	struct list_head list;
};

/* One per TCS type of a controller */
struct tcs_mbox {
	struct rsc_drv *drv;
	int type;		/* type of TCS */
	u32 tcs_mask;		/* Mask of the TCSes of a type */
	u32 tcs_offset;		/* Start index of TCS */
	int num_tcs;		/* # of TCS in this type */
	int ncpt;		/* num cmds per tcs */
	spinlock_t tcs_lock;	/* TCS type lock */
	struct tcs_response *responses[MAX_TCS_PER_TYPE];
				/* Response object for each TCS */
	u32 *cmd_addr;		/* Flattened cache of cmds in TCS */
	DECLARE_BITMAP(slots, MAX_TCS_SLOTS);
};

/* One per MBOX controller */
struct rsc_drv {
	struct mbox_controller mbox;
	struct platform_device *pdev;
	const char *name;
	void __iomem *base;	/* start address of the RSC's registers */
	void __iomem *reg_base;	/* start address for DRV specific register */
	int drv_id;		/* DRV instance in the RSC */
	int num_assigned;	/* # of allocated channels */
	int num_tcs;		/* total # of TCS in this DRV */
	struct tasklet_struct tasklet;		/* to handle tx_done callback */
	struct list_head response_pending;	/* list of pending responses */
	struct tcs_mbox tcs[TCS_TYPE_NR];	/* TCS of each type */
	atomic_t tcs_in_use[MAX_TCS_NR];	/* s/w state of the TCS h/w */
	spinlock_t drv_lock;
};

static inline struct tcs_mbox *get_tcs_from_index(struct rsc_drv *drv, int m)
{
	struct tcs_mbox *tcs = NULL;
	int i;

	for (i = 0; i < drv->num_tcs; i++) {
		tcs = &drv->tcs[i];
		if (tcs->tcs_mask & (u32)BIT(m))
			break;
	}

	if (i == drv->num_tcs) {
		WARN(1, "Incorrect TCS index %d", m);
		tcs = NULL;
	}

	return tcs;
}

static struct tcs_response *setup_response(struct rsc_drv *drv,
		struct tcs_mbox_msg *msg, struct mbox_chan *chan, int m, int err)
{
	struct tcs_response *resp;
	struct tcs_mbox *tcs;

	resp = kcalloc(1, sizeof(*resp), GFP_ATOMIC);
	if (!resp)
		return ERR_PTR(-ENOMEM);

	resp->drv = drv;
	resp->chan = chan;
	resp->msg = msg;
	resp->err = err;

	/*
	 * If we are requesting for an error response,
	 * there is nothing else to do.
	 */
	if (m < 0)
		return resp;

	tcs = get_tcs_from_index(drv, m);
	if (!tcs)
		return ERR_PTR(-EINVAL);

	/*
	 * We should have beenn called from a TCS-type locked context, and
	 * we overwrite the previously saved reponse.
	 */
	tcs->responses[m - tcs->tcs_offset] = resp;

	return resp;
}

static void free_response(struct tcs_response *resp)
{
	kfree(resp);
}

static inline struct tcs_response *get_response(struct rsc_drv *drv, u32 m)
{
	struct tcs_mbox *tcs = get_tcs_from_index(drv, m);

	return tcs->responses[m - tcs->tcs_offset];
}

static inline u32 read_drv_config(void __iomem *base)
{
	return le32_to_cpu(readl_relaxed(base + DRV_PRNT_CHLD_CONFIG));
}

static inline u32 read_tcs_reg(void __iomem *base, int reg, int m, int n)
{
	return le32_to_cpu(readl_relaxed(base + reg +
			RSC_DRV_TCS_OFFSET * m + RSC_DRV_CMD_OFFSET * n));
}

static inline void write_tcs_reg(void __iomem *base, int reg, int m, int n,
				u32 data)
{
	writel_relaxed(cpu_to_le32(data), base + reg +
			RSC_DRV_TCS_OFFSET * m + RSC_DRV_CMD_OFFSET * n);
}

static inline void write_tcs_reg_sync(void __iomem *base, int reg, int m, int n,
				u32 data)
{
	do {
		write_tcs_reg(base, reg, m, n, data);
		if (data == read_tcs_reg(base, reg, m, n))
			break;
		udelay(1);
	} while (1);
}

static inline bool tcs_is_free(struct rsc_drv *drv, int m)
{
	void __iomem *base = drv->reg_base;

	return read_tcs_reg(base, RSC_DRV_STATUS, m, 0) &&
			!atomic_read(&drv->tcs_in_use[m]);
}

static inline struct tcs_mbox *get_tcs_of_type(struct rsc_drv *drv, int type)
{
	int i;
	struct tcs_mbox *tcs;

	for (i = 0; i < TCS_TYPE_NR; i++)
		if (type == drv->tcs[i].type)
			break;

	if (i == TCS_TYPE_NR)
		return ERR_PTR(-EINVAL);

	tcs = &drv->tcs[i];
	if (!tcs->num_tcs)
		return ERR_PTR(-EINVAL);

	return tcs;
}

/**
 * tcs_mbox_invalidate - Invalidate sleep and wake TCSes
 *
 * @drv: the mailbox controller
 */
static int tcs_mbox_invalidate(struct rsc_drv *drv)
{
	struct tcs_mbox *tcs;
	int m;
	int inv_types[] = { WAKE_TCS, SLEEP_TCS };
	int type = 0;
	unsigned long drv_flags, flags;
	int ret = 0;

	/* Lock the DRV and clear sleep and wake TCSes */
	spin_lock_irqsave(&drv->drv_lock, drv_flags);
	do {
		tcs = get_tcs_of_type(drv, inv_types[type]);
		if (IS_ERR(tcs))
			continue;

		spin_lock_irqsave(&tcs->tcs_lock, flags);
		if (!bitmap_empty(tcs->slots, MAX_TCS_SLOTS)) {
			/* Clear the enable register for each TCS of the type */
			for (m = tcs->tcs_offset;
				m < tcs->tcs_offset + tcs->num_tcs; m++)
				if (!tcs_is_free(drv, m)) {
					spin_unlock_irqrestore(&tcs->tcs_lock,
									flags);
					ret = -EAGAIN;
					goto drv_unlock;
				}
				write_tcs_reg_sync(drv->reg_base,
						RSC_DRV_CMD_ENABLE, m, 0, 0);
			/* Mark the TCS slots as free */
			bitmap_zero(tcs->slots, MAX_TCS_SLOTS);
		}
		spin_unlock_irqrestore(&tcs->tcs_lock, flags);
	} while (++type < ARRAY_SIZE(inv_types));
drv_unlock:
	spin_unlock_irqrestore(&drv->drv_lock, drv_flags);

	return ret;
}

static inline struct tcs_mbox *get_tcs_for_msg(struct rsc_drv *drv,
						struct tcs_mbox_msg *msg)
{
	int type = -1;

	switch (msg->state) {
	case RPMH_ACTIVE_ONLY_STATE:
		type = ACTIVE_TCS;
		break;
	case RPMH_WAKE_ONLY_STATE:
		type = WAKE_TCS;
		break;
	case RPMH_SLEEP_STATE:
		type = SLEEP_TCS;
		break;
	default:
		break;
	}

	if (type < 0)
		return ERR_PTR(-EINVAL);

	return get_tcs_of_type(drv, type);
}

static inline void send_tcs_response(struct tcs_response *resp)
{
	struct rsc_drv *drv = resp->drv;
	unsigned long flags;

	spin_lock_irqsave(&drv->drv_lock, flags);
	INIT_LIST_HEAD(&resp->list);
	list_add_tail(&resp->list, &drv->response_pending);
	spin_unlock_irqrestore(&drv->drv_lock, flags);

	tasklet_schedule(&drv->tasklet);
}

/**
 * tcs_irq_handler: TX Done interrupt handler
 */
static irqreturn_t tcs_irq_handler(int irq, void *p)
{
	struct rsc_drv *drv = p;
	void __iomem *base = drv->reg_base;
	int m, i;
	u32 irq_status, sts;
	struct tcs_response *resp;
	struct tcs_cmd *cmd;
	int err;

	irq_status = read_tcs_reg(base, RSC_DRV_IRQ_STATUS, 0, 0);

	for (m = 0; m < drv->num_tcs; m++) {
		if (!(irq_status & (u32)BIT(m)))
			continue;

		err = 0;
		resp = get_response(drv, m);
		if (!resp) {
			for (i = 0; i < resp->msg->num_payload; i++) {
				cmd = &resp->msg->payload[i];
				sts = read_tcs_reg(base, RSC_DRV_CMD_STATUS, m, i);
				if ((!(sts & CMD_STATUS_ISSUED)) ||
					((resp->msg->is_complete ||
					  cmd->complete) &&
					(!(sts & CMD_STATUS_COMPL)))) {
					resp->err = -EIO;
					break;
				}
			}
		}

		trace_rpmh_notify_irq(drv->name, m, resp->msg->payload[0].addr,
						resp->err);
		write_tcs_reg(base, RSC_DRV_CMD_ENABLE, m, 0, 0);
		write_tcs_reg(base, RSC_DRV_IRQ_CLEAR, 0, 0, BIT(m));
		atomic_set(&drv->tcs_in_use[m], 0);

		if (resp) {
			resp->err = err;
			send_tcs_response(resp);
		}
	}

	return IRQ_HANDLED;
}

/**
 * tcs_notify_tx_done: TX Done for requests that got a response
 *
 * @data: the tasklet argument
 *
 * Tasklet function to notify MBOX that we are done with the request.
 * Handles all pending reponses whenever run.
 */
static void tcs_notify_tx_done(unsigned long data)
{
	struct rsc_drv *drv = (struct rsc_drv *)data;
	struct tcs_response *resp;
	unsigned long flags;
	struct mbox_chan *chan;
	int err, m;
	struct tcs_mbox_msg *msg;

	do {
		spin_lock_irqsave(&drv->drv_lock, flags);
		if (list_empty(&drv->response_pending)) {
			spin_unlock_irqrestore(&drv->drv_lock, flags);
			break;
		}
		resp = list_first_entry(&drv->response_pending,
					struct tcs_response, list);
		list_del(&resp->list);
		spin_unlock_irqrestore(&drv->drv_lock, flags);
		chan = resp->chan;
		err = resp->err;
		m = resp->m;
		msg = resp->msg;
		free_response(resp);
		trace_rpmh_notify(drv->name, m, msg->payload[0].addr, err);
		mbox_chan_txdone(chan, err);

	} while (1);
}

static void __tcs_buffer_write(struct rsc_drv *drv, int m, int n,
				struct tcs_mbox_msg *msg)
{
	void __iomem *base = drv->reg_base;
	u32 msgid, cmd_msgid = 0;
	u32 cmd_enable = 0;
	u32 cmd_complete;
	struct tcs_cmd *cmd;
	int i;

	cmd_msgid = CMD_MSGID_LEN;
	cmd_msgid |= (msg->is_complete) ? CMD_MSGID_RESP_REQ : 0;
	cmd_msgid |= CMD_MSGID_WRITE;

	cmd_complete = read_tcs_reg(base, RSC_DRV_CMD_WAIT_FOR_CMPL, m, 0);

	for (i = 0; i < msg->num_payload; i++) {
		cmd = &msg->payload[i];
		cmd_enable |= BIT(n + i);
		cmd_complete |= cmd->complete << (n + i);
		msgid = cmd_msgid;
		msgid |= (cmd->complete) ? CMD_MSGID_RESP_REQ : 0;
		write_tcs_reg(base, RSC_DRV_CMD_MSGID, m, n + i, msgid);
		write_tcs_reg(base, RSC_DRV_CMD_ADDR, m, n + i, cmd->addr);
		write_tcs_reg(base, RSC_DRV_CMD_DATA, m, n + i, cmd->data);
		trace_rpmh_send_msg(drv->name, m, n + i, msgid, cmd->addr,
						cmd->data, cmd->complete);
	}

	write_tcs_reg(base, RSC_DRV_CMD_WAIT_FOR_CMPL, m, 0, cmd_complete);
	cmd_enable |= read_tcs_reg(base, RSC_DRV_CMD_ENABLE, m, 0);
	write_tcs_reg(base, RSC_DRV_CMD_ENABLE, m, 0, cmd_enable);
}

static void __tcs_trigger(struct rsc_drv *drv, int m)
{
	void __iomem *base = drv->reg_base;
	u32 enable;

	/*
	 * HW req: Clear the DRV_CONTROL and enable TCS again
	 * While clearing ensure that the AMC mode trigger is cleared
	 * and then the mode enable is cleared.
	 */
	enable = read_tcs_reg(base, RSC_DRV_CONTROL, m, 0);
	enable &= ~TCS_AMC_MODE_TRIGGER;
	write_tcs_reg_sync(base, RSC_DRV_CONTROL, m, 0, enable);
	enable &= ~TCS_AMC_MODE_ENABLE;
	write_tcs_reg_sync(base, RSC_DRV_CONTROL, m, 0, enable);

	/* Enable the AMC mode on the TCS and then trigger the TCS */
	enable = TCS_AMC_MODE_ENABLE;
	write_tcs_reg_sync(base, RSC_DRV_CONTROL, m, 0, enable);
	enable |= TCS_AMC_MODE_TRIGGER;
	write_tcs_reg_sync(base, RSC_DRV_CONTROL, m, 0, enable);
}

static int check_for_req_inflight(struct rsc_drv *drv, struct tcs_mbox *tcs,
						struct tcs_mbox_msg *msg)
{
	u32 curr_enabled, addr;
	int i, j, k;
	void __iomem *base = drv->reg_base;
	int m = tcs->tcs_offset;

	for (i = 0; i < tcs->num_tcs; i++, m++) {
		if (tcs_is_free(drv, m))
			continue;

		curr_enabled = read_tcs_reg(base, RSC_DRV_CMD_ENABLE, m, 0);

		for (j = 0; j < MAX_CMDS_PER_TCS; j++) {
			if (!(curr_enabled & (u32)BIT(j)))
				continue;

			addr = read_tcs_reg(base, RSC_DRV_CMD_ADDR, m, j);
			for (k = 0; k < msg->num_payload; k++) {
				if (addr == msg->payload[k].addr)
					return -EBUSY;
			}
		}
	}

	return 0;
}

static int find_free_tcs(struct tcs_mbox *tcs)
{
	int m;

	for (m = 0; m < tcs->num_tcs; m++)
		if (tcs_is_free(tcs->drv, tcs->tcs_offset + m))
			break;

	return (m != tcs->num_tcs) ? m : -EBUSY;
}

static int tcs_mbox_write(struct mbox_chan *chan, struct tcs_mbox_msg *msg)
{
	struct rsc_drv *drv = container_of(chan->mbox, struct rsc_drv, mbox);
	struct tcs_mbox *tcs;
	int m;
	struct tcs_response *resp = NULL;
	unsigned long flags;
	int ret = 0;

	tcs = get_tcs_for_msg(drv, msg);
	if (IS_ERR(tcs))
		return PTR_ERR(tcs);

	spin_lock_irqsave(&tcs->tcs_lock, flags);
	m = find_free_tcs(tcs);
	if (m < 0) {
		ret = m;
		goto done_write;
	}

	/*
	 * The h/w does not like if we send a request to the same address,
	 * when one is already in-flight or bring processed.
	 */
	ret = check_for_req_inflight(drv, tcs, msg);
	if (ret)
		goto done_write;

	resp = setup_response(drv, msg, chan, m, 0);
	if (IS_ERR_OR_NULL(resp)) {
		ret = PTR_ERR(resp);
		goto done_write;
	}
	resp->m = m;

	atomic_set(&drv->tcs_in_use[m], 1);
	__tcs_buffer_write(drv, m, 0, msg);
	__tcs_trigger(drv, m);

done_write:
	spin_unlock_irqrestore(&tcs->tcs_lock, flags);
	return ret;
}

/**
 * chan_tcs_write: Validate the incoming message and write to the
 * appropriate TCS block.
 *
 * @chan: the MBOX channel
 * @data: the tcs_mbox_msg*
 *
 * Returns a negative error for invalid message structure and invalid
 * message combination, -EBUSY if there is an other active request for
 * the channel in process, otherwise bubbles up internal error.
 */
static int chan_tcs_write(struct mbox_chan *chan, void *data)
{
	struct rsc_drv *drv = container_of(chan->mbox, struct rsc_drv, mbox);
	struct tcs_mbox_msg *msg = data;
	int ret = 0;

	if (!msg) {
		ret = -EINVAL;
		goto tx_fail;
	}

	if (!msg->payload || !msg->num_payload ||
			msg->num_payload > MAX_RPMH_PAYLOAD) {
		ret = -EINVAL;
		goto tx_fail;
	}

	ret = tcs_mbox_write(chan, msg);

tx_fail:
	/* If there was an error in the request, schedule a response */
	if (ret < 0 && ret != -EBUSY) {
		struct tcs_response *resp = setup_response(drv, msg, chan,
							-1, ret);

		pr_err("Error sending RPMH message %d\n", ret);
		if (!IS_ERR(resp))
			send_tcs_response(resp);
		else
			pr_err("No response object %ld\n", PTR_ERR(resp));
		ret = 0;
	}

	/* If we were just busy waiting for TCS, dump the state and return */
	if (ret == -EBUSY) {
		pr_info_ratelimited("TCS Busy, retrying RPMH message send\n");
		ret = -EAGAIN;
	}

	return ret;
}

static int find_match(struct tcs_mbox *tcs, struct tcs_cmd *cmd, int len)
{
	bool found = false;
	int i = 0, j;

	/* Check for already cached commands */
	while ((i = find_next_bit(tcs->slots, MAX_TCS_SLOTS, i)) <
			MAX_TCS_SLOTS) {
		if (tcs->cmd_addr[i] != cmd[0].addr) {
			i++;
			continue;
		}
		/* sanity check to ensure the seq is same */
		for (j = 1; j < len; j++) {
			WARN((tcs->cmd_addr[i + j] != cmd[j].addr),
				"Message does not match previous sequence.\n");
			return -EINVAL;
		}
		found = true;
		break;
	}

	return found ? i : -1;
}

static int find_slots(struct tcs_mbox *tcs, struct tcs_mbox_msg *msg,
						int *m, int *n)
{
	int slot, offset;
	int i = 0;

	/* Find if we already have the msg in our TCS */
	slot = find_match(tcs, msg->payload, msg->num_payload);
	if (slot >= 0)
		goto copy_data;

	/* Do over, until we can fit the full payload in a TCS */
	do {
		slot = bitmap_find_next_zero_area(tcs->slots, MAX_TCS_SLOTS,
				i, msg->num_payload, 0);
		if (slot == MAX_TCS_SLOTS)
			break;
		i += tcs->ncpt;
	} while (slot + msg->num_payload - 1 >= i);

	if (slot == MAX_TCS_SLOTS)
		return -ENOMEM;

copy_data:
	bitmap_set(tcs->slots, slot, msg->num_payload);
	/* Copy the addresses of the resources over to the slots */
	for (i = 0; tcs->cmd_addr && i < msg->num_payload; i++)
		tcs->cmd_addr[slot + i] = msg->payload[i].addr;

	offset = slot / tcs->ncpt;
	*m = offset + tcs->tcs_offset;
	*n = slot % tcs->ncpt;

	return 0;
}

static int tcs_ctrl_write(struct rsc_drv *drv, struct tcs_mbox_msg *msg)
{
	struct tcs_mbox *tcs;
	int m = 0, n = 0;
	unsigned long flags;
	int ret = 0;

	tcs = get_tcs_for_msg(drv, msg);
	if (IS_ERR(tcs))
		return PTR_ERR(tcs);

	spin_lock_irqsave(&tcs->tcs_lock, flags);
	/* find the m-th TCS and the n-th position in the TCS to write to */
	ret = find_slots(tcs, msg, &m, &n);
	if (!ret)
		__tcs_buffer_write(drv, m, n, msg);
	spin_unlock_irqrestore(&tcs->tcs_lock, flags);

	return ret;
}

/**
 * chan_tcs_ctrl_write: Write message to the controller, no ACK sent.
 *
 * @chan: the MBOX channel
 * @data: the tcs_mbox_msg*
 */
static int chan_tcs_ctrl_write(struct mbox_chan *chan, void *data)
{
	struct rsc_drv *drv = container_of(chan->mbox, struct rsc_drv, mbox);
	struct tcs_mbox_msg *msg = data;

	if (!msg || ((!msg->payload || !msg->num_payload) && !msg->invalidate)
				|| msg->num_payload > MAX_RPMH_PAYLOAD) {
		pr_err("Payload error\n");
		return -EINVAL;
	}

	/* Data sent to this API will not be sent immediately */
	if (msg->state == RPMH_ACTIVE_ONLY_STATE)
		return -EFAULT;

	if (msg->invalidate)
		return tcs_mbox_invalidate(drv);

	/* Post the message to the TCS without trigger */
	return tcs_ctrl_write(drv, msg);
}

static const struct mbox_chan_ops mbox_ops = {
	.send_data = chan_tcs_write,
	.write_controller_data = chan_tcs_ctrl_write,
};

static struct mbox_chan *of_tcs_mbox_xlate(struct mbox_controller *mbox,
				const struct of_phandle_args *sp)
{
	struct rsc_drv *drv = container_of(mbox, struct rsc_drv, mbox);
	struct mbox_chan *chan;

	if (drv->num_assigned >= mbox->num_chans) {
		pr_err("TCS-Mbox out of channel memory\n");
		return ERR_PTR(-ENOMEM);
	}

	chan = &mbox->chans[drv->num_assigned++];
	chan->con_priv = drv;

	return chan;
}

static int rpmh_rsc_probe(struct platform_device *pdev)
{
	struct device_node *dn = pdev->dev.of_node;
	struct device_node *np;
	struct rsc_drv *drv;
	struct mbox_chan *chans;
	struct tcs_mbox *tcs;
	struct of_phandle_args p;
	int irq;
	u32 val[8] = { 0 };
	int num_chans = 0;
	int st = 0;
	int i, j, ret, nelem;
	u32 config, max_tcs, ncpt;
	int tcs_type_count[TCS_TYPE_NR] = { 0 };
	struct resource *res;

	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	ret = of_property_read_u32(dn, "qcom,drv-id", &drv->drv_id);
	if (ret)
		return ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	drv->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(drv->base))
		return PTR_ERR(drv->base);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res)
		return -EINVAL;
	drv->reg_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(drv->reg_base))
		return PTR_ERR(drv->reg_base);

	config = read_drv_config(drv->base);
	max_tcs = config & (DRV_NUM_TCS_MASK <<
				(DRV_NUM_TCS_SHIFT * drv->drv_id));
	max_tcs = max_tcs >> (DRV_NUM_TCS_SHIFT * drv->drv_id);
	ncpt = config & (DRV_NCPT_MASK << DRV_NCPT_SHIFT);
	ncpt = ncpt >> DRV_NCPT_SHIFT;

	nelem = of_property_count_elems_of_size(dn, "qcom,tcs-config",
						sizeof(u32));
	if (!nelem || (nelem % 2) || (nelem > 2 * TCS_TYPE_NR))
		return -EINVAL;

	ret = of_property_read_u32_array(dn, "qcom,tcs-config", val, nelem);
	if (ret)
		return ret;

	for (i = 0; i < (nelem / 2); i++) {
		if (val[2 * i] >= TCS_TYPE_NR)
			return -EINVAL;
		tcs_type_count[val[2 * i]]++;
		if (tcs_type_count[val[2 * i]] > 1)
			return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(tcs_type_count); i++)
		if (!tcs_type_count[i])
			return -EINVAL;

	for (i = 0; i < (nelem / 2); i++) {
		tcs = &drv->tcs[val[2 * i]];
		tcs->drv = drv;
		tcs->type = val[2 * i];
		tcs->num_tcs = val[2 * i + 1];
		tcs->ncpt = ncpt;
		spin_lock_init(&tcs->tcs_lock);

		if (tcs->num_tcs <= 0 || tcs->type == CONTROL_TCS)
			continue;

		if (tcs->num_tcs > MAX_TCS_PER_TYPE ||
			st + tcs->num_tcs > max_tcs ||
			st + tcs->num_tcs >= 8 * sizeof(tcs->tcs_mask))
			return -EINVAL;

		tcs->tcs_mask = ((1 << tcs->num_tcs) - 1) << st;
		tcs->tcs_offset = st;
		st += tcs->num_tcs;

		/*
		 * Allocate memory to cache sleep and wake requests to
		 * avoid reading TCS register memory.
		 */
		if (tcs->type == ACTIVE_TCS)
			continue;

		tcs->cmd_addr = devm_kzalloc(&pdev->dev, sizeof(u32) *
				tcs->num_tcs * tcs->ncpt, GFP_KERNEL);
		if (!tcs->cmd_addr)
			return -ENOMEM;
	}

	for_each_node_with_property(np, "mboxes") {
		if (!of_device_is_available(np))
			continue;
		i = of_count_phandle_with_args(np, "mboxes", "#mbox-cells");
		for (j = 0; j < i; j++) {
			ret = of_parse_phandle_with_args(np, "mboxes",
							"#mbox-cells", j, &p);
			of_node_put(p.np);
			if (!ret && p.np == pdev->dev.of_node) {
				num_chans++;
				break;
			}
		}
	}

	if (!num_chans) {
		pr_err("%s: No clients for controller (%s)\n", __func__,
							dn->full_name);
		return -ENODEV;
	}

	chans = devm_kzalloc(&pdev->dev, num_chans * sizeof(*chans),
				GFP_KERNEL);
	if (!chans)
		return -ENOMEM;

	for (i = 0; i < num_chans; i++) {
		chans[i].mbox = &drv->mbox;
		chans[i].txdone_method = TXDONE_BY_IRQ;
	}

	drv->mbox.dev = &pdev->dev;
	drv->mbox.ops = &mbox_ops;
	drv->mbox.chans = chans;
	drv->mbox.num_chans = num_chans;
	drv->mbox.txdone_irq = true;
	drv->mbox.of_xlate = of_tcs_mbox_xlate;
	drv->num_tcs = st;
	drv->pdev = pdev;
	INIT_LIST_HEAD(&drv->response_pending);
	spin_lock_init(&drv->drv_lock);
	tasklet_init(&drv->tasklet, tcs_notify_tx_done, (unsigned long)drv);

	drv->name = of_get_property(pdev->dev.of_node, "label", NULL);
	if (!drv->name)
		drv->name = dev_name(&pdev->dev);

	irq = of_irq_get(dn, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(&pdev->dev, irq, tcs_irq_handler,
			IRQF_TRIGGER_HIGH | IRQF_NO_SUSPEND, drv->name, drv);
	if (ret)
		return ret;

	write_tcs_reg(drv->reg_base, RSC_DRV_IRQ_ENABLE, 0, 0,
					drv->tcs[ACTIVE_TCS].tcs_mask);

	for (i = 0; i < ARRAY_SIZE(drv->tcs_in_use); i++)
		atomic_set(&drv->tcs_in_use[i], 0);

	ret = mbox_controller_register(&drv->mbox);
	if (ret)
		return ret;

	pr_debug("Mailbox controller (%s, drv=%d) registered\n",
					dn->full_name, drv->drv_id);

	return 0;
}

static const struct of_device_id rpmh_drv_match[] = {
	{ .compatible = "qcom,rpmh_rsc", },
	{ }
};

static struct platform_driver rpmh_mbox_driver = {
	.probe = rpmh_rsc_probe,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = rpmh_drv_match,
	},
};

static int __init rpmh_mbox_driver_init(void)
{
	return platform_driver_register(&rpmh_mbox_driver);
}
arch_initcall(rpmh_mbox_driver_init);
