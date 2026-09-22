# cvm_shared 模块：问题清单

记录写这个模块过程中遇到的、以及我知道但暂时没处理的所有问题。
按「已解决 / 设计取舍 / 已知缺陷 / 未验证」分组。

---

## A. 写的过程中撞到并已解决

### A1. modpost 报 DMA_BUF 命名空间未导入
`dma_buf_export` 等符号自 5.16 起归入 `DMA_BUF` 命名空间，模块必须
`MODULE_IMPORT_NS(...)`。**6.13 之后这个宏的参数要加引号**，写成
`MODULE_IMPORT_NS(DMA_BUF)` 编不过，必须 `MODULE_IMPORT_NS("DMA_BUF")`。

### A2. sync.sh 的 --delete 删掉了 CVM 侧的编译产物
`.ko` 是在 CVM 里编的，主机没有，`rsync --delete` 每次同步都把它删了，
导致 `insmod` 报 "No such file or directory"（这个报错信息很有误导性，
看起来像符号缺失）。已在 sync.sh 加 exclude。

### A3. fd 泄漏
最初用 `dma_buf_fd()`，它内部直接 `fd_install`。若之后 `copy_to_user`
失败，fd 已进入进程 fd 表而用户态拿不到编号 —— 泄漏且无法回收。
已改为 `get_unused_fd_flags()` + 延迟到 `copy_to_user` 成功后再
`fd_install()`，失败路径用 `put_unused_fd()` 干净回滚。

### A4. 用户态映射的 C-bit
`set_memory_decrypted()` 只改了内核直接映射那一份。新建的用户态映射默认
带 `_PAGE_ENC`，CPU 通过它读到的会是「把明文当密文解出来」的垃圾。
必须在 mmap 回调里 `vma->vm_page_prot = pgprot_decrypted(vma->vm_page_prot)`。
**已实测验证**：shared-probe 的 CPU 读写校验通过。

---

## B. 有意的设计取舍（不是 bug，但要知道）

### B1. 直接返回物理地址，依赖「guest 无 vIOMMU」
`cvm_build_sgt()` 手工填 `sg_dma_address = page_to_phys(page)`，绕开
`dma_map_sgtable()`。这正是本模块区别于系统 dma-heap 的关键，也是它能
突破 swiotlb 1MB 限制的原因。

**前提是 guest 里没有 vIOMMU**（本环境 `/sys/kernel/iommu_groups` 为空，
GPA 即设备地址）。若将来给 guest 加了 vIOMMU，这里必须改成申请 IOVA
并建立映射，否则设备会访问到错误的地址 —— 而且很可能是静默的错误。

### B2. `sgt.nents` 手工设置
正常流程里 `nents` 由 `dma_map_sgtable()` 设置。我们没调它，所以自己设成
块数。目前 mlx5 的导入方接受，但这是在依赖「importer 只读 nents 不做
额外校验」这个行为。

### B3. 2MB 分配粒度
`CVM_MAX_ORDER 9`，即优先按 2MB 成块，与 DPDK 大页形态一致。
代价：256MB 会拆成 128 个块，sg_table 条目多，影响网卡 MTT 大小和注册
耗时。实测 256MB 分配耗时约 0.7 秒（逐页做 page state change）。
启动时一次性开销，可接受，但如果要分配几个 GB 需要重新评估。

### B4. `set_memory_encrypted` 失败时故意泄漏页
转回私有失败的话，页会以共享状态被还给伙伴系统，之后被当私有内存复用
时会触发 RMP 不匹配 —— 那是很难排查的随机崩溃。所以选择报错并把
`page` 指针置 NULL，故意不释放。宁可泄漏内存，不可污染分配器。

### B5. 要求 CAP_SYS_ADMIN + 设备节点 0600
制造共享内存等于在机密边界上开口子。对比：dpdk-test-main 的
`/proc/setshared` 是 **0777，任何用户可写**，且能对任意地址操作。

---

## C. 已知缺陷（暂未处理，我认为不阻塞当前实验）

### C1. 没有内存用量记账
单次上限 1GB（`CVM_MAX_SIZE`），但没有总量限制、没有 per-process 配额。
有 CAP_SYS_ADMIN 的进程可以反复分配直到打爆 guest 内存。
正式版需要加 cgroup 记账或全局水位。

### C2. LAYOUT ioctl 把物理地址暴露给用户态
`CVM_SHARED_IOC_LAYOUT` 是为了这次实验人工核对地址加的。
**正式版必须删掉** —— 向用户态泄露物理地址是信息泄露，会削弱 KASLR 之类
的防护。

### C3. VM_PFNMAP：这段内存不能走 ibv_reg_mr —— 对 DPDK 有硬性约束
分块调用 `remap_pfn_range` 会给 vma 打上 `VM_IO|VM_PFNMAP|VM_DONTEXPAND`，
这段用户内存**不能被 `get_user_pages()` pin 住**。

注册只能走 dmabuf fd 这条路（`devx_umem_reg_ex` / `ibv_reg_dmabuf_mr`），
**按用户 VA 的 `ibv_reg_mr` 一定会失败**。

对 DPDK 侧的直接要求：

- **MR 缓存必须预先填充。** 在队列启动前就把共享区的「地址范围 → lkey」
  全部登记进 `mlx5_mr_btree`。
- **收发包路径上不能出现退回普通注册的情况。** mlx5 的 MR 缓存在查不到
  地址时会走 miss 路径去现场 `ibv_reg_mr`（`mlx5_mr_create_primary` 一类）。
  对共享区内存这条退路是死路，而且失败发生在数据面，表现为丢包或卡死，
  不是启动时的明确报错。
- 因此需要在 miss 路径上对共享区地址加一个显式拒绝 + 告警，
  让它在开发期就暴露，而不是静默退化。

### C4. 没有 NUMA 亲和
`alloc_pages()` 没指定 node，拿到的页可能跨 NUMA。这台机器是双路 EPYC，
网卡在 NUMA node 1（`/sys/bus/pci/devices/.../numa_node` 可查）。
跨 node 的 DMA 会有明显性能损失。正式版应加 node 参数。

### C5. `set_memory_decrypted` 会拆分直接映射的大页
把一段内存转成共享，内核需要在直接映射里把覆盖它的 1GB/2MB 页表项拆成
小页。这是不可逆的，会永久降低宿主内核直接映射的 TLB 效率。
一次性代价，但分配-释放反复做会持续劣化。

### C6. 没实现 pin/unpin/move_notify
本模块是静态（非 dynamic）exporter。目前 mlx5 走的是 pinned 导入路径，
所以能工作。如果将来的 mlx5 或内核版本要求 dynamic attachment
（带 `move_notify` 回调），注册会失败，需要补这几个 ops。

---

## D. 尚未验证（按重要性排序）

### ~~D1. 注册成功 ≠ DMA 真的落地~~ —— 已验证通过（2026-09-21）
用 `nop-probe` 建起最小发送通路并投递 NOP 任务单，完整链路走通：

```
网卡 DMA 读 WQE（共享内存 +0x10000）
网卡 DMA 读 doorbell record（共享内存 +0x20000）
网卡处理 NOP
网卡 DMA 写 CQE（共享内存 +0x0）
CPU 经 mmap 读到 CQE
```

不可伪造的判据：

| 字段 | 值 | 含义 |
|---|---|---|
| `sop_drop_qpn` | 4682 | **等于本次创建的 sqn**，每次运行都跟着变 |
| `timestamp` | 0x68c9795c9d772 | 非零硬件时间戳 |
| `wqe_counter` | 0 | 正是投递的 WQE 索引 |
| `op_own` | 0x00 | opcode=REQ，正常完成，owner 位已翻转 |

**教训**：中途有过两次误判 ——
1. 最初的检测条件太弱（把全零缓冲区当成「网卡写过」），假阳性；
2. 随后 `sop_drop_qpn` 的偏移写成 0x24（实际在 0x38），假阴性。

结论必须建立在不可伪造的判据上。mlx5_cqe64 尾部布局：
`0x30 timestamp(8) / 0x38 sop_drop_qpn(4) / 0x3c wqe_counter(2) /
0x3e signature(1) / 0x3f op_own(1)`。

### ~~D3. DevX 建 CQ/SQ 对象本身~~ —— 已验证通过
CQ、传输域、TIS、SQ、RST→RDY 全部成功，缓冲区均在共享内存里。
PRM 定义通过 `prm/` 垫片复用 DPDK 的 `mlx5_prm.h`，未手工抄写。

### ~~D2. 能否基于这个 umem 建 mkey~~ —— 已验证通过（2026-09-21）
`CREATE_MKEY` 以 `mkey_umem_id = umem_id`、`access_mode = MTT`、
`log_page_size = 12`、`start_addr = 0`（与 umem 注册时 `addr=0` 的偏移语义
一致）创建成功，得到 `lkey = 0x1bf601`。

lkey 的拼法照 DPDK：`(mkey_index << 8) | (umem_id & 0xff)`。

**并且实测可用**：用这个 lkey 发了一个真实以太网帧（见下）。

### ~~发送方向数据面~~ —— 已验证通过
在共享内存 +0x30000 处构造一个 60 字节以太帧（ethertype 0x88b5，
载荷 "CVM-SHARED-DMA-READ-PROOF"），投递 SEND 任务单，
数据段为 `{byte_count=60, lkey, addr=0x30000}`。

结果：
- CQE 正常（opcode=0 REQ，qpn 匹配 sqn，wqe_counter=1）
- 主机 PF0 计数器 rx_packets +1、rx_bytes +60，与构造的帧长一致
- **主机侧 tcpdump 抓到帧，载荷字节级一致**：

```
60:5e:65:e3:b8:65 > 60:5e:65:e3:b8:64, ethertype Unknown (0x88b5), length 60:
  0x0000:  605e 65e3 b864 605e 65e3 b865 88b5 4356  `^e..d`^e..e..CV
  0x0010:  4d2d 5348 4152 4544 2d44 4d41 2d52 4541  M-SHARED-DMA-REA
  0x0020:  442d 5052 4f4f 4600 0000 0000 0000 0000  D-PROOF.........
```

计数器只能证明「一个 60 字节的帧到了」，证明不了内容。
抓包证明了网卡读的确实是我们写进共享内存的那些字节，而非别处内存。

即网卡确实**从共享内存 DMA 读走了帧数据**并发上了网线。
至此收发两个方向的 DMA 都已证实：
- 写方向：网卡写 CQE 进共享内存（NOP 实验）
- 读方向：网卡读 WQE、doorbell record、以及帧数据（本实验）

### D3b. 接收方向 —— 队列全部建成，卡在**流量引导**（与 SNP 无关）

已完成：接收 CQ、RQ（缓冲区在共享内存 +0x18000）、16 个接收描述符
（缓冲区 +0x40000）、RQ 进入 RDY、TIR（DIRECT 模式直指该 RQ）
—— **全部创建成功**。

卡住的是把流量导向这个 TIR。两套 API 都试过：

| 方式 | 结果 |
|---|---|
| `mlx5dv_create_flow`（内核 verbs 流引导） | 规则下发成功，但**不匹配任何流量** |
| `mlx5dv_dr_*`（Direct Rules，DPDK 用的那套） | `dr_domain_create` 返回 **ENODEV** |

verbs 那条路做过二分诊断：把动作换成 DROP，ping 依然 0% 丢包，
说明规则根本没匹配上，问题在 TIR/RQ 的**上游**。

DR 那条路即使用 `mlx5dv_open_device()` 带 `MLX5DV_CONTEXT_FLAGS_DEVX`
打开上下文（DPDK 的做法）仍然 ENODEV，说明本环境的 SW steering 不可用。

**重要判断：这不是共享内存的问题。**
网卡对共享内存的读和写都已经独立证实：
- 写：CQE（不可伪造的 qpn + 硬件时间戳）
- 读：帧数据（主机 tcpdump 字节级一致）

RX 数据落地是又一次「写」，机制与 CQE 同源。现在缺的只是把包导过来。

**而流量引导正是 DPDK 的 mlx5 PMD 已经解决的事**（`rte_flow_isolate()`
加端口配置，带一整套能力探测和多级回退）。我们的补丁要做的是让 DPDK
现有的队列内存改用共享内存，不需要重新发明引导逻辑。所以这个阻塞点
很可能在真正做 DPDK 集成时自然消失，不值得在独立探针里继续深挖。

### D4. 多进程 / 并发
没测过两个进程同时分配、或一个 buffer 被多个 attachment 导入。

### D5. 反复分配释放的稳定性
没做过压力测试。C5 提到的直接映射拆分在反复做的时候会不会出问题，未知。

---

## 补充验证（2026-09-21，应要求追加）

### V1. 跨映射校验 —— 通过
16MB 缓冲区，两个方向各校验全部 8 字节槽位：
- 方向 A：用户态经 mmap 写入 → 内核经直接映射（`page_address`）读出，全部一致
- 方向 B：内核经直接映射写入 → 用户态经 mmap 读出，全部一致

两条路径的页表项互相独立，C-bit 任一侧处理错都会在这里暴露。

### V2. swiotlb 占用计数 —— 零变化
```
注册前 io_tlb_used = 81262 slabs (158.71 MB)
注册后 io_tlb_used = 81262 slabs (158.71 MB)
注销后 io_tlb_used = 81262 slabs
```
注册 16MB 内存，swiotlb 占用**一个 slab 都没动**。这比「dmesg 里没有
报错」强得多 —— 后者只能证明没失败，前者直接证明没经过。

### V3. IOMMU 拦截 —— 已实现为硬性检查
`cvm_attach()` 里调用 `iommu_get_domain_for_dev()`：域为 NULL 或
`IOMMU_DOMAIN_IDENTITY` 放行，其它类型返回 `-EOPNOTSUPP` 并打印
带设备名的明确原因。不再只是注释里的前提说明。

当前环境网卡没有 IOMMU 域，attach 通过。

### V4. close(fd) 先于 munmap —— 页不会被提前转回
用 `/dev/kmsg` 打标记，使用户态动作与内核事件在同一条时间线上对齐：

```
allocated 16777216 bytes
>>> BEFORE close(fd)
>>> AFTER close(fd)                    ← 此处没有 release
>>> BEFORE munmap
release: refcount dropped to zero      ← release 发生在 munmap 内部
release: done
>>> AFTER munmap
```

结论：vma 持有 dma-buf 的引用，`close(fd)` 只减一次引用计数，
页在 `munmap` 之后才转回私有。这是直接证据而非时间戳推断。

## 实测结论（2026-09-21）

| 尺寸 | cvm_shared | 系统 dma-heap |
|---|---|---|
| 0.25 MB | 成功 | 成功 |
| 1 MB | **成功** | EIO (swiotlb) |
| 4 MB | **成功** | EIO |
| 16 MB | **成功** | EIO |
| 64 MB | **成功** | EIO |
| 256 MB | **成功** | EIO |

内核日志里 `swiotlb buffer is full` 只在系统 dma-heap 那侧出现，
证明本模块的内存全程没有经过 swiotlb。

16MB 详查：8 个不连续的 2MB 块，CPU 读写校验通过，
`devx_umem_reg_ex` 成功。

---

## 里程碑：DPDK 在 CVM 内完成收发（2026-09-22）

testpmd 在 SEV-SNP CVM 里跑通端到端数据面，全部 DMA 内存位于共享页。

### 收包
```
rx_phy_packets: 12   rx_q0_packets: 12   Forward RX-packets: 12
```
账目吻合：ping 发 20，内核收 8，DPDK 抢走 12。

### 收发往返（macswap）
```
Forward statistics: RX-packets: 13  TX-packets: 13  dropped: 0
```

### 字节级证据
主机侧抓包，去程与回程逐字节对照：

```
偏移        去程(源 ...b8:64)           回程(源 ...b8:65)
0x00-0x0b   605e65e3b865 605e65e3b864 | 605e65e3b864 605e65e3b865   MAC 对调
0x0c 起     0800 4500 0054 7b60 ...   | 0800 4500 0054 7b60 ...     完全一致
0x3a 起     4841 5245 444d 454d ...   | 4841 5245 444d 454d ...     载荷一致
```

从 0x0c 到帧尾全部相同：IP 头、ICMP 头、校验和 0x9967、时间戳、
以及 CVMSHAREDMEM 重复填充的载荷。仅前 12 字节的两个 MAC 按
macswap 语义对调。

完整路径：
```
主机上线 → 网卡 DMA 写入共享页 → CPU 从共享页读出 → CPU 改写 MAC
        → 网卡 DMA 读出共享页 → 上线 → 主机抓到
```
往返 144 微秒。

### 运行方式
```bash
sudo insmod cvm_shared.ko
dpdk-testpmd -l 0-3 -n 4 -a <bdf>,dv_flow_en=2 --iova-mode=va -- -i \
    --mp-alloc=xbuf --forward-mode=macswap
# 端口起来后需 set promisc 0 on
```
