// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) 2020-2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/devcoredump.h>
#include <linux/dma-direction.h>
#include <linux/mm.h>
#include <linux/uuid.h>
#include <linux/time.h>
#include "core.h"
#include "coredump.h"
#include "debug.h"

static void ath11k_coredump_update_hdr(struct ath11k_base *ab,
				       struct ath11k_dump_file_data *file_data,
				       size_t header_size)
{
	struct timespec64 timestamp;

	strscpy(file_data->df_magic, "ATH11K-FW-DUMP",
		sizeof(file_data->df_magic));
	file_data->len = cpu_to_le32(header_size);
	file_data->version = cpu_to_le32(ATH11K_FW_CRASH_DUMP_VERSION);
	guid_gen(&file_data->guid);
	ktime_get_real_ts64(&timestamp);
	file_data->tv_sec = cpu_to_le64(timestamp.tv_sec);
	file_data->tv_nsec = cpu_to_le64(timestamp.tv_nsec);
}

void ath11k_coredump_msa(struct ath11k_base *ab,
			 struct ath11k_msa_dump *msa_data)
{
	struct ath11k_dump_segment *segment;
	struct ath11k_dump_file_data *file_data;
	size_t header_size;
	int ret;
	u8 *buf, *dump;

	segment = vzalloc(sizeof(*segment));
	if (!segment)
		return;

	header_size = sizeof(struct ath11k_dump_file_data);
	header_size += sizeof(*segment);
	header_size = PAGE_ALIGN(header_size);
	buf = vzalloc(header_size);
	if (!buf) {
		vfree(segment);
		return;
	}

	file_data = (struct ath11k_dump_file_data *)buf;

	ath11k_coredump_update_hdr(ab, file_data, header_size);

	file_data->num_seg = cpu_to_le32(1);
	file_data->seg_size = cpu_to_le32(sizeof(*segment));

	/* copy segment details to file */
	buf += offsetof(struct ath11k_dump_file_data, seg);
	file_data->seg = (struct ath11k_dump_segment *)buf;

	segment->addr = msa_data->paddr;
	segment->vaddr = msa_data->vaddr;
	segment->len = msa_data->size;
	segment->type = ATH11K_FW_MSA_DUMP_DATA;

	memcpy(file_data->seg, segment, sizeof(*segment));

	dump = vzalloc(header_size + segment->len);
	if (!dump) {
		ret = -ENOMEM;
		ath11k_err(ab, "failed to allocate memory for msa dump %d\n", ret);
		goto err_alloc_fail;
	}

	memcpy(dump, (void *)file_data, header_size);
	memcpy(dump + header_size, segment->vaddr, segment->len);

	dev_coredumpv(ab->dev, dump, header_size + segment->len,
		      GFP_KERNEL);
err_alloc_fail:
	vfree(file_data);
	vfree(segment);
}
EXPORT_SYMBOL(ath11k_coredump_msa);
