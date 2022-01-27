/* SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * Copyright (c) 2020-2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 *  
 */

#ifndef _COREDUMP_H_
#define _COREDUMP_H_

#define ATH11K_FW_CRASH_DUMP_VERSION 1

enum ath11k_fw_crash_dump_type {
	ATH11K_FW_MSA_DUMP_DATA,
	ATH11K_FW_CRASH_DUMP_MAX,
};

struct ath11k_dump_segment {
	unsigned long addr;
	u32 *vaddr;
	unsigned int len;
	unsigned int type;
};

struct ath11k_dump_file_data {
	/* "ATH11K-FW-DUMP" */
	char df_magic[16];
	__le32 len;
	/* file dump version */
	__le32 version;
	/* pci device id */
	__le32 chip_id;
	guid_t guid;
	/* time-of-day stamp */
	__le64 tv_sec;
	/* time-of-day stamp, nano-seconds */
	__le64 tv_nsec;
	/* room for growth w/out changing binary format */
	u8 unused[8];
	/* number of segments */
	__le32 num_seg;
	/* ath11k_dump_segment struct size */
	__le32 seg_size;

	struct ath11k_dump_segment *seg;
	/* struct ath11k_dump_segment + more */

	u8 data[];
} __packed;

struct ath11k_coredump_state {
	struct ath11k_dump_file_data *header;
	struct ath11k_dump_segment *segments;
	u32 num_seg;
};

struct ath11k_msa_dump {
	u64 paddr;
	u32 *vaddr;
	u64 size;
};

#ifdef CONFIG_DEV_COREDUMP
void ath11k_coredump_msa(struct ath11k_base *ab,
			 struct ath11k_msa_dump *msa_data);
#else
static inline void
ath11k_coredump_msa(struct ath11k_base *ab,
		    struct ath11k_msa_dump *msa_data)
{
}
#endif /* CONFIG_DEV_COREDUMP */
#endif /* _COREDUMP_H_ */
