"""
第 5 步：HWS 发送环 CQ 从 verbs 改为 DevX。

mlx5dr_send_ring_open_cq() 原本调 mlx5_glue->create_cq()，即 ibv_create_cq。
rdma-core 在用户态分配 CQ 缓冲区并经 ib_umem_get pin 住再注册 ——
机密虚拟机里那是私有加密页，注册返回 EIO；而共享区内存带 VM_PFNMAP，
get_user_pages 也 pin 不住。两头都走不通，改 rdma-core 无解。

改用 mlx5_devx_cq_create()：它的缓冲区分配点已在前一步改道到共享区
（mlx5_common_devx.c 的类别 B），需要的五个字段都能从
struct mlx5_devx_cq 取到。

**无条件替换，不加 CVM 分支** —— HWS 本来就要求 DevX，
多一条分支只会多一份没人测的代码。
"""
import os

BASE = os.path.expanduser('~/dpdk-stable-25.11.3/drivers/net/mlx5/hws')
os.chdir(BASE)

# ---------------------------------------------------------------- #
# 1. mlx5dr_send.h: ibv_cq 换成 DevX CQ 对象
# ---------------------------------------------------------------- #
p = 'mlx5dr_send.h'
s = open(p).read()
old = '''	uint16_t poll_wqe;
	struct ibv_cq *ibv_cq;
	uint32_t cqn;
	uint32_t cqe_sz;
};'''
new = '''	uint16_t poll_wqe;
	struct mlx5_devx_cq devx_cq; /* DevX CQ：verbs 路径在机密虚拟机下不可用 */
	uint32_t cqn;
	uint32_t cqe_sz;
};'''
assert old in s, 'send_ring_cq 结构未匹配'
s = s.replace(old, new, 1)
if 'mlx5_common_devx.h' not in s:
    for anchor in ['#include "mlx5dr_internal.h"', '#include <rte_atomic.h>',
                   '#ifndef MLX5DR_SEND_H_']:
        if anchor in s:
            if anchor.startswith('#ifndef'):
                # 放到头文件保护之后的第一行
                s = s.replace(anchor, anchor + '\n\n#include <mlx5_common_devx.h>', 1)
            else:
                s = s.replace(anchor, anchor + '\n#include <mlx5_common_devx.h>', 1)
            break
open(p, 'w').write(s)
print('mlx5dr_send.h: send_ring_cq 改持有 struct mlx5_devx_cq')

# ---------------------------------------------------------------- #
# 2. mlx5dr_send.c: open_cq / close_cq 改写
# ---------------------------------------------------------------- #
p = 'mlx5dr_send.c'
s = open(p).read()

old_open = '''	struct mlx5dv_cq mlx5_cq = {0};
	struct mlx5dv_obj obj;
	struct ibv_cq *ibv_cq;
	size_t cq_size;
	int err;

	cq_size = queue->num_entries;
	ibv_cq = mlx5_glue->create_cq(ctx->ibv_ctx, cq_size, NULL, NULL, 0);
	if (!ibv_cq) {
		DR_LOG(ERR, "Failed to create CQ");
		rte_errno = errno;
		return rte_errno;
	}

	obj.cq.in = ibv_cq;
	obj.cq.out = &mlx5_cq;
	err = mlx5_glue->dv_init_obj(&obj, MLX5DV_OBJ_CQ);
	if (err) {
		err = errno;
		goto close_cq;
	}

	cq->buf = mlx5_cq.buf;
	cq->db = mlx5_cq.dbrec;
	cq->ncqe = mlx5_cq.cqe_cnt;
	cq->cqe_sz = mlx5_cq.cqe_size;
	cq->cqe_log_sz = log2above(cq->cqe_sz);
	cq->ncqe_mask = cq->ncqe - 1;
	cq->buf_sz = cq->cqe_sz * cq->ncqe;
	cq->cqn = mlx5_cq.cqn;
	cq->ibv_cq = ibv_cq;

	return 0;

close_cq:
	mlx5_glue->destroy_cq(ibv_cq);
	rte_errno = err;
	return err;
}'''

new_open = '''	struct mlx5_devx_cq_attr cq_attr = {0};
	uint16_t log_cq_size;
	int err;

	/*
	 * 用 DevX 而非 ibv_create_cq 建这个 CQ。
	 *
	 * rdma-core 的 ibv_create_cq 在用户态分配 CQ 缓冲区，再经内核
	 * ib_umem_get pin 住注册。机密虚拟机里普通内存是私有加密页，
	 * 注册返回 EIO；而共享区内存带 VM_PFNMAP，get_user_pages 同样
	 * pin 不住 —— 两条路都不通，改 rdma-core 也解决不了。
	 *
	 * mlx5_devx_cq_create() 的缓冲区走 mlx5_malloc_dma()，
	 * 机密虚拟机下自动来自共享区，普通环境行为不变。
	 * HWS 本来就要求 DevX，因此这里无条件替换，不分情况。
	 */
	log_cq_size = log2above(queue->num_entries);
	cq_attr.uar_page_id = mlx5_os_get_devx_uar_page_id(queue->uar);

	err = mlx5_devx_cq_create(ctx->ibv_ctx, &cq->devx_cq, log_cq_size,
				  &cq_attr, SOCKET_ID_ANY);
	if (err) {
		DR_LOG(ERR, "Failed to create DevX CQ");
		rte_errno = errno ? errno : ENOMEM;
		return rte_errno;
	}

	cq->buf = (uint8_t *)(uintptr_t)cq->devx_cq.umem_buf;
	cq->db = (__be32 *)(uintptr_t)cq->devx_cq.db_rec;
	cq->ncqe = 1u << log_cq_size;
	cq->cqe_sz = sizeof(struct mlx5_cqe);
	cq->cqe_log_sz = log2above(cq->cqe_sz);
	cq->ncqe_mask = cq->ncqe - 1;
	cq->buf_sz = cq->cqe_sz * cq->ncqe;
	cq->cqn = cq->devx_cq.cq->id;

	return 0;
}'''
assert old_open in s, 'open_cq 函数体未匹配'
s = s.replace(old_open, new_open, 1)

old_close = '''static void mlx5dr_send_ring_close_cq(struct mlx5dr_send_ring_cq *cq)
{
	mlx5_glue->destroy_cq(cq->ibv_cq);
}'''
new_close = '''static void mlx5dr_send_ring_close_cq(struct mlx5dr_send_ring_cq *cq)
{
	mlx5_devx_cq_destroy(&cq->devx_cq);
}'''
assert old_close in s, 'close_cq 未匹配'
s = s.replace(old_close, new_close, 1)

if 'mlx5_common_devx.h' not in s:
    for anchor in ['#include "mlx5dr_internal.h"']:
        if anchor in s:
            s = s.replace(anchor, anchor + '\n#include <mlx5_common_devx.h>', 1)
            break
open(p, 'w').write(s)
print('mlx5dr_send.c: open_cq 改用 mlx5_devx_cq_create，close_cq 跟进')

# ---------------------------------------------------------------- #
# 3. 其余引用 ibv_cq 的地方
# ---------------------------------------------------------------- #
import subprocess
out = subprocess.run(['grep', '-rn', 'ibv_cq', '.'], capture_output=True, text=True).stdout
if out.strip():
    print('  仍引用 ibv_cq 的位置：')
    for line in out.strip().split('\n'):
        print('   ', line)
else:
    print('  已无 ibv_cq 引用')
