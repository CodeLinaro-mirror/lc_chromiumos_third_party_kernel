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

#include <linux/atomic.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>

#include <soc/qcom/rpmh.h>
#include <soc/qcom/tcs.h>

#define RPMH_MAX_MBOXES			2
#define RPMH_TIMEOUT			msecs_to_jiffies(10000)

#define DEFINE_RPMH_MSG_ONSTACK(rc, s, q, c, name)	\
	struct rpmh_msg name = {			\
		.msg = {				\
			.state = s,			\
			.payload = name.cmd,		\
			.num_payload = 0,		\
			.is_complete = true,		\
			.invalidate = false,		\
		},					\
		.cmd = { { 0 } },			\
		.completion = q,			\
		.wait_count = c,			\
		.rc = rc,				\
		.free = NULL,				\
	}

struct rpmh_req {
	u32 addr;
	u32 sleep_val;
	u32 wake_val;
	struct list_head list;
};

struct rpmh_msg {
	struct tcs_mbox_msg msg;
	struct tcs_cmd cmd[MAX_RPMH_PAYLOAD];
	struct completion *completion;
	atomic_t *wait_count;
	struct rpmh_client *rc;
	int err; /* relay error from mbox for sync calls */
	struct rpmh_msg *free;
};

struct rpmh_mbox {
	struct device_node *mbox_dn;
	struct list_head resources;
	spinlock_t lock;
	bool dirty;
};

struct rpmh_client {
	struct device *dev;
	struct mbox_client client;
	struct mbox_chan *chan;
	struct rpmh_mbox *rpmh;
};

static struct rpmh_mbox mbox_ctrlr[RPMH_MAX_MBOXES];
DEFINE_MUTEX(rpmh_mbox_mutex);

static void rpmh_tx_done(struct mbox_client *cl, void *msg, int r)
{
	struct rpmh_msg *rpm_msg = container_of(msg, struct rpmh_msg, msg);
	atomic_t *wc = rpm_msg->wait_count;
	struct completion *compl = rpm_msg->completion;

	rpm_msg->err = r;

	if (r)
		dev_err(rpm_msg->rc->dev,
			"RPMH TX fail in msg addr 0x%x, err=%d\n",
			rpm_msg->msg.payload[0].addr, r);

	if (rpm_msg->free)
		kfree(rpm_msg->free);

	/* Signal the blocking thread we are done */
	if (wc && atomic_dec_and_test(wc))
		if (compl)
			complete(compl);
}

/**
 * wait_for_tx_done: Wait forever until the response is received.
 *
 * @rc: The RPMH client
 * @compl: The completion object
 * @addr: An addr that we sent in that request
 * @data: The data for the address in that request
 *
 */
static inline void wait_for_tx_done(struct rpmh_client *rc,
		struct completion *compl, u32 addr, u32 data)
{
	int ret;
	int count = 4; /* arbitary wait value */

	do {
		ret = wait_for_completion_timeout(compl, RPMH_TIMEOUT);
		if (ret) {
			if (count != 4)
				dev_notice(rc->dev,
				"RPMH response received addr=0x%x data=0x%x\n",
				addr, data);
			return;
		}
		if (count) {
			dev_err(rc->dev,
			"RPMH response timeout (%d) addr=0x%x,data=0x%x\n",
			count, addr, data);
			count--;
		}
	} while (true);
}

static struct rpmh_req *__find_req(struct rpmh_client *rc, u32 addr)
{
	struct rpmh_req *p, *req = NULL;

	list_for_each_entry(p, &rc->rpmh->resources, list) {
		if (p->addr == addr) {
			req = p;
			break;
		}
	}

	return req;
}

static struct rpmh_req *cache_rpm_request(struct rpmh_client *rc,
			enum rpmh_state state, struct tcs_cmd *cmd)
{
	struct rpmh_req *req;
	struct rpmh_mbox *rpm = rc->rpmh;
	unsigned long flags;

	spin_lock_irqsave(&rpm->lock, flags);
	req = __find_req(rc, cmd->addr);
	if (req)
		goto existing;

	req = kzalloc(sizeof(*req), GFP_ATOMIC);
	if (!req) {
		req = ERR_PTR(-ENOMEM);
		goto unlock;
	}

	req->addr = cmd->addr;
	req->sleep_val = req->wake_val = UINT_MAX;
	INIT_LIST_HEAD(&req->list);
	list_add_tail(&req->list, &rpm->resources);

existing:
	switch (state) {
	case RPMH_ACTIVE_ONLY_STATE:
		if (req->sleep_val != UINT_MAX)
			req->wake_val = cmd->data;
		break;
	case RPMH_WAKE_ONLY_STATE:
		req->wake_val = cmd->data;
		break;
	case RPMH_SLEEP_STATE:
		req->sleep_val = cmd->data;
		break;
	default:
		break;
	};

unlock:
	rpm->dirty = true;
	spin_unlock_irqrestore(&rpm->lock, flags);

	return req;
}

/**
 * __rpmh_write: Cache and send the RPMH request
 *
 * @rc: The RPMH client
 * @state: Active/Sleep request type
 * @rpm_msg: The data that needs to be sent (payload).
 *
 * Cache the RPMH request and send if the state is ACTIVE_ONLY.
 * SLEEP/WAKE_ONLY requests are not sent to the controller at
 * this time. Use rpmh_flush() to send them to the controller.
 */
int __rpmh_write(struct rpmh_client *rc, enum rpmh_state state,
			struct rpmh_msg *rpm_msg)
{
	struct rpmh_req *req;
	int ret = 0;
	int i;

	/* Cache the request in our store and link the payload */
	for (i = 0; i < rpm_msg->msg.num_payload; i++) {
		req = cache_rpm_request(rc, state, &rpm_msg->msg.payload[i]);
		if (IS_ERR(req))
			return PTR_ERR(req);
	}

	rpm_msg->msg.state = state;

	/* Send to mailbox only if active */
	if (state == RPMH_ACTIVE_ONLY_STATE) {
		ret = mbox_send_message(rc->chan, &rpm_msg->msg);
		if (ret > 0)
			ret = 0;
	} else {
		/* Clean up our call by spoofing tx_done */
		rpmh_tx_done(&rc->client, &rpm_msg->msg, ret);
	}

	return ret;
}

struct rpmh_msg *__get_rpmh_msg_async(struct rpmh_client *rc,
		enum rpmh_state state, struct tcs_cmd *cmd, int n)
{
	struct rpmh_msg *rpm_msg;

	if (IS_ERR_OR_NULL(rc) || !cmd || n <= 0 || n > MAX_RPMH_PAYLOAD)
		return ERR_PTR(-EINVAL);

	rpm_msg = kcalloc(1, sizeof(*rpm_msg), GFP_ATOMIC);
	if (!rpm_msg)
		return ERR_PTR(-ENOMEM);

	memcpy(rpm_msg->cmd, cmd, n * sizeof(*cmd));

	rpm_msg->msg.state = state;
	rpm_msg->msg.payload = rpm_msg->cmd;
	rpm_msg->msg.num_payload = n;
	rpm_msg->free = rpm_msg;

	return rpm_msg;
}

/**
 * rpmh_write_async: Write a set of RPMH commands
 *
 * @rc: The RPMh handle got from rpmh_get_dev_channel
 * @state: Active/sleep set
 * @cmd: The payload data
 * @n: The number of elements in payload
 *
 * Write a set of RPMH commands, the order of commands is maintained
 * and will be sent as a single shot.
 */
int rpmh_write_async(struct rpmh_client *rc, enum rpmh_state state,
			struct tcs_cmd *cmd, int n)
{
	struct rpmh_msg *rpm_msg;

	rpm_msg = __get_rpmh_msg_async(rc, state, cmd, n);
	if (IS_ERR(rpm_msg))
		return PTR_ERR(rpm_msg);

	return __rpmh_write(rc, state, rpm_msg);
}
EXPORT_SYMBOL(rpmh_write_async);

/**
 * rpmh_write: Write a set of RPMH commands and block until response
 *
 * @rc: The RPMh handle got from rpmh_get_dev_channel
 * @state: Active/sleep set
 * @cmd: The payload data
 * @n: The number of elements in payload
 *
 * May sleep. Do not call from atomic contexts.
 */
int rpmh_write(struct rpmh_client *rc, enum rpmh_state state,
			struct tcs_cmd *cmd, int n)
{
	DECLARE_COMPLETION_ONSTACK(compl);
	atomic_t wait_count = ATOMIC_INIT(1);
	DEFINE_RPMH_MSG_ONSTACK(rc, state, &compl, &wait_count, rpm_msg);
	int ret;

	if (IS_ERR_OR_NULL(rc) || !cmd || n <= 0 || n > MAX_RPMH_PAYLOAD)
		return -EINVAL;

	might_sleep();

	memcpy(rpm_msg.cmd, cmd, n * sizeof(*cmd));
	rpm_msg.msg.num_payload = n;

	ret = __rpmh_write(rc, state, &rpm_msg);
	if (ret)
		return ret;

	wait_for_tx_done(rc, &compl, cmd[0].addr, cmd[0].data);

	return rpm_msg.err;
}
EXPORT_SYMBOL(rpmh_write);

static inline int is_req_valid(struct rpmh_req *req)
{
	return (req->sleep_val != UINT_MAX && req->wake_val != UINT_MAX
			&& req->sleep_val != req->wake_val);
}

int send_single(struct rpmh_client *rc, enum rpmh_state state, u32 addr,
				u32 data)
{
	DEFINE_RPMH_MSG_ONSTACK(rc, state, NULL, NULL, rpm_msg);

	/* Wake sets are always complete and sleep sets are not */
	rpm_msg.msg.is_complete = (state == RPMH_WAKE_ONLY_STATE);
	rpm_msg.cmd[0].addr = addr;
	rpm_msg.cmd[0].data = data;
	rpm_msg.msg.num_payload = 1;
	rpm_msg.msg.is_complete = false;

	return mbox_write_controller_data(rc->chan, &rpm_msg.msg);
}

/**
 * rpmh_flush: Flushes the buffered active and sleep sets to TCS
 *
 * @rc: The RPMh handle got from rpmh_get_dev_channel
 *
 * This function is generally called from the sleep code from the last CPU
 * that is powering down the entire system.
 *
 * Returns -EBUSY if the controller is busy, probably waiting on a response
 * to a RPMH request sent earlier.
 */
int rpmh_flush(struct rpmh_client *rc)
{
	struct rpmh_req *p;
	struct rpmh_mbox *rpm = rc->rpmh;
	int ret;
	unsigned long flags;

	if (IS_ERR_OR_NULL(rc))
		return -EINVAL;

	spin_lock_irqsave(&rpm->lock, flags);
	if (!rpm->dirty) {
		pr_debug("Skipping flush, TCS has latest data.\n");
		spin_unlock_irqrestore(&rpm->lock, flags);
		return 0;
	}
	spin_unlock_irqrestore(&rpm->lock, flags);

	/*
	 * Nobody else should be calling this function other than system PM,,
	 * hence we can run without locks.
	 */
	list_for_each_entry(p, &rc->rpmh->resources, list) {
		if (!is_req_valid(p)) {
			pr_debug("%s: skipping RPMH req: a:0x%x s:0x%x w:0x%x",
				__func__, p->addr, p->sleep_val, p->wake_val);
			continue;
		}
		ret = send_single(rc, RPMH_SLEEP_STATE, p->addr, p->sleep_val);
		if (ret)
			return ret;
		ret = send_single(rc, RPMH_WAKE_ONLY_STATE, p->addr,
						p->wake_val);
		if (ret)
			return ret;
	}

	spin_lock_irqsave(&rpm->lock, flags);
	rpm->dirty = false;
	spin_unlock_irqrestore(&rpm->lock, flags);

	return 0;
}
EXPORT_SYMBOL(rpmh_flush);

/**
 * rpmh_invalidate: Invalidate all sleep and active sets
 * sets.
 *
 * @rc: The RPMh handle got from rpmh_get_dev_channel
 *
 * Invalidate the sleep and active values in the TCS blocks.
 * Nothing to do here.
 */
int rpmh_invalidate(struct rpmh_client *rc)
{
	DEFINE_RPMH_MSG_ONSTACK(rc, 0, NULL, NULL, rpm_msg);
	struct rpmh_mbox *rpm;
	unsigned long flags;

	if (IS_ERR_OR_NULL(rc))
		return -EINVAL;

	rpm = rc->rpmh;
	rpm_msg.msg.invalidate = true;
	rpm_msg.msg.is_complete = false;

	spin_lock_irqsave(&rpm->lock, flags);
	rpm->dirty = true;
	spin_unlock_irqrestore(&rpm->lock, flags);

	return mbox_write_controller_data(rc->chan, &rpm_msg.msg);
}
EXPORT_SYMBOL(rpmh_invalidate);

/**
 * get_mbox: Get the MBOX controller
 * @pdev: the platform device
 * @name: the MBOX name as specified in DT for the device.
 * @index: the index in the mboxes property if name is not provided.
 *
 * Get the MBOX Device node. We will use that to know which
 * MBOX controller this platform device is intending to talk
 * to.
 */
static struct rpmh_mbox *get_mbox(struct platform_device *pdev,
			const char *name, int index)
{
	int i;
	struct property *prop;
	struct of_phandle_args spec;
	const char *mbox_name;
	struct rpmh_mbox *rpmh;

	if (index < 0) {
		if (!name || !name[0])
			return ERR_PTR(-EINVAL);
		index = 0;
		of_property_for_each_string(pdev->dev.of_node,
				"mbox-names", prop, mbox_name) {
			if (!strcmp(name, mbox_name))
				break;
			index++;
		}
	}

	if (of_parse_phandle_with_args(pdev->dev.of_node, "mboxes",
					"#mbox-cells", index, &spec)) {
		dev_dbg(&pdev->dev, "%s: can't parse mboxes property\n",
					__func__);
		return ERR_PTR(-ENODEV);
	}

	for (i = 0; i < RPMH_MAX_MBOXES; i++)
		if (mbox_ctrlr[i].mbox_dn == spec.np) {
			rpmh = &mbox_ctrlr[i];
			goto found;
		}

	/* A new MBOX */
	for (i = 0; i < RPMH_MAX_MBOXES; i++)
		if (!mbox_ctrlr[i].mbox_dn)
			break;

	/* More controllers than expected - not recoverable */
	WARN_ON(i == RPMH_MAX_MBOXES);

	rpmh = &mbox_ctrlr[i];

	rpmh->mbox_dn = spec.np;
	INIT_LIST_HEAD(&rpmh->resources);
	spin_lock_init(&rpmh->lock);

found:
	of_node_put(spec.np);

	return rpmh;
}

static struct rpmh_client *get_rpmh_client(struct platform_device *pdev,
				const char *name, int index)
{
	struct rpmh_client *rc;
	int ret = 0;

	rc = kzalloc(sizeof(*rc), GFP_KERNEL);
	if (!rc)
		return ERR_PTR(-ENOMEM);

	rc->client.tx_prepare = NULL;
	rc->client.tx_done = rpmh_tx_done;
	rc->client.tx_block = false;
	rc->client.knows_txdone = false;
	rc->client.dev = &pdev->dev;
	rc->dev = &pdev->dev;

	rc->chan = ERR_PTR(-EINVAL);

	/* Initialize by index or name, whichever is present */
	if (index >= 0)
		rc->chan = mbox_request_channel(&rc->client, index);
	else if (name)
		rc->chan = mbox_request_channel_byname(&rc->client, name);

	if (IS_ERR_OR_NULL(rc->chan)) {
		ret = PTR_ERR(rc->chan);
		goto cleanup;
	}

	mutex_lock(&rpmh_mbox_mutex);
	rc->rpmh = get_mbox(pdev, name, index);
	mutex_unlock(&rpmh_mbox_mutex);

	if (IS_ERR(rc->rpmh)) {
		ret = PTR_ERR(rc->rpmh);
		mbox_free_channel(rc->chan);
		goto cleanup;
	}

	return rc;

cleanup:
	kfree(rc);
	return ERR_PTR(ret);
}

/**
 * rpmh_get_byname: Get the RPMh handle by mbox name
 *
 * @pdev: the platform device which needs to communicate with RPM
 * accelerators
 * @name: The mbox-name assigned to the client's mailbox handle
 *
 * May sleep.
 */
struct rpmh_client *rpmh_get_byname(struct platform_device *pdev,
				const char *name)
{
	return get_rpmh_client(pdev, name, -1);
}
EXPORT_SYMBOL(rpmh_get_byname);

/**
 * rpmh_get_byindex: Get the RPMh handle by mbox index
 *
 * @pdev: the platform device which needs to communicate with RPM
 * accelerators
 * @index : The index of the mbox tuple as specified in order in DT
 *
 * May sleep.
 */
struct rpmh_client *rpmh_get_byindex(struct platform_device *pdev,
				int index)
{
	return get_rpmh_client(pdev, NULL, index);
}
EXPORT_SYMBOL(rpmh_get_byindex);

/**
 * rpmh_release: Release the RPMH client
 *
 * @rc: The RPMh handle to be freed.
 */
void rpmh_release(struct rpmh_client *rc)
{
	if (rc && !IS_ERR_OR_NULL(rc->chan))
		mbox_free_channel(rc->chan);

	kfree(rc);
}
EXPORT_SYMBOL(rpmh_release);
