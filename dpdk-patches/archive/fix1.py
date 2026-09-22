"""
修复审查第 1、3、4 条。

第 1 条（最严重，是我引入的功能回归）：
    txq->wqe_pi 与 wqe_ci 是**滚动 16 位序号**，参与差值运算
    （(uint16_t)(wqe_ci - wqe_comp) >= wqe_thres），只在取用时才
    用 wqe_m 掩码定位环内位置。把 wqe_pi 本身掩成环索引会破坏
    差值语义：队列逻辑回绕后 wqe_ci - wqe_pi 可能超过 wqe_s，
    导致队列误判满或资源计数下溢。

    原代码本来是正确的。正确做法是保留完整计数器，改为校验
    新旧计数之间的距离是否合法。

第 3 条：cvm_set_decrypted() 的回滚路径里，set_memory_encrypted()
    失败只打日志，随后调用方仍执行 cvm_free_chunks()，把仍处于
    shared 状态的页还给伙伴系统 —— 正是 ISSUES.md B4 里写明
    "绝不能做"的事。正常释放路径做了保护，回滚路径漏了。

第 4 条：cvm.inited 在函数开头就置 true，导致第一次失败之后，
    第二个设备的探测会返回成功且 enabled=false，静默落回私有内存，
    违反"严格模式失败即停"。改为显式状态机。
"""
import os

# ================================================================== #
# 第 1 条：撤销 wqe_pi 掩码，改为距离校验
# ================================================================== #
p = os.path.expanduser('~/dpdk-stable-25.11.3/drivers/net/mlx5/mlx5_tx.c')
s = open(p).read()

old = '''		/*
		 * wqe_counter 来自共享页（宿主机可写），被用作 WQE 环
		 * 的生产者索引。按环掩码取模，使异常值不会转化为
		 * 越界访问。
		 */
		txq->wqe_pi = rte_be_to_cpu_16(last_cqe->wqe_counter) &
			      txq->wqe_m;'''
new = '''		/*
		 * wqe_counter 来自共享页（宿主机可写）。
		 *
		 * 注意它是**滚动 16 位序号**，与 wqe_ci 配对做差值运算
		 * （见 mlx5_tx.h 里 (uint16_t)(wqe_ci - wqe_comp) 的用法），
		 * 只在定位环内位置时才用 wqe_m 掩码。因此**不能**把它本身
		 * 掩成环索引 —— 那会在队列逻辑回绕后破坏差值语义，
		 * 导致队列误判满或资源计数下溢。
		 *
		 * 正确的防护是校验新旧计数之间的距离：已完成的 WQE 数
		 * 不可能超过在途上限 wqe_s。超出即说明该 CQE 不可信，
		 * 保持原值不动。
		 */
		uint16_t new_pi = rte_be_to_cpu_16(last_cqe->wqe_counter);

		if (likely((uint16_t)(txq->wqe_ci - new_pi) <= txq->wqe_s)) {
			txq->wqe_pi = new_pi;
		} else {
			DRV_LOG(DEBUG,
				"tx: 丢弃不可信的 wqe_counter %u"
				"（wqe_ci=%u, 在途上限=%u）",
				new_pi, txq->wqe_ci, txq->wqe_s);
			return;
		}'''
assert old in s, '第1条: TX wqe_pi 未匹配'
s = s.replace(old, new, 1)
open(p, 'w').write(s)
print('第1条: mlx5_tx.c 撤销掩码，改为距离校验')

# ================================================================== #
# 第 3 条：回滚失败的页不得还给分配器
# ================================================================== #
p = os.path.expanduser('~/snp-dpdk/kmod/cvm_shared.c')
s = open(p).read()

old = '''			/* 已转的部分回滚，避免留下状态不一致的页 */
			while (i--) {
				addr = (unsigned long)page_address(buf->chunks[i].page);
				npages = 1 << buf->chunks[i].order;
				if (set_memory_encrypted(addr, npages))
					pr_err("rollback set_memory_encrypted failed at %u\\n", i);
			}'''
new = '''			/*
			 * 已转的部分回滚。转回失败的页**绝不能**还给伙伴
			 * 系统 —— 它仍处于 shared 状态，被当私有内存复用时
			 * 会触发 RMP 不匹配，表现为难以排查的随机崩溃。
			 * 与正常释放路径一致：置空指针，故意泄漏。
			 */
			while (i--) {
				addr = (unsigned long)page_address(buf->chunks[i].page);
				npages = 1 << buf->chunks[i].order;
				if (set_memory_encrypted(addr, npages)) {
					pr_err("rollback set_memory_encrypted failed at chunk %u; leaking pages\\n",
					       i);
					buf->chunks[i].page = NULL;
				}
			}'''
assert old in s, '第3条: 回滚路径未匹配'
s = s.replace(old, new, 1)
open(p, 'w').write(s)
print('第3条: cvm_shared.c 回滚失败的页改为泄漏而非释放')

# ================================================================== #
# 第 4 条：初始化失败必须粘性
# ================================================================== #
p = os.path.expanduser('~/dpdk-stable-25.11.3/drivers/common/mlx5/linux/mlx5_cvm_shared.c')
s = open(p).read()

old_state = '''static struct {
	bool inited;
	bool enabled;          /* CVM 严格模式 */'''
new_state = '''/*
 * 初始化状态。必须是粘性的：一旦判定为永久失败，后续设备探测
 * 也要失败，不能静默落回私有内存 —— 那会让一部分 DMA 内存位于
 * 私有页上，失败推迟到数据面。
 */
enum cvm_state {
	CVM_UNINIT = 0,   /* 尚未初始化 */
	CVM_NOT_CVM,      /* 确认非机密虚拟机，走原路 */
	CVM_ENABLED,      /* 严格模式已就绪 */
	CVM_FAILED,       /* 检测到 cvm_shared 但初始化失败，永久失败 */
};

static struct {
	enum cvm_state state;
	bool enabled;          /* CVM 严格模式 */'''
assert old_state in s, '第4条: 状态结构未匹配'
s = s.replace(old_state, new_state, 1)

old_init = '''	if (cvm.inited)
		return 0;
	cvm.inited = true;

	/*
	 * 模式在此处确定一次，之后不再改变。
	 */
	fd = open(CVM_SHARED_DEV, O_RDWR);
	if (fd < 0) {
		DRV_LOG(DEBUG, "未检测到 %s，按非 CVM 环境运行", CVM_SHARED_DEV);
		cvm.enabled = false;
		return 0;
	}
	close(fd);

	ret = cvm_region_create(size ? size : CVM_DEFAULT_REGION_SIZE);
	if (ret != 0) {
		/*
		 * 检测到 cvm_shared 却建不起共享区，是致命错误：
		 * 继续用普通内存必然在注册阶段失败，不如就地报错。
		 */
		DRV_LOG(ERR, "检测到 cvm_shared 但共享区创建失败，无法继续");
		return ret;
	}
	cvm.enabled = true;'''
new_init = '''	/* 状态是粘性的：失败一次之后，后续探测也必须失败 */
	switch (cvm.state) {
	case CVM_ENABLED:
	case CVM_NOT_CVM:
		return 0;
	case CVM_FAILED:
		DRV_LOG(ERR,
			"共享 DMA 内存此前初始化失败，本进程不再尝试。"
			"继续使用私有内存必然在注册阶段失败。");
		return -ENODEV;
	case CVM_UNINIT:
		break;
	}

	fd = open(CVM_SHARED_DEV, O_RDWR);
	if (fd < 0) {
		DRV_LOG(DEBUG, "未检测到 %s，按非 CVM 环境运行", CVM_SHARED_DEV);
		cvm.state = CVM_NOT_CVM;
		cvm.enabled = false;
		return 0;
	}
	close(fd);

	ret = cvm_region_create(size ? size : CVM_DEFAULT_REGION_SIZE);
	if (ret != 0) {
		/*
		 * 检测到 cvm_shared 却建不起共享区，是致命且不可恢复的：
		 * 标记为永久失败，后续设备探测一并拒绝。
		 */
		DRV_LOG(ERR, "检测到 cvm_shared 但共享区创建失败，无法继续");
		cvm.state = CVM_FAILED;
		return ret;
	}
	cvm.state = CVM_ENABLED;
	cvm.enabled = true;'''
assert old_init in s, '第4条: init 函数体未匹配'
s = s.replace(old_init, new_init, 1)
open(p, 'w').write(s)
print('第4条: mlx5_cvm_shared.c 改为显式状态机，失败粘性')
