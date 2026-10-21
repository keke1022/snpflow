# RX adapter and PMD test setup — 2026-10-21

This follow-up turns two previously failing test configurations into
reproducible software-device checks. The starting repository revision was
`494dcd5573e1`; the DPDK source remains the thirteen-patch series. No DPDK
implementation, assertion, benchmark workload, or test timeout was changed.
The original [full-suite audit](VALIDATION-2026-10-21-upstream-tests.md)
remains historical evidence, not a revised all-tests-pass claim.

## RX adapter: provide enough memory for the existing fixture

`event_eth_rx_adapter_autotest` allocates 524,288 mbufs for its initial port.
Its normal-memory allocation exceeds the previous 2-GiB EAL budget on this
host. With the same binary, software drivers, and two lcores:

| EAL memory | Exit status | Result |
|---|---:|---|
| `-m 2048` | 255 | Setup fails with `-ENOMEM`; no subcases execute |
| `-m 4096` | 0 | All 13 subcases pass, with no skips |

The unmodified release with the same software drivers also passes at 4 GiB.
The earlier extended-driver audit had already observed this result; this
change makes the required budget part of the maintained runner and CI.
Only this command receives 4 GiB; the other commands retain 2 GiB and run
sequentially. These allocations are additional to OS/process overhead.

## PMD: supply a software loopback port

`pmd_perf_autotest` requires an Ethernet port. A no-PCI invocation without a
virtual device still fails with exit status 255 and its missing-port message.
Adding `--vdev=net_ring0` makes the command pass on both patched and pristine
software builds. The runner uses two allowed CPUs on the same NUMA node,
matching the test's worker placement and ring-port allocation. The upstream
test supports only NUMA node IDs 0 and 1.

The diagnostic run receives 29,761,920 packets, with zero measured drops and
zero idle polls. The default ring holds 1,023 packets, so the test primes and
later drains 1,023 rather than its requested 2,048; this diagnostic remains
visible in the log. The test circulates those packets through the existing
measurement loop without reducing its target packet count. No assertion or
result check was suppressed.

This is a ring-PMD software-path check. Its advertised link speed and cycle
counts are not NIC or wire-throughput measurements, and it does not exercise
mlx5 DMA, shared buffers, or the private bridge.

## Reproduction and remaining scope

Build the complete patch series with the software-driver profile listed in
the [test guide](../tests/dpdk/README.md), then run:

```sh
bash scripts/test-dpdk-upstream.sh /path/to/dpdk-build --software
python3 scripts/test-dpdk-registration.py /path/to/dpdk-build
```

All twelve functional commands and twelve BPF commands passed locally using
the exact runners above. Skips, crashes, timeouts,
and nonzero exits still fail that run. The separate registration check covers
twelve BPF commands and expects a disabled scheduler to report a skip; the
latter is not a scheduler functional pass.

The [machine-readable record](results/2026-10-21-software-rx-pmd.json)
contains negative controls, pristine comparisons, binary hashes, and runner
outcomes. Local diagnostic logs are in `/tmp/snpflow-test-followup`; the runner
also retains logs under the build's `snpflow-upstream-tests/` directory.

RX interrupt support on `net_null`, the full long-running mempool benchmarks,
and the initial timer failure remain unresolved by this change. Hugepage
subprocess tests retain their separately recorded guest results. This is not
a new full-suite or hardware run.
