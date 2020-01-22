/*
 * Copyright 2014 Canonical
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Andreas Pokorny
 */

#include <drm/drm_prime.h>
#include <linux/dma-buf.h>
#include "virtgpu_drv.h"

int virtgpu_gem_prime_get_uuid(struct drm_gem_object *obj,
			       uuid_t *uuid)
{
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	struct virtio_gpu_device *vgdev = obj->dev->dev_private;

	wait_event(vgdev->resp_wq, bo->uuid_state != UUID_INITIALIZING);
	if (bo->uuid_state != UUID_INITIALIZED)
		return -ENODEV;

	uuid_copy(uuid, &bo->uuid);

	return 0;
}

struct dma_buf *virtgpu_gem_prime_export(struct drm_gem_object *obj,
					 int flags)
{
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	struct virtio_gpu_device *vgdev = obj->dev->dev_private;
	int ret = 0;

	if (vgdev->has_resource_assign_uuid) {
		ret = virtio_gpu_cmd_resource_assign_uuid(vgdev, bo);
		if (ret)
			return ERR_PTR(ret);
	} else {
		bo->uuid_state = UUID_INITIALIZATION_FAILED;
	}

	return drm_gem_prime_export(obj, flags);
}

struct drm_gem_object *virtgpu_gem_prime_import_sg_table(
	struct drm_device *dev, struct dma_buf_attachment *attach,
	struct sg_table *table)
{
	return ERR_PTR(-ENODEV);
}
