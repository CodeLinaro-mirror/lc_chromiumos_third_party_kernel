// SPDX-License-Identifier: GPL-2.0-only
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

#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/dma-buf.h>
#include <media/videobuf2-dma-sg.h>

#if defined(CONFIG_ARCH_SM6150)
#include <linux/msm_dma_iommu_mapping.h>
#endif
#include <media/cam_req_mgr.h>

#include "cam_req_mgr_dev.h"
#include "cam_buf_mgr.h"

//#define USE_VIDEOBUF2_MEMOPS

struct cbuf_vma_list {
	struct list_head list;
	struct vm_area_struct *vma;
};

struct cbm_node {
	struct page *page;
	struct list_head list;
};

struct cbm_nodes {
	struct page **pages;
	u32 size;
};

struct cbm_dma_buf_attachment {
	struct device *dev;
	struct sg_table *table;
	struct list_head list;
	bool dma_mapped;
};

/**
 * struct cbm_context - camera buffer manager context
 *
 * @nodes:     an rb tree of all the existing nodes
 * @t_lock:    lock protecting the tree of nodes
 */
struct cbm_context {
	struct device *dev;
	struct rb_root nodes;
	struct mutex t_lock;	/* table lock */
	const struct vb2_mem_ops *vb2_ops;
};

/**
 * struct cbm_buffer - metadata for a buffer
 *
 * @node:     node in the CBM nodes tree
 * @flags:    buffer specific flags
 * @gfp_mask: page alocation mask
 * @size:     size of the buffer
 * @b_lock:   protects the camera buffer fields
 * @kmap_cnt: number of times the buffer is mapped to the kernel
 * @vaddr:    the kernel mapping if kmap_cnt is not zero
 * @sg_table: the sg table for the buffer
 * @attached: list of entry attachments
 * @vmas:     list of vma's mapping this buffer
 * @ctx:      reference to CBM context
 */
struct cbm_buffer {
	struct rb_node node;
	unsigned long flags;
	gfp_t gfp_mask;
	size_t size;
	struct mutex b_lock;	/* buffer lock */
	int kmap_cnt;
	void *vaddr;
	struct sg_table *sg_table;
	struct list_head attached;
	struct list_head vmas;
	struct cbm_context *ctx;
};

static struct cbm_context *idev;

#if !defined(USE_VIDEOBUF2_MEMOPS)

static void _cbm_vm_open(struct vm_area_struct *vma)
{
	struct cbm_buffer *cbuf = vma->vm_private_data;
	struct cbuf_vma_list *it;

	it = kmalloc(sizeof(*it), GFP_KERNEL);
	if (IS_ERR_OR_NULL(it))
		return;

	it->vma = vma;
	mutex_lock(&cbuf->b_lock);
	list_add(&it->list, &cbuf->vmas);
	mutex_unlock(&cbuf->b_lock);
}

static void _cbm_vm_close(struct vm_area_struct *vma)
{
	struct cbm_buffer *cbuf = vma->vm_private_data;
	struct cbuf_vma_list *it, *head;

	mutex_lock(&cbuf->b_lock);
	list_for_each_entry_safe(it, head, &cbuf->vmas, list) {
		if (it->vma == vma) {
			list_del(&it->list);
			kfree(it);
			break;
		}
	}
	mutex_unlock(&cbuf->b_lock);
}

static const struct vm_operations_struct cbm_vma_ops = {
	.open  = _cbm_vm_open,
	.close = _cbm_vm_close,
};

static void *_cbm_sgt_alloc(unsigned int nents)
{
	struct sg_table *sgt;
	int rc;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (IS_ERR_OR_NULL(sgt))
		return ERR_CAST(sgt);

	rc = sg_alloc_table(sgt, nents, GFP_KERNEL);
	if (rc) {
		sgt = ERR_PTR(rc);
		goto err_sgt_free;
	}

	return sgt;

err_sgt_free:
	sg_free_table(sgt);

	return ERR_CAST(sgt);
}

static void _cbm_sgt_free(struct sg_table *sgt)
{
	struct scatterlist *scl;
	int i = 0;

	if (sgt->sgl) {
		for_each_sg(sgt->sgl, scl, sgt->nents, i)
			__free_pages(sg_page(scl), 0);
	}

	sg_free_table(sgt);

	kfree(sgt);
}

static void _cbm_cbuf_rb_insert(struct cbm_buffer *cbuf)
{
	struct rb_root *root = &cbuf->ctx->nodes;
	struct rb_node **it = &root->rb_node;
	struct rb_node *parent = NULL;
	struct cbm_buffer *entry;

	while (*it) {
		parent = *it;
		entry = rb_entry(parent, struct cbm_buffer, node);

		if (cbuf < entry) {
			it = &(*it)->rb_left;
		} else if (cbuf > entry) {
			it = &(*it)->rb_right;
		} else {
			pr_err("%s: cbuf already exist", __func__);
			return;
		}
	}

	rb_link_node(&cbuf->node, parent, it);
	rb_insert_color(&cbuf->node, root);
}

static void _cbm_cbuf_rb_delete(struct cbm_buffer *cbuf)
{
	struct rb_root *root = &cbuf->ctx->nodes;

	rb_erase(&cbuf->node, root);
}

static int _cbm_cbuf_pages_alloc(struct cbm_nodes *nodes)
{
	unsigned int table_size;

	table_size = sizeof(struct page *) * (nodes->size >> PAGE_SHIFT);
	nodes->pages = kmalloc(table_size, GFP_KERNEL);
	if (IS_ERR_OR_NULL(nodes->pages))
		return PTR_ERR(nodes->pages);

	return 0;
}

static void _cbm_cbuf_pages_free(struct cbm_nodes *nodes)
{
	kvfree(nodes->pages);
}

static struct cbm_node *_cbm_node_create(struct cbm_context *ctx,
					 struct cbm_buffer *cbuf,
					 unsigned long size)
{
	struct cbm_node *node;
	struct page *page;

	node = kmalloc(sizeof(*node), GFP_KERNEL);
	if (IS_ERR_OR_NULL(node))
		return ERR_CAST(node);

	page = alloc_page(cbuf->gfp_mask);
	if (IS_ERR_OR_NULL(page))
		goto err_free_info;

	node->page = page;

	INIT_LIST_HEAD(&node->list);

	return node;

err_free_info:
	kfree(node);

	return ERR_CAST(page);
}

static unsigned int _cbm_node_destroy(struct cbm_node *node,
				      struct scatterlist *scl,
				      struct scatterlist *scl_sync,
				      struct cbm_nodes *nodes, unsigned int i)
{
	struct page *page = node->page;

	if (scl_sync) {
		sg_set_page(scl_sync, page, PAGE_SIZE, 0);
		sg_dma_address(scl_sync) = page_to_phys(page);
	}

	sg_set_page(scl, page, PAGE_SIZE, 0);
	sg_dma_address(scl) = page_to_phys(page);

	if (nodes)
		nodes->pages[i++] = nth_page(page, 0);

	list_del(&node->list);

	kfree(node);

	return i;
}

static int _cbm_cbuf_sgt_alloc(struct cbm_buffer *cbuf, unsigned int pages)
{
	cbuf->sg_table = _cbm_sgt_alloc(pages);
	if (IS_ERR_OR_NULL(cbuf->sg_table))
		return PTR_ERR(cbuf->sg_table);

	return 0;
}

static void _cbm_cbuf_sgt_free(struct cbm_buffer *cbuf)
{
	return _cbm_sgt_free(cbuf->sg_table);
}

static int _cbm_cbuf_alloc(struct cbm_buffer *buffer)
{
	struct sg_table *sgt;
	struct scatterlist *sg;
	struct list_head pages;
	struct cbm_node *node;
	unsigned int nents_sync = 0;
	unsigned long rest_size = PAGE_ALIGN(buffer->size);
	struct cbm_nodes nodes;
	int i = 0;
	int ret = -ENOMEM;

#if defined(CONFIG_ARCH_SM6150)
	if (buffer->size / PAGE_SIZE > totalram_pages / 2)
#else
	if (buffer->size / PAGE_SIZE > totalram_pages() / 2)
#endif
		return -ENOMEM;

	nodes.size = 0;
	INIT_LIST_HEAD(&pages);

	while (rest_size > 0) {
		/* Create node and add it into the temporary list */
		node = _cbm_node_create(buffer->ctx, buffer, rest_size);
		if (IS_ERR(node)) {
			ret = PTR_ERR(node);
			goto err_info_free;
		}
		list_add_tail(&node->list, &pages);
		nodes.size += PAGE_SIZE;
		++nents_sync;
		rest_size -= PAGE_SIZE;
		i++;
	}

	ret = _cbm_cbuf_pages_alloc(&nodes);
	if (ret) {
		pr_err("%s: Cannot allocate buffer pages", __func__);
		goto err_info_free;
	}

	ret = _cbm_cbuf_sgt_alloc(buffer, i);
	if (ret) {
		pr_err("%s: Cannot allocate buffer sg table", __func__);
		goto err_cbuf_pages_free;
	}

	sgt = _cbm_sgt_alloc(nents_sync);
	if (IS_ERR_OR_NULL(sgt)) {
		pr_err("%s: Cannot allocate buffer nents", __func__);
		goto err_sg_table_free;
	}

	i = 0;
	sg = buffer->sg_table->sgl;
	do {
		node = list_first_entry_or_null(&pages, struct cbm_node, list);
		/* Remove the node from list and destroy it */
		i = _cbm_node_destroy(node, sg, sgt->sgl, &nodes, i);
		sgt->sgl = sg_next(sgt->sgl);
		sg	= sg_next(sg);
	} while (sg);

	_cbm_sgt_free(sgt);

	_cbm_cbuf_pages_free(&nodes);

	return 0;

err_sg_table_free:
	_cbm_cbuf_sgt_free(buffer);
err_cbuf_pages_free:
	_cbm_cbuf_pages_free(&nodes);
err_info_free:
	__free_pages(node->page, 0);
	kfree(node);

	return ret;
}

static struct cbm_buffer *_cbm_cbuf_create(struct cbm_context *ctx,
					   unsigned long len,
					   unsigned long flags)
{
	struct cbm_buffer *buffer;
	struct sg_table *table;
	int ret;

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return ERR_PTR(-ENOMEM);

	buffer->ctx   = ctx;
	buffer->size  = PAGE_ALIGN(len);
	buffer->flags = flags;

	ret = _cbm_cbuf_alloc(buffer);
	if (ret)
		goto err_buf_free;

	if (IS_ERR_OR_NULL(buffer->sg_table)) {
		ret = PTR_ERR(buffer->sg_table);
		goto err_cbuf_free;
	}

	table = buffer->sg_table;

	INIT_LIST_HEAD(&buffer->attached);
	INIT_LIST_HEAD(&buffer->vmas);
	mutex_init(&buffer->b_lock);

	mutex_lock(&ctx->t_lock);
	_cbm_cbuf_rb_insert(buffer);
	mutex_unlock(&ctx->t_lock);

	return buffer;

err_cbuf_free:
	_cbm_cbuf_sgt_free(buffer);
err_buf_free:
	kfree(buffer);

	return ERR_PTR(ret);
}

static void _cbm_cbuf_destroy(struct cbm_buffer *cbuf)
{
	struct cbm_context *ctx = cbuf->ctx;

	mutex_lock(&ctx->t_lock);
	_cbm_cbuf_rb_delete(cbuf);
	mutex_unlock(&ctx->t_lock);

	if (cbuf->kmap_cnt > 0) {
		pr_err_once("Likely missing a call to unmap\n");
		vunmap(cbuf->vaddr);
	}

	mutex_destroy(&cbuf->b_lock);
	_cbm_cbuf_sgt_free(cbuf);
	kfree(cbuf);
}

static void *_cbm_cbuf_kmap_get(struct cbm_buffer *cbuf)
{
	struct scatterlist *scl;
	pgprot_t pgprot;
	struct sg_table *sgt = cbuf->sg_table;
	int npages = PAGE_ALIGN(cbuf->size) / PAGE_SIZE;
	struct page **pages = vmalloc(sizeof(struct page *) * npages);
	struct page **tmp = pages;
	void *vaddr;
	int i, j;

	if (cbuf->kmap_cnt) {
		cbuf->kmap_cnt++;
		return cbuf->vaddr;
	}

	if (!pages)
		return ERR_PTR(-ENOMEM);

	if (cbuf->flags & CAM_MEM_FLAG_CACHE)
		pgprot = PAGE_KERNEL;
	else
		pgprot = pgprot_writecombine(PAGE_KERNEL);

	for_each_sg(sgt->sgl, scl, sgt->nents, i) {
		int entry_pages = PAGE_ALIGN(scl->length) / PAGE_SIZE;
		struct page *page = sg_page(scl);

		WARN_ON(i >= npages);
		for (j = 0; j < entry_pages; j++)
			*(tmp++) = page++;
	}

	vaddr = vmap(pages, npages, VM_MAP, pgprot);
	vfree(pages);

	if (IS_ERR_OR_NULL(vaddr)) {
		if (WARN_ONCE(1, "Map should return valid pointer"))
			return ERR_PTR(-ENOMEM);
	}

	cbuf->vaddr = vaddr;
	cbuf->kmap_cnt++;

	return vaddr;
}

static void _cbm_cbuf_kmap_put(struct cbm_buffer *cbuf)
{
	if (cbuf->kmap_cnt == 0) {
		pr_warn_ratelimited("Likely unbalanced map/unmap, pid:%d\n",
				    current->pid);
		return;
	}

	cbuf->kmap_cnt--;
	if (!cbuf->kmap_cnt) {
		vunmap(cbuf->vaddr);
		cbuf->vaddr = NULL;
	}
}

static struct sg_table *_cbm_sg_table_new(struct sg_table *sgt)
{
	struct sg_table *new_sgt;
	struct scatterlist *scl, *new_scl;
	int ret, i;

	new_sgt = kzalloc(sizeof(*new_sgt), GFP_KERNEL);
	if (!new_sgt)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(new_sgt, sgt->nents, GFP_KERNEL);
	if (ret) {
		kfree(new_sgt);
		return ERR_PTR(-ENOMEM);
	}

	new_scl = new_sgt->sgl;
	for_each_sg(sgt->sgl, scl, sgt->nents, i) {
		memcpy(new_scl, scl, sizeof(*scl));
		sg_dma_address(new_scl) = 0;
		sg_dma_len(new_scl) = 0;
		new_scl = sg_next(new_scl);
	}

	return new_sgt;
}

static void _cbm_sg_table_del(struct sg_table *sgt)
{
	sg_free_table(sgt);
	kfree(sgt);
}

#if defined(CONFIG_ARCH_SM6150)
static int _op_dma_buf_attach(struct dma_buf *dmabuf, struct device *dev,
			      struct dma_buf_attachment *attachment)
#else
static int _op_dma_buf_attach(struct dma_buf *dmabuf,
			      struct dma_buf_attachment *attachment)
#endif
{
	struct cbm_buffer *cbuf = dmabuf->priv;
	struct cbm_dma_buf_attachment *new_att;
	struct sg_table *sgt;
	int rc;

	new_att = kzalloc(sizeof(*new_att), GFP_KERNEL);
	if (IS_ERR_OR_NULL(new_att))
		return PTR_ERR(new_att);

	sgt = _cbm_sg_table_new(cbuf->sg_table);
	if (IS_ERR_OR_NULL(sgt)) {
		rc = PTR_ERR(sgt);
		goto err_free_att;
	}

	new_att->table = sgt;
#if defined(CONFIG_ARCH_SM6150)
	new_att->dev = dev;
#endif
	new_att->dma_mapped = false;
	INIT_LIST_HEAD(&new_att->list);

	attachment->priv = new_att;

	mutex_lock(&cbuf->b_lock);
	list_add(&new_att->list, &cbuf->attached);
	mutex_unlock(&cbuf->b_lock);

	return 0;

err_free_att:
	kfree(new_att);

	return rc;
}

static void _op_dma_buf_detatch(struct dma_buf *dmabuf,
				struct dma_buf_attachment *attachment)
{
	struct cbm_dma_buf_attachment *a = attachment->priv;
	struct cbm_buffer *buffer = dmabuf->priv;

	mutex_lock(&buffer->b_lock);
	list_del(&a->list);
	mutex_unlock(&buffer->b_lock);
	_cbm_sg_table_del(a->table);

	kfree(a);
}

static struct sg_table *_op_dma_buf_map(struct dma_buf_attachment *attachment,
					enum dma_data_direction direction)
{
	struct cbm_dma_buf_attachment *a = attachment->priv;
	struct sg_table *table;
	int count, map_attrs;
	struct cbm_buffer *buffer = attachment->dmabuf->priv;

	table = a->table;

	map_attrs = attachment->dma_map_attrs;
	if (!(buffer->flags & CAM_MEM_FLAG_CACHE))
		map_attrs |= DMA_ATTR_SKIP_CPU_SYNC;

	mutex_lock(&buffer->b_lock);
	count = dma_map_sg_attrs(attachment->dev, table->sgl,
				 table->nents, direction,
				 map_attrs);

	if (count <= 0) {
		mutex_unlock(&buffer->b_lock);
		return ERR_PTR(-ENOMEM);
	}

	a->dma_mapped = true;
	mutex_unlock(&buffer->b_lock);

	return table;
}

static void _op_dma_buf_unmap(struct dma_buf_attachment *attachment,
			      struct sg_table *table,
			      enum dma_data_direction direction)
{
	int map_attrs;
	struct cbm_buffer *buffer = attachment->dmabuf->priv;
	struct cbm_dma_buf_attachment *a = attachment->priv;

	map_attrs = attachment->dma_map_attrs;
	if (!(buffer->flags & CAM_MEM_FLAG_CACHE))
		map_attrs |= DMA_ATTR_SKIP_CPU_SYNC;

	mutex_lock(&buffer->b_lock);
	dma_unmap_sg_attrs(attachment->dev, table->sgl, table->nents,
			   direction, map_attrs);
	a->dma_mapped = false;
	mutex_unlock(&buffer->b_lock);
}

static int _op_dma_buf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct cbm_buffer *buffer = dmabuf->priv;
	struct sg_table *table = buffer->sg_table;
	unsigned long addr = vma->vm_start;
	unsigned long offset = vma->vm_pgoff * PAGE_SIZE;
	struct scatterlist *sg;
	int i, ret = 0;

	if (!(buffer->flags & CAM_MEM_FLAG_CACHE))
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	vma->vm_private_data = buffer;
	vma->vm_ops = &cbm_vma_ops;
	_cbm_vm_open(vma);

	mutex_lock(&buffer->b_lock);

	for_each_sg(table->sgl, sg, table->nents, i) {
		struct page *page = sg_page(sg);
		unsigned long remainder = vma->vm_end - addr;
		unsigned long len = sg->length;

		if (offset >= sg->length) {
			offset -= sg->length;
			continue;
		} else if (offset) {
			page += offset / PAGE_SIZE;
			len = sg->length - offset;
			offset = 0;
		}
		len = min(len, remainder);
		ret = remap_pfn_range(vma, addr, page_to_pfn(page), len,
				      vma->vm_page_prot);
		if (ret)
			goto err_vm_close;

		addr += len;
		if (addr >= vma->vm_end)
			break;
	}

	mutex_unlock(&buffer->b_lock);

	return ret;

err_vm_close:
	_cbm_vm_close(vma);
	mutex_unlock(&buffer->b_lock);
	pr_err("%s: failure mapping buffer to userspace\n", __func__);

	return ret;
}

static void _op_dma_buf_release(struct dma_buf *dmabuf)
{
	struct cbm_buffer *buffer = dmabuf->priv;

	_cbm_cbuf_destroy(buffer);
	kfree(dmabuf->exp_name);
}

static void *_op_dma_buf_vmap(struct dma_buf *dmabuf)
{
	struct cbm_buffer *buffer = dmabuf->priv;
	void *vaddr = ERR_PTR(-EINVAL);

	mutex_lock(&buffer->b_lock);
	vaddr = _cbm_cbuf_kmap_get(buffer);
	mutex_unlock(&buffer->b_lock);

	return vaddr;
}

static void _op_dma_buf_vunmap(struct dma_buf *dmabuf, void *vaddr)
{
	struct cbm_buffer *buffer = dmabuf->priv;

	mutex_lock(&buffer->b_lock);
	_cbm_cbuf_kmap_put(buffer);
	mutex_unlock(&buffer->b_lock);
}

static void *_op_dma_buf_kmap(struct dma_buf *dmabuf, unsigned long offset)
{
	void *vaddr = _op_dma_buf_vmap(dmabuf);

	if (IS_ERR_OR_NULL(vaddr))
		return ERR_CAST(vaddr);

	return vaddr + offset * PAGE_SIZE;
}

static void _op_dma_buf_kunmap(struct dma_buf *dmabuf, unsigned long offset,
			       void *ptr)
{
	_op_dma_buf_vunmap(dmabuf, ptr);
}

static int _op_dma_buf_beg_cpu_access(struct dma_buf *dmabuf,
				      enum dma_data_direction direction)
{
	struct cbm_buffer *buffer = dmabuf->priv;
	struct cbm_dma_buf_attachment *a;
	int ret = 0;

	mutex_lock(&buffer->b_lock);
	list_for_each_entry(a, &buffer->attached, list) {
		dma_sync_sg_for_cpu(a->dev, a->table->sgl,
				    a->table->nents, direction);
	}
	mutex_unlock(&buffer->b_lock);

	return ret;
}

static int _op_dma_buf_end_cpu_access(struct dma_buf *dmabuf,
				      enum dma_data_direction direction)
{
	struct cbm_buffer *buffer = dmabuf->priv;
	struct cbm_dma_buf_attachment *a;
	int ret = 0;

	mutex_lock(&buffer->b_lock);
	list_for_each_entry(a, &buffer->attached, list) {
		dma_sync_sg_for_device(a->dev, a->table->sgl,
				       a->table->nents, direction);
	}
	mutex_unlock(&buffer->b_lock);

	return ret;
}

static int _op_dma_buf_get_flags(struct dma_buf *dmabuf,
				 unsigned long *flags)
{
	struct cbm_buffer *buffer = dmabuf->priv;
	*flags = buffer->flags;

	return 0;
}

static const struct dma_buf_ops cbm_dma_buf_ops = {
	.map_dma_buf	  = _op_dma_buf_map,
	.unmap_dma_buf	  = _op_dma_buf_unmap,
	.mmap		  = _op_dma_buf_mmap,
	.release	  = _op_dma_buf_release,
	.attach		  = _op_dma_buf_attach,
	.detach		  = _op_dma_buf_detatch,
	.begin_cpu_access = _op_dma_buf_beg_cpu_access,
	.end_cpu_access   = _op_dma_buf_end_cpu_access,
#if defined(CONFIG_ARCH_SM6150)
	.map_atomic = _op_dma_buf_kmap,
	.unmap_atomic = _op_dma_buf_kunmap,
#endif
	.map		  = _op_dma_buf_kmap,
	.unmap		  = _op_dma_buf_kunmap,
	.vmap		  = _op_dma_buf_vmap,
	.vunmap		  = _op_dma_buf_vunmap,
	.get_flags	  = _op_dma_buf_get_flags,
};

struct dma_buf *cbm_alloc_buffer(size_t len, unsigned int flags)
{
	struct cbm_context *ctx = idev;
	struct cbm_buffer *buffer = NULL;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;

	pr_debug("%s: len %zu flags %x\n", __func__, len, flags);

	len = PAGE_ALIGN(len);
	if (!len)
		return ERR_PTR(-EINVAL);

	buffer = _cbm_cbuf_create(ctx, len, flags);
	if (IS_ERR_OR_NULL(buffer)) {
		pr_err("%s: Cannot create camera buffer\n", __func__);
		return ERR_CAST(buffer);
	}

	exp_info.ops = &cbm_dma_buf_ops;
	exp_info.size = buffer->size;
	exp_info.flags = O_RDWR;
	exp_info.priv = buffer;
	exp_info.exp_name = kasprintf(GFP_KERNEL, "%s%p", KBUILD_MODNAME, ctx);

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		pr_err("%s: Cannot export camera buffer\n", __func__);
		_cbm_cbuf_destroy(buffer);
		kfree(exp_info.exp_name);
		return ERR_CAST(dmabuf);
	}

	return dmabuf;
}

void cbm_free_buffer(struct dma_buf *dmabuf)
{
	if (!dmabuf) {
		pr_err("%s: Invalid argument(s)\n", __func__);
		return;
	}

	dma_buf_put(dmabuf);
}

#else

static int cam_smmu_get_dma_dir(uint32_t flags)
{
	int dir = DMA_BIDIRECTIONAL;

	if (flags & CAM_MEM_FLAG_HW_READ_ONLY)
		dir = DMA_TO_DEVICE;
	else if (flags & CAM_MEM_FLAG_HW_WRITE_ONLY)
		dir = DMA_FROM_DEVICE;

	return dir;
}

struct dma_buf *cbm_alloc_buffer(size_t len, unsigned int flags)
{
	const struct vb2_mem_ops *ops = idev->vb2_ops;
	enum dma_data_direction dma_dir = cam_smmu_get_dma_dir(flags);
	struct dma_buf *dmabuf;
	void *vbuf;

	if (!ops) {
		pr_err("%s: Invalid argument(s)\n", __func__);
		return NULL;
	}

	vbuf = ops->alloc(idev->dev, 0, len, dma_dir, GFP_KERNEL);
	if (IS_ERR_OR_NULL(vbuf)) {
		pr_err("%s: Cannot allocate VB2 buffer\n", __func__);
		return ERR_CAST(vbuf);
	}

	dmabuf = ops->get_dmabuf(vbuf, flags);
	if (IS_ERR_OR_NULL(dmabuf)) {
		pr_err("%s: Cannot get DMA buffer\n", __func__);
		dmabuf = ERR_CAST(dmabuf);
		goto err_vbuf_free;
	}

	return dmabuf;

err_vbuf_free:
	ops->put(vbuf);
	return dmabuf;
}

void cbm_free_buffer(struct dma_buf *dmabuf)
{
	const struct vb2_mem_ops *ops = idev->vb2_ops;
	void *vbuf = dmabuf->priv;

	if (!ops) {
		pr_err("%s: Invalid argument(s)\n", __func__);
		return;
	}

	ops->put(vbuf);
}

#endif	/* USE_VIDEOBUF2_MEMOPS */

int cam_buf_mgr_init(struct platform_device *pdev)
{
	idev = kzalloc(sizeof(struct cbm_context), GFP_KERNEL);
	if (IS_ERR_OR_NULL(idev)) {
		pr_err("%s: Cannot allocate memory\n", __func__);
		return PTR_ERR(idev);
	}

#if defined(USE_VIDEOBUF2_MEMOPS)
	idev->dev = &pdev->dev;
	idev->vb2_ops = &vb2_dma_sg_memops;
#endif
	idev->nodes = RB_ROOT;
	mutex_init(&idev->t_lock);

	return 0;
}

void cam_buf_mgr_exit(struct platform_device *pdev)
{
	if (IS_ERR_OR_NULL(idev)) {
		pr_err("%s: Invalid CBM context\n", __func__);
		return;
	}

	mutex_destroy(&idev->t_lock);

#if defined(USE_VIDEOBUF2_MEMOPS)
	idev->vb2_ops = NULL;
#endif
	kfree(idev);
}
