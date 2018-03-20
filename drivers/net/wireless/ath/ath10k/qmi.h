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
#ifndef _QMI_H_
#define _QMI_H_

#define MAX_NUM_MEMORY_REGIONS			2

enum ath10k_qmi_driver_event_type {
	ATH10K_QMI_EVENT_SERVER_ARRIVE,
	ATH10K_QMI_EVENT_SERVER_EXIT,
	ATH10K_QMI_EVENT_FW_READY_IND,
	ATH10K_QMI_EVENT_MAX,
};

struct ath10k_msa_mem_region_info {
	u64 reg_addr;
	u32 size;
	u8 secure_flag;
};

struct ath10k_qmi {
	struct platform_device *pdev;
	struct qmi_handle qmi_hdl;
	struct sockaddr_qrtr sq;
	bool fw_ready;
	bool msa_ready;
	struct work_struct work_svc_arrive;
	struct work_struct work_svc_exit;
	struct workqueue_struct *event_wq;
	spinlock_t event_lock; /* spinlock for fw ready status*/
	u32 nr_mem_region;
	struct ath10k_msa_mem_region_info
		mem_region[MAX_NUM_MEMORY_REGIONS];
	phys_addr_t msa_pa;
	u32 msa_mem_size;
	void *msa_va;
};
#endif /* _QMI_H_ */
