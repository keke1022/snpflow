"""
第 3 步：接入共享区分配器。

1. mlx5_malloc.c：新增 mlx5_malloc_dma()，并修正 mlx5_free()
   —— 共享区经 rte_extmem_register 登记后 mlx5_mem_is_rte() 会把它
   认成 RTE 内存并 rte_free()，退出/错误路径必崩，必须先判断。
2. linux/mlx5_common_os.h：mlx5_os_umem_reg 对共享区地址改走
   devx_umem_reg_ex(fd, 偏移)。改这一处覆盖 6 个调用点，
   且 DevX 创建命令里的 umem_offset 保持 0，命令结构不变。
3. 按清单替换各 DMA 分配点。
"""
import os
import re

BASE = os.path.expanduser('~/dpdk-stable-25.11.3/drivers')
os.chdir(BASE)

# ================================================================== #
# 1. common/mlx5/mlx5_malloc.c : mlx5_malloc_dma + mlx5_free 修正
# ================================================================== #
p = 'common/mlx5/mlx5_malloc.c'
s = open(p).read()

inc = '#include "mlx5_malloc.h"'
assert inc in s, 'mlx5_malloc.c include 锚点未匹配'
if 'mlx5_cvm_shared.h' not in s:
    s = s.replace(inc, inc + '\n#include "mlx5_cvm_shared.h"', 1)

# --- mlx5_free：共享区优先判断 ---
old_free = '''	if (addr == NULL)
		return;
	if (!mlx5_mem_is_rte(addr)) {'''
new_free = '''	if (addr == NULL)
		return;
	/*
	 * 必须先判断共享区。共享区经 rte_extmem_register 登记之后，
	 * mlx5_mem_is_rte() 会把它认作 RTE 内存并调用 rte_free()，
	 * 在退出或错误路径上直接崩溃。
	 */
	if (mlx5_cvm_shared_contains(addr)) {
		mlx5_cvm_shared_free(addr);
		return;
	}
	if (!mlx5_mem_is_rte(addr)) {'''
assert old_free in s, 'mlx5_free 函数体未匹配'
s = s.replace(old_free, new_free, 1)

# --- 新增 mlx5_malloc_dma ---
anchor = 'RTE_EXPORT_INTERNAL_SYMBOL(mlx5_free)'
dma_alloc = '''/**
 * 分配设备可直接 DMA 访问的内存。
 *
 * 机密虚拟机（SEV-SNP）里普通内存是私有加密页，设备无法访问，
 * 注册阶段会返回 EIO。CVM 严格模式下本函数从共享区分配；
 * 非 CVM 环境完全退回 mlx5_malloc，行为不变。
 *
 * 刻意不做「分配失败就退回普通内存」：那会让一部分 DMA 内存落在
 * 私有页上，失败推迟到数据面，表现为丢包或卡死，而不是启动期的
 * 明确错误。共享区用尽就是配置问题，应当就地暴露。
 *
 * 释放仍用 mlx5_free()，它会自行识别共享区地址。
 *
 * @param flags 同 mlx5_malloc，非 CVM 路径下透传。
 * @param size  字节数。
 * @param align 对齐要求；DevX 队列通常要求页对齐，必须满足。
 * @param socket NUMA socket，非 CVM 路径下透传。
 */
RTE_EXPORT_INTERNAL_SYMBOL(mlx5_malloc_dma)
void *
mlx5_malloc_dma(uint32_t flags, size_t size, unsigned int align, int socket)
{
	if (mlx5_cvm_shared_enabled()) {
		void *addr = mlx5_cvm_shared_alloc(size, align);

		if (addr != NULL && (flags & MLX5_MEM_ZERO))
			memset(addr, 0, size);
		return addr;
	}
	return mlx5_malloc(flags, size, align, socket);
}

'''
assert anchor in s, 'mlx5_free 导出锚点未匹配'
s = s.replace(anchor, dma_alloc + anchor, 1)
open(p, 'w').write(s)
print('mlx5_malloc.c: 新增 mlx5_malloc_dma()，mlx5_free() 先判共享区')

# --- 头文件声明 ---
p = 'common/mlx5/mlx5_malloc.h'
s = open(p).read()
if 'mlx5_malloc_dma' not in s:
    anchor = 'void *mlx5_malloc(uint32_t flags, size_t size, unsigned int align, int socket);'
    assert anchor in s, 'mlx5_malloc.h 声明锚点未匹配'
    s = s.replace(anchor, anchor + '''

/**
 * 分配设备可 DMA 访问的内存（机密虚拟机下来自共享区）。
 * 释放用 mlx5_free()。
 */
__rte_internal
void *mlx5_malloc_dma(uint32_t flags, size_t size, unsigned int align,
		      int socket);''', 1)
    open(p, 'w').write(s)
    print('mlx5_malloc.h: 已声明 mlx5_malloc_dma')

# ================================================================== #
# 2. linux/mlx5_common_os.h : umem_reg 改道
# ================================================================== #
p = 'common/mlx5/linux/mlx5_common_os.h'
s = open(p).read()
old = '''__rte_internal
static inline void *
mlx5_os_umem_reg(void *ctx, void *addr, size_t size, uint32_t access)
{
	return mlx5_glue->devx_umem_reg(ctx, addr, size, access);
}'''
new = '''__rte_internal
static inline void *
mlx5_os_umem_reg(void *ctx, void *addr, size_t size, uint32_t access)
{
	int fd;
	size_t offset;

	/*
	 * 共享区内存只能以 dma-buf 注册：它由 remap_pfn_range 映射，
	 * vma 带 VM_PFNMAP，get_user_pages 无法 pin，按虚拟地址注册
	 * 必然失败。
	 *
	 * 每次分配单独注册一个 umem，其起点即本次分配的地址，因此
	 * DevX 创建命令里的 umem_offset 仍然保持 0，命令结构不变。
	 */
	if (mlx5_cvm_shared_lookup(addr, &fd, &offset)) {
		struct mlx5dv_devx_umem_in umem_in;

		memset(&umem_in, 0, sizeof(umem_in));
		umem_in.addr = (void *)(uintptr_t)offset;
		umem_in.size = size;
		umem_in.access = access;
		/* pgsz_bitmap 为 0 会得到 EINVAL，易误判成不支持 dma-buf */
		umem_in.pgsz_bitmap = ~0ULL;
		umem_in.comp_mask = MLX5DV_UMEM_MASK_DMABUF;
		umem_in.dmabuf_fd = fd;
		return mlx5_glue->devx_umem_reg_ex(ctx, &umem_in);
	}
	return mlx5_glue->devx_umem_reg(ctx, addr, size, access);
}'''
assert old in s, 'mlx5_os_umem_reg 未匹配'
s = s.replace(old, new, 1)
if 'mlx5_cvm_shared.h' not in s:
    inc = '#include "mlx5_autoconf.h"'
    if inc in s:
        s = s.replace(inc, inc + '\n#include "mlx5_cvm_shared.h"', 1)
    else:
        s = s.replace('#include <mlx5_glue.h>', '#include <mlx5_glue.h>\n#include "mlx5_cvm_shared.h"', 1)
open(p, 'w').write(s)
print('linux/mlx5_common_os.h: mlx5_os_umem_reg 对共享区改走 dma-buf')

# ================================================================== #
# 3. 按清单替换各 DMA 分配点
# ================================================================== #
sites = [
    # (文件, 旧片段, 新片段, 说明)
    ('common/mlx5/mlx5_common_devx.c',
     'umem_buf = mlx5_malloc_numa_tolerant(MLX5_MEM_RTE | MLX5_MEM_ZERO, umem_size,\n\t\t\t\t\t     alignment, socket);',
     'umem_buf = mlx5_malloc_dma(MLX5_MEM_RTE | MLX5_MEM_ZERO, umem_size,\n\t\t\t\t\t     alignment, socket);',
     'DevX CQ/RQ 队列缓冲区（含 doorbell record）'),
    ('common/mlx5/mlx5_common_devx.c',
     'umem_buf = mlx5_malloc(MLX5_MEM_RTE | MLX5_MEM_ZERO, umem_size,\n\t\t\t       alignment, socket);',
     'umem_buf = mlx5_malloc_dma(MLX5_MEM_RTE | MLX5_MEM_ZERO, umem_size,\n\t\t\t       alignment, socket);',
     'DevX SQ/QP 队列缓冲区（含 doorbell record）'),
    ('net/mlx5/mlx5_flow_aso.c',
     'mr->addr = mlx5_malloc(MLX5_MEM_RTE | MLX5_MEM_ZERO, length, 4096,',
     'mr->addr = mlx5_malloc_dma(MLX5_MEM_RTE | MLX5_MEM_ZERO, length, 4096,',
     'ASO 位图'),
    ('net/mlx5/mlx5_hws_cnt.c',
     'mng->raw = mlx5_malloc(MLX5_MEM_ANY | MLX5_MEM_ZERO, sz, pgsz,',
     'mng->raw = mlx5_malloc_dma(MLX5_MEM_ANY | MLX5_MEM_ZERO, sz, pgsz,',
     'HWS 计数器 raw'),
]

for f, old, new, desc in sites:
    s = open(f).read()
    n = s.count(old)
    if n == 0:
        print('  [跳过] %-42s 片段未匹配 (%s)' % (f, desc))
        continue
    s = s.replace(old, new)
    open(f, 'w').write(s)
    print('  [改道] %-42s x%d  %s' % (f, n, desc))

# mlx5_flow_quota.c 的分配语句需要单独看（跨行且参数不同）
p = 'net/mlx5/mlx5_flow_quota.c'
s = open(p).read()
m = re.search(r'buf = mlx5_malloc\(MLX5_MEM_ANY \| MLX5_MEM_ZERO, rd_buf_size,[^;]*;', s)
if m:
    s = s[:m.start()] + m.group(0).replace('mlx5_malloc(', 'mlx5_malloc_dma(', 1) + s[m.end():]
    open(p, 'w').write(s)
    print('  [改道] %-42s x1  MTR ASO 读缓冲' % p)
else:
    print('  [跳过] %-42s 未匹配' % p)
