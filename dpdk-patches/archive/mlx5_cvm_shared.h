/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026
 */
/*
 * 机密虚拟机（SEV-SNP）下的共享 DMA 内存。
 *
 * SNP guest 里 guest 内存默认是私有加密页，RMP 拒绝设备对其 DMA：
 * ibv_reg_mr 与 devx_umem_reg 对普通内存和大页一律返回 EIO。
 * 内核网络能工作是因为 DMA API 把数据 bounce 到 swiotlb 的共享池，
 * 但那有单次约 1MB 的映射上限，且与零拷贝相悖。
 *
 * 解法是第三类内存：已转共享（C-bit 清除、RMP 标记 shared）但不归
 * swiotlb 管，由 cvm_shared 内核模块以 dma-buf 导出。本模块在 DPDK
 * 侧管理它，并提供 VA -> (dmabuf fd, 偏移) 查找 —— 注册只能走
 * (fd, 偏移)，因为该内存以 remap_pfn_range 映射、vma 带 VM_PFNMAP，
 * get_user_pages 无法 pin，ibv_reg_mr 这条路是死的。
 */
#ifndef RTE_PMD_MLX5_CVM_SHARED_H_
#define RTE_PMD_MLX5_CVM_SHARED_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <rte_compat.h>

struct rte_device;

#ifdef RTE_EXEC_ENV_LINUX

/**
 * 探测期调用一次，决定本进程的运行模式。
 *
 * 模式一经确定不再改变：cvm_shared 可用即进入 **CVM 严格模式**，
 * 此后所有 DMA 内存只从共享区取，取不到直接报错；不可用则完全走原路。
 * 刻意不做「按调用回退」—— 那会让一部分 DMA 内存落在私有页上，
 * 失败推迟到数据面，表现为丢包或卡死而非启动期的明确错误。
 *
 * @param size 共享区大小，0 表示用默认值。
 * @return 0 成功（含「非 CVM，走原路」），负值为致命错误。
 */
__rte_internal
int mlx5_cvm_shared_init(size_t size);

/** 是否处于 CVM 严格模式。 */
__rte_internal
bool mlx5_cvm_shared_enabled(void);

/**
 * 从共享区分配 DMA 内存。仅在 CVM 严格模式下有意义。
 *
 * @param size  字节数。
 * @param align 对齐要求（DevX 队列通常要求页对齐，必须满足，
 *              否则创建命令会失败）。
 * @return 成功返回地址，失败返回 NULL（严格模式下不回退）。
 */
__rte_internal
void *mlx5_cvm_shared_alloc(size_t size, size_t align);

/** 释放共享区内存。addr 必须来自 mlx5_cvm_shared_alloc。 */
__rte_internal
void mlx5_cvm_shared_free(void *addr);

/**
 * addr 是否落在共享区内。
 *
 * mlx5_free() 必须先用它判断：共享区经 rte_extmem_register 登记后，
 * mlx5_mem_is_rte() 会把它认成 RTE 内存并调用 rte_free()，
 * 在退出或错误路径上直接崩溃。
 */
__rte_internal
bool mlx5_cvm_shared_contains(const void *addr);

/**
 * 查找 addr 所属区域的 dma-buf fd 与区内偏移。
 *
 * MR 注册与 umem 注册都需要它：回调只拿得到虚拟地址，
 * 而 devx_umem_reg_ex 需要 (fd, 偏移)。
 */
__rte_internal
bool mlx5_cvm_shared_lookup(const void *addr, int *fd, size_t *offset);

/**
 * 对共享区做 rte_dev_dma_map，预先建立 MR。
 *
 * 在设备探测之后调用。数据面不能容忍 MR 缓存 miss —— 那会退回
 * 现场注册，而对共享区地址只有 dma-buf 一条路，miss 路径必然失败。
 */
__rte_internal
int mlx5_cvm_shared_dma_map(struct rte_device *dev);

#else /* !RTE_EXEC_ENV_LINUX */

static inline int mlx5_cvm_shared_init(size_t size)
{ (void)size; return 0; }
static inline bool mlx5_cvm_shared_enabled(void) { return false; }
static inline void *mlx5_cvm_shared_alloc(size_t size, size_t align)
{ (void)size; (void)align; return NULL; }
static inline void mlx5_cvm_shared_free(void *addr) { (void)addr; }
static inline bool mlx5_cvm_shared_contains(const void *addr)
{ (void)addr; return false; }
static inline bool mlx5_cvm_shared_lookup(const void *addr, int *fd,
					  size_t *offset)
{ (void)addr; (void)fd; (void)offset; return false; }
static inline int mlx5_cvm_shared_dma_map(struct rte_device *dev)
{ (void)dev; return 0; }

#endif /* RTE_EXEC_ENV_LINUX */

#endif /* RTE_PMD_MLX5_CVM_SHARED_H_ */
