"""
第 6 步：testpmd 的 setup_extbuf() 从共享区取外部内存。

这是应用侧的参考实现：应用自己打开 /dev/cvm_shared 申请内存，
再用 rte_pktmbuf_pool_create_extbuf() 建池。mbuf 头仍在普通内存，
只有数据区落在共享页上 —— 即方案 B 的第一阶段。

buf_iova 直接填虚拟地址：共享区以 IOVA==VA 登记进 EAL
（rte_extmem_register 的 iova_addrs 显式填 VA），mkey 的 start_addr
也用 VA，整条链路地址语义一致。因此必须带 --iova-mode=va。

MR 策略：不做整区预填，由 mlx5 在队列建立时按范围创建。
一个 dmabuf 上建多个重叠 MR 会让 btree 的区间语义变复杂，
而按需路径已验证可用。
"""
import os

p = os.path.expanduser('~/dpdk-stable-25.11.3/app/test-pmd/testpmd.c')
s = open(p).read()

# ---------------------------------------------------------------- #
# 1. 共享区申请辅助函数
# ---------------------------------------------------------------- #
helper = '''
/*
 * 机密虚拟机（SEV-SNP）下的外部内存来源。
 *
 * SNP guest 里普通内存与大页都是私有加密页，设备无法 DMA，
 * 注册阶段返回 EIO。cvm_shared 内核模块提供「已转共享且不经 swiotlb」
 * 的内存，以 dma-buf 导出。
 *
 * 这里是应用侧的用法示范：自己申请、自己登记进 EAL，
 * 再交给 rte_pktmbuf_pool_create_extbuf()。
 */
#define CVM_SHARED_DEV "/dev/cvm_shared"

struct cvm_shared_alloc_req {
	uint64_t size;
	uint32_t flags;
	int32_t fd;
};

#define CVM_SHARED_IOC_ALLOC _IOWR('C', 0x01, struct cvm_shared_alloc_req)

/* 返回映射地址，失败返回 NULL（表示本机不是机密虚拟机，走原路） */
static void *
cvm_shared_get(size_t size)
{
	struct cvm_shared_alloc_req req = { .size = size, .flags = 0, .fd = -1 };
	size_t page_sz = rte_mem_page_size();
	rte_iova_t *iovas;
	unsigned int i, n_pages;
	void *va;
	int dev;

	dev = open(CVM_SHARED_DEV, O_RDWR);
	if (dev < 0)
		return NULL;                    /* 非机密虚拟机 */
	if (ioctl(dev, CVM_SHARED_IOC_ALLOC, &req) < 0) {
		TESTPMD_LOG(ERR, "cvm_shared: 申请 %zu 字节失败: %s\\n",
			    size, strerror(errno));
		close(dev);
		return NULL;
	}
	close(dev);

	va = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, req.fd, 0);
	if (va == MAP_FAILED) {
		TESTPMD_LOG(ERR, "cvm_shared: 映射失败: %s\\n", strerror(errno));
		close(req.fd);
		return NULL;
	}

	/*
	 * 登记进 EAL，否则 mlx5 的 mlx5_mr_create() 里
	 * rte_mem_virt2memseg_list() 查不到这块内存。
	 * iova_addrs 不能传 NULL（那会置成 RTE_BAD_IOVA），
	 * 显式填 IOVA == VA，需配合 --iova-mode=va。
	 */
	n_pages = size / page_sz;
	iovas = malloc(sizeof(*iovas) * n_pages);
	if (iovas == NULL) {
		munmap(va, size);
		close(req.fd);
		return NULL;
	}
	for (i = 0; i < n_pages; i++)
		iovas[i] = (rte_iova_t)(uintptr_t)va + (rte_iova_t)i * page_sz;
	if (rte_extmem_register(va, size, iovas, n_pages, page_sz) != 0) {
		TESTPMD_LOG(ERR, "cvm_shared: rte_extmem_register 失败: %s\\n",
			    rte_strerror(rte_errno));
		free(iovas);
		munmap(va, size);
		close(req.fd);
		return NULL;
	}
	free(iovas);

	TESTPMD_LOG(INFO,
		    "cvm_shared: 外部内存就绪 va=%p size=%zu（IOVA=VA）\\n",
		    va, size);
	/* fd 故意不关：dma-buf 引用计数要等 munmap 之后才归零，
	 * 但保留 fd 可以让区域在进程生命周期内稳定存在。 */
	return va;
}

'''

anchor = '''static unsigned int
setup_extbuf(uint32_t nb_mbufs, uint16_t mbuf_sz, unsigned int socket_id,'''
assert anchor in s, 'setup_extbuf 锚点未匹配'
s = s.replace(anchor, helper + anchor, 1)

# ---------------------------------------------------------------- #
# 2. setup_extbuf 优先用共享区
# ---------------------------------------------------------------- #
old_loop = '''	for (ext_num = 0; ext_num < zone_num; ext_num++) {
		struct rte_pktmbuf_extmem *xseg = xmem + ext_num;
		const struct rte_memzone *mz;
		char mz_name[RTE_MEMZONE_NAMESIZE];
		int ret;
'''
new_loop = '''	/*
	 * 机密虚拟机下改从 cvm_shared 取内存：普通 memzone 背后是私有
	 * 加密页，设备注册会返回 EIO。一次申请全部分区，避免多次
	 * extmem_register 产生零碎区间。
	 */
	cvm_base = cvm_shared_get((size_t)EXTBUF_ZONE_SIZE * zone_num);
	if (cvm_base != NULL) {
		for (ext_num = 0; ext_num < zone_num; ext_num++) {
			struct rte_pktmbuf_extmem *xseg = xmem + ext_num;
			void *p = (uint8_t *)cvm_base +
				  (size_t)EXTBUF_ZONE_SIZE * ext_num;

			xseg->buf_ptr = p;
			/* IOVA == VA，与共享区的登记方式和 mkey 的
			 * start_addr 保持一致 */
			xseg->buf_iova = (rte_iova_t)(uintptr_t)p;
			xseg->buf_len = EXTBUF_ZONE_SIZE;
			xseg->elt_size = elt_size;
		}
		*ext_mem = xmem;
		return ext_num;
	}

	for (ext_num = 0; ext_num < zone_num; ext_num++) {
		struct rte_pktmbuf_extmem *xseg = xmem + ext_num;
		const struct rte_memzone *mz;
		char mz_name[RTE_MEMZONE_NAMESIZE];
		int ret;
'''
assert old_loop in s, 'setup_extbuf 循环未匹配'
s = s.replace(old_loop, new_loop, 1)

# 局部变量
old_decl = '''	struct rte_pktmbuf_extmem *xmem;
	unsigned int ext_num, zone_num, elt_num;
	uint16_t elt_size;
'''
new_decl = '''	struct rte_pktmbuf_extmem *xmem;
	unsigned int ext_num, zone_num, elt_num;
	uint16_t elt_size;
	void *cvm_base;
'''
assert old_decl in s, 'setup_extbuf 变量声明未匹配'
s = s.replace(old_decl, new_decl, 1)

# 需要的头文件
for inc, after in (('#include <sys/ioctl.h>', '#include <sys/types.h>'),
                   ('#include <sys/mman.h>', '#include <sys/types.h>'),
                   ('#include <fcntl.h>', '#include <sys/types.h>')):
    if inc not in s:
        assert after in s, '头文件锚点 %s 未匹配' % after
        s = s.replace(after, after + '\n' + inc, 1)

open(p, 'w').write(s)
print('testpmd.c: setup_extbuf 优先从 cvm_shared 取内存，buf_iova 填 VA')
