#!/usr/bin/env bash
# Targeted upstream regressions for the EFD and security fixes (Linux).
set -euo pipefail

if [[ $# != 1 ]]; then
    echo "Usage: $0 /path/to/dpdk-build" >&2
    exit 2
fi

build=$(realpath -- "$1")
binary="$build/app/dpdk-test"
logs="$build/snpflow-upstream-tests"
mkdir -p -- "$logs"
cpu=$(python3 -c 'import os; print(min(os.sched_getaffinity(0)))')
status=0
printf 'test\texit_status\n' > "$logs/results.tsv"

for test in efd_autotest efd_perf_autotest security_autotest ipsec_autotest; do
    args=(-l "$cpu" --no-huge -m 2048 --no-pci --no-shconf --no-telemetry)
    if [[ "$test" == ipsec_autotest ]]; then
        args+=(--vdev=crypto_null0)
    fi
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
