/*
 * Copyright (c) 2018 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/qrtr.h>
#include <linux/net.h>
#include <linux/completion.h>
#include <linux/idr.h>
#include <linux/string.h>
#include <net/sock.h>
#include <linux/soc/qcom/qmi.h>
#include "qmi.h"
#include "qmi_svc_v01.h"

static struct ath10k_qmi *qmi;

static int ath10k_qmi_new_server(struct qmi_handle *qmi_hdl,
				 struct qmi_service *service)
{
	return 0;
}

static void ath10k_qmi_del_server(struct qmi_handle *qmi_hdl,
				  struct qmi_service *service)
{
}

static struct qmi_ops ath10k_qmi_ops = {
	.new_server = ath10k_qmi_new_server,
	.del_server = ath10k_qmi_del_server,
};

static int ath10k_qmi_probe(struct platform_device *pdev)
{
	int ret;

	qmi = devm_kzalloc(&pdev->dev, sizeof(*qmi),
			   GFP_KERNEL);
	if (!qmi)
		return -ENOMEM;

	qmi->pdev = pdev;
	platform_set_drvdata(pdev, qmi);
	ret = qmi_handle_init(&qmi->qmi_hdl,
			      WLFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_MSG_LEN,
			      &ath10k_qmi_ops, NULL);
	if (ret < 0)
		goto err;

	ret = qmi_add_lookup(&qmi->qmi_hdl, WLFW_SERVICE_ID_V01,
			     WLFW_SERVICE_VERS_V01, 0);
	if (ret < 0)
		goto err;

	pr_debug("qmi client driver probed successfully\n");

	return 0;

err:
	return ret;
}

static int ath10k_qmi_remove(struct platform_device *pdev)
{
	struct ath10k_qmi *qmi = platform_get_drvdata(pdev);

	qmi_handle_release(&qmi->qmi_hdl);

	return 0;
}

static const struct of_device_id ath10k_qmi_dt_match[] = {
	{.compatible = "qcom,ath10k-qmi"},
	{}
};

MODULE_DEVICE_TABLE(of, ath10k_qmi_dt_match);

static struct platform_driver ath10k_qmi_clinet = {
	.probe  = ath10k_qmi_probe,
	.remove = ath10k_qmi_remove,
	.driver = {
		.name = "ath10k QMI client",
		.owner = THIS_MODULE,
		.of_match_table = ath10k_qmi_dt_match,
	},
};

static int __init ath10k_qmi_init(void)
{
	return platform_driver_register(&ath10k_qmi_clinet);
}

static void __exit ath10k_qmi_exit(void)
{
	platform_driver_unregister(&ath10k_qmi_clinet);
}

module_init(ath10k_qmi_init);
module_exit(ath10k_qmi_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("ath10k QMI client driver");
