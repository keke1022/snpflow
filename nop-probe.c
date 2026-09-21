/*
 * nop-probe: 证明网卡真的能 DMA 写入 cvm_shared 的内存。
 *
 * 到目前为止我们只证明了 devx_umem_reg_ex 接受这块内存 —— 注册成功
 * 不等于 DMA 落地。本程序用 DevX 建起一条最小的发送通路：
 *
 *   共享内存（cvm_shared）
 *     ├── CQ 缓冲区   ← 网卡往这里 DMA 写完成通知
 *     ├── SQ 缓冲区   ← 网卡从这里 DMA 读任务单
 *     └── doorbell record ← 网卡从这里 DMA 读队列进度
 *
 * 然后发一条 NOP 任务单（不触网、不需要 mkey、不需要对端），
 * 轮询 CQ 缓冲区看能否读到网卡写回的 CQE。
 *
 * CPU 读到了正确的 CQE == 网卡的 DMA 写确实落进了我们的共享页，
 * 且 CPU 通过 mmap 能看到。这是整个方案的地基证明。
 *
 * 对象创建顺序：
 *   PD(pdn) → UAR → EQN → umem → CQ → 传输域 TD → TIS → SQ → RST2RDY
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "mlx5_prm.h"          /* DPDK 的 PRM 定义，经 prm/ 垫片引入 */
#include "kmod/cvm_shared.h"

/* ---- 共享区布局（偏移都按 4KB 对齐，doorbell 需要 64B 对齐即可）---- */
#define CQ_LOG_SIZE   6                       /* 64 个 CQE */
#define CQ_N          (1u << CQ_LOG_SIZE)
#define CQE_SIZE      64
#define SQ_LOG_SIZE   6                       /* 64 个 WQE BB */
#define SQ_N          (1u << SQ_LOG_SIZE)
#define WQE_BB        64

#define OFF_CQ        0
#define SZ_CQ         (CQ_N * CQE_SIZE)                 /* 4KB */
#define OFF_SQ        0x10000                           /* 64KB */
#define SZ_SQ         (SQ_N * WQE_BB)                   /* 4KB */
#define OFF_DBR       0x20000                           /* 128KB */
#define SZ_DBR        64
#define TOTAL_SIZE    (1u << 20)                        /* 1MB，够用且跨不过一个 2MB 块 */

#define DBR_CQ_OFF    0    /* doorbell record 里 CQ 用前 8 字节 */
#define DBR_SQ_OFF    8    /* SQ 用接下来 8 字节 */

static struct ibv_context *ctx;
static int fails;

static void ok(const char *fmt, ...)
{ va_list a; printf("  [ OK ] "); va_start(a,fmt); vprintf(fmt,a); va_end(a); printf("\n"); }
static void bad(const char *fmt, ...)
{ va_list a; fails++; printf("  [FAIL] "); va_start(a,fmt); vprintf(fmt,a); va_end(a);
  printf(" | errno=%d (%s)\n", errno, strerror(errno)); }
static void info(const char *fmt, ...)
{ va_list a; printf("         "); va_start(a,fmt); vprintf(fmt,a); va_end(a); printf("\n"); }

static int cvm_alloc(size_t len)
{
	struct cvm_shared_alloc req = { .size = len, .flags = 0, .fd = -1 };
	int dev = open(CVM_SHARED_DEV, O_RDWR);

	if (dev < 0)
		return -1;
	if (ioctl(dev, CVM_SHARED_IOC_ALLOC, &req) < 0) { close(dev); return -1; }
	close(dev);
	return req.fd;
}

int main(void)
{
	struct ibv_device **devs;
	struct ibv_pd *pd = NULL;
	struct mlx5dv_devx_uar *uar = NULL;
	struct mlx5dv_devx_umem *umem = NULL;
	struct mlx5dv_devx_obj *cq_obj = NULL, *tis_obj = NULL, *sq_obj = NULL;
	struct mlx5dv_obj dv_obj;
	struct mlx5dv_pd dv_pd;
	uint32_t pdn = 0, eqn = 0, cqn = 0, tisn = 0, sqn = 0, tdn = 0;
	uint8_t *base = NULL;
	volatile uint8_t *cq_buf, *sq_buf;
	volatile uint32_t *dbr;
	int fd = -1, ndev = 0;

	devs = ibv_get_device_list(&ndev);
	if (!devs || ndev < 1) { fprintf(stderr, "无 RDMA 设备\n"); return 1; }
	ctx = ibv_open_device(devs[0]);
	if (!ctx) { perror("ibv_open_device"); return 1; }
	printf("设备: %s\n", ibv_get_device_name(devs[0]));

	/* ---------------- 1. PD / pdn ---------------- */
	printf("\n=== [1] PD / UAR / EQN ===\n");
	pd = ibv_alloc_pd(ctx);
	if (!pd) { bad("ibv_alloc_pd"); goto out; }

	memset(&dv_obj, 0, sizeof(dv_obj));
	memset(&dv_pd, 0, sizeof(dv_pd));
	dv_obj.pd.in = pd;
	dv_obj.pd.out = &dv_pd;
	if (mlx5dv_init_obj(&dv_obj, MLX5DV_OBJ_PD)) { bad("mlx5dv_init_obj(PD)"); goto out; }
	pdn = dv_pd.pdn;
	ok("pdn = %u", pdn);

	uar = mlx5dv_devx_alloc_uar(ctx, MLX5DV_UAR_ALLOC_TYPE_NC);
	if (!uar) uar = mlx5dv_devx_alloc_uar(ctx, 0);
	if (!uar) { bad("alloc_uar"); goto out; }
	ok("uar page_id = %u, reg_addr = %p", uar->page_id, uar->reg_addr);

	if (mlx5dv_devx_query_eqn(ctx, 0, &eqn)) { bad("query_eqn"); goto out; }
	ok("eqn = %u", eqn);

	/* ---------------- 2. 共享内存 ---------------- */
	printf("\n=== [2] 从 cvm_shared 取 %u KB 并注册为 umem ===\n", TOTAL_SIZE / 1024);
	fd = cvm_alloc(TOTAL_SIZE);
	if (fd < 0) { bad("cvm_shared 分配（模块加载了吗？是否 root？）"); goto out; }

	base = mmap(NULL, TOTAL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (base == MAP_FAILED) { bad("mmap"); goto out; }
	memset(base, 0, TOTAL_SIZE);

	cq_buf = base + OFF_CQ;
	sq_buf = base + OFF_SQ;
	dbr    = (volatile uint32_t *)(base + OFF_DBR);

	/*
	 * CQE 的 owner 位约定：初始全置为 0xff，网卡写入时会翻转。
	 * 这样"读到有效 CQE"这件事本身就证明了网卡确实写过这块内存。
	 */
	memset((void *)cq_buf, 0xff, SZ_CQ);

	{
		struct mlx5dv_devx_umem_in in;
		memset(&in, 0, sizeof(in));
		in.addr = (void *)0;
		in.size = TOTAL_SIZE;
		in.access = IBV_ACCESS_LOCAL_WRITE;
		in.pgsz_bitmap = ~0ULL;
		in.comp_mask = MLX5DV_UMEM_MASK_DMABUF;
		in.dmabuf_fd = fd;
		errno = 0;
		umem = mlx5dv_devx_umem_reg_ex(ctx, &in);
	}
	if (!umem) { bad("devx_umem_reg_ex"); goto out; }
	ok("umem_id = %u", umem->umem_id);

	/* ---------------- 3. 创建 CQ ---------------- */
	printf("\n=== [3] DevX 创建 CQ（缓冲区在共享内存里）===\n");
	{
		uint32_t in[MLX5_ST_SZ_DW(create_cq_in)] = {0};
		uint32_t out[MLX5_ST_SZ_DW(create_cq_out)] = {0};
		void *cqc = MLX5_ADDR_OF(create_cq_in, in, cq_context);

		MLX5_SET(create_cq_in, in, opcode, MLX5_CMD_OP_CREATE_CQ);
		MLX5_SET(create_cq_in, in, cq_umem_id, umem->umem_id);
		MLX5_SET64(create_cq_in, in, cq_umem_offset, OFF_CQ);
		MLX5_SET(create_cq_in, in, cq_umem_valid, 1);

		MLX5_SET(cqc, cqc, log_cq_size, CQ_LOG_SIZE);
		MLX5_SET(cqc, cqc, uar_page, uar->page_id);
		MLX5_SET(cqc, cqc, c_eqn, eqn);
		MLX5_SET(cqc, cqc, dbr_umem_id, umem->umem_id);
		MLX5_SET(cqc, cqc, dbr_umem_valid, 1);
		MLX5_SET64(cqc, cqc, dbr_addr, OFF_DBR + DBR_CQ_OFF);
		MLX5_SET(cqc, cqc, cqe_sz, 0);      /* 64 字节 CQE */
		MLX5_SET(cqc, cqc, cc, 0);
		MLX5_SET(cqc, cqc, oi, 0);

		errno = 0;
		cq_obj = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
		if (!cq_obj) {
			bad("CREATE_CQ (syndrome=0x%x)", MLX5_GET(create_cq_out, out, syndrome));
			goto out;
		}
		cqn = MLX5_GET(create_cq_out, out, cqn);
		ok("CQ 创建成功, cqn = %u —— 网卡已接受共享内存作为 CQ 缓冲区", cqn);
	}

	/* ---------------- 4. 传输域 + TIS ---------------- */
	printf("\n=== [4] 传输域 / TIS（发送队列的前置对象）===\n");
	{
		uint32_t in[MLX5_ST_SZ_DW(alloc_transport_domain_in)] = {0};
		uint32_t out[MLX5_ST_SZ_DW(alloc_transport_domain_out)] = {0};
		struct mlx5dv_devx_obj *td;

		MLX5_SET(alloc_transport_domain_in, in, opcode,
			 MLX5_CMD_OP_ALLOC_TRANSPORT_DOMAIN);
		errno = 0;
		td = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
		if (!td) { bad("ALLOC_TRANSPORT_DOMAIN"); goto out; }
		tdn = MLX5_GET(alloc_transport_domain_out, out, transport_domain);
		ok("transport domain = %u", tdn);
		/* td 对象保持存活；进程退出时释放 */
	}
	{
		uint32_t in[MLX5_ST_SZ_DW(create_tis_in)] = {0};
		uint32_t out[MLX5_ST_SZ_DW(create_tis_out)] = {0};
		void *tisc = MLX5_ADDR_OF(create_tis_in, in, ctx);

		MLX5_SET(create_tis_in, in, opcode, MLX5_CMD_OP_CREATE_TIS);
		MLX5_SET(tisc, tisc, transport_domain, tdn);
		errno = 0;
		tis_obj = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
		if (!tis_obj) { bad("CREATE_TIS"); goto out; }
		tisn = MLX5_GET(create_tis_out, out, tisn);
		ok("tisn = %u", tisn);
	}

	/* ---------------- 5. 创建 SQ ---------------- */
	printf("\n=== [5] DevX 创建 SQ（缓冲区同样在共享内存里）===\n");
	{
		uint32_t in[MLX5_ST_SZ_DW(create_sq_in)] = {0};
		uint32_t out[MLX5_ST_SZ_DW(create_sq_out)] = {0};
		void *sqc = MLX5_ADDR_OF(create_sq_in, in, ctx);
		void *wq  = MLX5_ADDR_OF(sqc, sqc, wq);

		MLX5_SET(create_sq_in, in, opcode, MLX5_CMD_OP_CREATE_SQ);
		MLX5_SET(sqc, sqc, cqn, cqn);
		MLX5_SET(sqc, sqc, tis_num_0, tisn);
		MLX5_SET(sqc, sqc, tis_lst_sz, 1);
		MLX5_SET(sqc, sqc, state, MLX5_SQC_STATE_RST);
		MLX5_SET(sqc, sqc, flush_in_error_en, 1);

		MLX5_SET(wq, wq, wq_type, MLX5_WQ_TYPE_CYCLIC);
		MLX5_SET(wq, wq, uar_page, uar->page_id);
		MLX5_SET(wq, wq, log_wq_stride, 6);          /* 64 字节 */
		MLX5_SET(wq, wq, log_wq_sz, SQ_LOG_SIZE);
		MLX5_SET(wq, wq, wq_umem_id, umem->umem_id);
		MLX5_SET64(wq, wq, wq_umem_offset, OFF_SQ);
		MLX5_SET(wq, wq, wq_umem_valid, 1);
		MLX5_SET(wq, wq, dbr_umem_id, umem->umem_id);
		MLX5_SET(wq, wq, dbr_umem_valid, 1);
		MLX5_SET64(wq, wq, dbr_addr, OFF_DBR + DBR_SQ_OFF);
		MLX5_SET(wq, wq, pd, pdn);

		errno = 0;
		sq_obj = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
		if (!sq_obj) {
			bad("CREATE_SQ (syndrome=0x%x)", MLX5_GET(create_sq_out, out, syndrome));
			goto out;
		}
		sqn = MLX5_GET(create_sq_out, out, sqn);
		ok("SQ 创建成功, sqn = %u", sqn);
	}

	/* ---------------- 6. SQ: RST -> RDY ---------------- */
	printf("\n=== [6] SQ 状态迁移 RST -> RDY ===\n");
	{
		uint32_t in[MLX5_ST_SZ_DW(modify_sq_in)] = {0};
		uint32_t out[MLX5_ST_SZ_DW(modify_sq_out)] = {0};
		void *sqc = MLX5_ADDR_OF(modify_sq_in, in, ctx);

		MLX5_SET(modify_sq_in, in, opcode, MLX5_CMD_OP_MODIFY_SQ);
		MLX5_SET(modify_sq_in, in, sq_state, MLX5_SQC_STATE_RST);
		MLX5_SET(modify_sq_in, in, sqn, sqn);
		MLX5_SET(sqc, sqc, state, MLX5_SQC_STATE_RDY);

		errno = 0;
		if (mlx5dv_devx_obj_modify(sq_obj, in, sizeof(in), out, sizeof(out))) {
			bad("MODIFY_SQ RST->RDY (syndrome=0x%x)",
			    MLX5_GET(modify_sq_out, out, syndrome));
			goto out;
		}
		ok("SQ 进入 RDY");
	}

	/* ---------------- 7. 发一条 NOP，等 CQE ---------------- */
	printf("\n=== [7] 投递 NOP 任务单，等待网卡写回 CQE ===\n");
	{
		volatile uint32_t *wqe = (volatile uint32_t *)sq_buf;
		struct timespec t0, t1;
		int got = 0;

		/*
		 * 最小 NOP 的控制段（Control Segment）：
		 *   dw0: opcode(8) | wqe_index(16) << 8
		 *   dw1: qpn/sqn(24) << 8 | ds(6)
		 *   dw2: signature/flags —— 要求产生 CQE
		 */
		wqe[0] = htobe32((0u << 8) | MLX5_OPCODE_NOP);      /* wqe_index=0 */
		wqe[1] = htobe32((sqn << 8) | 0x01);               /* ds = 1 个 16B 段 */
		wqe[2] = htobe32(MLX5_COMP_ALWAYS << 2);           /* 总是产生 CQE */
		wqe[3] = 0;
		__sync_synchronize();

		info("NOP WQE 已写入共享内存的 SQ 缓冲区 @ +0x%x", OFF_SQ);

		/* doorbell record: 告诉网卡生产者索引前进到 1 */
		dbr[DBR_SQ_OFF / 4] = htobe32(1);
		__sync_synchronize();

		/* 敲 BAR 上的 doorbell 寄存器（MMIO，非 DMA） */
		*(volatile uint64_t *)uar->reg_addr = *(volatile uint64_t *)wqe;
		__sync_synchronize();
		info("doorbell 已敲响（UAR MMIO）");

		/* 轮询 CQ 缓冲区。初始填了 0xff，网卡写入才会变。 */
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (;;) {
			volatile uint8_t *cqe = cq_buf;   /* 第 0 个 CQE */
			uint8_t op_own = cqe[CQE_SIZE - 1];

			/* owner 位为 0 表示这是网卡新写的（我们初始化成 0xff） */
			if ((op_own & 0x1) == 0 && op_own != 0xff) {
				got = 1;
				break;
			}
			clock_gettime(CLOCK_MONOTONIC, &t1);
			if (t1.tv_sec - t0.tv_sec > 5)
				break;
		}

		if (!got) {
			bad("超时：5 秒内 CQ 缓冲区没有被网卡写入");
			info("CQE[0] 后 16 字节仍是: ");
			for (int i = 48; i < 64; i++) printf("%02x ", cq_buf[i]);
			printf("\n");
			info("说明 DMA 写没有落到共享页里 —— 这是需要深挖的关键失败");
		} else {
			volatile uint8_t *cqe = cq_buf;
			uint8_t op_own = cqe[CQE_SIZE - 1];
			uint8_t opcode = op_own >> 4;

			/*
			 * 关键判据：CQE 里的 qpn 必须等于我们创建的 sqn。
			 * 这个值不可能由 memset 或任何本地写入产生 ——
			 * 只有网卡知道它。全零的 CQE 不足以证明 DMA 落地。
			 */
			/* mlx5_cqe64 尾部布局：
			 *   0x30 timestamp(8)  0x38 sop_drop_qpn(4)
			 *   0x3c wqe_counter(2)  0x3e signature(1)  0x3f op_own(1) */
			uint32_t qpn_field = be32toh(*(volatile uint32_t *)(cqe + 0x38)) & 0xffffff;
			uint16_t wqe_cnt = be16toh(*(volatile uint16_t *)(cqe + 0x3c));
			uint64_t tstamp  = be64toh(*(volatile uint64_t *)(cqe + 0x30));

			info("op_own = 0x%02x, opcode = 0x%x %s", op_own, opcode,
			     opcode == 0 ? "(REQ, 正常完成)" :
			     opcode == 13 || opcode == 14 ? "(错误 CQE)" : "");
			printf("         CQE 后 32 字节(有效字段所在): ");
			for (int i = 32; i < 64; i++) printf("%02x ", cqe[i]);
			printf("\n");
			info("解码: qpn=%u  wqe_counter=%u  timestamp=0x%llx",
			     qpn_field, wqe_cnt, (unsigned long long)tstamp);

			if (qpn_field == sqn) {
				ok("**qpn(%u) == sqn(%u) —— 只有网卡知道这个值**", qpn_field, sqn);
				if (tstamp)
					ok("且带有非零硬件时间戳 0x%llx —— 双重佐证",
					   (unsigned long long)tstamp);
				ok("DMA 写确实落进了共享页，且 CPU 经 mmap 读得到。地基成立。");
			} else {
				bad("qpn(%u) != sqn(%u) —— 这不是网卡写的 CQE，", qpn_field, sqn);
				info("全零缓冲区不能当作 DMA 落地的证据。需要继续排查。");
			}

			if (opcode == 13 || opcode == 14) {
				info("是错误 CQE —— 但只要 qpn 对得上，仍然证明 DMA 写落进了");
				info("我们的共享内存，地基依然成立（错误本身另行排查）。");
			}
		}
	}

out:
	printf("\n==================== 小结 ====================\n");
	printf("失败项: %d\n", fails);
	if (sq_obj) mlx5dv_devx_obj_destroy(sq_obj);
	if (tis_obj) mlx5dv_devx_obj_destroy(tis_obj);
	if (cq_obj) mlx5dv_devx_obj_destroy(cq_obj);
	if (umem) mlx5dv_devx_umem_dereg(umem);
	if (base && base != MAP_FAILED) munmap(base, TOTAL_SIZE);
	if (fd >= 0) close(fd);
	if (uar) mlx5dv_devx_free_uar(uar);
	if (pd) ibv_dealloc_pd(pd);
	if (ctx) ibv_close_device(ctx);
	if (devs) ibv_free_device_list(devs);
	return fails ? 1 : 0;
}
