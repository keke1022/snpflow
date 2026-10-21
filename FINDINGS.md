> Historical exploration record. Some conclusions below were superseded.
> See the current [project README](README.md) and the ordered
> [patch series](patches/README.md) before reproducing a result.

# SNP guest 下 mlx5 数据面实测结论

环境：AMD EPYC 9354 / SEV-SNP guest (VMPL0) / ConnectX-7 PF1 直通 /
rdma-core 61.0 / guest kernel 7.0.0-31 / swiotlb 1024MB

## 完整测试矩阵

| # | 路径 | 内存来源 | 结果 |
|---|------|---------|------|
| 1 | verbs `ibv_reg_mr` | 普通 4KiB | ❌ EIO |
| 2 | verbs `ibv_reg_mr` | 2MiB 大页 | ❌ EIO |
| 3 | verbs `ibv_reg_mr` | dma-buf (mmap 后) | ❌ EFAULT |
| 4 | verbs `ibv_reg_dmabuf_mr` | dma-heap ≤768KB | ✅ |
| 5 | verbs `ibv_reg_dmabuf_mr` | dma-heap ≥1MB | ❌ EIO (swiotlb) |
| 6 | verbs `ibv_create_cq` | rdma-core 内部分配 | ❌ EIO |
| 7 | DevX `devx_umem_reg` | 普通内存 | ❌ EIO |
| 8 | DevX `devx_umem_reg` | **2MiB 大页（DPDK 形态）** | ❌ **EIO** |
| 9 | DevX `devx_umem_reg_ex` | dma-buf, `pgsz_bitmap=0` | ❌ EINVAL（参数错，非能力问题） |
| 10 | DevX `devx_umem_reg_ex` | dma-buf ≤768KB, `pgsz_bitmap≠0` | ✅ |
| 11 | DevX `devx_umem_reg_ex` | dma-buf ≥1MB | ❌ EIO (swiotlb) |
| 12 | DevX `devx_alloc_uar` | BAR MMIO | ✅ |

第 5/11 项的内核日志：

```
mlx5_core 0000:04:00.0: swiotlb buffer is full (sz: 1048576 bytes),
                        total 524288 (slots), used 81246 (slots)
```

总槽位远未用满，是**单次连续 1MB 的映射**分配不出来。
verbs 和 DevX 两条路撞的是同一堵墙。

## 统一结论

**在这个 SNP guest 里，网卡唯一够得着的内存是 swiotlb 的 bounce 缓冲区。**

因为那是内核唯一主动转成共享的内存。其它一切（普通内存、大页、
rdma-core 内部分配）都是私有加密页，在注册阶段就被拒绝（EIO）。
第 12 项能过是因为它走 BAR MMIO，压根不是 DMA。

三个后果：

1. **单次映射 <1MB** —— 对几十 MB 的 mbuf 池是致命的
2. **swiotlb 总量有限**（1GB，已用约 159MB）
3. **bounce 语义破坏零拷贝** —— 网卡写进 bounce 缓冲页，应用看到的是原页。
   RDMA 的 MR 是长生命周期映射，不会每包做 `dma_sync`，所以数据很可能
   根本传不到应用手里。**此条为推断，尚未实测**（探针在 create_cq 就停了）。

因此，任何可行方案都必须造出**第三类内存**：
guest 里已转共享（C-bit 清除、RMP 标记 shared）、但**不归 swiotlb 管**的内存，
让网卡直接 DMA 进应用也能看到的那一页。

**这正是 `dpdk-test-main` 那个内核模块做的事。** 实验独立地验证了它的核心前提。

## DevX 的意义（与预期不同）

原本猜想 DevX 能绕开内存问题 —— **不成立**，第 8 项证明 DPDK 自己分配的
大页同样注册失败。

但 DevX 仍然关键，原因不同：**它把队列内存放进了 DPDK 的分配器**。

| | verbs 路径 | DevX 路径 |
|---|---|---|
| CQ/WQ 缓冲区 | rdma-core 用户态分配 | `mlx5_malloc()` 由 DPDK 分配 |
| DPDK 侧挂钩能否覆盖 | **不能** | **能** |

`drivers/net/mlx5/linux/mlx5_os.c:1778` 显示 PMD 按设备能力在
`devx_obj_ops` 和 `ibv_obj_ops` 之间选择。

## 由此得出的架构

```
内核: 页翻转机制（转共享/转回，显式 opt-in，非自动）
  +
DPDK: 挂钩覆盖两处 —— mempool（mbuf）+ DevX 队列内存（CQ/WQ/doorbell）
  +
强制走 DevX 路径（verbs 路径的队列内存够不着）
```

即 `dpdk-test-main` 的架构，但要：
- 扩展到覆盖 DevX 队列内存（原版只挂了 memzone/mempool，面向 uio 网卡）
- 内核模块重写（原版的步长 bug、只刷本核 TLB、0777 proc 接口、
  无生命周期跟踪，详见对该压缩包的审阅）

## 复现

```bash
./sync.sh cvm26:~/snp-dpdk/
ssh cvm26 'cd ~/snp-dpdk && make'
ssh cvm26 'cd ~/snp-dpdk && sudo ./snp-probe [字节数]'   # verbs 路径，默认 64MB
ssh cvm26 'cd ~/snp-dpdk && sudo ./devx-probe [字节数]'  # DevX 路径，默认 256KB
```

需要 root：`/dev/dma_heap/system` 是 root-only，raw packet QP 需 CAP_NET_RAW。
DevX dma-buf 注册必须设 `pgsz_bitmap`（设 0 会得到 EINVAL，容易误判成不支持）。

## 尚未验证

- 转成共享后的内存，`devx_umem_reg` / `ibv_reg_mr` 是否就能接受（需要先有页翻转机制）
- bounce 语义是否真的导致数据传不到应用（需要能建起 CQ 才能测）
- DevX 建 CQ/SQ/RQ 对象本身是否可行（需要拼 PRM 命令）
