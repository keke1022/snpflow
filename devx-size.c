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

int main(void)
{
    struct ibv_device **d = ibv_get_device_list(NULL);
    struct ibv_context *ctx;
    size_t sizes[] = {262144, 1048576, 4194304, 16777216, 67108864, 268435456};
    if (!d) return 1;
    ctx = ibv_open_device(d[0]);
    if (!ctx) return 1;
    for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        size_t L = sizes[i];
        struct dma_heap_allocation_data req = {.len=L, .fd_flags=O_RDWR|O_CLOEXEC};
        struct mlx5dv_devx_umem_in in;
        struct mlx5dv_devx_umem *u;
        int heap = open("/dev/dma_heap/system", O_RDWR);
        if (heap < 0) { perror("heap"); return 1; }
        if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &req) < 0) {
            printf("%9zu bytes (%6.1f MB)  heap分配失败: %s\n", L, L/1048576.0, strerror(errno));
            close(heap); continue;
        }
        close(heap);
        memset(&in, 0, sizeof(in));
        in.addr=(void*)0; in.size=L; in.access=IBV_ACCESS_LOCAL_WRITE;
        in.pgsz_bitmap=~0ULL; in.comp_mask=MLX5DV_UMEM_MASK_DMABUF; in.dmabuf_fd=req.fd;
        errno = 0;
        u = mlx5dv_devx_umem_reg_ex(ctx, &in);
        printf("%9zu bytes (%6.1f MB)  devx_umem_reg_ex -> %s%s%s\n", L, L/1048576.0,
               u ? "成功" : "失败", u?"":" errno=", u?"":strerror(errno));
        if (u) mlx5dv_devx_umem_dereg(u);
        close(req.fd);
    }
    return 0;
}
