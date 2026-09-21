CFLAGS ?= -O2 -g -Wall -Wextra
BINS = snp-probe devx-probe shared-probe verify-probe nop-probe

all: $(BINS)

snp-probe: snp-probe.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs

shared-probe: shared-probe.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs -lmlx5

verify-probe: verify-probe.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs -lmlx5

devx-probe: devx-probe.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs -lmlx5

clean:
	rm -f $(BINS)

.PHONY: all clean

# nop-probe 需要 DPDK 的 PRM 定义，经 prm/ 垫片引入
DPDK_MLX5 ?= $(HOME)/dpdk-stable-25.11.3/drivers/common/mlx5
nop-probe: nop-probe.c
	$(CC) $(CFLAGS) -I prm -I $(DPDK_MLX5) -I . -o $@ $< -libverbs -lmlx5
