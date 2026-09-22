/*
 * bounds-test: 独立验证 CQE 边界校验的逻辑。
 *
 * 为什么需要它：
 *   前两轮修复我都拿"正常流量零误触发"当作校验有效的证据 —— 那只能
 *   证明没有误杀，完全不能证明拦得住坏值。两轮下来审查都发现了真实
 *   缺陷（32 位截断、剩余容量算错、累计不检查）。
 *
 *   所以先把边界逻辑抽成可独立测试的函数，用构造的边界用例跑通，
 *   再接进 PMD 的热路径。
 *
 * 覆盖的用例：
 *   - 零值、正常值
 *   - 超出剩余连续容量（环尾场景）
 *   - 32 位超范围（截断前必须拦住）
 *   - 链式压缩的累计越界
 *   - 容量为 0 的退化情形
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* ================================================================
 * 被测逻辑 —— 与将要植入 mlx5_rx.h 的版本保持一致
 * ================================================================ */

/**
 * 校验一段压缩 CQE 声明的 mini-CQE 数量。
 *
 * @param raw
 *   从 CQE 读出的**原始 32 位值**。必须在截断成 16 位之前传入 ——
 *   先赋给 uint16_t 再校验，校验的就是截断后的值，等于没校验。
 * @param cap
 *   从当前指针起的剩余**连续**容量，取 CQ 与 elts 两者的较小值。
 *   注意解压函数拿到的 cq/elts 指针已经偏移过，所以不能拿整个
 *   队列容量来比。
 * @param acc
 *   本轮批处理中已累计的 mini-CQE 数。链式压缩下每段单独合法，
 *   累计仍可能走出数组。
 * @param[out] n
 *   通过校验时输出实际数量。
 *
 * @return
 *   true 表示合法。
 */
static inline bool
mlx5_rx_mcqe_n_valid(uint32_t raw, uint16_t cap, uint16_t acc, uint16_t *n)
{
	/* raw > cap 同时覆盖了 32 位超范围：cap 最大也只有 65535 */
	if (raw == 0 || raw > (uint32_t)cap)
		return false;
	/* 累计不得走出剩余容量 */
	if ((uint32_t)acc + raw > (uint32_t)cap)
		return false;
	*n = (uint16_t)raw;
	return true;
}

/**
 * 校验包长是否落在 mbuf 实际可写空间内。
 *
 * 原先那版只检查 <= UINT16_MAX，形同虚设：mbuf 的可写空间通常
 * 只有 2KB 左右，而 UINT16_MAX 是 65535。
 *
 * @param len       CQE 声明的包长
 * @param buf_len   mbuf->buf_len
 * @param data_off  mbuf->data_off
 */
static inline bool
mlx5_rx_len_valid(uint32_t len, uint16_t buf_len, uint16_t data_off)
{
	if (data_off > buf_len)
		return false;
	return len != 0 && len <= (uint32_t)(buf_len - data_off);
}

/* ================================================================
 * 测试
 * ================================================================ */

static int fails;

static void
check(bool got, bool want, const char *what)
{
	if (got == want) {
		printf("  [ OK ] %s\n", what);
	} else {
		printf("  [FAIL] %s —— 得到 %s，期望 %s\n",
		       what, got ? "通过" : "拒绝", want ? "通过" : "拒绝");
		fails++;
	}
}

int main(void)
{
	uint16_t n;

	printf("=== mini-CQE 计数校验 ===\n");

	check(mlx5_rx_mcqe_n_valid(1, 64, 0, &n), true, "正常值 1（容量 64）");
	check(mlx5_rx_mcqe_n_valid(64, 64, 0, &n), true, "恰好用满容量");
	check(mlx5_rx_mcqe_n_valid(0, 64, 0, &n), false, "零值必须拒绝");
	check(mlx5_rx_mcqe_n_valid(65, 64, 0, &n), false, "超出容量 1");

	/* 环尾场景：整个 CQ 有 256 项，但当前指针在第 200 项，
	 * 剩余连续容量只有 56。审查第 1 点指的就是这个。 */
	check(mlx5_rx_mcqe_n_valid(200, 56, 0, &n), false,
	      "环尾：值 200 对整个 CQ 合法，但剩余容量只有 56");

	/* 32 位超范围：截断成 uint16_t 会变成合法的小值 */
	check(mlx5_rx_mcqe_n_valid(0x10001, 64, 0, &n), false,
	      "32 位超范围 0x10001（截断后会变成 1）");
	check(mlx5_rx_mcqe_n_valid(0x10000, 64, 0, &n), false,
	      "32 位超范围 0x10000（截断后会变成 0）");
	check(mlx5_rx_mcqe_n_valid(0xffffffff, 64, 0, &n), false,
	      "32 位全 1");

	/* 链式压缩：每段都合法，累计越界 */
	check(mlx5_rx_mcqe_n_valid(30, 64, 0, &n), true, "链式第一段 30");
	check(mlx5_rx_mcqe_n_valid(30, 64, 30, &n), true, "链式第二段 30（累计 60）");
	check(mlx5_rx_mcqe_n_valid(30, 64, 60, &n), false,
	      "链式第三段 30（累计 90 > 64）—— 单段合法但累计越界");

	check(mlx5_rx_mcqe_n_valid(1, 0, 0, &n), false, "剩余容量为 0");

	printf("\n=== 包长校验 ===\n");

	/* 典型 mbuf：buf_len 2176，data_off 128 → 可写 2048 */
	check(mlx5_rx_len_valid(1500, 2176, 128), true, "正常 1500 字节");
	check(mlx5_rx_len_valid(2048, 2176, 128), true, "恰好填满可写空间");
	check(mlx5_rx_len_valid(2049, 2176, 128), false, "超出可写空间 1 字节");
	check(mlx5_rx_len_valid(0, 2176, 128), false, "零长");
	check(mlx5_rx_len_valid(60000, 2176, 128), false,
	      "60000 字节 —— 旧版只查 <= UINT16_MAX 会放行");
	check(mlx5_rx_len_valid(65535, 2176, 128), false, "UINT16_MAX 必须拒绝");
	check(mlx5_rx_len_valid(100, 128, 200), false, "data_off 超过 buf_len");

	printf("\n==================== 小结 ====================\n");
	printf("失败: %d\n", fails);
	return fails ? 1 : 0;
}
