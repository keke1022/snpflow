// SPDX-License-Identifier: GPL-2.0
/* Generic dma-buf attach/map probe. No device DMA is submitted. */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/capability.h>
#include <linux/dma-buf.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/scatterlist.h>
#include <linux/uaccess.h>

#include "probe_uapi.h"

struct mapped_attachment {
	struct pci_dev *pdev;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
};

static void unmap_one(struct dma_buf *dmabuf, struct mapped_attachment *m)
{
	if (m->sgt)
		dma_buf_unmap_attachment(m->attach, m->sgt, DMA_BIDIRECTIONAL);
	if (m->attach)
		dma_buf_detach(dmabuf, m->attach);
	if (m->pdev)
		pci_dev_put(m->pdev);
}

static int map_one(struct dma_buf *dmabuf, const struct cvm_probe_pci_id *id,
		   struct mapped_attachment *m)
{
	m->pdev = pci_get_domain_bus_and_slot(id->domain, id->bus, id->devfn);
	if (!m->pdev)
		return -ENODEV;
	m->attach = dma_buf_attach(dmabuf, &m->pdev->dev);
	if (IS_ERR(m->attach)) {
		int ret = PTR_ERR(m->attach);

		m->attach = NULL;
		return ret;
	}
	m->sgt = dma_buf_map_attachment(m->attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(m->sgt)) {
		int ret = PTR_ERR(m->sgt);

		m->sgt = NULL;
		return ret;
	}
	return 0;
}

static u64 mapped_bytes(const struct sg_table *sgt)
{
	struct scatterlist *sg;
	u64 bytes = 0;
	unsigned int i;

	for_each_sg(sgt->sgl, sg, sgt->nents, i)
		bytes += sg_dma_len(sg);
	return bytes;
}

static bool layouts_equal(const struct sg_table *a, const struct sg_table *b)
{
	struct scatterlist *sa = a->sgl;
	struct scatterlist *sb = b->sgl;
	unsigned int i;

	if (a->nents != b->nents)
		return false;
	for (i = 0; i < a->nents; i++) {
		if (sg_dma_address(sa) != sg_dma_address(sb) ||
		    sg_dma_len(sa) != sg_dma_len(sb))
			return false;
		sa = sg_next(sa);
		sb = sg_next(sb);
	}
	return true;
}

static long probe_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct cvm_dmabuf_probe_req req;
	struct mapped_attachment first = {0};
	struct mapped_attachment second = {0};
	struct dma_buf *dmabuf;
	int ret;

	if (cmd != CVM_DMABUF_PROBE_IOC_ATTACH)
		return -ENOTTY;
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (req.flags & ~CVM_PROBE_F_SECOND)
		return -EINVAL;

	dmabuf = dma_buf_get(req.fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);
	ret = map_one(dmabuf, &req.first, &first);
	if (ret)
		goto out;

	req.first_nents = first.sgt->nents;
	req.first_bytes = mapped_bytes(first.sgt);
	req.first_dma = sg_dma_address(first.sgt->sgl);
	req.layouts_equal = 1;

	if (req.flags & CVM_PROBE_F_SECOND) {
		ret = map_one(dmabuf, &req.second, &second);
		if (ret)
			goto out;
		req.second_nents = second.sgt->nents;
		req.second_bytes = mapped_bytes(second.sgt);
		req.second_dma = sg_dma_address(second.sgt->sgl);
		req.layouts_equal = layouts_equal(first.sgt, second.sgt);
	}

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		ret = -EFAULT;
out:
	unmap_one(dmabuf, &second);
	unmap_one(dmabuf, &first);
	dma_buf_put(dmabuf);
	return ret;
}

static const struct file_operations probe_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = probe_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = probe_ioctl,
#endif
};

static struct miscdevice probe_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "cvm_dmabuf_probe",
	.fops = &probe_fops,
	.mode = 0600,
};

static int __init probe_init(void)
{
	return misc_register(&probe_misc);
}

static void __exit probe_exit(void)
{
	misc_deregister(&probe_misc);
}

module_init(probe_init);
module_exit(probe_exit);
MODULE_IMPORT_NS("DMA_BUF");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Generic CVM dma-buf multi-device attachment probe");
