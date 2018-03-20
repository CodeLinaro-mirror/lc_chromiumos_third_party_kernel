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
#define MAX_TIMESTAMP_LEN			32
#define MAX_BUILD_ID_LEN			128
#define BDF_FILE_NAME			"bdwlan.bin"
#define MAX_NUM_CAL_V01			5

enum ath10k_qmi_driver_event_type {
	ATH10K_QMI_EVENT_SERVER_ARRIVE,
	ATH10K_QMI_EVENT_SERVER_EXIT,
	ATH10K_QMI_EVENT_FW_READY_IND,
	ATH10K_QMI_EVENT_MAX,
};

enum ath10k_qmi_driver_mode {
	QMI_MISSION,
	QMI_FTM,
	QMI_EPPING,
	QMI_OFF,
};

struct ath10k_msa_mem_region_info {
	u64 reg_addr;
	u32 size;
	u8 secure_flag;
};

struct ath10k_qmi_chip_info {
	u32 chip_id;
	u32 chip_family;
};

struct ath10k_qmi_board_info {
	u32 board_id;
};

struct ath10k_qmi_soc_info {
	u32 soc_id;
};

struct ath10k_qmi_fw_version_info {
	u32 fw_version;
	char fw_build_timestamp[MAX_TIMESTAMP_LEN + 1];
};

struct ath10k_qmi_cal_data {
	u32 cal_id;
	u32 total_size;
	u8 *data;
};

struct ath10k_tgt_pipe_cfg {
	u32 pipe_num;
	u32 pipe_dir;
	u32 nentries;
	u32 nbytes_max;
	u32 flags;
	u32 reserved;
};

struct ath10k_svc_pipe_cfg {
	u32 service_id;
	u32 pipe_dir;
	u32 pipe_num;
};

struct ath10k_shadow_reg_cfg {
	u16 ce_id;
	u16 reg_offset;
};

struct ath10k_qmi_wlan_enable_cfg {
	u32 num_ce_tgt_cfg;
	struct ath10k_tgt_pipe_cfg *ce_tgt_cfg;
	u32 num_ce_svc_pipe_cfg;
	struct ath10k_svc_pipe_cfg *ce_svc_cfg;
	u32 num_shadow_reg_cfg;
	struct ath10k_shadow_reg_cfg *shadow_reg_cfg;
};

int ath10k_qmi_wlan_enable(struct ath10k_qmi_wlan_enable_cfg *config,
			   enum ath10k_qmi_driver_mode mode,
			   const char *host_version);
int ath10k_qmi_wlan_disable(void);
#endif /* _QMI_H_ */
