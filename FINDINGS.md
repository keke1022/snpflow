# SNP guest 下 mlx5 数据面实测结论

测试环境：AMD EPYC 9354 / SEV-SNP guest (VMPL0) / ConnectX-7 PF1 直通 /
rdma-core 61.0 / guest kernel 7.0.0-31 / swiotlb 1024MB

## 实测结果

| # | 操作 | 内存来源 | 结果 |
|---|------|---------|------|
| 1 | `ibv_reg_mr` | 普通 4KiB (posix_memalign) | ❌ EIO |
| 2 | `ibv_reg_mr` | 2MiB 大页 (MAP_HUGETLB) | ❌ EIO |
| 3 | `ibv_reg_mr` | dma-buf (mmap 后) | ❌ EFAULT |
| 4 | `ibv_reg_dmabuf_mr` | dma-heap ≤ 768KB | ✅ 成功 |
| 5 | `ibv_reg_dmabuf_mr` | dma-heap ≥ 1MB | ❌ EIO |
| 6 | **`ibv_create_cq`** | rdma-core 内部分配 | ❌ **EIO** |

第 5 项的内核日志明确指向 swiotlb：

```
mlx5_core 0000:04:00.0: swiotlb buffer is full (sz: 1048576 bytes),
                        total 524288 (slots), used 81550 (slots)
```

注意总槽位远未用满——是**单次连续 1MB 的映射请求**分配不出来，不是总量耗尽。
说明 dma-buf 这条路上的 DMA 是经过 swiotlb bounce 的。

第 6 项在 dmesg 里**无任何输出**，是静默 EIO。根因尚未确定
（与第 1 项同样是静默 EIO，可能同源）。

## 核心结论

**`ibv_create_cq` 失败是硬阻断。** rdma-core 在用户态自行分配 CQ/WQ 缓冲区，
这部分内存不经过 DPDK 的任何分配器。连 CQ 都建不出来，就谈不上数据面——
所以**仅让 DPDK 的 mempool/描述符环变成共享内存是不够的**。

这一条同时否定了两个方案：

- **dma-buf 路线**：数据缓冲区能注册（≤768KB），但队列建不起来，死在更前面。
- **dpdk-test-main 的挂钩位置**：它挂在 `rte_eth_dma_zone_reserve()` 和
  mempool populate。对 uio 网卡够用（DPDK 拥有全部队列内存），但 mlx5 的
  队列内存在 rdma-core 手里，挂钩根本够不着。

## 对方案选择的影响

要让 mlx5 在 SNP 下工作，必须让 rdma-core 内部分配的内存也能被网卡访问。
可能的着手点：

1. **改 rdma-core**：让 `mlx5_alloc_buf` 从可共享的内存来源分配。
   需同时维护 rdma-core 补丁和 DPDK 补丁。
2. **改内核 RDMA 子系统**：在 `ib_umem_get` pin 页时自动转共享、释放时转回。
   一处改动即可让上层（DPDK、rdma-core、testpmd）全部无感工作，
   补丁面最小，但动的是内核。
3. **换网卡类型**：用 virtio-net 等走 uio/vfio 的网卡，DPDK 拥有全部
   队列内存，`dpdk-test-main` 的挂钩位置就够用了。

第 3 点值得认真考虑：若目标只是"DPDK 跑在 host 和 CVM 之间"，
host↔CVM 的流量并不一定需要 ConnectX-7。坚持用 mlx5 意味着
额外承担 rdma-core 和/或内核的补丁工作。

## 复现方式

```bash
./sync.sh                          # 主机 → CVM
ssh cvm26 'cd ~/snp-dpdk && make'
ssh cvm26 'cd ~/snp-dpdk && sudo ./snp-probe [字节数]'   # 默认 64MB
```

需要 root：`/dev/dma_heap/system` 是 root-only，raw packet QP 需要 CAP_NET_RAW。
