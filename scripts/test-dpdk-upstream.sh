#!/usr/bin/env bash
# Targeted upstream regressions, with optional software-device coverage (Linux).
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 || ( $# == 2 && $2 != --software ) ]]; then
    echo "Usage: $0 /path/to/dpdk-build [--software]" >&2
    exit 2
fi

build=$(realpath -- "$1")
binary="$build/app/dpdk-test"
logs="$build/snpflow-upstream-tests"
mkdir -p -- "$logs"
cpu=$(python3 -c 'import os; print(min(os.sched_getaffinity(0)))')
tests=(efd_autotest efd_perf_autotest security_autotest ipsec_autotest)
if [[ ${2:-} == --software ]]; then
    cpu=$(python3 - <<'PY'
import os
from pathlib import Path

# The PMD test needs a worker on the ring port's NUMA node and supports
# socket IDs 0 and 1. Keep its main lcore and worker on the same node.
groups = {}
for cpu in sorted(os.sched_getaffinity(0)):
    nodes = list(Path(f"/sys/devices/system/cpu/cpu{cpu}").glob("node[0-9]*"))
    node = int(nodes[0].name[4:]) if nodes else 0
    if node < 2:
        groups.setdefault(node, []).append(cpu)
cpus = next((group[:2] for group in groups.values() if len(group) >= 2), None)
if cpus is None:
    raise SystemExit("Software-device tests require two available CPUs on NUMA node 0 or 1")
print(','.join(map(str, cpus)))
PY
    )
    tests+=(cryptodev_null_autotest eventdev_common_autotest eventdev_selftest_sw
        event_crypto_adapter_autotest event_dma_adapter_autotest event_eth_tx_adapter_autotest
        event_eth_rx_adapter_autotest pmd_perf_autotest)
fi
status=0
printf 'test\texit_status\n' > "$logs/results.tsv"

for test in "${tests[@]}"; do
    memory=2048
    # This suite allocates 524,288 mbufs; 2 GiB is insufficient with 4-KiB pages.
    if [[ $test == event_eth_rx_adapter_autotest ]]; then
        memory=4096
    fi
    args=(-l "$cpu" --no-huge -m "$memory" --no-pci --no-shconf --no-telemetry)
    case "$test" in
        ipsec_autotest|cryptodev_null_autotest) args+=(--vdev=crypto_null0) ;;
        event_dma_adapter_autotest) args+=(--vdev=dma_skeleton0) ;;
        pmd_perf_autotest) args+=(--vdev=net_ring0) ;;
    esac
    echo "Running $test"
    if timeout --kill-after=10s 600 env -u DPDK_TEST_PARAMS -u DPDK_TEST_SKIP \
        DPDK_TEST="$test" "$binary" "${args[@]}" > "$logs/$test.log" 2>&1; then
        code=0
    else
        code=$?
        status=1
    fi
    cat -- "$logs/$test.log"
    printf '%s\t%s\n' "$test" "$code" >> "$logs/results.tsv"
done

# A skip (77), crash, timeout, or missing device is a failed regression run.
exit "$status"
