/*
 * snp-probe: 在 SEV-SNP guest 里验证 mlx5 数据面最小可行路径。
 *
 * 背景：SNP guest 里 ibv_reg_mr 对普通/大页内存返回 EIO（设备无法 DMA 进
 * 私有加密内存），但 ibv_reg_dmabuf_mr 对 dma-heap 分配的内存能成功。
 * 本程序验证这条路能否撑起完整的收包数据面。
 *
 * 分阶段执行，每阶段独立报告，失败不立即退出（除非无法继续），
 * 以便一次运行拿到尽可能多的信息。
 *
 * 关键未知项：
 *   - ibv_create_cq / ibv_create_qp 的队列缓冲区由 rdma-core 在用户态分配，
 *     若其内部走 ibv_reg_mr，则 SNP 下会失败 —— 那么仅靠 dma-buf 不够。
 *   - 注册成功不等于 DMA 真的落地，所以最后要实际收包。
 *
 * 需要 root（dma_heap 是 root-only，raw packet QP 需要 CAP_NET_RAW）。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <linux/dma-heap.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

#define DMA_HEAP "/dev/dma_heap/system"
#define RX_DEPTH 64
#define RECV_WAIT_SEC 20

static int stage_no;
static int fail_count;

static void stage(const char *fmt, ...)
{
	va_list ap;
	printf("\n=== [%d] ", ++stage_no);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf(" ===\n");
}

static void ok(const char *fmt, ...)
{
	va_list ap;
	printf("  [ OK ] ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

static void fail(const char *fmt, ...)
{
	va_list ap;
	fail_count++;
	printf("  [FAIL] ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf(" | errno=%d (%s)\n", errno, strerror(errno));
}

static void info(const char *fmt, ...)
{
	va_list ap;
	printf("         ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

/* ---- dma-heap 分配 ---- */
struct dmabuf {
	int fd;
	size_t len;
	void *map; /* 可能为 NULL：映射失败不影响 DMA 注册 */
};

static int dmabuf_alloc(struct dmabuf *db, size_t len)
{
	struct dma_heap_allocation_data req = {
		.len = len,
		.fd_flags = O_RDWR | O_CLOEXEC,
	};
	int heap;

	db->fd = -1;
	db->map = NULL;
	db->len = len;

	heap = open(DMA_HEAP, O_RDWR);
	if (heap < 0)
		return -1;

	if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &req) < 0) {
		close(heap);
		return -1;
	}
	close(heap);

	db->fd = req.fd;
	/* 映射是为了让 CPU 能读收到的包；失败不致命 */
	db->map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, db->fd, 0);
	if (db->map == MAP_FAILED)
		db->map = NULL;
	return 0;
}

static void dmabuf_free(struct dmabuf *db)
{
	if (db->map)
		munmap(db->map, db->len);
	if (db->fd >= 0)
		close(db->fd);
	db->map = NULL;
	db->fd = -1;
}

/* 找到 RDMA 设备对应的网卡 MAC，用于下发 flow 规则 */
static int find_mac(struct ibv_context *ctx, uint8_t mac[6])
{
	char path[512], buf[64];
	const char *dev = ibv_get_device_name(ctx->device);
	DIR *d;
	struct dirent *e;
	FILE *f;
	int ret = -1;

	snprintf(path, sizeof(path), "/sys/class/infiniband/%s/device/net", dev);
	d = opendir(path);
	if (!d)
		return -1;

	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path),
			 "/sys/class/infiniband/%s/device/net/%s/address", dev, e->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(buf, sizeof(buf), f) &&
		    sscanf(buf, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
			   &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
			info("使用网卡 %s, MAC %02x:%02x:%02x:%02x:%02x:%02x",
			     e->d_name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
			ret = 0;
		}
		fclose(f);
		if (ret == 0)
			break;
	}
	closedir(d);
	return ret;
}

struct raw_flow {
	struct ibv_flow_attr attr;
	struct ibv_flow_spec_eth eth;
} __attribute__((packed));

int main(int argc, char **argv)
{
	struct ibv_device **devs = NULL;
	struct ibv_context *ctx = NULL;
	struct ibv_pd *pd = NULL;
	struct ibv_cq *cq = NULL;
	struct ibv_qp *qp = NULL;
	struct ibv_flow *flow = NULL;
	struct ibv_mr *mr = NULL;
	struct dmabuf db = { .fd = -1 };
	uint8_t mac[6];
	int ndev = 0;
	size_t buf_len = 64u << 20; /* 默认 64MB，覆盖"大块分配"这一未知项 */
	const size_t entry = 2048;  /* 每个接收缓冲 2KB */

	if (argc > 1)
		buf_len = strtoull(argv[1], NULL, 0);

	printf("snp-probe: SNP guest 下 mlx5 dma-buf 数据面验证\n");
	printf("接收缓冲区总大小: %zu bytes (%.1f MB)\n", buf_len, buf_len / 1048576.0);

	/* ---------------------------------------------------------------- */
	stage("打开 RDMA 设备");
	devs = ibv_get_device_list(&ndev);
	if (!devs || ndev < 1) {
		fail("ibv_get_device_list: 未找到设备");
		return 1;
	}
	info("发现 %d 个设备, 使用 %s", ndev, ibv_get_device_name(devs[0]));
	ctx = ibv_open_device(devs[0]);
	if (!ctx) {
		fail("ibv_open_device");
		goto out;
	}
	ok("ibv_open_device");

	pd = ibv_alloc_pd(ctx);
	if (!pd) {
		fail("ibv_alloc_pd");
		goto out;
	}
	ok("ibv_alloc_pd");

	/* ---------------------------------------------------------------- */
	stage("对照组: 普通内存注册 (SNP 下预期失败)");
	{
		void *plain = NULL;
		if (posix_memalign(&plain, 4096, 4096) == 0) {
			struct ibv_mr *m;
			memset(plain, 0, 4096);
			errno = 0;
			m = ibv_reg_mr(pd, plain, 4096, IBV_ACCESS_LOCAL_WRITE);
			if (m) {
				ok("ibv_reg_mr 普通内存成功 —— 与先前观测不符, 值得注意");
				ibv_dereg_mr(m);
			} else {
				info("ibv_reg_mr 普通内存失败: errno=%d (%s) —— 符合预期",
				     errno, strerror(errno));
			}
			free(plain);
		}
	}

	/* ---------------------------------------------------------------- */
	stage("dma-heap 分配 %.1f MB", buf_len / 1048576.0);
	if (dmabuf_alloc(&db, buf_len) < 0) {
		fail("dma_heap 分配失败 (heap=%s)", DMA_HEAP);
		info("若为 EPERM: 需要 root; 若为 ENOMEM: 试试更小的 size 参数");
		goto out;
	}
	ok("分配成功, dmabuf fd=%d", db.fd);
	if (db.map)
		ok("mmap 成功 @ %p (CPU 可读取收到的包)", db.map);
	else
		info("mmap 失败: errno=%d (%s) —— 仍可注册, 但无法用 CPU 校验包内容",
		     errno, strerror(errno));

	/* ---------------------------------------------------------------- */
	stage("ibv_reg_dmabuf_mr 注册");
	errno = 0;
	mr = ibv_reg_dmabuf_mr(pd, 0, buf_len, 0, db.fd, IBV_ACCESS_LOCAL_WRITE);
	if (!mr) {
		fail("ibv_reg_dmabuf_mr");
		goto out;
	}
	ok("注册成功, lkey=0x%x addr=%p", mr->lkey, mr->addr);

	/* ---------------------------------------------------------------- */
	stage("ibv_create_cq  [关键未知项: rdma-core 用户态分配 CQ 缓冲区]");
	errno = 0;
	cq = ibv_create_cq(ctx, RX_DEPTH * 2, NULL, NULL, 0);
	if (!cq) {
		fail("ibv_create_cq");
		info("结论: rdma-core 自己分配的队列缓冲区在 SNP 下不可用,");
		info("      仅靠 dma-buf 注册数据缓冲区是不够的。");
		goto out;
	}
	ok("ibv_create_cq 成功 (深度 %d)", RX_DEPTH * 2);
	info("说明 rdma-core 的内部队列内存在 SNP 下可用");

	/* ---------------------------------------------------------------- */
	stage("ibv_create_qp  RAW_PACKET  [关键未知项]");
	{
		struct ibv_qp_init_attr ia;
		memset(&ia, 0, sizeof(ia));
		ia.send_cq = cq;
		ia.recv_cq = cq;
		ia.cap.max_send_wr = 1;
		ia.cap.max_recv_wr = RX_DEPTH;
		ia.cap.max_send_sge = 1;
		ia.cap.max_recv_sge = 1;
		ia.qp_type = IBV_QPT_RAW_PACKET;

		errno = 0;
		qp = ibv_create_qp(pd, &ia);
		if (!qp) {
			fail("ibv_create_qp(RAW_PACKET)");
			info("若为 EOPNOTSUPP: 检查是否有 CAP_NET_RAW (需 root)");
			goto out;
		}
		ok("ibv_create_qp 成功, qp_num=%u", qp->qp_num);
	}

	/* ---------------------------------------------------------------- */
	stage("QP 状态迁移 INIT -> RTR");
	{
		struct ibv_qp_attr a;
		memset(&a, 0, sizeof(a));
		a.qp_state = IBV_QPS_INIT;
		a.port_num = 1;
		errno = 0;
		if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PORT)) {
			fail("modify_qp -> INIT");
			goto out;
		}
		ok("-> INIT");

		memset(&a, 0, sizeof(a));
		a.qp_state = IBV_QPS_RTR;
		errno = 0;
		if (ibv_modify_qp(qp, &a, IBV_QP_STATE)) {
			fail("modify_qp -> RTR");
			goto out;
		}
		ok("-> RTR (可以收包了)");
	}

	/* ---------------------------------------------------------------- */
	stage("投递接收缓冲 (post_recv)");
	{
		int posted = 0;
		for (int i = 0; i < RX_DEPTH; i++) {
			struct ibv_sge sge;
			struct ibv_recv_wr wr, *bad = NULL;

			/* 注意: addr 用的是 MR 内的偏移语义 —— dmabuf MR 的
			 * iova 传了 0, 所以这里直接用偏移量寻址。 */
			sge.addr = (uint64_t)(i * entry);
			sge.length = entry;
			sge.lkey = mr->lkey;

			memset(&wr, 0, sizeof(wr));
			wr.wr_id = i;
			wr.sg_list = &sge;
			wr.num_sge = 1;

			errno = 0;
			if (ibv_post_recv(qp, &wr, &bad)) {
				fail("ibv_post_recv 第 %d 个", i);
				break;
			}
			posted++;
		}
		if (posted == 0)
			goto out;
		ok("投递了 %d 个接收缓冲, 每个 %zu 字节", posted, entry);
	}

	/* ---------------------------------------------------------------- */
	stage("下发 flow 规则 (把本机 MAC 的包导向这个 QP)");
	if (find_mac(ctx, mac) < 0) {
		fail("无法确定网卡 MAC");
		goto out;
	}
	{
		struct raw_flow f;
		memset(&f, 0, sizeof(f));
		f.attr.type = IBV_FLOW_ATTR_NORMAL;
		f.attr.size = sizeof(f);
		f.attr.num_of_specs = 1;
		f.attr.port = 1;
		f.eth.type = IBV_FLOW_SPEC_ETH;
		f.eth.size = sizeof(f.eth);
		memcpy(f.eth.val.dst_mac, mac, 6);
		memset(f.eth.mask.dst_mac, 0xff, 6);

		errno = 0;
		flow = ibv_create_flow(qp, &f.attr);
		if (!flow) {
			fail("ibv_create_flow");
			goto out;
		}
		ok("flow 规则已下发");
	}

	/* ---------------------------------------------------------------- */
	stage("等待收包 —— 请在主机上执行: ping 172.31.255.2");
	{
		time_t deadline = time(NULL) + RECV_WAIT_SEC;
		int got = 0;

		printf("         最多等待 %d 秒...\n", RECV_WAIT_SEC);
		while (time(NULL) < deadline && got == 0) {
			struct ibv_wc wc;
			int n = ibv_poll_cq(cq, 1, &wc);

			if (n < 0) {
				fail("ibv_poll_cq");
				break;
			}
			if (n == 0) {
				struct timespec ts = { .tv_sec = 0, .tv_nsec = 20000000 };
				nanosleep(&ts, NULL);
				continue;
			}
			got++;
			if (wc.status != IBV_WC_SUCCESS) {
				fail("完成队列返回错误状态: %s (%d)",
				     ibv_wc_status_str(wc.status), wc.status);
				break;
			}
			ok("收到 1 个包! 长度 %u 字节, wr_id=%llu",
			   wc.byte_len, (unsigned long long)wc.wr_id);

			if (db.map) {
				unsigned char *p = (unsigned char *)db.map + wc.wr_id * entry;
				printf("         前 32 字节: ");
				for (unsigned i = 0; i < 32 && i < wc.byte_len; i++)
					printf("%02x ", p[i]);
				printf("\n");
				printf("         目的 MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
				       p[0], p[1], p[2], p[3], p[4], p[5]);
				info("DMA 确实落地了 —— 这是整条链路打通的决定性证据");
			} else {
				info("(无 CPU 映射, 无法校验内容, 但 DMA 完成本身已说明问题)");
			}
		}
		if (got == 0) {
			fail("超时: 没有收到任何包");
			info("注册与队列都成功但收不到包 —— 说明 DMA 写入被静默丢弃,");
			info("这正是 dma-buf 路线需要排查的关键点。");
		}
	}

out:
	printf("\n==================== 小结 ====================\n");
	printf("失败阶段数: %d\n", fail_count);
	if (flow)
		ibv_destroy_flow(flow);
	if (qp)
		ibv_destroy_qp(qp);
	if (cq)
		ibv_destroy_cq(cq);
	if (mr)
		ibv_dereg_mr(mr);
	dmabuf_free(&db);
	if (pd)
		ibv_dealloc_pd(pd);
	if (ctx)
		ibv_close_device(ctx);
	if (devs)
		ibv_free_device_list(devs);
	return fail_count ? 1 : 0;
}
