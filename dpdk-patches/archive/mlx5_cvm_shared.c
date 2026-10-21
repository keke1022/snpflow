/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <eal_export.h>
#include <rte_common.h>
#include <rte_dev.h>
#include <rte_eal_paging.h>
#include <rte_errno.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_rwlock.h>

#include "mlx5_common_log.h"
#include "mlx5_cvm_shared.h"

/*
 * 与 cvm_shared 内核模块的接口。此处内联定义，避免给 DPDK 构建
 * 引入外部头文件依赖；两侧必须保持一致。
 */
#define CVM_SHARED_DEV "/dev/cvm_shared"

struct cvm_shared_alloc_req {
	uint64_t size;
	uint32_t flags;
	int32_t fd;
};

#define CVM_SHARED_IOC_MAGIC 'C'
#define CVM_SHARED_IOC_ALLOC \
	_IOWR(CVM_SHARED_IOC_MAGIC, 0x01, struct cvm_shared_alloc_req)

/** 默认共享区大小。队列、驱动杂项内存与 mbuf 数据区都从这里出。 */
#define CVM_DEFAULT_REGION_SIZE (512ull << 20)

/** 子分配的最小粒度与记账块。 */
struct cvm_chunk {
	void *addr;
	size_t size;
	bool used;
	struct cvm_chunk *next;
};

static struct {
	bool inited;
	bool enabled;          /* CVM 严格模式 */
	void *base;
	size_t len;
	int fd;
	struct cvm_chunk *chunks;
	rte_rwlock_t lock;
} cvm = { .fd = -1, .lock = RTE_RWLOCK_INITIALIZER };

/* ------------------------------------------------------------------ */

static int
cvm_region_create(size_t size)
{
	struct cvm_shared_alloc_req req = { .size = size, .flags = 0, .fd = -1 };
	size_t page_sz = rte_mem_page_size();
	rte_iova_t *iovas = NULL;
	struct cvm_chunk *first;
	unsigned int n_pages;
	void *va;
	int dev, ret;

	dev = open(CVM_SHARED_DEV, O_RDWR);
	if (dev < 0)
		return -errno;
	if (ioctl(dev, CVM_SHARED_IOC_ALLOC, &req) < 0) {
		ret = -errno;
		close(dev);
		DRV_LOG(ERR, "cvm_shared 分配 %zu 字节失败: %s",
			size, strerror(-ret));
		return ret;
	}
	close(dev);

	va = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, req.fd, 0);
	if (va == MAP_FAILED) {
		ret = -errno;
		close(req.fd);
		DRV_LOG(ERR, "映射共享区失败: %s", strerror(-ret));
		return ret;
	}

	/*
	 * 必须把共享区登记进 DPDK 的内存表，否则 mlx5_mr_create() 里的
	 * rte_mem_virt2memseg_list() 查不到这块内存。
	 *
	 * iova_addrs 不能传 NULL —— 那会把 IOVA 置成 RTE_BAD_IOVA。
	 * 这里显式填 IOVA == VA，与 mkey 用虚拟地址作 start_addr 的
	 * 选择一致，也要求 EAL 以 --iova-mode=va 运行。
	 */
	n_pages = size / page_sz;
	iovas = malloc(sizeof(*iovas) * n_pages);
	if (iovas == NULL) {
		munmap(va, size);
		close(req.fd);
		return -ENOMEM;
	}
	for (unsigned int i = 0; i < n_pages; i++)
		iovas[i] = (rte_iova_t)(uintptr_t)va + (rte_iova_t)i * page_sz;

	ret = rte_extmem_register(va, size, iovas, n_pages, page_sz);
	free(iovas);
	if (ret != 0) {
		DRV_LOG(ERR, "rte_extmem_register 失败: %s", rte_strerror(rte_errno));
		munmap(va, size);
		close(req.fd);
		return -rte_errno;
	}

	first = rte_zmalloc("cvm_chunk", sizeof(*first), 0);
	if (first == NULL) {
		rte_extmem_unregister(va, size);
		munmap(va, size);
		close(req.fd);
		return -ENOMEM;
	}
	first->addr = va;
	first->size = size;
	first->used = false;

	cvm.base = va;
	cvm.len = size;
	cvm.fd = req.fd;
	cvm.chunks = first;
	DRV_LOG(INFO, "共享区就绪: va=%p len=%zu fd=%d（已 extmem_register，IOVA=VA）",
		va, size, req.fd);
	return 0;
}

RTE_EXPORT_INTERNAL_SYMBOL(mlx5_cvm_shared_init)
int
mlx5_cvm_shared_init(size_t size)
{
	int fd, ret;

	if (cvm.inited)
		return 0;
	cvm.inited = true;

	/*
	 * 模式在此处确定一次，之后不再改变。
	 */
	fd = open(CVM_SHARED_DEV, O_RDWR);
	if (fd < 0) {
		DRV_LOG(DEBUG, "未检测到 %s，按非 CVM 环境运行", CVM_SHARED_DEV);
		cvm.enabled = false;
		return 0;
	}
	close(fd);

	ret = cvm_region_create(size ? size : CVM_DEFAULT_REGION_SIZE);
	if (ret != 0) {
		/*
		 * 检测到 cvm_shared 却建不起共享区，是致命错误：
		 * 继续用普通内存必然在注册阶段失败，不如就地报错。
		 */
		DRV_LOG(ERR, "检测到 cvm_shared 但共享区创建失败，无法继续");
		return ret;
	}
	cvm.enabled = true;
	DRV_LOG(INFO, "进入 CVM 严格模式：所有 DMA 内存只从共享区分配");
	return 0;
}

RTE_EXPORT_INTERNAL_SYMBOL(mlx5_cvm_shared_enabled)
bool
mlx5_cvm_shared_enabled(void)
{
	return cvm.enabled;
}

/* ------------------------------------------------------------------ */
/* 带对齐的子分配器                                                    */
/* ------------------------------------------------------------------ */

RTE_EXPORT_INTERNAL_SYMBOL(mlx5_cvm_shared_alloc)
void *
mlx5_cvm_shared_alloc(size_t size, size_t align)
{
	struct cvm_chunk *c;
	void *result = NULL;

	if (!cvm.enabled || size == 0)
		return NULL;
	if (align == 0)
		align = RTE_CACHE_LINE_SIZE;

	rte_rwlock_write_lock(&cvm.lock);
	for (c = cvm.chunks; c != NULL; c = c->next) {
		uintptr_t start, aligned;
		size_t lead, need;

		if (c->used)
			continue;
		start = (uintptr_t)c->addr;
		aligned = RTE_ALIGN_CEIL(start, align);
		lead = aligned - start;
		need = lead + size;
		if (c->size < need)
			continue;

		/* 对齐产生的前段留作空闲块 */
		if (lead != 0) {
			struct cvm_chunk *head = rte_zmalloc("cvm_chunk",
							     sizeof(*head), 0);
			if (head == NULL)
				break;
			head->addr = c->addr;
			head->size = lead;
			head->used = false;
			head->next = c;
			/* 把 head 插到 c 之前 */
			if (cvm.chunks == c) {
				cvm.chunks = head;
			} else {
				struct cvm_chunk *p = cvm.chunks;
				while (p->next != c)
					p = p->next;
				p->next = head;
			}
			c->addr = (void *)aligned;
			c->size -= lead;
		}

		/* 尾部多余的切成新的空闲块 */
		if (c->size > size) {
			struct cvm_chunk *tail = rte_zmalloc("cvm_chunk",
							     sizeof(*tail), 0);
			if (tail == NULL)
				break;
			tail->addr = (void *)((uintptr_t)c->addr + size);
			tail->size = c->size - size;
			tail->used = false;
			tail->next = c->next;
			c->next = tail;
			c->size = size;
		}

		c->used = true;
		result = c->addr;
		break;
	}
	rte_rwlock_write_unlock(&cvm.lock);

	if (result == NULL) {
		/*
		 * 严格模式下不回退到普通内存：那只会把失败推迟到
		 * 注册或数据面，变成更难排查的问题。
		 */
		DRV_LOG(ERR,
			"共享区已用尽，无法分配 %zu 字节（对齐 %zu）。"
			"请增大共享区大小。",
			size, align);
	}
	return result;
}

RTE_EXPORT_INTERNAL_SYMBOL(mlx5_cvm_shared_free)
void
mlx5_cvm_shared_free(void *addr)
{
	struct cvm_chunk *c, *prev = NULL;

	if (addr == NULL || !cvm.enabled)
		return;

	rte_rwlock_write_lock(&cvm.lock);
	for (c = cvm.chunks; c != NULL; prev = c, c = c->next) {
		if (c->addr != addr)
			continue;
		c->used = false;
		/* 与后继空闲块合并 */
		while (c->next != NULL && !c->next->used) {
			struct cvm_chunk *n = c->next;

			c->size += n->size;
			c->next = n->next;
			rte_free(n);
		}
		/* 与前驱空闲块合并 */
		if (prev != NULL && !prev->used) {
			prev->size += c->size;
			prev->next = c->next;
			rte_free(c);
		}
		break;
	}
	rte_rwlock_write_unlock(&cvm.lock);
}

RTE_EXPORT_INTERNAL_SYMBOL(mlx5_cvm_shared_contains)
bool
mlx5_cvm_shared_contains(const void *addr)
{
	uintptr_t a = (uintptr_t)addr;
	uintptr_t base = (uintptr_t)cvm.base;

	if (!cvm.enabled || cvm.base == NULL)
		return false;
	return a >= base && a < base + cvm.len;
}

RTE_EXPORT_INTERNAL_SYMBOL(mlx5_cvm_shared_lookup)
bool
mlx5_cvm_shared_lookup(const void *addr, int *fd, size_t *offset)
{
	if (!mlx5_cvm_shared_contains(addr))
		return false;
	if (fd != NULL)
		*fd = cvm.fd;
	if (offset != NULL)
		*offset = (uintptr_t)addr - (uintptr_t)cvm.base;
	return true;
}

RTE_EXPORT_INTERNAL_SYMBOL(mlx5_cvm_shared_dma_map)
int
mlx5_cvm_shared_dma_map(struct rte_device *dev)
{
	int ret;

	if (!cvm.enabled)
		return 0;
	/*
	 * 预先建立 MR。数据面不能容忍 MR 缓存 miss —— miss 会退回现场
	 * 注册，而共享区地址只有 dma-buf 一条路可走，那条退路是死的，
	 * 且失败发生在收发包途中，表现为丢包或卡死。
	 */
	ret = rte_dev_dma_map(dev, cvm.base, (uint64_t)(uintptr_t)cvm.base,
			      cvm.len);
	if (ret != 0) {
		DRV_LOG(ERR, "共享区 rte_dev_dma_map 失败: %s",
			rte_strerror(rte_errno));
		return ret;
	}
	DRV_LOG(INFO, "共享区 MR 已预填充（%zu 字节）", cvm.len);
	return 0;
}
