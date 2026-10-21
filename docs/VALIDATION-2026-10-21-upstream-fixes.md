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

A diagnostic variant compiled only `rte_efd.c` with `-mavx2` and relinked the
otherwise unchanged binary. Both commands also passed (16.310 and 9.892
seconds), checking that the compiled AVX2 path remains usable.

## IPsec: an incomplete mock device exposed an unchecked callback

The crash is not a failure to encrypt with the NULL crypto PMD. The IPsec
suite constructs its own dummy security context for inline-security cases.
Its operations included create/destroy callbacks but omitted
`session_get_size`. `rte_security_session_create()` checked the create
callback, then called the absent size callback unconditionally. The debugger
trace through address zero identified this call before packet processing.

[Patch 0012](../patches/dpdk-25.11.3/0012-security-session-size-validation.patch)
makes two corrections:

- The dummy device supplies a size callback returning zero, since it has no
  driver-private session data. It still allocates the common session header.
- The security library rejects a missing size callback before allocating a
  session, consistent with its validation of the create callback.

An added `security_autotest` case provides valid operations except for the
missing size callback. It checks NULL return, no create callback invocation,
no consumed mempool object, and no session-count increment. Linking that new
test against the unmodified security library reproduced SIGSEGV (return code
`-11`); linking it against the corrected library passed.

The full `ipsec_autotest --vdev=crypto_null0` passed **15/15** subcases.
`security_autotest` passed **57** subcases, including the new regression,
with **3** existing metadata-validation cases skipped in this non-debug build.
No inline-security assertions were removed or reclassified as skips.

## Reproducible checks

The complete twelve-patch series was reapplied from both recorded clean
baselines. Both resulting tree hashes match the updated manifests. The full
host build, including testpmd and the examples, succeeded, and the existing
48 SNPFlow production-code regressions passed.

CI now enables `crypto/null` and runs four upstream commands: `efd_autotest`,
`efd_perf_autotest`, `security_autotest`, and `ipsec_autotest`. The exact CI
runner also passed locally:

```sh
bash scripts/test-dpdk-upstream.sh /path/to/dpdk-build
```

Each command has a 600-second limit; errors, skips, crashes, and timeouts make
the runner fail. Per-command logs and a status table are retained as CI
artifacts. The original hardware and build-dependent upstream tests remain
outside this targeted CI group.

For code revision `61dd1fc2654e829dd524bbad6ef756054eb5c358`, both GitHub
workflows completed successfully:
[portable checks](https://github.com/keke1022/snpflow/actions/runs/39574286788)
and [DPDK integration](https://github.com/keke1022/snpflow/actions/runs/39574286790).
The latter includes the four native commands, production-code regressions,
and the existing ASan/UBSan regressions. These CI results do not imply that
every upstream test or every enabled library was sanitizer-instrumented.

Running the same runner against the preserved pre-fix software-profile
executable returned failure: both EFD commands exited 255, IPsec exited 139
(SIGSEGV), and the old security suite passed. This confirms that the CI
commands exercise the failures rather than merely accepting a successful
process startup.

## Why the other host entries failed

The original 25 host failures break down as follows. This table explains
their initial failures; it is not a revised full-suite pass count.

| Entries | Cause | Evidence / disposition |
|---:|---|---|
| 2 | Generic EFD dispatch selects unavailable AVX2 code | Fixed in 0011; both original commands pass |
| 11 | Ordinary-memory configuration cannot meet hugepage/secondary-process assumptions | All eleven pass in the earlier SNP hugepage rerun |
| 6 | Event adapter software drivers unavailable in the original build | Expanded-driver run exercises the adapters; RX interrupt support remains unavailable on `net_null` |
| 1 | Crypto scheduler disabled; Meson lists a command absent from the executable | Build configuration issue; not a passing test |
| 4 | Four performance mempools do not fit the 2-GiB budget | Allocation-only diagnostic below |
| 1 | PMD performance test has no Ethernet port | `--no-pci` run supplies no port; no NIC performance claim |

The four mempool commands share the same allocation routine. Its capacity is
`lcore_count * (32768 + 2 * 512) - 1` objects per pool: 270,335 objects for
eight lcores. It creates four 2,048-byte-object pools before benchmarking.
Even payload alone exceeds 2 GiB; ordinary 4-KiB pages and object metadata
increase the requirement further. The `1core` command still sizes its pools
using all enabled EAL lcores, so its name does not imply a small allocation.

A debugger probe on the preserved pre-fix binary reproduced allocation
failure with `-m 2048`. With `-m 6144`, both that binary and pristine DPDK
reached `do_one_mempool_test()`, which is called only after all four pools
have been created. The probe stopped there. It validates the allocation
diagnosis, **not completion of the performance tests**.

The benchmark also contains hundreds of one-second cases per cache mode;
completing it requires an appropriate long-run timeout after fixing the
memory budget. Its original failures have not been relabeled as passes.

## Timer: the first failure is still not reproducible

The follow-up used the exact preserved pre-fix patched executable and the
pristine executable on lcores **8–15**, matching the initially failing CPU
range. Each passed **31/31** cases, in 224.614 and 224.545 seconds respectively.
The earlier successful comparisons on lcores 16–23 are therefore not the
only successful runs. The new EFD and security changes were not present in
these two timer executables.

The first failing log begins with the periodic timer count missing its
one-second window; later cases also report unexpected events (for example,
16 dequeued instead of 1). The suite shares an event device across cases, so
these symptoms do not establish twelve independent implementation defects.
The software timer depends on service-lcore scheduling, but no scheduler trace
was captured during the initial failure. Current successful reruns do not
prove that scheduling caused it, nor that a particular CPU range is faulty.

The initial failure remains recorded. No timer assertions, tick values, or
timeouts were changed. Diagnosing it conclusively requires capturing the
next failure with service-lcore scheduling and event-queue state.

## Evidence and scope

The [machine-readable evidence](results/2026-10-21-upstream-fixes.json)
records commands, exit statuses, durations, and binary hashes. Local raw logs
are under `/tmp/snpflow-upstream-fixes`. Before-fix executables were copied
there before rebuilding, preserving the earlier audit's evidence.

The native tests here exercise ordinary DPDK memory and software devices.
They do not establish new xbuf/private-bridge or NIC DMA coverage.
