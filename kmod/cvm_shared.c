// SPDX-License-Identifier: GPL-2.0
/*
 * cvm_shared —— 在 SEV-SNP guest 里分配「已转共享、且不经 swiotlb」的内存。
 *
 * 要解决的问题
 * ------------
 * SNP guest 里 guest 内存默认是私有加密页，设备无法 DMA 进去。实测：
 *   - ibv_reg_mr / devx_umem_reg 对普通内存和大页一律返回 EIO
 *   - 走系统 dma-heap 的 dma-buf 能注册，但内核日志显示它经过 swiotlb，
 *     且单次映射超过约 1MB 就失败（"swiotlb buffer is full"）
 *
 * 根因是系统 dma-heap 的 exporter 在 map_dma_buf 里调用了 dma_map_sgtable()，
 * 而这些页是私有的，于是被 bounce 到 swiotlb 的共享缓冲区里。
 *
 * 本模块的做法
 * ------------
 *   1. 自己分配页
 *   2. set_memory_decrypted() 把页转成共享（清 C-bit + 向 hypervisor 做
 *      page state change + PVALIDATE，内核标准函数全帮我们做了）
 *   3. 包装成 dma-buf，但 map_dma_buf **不调用 dma_map_sgtable()**，
 *      而是直接把物理地址填进 sg_dma_address —— 页已经是共享的，
 *      不需要也不应该再 bounce 一次
 *   4. 引用计数归零时 set_memory_encrypted() 转回并释放
 *
 * 前提条件（重要）
 * ----------------
 * 第 3 步直接返回物理地址，成立的前提是 **guest 里没有 vIOMMU**
 * （本环境已确认 /sys/kernel/iommu_groups 为空，GPA 即设备地址）。
 * 若将来 guest 侧引入 vIOMMU，这里必须改成申请 IOVA 并建立映射。
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <linux/set_memory.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/capability.h>
#include <linux/sizes.h>
#include <linux/iommu.h>
#include <linux/list.h>
#include <linux/mutex.h>

#include "cvm_shared.h"

/*
 * 分配粒度：优先按 2MB（order 9）成块，与 DPDK 的大页形态一致，
 * 也让 sg_table 的条目数尽量少。分配不到就逐级降阶。
 */
#define CVM_MAX_ORDER   9
#define CVM_MAX_SIZE    (1UL << 30)   /* 单次最大 1GB，防止误用打爆内存 */

struct cvm_chunk {
	struct page *page;
	unsigned int order;
};

/*
 * 映射登记：记录每个 mmap 出来的区间属于哪块缓冲区。
 *
 * 驱动与应用可能各自申请共享内存，任何一方都无法掌握全部区间，
 * 但内核知道全部映射。LOOKUP ioctl 据此按虚拟地址反查归属。
 */
struct cvm_mapping {
	struct mm_struct *mm;
	unsigned long start;
	unsigned long end;
	struct cvm_buffer *buf;
	struct list_head node;
};

static LIST_HEAD(cvm_mappings);
static DEFINE_MUTEX(cvm_mappings_lock);

struct cvm_buffer {
	size_t size;                /* 对齐后的总字节数 */
	unsigned int nr_chunks;
	struct cvm_chunk *chunks;
	struct sg_table sgt;        /* 模板：dma 地址已填好，map 时复制 */
	bool decrypted;
	struct dma_buf *dmabuf;
};

/* ------------------------------------------------------------------ */
/* 页分配与加解密转换                                                  */
/* ------------------------------------------------------------------ */

static void cvm_free_chunks(struct cvm_buffer *buf)
{
	unsigned int i;

	if (!buf->chunks)
		return;

	for (i = 0; i < buf->nr_chunks; i++)
		if (buf->chunks[i].page)
			__free_pages(buf->chunks[i].page, buf->chunks[i].order);

	kvfree(buf->chunks);
	buf->chunks = NULL;
	buf->nr_chunks = 0;
}

static int cvm_alloc_chunks(struct cvm_buffer *buf, size_t size)
{
	size_t remaining = size;
	unsigned int max_chunks = size >> PAGE_SHIFT;  /* 最坏情况每块一页 */
	unsigned int n = 0;

	buf->chunks = kvcalloc(max_chunks, sizeof(*buf->chunks), GFP_KERNEL);
	if (!buf->chunks)
		return -ENOMEM;

	while (remaining) {
		unsigned int order = min_t(unsigned int, CVM_MAX_ORDER,
					   get_order(remaining));
		struct page *page = NULL;

		/* 逐级降阶，直到要到内存 */
		for (;;) {
			page = alloc_pages(GFP_KERNEL | __GFP_ZERO |
					   __GFP_NOWARN, order);
			if (page)
				break;
			if (order == 0)
				goto out_nomem;
			order--;
		}

		buf->chunks[n].page = page;
		buf->chunks[n].order = order;
		n++;

		if ((PAGE_SIZE << order) >= remaining)
			remaining = 0;
		else
			remaining -= PAGE_SIZE << order;
	}

	buf->nr_chunks = n;
	return 0;

out_nomem:
	buf->nr_chunks = n;
	cvm_free_chunks(buf);
	return -ENOMEM;
}

/*
 * 把所有块转成共享页。
 *
 * set_memory_decrypted() 接受的是内核虚拟地址；对 alloc_pages 得到的页，
 * page_address() 给出直接映射地址。这比 dpdk-test-main 里「临时篡改一个
 * dummy 页的 PTE 指向目标物理页」的做法干净得多，也没有那个全局竞态。
 */
static int cvm_set_decrypted(struct cvm_buffer *buf)
{
	unsigned int i;
	int ret;

	for (i = 0; i < buf->nr_chunks; i++) {
		unsigned long addr = (unsigned long)page_address(buf->chunks[i].page);
		int npages = 1 << buf->chunks[i].order;

		ret = set_memory_decrypted(addr, npages);
		if (ret) {
			pr_err("set_memory_decrypted failed at chunk %u: %d\n", i, ret);
			/* 已转的部分回滚，避免留下状态不一致的页 */
			while (i--) {
				addr = (unsigned long)page_address(buf->chunks[i].page);
				npages = 1 << buf->chunks[i].order;
				if (set_memory_encrypted(addr, npages))
					pr_err("rollback set_memory_encrypted failed at %u\n", i);
			}
			return ret;
		}
	}

	buf->decrypted = true;
	return 0;
}

static void cvm_set_encrypted(struct cvm_buffer *buf)
{
	unsigned int i;

	if (!buf->decrypted)
		return;

	for (i = 0; i < buf->nr_chunks; i++) {
		unsigned long addr = (unsigned long)page_address(buf->chunks[i].page);
		int npages = 1 << buf->chunks[i].order;

		/*
		 * 这一步失败是严重问题：页会以共享状态被交还给伙伴系统，
		 * 之后被当成私有内存复用时会触发 RMP 不匹配。只能报错并
		 * 故意泄漏这些页，不能还给分配器。
		 */
		if (set_memory_encrypted(addr, npages)) {
			pr_err("set_memory_encrypted failed at chunk %u; leaking pages\n", i);
			buf->chunks[i].page = NULL;
		}
	}

	buf->decrypted = false;
}

/* ------------------------------------------------------------------ */
/* sg_table：直接填物理地址，不经 dma_map_sgtable                      */
/* ------------------------------------------------------------------ */

static int cvm_build_sgt(struct cvm_buffer *buf)
{
	struct scatterlist *sg;
	unsigned int i;
	int ret;

	ret = sg_alloc_table(&buf->sgt, buf->nr_chunks, GFP_KERNEL);
	if (ret)
		return ret;

	sg = buf->sgt.sgl;
	for (i = 0; i < buf->nr_chunks; i++) {
		unsigned int len = PAGE_SIZE << buf->chunks[i].order;

		sg_set_page(sg, buf->chunks[i].page, len, 0);

		/*
		 * 关键点：页已经是共享的，设备可以直接访问其物理地址。
		 * 这里手工填 dma 地址，绕开 dma_map_sgtable() —— 后者会
		 * 把私有页 bounce 到 swiotlb，正是我们要避免的。
		 */
		sg_dma_address(sg) = page_to_phys(buf->chunks[i].page);
		sg_dma_len(sg) = len;

		sg = sg_next(sg);
	}

	/* 表示「已映射」的条目数；未调用 dma_map 时必须自己设 */
	buf->sgt.nents = buf->nr_chunks;
	return 0;
}

/* 每个 attachment 需要自己的一份 sg_table，这里从模板复制 */
static struct sg_table *cvm_dup_sgt(struct cvm_buffer *buf)
{
	struct sg_table *sgt;
	struct scatterlist *dst, *src;
	unsigned int i;
	int ret;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(sgt, buf->sgt.orig_nents, GFP_KERNEL);
	if (ret) {
		kfree(sgt);
		return ERR_PTR(ret);
	}

	dst = sgt->sgl;
	src = buf->sgt.sgl;
	for (i = 0; i < buf->sgt.orig_nents; i++) {
		sg_set_page(dst, sg_page(src), src->length, src->offset);
		sg_dma_address(dst) = sg_dma_address(src);
		sg_dma_len(dst) = sg_dma_len(src);
		dst = sg_next(dst);
		src = sg_next(src);
	}
	sgt->nents = buf->sgt.nents;

	return sgt;
}

/* ------------------------------------------------------------------ */
/* dma_buf_ops                                                         */
/* ------------------------------------------------------------------ */

/*
 * 我们在 sg_dma_address 里填的是裸物理地址。只有当设备**不**经过 IOMMU
 * 翻译时这才是对的。如果设备挂在一个会做地址翻译的 IOMMU 域后面，
 * 物理地址会被当成 IOVA 去查页表 —— 那是静默的错误数据，比报错难查得多。
 *
 * 所以在 attach 阶段就硬性拒绝，而不是只在注释里写「前提是没有 vIOMMU」。
 * identity/passthrough 域里 IOVA == PA，可以放行。
 */
static int cvm_attach(struct dma_buf *dmabuf, struct dma_buf_attachment *attach)
{
	struct iommu_domain *domain;

	if (!attach->dev)
		return 0;

	domain = iommu_get_domain_for_dev(attach->dev);
	if (!domain)
		return 0;   /* 没有 IOMMU 域，物理地址直通 */

	if (domain->type == IOMMU_DOMAIN_IDENTITY)
		return 0;   /* 直通域，IOVA == PA */

	dev_err(attach->dev,
		"cvm_shared: device sits behind an IOMMU domain (type %u); "
		"raw physical addresses would be misinterpreted as IOVAs. "
		"Refusing to attach.\n", domain->type);
	return -EOPNOTSUPP;
}

static struct sg_table *cvm_map_dma_buf(struct dma_buf_attachment *attach,
					enum dma_data_direction dir)
{
	struct cvm_buffer *buf = attach->dmabuf->priv;

	return cvm_dup_sgt(buf);
}

static void cvm_unmap_dma_buf(struct dma_buf_attachment *attach,
			      struct sg_table *sgt, enum dma_data_direction dir)
{
	/* 没有调用过 dma_map_sgtable，所以这里也不能 unmap，只释放表 */
	sg_free_table(sgt);
	kfree(sgt);
}

static int cvm_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct cvm_buffer *buf = dmabuf->priv;
	unsigned long addr = vma->vm_start;
	unsigned int i;

	if (vma->vm_pgoff)
		return -EINVAL;
	if (vma->vm_end - vma->vm_start > buf->size)
		return -EINVAL;

	/*
	 * 用户态映射也必须清掉 C-bit，否则 CPU 通过这个映射读到的是把
	 * 明文当密文解出来的垃圾 —— 内核侧 set_memory_decrypted 只改了
	 * 直接映射那一份，新建的用户映射默认带 _PAGE_ENC。
	 */
	vma->vm_page_prot = pgprot_decrypted(vma->vm_page_prot);

	for (i = 0; i < buf->nr_chunks && addr < vma->vm_end; i++) {
		unsigned long len = PAGE_SIZE << buf->chunks[i].order;
		int ret;

		if (addr + len > vma->vm_end)
			len = vma->vm_end - addr;

		ret = remap_pfn_range(vma, addr,
				      page_to_pfn(buf->chunks[i].page),
				      len, vma->vm_page_prot);
		if (ret)
			return ret;

		addr += len;
	}

	/* 登记本次映射，供 LOOKUP 反查 */
	{
		struct cvm_mapping *m = kzalloc(sizeof(*m), GFP_KERNEL);

		if (m == NULL)
			return -ENOMEM;
		m->mm = vma->vm_mm;
		m->start = vma->vm_start;
		m->end = vma->vm_end;
		m->buf = buf;
		mutex_lock(&cvm_mappings_lock);
		list_add(&m->node, &cvm_mappings);
		mutex_unlock(&cvm_mappings_lock);
	}

	return 0;
}

static void cvm_release(struct dma_buf *dmabuf)
{
	struct cvm_buffer *buf = dmabuf->priv;

	/*
	 * 走到这里说明 dma-buf 引用计数已归零：用户态的 fd 关了，
	 * 导入方（mlx5）也已经 detach。此时转回私有才是安全的 ——
	 * 不会出现「页正在被重新加密而网卡还在往里写」的窗口。
	 *
	 * 进程崩溃时内核自动关闭 fd，同样走到这里，所以不需要额外的
	 * 崩溃清理逻辑。这是相对 set_shared.ko 最重要的改进。
	 */
	pr_info("release: refcount dropped to zero, converting %zu bytes back to private\n",
		buf->size);

	/* 清掉本缓冲区的映射登记 */
	{
		struct cvm_mapping *m, *tmp;

		mutex_lock(&cvm_mappings_lock);
		list_for_each_entry_safe(m, tmp, &cvm_mappings, node) {
			if (m->buf == buf) {
				list_del(&m->node);
				kfree(m);
			}
		}
		mutex_unlock(&cvm_mappings_lock);
	}

	sg_free_table(&buf->sgt);
	cvm_set_encrypted(buf);
	cvm_free_chunks(buf);
	kfree(buf);

	pr_info("release: done\n");
}

static const struct dma_buf_ops cvm_dma_buf_ops = {
	.attach        = cvm_attach,
	.map_dma_buf   = cvm_map_dma_buf,
	.unmap_dma_buf = cvm_unmap_dma_buf,
	.release       = cvm_release,
	.mmap          = cvm_mmap,
};

/* ------------------------------------------------------------------ */
/* ioctl                                                               */
/* ------------------------------------------------------------------ */

/*
 * 分配并准备好 fd，但**不** fd_install —— 把 dmabuf 通过 *out 交还调用方，
 * 由调用方在 copy_to_user 成功之后再 install。这样失败路径可以干净回滚。
 */
static int cvm_do_alloc(struct cvm_shared_alloc *req, struct dma_buf **out)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct cvm_buffer *buf;
	struct dma_buf *dmabuf;
	size_t size;
	int fd, ret;

	if (req->flags)
		return -EINVAL;
	if (!req->size || req->size > CVM_MAX_SIZE)
		return -EINVAL;

	size = PAGE_ALIGN(req->size);

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	buf->size = size;

	ret = cvm_alloc_chunks(buf, size);
	if (ret)
		goto err_free_buf;

	ret = cvm_set_decrypted(buf);
	if (ret)
		goto err_free_chunks;

	ret = cvm_build_sgt(buf);
	if (ret)
		goto err_encrypt;

	exp_info.exp_name = KBUILD_MODNAME;
	exp_info.owner    = THIS_MODULE;
	exp_info.ops      = &cvm_dma_buf_ops;
	exp_info.size     = size;
	exp_info.flags    = O_RDWR | O_CLOEXEC;
	exp_info.priv     = buf;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		goto err_sgt;
	}
	buf->dmabuf = dmabuf;

	/*
	 * 不用 dma_buf_fd()：它内部直接 fd_install，一旦之后 copy_to_user
	 * 失败，fd 已经进了进程的 fd 表而用户态拿不到编号，就泄漏了。
	 * 拆成 get_unused_fd_flags + fd_install，让调用方能在失败时回滚。
	 */
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		/* dma_buf_put 会调用 release，那里做转回和释放 */
		dma_buf_put(dmabuf);
		return fd;
	}

	pr_info("allocated %zu bytes in %u chunks, fd=%d\n",
		size, buf->nr_chunks, fd);

	req->fd = fd;
	*out = dmabuf;
	return 0;

err_sgt:
	sg_free_table(&buf->sgt);
err_encrypt:
	cvm_set_encrypted(buf);
err_free_chunks:
	cvm_free_chunks(buf);
err_free_buf:
	kfree(buf);
	return ret;
}

static int cvm_do_layout(struct cvm_shared_layout *req)
{
	struct cvm_shared_chunk __user *uchunks;
	struct dma_buf *dmabuf;
	struct cvm_buffer *buf;
	unsigned int i, n;
	int ret = 0;

	dmabuf = dma_buf_get(req->fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	/* 只认自己导出的 buffer，否则 priv 的类型就没保证了 */
	if (dmabuf->ops != &cvm_dma_buf_ops) {
		dma_buf_put(dmabuf);
		return -EINVAL;
	}

	buf = dmabuf->priv;
	req->nr_chunks = buf->nr_chunks;

	uchunks = (struct cvm_shared_chunk __user *)(uintptr_t)req->chunks;
	if (uchunks) {
		n = min(req->max_chunks, buf->nr_chunks);
		for (i = 0; i < n; i++) {
			struct cvm_shared_chunk c = {
				.phys = page_to_phys(buf->chunks[i].page),
				.len  = PAGE_SIZE << buf->chunks[i].order,
			};

			if (copy_to_user(&uchunks[i], &c, sizeof(c))) {
				ret = -EFAULT;
				break;
			}
		}
	}

	dma_buf_put(dmabuf);
	return ret;
}

/* 由 (offset, seed) 决定的确定性图案，两侧独立生成、互相比对 */
static inline u64 cvm_pattern(u64 off, u64 seed)
{
	return seed ^ (off * 0x9E3779B97F4A7C15ULL) ^ 0x5851F42D4C957F2DULL;
}

/*
 * 跨映射校验：**通过直接映射**（page_address）访问这些页。
 * 用户态那一侧走的是 mmap 出来的映射。两条路径的页表项各自独立，
 * C-bit 只要有一侧处理错了，这里比对就会失败。
 */
static int cvm_do_xcheck(struct cvm_shared_xcheck *req)
{
	struct dma_buf *dmabuf;
	struct cvm_buffer *buf;
	u64 off, end;
	unsigned int i;
	int ret = 0;

	dmabuf = dma_buf_get(req->fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);
	if (dmabuf->ops != &cvm_dma_buf_ops) {
		dma_buf_put(dmabuf);
		return -EINVAL;
	}
	buf = dmabuf->priv;

	if (req->op != CVM_XCHECK_WRITE && req->op != CVM_XCHECK_VERIFY) {
		ret = -EINVAL;
		goto out;
	}
	/* 只按 8 字节粒度操作，省掉边界处理 */
	if ((req->offset | req->len) & 7 || !req->len) {
		ret = -EINVAL;
		goto out;
	}
	if (req->offset > buf->size || req->len > buf->size - req->offset) {
		ret = -EINVAL;
		goto out;
	}

	req->bad_off = ~0ULL;
	off = req->offset;
	end = req->offset + req->len;

	/* 定位到起始块 */
	for (i = 0; i < buf->nr_chunks && off < end; i++) {
		u64 chunk_len = PAGE_SIZE << buf->chunks[i].order;
		u64 chunk_start = 0, k;
		u64 *kaddr;

		/* 累加出本块在整个 buffer 中的起始偏移 */
		{
			unsigned int j;
			for (j = 0; j < i; j++)
				chunk_start += PAGE_SIZE << buf->chunks[j].order;
		}

		if (off >= chunk_start + chunk_len)
			continue;

		kaddr = (u64 *)page_address(buf->chunks[i].page);

		for (k = off - chunk_start;
		     k < chunk_len && chunk_start + k < end; k += 8) {
			u64 abs = chunk_start + k;
			u64 want = cvm_pattern(abs, req->seed);
			u64 *slot = kaddr + (k / 8);

			if (req->op == CVM_XCHECK_WRITE) {
				*slot = want;
			} else {
				u64 got = *slot;

				if (got != want) {
					req->bad_off = abs;
					req->got = got;
					req->want = want;
					goto out;
				}
			}
		}

		off = chunk_start + chunk_len;
	}

	/* 写入后要确保对其它映射可见 */
	if (req->op == CVM_XCHECK_WRITE)
		wmb();

out:
	dma_buf_put(dmabuf);
	return ret;
}

/*
 * 按虚拟地址反查归属，返回一个新的 dma-buf fd 与区内偏移。
 *
 * 新 fd 会让 dma-buf 引用计数加一，调用方用完必须 close，
 * 否则缓冲区永远不会转回私有。
 */
static int cvm_do_lookup(struct cvm_shared_lookup *req, struct dma_buf **out)
{
	struct cvm_mapping *m;
	struct cvm_buffer *buf = NULL;
	unsigned long va = (unsigned long)req->va;
	unsigned long start = 0;
	int fd;

	req->fd = -1;

	mutex_lock(&cvm_mappings_lock);
	list_for_each_entry(m, &cvm_mappings, node) {
		if (m->mm == current->mm && va >= m->start && va < m->end) {
			buf = m->buf;
			start = m->start;
			break;
		}
	}
	mutex_unlock(&cvm_mappings_lock);

	if (buf == NULL)
		return -ENOENT;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;
	get_dma_buf(buf->dmabuf);   /* 新 fd 会持有一份引用 */

	req->fd = fd;
	*out = buf->dmabuf;
	req->offset = va - start;
	req->len = buf->size;
	return 0;
}

static long cvm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;

	/*
	 * 制造共享内存等于在 guest 的机密边界上开口子，不应该让任意用户
	 * 都能做。这里要求 CAP_SYS_ADMIN，同时设备节点本身建议 0600。
	 * （dpdk-test-main 的 /proc/setshared 是 0777，任何用户可写。）
	 */
	/*
	 * LOOKUP 只查询调用者自己的映射，不制造共享内存，
	 * 因此不要求 CAP_SYS_ADMIN。
	 */
	if (cmd != CVM_SHARED_IOC_LOOKUP && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	switch (cmd) {
	case CVM_SHARED_IOC_ALLOC: {
		struct cvm_shared_alloc req;
		struct dma_buf *dmabuf = NULL;
		int ret;

		if (copy_from_user(&req, uarg, sizeof(req)))
			return -EFAULT;
		ret = cvm_do_alloc(&req, &dmabuf);
		if (ret)
			return ret;
		if (copy_to_user(uarg, &req, sizeof(req))) {
			/* 回滚：fd 还没 install，可以干净地还回去 */
			put_unused_fd(req.fd);
			dma_buf_put(dmabuf);
			return -EFAULT;
		}
		/* 确认用户态已拿到 fd 编号，此时才真正装入 fd 表 */
		fd_install(req.fd, dmabuf->file);
		return 0;
	}
	case CVM_SHARED_IOC_LAYOUT: {
		struct cvm_shared_layout req;
		int ret;

		if (copy_from_user(&req, uarg, sizeof(req)))
			return -EFAULT;
		ret = cvm_do_layout(&req);
		if (ret)
			return ret;
		if (copy_to_user(uarg, &req, sizeof(req)))
			return -EFAULT;
		return 0;
	}
	case CVM_SHARED_IOC_LOOKUP: {
		struct cvm_shared_lookup req;
		struct dma_buf *dmabuf = NULL;
		int ret;

		if (copy_from_user(&req, uarg, sizeof(req)))
			return -EFAULT;
		ret = cvm_do_lookup(&req, &dmabuf);
		if (ret)
			return ret;
		if (copy_to_user(uarg, &req, sizeof(req))) {
			/* fd 尚未装入 fd 表，可以干净回滚 */
			put_unused_fd(req.fd);
			dma_buf_put(dmabuf);
			return -EFAULT;
		}
		fd_install(req.fd, dmabuf->file);
		return 0;
	}
	case CVM_SHARED_IOC_XCHECK: {
		struct cvm_shared_xcheck req;
		int ret;

		if (copy_from_user(&req, uarg, sizeof(req)))
			return -EFAULT;
		ret = cvm_do_xcheck(&req);
		if (ret)
			return ret;
		if (copy_to_user(uarg, &req, sizeof(req)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations cvm_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = cvm_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
	.llseek         = noop_llseek,
};

static struct miscdevice cvm_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "cvm_shared",
	.fops  = &cvm_fops,
	.mode  = 0600,
};

static int __init cvm_shared_init(void)
{
	int ret;

	ret = misc_register(&cvm_misc);
	if (ret) {
		pr_err("misc_register failed: %d\n", ret);
		return ret;
	}

	pr_info("loaded (%s)\n", CVM_SHARED_DEV);
	return 0;
}

static void __exit cvm_shared_exit(void)
{
	misc_deregister(&cvm_misc);
	pr_info("unloaded\n");
}

module_init(cvm_shared_init);
module_exit(cvm_shared_exit);

/* dma_buf_* 系列自 5.16 起归入 DMA_BUF 命名空间，必须显式导入。
 * 注意 6.13 之后这个宏的参数要加引号。 */
MODULE_IMPORT_NS("DMA_BUF");

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Shared (decrypted) DMA memory exporter for SEV-SNP guests");
