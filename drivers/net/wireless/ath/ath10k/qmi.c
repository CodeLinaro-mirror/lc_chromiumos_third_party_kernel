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
#include <linux/qcom_scm.h>
#include <linux/of.h>
#include "qmi.h"
#include "qmi_svc_v01.h"

#define WLFW_CLIENT_ID			0x4b4e454c
#define WLFW_TIMEOUT			500

static struct ath10k_qmi *qmi;

static int
ath10k_qmi_map_msa_permissions(struct ath10k_msa_mem_region_info *mem_region)
{
	struct qcom_scm_vmperm dst_perms[3];
	unsigned int src_perms;
	phys_addr_t addr;
	u32 perm_count;
	u32 size;
	int ret;

	addr = mem_region->reg_addr;
	size = mem_region->size;

	src_perms = BIT(QCOM_SCM_VMID_HLOS);

	dst_perms[0].vmid = QCOM_SCM_VMID_MSS_MSA;
	dst_perms[0].perm = QCOM_SCM_PERM_RW;
	dst_perms[1].vmid = QCOM_SCM_VMID_WLAN;
	dst_perms[1].perm = QCOM_SCM_PERM_RW;

	if (!mem_region->secure_flag) {
		dst_perms[2].vmid = QCOM_SCM_VMID_WLAN_CE;
		dst_perms[2].perm = QCOM_SCM_PERM_RW;
		perm_count = 3;
	} else {
		dst_perms[2].vmid = 0;
		dst_perms[2].perm = 0;
		perm_count = 2;
	}

	ret = qcom_scm_assign_mem(addr, size, &src_perms,
				  dst_perms, perm_count);
	if (ret < 0)
		pr_err("msa map permission failed=%d\n", ret);

	return ret;
}

static int
ath10k_qmi_unmap_msa_permissions(struct ath10k_msa_mem_region_info *mem_region)
{
	struct qcom_scm_vmperm dst_perms;
	unsigned int src_perms;
	phys_addr_t addr;
	u32 size;
	int ret;

	addr = mem_region->reg_addr;
	size = mem_region->size;

	src_perms = BIT(QCOM_SCM_VMID_MSS_MSA) | BIT(QCOM_SCM_VMID_WLAN);

	if (!mem_region->secure_flag)
		src_perms |= BIT(QCOM_SCM_VMID_WLAN_CE);

	dst_perms.vmid = QCOM_SCM_VMID_HLOS;
	dst_perms.perm = QCOM_SCM_PERM_RW;

	ret = qcom_scm_assign_mem(addr, size, &src_perms, &dst_perms, 1);
	if (ret < 0)
		pr_err("msa unmap permission failed=%d\n", ret);

	return ret;
}

static int ath10k_qmi_setup_msa_permissions(struct ath10k_qmi *qmi)
{
	int ret;
	int i;

	for (i = 0; i < qmi->nr_mem_region; i++) {
		ret = ath10k_qmi_map_msa_permissions(&qmi->mem_region[i]);
		if (ret)
			goto err_unmap;
	}

	return 0;

err_unmap:
	for (i--; i >= 0; i--)
		ath10k_qmi_unmap_msa_permissions(&qmi->mem_region[i]);
	return ret;
}

static void ath10k_qmi_remove_msa_permissions(struct ath10k_qmi *qmi)
{
	int i;

	for (i = 0; i < qmi->nr_mem_region; i++)
		ath10k_qmi_unmap_msa_permissions(&qmi->mem_region[i]);
}

static int
	ath10k_qmi_msa_mem_info_send_sync_msg(struct ath10k_qmi *qmi)
{
	struct wlfw_msa_info_resp_msg_v01 *resp;
	struct wlfw_msa_info_req_msg_v01 *req;
	struct qmi_txn txn;
	int ret;
	int i;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp) {
		kfree(req);
		return -ENOMEM;
	}

	req->msa_addr = qmi->msa_pa;
	req->size = qmi->msa_mem_size;

	ret = qmi_txn_init(&qmi->qmi_hdl, &txn,
			   wlfw_msa_info_resp_msg_v01_ei, resp);
	if (ret < 0) {
		pr_err("fail to init txn for MSA mem info resp %d\n",
		       ret);
		goto out;
	}

	ret = qmi_send_request(&qmi->qmi_hdl, NULL, &txn,
			       QMI_WLFW_MSA_INFO_REQ_V01,
			       WLFW_MSA_INFO_REQ_MSG_V01_MAX_MSG_LEN,
			       wlfw_msa_info_req_msg_v01_ei, req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		pr_err("fail to send MSA mem info req %d\n", ret);
		goto out;
	}

	ret = qmi_txn_wait(&txn, WLFW_TIMEOUT * HZ);
	if (ret < 0)
		goto out;

	if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		pr_err("MSA mem info request rejected, result:%d error:%d\n",
		       resp->resp.result, resp->resp.error);
		ret = -resp->resp.result;
		goto out;
	}

	pr_debug("receive mem_region_info_len: %d\n",
		 resp->mem_region_info_len);

	if (resp->mem_region_info_len > QMI_WLFW_MAX_NUM_MEMORY_REGIONS_V01) {
		pr_err("invalid memory region length received: %d\n",
		       resp->mem_region_info_len);
		ret = -EINVAL;
		goto out;
	}

	qmi->nr_mem_region = resp->mem_region_info_len;
	for (i = 0; i < resp->mem_region_info_len; i++) {
		qmi->mem_region[i].reg_addr =
			resp->mem_region_info[i].region_addr;
		qmi->mem_region[i].size =
			resp->mem_region_info[i].size;
		qmi->mem_region[i].secure_flag =
			resp->mem_region_info[i].secure_flag;
		pr_debug("mem region: %d Addr: 0x%llx Size: 0x%x Flag: 0x%08x\n",
			 i, qmi->mem_region[i].reg_addr,
			 qmi->mem_region[i].size,
			 qmi->mem_region[i].secure_flag);
	}

	pr_debug("MSA mem info request completed\n");
	kfree(resp);
	kfree(req);
	return 0;

out:
	kfree(resp);
	kfree(req);
	return ret;
}

static int ath10k_qmi_msa_ready_send_sync_msg(struct ath10k_qmi *qmi)
{
	struct wlfw_msa_ready_resp_msg_v01 *resp;
	struct wlfw_msa_ready_req_msg_v01 *req;
	struct qmi_txn txn;
	int ret;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp) {
		kfree(req);
		return -ENOMEM;
	}

	ret = qmi_txn_init(&qmi->qmi_hdl, &txn,
			   wlfw_msa_ready_resp_msg_v01_ei, resp);
	if (ret < 0) {
		pr_err("fail to init txn for MSA mem ready resp %d\n",
		       ret);
		goto out;
	}

	ret = qmi_send_request(&qmi->qmi_hdl, NULL, &txn,
			       QMI_WLFW_MSA_READY_REQ_V01,
			       WLFW_MSA_READY_REQ_MSG_V01_MAX_MSG_LEN,
			       wlfw_msa_ready_req_msg_v01_ei, req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		pr_err("fail to send MSA mem ready req %d\n", ret);
		goto out;
	}

	ret = qmi_txn_wait(&txn, WLFW_TIMEOUT * HZ);
	if (ret < 0)
		goto out;

	if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		pr_err("qmi MSA mem ready request rejected, result:%d error:%d\n",
		       resp->resp.result, resp->resp.error);
		ret = -resp->resp.result;
	}

	pr_debug("MSA mem ready request completed\n");
	kfree(resp);
	kfree(req);
	return 0;

out:
	kfree(resp);
	kfree(req);
	return ret;
}

static int
ath10k_qmi_ind_register_send_sync_msg(struct ath10k_qmi *qmi)
{
	struct wlfw_ind_register_resp_msg_v01 *resp;
	struct wlfw_ind_register_req_msg_v01 *req;
	struct qmi_txn txn;
	int ret;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp) {
		kfree(req);
		return -ENOMEM;
	}

	req->client_id_valid = 1;
	req->client_id = WLFW_CLIENT_ID;
	req->fw_ready_enable_valid = 1;
	req->fw_ready_enable = 1;
	req->msa_ready_enable_valid = 1;
	req->msa_ready_enable = 1;

	ret = qmi_txn_init(&qmi->qmi_hdl, &txn,
			   wlfw_ind_register_resp_msg_v01_ei, resp);
	if (ret < 0) {
		pr_err("fail to init txn for ind register resp %d\n",
		       ret);
		goto out;
	}

	ret = qmi_send_request(&qmi->qmi_hdl, NULL, &txn,
			       QMI_WLFW_IND_REGISTER_REQ_V01,
			       WLFW_IND_REGISTER_REQ_MSG_V01_MAX_MSG_LEN,
			       wlfw_ind_register_req_msg_v01_ei, req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		pr_err("fail to send ind register req %d\n", ret);
		goto out;
	}

	ret = qmi_txn_wait(&txn, WLFW_TIMEOUT * HZ);
	if (ret < 0)
		goto out;

	if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		pr_err("qmi indication register request rejected:");
		pr_err("resut:%d error:%d\n",
		       resp->resp.result, resp->resp.error);
		ret = resp->resp.result;
	}

	pr_debug("indication register request completed\n");
	kfree(resp);
	kfree(req);
	return 0;

out:
	kfree(resp);
	kfree(req);
	return ret;
}

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

	pr_debug("wlan qmi server arrive\n");
	ret = ath10k_qmi_connect_to_fw_server(qmi);
	if (ret)
		return;

	ret = ath10k_qmi_ind_register_send_sync_msg(qmi);
	if (ret)
		return;

	ret = ath10k_qmi_msa_mem_info_send_sync_msg(qmi);
	if (ret)
		return;

	ret = ath10k_qmi_setup_msa_permissions(qmi);
	if (ret)
		return;

	ret = ath10k_qmi_msa_ready_send_sync_msg(qmi);
	if (ret)
		goto err_setup_msa;

	return;

err_setup_msa:
	ath10k_qmi_remove_msa_permissions(qmi);
	return;
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

static int ath10k_qmi_setup_msa_resources(struct device *dev,
					  struct ath10k_qmi *qmi)
{
	int ret;

	ret = of_property_read_u32(dev->of_node, "qcom,wlan-msa-memory",
				   &qmi->msa_mem_size);

	if (ret || qmi->msa_mem_size == 0) {
		pr_err("fail to get MSA memory size: %u, ret: %d\n",
		       qmi->msa_mem_size, ret);
		return -ENOMEM;
	}

	qmi->msa_va = dmam_alloc_coherent(dev, qmi->msa_mem_size,
					  &qmi->msa_pa, GFP_KERNEL);
	if (!qmi->msa_va) {
		pr_err("dma alloc failed for MSA\n");
		return -ENOMEM;
	}

	pr_debug("MSA pa: %pa, MSA va: 0x%p\n",
		 &qmi->msa_pa,
		 qmi->msa_va);

	return 0;
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
	ath10k_qmi_setup_msa_resources(&pdev->dev, qmi);
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
