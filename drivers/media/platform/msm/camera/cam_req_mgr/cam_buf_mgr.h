/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef _CAM_BUF_MGR_H_
#define _CAM_BUF_MGR_H_

#include <linux/types.h>
#include <linux/platform_device.h>

struct dma_buf *cbm_alloc_buffer(size_t len, unsigned int flags);

void cbm_free_buffer(struct dma_buf *dmabuf);

int cam_buf_mgr_init(struct platform_device *pdev);

void cam_buf_mgr_exit(struct platform_device *pdev);

#endif /* _CAM_BUF_MGR_H_ */
