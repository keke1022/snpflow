/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cvm_shared: 在 SEV-SNP guest 里分配「已转共享、且不经 swiotlb」的内存，
 * 以 dma-buf 形式导出，供设备直接 DMA。
 *
 * 用户态接口（内核/用户态共用此头文件）。
 */
#ifndef _UAPI_CVM_SHARED_H
#define _UAPI_CVM_SHARED_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define CVM_SHARED_DEV "/dev/cvm_shared"

/*
 * 分配一块共享内存并返回 dma-buf fd。
 *
 * size  : 请求字节数，向上对齐到页。
 * flags : 保留，必须为 0。
 * fd    : 出参，dma-buf 文件描述符。
 *
 * 关闭 fd（显式 close 或进程退出时内核自动关闭）会触发引用计数归零，
 * 届时模块把页转回私有并释放。
 */
struct cvm_shared_alloc {
	__u64 size;
	__u32 flags;
	__s32 fd;
};

/*
 * 查询一块已分配缓冲区的物理布局，用于实验期间人工核对
 * 「设备拿到的地址是否等于物理地址」。
 *
 * fd        : 入参，已分配的 dma-buf fd。
 * nr_chunks : 出参，物理连续块的数量。
 * chunks    : 入参，用户提供的数组地址；为 0 时只返回 nr_chunks。
 * max_chunks: 入参，数组容量。
 */
struct cvm_shared_chunk {
	__u64 phys;
	__u64 len;
};

struct cvm_shared_layout {
	__s32 fd;
	__u32 nr_chunks;
	__u64 chunks;
	__u32 max_chunks;
	__u32 pad;
};

/*
 * 跨映射校验：内核通过直接映射访问，用户态通过 mmap 访问，
 * 两边看到的必须是同一份数据。C-bit 只在一侧处理对了的话，这里会露馅。
 *
 * op = CVM_XCHECK_WRITE  : 内核按 seed 生成图案写入直接映射
 * op = CVM_XCHECK_VERIFY : 内核从直接映射读出并与 seed 图案比对
 *
 * 失败时 bad_off 给出第一个不一致的偏移，got/want 给出实际值和期望值。
 */
#define CVM_XCHECK_WRITE  0
#define CVM_XCHECK_VERIFY 1

struct cvm_shared_xcheck {
	__s32 fd;
	__u32 op;
	__u64 offset;
	__u64 len;
	__u64 seed;
	__u64 bad_off;   /* 出参，~0ULL 表示全部一致 */
	__u64 got;       /* 出参 */
	__u64 want;      /* 出参 */
};

#define CVM_SHARED_IOC_MAGIC 'C'
#define CVM_SHARED_IOC_ALLOC  _IOWR(CVM_SHARED_IOC_MAGIC, 0x01, struct cvm_shared_alloc)
#define CVM_SHARED_IOC_LAYOUT _IOWR(CVM_SHARED_IOC_MAGIC, 0x02, struct cvm_shared_layout)
#define CVM_SHARED_IOC_XCHECK _IOWR(CVM_SHARED_IOC_MAGIC, 0x03, struct cvm_shared_xcheck)

#endif /* _UAPI_CVM_SHARED_H */
