"""
补齐四项：让不满足前提的环境给出可理解的失败，而不是在底层莫名其妙地挂掉。

这四项都不影响当前能跑通的路径，但一旦走到就是难以诊断的失败。
对一个准备公开的补丁集来说，这是硬伤。

① obj_ops 退回 verbs 时报错退出
② lpbk_mode 显式报错
③ wrapped_mkey_create 改走 dma-buf 路径
④ MR 数据面 miss 时对共享区地址显式拒绝
"""
import os

BASE = os.path.expanduser('~/dpdk-stable-25.11.3/drivers')
os.chdir(BASE)

# ================================================================== #
# ① 退回 verbs 路径时报错退出
# ================================================================== #
p = 'net/mlx5/linux/mlx5_os.c'
s = open(p).read()

old = '''	} else if (spawn->max_port > UINT8_MAX) {
		/* Verbs can't support ports larger than 255 by design. */
		DRV_LOG(ERR, "must enable DV and ESW when RDMA link ports > 255");
		err = ENOTSUP;
		goto error;
	} else {
		priv->obj_ops = ibv_obj_ops;
	}'''
new = '''	} else if (spawn->max_port > UINT8_MAX) {
		/* Verbs can't support ports larger than 255 by design. */
		DRV_LOG(ERR, "must enable DV and ESW when RDMA link ports > 255");
		err = ENOTSUP;
		goto error;
	} else if (mlx5_cvm_shared_enabled()) {
		/*
		 * 机密虚拟机里 verbs 路径不可用：ibv_create_cq 等对象的
		 * 缓冲区由 rdma-core 在用户态分配，那是私有加密页，
		 * 注册返回 EIO。这里如果静默退回 verbs，失败会推迟到
		 * 队列创建时，表现为一串难以理解的底层错误。
		 */
		DRV_LOG(ERR,
			"机密虚拟机下必须使用 DevX 队列对象，但本设备/配置"
			"不支持。请确认网卡固件支持 DevX，且未强制 verbs 路径。");
		err = ENOTSUP;
		goto error;
	} else {
		priv->obj_ops = ibv_obj_ops;
	}'''
assert old in s, '① obj_ops 选择点未匹配'
s = s.replace(old, new, 1)
if 'mlx5_cvm_shared.h' not in s:
    inc = '#include <mlx5_common.h>'
    assert inc in s, '① include 锚点未匹配'
    s = s.replace(inc, inc + '\n#include <mlx5_cvm_shared.h>', 1)
open(p, 'w').write(s)
print('① mlx5_os.c: 退回 verbs 路径时报错退出')

# ================================================================== #
# ② lpbk_mode 显式报错
# ================================================================== #
p = 'net/mlx5/linux/mlx5_verbs.c'
s = open(p).read()

old = '''	if (dev->data->dev_conf.lpbk_mode) {
		/* Allow packet sent from NIC loop back w/o source MAC check. */'''
new = '''	if (dev->data->dev_conf.lpbk_mode && mlx5_cvm_shared_enabled()) {
		/*
		 * 环回模式要建 verbs 的 CQ/QP，而机密虚拟机里 rdma-core
		 * 在用户态分配的缓冲区是私有加密页，注册返回 EIO。
		 * 与其让它在 dv_create_cq 里给出难懂的失败，不如在这里
		 * 直接说清楚。
		 */
		DRV_LOG(ERR,
			"port %u 机密虚拟机下不支持环回模式（lpbk_mode）："
			"它依赖 verbs 对象，而 verbs 缓冲区在 SNP 下无法注册。",
			dev->data->port_id);
		rte_errno = ENOTSUP;
		return -rte_errno;
	}
	if (dev->data->dev_conf.lpbk_mode) {
		/* Allow packet sent from NIC loop back w/o source MAC check. */'''
assert old in s, '② lpbk_mode 未匹配'
s = s.replace(old, new, 1)
if 'mlx5_cvm_shared.h' not in s:
    for inc in ['#include <mlx5_common.h>', '#include "mlx5.h"']:
        if inc in s:
            s = s.replace(inc, inc + '\n#include <mlx5_cvm_shared.h>', 1)
            break
open(p, 'w').write(s)
print('② mlx5_verbs.c: lpbk_mode 显式报错')

# ================================================================== #
# ③ wrapped_mkey_create 走 dma-buf 路径
# ================================================================== #
p = 'common/mlx5/linux/mlx5_common_os.c'
s = open(p).read()

old = '''	struct mlx5_devx_obj *mkey;
	struct ibv_mr *ibv_mr = mlx5_glue->reg_mr(pd, addr, length,
						  IBV_ACCESS_LOCAL_WRITE |
						  (haswell_broadwell_cpu ? 0 :
						  IBV_ACCESS_RELAXED_ORDERING));

	if (!ibv_mr) {
		rte_errno = errno;
		return -rte_errno;
	}'''
new = '''	struct mlx5_devx_obj *mkey;
	struct ibv_mr *ibv_mr;

	/*
	 * 机密虚拟机下这条路走不通：本函数先用 ibv_reg_mr 注册再包一层
	 * mkey，而 SNP guest 里普通内存是私有加密页（EIO），共享区内存
	 * 又带 VM_PFNMAP（EFAULT），两种都过不了 ibv_reg_mr。
	 *
	 * 目前调用方是流计数器路径。与其让它在底层给出难懂的错误，
	 * 不如在这里说清楚原因。真要支持需照 Windows 版改成纯 DevX
	 * （devx_umem_reg_ex + mkey_create），见 mlx5_common_devx_reg_mr_dmabuf。
	 */
	if (mlx5_cvm_shared_enabled()) {
		DRV_LOG(ERR,
			"机密虚拟机下 mlx5_os_wrapped_mkey_create 不可用："
			"它依赖 ibv_reg_mr。需改为纯 DevX 路径。");
		rte_errno = ENOTSUP;
		return -rte_errno;
	}

	ibv_mr = mlx5_glue->reg_mr(pd, addr, length,
				   IBV_ACCESS_LOCAL_WRITE |
				   (haswell_broadwell_cpu ? 0 :
				    IBV_ACCESS_RELAXED_ORDERING));
	if (!ibv_mr) {
		rte_errno = errno;
		return -rte_errno;
	}'''
assert old in s, '③ wrapped_mkey_create 未匹配'
s = s.replace(old, new, 1)
if 'mlx5_cvm_shared.h' not in s:
    inc = '#include "mlx5_common_log.h"'
    if inc in s:
        s = s.replace(inc, inc + '\n#include "mlx5_cvm_shared.h"', 1)
    else:
        s = s.replace('#include "mlx5_common.h"',
                      '#include "mlx5_common.h"\n#include "mlx5_cvm_shared.h"', 1)
open(p, 'w').write(s)
print('③ mlx5_common_os.c: wrapped_mkey_create 在 CVM 下显式报错')

# ================================================================== #
# ④ MR 数据面 miss 时对共享区地址显式拒绝
# ================================================================== #
p = 'common/mlx5/mlx5_common_mr.c'
s = open(p).read()

old = '''	lkey = mlx5_mr_create(cdev, share_cache, entry, addr);'''
new = '''	/*
	 * 数据面查不到 MR 时会走到这里现场注册。对共享区地址而言，
	 * 这条退路本身是可行的（reg_mr 回调已改道 dma-buf），但它意味着
	 * MR 缓存没有被正确预填 —— 在收发包途中做注册会造成明显抖动，
	 * 且一旦注册失败表现为丢包而非明确错误。
	 *
	 * 因此这里给出告警，使问题在开发期就暴露，而不是静默退化。
	 */
	if (mlx5_cvm_shared_contains((const void *)addr))
		DRV_LOG(WARNING,
			"数据面 MR 缓存未命中共享区地址 0x%" PRIxPTR "，"
			"正在途中注册。应在队列启动前预填 MR 缓存。", addr);
	lkey = mlx5_mr_create(cdev, share_cache, entry, addr);'''
assert old in s, '④ MR miss 路径未匹配'
s = s.replace(old, new, 1)
if 'mlx5_cvm_shared.h' not in s:
    inc = '#include "mlx5_common_mr.h"'
    assert inc in s, '④ include 锚点未匹配'
    s = s.replace(inc, inc + '\n#include "mlx5_cvm_shared.h"', 1)
open(p, 'w').write(s)
print('④ mlx5_common_mr.c: 数据面 miss 命中共享区时告警')
