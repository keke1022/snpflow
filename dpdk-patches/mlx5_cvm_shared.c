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

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_rwlock.h>

#include "mlx5_common.h"
#include "mlx5_cvm_shared.h"

/*
 * 与内核模块 cvm_shared 的接口。此处内联定义而不引用内核头文件，
 * 避免给 DPDK 构建引入外部依赖；两边必须保持一致。
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

/** 一块已映射的共享区域。 */
struct mlx5_cvm_region {
	void *va;                     /* mmap 起始地址 */
	size_t len;                   /* 长度 */
	int fd;                       /* dma-buf fd，注册时要用 */
	struct mlx5_cvm_region *next;
};

static struct mlx5_cvm_region *cvm_regions;
static rte_rwlock_t cvm_lock = RTE_RWLOCK_INITIALIZER;

RTE_EXPORT_INTERNAL_SYMBOL(mlx5_cvm_shared_available)
bool
mlx5_cvm_shared_available(void)
{
	static int cached = -1;
	int fd;

	if (cached >= 0)
		return cached != 0;

	fd = open(CVM_SHARED_DEV, O_RDWR);
	if (fd < 0) {
		DRV_LOG(DEBUG, "cvm_shared 不可用: %s (%s)",
			CVM_SHARED_DEV, strerror(errno));
		cached = 0;
	} else {
		close(fd);
		cached = 1;
	}
	return cached != 0;
}

RTE_EXPORT_INTERNAL_SYMBOL(mlx5_cvm_shared_alloc)
int
mlx5_cvm_shared_alloc(size_t size, void **va_out)
{
	struct cvm_shared_alloc_req req = { .size = size, .flags = 0, .fd = -1 };
	struct mlx5_cvm_region *reg;
	void *va;
	int dev, ret = 0;

	*va_out = NULL;

	dev = open(CVM_SHARED_DEV, O_RDWR);
	if (dev < 0) {
		DRV_LOG(ERR, "打开 %s 失败: %s", CVM_SHARED_DEV, strerror(errno));
		return -errno;
	}
	if (ioctl(dev, CVM_SHARED_IOC_ALLOC, &req) < 0) {
		ret = -errno;
		DRV_LOG(ERR, "cvm_shared 分配 %zu 字节失败: %s",
			size, strerror(errno));
		close(dev);
		return ret;
	}
	close(dev);

	va = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, req.fd, 0);
	if (va == MAP_FAILED) {
		ret = -errno;
		DRV_LOG(ERR, "映射共享区失败: %s", strerror(errno));
		close(req.fd);
		return ret;
	}

	reg = rte_zmalloc("mlx5_cvm_region", sizeof(*reg), 0);
	if (reg == NULL) {
		munmap(va, size);
		close(req.fd);
		return -ENOMEM;
	}
	reg->va = va;
	reg->len = size;
	reg->fd = req.fd;

	rte_rwlock_write_lock(&cvm_lock);
	reg->next = cvm_regions;
	cvm_regions = reg;
	rte_rwlock_write_unlock(&cvm_lock);

	DRV_LOG(INFO, "共享区已就绪: va=%p len=%zu fd=%d", va, size, req.fd);
	*va_out = va;
	return 0;
}

RTE_EXPORT_INTERNAL_SYMBOL(mlx5_cvm_shared_free)
void
mlx5_cvm_shared_free(void *va)
{
	struct mlx5_cvm_region *reg, **pp;

	rte_rwlock_write_lock(&cvm_lock);
	for (pp = &cvm_regions; (reg = *pp) != NULL; pp = &reg->next) {
		if (reg->va == va) {
			*pp = reg->next;
			break;
		}
	}
	rte_rwlock_write_unlock(&cvm_lock);

	if (reg == NULL) {
		DRV_LOG(ERR, "释放未知的共享区 %p", va);
		return;
	}

	/*
	 * 顺序无关紧要：dma-buf 的引用计数要等 munmap 之后才归零，
	 * 内核模块此时才把页转回私有。close(fd) 不会提前触发。
	 */
	munmap(reg->va, reg->len);
	close(reg->fd);
	rte_free(reg);
}

RTE_EXPORT_INTERNAL_SYMBOL(mlx5_cvm_shared_lookup)
bool
mlx5_cvm_shared_lookup(const void *addr, int *fd, size_t *offset,
		       void **base, size_t *len)
{
	const uintptr_t a = (uintptr_t)addr;
	struct mlx5_cvm_region *reg;
	bool found = false;

	rte_rwlock_read_lock(&cvm_lock);
	for (reg = cvm_regions; reg != NULL; reg = reg->next) {
		uintptr_t start = (uintptr_t)reg->va;

		if (a >= start && a < start + reg->len) {
			if (fd != NULL)
				*fd = reg->fd;
			if (offset != NULL)
				*offset = a - start;
			if (base != NULL)
				*base = reg->va;
			if (len != NULL)
				*len = reg->len;
			found = true;
			break;
		}
	}
	rte_rwlock_read_unlock(&cvm_lock);
	return found;
}

RTE_EXPORT_INTERNAL_SYMBOL(mlx5_cvm_shared_contains)
bool
mlx5_cvm_shared_contains(const void *addr)
{
	return mlx5_cvm_shared_lookup(addr, NULL, NULL, NULL, NULL);
}
