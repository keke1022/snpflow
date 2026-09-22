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
#include <stddef.h>
#include <infiniband/mlx5dv.h>

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
#define OFF_DATA      0x30000                           /* 192KB，发包数据缓冲区 */
#define SZ_DATA       2048
/* ---- RX 方向 ---- */
#define OFF_RCQ       0x8000                            /* 收包 CQ，与发包 CQ 分开避免索引混淆 */
#define OFF_RQ        0x18000                           /* 接收队列 */
#define RQ_LOG_SIZE   4                                 /* 16 个接收描述符 */
#define RQ_N          (1u << RQ_LOG_SIZE)
#define RQ_STRIDE     16                                /* 每个描述符一个 scatter 段 */
#define OFF_RXBUF     0x40000                           /* 收包缓冲区起点 */
#define RXBUF_SZ      2048
#define DBR_RCQ_OFF   16
#define DBR_RQ_OFF    24
#define TOTAL_SIZE    (1u << 20)                        /* 1MB，够用且跨不过一个 2MB 块 */

#define DBR_CQ_OFF    0    /* doorbell record 里 CQ 用前 8 字节 */
#define DBR_SQ_OFF    8    /* SQ 用接下来 8 字节 */
#define RX_WAIT_SEC   20

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
	struct mlx5dv_devx_obj *mkey_obj = NULL, *rcq_obj = NULL;
	struct mlx5dv_devx_obj *rq_obj = NULL, *tir_obj = NULL;
	struct mlx5dv_dr_domain *dr_dom = NULL;
	struct mlx5dv_dr_table *dr_tbl = NULL;
	struct mlx5dv_dr_matcher *dr_match = NULL;
	struct mlx5dv_dr_action *dr_act = NULL;
	struct mlx5dv_dr_rule *dr_rule = NULL;
	uint32_t lkey = 0, rcqn = 0, rqn = 0;
	uint64_t mkey_base = 0;
	struct mlx5dv_obj dv_obj;
	struct mlx5dv_pd dv_pd;
	uint32_t pdn = 0, eqn = 0, cqn = 0, tisn = 0, sqn = 0, tdn = 0;
	uint8_t *base = NULL;
	volatile uint8_t *cq_buf, *sq_buf;
	volatile uint32_t *dbr;
	int fd = -1, ndev = 0;

	devs = ibv_get_device_list(&ndev);
	if (!devs || ndev < 1) { fprintf(stderr, "无 RDMA 设备\n"); return 1; }
	/* 必须用 mlx5dv_open_device 带 DEVX 标志打开：
	 * 普通 ibv_open_device 下 mlx5dv_dr_domain_create 会返回 ENODEV。
	 * DPDK 的 mlx5 PMD 也是这么开的。 */
	{
		struct mlx5dv_context_attr attr;
		memset(&attr, 0, sizeof(attr));
		attr.flags = MLX5DV_CONTEXT_FLAGS_DEVX;
		errno = 0;
		ctx = mlx5dv_open_device(devs[0], &attr);
		if (!ctx) {
			info("mlx5dv_open_device(DEVX) 失败: %s，退回普通打开",
			     strerror(errno));
			ctx = ibv_open_device(devs[0]);
		} else {
			printf("（已用 DEVX 标志打开设备）\n");
		}
	}
	if (!ctx) { perror("open_device"); return 1; }
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

	/* ---------------- 8. D2: 基于 umem 创建 mkey ---------------- */
	printf("\n=== [8] 基于 umem 创建 mkey（D2）===\n");
	{
		uint32_t in[MLX5_ST_SZ_DW(create_mkey_in)] = {0};
		uint32_t out[MLX5_ST_SZ_DW(create_mkey_out)] = {0};
		void *mkc = MLX5_ADDR_OF(create_mkey_in, in, memory_key_mkey_entry);
		uint32_t xlat = (uint32_t)(((TOTAL_SIZE + 4095) / 4096) * 4096 * 8 / 16);

		MLX5_SET(create_mkey_in, in, opcode, MLX5_CMD_OP_CREATE_MKEY);
		MLX5_SET(create_mkey_in, in, mkey_umem_id, umem->umem_id);
		MLX5_SET(create_mkey_in, in, translations_octword_actual_size, xlat);
		MLX5_SET(create_mkey_in, in, pg_access, 0);

		MLX5_SET(mkc, mkc, access_mode_1_0, MLX5_MKC_ACCESS_MODE_MTT);
		MLX5_SET(mkc, mkc, log_page_size, 12);      /* 4KB */
		MLX5_SET(mkc, mkc, lw, 1);
		MLX5_SET(mkc, mkc, lr, 1);
		MLX5_SET(mkc, mkc, qpn, 0xffffff);
		MLX5_SET(mkc, mkc, pd, pdn);
		MLX5_SET(mkc, mkc, mkey_7_0, umem->umem_id & 0xff);
		MLX5_SET(mkc, mkc, translations_octword_size, xlat);
		/* 默认偏移语义（start_addr=0）；设 CVM_MKEY_VA 则改用 mmap 后的
		 * 虚拟地址作 start_addr —— 若可行，WQE 里就能直填 VA，
		 * 与 DPDK 的 IOVA=VA 模式对齐，集成更干净。 */
		mkey_base = getenv("CVM_MKEY_VA") ? (uint64_t)(uintptr_t)base : 0;
		MLX5_SET64(mkc, mkc, start_addr, mkey_base);
		MLX5_SET64(mkc, mkc, len, TOTAL_SIZE);

		errno = 0;
		mkey_obj = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
		if (!mkey_obj) {
			bad("CREATE_MKEY (syndrome=0x%x)",
			    MLX5_GET(create_mkey_out, out, syndrome));
			goto out;
		}
		lkey = (MLX5_GET(create_mkey_out, out, mkey_index) << 8) |
		       (umem->umem_id & 0xff);
		ok("mkey 创建成功, lkey = 0x%x —— D2 通过", lkey);
		info("说明共享内存可以直接作为数据缓冲区被网卡寻址");
	}

	/* ---------------- 9. 从共享内存发一个真实以太网帧 ---------------- */
	printf("\n=== [9] 发送真实帧：网卡需从共享内存 DMA 读数据 ===\n");
	{
		volatile uint8_t *data = base + OFF_DATA;
		volatile uint32_t *wqe = (volatile uint32_t *)(sq_buf + WQE_BB);
		const uint16_t ETHERTYPE = 0x88b5;   /* 本地实验用 */
		const char *payload = "CVM-SHARED-DMA-READ-PROOF";
		unsigned frame_len;
		struct timespec t0, t1;
		int got = 0;

		/* 目的 MAC = 主机 PF0，源 MAC = guest 网卡 */
		static const uint8_t dst[6] = {0x60,0x5e,0x65,0xe3,0xb8,0x64};
		static const uint8_t src[6] = {0x60,0x5e,0x65,0xe3,0xb8,0x65};

		for (int i = 0; i < 6; i++) data[i] = dst[i];
		for (int i = 0; i < 6; i++) data[6 + i] = src[i];
		data[12] = ETHERTYPE >> 8;
		data[13] = ETHERTYPE & 0xff;
		for (unsigned i = 0; i < strlen(payload); i++)
			data[14 + i] = payload[i];
		frame_len = 14 + strlen(payload);
		/* 补到 60 字节最小帧长 */
		while (frame_len < 60) data[frame_len++] = 0x00;
		__sync_synchronize();

		info("帧已写入共享内存 +0x%x, 长度 %u, 载荷 \"%s\"",
		     OFF_DATA, frame_len, payload);

		/* 控制段 */
		wqe[0] = htobe32((1u << 8) | MLX5_OPCODE_SEND);   /* wqe_index = 1 */
		wqe[1] = htobe32((sqn << 8) | 3);                 /* ds = 3 个 16B 段 */
		wqe[2] = htobe32(MLX5_COMP_ALWAYS << 2);
		wqe[3] = 0;
		/* 以太网段：不内联头部 */
		wqe[4] = 0; wqe[5] = 0; wqe[6] = 0; wqe[7] = 0;
		/* 数据段：{byte_count, lkey, addr} —— addr 用 umem 内偏移 */
		wqe[8]  = htobe32(frame_len);
		wqe[9]  = htobe32(lkey);
		{
			uint64_t dseg_addr = mkey_base + OFF_DATA;
			wqe[10] = htobe32((uint32_t)(dseg_addr >> 32));
			wqe[11] = htobe32((uint32_t)dseg_addr);
			info("数据段地址 = 0x%llx (%s)", (unsigned long long)dseg_addr,
			     mkey_base ? "VA 语义" : "偏移语义");
		}
		__sync_synchronize();

		dbr[DBR_SQ_OFF / 4] = htobe32(2);   /* 生产者索引推进到 2 */
		__sync_synchronize();
		*(volatile uint64_t *)uar->reg_addr = *(volatile uint64_t *)wqe;
		__sync_synchronize();
		info("doorbell 已敲响，等待完成通知");

		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (;;) {
			volatile uint8_t *cqe = cq_buf + CQE_SIZE;   /* 第 1 个 CQE */
			uint8_t op_own = cqe[CQE_SIZE - 1];

			if ((op_own & 0x1) == 0 && op_own != 0xff) { got = 1; break; }
			clock_gettime(CLOCK_MONOTONIC, &t1);
			if (t1.tv_sec - t0.tv_sec > 5) break;
		}

		if (!got) {
			bad("超时：没有收到发送完成通知");
		} else {
			volatile uint8_t *cqe = cq_buf + CQE_SIZE;
			uint8_t op_own = cqe[CQE_SIZE - 1];
			uint8_t opcode = op_own >> 4;
			uint32_t qpn_f = be32toh(*(volatile uint32_t *)(cqe + 0x38)) & 0xffffff;
			uint16_t wc = be16toh(*(volatile uint16_t *)(cqe + 0x3c));

			info("CQE: opcode=0x%x qpn=%u wqe_counter=%u", opcode, qpn_f, wc);
			if (opcode == 0 && qpn_f == sqn) {
				ok("**发送完成，网卡从共享内存 DMA 读走了帧数据**");
				ok("请在主机侧 tcpdump 确认收到载荷 \"%s\"", payload);
			} else if (opcode == 13 || opcode == 14) {
				bad("错误 CQE (opcode=0x%x)", opcode);
				printf("         CQE 全部 64 字节: ");
				for (int i = 0; i < 64; i++) printf("%02x ", cqe[i]);
				printf("\n");
			} else {
				bad("CQE 异常: opcode=0x%x qpn=%u", opcode, qpn_f);
			}
		}
	}

	/* ---------------- 10. RX: 收包 CQ ---------------- */
	printf("\n=== [10] 建立接收方向：CQ / RQ / TIR / 流规则 ===\n");
	{
		uint32_t in[MLX5_ST_SZ_DW(create_cq_in)] = {0};
		uint32_t out[MLX5_ST_SZ_DW(create_cq_out)] = {0};
		void *cqc = MLX5_ADDR_OF(create_cq_in, in, cq_context);

		memset((void *)(base + OFF_RCQ), 0xff, CQ_N * CQE_SIZE);
		__sync_synchronize();

		MLX5_SET(create_cq_in, in, opcode, MLX5_CMD_OP_CREATE_CQ);
		MLX5_SET(create_cq_in, in, cq_umem_id, umem->umem_id);
		MLX5_SET64(create_cq_in, in, cq_umem_offset, OFF_RCQ);
		MLX5_SET(create_cq_in, in, cq_umem_valid, 1);
		MLX5_SET(cqc, cqc, log_cq_size, CQ_LOG_SIZE);
		MLX5_SET(cqc, cqc, uar_page, uar->page_id);
		MLX5_SET(cqc, cqc, c_eqn, eqn);
		MLX5_SET(cqc, cqc, dbr_umem_id, umem->umem_id);
		MLX5_SET(cqc, cqc, dbr_umem_valid, 1);
		MLX5_SET64(cqc, cqc, dbr_addr, OFF_DBR + DBR_RCQ_OFF);
		MLX5_SET(cqc, cqc, cqe_sz, 0);

		errno = 0;
		rcq_obj = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
		if (!rcq_obj) { bad("CREATE_CQ (RX)"); goto out; }
		rcqn = MLX5_GET(create_cq_out, out, cqn);
		ok("接收 CQ: cqn = %u", rcqn);
	}

	/* ---------------- 11. RQ ---------------- */
	{
		uint32_t in[MLX5_ST_SZ_DW(create_rq_in)] = {0};
		uint32_t out[MLX5_ST_SZ_DW(create_rq_out)] = {0};
		void *rqc = MLX5_ADDR_OF(create_rq_in, in, ctx);
		void *wq  = MLX5_ADDR_OF(rqc, rqc, wq);

		MLX5_SET(create_rq_in, in, opcode, MLX5_CMD_OP_CREATE_RQ);
		MLX5_SET(rqc, rqc, cqn, rcqn);
		MLX5_SET(rqc, rqc, state, MLX5_RQC_STATE_RST);
		MLX5_SET(rqc, rqc, flush_in_error_en, 1);

		MLX5_SET(wq, wq, wq_type, MLX5_WQ_TYPE_CYCLIC);
		MLX5_SET(wq, wq, log_wq_stride, 4);        /* 16 字节一个 scatter 段 */
		MLX5_SET(wq, wq, log_wq_sz, RQ_LOG_SIZE);
		MLX5_SET(wq, wq, pd, pdn);
		MLX5_SET(wq, wq, wq_umem_id, umem->umem_id);
		MLX5_SET64(wq, wq, wq_umem_offset, OFF_RQ);
		MLX5_SET(wq, wq, wq_umem_valid, 1);
		MLX5_SET(wq, wq, dbr_umem_id, umem->umem_id);
		MLX5_SET(wq, wq, dbr_umem_valid, 1);
		MLX5_SET64(wq, wq, dbr_addr, OFF_DBR + DBR_RQ_OFF);

		errno = 0;
		rq_obj = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
		if (!rq_obj) {
			bad("CREATE_RQ (syndrome=0x%x)", MLX5_GET(create_rq_out, out, syndrome));
			goto out;
		}
		rqn = MLX5_GET(create_rq_out, out, rqn);
		ok("RQ 创建成功: rqn = %u（缓冲区在共享内存 +0x%x）", rqn, OFF_RQ);
	}

	/* 投递接收描述符：每个 16 字节 {byte_count, lkey, addr} */
	{
		volatile uint32_t *rwqe = (volatile uint32_t *)(base + OFF_RQ);

		for (unsigned i = 0; i < RQ_N; i++) {
			uint64_t off = OFF_RXBUF + (uint64_t)i * RXBUF_SZ;

			rwqe[i * 4 + 0] = htobe32(RXBUF_SZ);
			rwqe[i * 4 + 1] = htobe32(lkey);
			rwqe[i * 4 + 2] = htobe32((uint32_t)(off >> 32));
			rwqe[i * 4 + 3] = htobe32((uint32_t)off);
		}
		memset((void *)(base + OFF_RXBUF), 0, RQ_N * RXBUF_SZ);
		__sync_synchronize();
		dbr[DBR_RQ_OFF / 4] = htobe32(RQ_N);   /* 生产者索引 */
		__sync_synchronize();
		ok("投递 %u 个接收描述符，缓冲区在共享内存 +0x%x", RQ_N, OFF_RXBUF);
	}

	/* RQ: RST -> RDY */
	{
		uint32_t in[MLX5_ST_SZ_DW(modify_rq_in)] = {0};
		uint32_t out[MLX5_ST_SZ_DW(modify_rq_out)] = {0};
		void *rqc = MLX5_ADDR_OF(modify_rq_in, in, ctx);

		MLX5_SET(modify_rq_in, in, opcode, MLX5_CMD_OP_MODIFY_RQ);
		MLX5_SET(modify_rq_in, in, rq_state, MLX5_RQC_STATE_RST);
		MLX5_SET(modify_rq_in, in, rqn, rqn);
		MLX5_SET(rqc, rqc, state, MLX5_RQC_STATE_RDY);

		errno = 0;
		if (mlx5dv_devx_obj_modify(rq_obj, in, sizeof(in), out, sizeof(out))) {
			bad("MODIFY_RQ RST->RDY (syndrome=0x%x)",
			    MLX5_GET(modify_rq_out, out, syndrome));
			goto out;
		}
		ok("RQ 进入 RDY");
	}

	/* ---------------- 12. TIR（直接指向该 RQ）---------------- */
	{
		uint32_t in[MLX5_ST_SZ_DW(create_tir_in)] = {0};
		uint32_t out[MLX5_ST_SZ_DW(create_tir_out)] = {0};
		void *tirc = MLX5_ADDR_OF(create_tir_in, in, ctx);

		MLX5_SET(create_tir_in, in, opcode, MLX5_CMD_OP_CREATE_TIR);
		MLX5_SET(tirc, tirc, disp_type, MLX5_TIRC_DISP_TYPE_DIRECT);
		MLX5_SET(tirc, tirc, inline_rqn, rqn);
		MLX5_SET(tirc, tirc, transport_domain, tdn);

		errno = 0;
		tir_obj = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
		if (!tir_obj) {
			bad("CREATE_TIR (syndrome=0x%x)", MLX5_GET(create_tir_out, out, syndrome));
			goto out;
		}
		ok("TIR 创建成功（DIRECT 模式，直接指向 rqn=%u）", rqn);
	}

	/* ---------------- 13. 流规则：把本机 MAC 的包导向该 TIR ----------------
	 *
	 * 用 Direct Rules (DR) API 而不是 mlx5dv_create_flow。
	 * 后者走内核 verbs 流引导，实测下发成功但不匹配任何流量
	 * （用 DROP 动作做二分验证：ping 依然 0% 丢包）。
	 * DPDK 的 mlx5 PMD 走的就是 DR 这条路。
	 */
	{
		struct mlx5dv_flow_match_parameters *mask, *val;
		size_t psz = offsetof(struct mlx5dv_flow_match_parameters, match_buf) +
			     MLX5_ST_SZ_BYTES(fte_match_param);
		static const uint8_t my_mac[6] = {0x60,0x5e,0x65,0xe3,0xb8,0x65};

		mask = calloc(1, psz);
		val  = calloc(1, psz);
		if (!mask || !val) { bad("calloc"); goto out; }
		mask->match_sz = MLX5_ST_SZ_BYTES(fte_match_param);
		val->match_sz  = MLX5_ST_SZ_BYTES(fte_match_param);
		{
			void *m = MLX5_ADDR_OF(fte_match_param, mask->match_buf, outer_headers);
			void *v = MLX5_ADDR_OF(fte_match_param, val->match_buf, outer_headers);
			uint8_t *mm = MLX5_ADDR_OF(fte_match_set_lyr_2_4, m, dmac_47_16);
			uint8_t *vv = MLX5_ADDR_OF(fte_match_set_lyr_2_4, v, dmac_47_16);

			memset(mm, 0xff, 6);      /* dmac_47_16 与 dmac_15_0 相邻，6 字节连写 */
			memcpy(vv, my_mac, 6);
		}

		errno = 0;
		dr_dom = mlx5dv_dr_domain_create(ctx, MLX5DV_DR_DOMAIN_TYPE_NIC_RX);
		if (!dr_dom) { bad("dr_domain_create"); free(mask); free(val); goto out; }

		dr_tbl = mlx5dv_dr_table_create(dr_dom, 0);
		if (!dr_tbl) { bad("dr_table_create(level 0)"); free(mask); free(val); goto out; }

		dr_match = mlx5dv_dr_matcher_create(dr_tbl, 0, 1 /* outer_headers */, mask);
		if (!dr_match) { bad("dr_matcher_create"); free(mask); free(val); goto out; }

		if (getenv("CVM_FLOW_DROP")) {
			dr_act = mlx5dv_dr_action_create_drop();
			info("诊断模式：动作设为 DROP");
		} else {
			dr_act = mlx5dv_dr_action_create_dest_devx_tir(tir_obj);
		}
		if (!dr_act) { bad("dr_action_create"); free(mask); free(val); goto out; }

		{
			struct mlx5dv_dr_action *acts[1] = { dr_act };
			errno = 0;
			dr_rule = mlx5dv_dr_rule_create(dr_match, val, 1, acts);
		}
		free(mask); free(val);
		if (!dr_rule) { bad("dr_rule_create"); goto out; }
		ok("DR 流规则已下发：目的 MAC 60:5e:65:e3:b8:65 -> 本 TIR");
	}

	/* ---------------- 14. 等待收包 ---------------- */
	printf("\n=== [14] 等待收包 —— 请在主机上执行: ping 172.31.255.2 ===\n");
	{
		struct timespec t0, t1;
		int got = 0;

		info("最多等待 %d 秒……", RX_WAIT_SEC);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (;;) {
			volatile uint8_t *cqe = base + OFF_RCQ;
			uint8_t op_own = cqe[CQE_SIZE - 1];

			if ((op_own & 0x1) == 0 && op_own != 0xff) { got = 1; break; }
			clock_gettime(CLOCK_MONOTONIC, &t1);
			if (t1.tv_sec - t0.tv_sec > RX_WAIT_SEC) break;
		}

		if (!got) {
			bad("超时：接收 CQ 没有被网卡写入");
			info("若发送方向已通过而这里不通，重点查流规则是否生效");
		} else {
			volatile uint8_t *cqe = base + OFF_RCQ;
			uint8_t op_own = cqe[CQE_SIZE - 1];
			uint8_t opcode = op_own >> 4;
			uint32_t bcnt = be32toh(*(volatile uint32_t *)(cqe + 0x2c));
			uint16_t wqe_cnt = be16toh(*(volatile uint16_t *)(cqe + 0x3c));
			volatile uint8_t *pkt = base + OFF_RXBUF + (uint64_t)wqe_cnt * RXBUF_SZ;

			info("CQE: opcode=0x%x byte_cnt=%u wqe_counter=%u",
			     opcode, bcnt, wqe_cnt);
			if (opcode == 13 || opcode == 14) {
				bad("错误 CQE");
			} else if (bcnt == 0 || bcnt > RXBUF_SZ) {
				bad("byte_cnt 异常: %u", bcnt);
			} else {
				ok("**收到 %u 字节，网卡把包 DMA 写进了共享内存**", bcnt);
				printf("         目的 MAC: ");
				for (int i = 0; i < 6; i++) printf("%02x%s", pkt[i], i<5?":":"");
				printf("  源 MAC: ");
				for (int i = 6; i < 12; i++) printf("%02x%s", pkt[i], i<11?":":"");
				printf("\n         ethertype: 0x%02x%02x\n", pkt[12], pkt[13]);
				printf("         前 48 字节: ");
				for (unsigned i = 0; i < 48 && i < bcnt; i++)
					printf("%02x ", pkt[i]);
				printf("\n");
				if (pkt[0] == 0x60 && pkt[1] == 0x5e && pkt[5] == 0x65)
					ok("目的 MAC 与流规则匹配，收包路径完整打通");
			}
		}
	}

out:
	printf("\n==================== 小结 ====================\n");
	printf("失败项: %d\n", fails);
	if (dr_rule) mlx5dv_dr_rule_destroy(dr_rule);
	if (dr_act) mlx5dv_dr_action_destroy(dr_act);
	if (dr_match) mlx5dv_dr_matcher_destroy(dr_match);
	if (dr_tbl) mlx5dv_dr_table_destroy(dr_tbl);
	if (dr_dom) mlx5dv_dr_domain_destroy(dr_dom);
	if (tir_obj) mlx5dv_devx_obj_destroy(tir_obj);
	if (rq_obj) mlx5dv_devx_obj_destroy(rq_obj);
	if (rcq_obj) mlx5dv_devx_obj_destroy(rcq_obj);
	if (mkey_obj) mlx5dv_devx_obj_destroy(mkey_obj);
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
