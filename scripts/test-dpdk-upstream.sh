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
cpus = sorted(os.sched_getaffinity(0))
if len(cpus) < 2:
    raise SystemExit("Software-device tests require at least two available CPUs")
print(','.join(map(str, cpus[:2])))
PY
    )
    tests+=(cryptodev_null_autotest eventdev_common_autotest eventdev_selftest_sw
        event_crypto_adapter_autotest event_dma_adapter_autotest event_eth_tx_adapter_autotest)
fi
status=0
printf 'test\texit_status\n' > "$logs/results.tsv"

for test in "${tests[@]}"; do
    args=(-l "$cpu" --no-huge -m 2048 --no-pci --no-shconf --no-telemetry)
    case "$test" in
        ipsec_autotest|cryptodev_null_autotest) args+=(--vdev=crypto_null0) ;;
        event_dma_adapter_autotest) args+=(--vdev=dma_skeleton0) ;;
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
