/*
 * shared-probe: 验证 cvm_shared 模块产出的内存能否被 mlx5 直接 DMA。
 *
 * 这是整个方案的地基实验。要回答三个问题：
 *   1. 「已转共享、不经 swiotlb」的内存，devx_umem_reg 是否接受？
 *   2. 能否突破之前 dma-buf(经 swiotlb) 卡住的约 1MB 上限？
 *   3. CPU 通过 mmap 读写这块内存是否正常（C-bit 是否处理对了）？
 *
 * 对照组是系统 dma-heap：同样是 dma-buf，但它的 exporter 调用
 * dma_map_sgtable()，会把页 bounce 到 swiotlb，已实测卡在 1MB。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <infiniband/mlx5dv.h>
#include <infiniband/verbs.h>
#include <linux/dma-heap.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "kmod/cvm_shared.h"

static struct ibv_context *ctx;

static void ok(const char *fmt, ...)
{
	va_list ap; printf("  [ OK ] "); va_start(ap, fmt);
	vprintf(fmt, ap); va_end(ap); printf("\n");
}

static void bad(const char *fmt, ...)
{
	va_list ap; printf("  [FAIL] "); va_start(ap, fmt);
	vprintf(fmt, ap); va_end(ap);
	printf(" | errno=%d (%s)\n", errno, strerror(errno));
}

static void info(const char *fmt, ...)
{
	va_list ap; printf("         "); va_start(ap, fmt);
	vprintf(fmt, ap); va_end(ap); printf("\n");
}

/* 从我们的模块拿一块共享内存 */
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

/* 从系统 dma-heap 拿一块（对照组） */
static int heap_alloc(size_t len)
{
	struct dma_heap_allocation_data req = {
		.len = len, .fd_flags = O_RDWR | O_CLOEXEC,
	};
	int heap = open("/dev/dma_heap/system", O_RDWR);

	if (heap < 0)
		return -1;
	if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &req) < 0) {
		close(heap);
		return -1;
	}
	close(heap);
	return req.fd;
}

/* 把 dma-buf 注册为 DevX umem。pgsz_bitmap 必须非 0，否则 EINVAL。 */
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

static void dump_layout(int fd)
{
	struct cvm_shared_layout lay;
	struct cvm_shared_chunk chunks[8];

	memset(&lay, 0, sizeof(lay));
	lay.fd = fd;
	lay.chunks = (uint64_t)(uintptr_t)chunks;
	lay.max_chunks = 8;

	int dev = open(CVM_SHARED_DEV, O_RDWR);
	if (dev < 0)
		return;
	if (ioctl(dev, CVM_SHARED_IOC_LAYOUT, &lay) == 0) {
		unsigned n = lay.nr_chunks < 8 ? lay.nr_chunks : 8;
		info("物理布局: %u 个连续块%s", lay.nr_chunks,
		     lay.nr_chunks > 8 ? "（只列前 8 个）" : "");
		for (unsigned i = 0; i < n; i++)
			info("  [%u] phys=0x%llx len=%llu KB", i,
			     (unsigned long long)chunks[i].phys,
			     (unsigned long long)chunks[i].len / 1024);
	}
	close(dev);
}

/* CPU 读写校验：C-bit 处理错了的话这里会读出垃圾 */
static int check_cpu_access(int fd, size_t len)
{
	volatile unsigned char *p;
	size_t probe = len < 4096 ? len : 4096;
	int rc = 0;

	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		bad("mmap 共享内存");
		return -1;
	}

	for (size_t i = 0; i < probe; i++)
		p[i] = (unsigned char)(i & 0xff);
	for (size_t i = 0; i < probe; i++) {
		if (p[i] != (unsigned char)(i & 0xff)) {
			bad("CPU 回读不一致 @%zu: 写入 0x%02x 读出 0x%02x",
			    i, (unsigned)(i & 0xff), p[i]);
			rc = -1;
			break;
		}
	}
	if (rc == 0)
		ok("CPU 读写校验通过（C-bit 处理正确）");

	munmap((void *)p, len);
	return rc;
}

int main(int argc, char **argv)
{
	struct ibv_device **devs;
	int ndev = 0;
	size_t sizes[] = {
		256u << 10,  1u << 20,  4u << 20,
		16u << 20,   64u << 20, 256u << 20,
	};
	unsigned nsz = sizeof(sizes) / sizeof(sizes[0]);

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
	printf("设备: %s\n", ibv_get_device_name(devs[0]));

	/* 单尺寸详查模式 */
	if (argc > 1) {
		size_t len = strtoull(argv[1], NULL, 0);
		int fd;

		printf("\n=== 详查 %zu bytes (%.2f MB) ===\n", len, len / 1048576.0);
		fd = cvm_alloc(len);
		if (fd < 0) {
			bad("cvm_shared 分配失败");
			info("检查: lsmod | grep cvm_shared; 以及是否用 root 运行");
			return 1;
		}
		ok("cvm_shared 分配成功, fd=%d", fd);
		dump_layout(fd);
		check_cpu_access(fd, len);
		{
			struct mlx5dv_devx_umem *u = reg_umem(fd, len);
			if (!u)
				bad("devx_umem_reg_ex");
			else {
				ok("devx_umem_reg_ex 成功, umem_id=%u", u->umem_id);
				mlx5dv_devx_umem_dereg(u);
			}
		}
		close(fd);
		return 0;
	}

	/* 尺寸扫描：本模块 vs 系统 dma-heap 对照 */
	printf("\n%-12s  %-28s  %s\n", "尺寸", "cvm_shared (不经 swiotlb)",
	       "系统 dma-heap (经 swiotlb)");
	printf("%-12s  %-28s  %s\n", "----", "-------------------------",
	       "--------------------------");

	for (unsigned i = 0; i < nsz; i++) {
		size_t len = sizes[i];
		char a[32] = "n/a", b[32] = "n/a";
		int fd;

		fd = cvm_alloc(len);
		if (fd < 0) {
			snprintf(a, sizeof(a), "分配失败(%s)", strerror(errno));
		} else {
			struct mlx5dv_devx_umem *u = reg_umem(fd, len);
			if (u) {
				snprintf(a, sizeof(a), "成功");
				mlx5dv_devx_umem_dereg(u);
			} else {
				snprintf(a, sizeof(a), "失败(%s)", strerror(errno));
			}
			close(fd);
		}

		fd = heap_alloc(len);
		if (fd < 0) {
			snprintf(b, sizeof(b), "分配失败(%s)", strerror(errno));
		} else {
			struct mlx5dv_devx_umem *u = reg_umem(fd, len);
			if (u) {
				snprintf(b, sizeof(b), "成功");
				mlx5dv_devx_umem_dereg(u);
			} else {
				snprintf(b, sizeof(b), "失败(%s)", strerror(errno));
			}
			close(fd);
		}

		printf("%-9.2f MB  %-28s  %s\n", len / 1048576.0, a, b);
	}

	ibv_close_device(ctx);
	ibv_free_device_list(devs);
	return 0;
}
