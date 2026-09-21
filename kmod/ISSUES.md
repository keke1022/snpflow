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

### C3. `remap_pfn_range` 带来 VM_PFNMAP 的副作用
分块调用 `remap_pfn_range` 会给 vma 打上 `VM_IO|VM_PFNMAP|VM_DONTEXPAND`。
后果是这段用户内存**不能再被 `get_user_pages()` pin 住**。
当前不影响（我们通过 dmabuf fd 注册，不走用户 VA 路径），但如果将来想让
其它组件按 VA 注册这块内存，会失败。

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

### D1. 注册成功 ≠ DMA 真的落地
这是当前最大的未知。我们证明了 `devx_umem_reg_ex` 接受这块内存，
但**没有证明网卡真的能往里写数据并且 CPU 能读到**。
要验证必须建起 CQ/QP 并实际收一个包。

### D2. 能否基于这个 umem 建 mkey
`mlx5_devx_mkey_attr` 有 `umem_id` 字段，`mlx5_devx_cmd_mkey_create()`
会把它写进 PRM 命令，Windows 版 `mlx5_os_reg_mr` 就是这么做的。
**设计上应该可行，但没实测。**

### D3. DevX 建 CQ/SQ/RQ 对象本身
需要手工拼 PRM 命令，工作量不小。

### D4. 多进程 / 并发
没测过两个进程同时分配、或一个 buffer 被多个 attachment 导入。

### D5. 反复分配释放的稳定性
没做过压力测试。C5 提到的直接映射拆分在反复做的时候会不会出问题，未知。

---

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
