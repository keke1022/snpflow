# Upstream test failure follow-up — 2026-10-21

This follows the [original upstream audit](VALIDATION-2026-10-21-upstream-tests.md).
It records targeted fixes and reruns, not a replacement full-suite result.
The original failures and skips remain in the earlier report.

## EFD: an unavailable AVX2 implementation was selected

The generic build compiles `lib/efd/rte_efd.c` with `-march=corei7`, without
`__AVX2__`. Its x86 header therefore supplies a dummy AVX2 lookup that returns
zero. However, `rte_efd_create()` previously selected that lookup whenever the
running CPU supported AVX2 and the EAL SIMD limit permitted 256-bit vectors.
That combination explains the incorrect values in both native EFD tests.
The same defect was reproduced in the unmodified release.

[Patch 0011](../patches/dpdk-25.11.3/0011-efd-generic-lookup-dispatch.patch)
adds the missing compile-time condition to the dispatcher. Generic builds
use the real scalar implementation; builds with AVX2 compiled in retain the
existing CPU and SIMD-limit checks. No test assertion, workload size, or
timeout was relaxed. This fix prioritizes correctness; it does not add
separately compiled AVX2 acceleration to generic builds.

With the original host profile, normal SIMD settings, 2 GiB ordinary memory,
and lcores 0–7, both original commands now pass:

| Command | Before | After | After duration |
|---|---|---|---:|
| `efd_autotest` | Incorrect lookup value | PASS | 20.873 s |
| `efd_perf_autotest` | Incorrect lookup value | PASS | 9.241 s |

The functional test completed all three 2,097,152-key utilization iterations
with no lost keys. Durations are observations on a shared host, not a
performance comparison with the invalid pre-fix implementation.

## Evidence and scope

The [machine-readable evidence](results/2026-10-21-upstream-fixes.json)
records commands, exit statuses, durations, and binary hashes. Local raw logs
are under `/tmp/snpflow-upstream-fixes`. Before-fix executables were copied
there before rebuilding, preserving the earlier audit's evidence.

The native tests here exercise ordinary DPDK memory and software devices.
They do not establish new xbuf/private-bridge or NIC DMA coverage.
