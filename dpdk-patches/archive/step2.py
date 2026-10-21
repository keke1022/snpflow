"""
第 2 步：MR 注册改道到 dmabuf + DevX mkey。

Linux 现有的 mlx5_common_verbs_reg_mr 走 ibv_reg_mr（虚拟地址语义），
对 cvm_shared 的内存必然失败：该内存用 remap_pfn_range 映射，
vma 带 VM_PFNMAP，get_user_pages 无法 pin。

做法照搬 Windows 的 mlx5_os_reg_mr（纯 DevX，不碰 verbs）：
    devx_umem_reg_ex(dmabuf_fd, offset)  ->  devx_cmd_mkey_create(umem_id)

mkey 的 start_addr 用虚拟地址而非 0 —— 已实测可行，这样 WQE 里
可以直填 VA，与 DPDK 的 IOVA=VA 模式天然对齐，不需要偏移换算层。
"""
import os

BASE = os.path.expanduser('~/dpdk-stable-25.11.3/drivers/common/mlx5')
os.chdir(BASE)

# ---------------------------------------------------------------- #
# 1. linux/mlx5_common_verbs.c: 增加 DevX/dmabuf 版 reg_mr，并在
#    set_reg_mr_cb 里按 cvm_shared 可用性选择实现
# ---------------------------------------------------------------- #
p = 'linux/mlx5_common_verbs.c'
s = open(p).read()

impl = r'''
/**
 * 以 dma-buf 方式注册内存区（机密虚拟机路径）。
 *
 * SNP guest 里 ibv_reg_mr 对普通内存返回 EIO；cvm_shared 分配的内存
 * 又因 VM_PFNMAP 无法被 get_user_pages pin 住。两者都决定了必须走
 * (dmabuf fd, 偏移) 这条注册路径。
 *
 * 流程与 Windows 版 mlx5_os_reg_mr 一致：先注册 umem，再基于
 * umem_id 建 mkey。区别只在 umem 用 dmabuf fd 而非虚拟地址。
 *
 * @param[in] pd
 *   保护域。
 * @param[in] addr
 *   共享区内的起始地址。
 * @param[in] length
 *   长度。
 * @param[out] pmd_mr
 *   填入 lkey、地址、长度、umem 对象与 mkey 对象。
 *
 * @return
 *   0 成功，-1 失败。
 */
int
mlx5_common_devx_reg_mr_dmabuf(void *pd, void *addr, size_t length,
			       struct mlx5_pmd_mr *pmd_mr)
{
	struct mlx5dv_devx_umem_in umem_in;
	struct mlx5dv_devx_umem *umem;
	struct mlx5_devx_mkey_attr mkey_attr;
	struct mlx5_devx_obj *mkey;
	struct mlx5_pd *mlx5_pd = pd;
	size_t offset = 0;
	int fd = -1;

	if (!mlx5_cvm_shared_lookup(addr, &fd, &offset, NULL, NULL)) {
		DRV_LOG(ERR,
			"地址 %p 不在任何 cvm_shared 区域内，无法以 dma-buf 注册",
			addr);
		return -1;
	}

	memset(&umem_in, 0, sizeof(umem_in));
	umem_in.addr = (void *)(uintptr_t)offset;
	umem_in.size = length;
	umem_in.access = IBV_ACCESS_LOCAL_WRITE |
			 (haswell_broadwell_cpu ? 0 :
			  IBV_ACCESS_RELAXED_ORDERING);
	/*
	 * pgsz_bitmap 必须非 0。置 0 时 rdma-core 返回 EINVAL，
	 * 极易被误判成「不支持 dma-buf」。
	 */
	umem_in.pgsz_bitmap = ~0ULL;
	umem_in.comp_mask = MLX5DV_UMEM_MASK_DMABUF;
	umem_in.dmabuf_fd = fd;

	umem = mlx5_glue->devx_umem_reg_ex(mlx5_pd->devx_ctx, &umem_in);
	if (umem == NULL) {
		DRV_LOG(ERR, "devx_umem_reg_ex 失败 (addr=%p len=%zu fd=%d): %s",
			addr, length, fd, strerror(errno));
		return -1;
	}

	memset(&mkey_attr, 0, sizeof(mkey_attr));
	/*
	 * start_addr 用虚拟地址：这样 WQE 里的数据段可以直填 VA，
	 * 与 DPDK 的 IOVA=VA 模式一致，省掉一层偏移换算。已实测可行。
	 */
	mkey_attr.addr = (uintptr_t)addr;
	mkey_attr.size = length;
	mkey_attr.umem_id = umem->umem_id;
	mkey_attr.pd = mlx5_pd->pdn;
	mkey_attr.pg_access = 1;
	if (!haswell_broadwell_cpu) {
		mkey_attr.relaxed_ordering_write = 1;
		mkey_attr.relaxed_ordering_read = 1;
	}

	mkey = mlx5_devx_cmd_mkey_create(mlx5_pd->devx_ctx, &mkey_attr);
	if (mkey == NULL) {
		DRV_LOG(ERR, "基于 umem_id=%u 创建 mkey 失败", umem->umem_id);
		claim_zero(mlx5_glue->devx_umem_dereg(umem));
		return -1;
	}

	pmd_mr->addr = addr;
	pmd_mr->len = length;
	pmd_mr->obj = (void *)umem;
	pmd_mr->mkey = mkey;
	pmd_mr->lkey = mkey->id;
	DRV_LOG(DEBUG, "dma-buf MR 已注册: addr=%p len=%zu umem_id=%u lkey=0x%x",
		addr, length, umem->umem_id, pmd_mr->lkey);
	return 0;
}

/**
 * 注销以 dma-buf 方式注册的 MR。
 */
void
mlx5_common_devx_dereg_mr_dmabuf(struct mlx5_pmd_mr *pmd_mr)
{
	if (pmd_mr == NULL)
		return;
	if (pmd_mr->mkey != NULL)
		claim_zero(mlx5_devx_cmd_destroy(pmd_mr->mkey));
	if (pmd_mr->obj != NULL)
		claim_zero(mlx5_glue->devx_umem_dereg(pmd_mr->obj));
	memset(pmd_mr, 0, sizeof(*pmd_mr));
}

'''

anchor = '''/**
 * Set the reg_mr and dereg_mr callbacks.'''
assert anchor in s, 'set_reg_mr_cb 注释锚点未匹配'
s = s.replace(anchor, impl + anchor, 1)

old_cb = '''mlx5_os_set_reg_mr_cb(mlx5_reg_mr_t *reg_mr_cb, mlx5_dereg_mr_t *dereg_mr_cb)
{
	*reg_mr_cb = mlx5_common_verbs_reg_mr;
	*dereg_mr_cb = mlx5_common_verbs_dereg_mr;
}'''
new_cb = '''mlx5_os_set_reg_mr_cb(mlx5_reg_mr_t *reg_mr_cb, mlx5_dereg_mr_t *dereg_mr_cb)
{
	/*
	 * 机密虚拟机里 ibv_reg_mr 对任何普通内存都返回 EIO，唯一可行的
	 * 注册路径是 dma-buf + DevX mkey。cvm_shared 可用即切换过去。
	 */
	if (mlx5_cvm_shared_available()) {
		DRV_LOG(INFO,
			"检测到 cvm_shared，MR 注册改用 dma-buf + DevX mkey 路径");
		*reg_mr_cb = mlx5_common_devx_reg_mr_dmabuf;
		*dereg_mr_cb = mlx5_common_devx_dereg_mr_dmabuf;
		return;
	}
	*reg_mr_cb = mlx5_common_verbs_reg_mr;
	*dereg_mr_cb = mlx5_common_verbs_dereg_mr;
}'''
assert old_cb in s, 'set_reg_mr_cb 函数体未匹配'
s = s.replace(old_cb, new_cb, 1)

# 需要的头文件
inc_anchor = '#include "mlx5_common.h"'
if inc_anchor in s and 'mlx5_cvm_shared.h' not in s:
    s = s.replace(inc_anchor, inc_anchor + '\n#include "mlx5_cvm_shared.h"\n#include "mlx5_devx_cmds.h"', 1)
open(p, 'w').write(s)
print('linux/mlx5_common_verbs.c: 已加 dma-buf reg_mr 并接入回调选择')

# ---------------------------------------------------------------- #
# 2. meson.build: 加入新源文件
# ---------------------------------------------------------------- #
p = 'linux/meson.build'
s = open(p).read()
if 'mlx5_cvm_shared.c' not in s:
    old = "sources += files('mlx5_common_auxiliary.c')"
    assert old in s, 'linux/meson.build sources 锚点未匹配'
    s = s.replace(old, "sources += files('mlx5_cvm_shared.c')\n" + old, 1)
    open(p, 'w').write(s)
    print('linux/meson.build: 已加入 mlx5_cvm_shared.c')
