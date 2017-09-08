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
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
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
};

struct rpmh_mbox {
	struct device_node *mbox_dn;
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

/**
 * __rpmh_write: send the RPMH request
 *
 * @rc: The RPMH client
 * @state: Active/Sleep request type
 * @rpm_msg: The data that needs to be sent (payload).
 */
int __rpmh_write(struct rpmh_client *rc, enum rpmh_state state,
			struct rpmh_msg *rpm_msg)
{
	int ret = 0;

	rpm_msg->msg.state = state;

	if (state == RPMH_ACTIVE_ONLY_STATE) {
		ret = mbox_send_message(rc->chan, &rpm_msg->msg);
	} else {
		ret = mbox_write_controller_data(rc->chan, &rpm_msg->msg);
		/* Clean up our call by spoofing tx_done */
		rpmh_tx_done(&rc->client, &rpm_msg->msg, ret);
	}

	return (ret < 0) ? ret : 0;
}

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
