> Historical exploration record. Some conclusions below were superseded.
> See the current [project README](../README.md) and the ordered
> [patch series](../patches/README.md) before reproducing a result.

# DPDK 25.11.3 mlx5：所有需要改道的 DMA 内存注册点

改道的对象不只是注册调用，而是**分配器 + 注册**这一对。
下面按「注册方式」分类，每项都标明内存由谁分配、对齐要求。

---

## 类别 A：走 `mr_scache.reg_mr_cb`（注册已改道，分配器待改）

这些是**探测阶段**就会触发的驱动内部杂项内存 —— 第一次跑 testpmd
失败在 `mlx5_flow_aso.c` 这一类，不是 mempool。

| 位置 | 用途 | 分配器 | 对齐 |
|---|---|---|---|
| `net/mlx5/mlx5_flow_aso.c:62` | ASO 位图 | `mlx5_malloc(MLX5_MEM_RTE\|ZERO, length, 4096, …)` | 4096 |
| `net/mlx5/mlx5_hws_cnt.c:299` | HWS 计数器 raw | `mlx5_malloc(MLX5_MEM_ANY\|ZERO, sz, pgsz, …)` | pgsz |
| `net/mlx5/mlx5_flow_quota.c:449` | MTR ASO 读缓冲 | `mlx5_malloc(MLX5_MEM_ANY\|ZERO, rd_buf_size, …)` | — |
| `common/mlx5/mlx5_common.c:1138` | MR 缓存自身 (`mlx5_mr_create`) | DPDK memseg | — |

注册回调已在 `mlx5_os_set_reg_mr_cb()` 切到 dma-buf 路径，
但这几处的 `mlx5_malloc` 仍来自普通堆，所以查找表会拒绝它们。

---

## 类别 B：直接调 `mlx5_os_umem_reg`（DevX 队列内存）

四处形态完全一致，且 **doorbell record 就在同一块缓冲区内**
（偏移 `umem_dbrec = RTE_ALIGN(umem_size, MLX5_DBR_SIZE)`），
所以一次分配同时覆盖队列与 doorbell。

| 位置 | 用途 | 分配器 |
|---|---|---|
| `common/mlx5/mlx5_common_devx.c:126` | CQ | `mlx5_malloc_numa_tolerant(RTE\|ZERO, size, alignment, socket)` |
| `common/mlx5/mlx5_common_devx.c:278` | SQ | `mlx5_malloc(RTE\|ZERO, size, alignment, socket)` |
| `common/mlx5/mlx5_common_devx.c:451` | QP | `mlx5_malloc(RTE\|ZERO, size, alignment, socket)` |
| `common/mlx5/mlx5_common_devx.c:558` | RQ | `mlx5_malloc_numa_tolerant(RTE\|ZERO, size, alignment, socket)` |

**对齐必须保留**：调用方传入的 `alignment` 通常是页大小，
共享区的子分配器必须支持对齐分配，否则 DevX 创建命令会失败。

---

## 类别 C：绕过 `mlx5_malloc` 的特例

| 位置 | 用途 | 分配器 | 备注 |
|---|---|---|---|
| `net/mlx5/hws/mlx5dr_send.c:754` | HWS 发送队列缓冲 | **`posix_memalign`** | 页对齐 |
| `net/mlx5/hws/mlx5dr_send.c:761` | HWS doorbell | **`posix_memalign`** | 8 字节，**独立分配** |

这两处连 `mlx5_malloc` 都不走，直接 `posix_memalign` +
`mlx5_glue->devx_umem_reg`。改道时要单独处理。

---

## 类别 D：其它

| 位置 | 用途 | 备注 |
|---|---|---|
| `net/mlx5/mlx5_devx.c:1398` | hairpin TX 队列 | `mlx5_malloc` + `mlx5_os_umem_reg` |
| `net/mlx5/mlx5_trigger.c:1185` | 连续 Tx 内存 | `mlx5_malloc` + `mlx5_os_umem_reg` |
| `net/mlx5/mlx5_flow.c:9680` | 流计数器 | `mlx5_os_wrapped_mkey_create`（Linux 版内部先调 `ibv_reg_mr`，**对共享区必失败**） |

`mlx5_os_wrapped_mkey_create` 的 Linux 实现是
`ibv_reg_mr` 再包一层 mkey，对共享区内存这条路是死的，
需要改成纯 DevX（Windows 版就是现成模板）。

---

## 关键结论

**绝大多数 DMA 内存都经由 `mlx5_malloc()`**，只有 HWS 那两处是例外。

所以改道方式建议：新增一个 DMA 专用分配入口（例如
`mlx5_malloc_dma(flags, size, align, socket)`），内部在
cvm_shared 可用时走共享区、否则退回 `mlx5_malloc`。
然后在上表各点逐一替换 —— 每处都是一行改动。

**不要**把 `mlx5_malloc` 整体改道：它还承担大量非 DMA 用途，
全量改道既浪费共享内存又不正确。

---

## 两个已确认的环境事实

### 1. `rte_extmem_register` 的 `iova_addrs` 不能传 NULL
文档明确：`Can be NULL, in which case page IOVA addresses will be set to
**RTE_BAD_IOVA**`。所以必须自己构造数组，令
`iova_addrs[i] = (rte_iova_t)(va + i * page_sz)`。

这样 IOVA == VA，与我们 mkey 用虚拟地址作 `start_addr` 的选择一致
（该选择已实测可行）。

共享区必须先 `rte_extmem_register()`，否则 `mlx5_mr_create()` 里的
`rte_mem_virt2memseg_list()` 查不到这块内存。

### 2. dummy loopback 队列不是阻塞点，但要加显式报错
`mlx5_rxq_ibv_obj_dummy_lb_create()` 开头即：

```c
if (dev->data->dev_conf.lpbk_mode) {
        ...
} else {
        return 0;
}
```

不开环回模式就直接返回，不创建任何 verbs 对象。
但一旦开启，它会走 `dv_create_cq` / `dv_create_qp` —— 在 SNP guest 里
必然失败。应在 CVM 模式下对 `lpbk_mode` 直接报错，
而不是让它在底层给出难以理解的失败。

注意：`devx_obj_ops` 里这两个回调本是 `NULL`
（`mlx5_devx.c:1755-1756`），是 `mlx5_os.c:1781` 之后又把 verbs 版本
装了回去 —— 所以 DevX 路径下依然会碰到它。

---

# 硬约束（实测得出，不可绕过）

## 1. 必须 `dv_flow_en=2`，网卡须支持 HWS

`dv_flow_en=1`（DV 流引擎）在机密虚拟机里**不可用**：它依赖
rdma-core 的 DR，而 `mlx5dv_dr_create_domain` 会失败。

strace 证实：用户态看到的是 `ENODEV`，但底层真实错误是 **EIO** ——
rdma-core 的 `dr_send.c` 用 `posix_memalign` + `devx_umem_reg` 建自己的
发送环，那是私有加密页，注册被拒；`ENODEV` 是清理路径覆盖掉的 errno。
rdma-core 不在补丁范围内，且改它也无解（verbs CQ 缓冲区要经
`ib_umem_get` pin 住，共享区内存带 `VM_PFNMAP` 过不去）。

`dv_flow_en=0`（Verbs 流引擎）同样划掉：它会把队列对象一并切回
verbs 路径，而 `ibv_create_cq` 在 SNP 下本就返回 EIO。

因此**只有 HWS 一条路**，命令行必须带 `dv_flow_en=2`，
且网卡与固件必须支持 HWS。不支持 HWS 的型号无法使用本方案。

## 2. 必须 `--iova-mode=va`

共享区以 IOVA == VA 登记进 EAL（`rte_extmem_register` 的 `iova_addrs`
显式填 VA），mkey 的 `start_addr` 也用 VA，`buf_iova` 同样填 VA。
整条链路地址语义一致，前提是 EAL 跑在 VA 模式。

注：`rte_extmem_register` 的 `iova_addrs` 不能传 NULL —— 那会置成
`RTE_BAD_IOVA`，不是 VA。

## 3. 共享区内存不能走 `ibv_reg_mr`，也不能走普通 `free()`

内核模块用 `remap_pfn_range` 建立用户映射，vma 带 `VM_PFNMAP`，
`get_user_pages` 无法 pin，按虚拟地址注册返回 **EFAULT**。
只能以 dma-buf `(fd, 偏移)` 注册。

对应地，`mlx5_malloc_dma()` 分配的内存必须用 `mlx5_free()` 释放。
交给 `free()` 会得到 `free(): invalid size` 并崩溃 —— 已实际踩到一次
（HWS 的 `close_sq` 正常路径漏改）。

## 4. 驱动与应用可能各自持有共享区

驱动在探测时申请自己的区，应用（如 testpmd 的 pinned-extbuf 池）
也可以自己申请。任何一方都掌握不了全部区间。

因此内核模块提供 `CVM_SHARED_IOC_LOOKUP`：按虚拟地址反查归属，
返回一个新的 dma-buf fd 与区内偏移。驱动在自己的表里查不到时
回落到内核查询。

---

# PMD 内存安全（B 步，无条件执行）

CQ 缓冲区位于共享页，宿主机可读可写。PMD 从中读出的若干字段被直接
当作**索引或长度**使用，取值越界的后果是内存破坏，而不是收到坏包。

这与 TOCTOU 是两回事：即便宿主机毫无恶意，一个出错的值也足以让 PMD
越界访问。因此校验无条件进行，性能数字必须包含它。

## 已加校验的点（非向量化路径）

| 位置 | 字段 | 危险 | 处理 |
|---|---|---|---|
| `mlx5_rx.c` 压缩 CQE | `byte_cnt`（此处是 mini-CQE 个数） | 用于 `zip->cq_ci = cq_ci + cqe_cnt`，**无界推进索引** | 单次读入局部变量，校验 `0 < cnt <= CQ 元素数`，越界则计 idropped 并返回 |
| `mlx5_rx.c` 压缩/非压缩 | `wqe_counter` | 用作 WQE 环索引 | 按 `(1 << elts_n) - 1` 取模 |
| `mlx5_tx.c` | `last_cqe->wqe_counter` | 用作 TX WQE 环生产者索引 | 按 `txq->wqe_m` 取模 |

`volatile` 已保证单次读取；缺的是读入之后的校验，本补丁补的就是这一层。

## 尚未覆盖

**向量化路径**（`mlx5_rxtx_vec_sse.h` / `_neon.h` / `_altivec.h`）里
mini-CQE 与 byte_cnt 的处理是 SIMD 批量展开的，校验需要不同写法，
另行处理。当前测试走的是标量路径。

**MPRQ**（`mlx5_rx.c` 的 `strd_cnt` / stride 长度）同理，
当前配置未启用 MPRQ。

## 回归结果

407 万包 macswap 往返：

```
RX-packets: 4,071,147   TX-packets: 4,071,147   TX-dropped: 0
RX-dropped: 38          RX-errors: 0            RX-nombuf: 0
```

38 个丢弃占 0.00093%，是队列满的瞬时丢弃 —— 校验失败会计入
`idropped` 并返回 0，与此不同。校验未误杀任何正常包。
