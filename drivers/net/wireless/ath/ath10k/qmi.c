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

static int ath10k_qmi_event_fw_ready_ind(struct ath10k_qmi *qmi)
{
	pr_debug("fw ready event received\n");
	spin_lock(&qmi->event_lock);
	qmi->fw_ready = true;
	spin_unlock(&qmi->event_lock);

	return 0;
}

static void ath10k_qmi_fw_ready_ind(struct qmi_handle *qmi_hdl,
				    struct sockaddr_qrtr *sq,
				    struct qmi_txn *txn, const void *data)
{
	struct ath10k_qmi *qmi = container_of(qmi_hdl, struct ath10k_qmi, qmi_hdl);

	ath10k_qmi_event_fw_ready_ind(qmi);
}

static void ath10k_qmi_msa_ready_ind(struct qmi_handle *qmi_hdl,
				     struct sockaddr_qrtr *sq,
				     struct qmi_txn *txn, const void *data)
{
	struct ath10k_qmi *qmi = container_of(qmi_hdl, struct ath10k_qmi, qmi_hdl);

	qmi->msa_ready = true;
}

static struct qmi_msg_handler qmi_msg_handler[] = {
	{
		.type = QMI_INDICATION,
		.msg_id = QMI_WLFW_FW_READY_IND_V01,
		.ei = wlfw_fw_ready_ind_msg_v01_ei,
		.decoded_size = sizeof(struct wlfw_fw_ready_ind_msg_v01),
		.fn = ath10k_qmi_fw_ready_ind,
	},
	{
		.type = QMI_INDICATION,
		.msg_id = QMI_WLFW_MSA_READY_IND_V01,
		.ei = wlfw_msa_ready_ind_msg_v01_ei,
		.decoded_size = sizeof(struct wlfw_msa_ready_ind_msg_v01),
		.fn = ath10k_qmi_msa_ready_ind,
	},
	{}
};

static int ath10k_qmi_connect_to_fw_server(struct ath10k_qmi *qmi)
{
	struct qmi_handle *qmi_hdl = &qmi->qmi_hdl;
	int ret;

	ret = kernel_connect(qmi_hdl->sock, (struct sockaddr *)&qmi->sq,
			     sizeof(qmi->sq), 0);
	if (ret) {
		pr_err("fail to connect to remote service port\n");
		return ret;
	}

	pr_info("wlan qmi service connected\n");

	return 0;
}

static void ath10k_qmi_event_server_arrive(struct work_struct *work)
{
	struct ath10k_qmi *qmi = container_of(work, struct ath10k_qmi,
					      work_svc_arrive);
	int ret;

	ret = ath10k_qmi_connect_to_fw_server(qmi);
	if (ret)
		return;

	pr_debug("qmi server arrive\n");
}

static void ath10k_qmi_event_server_exit(struct work_struct *work)
{
	struct ath10k_qmi *qmi = container_of(work, struct ath10k_qmi,
					      work_svc_exit);

	spin_lock(&qmi->event_lock);
	qmi->fw_ready = false;
	spin_unlock(&qmi->event_lock);
	pr_info("wlan fw service disconnected\n");
}

static int ath10k_qmi_new_server(struct qmi_handle *qmi_hdl,
				 struct qmi_service *service)
{
	struct ath10k_qmi *qmi = container_of(qmi_hdl, struct ath10k_qmi, qmi_hdl);
	struct sockaddr_qrtr *sq = &qmi->sq;

	sq->sq_family = AF_QIPCRTR;
	sq->sq_node = service->node;
	sq->sq_port = service->port;

	queue_work(qmi->event_wq, &qmi->work_svc_arrive);

	return 0;
}

static void ath10k_qmi_del_server(struct qmi_handle *qmi_hdl,
				  struct qmi_service *service)
{
	struct ath10k_qmi *qmi =
		container_of(qmi_hdl, struct ath10k_qmi, qmi_hdl);

	queue_work(qmi->event_wq, &qmi->work_svc_exit);
}

static struct qmi_ops ath10k_qmi_ops = {
	.new_server = ath10k_qmi_new_server,
	.del_server = ath10k_qmi_del_server,
};

static int ath10k_alloc_qmi_resources(struct ath10k_qmi *qmi)
{
	int ret;

	ret = qmi_handle_init(&qmi->qmi_hdl,
			      WLFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_MSG_LEN,
			      &ath10k_qmi_ops, qmi_msg_handler);
	if (ret)
		goto err;

	qmi->event_wq = alloc_workqueue("qmi_driver_event",
					WQ_UNBOUND, 1);
	if (!qmi->event_wq) {
		pr_err("workqueue alloc failed\n");
		ret = -EFAULT;
		goto err_qmi_service;
	}

	spin_lock_init(&qmi->event_lock);
	INIT_WORK(&qmi->work_svc_arrive, ath10k_qmi_event_server_arrive);
	INIT_WORK(&qmi->work_svc_exit, ath10k_qmi_event_server_exit);

	ret = qmi_add_lookup(&qmi->qmi_hdl, WLFW_SERVICE_ID_V01,
			     WLFW_SERVICE_VERS_V01, 0);
	if (ret)
		goto err_qmi_service;

	return 0;

err_qmi_service:
	qmi_handle_release(&qmi->qmi_hdl);

err:
	return ret;
}

static void ath10k_remove_qmi_resources(struct ath10k_qmi *qmi)
{
	cancel_work_sync(&qmi->work_svc_arrive);
	cancel_work_sync(&qmi->work_svc_exit);
	destroy_workqueue(qmi->event_wq);
	qmi_handle_release(&qmi->qmi_hdl);
	qmi = NULL;
}

static int ath10k_qmi_probe(struct platform_device *pdev)
{
	int ret;

	qmi = devm_kzalloc(&pdev->dev, sizeof(*qmi),
			   GFP_KERNEL);
	if (!qmi)
		return -ENOMEM;

	qmi->pdev = pdev;
	platform_set_drvdata(pdev, qmi);

	ret = ath10k_alloc_qmi_resources(qmi);
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

	ath10k_remove_qmi_resources(qmi);

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
