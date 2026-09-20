#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <infiniband/mlx5dv.h>
#include <infiniband/verbs.h>
#include <linux/dma-heap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static struct ibv_context *ctx;
static int dfd;
static size_t LEN = 262144;

static void t(const char *name, void *addr, uint64_t pgsz, uint64_t mask)
{
    struct mlx5dv_devx_umem_in in;
    struct mlx5dv_devx_umem *u;
    memset(&in, 0, sizeof(in));
    in.addr = addr; in.size = LEN; in.access = IBV_ACCESS_LOCAL_WRITE;
    in.pgsz_bitmap = pgsz; in.comp_mask = mask; in.dmabuf_fd = dfd;
    errno = 0;
    u = mlx5dv_devx_umem_reg_ex(ctx, &in);
    printf("  %-46s -> %s (errno=%d %s)\n", name,
           u ? "成功" : "失败", u ? 0 : errno, u ? "" : strerror(errno));
    if (u) mlx5dv_devx_umem_dereg(u);
}

int main(void)
{
    struct ibv_device **d = ibv_get_device_list(NULL);
    struct dma_heap_allocation_data req = {.len = LEN, .fd_flags = O_RDWR|O_CLOEXEC};
    int heap;
    if (!d) return 1;
    ctx = ibv_open_device(d[0]);
    if (!ctx) return 1;
    heap = open("/dev/dma_heap/system", O_RDWR);
    if (heap < 0) { perror("dma_heap"); return 1; }
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &req) < 0) { perror("alloc"); return 1; }
    close(heap); dfd = req.fd;
    printf("dmabuf fd=%d size=%zu\n", dfd, LEN);
    t("addr=0, pgsz=0,        mask=DMABUF",        (void*)0,   0,            MLX5DV_UMEM_MASK_DMABUF);
    t("addr=0, pgsz=4K,       mask=DMABUF",        (void*)0,   1ULL<<12,     MLX5DV_UMEM_MASK_DMABUF);
    t("addr=0, pgsz=4K|2M,    mask=DMABUF",        (void*)0,   (1ULL<<12)|(1ULL<<21), MLX5DV_UMEM_MASK_DMABUF);
    t("addr=0, pgsz=ALL(~0),  mask=DMABUF",        (void*)0,   ~0ULL,        MLX5DV_UMEM_MASK_DMABUF);
    t("addr=0, pgsz=ALL,      mask=0 (无DMABUF位)", (void*)0,   ~0ULL,        0);
    close(dfd);
    return 0;
}
