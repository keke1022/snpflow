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

## Evidence and scope

The [machine-readable evidence](results/2026-10-21-upstream-fixes.json)
records commands, exit statuses, durations, and binary hashes. Local raw logs
are under `/tmp/snpflow-upstream-fixes`. Before-fix executables were copied
there before rebuilding, preserving the earlier audit's evidence.

The native tests here exercise ordinary DPDK memory and software devices.
They do not establish new xbuf/private-bridge or NIC DMA coverage.
