/*
 * devx-probe: 测试 mlx5 的 DevX 路径在 SNP guest 里能否注册队列内存。
 *
 * 为什么要单独测这条路：
 *   verbs 路径 (ibv_create_cq) 的队列缓冲区由 rdma-core 在用户态自行分配，
 *   DPDK 的分配器管不着 —— snp-probe 已证实它在 SNP 下返回 EIO。
 *   但 DevX 路径不同：DPDK 用 mlx5_malloc() 自己分配缓冲区，再通过
 *   mlx5dv_devx_umem_reg() 注册给设备。若这条路可用，那么"把 DPDK 分配的
 *   内存转成共享"这一类补丁就能覆盖到队列内存，方案难度大幅下降。
 *
 * 另外 mlx5dv_devx_umem_in 带 dmabuf_fd 字段，所以也测 dma-buf 变体。
 *
 * 测试矩阵（全部是注册动作，即"让设备获得对这段内存的访问权"）：
 *   A. devx_umem_reg     普通内存
 *   B. devx_umem_reg     2MiB 大页（DPDK 实际使用的内存形态）
 *   C. devx_umem_reg_ex  dma-buf
 *   D. devx_alloc_uar    doorbell UAR（BAR 映射，非 DMA，作对照）
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <infiniband/mlx5dv.h>
#include <infiniband/verbs.h>
#include <linux/dma-heap.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define DMA_HEAP "/dev/dma_heap/system"

static int fails;

static void ok(const char *fmt, ...)
{
	va_list ap;
	printf("  [ OK ] ");
	va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
	printf("\n");
}

static void bad(const char *fmt, ...)
{
	va_list ap;
	fails++;
	printf("  [FAIL] ");
	va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
	printf(" | errno=%d (%s)\n", errno, strerror(errno));
}

static void note(const char *fmt, ...)
{
	va_list ap;
	printf("         ");
	va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
	printf("\n");
}

/* 注册一段普通/大页内存为 DevX umem */
static int try_umem(struct ibv_context *ctx, const char *what, void *addr, size_t len)
{
	struct mlx5dv_devx_umem *umem;

	memset(addr, 0, len);
	errno = 0;
	umem = mlx5dv_devx_umem_reg(ctx, addr, len, IBV_ACCESS_LOCAL_WRITE);
	if (!umem) {
		bad("%s: mlx5dv_devx_umem_reg(%zu bytes)", what, len);
		return -1;
	}
	ok("%s: devx_umem_reg 成功 (%zu bytes), umem_id=%u", what, len, umem->umem_id);
	mlx5dv_devx_umem_dereg(umem);
	return 0;
}

int main(int argc, char **argv)
{
	struct ibv_device **devs;
	struct ibv_context *ctx;
	int ndev = 0;
	size_t len = 262144; /* 默认 256KB：snp-probe 已知 dma-buf 在此尺寸可注册 */

	if (argc > 1)
		len = strtoull(argv[1], NULL, 0);

	printf("devx-probe: SNP guest 下 mlx5 DevX 队列内存注册测试\n");
	printf("测试尺寸: %zu bytes (%.0f KB)\n", len, len / 1024.0);

	devs = ibv_get_device_list(&ndev);
	if (!devs || ndev < 1) {
		bad("未找到 RDMA 设备");
		return 1;
	}
	ctx = ibv_open_device(devs[0]);
	if (!ctx) {
		bad("ibv_open_device");
		ibv_free_device_list(devs);
		return 1;
	}
	printf("设备: %s\n", ibv_get_device_name(devs[0]));

	/* DevX 是否可用，由下面的 umem 注册结果本身反映：
	 * 若返回 EOPNOTSUPP/ENOTSUP 则是 DevX 不可用，
	 * 若返回 EIO 则是内存本身不被接受 —— 两者要区分开看。 */

	/* A. 普通内存 —— 与 ibv_reg_mr 的行为对照 */
	printf("\n=== [A] DevX umem: 普通内存 ===\n");
	{
		void *p = NULL;
		if (posix_memalign(&p, 4096, len) == 0) {
			try_umem(ctx, "普通内存", p, len);
			free(p);
		} else {
			bad("posix_memalign");
		}
	}

	/* B. 大页 —— DPDK 实际使用的内存形态，最有代表性 */
	printf("\n=== [B] DevX umem: 2MiB 大页 (DPDK 的内存形态) ===\n");
	{
		size_t hlen = (len + 0x1fffff) & ~0x1fffffUL; /* 向上取整到 2MB */
		void *p = mmap(NULL, hlen, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
		if (p == MAP_FAILED) {
			bad("大页 mmap (%zu bytes)", hlen);
			note("检查: cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages");
		} else {
			try_umem(ctx, "2MiB 大页", p, hlen);
			munmap(p, hlen);
		}
	}

	/* C. dma-buf —— umem_in 带 dmabuf_fd，走和 ibv_reg_dmabuf_mr 类似的路径 */
	printf("\n=== [C] DevX umem: dma-buf (umem_reg_ex) ===\n");
	{
		struct dma_heap_allocation_data req = {
			.len = len, .fd_flags = O_RDWR | O_CLOEXEC,
		};
		int heap = open(DMA_HEAP, O_RDWR);

		if (heap < 0) {
			bad("open %s", DMA_HEAP);
			note("需要 root");
		} else if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &req) < 0) {
			bad("DMA_HEAP_IOCTL_ALLOC(%zu)", len);
			close(heap);
		} else {
			struct mlx5dv_devx_umem_in in;
			struct mlx5dv_devx_umem *umem;

			close(heap);
			memset(&in, 0, sizeof(in));
			in.addr = NULL;
			in.size = len;
			in.access = IBV_ACCESS_LOCAL_WRITE;
			in.pgsz_bitmap = 0;
			in.comp_mask = MLX5DV_UMEM_MASK_DMABUF;
			in.dmabuf_fd = req.fd;

			errno = 0;
			umem = mlx5dv_devx_umem_reg_ex(ctx, &in);
			if (!umem) {
				bad("mlx5dv_devx_umem_reg_ex(dmabuf, %zu bytes)", len);
			} else {
				ok("dma-buf: devx_umem_reg_ex 成功, umem_id=%u", umem->umem_id);
				mlx5dv_devx_umem_dereg(umem);
			}
			close(req.fd);
		}
	}

	/* D. UAR —— doorbell 走 BAR MMIO，不是 DMA，作为对照组 */
	printf("\n=== [D] DevX UAR (doorbell, BAR MMIO 非 DMA) ===\n");
	{
		struct mlx5dv_devx_uar *uar;
		errno = 0;
		uar = mlx5dv_devx_alloc_uar(ctx, MLX5DV_UAR_ALLOC_TYPE_NC);
		if (!uar) {
			errno = 0;
			uar = mlx5dv_devx_alloc_uar(ctx, 0);
		}
		if (!uar) {
			bad("mlx5dv_devx_alloc_uar");
		} else {
			ok("UAR 分配成功, page_id=%u reg_addr=%p", uar->page_id, uar->reg_addr);
			note("这一项走 BAR 映射而非 DMA，与上面几项的性质不同");
			mlx5dv_devx_free_uar(uar);
		}
	}

	printf("\n==================== 小结 ====================\n");
	printf("失败项: %d\n", fails);
	note("若 [B] 成功而 verbs 的 ibv_create_cq 失败，说明队列内存只要由");
	note("DPDK 自己分配就能注册 —— 补丁可以只在 DPDK 侧落地。");

	ibv_close_device(ctx);
	ibv_free_device_list(devs);
	return fails ? 1 : 0;
}
