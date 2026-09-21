/*
 * verify-probe: cvm_shared 的四项补充验证。
 *
 *  1. 跨映射校验 —— 用户态 mmap 写入、内核直接映射读出比对，再反向一次。
 *     C-bit 只要有一侧处理错了，两边看到的就不是同一份数据。
 *  2. swiotlb 占用计数 —— 注册前后读 /sys/kernel/debug/swiotlb/io_tlb_used，
 *     作为「没碰 swiotlb」的直接证据（而不是只看 dmesg 有没有报错）。
 *  3. IOMMU 拦截 —— 确认 attach 阶段的检查不会误伤当前环境。
 *  4. close(fd) 先于 munmap —— 确认页不会被提前转回私有。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <infiniband/mlx5dv.h>
#include <infiniband/verbs.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "kmod/cvm_shared.h"

#define SWIOTLB_USED "/sys/kernel/debug/swiotlb/io_tlb_used"

static struct ibv_context *ctx;
static int fails;

static void ok(const char *fmt, ...)
{
	va_list ap; printf("  [ OK ] "); va_start(ap, fmt);
	vprintf(fmt, ap); va_end(ap); printf("\n");
}
static void bad(const char *fmt, ...)
{
	va_list ap; fails++; printf("  [FAIL] "); va_start(ap, fmt);
	vprintf(fmt, ap); va_end(ap);
	printf(" | errno=%d (%s)\n", errno, strerror(errno));
}
static void info(const char *fmt, ...)
{
	va_list ap; printf("         "); va_start(ap, fmt);
	vprintf(fmt, ap); va_end(ap); printf("\n");
}

/* 与内核 cvm_pattern() 必须完全一致 */
static inline uint64_t pattern(uint64_t off, uint64_t seed)
{
	return seed ^ (off * 0x9E3779B97F4A7C15ULL) ^ 0x5851F42D4C957F2DULL;
}

/* 往内核日志打标记，使用户态动作与内核侧事件在同一条时间线上可见 */
static void kmark(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int fd;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	fd = open("/dev/kmsg", O_WRONLY);
	if (fd < 0)
		return;
	dprintf(fd, "verify-probe: >>> %s\n", buf);
	close(fd);
}

static long read_swiotlb_used(void)
{
	FILE *f = fopen(SWIOTLB_USED, "r");
	long v = -1;

	if (!f)
		return -1;
	if (fscanf(f, "%ld", &v) != 1)
		v = -1;
	fclose(f);
	return v;
}

static int cvm_alloc(size_t len)
{
	struct cvm_shared_alloc req = { .size = len, .flags = 0, .fd = -1 };
	int dev = open(CVM_SHARED_DEV, O_RDWR);

	if (dev < 0)
		return -1;
	if (ioctl(dev, CVM_SHARED_IOC_ALLOC, &req) < 0) {
		close(dev);
		return -1;
	}
	close(dev);
	return req.fd;
}

static int cvm_xcheck(int fd, unsigned op, uint64_t off, uint64_t len,
		      uint64_t seed, struct cvm_shared_xcheck *out)
{
	struct cvm_shared_xcheck req;
	int dev = open(CVM_SHARED_DEV, O_RDWR);
	int ret;

	if (dev < 0)
		return -1;
	memset(&req, 0, sizeof(req));
	req.fd = fd; req.op = op; req.offset = off; req.len = len; req.seed = seed;
	ret = ioctl(dev, CVM_SHARED_IOC_XCHECK, &req);
	close(dev);
	if (out)
		*out = req;
	return ret;
}

static struct mlx5dv_devx_umem *reg_umem(int fd, size_t len)
{
	struct mlx5dv_devx_umem_in in;

	memset(&in, 0, sizeof(in));
	in.addr = (void *)0;
	in.size = len;
	in.access = IBV_ACCESS_LOCAL_WRITE;
	in.pgsz_bitmap = ~0ULL;
	in.comp_mask = MLX5DV_UMEM_MASK_DMABUF;
	in.dmabuf_fd = fd;
	errno = 0;
	return mlx5dv_devx_umem_reg_ex(ctx, &in);
}

int main(void)
{
	struct ibv_device **devs;
	int ndev = 0;
	const size_t LEN = 16u << 20;   /* 16MB，跨多个 2MB 块 */
	const uint64_t SEED_U = 0xA5A5000000000001ULL;
	const uint64_t SEED_K = 0x5A5A000000000002ULL;

	devs = ibv_get_device_list(&ndev);
	if (!devs || ndev < 1) {
		fprintf(stderr, "未找到 RDMA 设备\n");
		return 1;
	}
	ctx = ibv_open_device(devs[0]);
	if (!ctx) {
		perror("ibv_open_device");
		return 1;
	}
	printf("设备: %s, 缓冲区 %zu MB\n", ibv_get_device_name(devs[0]),
	       LEN / (1024 * 1024));

	/* ============================================================ */
	printf("\n=== [1] 跨映射校验（用户态 mmap ↔ 内核直接映射）===\n");
	{
		int fd = cvm_alloc(LEN);
		uint64_t *p;
		struct cvm_shared_xcheck res;

		if (fd < 0) {
			bad("分配失败");
			goto skip1;
		}
		p = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (p == MAP_FAILED) {
			bad("mmap");
			close(fd);
			goto skip1;
		}

		/* 方向 A：用户态写 → 内核读 */
		for (uint64_t off = 0; off < LEN; off += 8)
			p[off / 8] = pattern(off, SEED_U);
		__sync_synchronize();

		if (cvm_xcheck(fd, CVM_XCHECK_VERIFY, 0, LEN, SEED_U, &res) < 0) {
			bad("XCHECK_VERIFY ioctl");
		} else if (res.bad_off != ~0ULL) {
			bad("方向A 不一致 @0x%llx: 内核读出 0x%016llx, 期望 0x%016llx",
			    (unsigned long long)res.bad_off,
			    (unsigned long long)res.got,
			    (unsigned long long)res.want);
		} else {
			ok("方向A: 用户态写入 %zu MB，内核经直接映射全部读到相同内容",
			   LEN / (1024 * 1024));
		}

		/* 方向 B：内核写 → 用户态读 */
		if (cvm_xcheck(fd, CVM_XCHECK_WRITE, 0, LEN, SEED_K, &res) < 0) {
			bad("XCHECK_WRITE ioctl");
		} else {
			uint64_t bad_off = ~0ULL, got = 0, want = 0;

			__sync_synchronize();
			for (uint64_t off = 0; off < LEN; off += 8) {
				uint64_t w = pattern(off, SEED_K);

				if (p[off / 8] != w) {
					bad_off = off; got = p[off / 8]; want = w;
					break;
				}
			}
			if (bad_off != ~0ULL)
				bad("方向B 不一致 @0x%llx: 用户态读出 0x%016llx, 期望 0x%016llx",
				    (unsigned long long)bad_off,
				    (unsigned long long)got,
				    (unsigned long long)want);
			else
				ok("方向B: 内核经直接映射写入，用户态 mmap 全部读到相同内容");
		}

		munmap(p, LEN);
		close(fd);
	}
skip1:

	/* ============================================================ */
	printf("\n=== [2] swiotlb 占用计数（注册前后对照）===\n");
	{
		long before, after, after_dereg;
		int fd = cvm_alloc(LEN);
		struct mlx5dv_devx_umem *umem;

		if (fd < 0) {
			bad("分配失败");
			goto skip2;
		}

		before = read_swiotlb_used();
		if (before < 0) {
			bad("读不到 %s（需要 root 且 debugfs 已挂载）", SWIOTLB_USED);
			close(fd);
			goto skip2;
		}
		info("注册前 io_tlb_used = %ld slabs (%.2f MB)",
		     before, before * 2048.0 / 1048576.0);

		umem = reg_umem(fd, LEN);
		after = read_swiotlb_used();

		if (!umem) {
			bad("devx_umem_reg_ex");
		} else {
			info("注册后 io_tlb_used = %ld slabs (%.2f MB)",
			     after, after * 2048.0 / 1048576.0);
			if (after == before)
				ok("注册 %zu MB 内存，swiotlb 占用**零变化** —— 直接证据",
				   LEN / (1024 * 1024));
			else
				bad("swiotlb 占用变化了 %+ld slabs (%.2f MB)，说明仍在走 bounce",
				    after - before, (after - before) * 2048.0 / 1048576.0);

			mlx5dv_devx_umem_dereg(umem);
			after_dereg = read_swiotlb_used();
			info("注销后 io_tlb_used = %ld slabs", after_dereg);
		}
		close(fd);
	}
skip2:

	/* ============================================================ */
	printf("\n=== [3] IOMMU 拦截检查 ===\n");
	{
		int fd = cvm_alloc(LEN);
		struct mlx5dv_devx_umem *umem;

		if (fd < 0) {
			bad("分配失败");
			goto skip3;
		}
		umem = reg_umem(fd, LEN);
		if (umem) {
			ok("当前环境 attach 通过（网卡不在翻译型 IOMMU 域后面）");
			info("若换到有 vIOMMU 的环境，attach 会返回 EOPNOTSUPP 并在");
			info("dmesg 里打印明确原因，而不是静默给出错误地址");
			mlx5dv_devx_umem_dereg(umem);
		} else {
			bad("注册失败 —— 若 errno 为 EOPNOTSUPP，说明被 IOMMU 检查拦下");
		}
		close(fd);
	}
skip3:

	/* ============================================================ */
	printf("\n=== [4] close(fd) 先于 munmap ===\n");
	{
		int fd = cvm_alloc(LEN);
		uint64_t *p;
		struct cvm_shared_xcheck res;
		int rc = 0;

		if (fd < 0) {
			bad("分配失败");
			goto skip4;
		}
		p = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (p == MAP_FAILED) {
			bad("mmap");
			close(fd);
			goto skip4;
		}

		for (uint64_t off = 0; off < LEN; off += 4096)
			p[off / 8] = pattern(off, SEED_U);

		/* 关键动作：先关 fd，映射还在。
		 * 往 /dev/kmsg 打标记，让 release 的时刻在内核日志里
		 * 与 close/munmap 直接对齐 —— 光看时间戳是推断，不是证据。 */
		kmark("BEFORE close(fd)");
		close(fd);
		kmark("AFTER close(fd) — 映射仍在，接下来读写");
		info("已 close(fd)，映射仍然存在");

		/* 若页被提前转回私有，这里读出的会是垃圾（或直接 #VC 崩溃） */
		for (uint64_t off = 0; off < LEN; off += 4096) {
			if (p[off / 8] != pattern(off, SEED_U)) {
				bad("close(fd) 后数据损坏 @0x%llx —— 页被提前转回私有了",
				    (unsigned long long)off);
				rc = -1;
				break;
			}
		}
		if (rc == 0)
			ok("close(fd) 之后映射内容完好，页未被提前转回私有");

		/* 写入也应该正常 */
		for (uint64_t off = 0; off < LEN; off += 4096)
			p[off / 8] = pattern(off, SEED_K);
		for (uint64_t off = 0; off < LEN; off += 4096) {
			if (p[off / 8] != pattern(off, SEED_K)) {
				bad("close(fd) 后写入失效 @0x%llx", (unsigned long long)off);
				rc = -1;
				break;
			}
		}
		if (rc == 0)
			ok("close(fd) 之后读写均正常（vma 持有 dma-buf 引用）");

		(void)res;
		kmark("BEFORE munmap");
		munmap(p, LEN);
		kmark("AFTER munmap");
		info("已 munmap —— release 应当出现在这两条标记之间");
	}
skip4:

	printf("\n==================== 小结 ====================\n");
	printf("失败项: %d\n", fails);
	ibv_close_device(ctx);
	ibv_free_device_list(devs);
	return fails ? 1 : 0;
}
