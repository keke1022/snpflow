/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026
 */
/*
 * 机密虚拟机（SEV-SNP）下的共享 DMA 内存管理。
 *
 * 背景
 * ----
 * SNP guest 里 guest 内存默认是私有加密页，RMP 会拒绝设备对其 DMA。
 * 实测：ibv_reg_mr 与 devx_umem_reg 对普通内存和大页一律返回 EIO；
 * 内核网络能工作是因为 DMA API 会把数据 bounce 到 swiotlb 的共享池，
 * 但 swiotlb 单次映射上限约 1MB，且 bounce 与 DPDK 的零拷贝相悖。
 *
 * 解法是引入第三类内存：已转共享（C-bit 清除、RMP 标记 shared）
 * 但不归 swiotlb 管，由 cvm_shared 内核模块以 dma-buf 形式导出。
 *
 * 本模块负责在 DPDK 侧管理这些区域，并提供地址查找 —— 因为注册只能
 * 走 (dmabuf fd, 偏移)，不能走虚拟地址：内核模块用 remap_pfn_range
 * 建立用户映射，vma 带 VM_PFNMAP，get_user_pages 无法 pin，
 * 所以 ibv_reg_mr 这条路是死的。
 */
#ifndef RTE_PMD_MLX5_CVM_SHARED_H_
#define RTE_PMD_MLX5_CVM_SHARED_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** cvm_shared 是否可用（内核模块已加载且可打开）。 */
__rte_internal
bool mlx5_cvm_shared_available(void);

/**
 * 从内核模块申请一块共享内存并映射到本进程。
 *
 * @param size   请求字节数，向上对齐到页。
 * @param[out] va 映射后的起始地址。
 * @return 0 成功，负值为 -errno。
 */
__rte_internal
int mlx5_cvm_shared_alloc(size_t size, void **va);

/**
 * 释放先前申请的区域。
 *
 * 注意生命周期：dma-buf 的引用计数在 munmap **之后**才归零，
 * 内核模块此时才把页转回私有。close(fd) 本身不会提前转回。
 */
__rte_internal
void mlx5_cvm_shared_free(void *va);

/**
 * 地址查找：给定区内任一地址，返回其所属区域的 dma-buf fd 与偏移。
 *
 * MR 注册路径需要它，因为 reg_mr 回调拿到的是虚拟地址，
 * 而 devx_umem_reg_ex 需要 (fd, 偏移)。
 *
 * @param addr        区内任一地址。
 * @param[out] fd     该区域的 dma-buf fd。
 * @param[out] offset addr 相对区域起点的偏移。
 * @param[out] base   区域起始地址，可为 NULL。
 * @param[out] len    区域长度，可为 NULL。
 * @return true 表示 addr 落在某个共享区内。
 */
__rte_internal
bool mlx5_cvm_shared_lookup(const void *addr, int *fd, size_t *offset,
			    void **base, size_t *len);

/**
 * addr 是否落在任一共享区内。
 *
 * MR 缓存的 miss 路径用它做显式拒绝：对共享区地址回退到普通
 * ibv_reg_mr 必然失败，而失败发生在数据面，表现为丢包或卡死，
 * 不是启动期的明确报错。宁可在这里直接报错。
 */
__rte_internal
bool mlx5_cvm_shared_contains(const void *addr);

#endif /* RTE_PMD_MLX5_CVM_SHARED_H_ */
