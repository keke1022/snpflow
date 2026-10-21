CFLAGS ?= -O2 -g -Wall -Wextra
PYTHON ?= python3
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
	rm -f build/bounds-test
	$(MAKE) -C scratch/cvm-chunk-proto clean

.PHONY: all clean test check test-dpdk test-hooks install-hooks uninstall-hooks

# Portable checks: no NIC, GPU, kernel module, or root privileges needed.
build/bounds-test: bounds-test.c
	mkdir -p build
	$(CC) $(CFLAGS) -Werror -o $@ $<

test: build/bounds-test
	./build/bounds-test
	$(MAKE) -C scratch/cvm-chunk-proto test
	$(PYTHON) scratch/secure-kv-channel/secure_kv_channel.py
	$(MAKE) test-hooks PYTHON="$(PYTHON)"

test-hooks:
	$(PYTHON) -m unittest discover -s tests/hooks -v

install-hooks:
	$(PYTHON) scripts/git-hooks.py install

uninstall-hooks:
	$(PYTHON) scripts/git-hooks.py uninstall

check:
	$(PYTHON) scripts/git-hooks.py check-index --all
	$(PYTHON) scripts/check-repo.py

# Real DPDK code, using a previously built tree; no NIC or root needed.
test-dpdk:
	@test -n "$(DPDK_BUILD)" || { echo 'Set DPDK_BUILD=/path/to/dpdk/build'; exit 2; }
	$(PYTHON) scripts/test-dpdk-integration.py "$(DPDK_BUILD)"

# nop-probe 需要 DPDK 的 PRM 定义，经 prm/ 垫片引入
DPDK_MLX5 ?= $(HOME)/dpdk-stable-25.11.3/drivers/common/mlx5
nop-probe: nop-probe.c
	$(CC) $(CFLAGS) -I prm -I $(DPDK_MLX5) -I . -o $@ $< -libverbs -lmlx5
