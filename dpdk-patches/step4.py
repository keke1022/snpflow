"""
第 4 步：走 HWS 路径，绕开 rdma-core 的 DR。

strace 证实：mlx5dv_dr_create_domain 向上报 ENODEV，底层真实错误是
**EIO** —— 与 ibv_reg_mr / devx_umem_reg 对私有内存失败是同一个签名。
rdma-core 的 dr_send.c 用 posix_memalign + devx_umem_reg 建自己的发送环，
那是私有加密页；ENODEV 是清理路径覆盖掉的 errno。

rdma-core 不在补丁范围内，但 DR 域只被 DV 流引擎使用
（sh->rx_domain / sh->tx_domain 只出现在 mlx5_flow_dv.c，
 sh->dr_drop_action 同理）。HWS 用 DPDK 自带的 mlx5dr，
经 mlx5dr_context_open() 建立自己的上下文。

因此：
  1. dv_flow_en=2 时跳过整段 DR 初始化（569~674 行，含 reclaim /
     pop_vlan_action / allow_duplicate_rules，它们都依赖域）
  2. 把 hws/mlx5dr_send.c 的两处 posix_memalign 改到共享区
     —— 清单里的类别 C，是 DPDK 自己的代码
"""
import os

BASE = os.path.expanduser('~/dpdk-stable-25.11.3/drivers/net/mlx5')
os.chdir(BASE)

# ================================================================== #
# 1. linux/mlx5_os.c: dv_flow_en=2 跳过 DR 初始化
# ================================================================== #
p = 'linux/mlx5_os.c'
s = open(p).read()

old = '''#ifdef HAVE_MLX5DV_DR
	void *domain;

	/* Reference counter is zero, we should initialize structures. */
	domain = mlx5_glue->dr_create_domain(sh->cdev->ctx,'''
new = '''#ifdef HAVE_MLX5DV_DR
	void *domain;

	/*
	 * rdma-core 的 DR 域只被 DV 流引擎使用：sh->rx_domain /
	 * sh->tx_domain / sh->dr_drop_action 都只出现在 mlx5_flow_dv.c。
	 * HWS 走 DPDK 自带的 mlx5dr，用 mlx5dr_context_open() 建自己的
	 * 上下文，不需要这些对象。
	 *
	 * 在机密虚拟机里这一步是必须的：rdma-core 的 dr_send.c 用
	 * posix_memalign + devx_umem_reg 建发送环，那块内存是私有加密页，
	 * 注册返回 EIO（向上被清理路径改写成 ENODEV），整个设备探测失败。
	 *
	 * 注意 25.11 里原有的 "dv_flow_en == 2 -> return 0" 提前返回位于
	 * #ifdef HAVE_IBV_FLOW_DV_SUPPORT 的 #else 分支，当该宏有定义时
	 * 被编译掉，所以这里需要显式判断。
	 */
	if (priv->sh->config.dv_flow_en == 2) {
		DRV_LOG(DEBUG, "dv_flow_en=2（HWS），跳过 rdma-core DR 初始化");
		goto skip_dr_init;
	}

	/* Reference counter is zero, we should initialize structures. */
	domain = mlx5_glue->dr_create_domain(sh->cdev->ctx,'''
assert old in s, 'DR 域创建段未匹配'
s = s.replace(old, new, 1)

old_end = '''	__mlx5_discovery_misc5_cap(priv);
#endif /* HAVE_MLX5DV_DR */'''
new_end = '''	__mlx5_discovery_misc5_cap(priv);
skip_dr_init:
#endif /* HAVE_MLX5DV_DR */'''
assert old_end in s, 'DR 区域结尾未匹配'
s = s.replace(old_end, new_end, 1)
open(p, 'w').write(s)
print('linux/mlx5_os.c: dv_flow_en=2 时跳过整段 DR 初始化')

# ================================================================== #
# 2. hws/mlx5dr_send.c: 两处 posix_memalign 改到共享区
# ================================================================== #
p = 'hws/mlx5dr_send.c'
s = open(p).read()

if 'mlx5_malloc.h' not in s:
    # 找一个已有的 include 作锚点
    for anchor in ['#include "mlx5dr_internal.h"', '#include "mlx5dr_buddy.h"']:
        if anchor in s:
            s = s.replace(anchor, anchor + '\n#include <mlx5_malloc.h>', 1)
            break

old_buf = """	err = posix_memalign((void **)&sq->buf, page_size, buf_aligned);
	if (err) {
		rte_errno = ENOMEM;
		return err;
	}
	memset(sq->buf, 0, buf_aligned);

	err = posix_memalign((void **)&sq->db, 8, 8);
	if (err)
		goto free_buf;"""
new_buf = """	/*
	 * 这两块内存都会被 devx_umem_reg 注册给设备。机密虚拟机里普通内存
	 * 是私有加密页，注册返回 EIO，所以必须来自共享区。
	 * mlx5_malloc_dma 在非 CVM 环境下等价于原先的对齐分配。
	 */
	sq->buf = mlx5_malloc_dma(MLX5_MEM_ZERO, buf_aligned, page_size,
				  SOCKET_ID_ANY);
	if (!sq->buf) {
		rte_errno = ENOMEM;
		return ENOMEM;
	}

	sq->db = mlx5_malloc_dma(MLX5_MEM_ZERO, 8, 8, SOCKET_ID_ANY);
	if (!sq->db) {
		err = ENOMEM;
		goto free_buf;
	}"""
assert old_buf in s, "hws 分配段未匹配"
s = s.replace(old_buf, new_buf, 1)
print("  hws/mlx5dr_send.c: 两处分配已改道")

# 错误路径的 free() 必须换成 mlx5_free()，否则 CVM 下会对共享区调 free()
old_free = """free_db:
	free(sq->db);
free_buf:
	free(sq->buf);"""
new_free = """free_db:
	mlx5_free(sq->db);
free_buf:
	mlx5_free(sq->buf);"""
assert old_free in s, "hws 错误路径未匹配"
s = s.replace(old_free, new_free, 1)
print("  hws/mlx5dr_send.c: 错误路径 free() -> mlx5_free()")

open(p, "w").write(s)
