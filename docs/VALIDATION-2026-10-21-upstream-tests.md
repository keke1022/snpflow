# DPDK upstream test audit — 2026-10-21

This audit runs DPDK 25.11.3's own tests, separately from SNPFlow's 48
production-code regressions and two added mlx5 validation commands. The
previous green CI results did not establish that the upstream suite passed.

**The upstream tests are not all passing.** The original host build ran 246
upstream entries, with **161 passed, 25 failed, and 60 skipped**:

| Host suite | Entries | Passed | Failed | Skipped |
|---|---:|---:|---:|---:|
| Fast | 83 | 79 | 0 | 4 |
| Performance | 47 | 39 | 7 | 1 |
| Drivers | 47 | 0 | 2 | 45 |
| Debug | 9 | 9 | 0 | 0 |
| Stress | 2 | 2 | 0 | 0 |
| Ungrouped | 14 | 2 | 6 | 6 |
| Supplemental binary commands | 44 | 30 | 10 | 4 |
| **Total** | **246** | **161** | **25** | **60** |

Both long stress commands, `ring_stress_autotest` and
`soring_stress_autotest`, passed in approximately 840 seconds each.
All 25 final host failures also occurred with the unmodified DPDK release
under the same build and test conditions.

Targeted runs with additional resources and drivers gave the following
results. These overlap the host inventory and must not be added to its total:

| Additional configuration | Entries | Passed | Failed | Skipped |
|---|---:|---:|---:|---:|
| SNP guest with hugepages | 48 | 42 | 3 | 3 |
| Host with additional software drivers | 20 | 18 | 2 | 0 |

The remaining failures in these configurations also reproduced on pristine
DPDK. One software timer test initially failed and later passed on a different
CPU range; its initial failure remains recorded and unexplained. These results
do not establish universal compatibility or execution of every upstream test.

## Scope and provenance

The repository revision was `4ba7b89da938320c1b9678b517f021ac288ded17`.
The patched DPDK source tree was
`bc671f0bfef8c50f2934d3002b76afe68397ccf4` (patches 0001–0010).
The comparison build used the unmodified official 25.11.3 release archive,
verified against SHA-256
`3719acc586b310c4f60ba230683bf4f1e12c6f2f5bee11f7c01b1bbd0ded7490`.
Both builds used the same compiler, dependencies and Meson options:

```text
tests=true
platform=generic
default_library=static
enable_apps=test,test-pmd
enable_drivers=bus/*,common/mlx5,net/mlx5,net/null,net/ring,net/pcap,mempool/ring
examples=l2fwd,l3fwd
```

The host used GCC 15.2.0, Meson 1.7.0 and rdma-core 56.1, with the locally
extracted development dependencies from the previous validation. The optional
static `libmtcr_ul` dependency was linked with `-lstdc++` in both builds.
Binary SHA-256 values were:

| Build | `dpdk-test` SHA-256 |
|---|---|
| Patched | `ccfd1a804ae810b4ec0b0991ccb6992a57dc43bdf28395ea5451d410ddf59504` |
| Pristine | `798e06358d85b32b46540325089b94ec6492285882dd7bd122549cb7f154b504` |

These same executables were copied into a separate temporary directory in the
SNP guest. That run used the guest's installed userspace libraries, including
rdma-core 61.0; it was not a new guest compilation.

This is a test of the enabled build, not all possible upstream components.
The build omitted ten test source files because their components or external
dependencies were not enabled or available: `test_flow_classify.c`,
`test_link_bonding.c`,
`test_link_bonding_mode4.c`, `test_link_bonding_rssconf.c`, `test_pcapng.c`,
`test_power.c`, `test_power_cpufreq.c`, `test_power_intel_uncore.c`,
`test_power_kvm_vm.c`, and `test_rawdev.c`.

## Host execution

The host had no configured hugepages. The registered upstream tests used
eight lcores, ordinary memory (`--no-huge -m 2048`), and `--no-pci`.
An isolated `XDG_RUNTIME_DIR` allowed EAL and telemetry sockets without
colliding with another DPDK process. Tests ran outside the restricted process
sandbox because these upstream tests require local IPC.

Meson registered 204 entries. Two are SNPFlow's added mlx5 commands, leaving
202 upstream entries. An additional 44 commands were present in the binary
but omitted by Meson's no-hugepages selection. This includes BPF commands
whose registration uses `NOHUGE_OK` rather than the literal `true` expected
by the upstream test-list extractor. Those 44 were explicitly attempted with
ordinary memory; they were not silently counted as skipped or passed.

The main runs retained Meson's exit-code interpretation (0 = pass, 77 = skip,
other = fail). Fast tests had a 60-second limit rather than the upstream
10-second default to accommodate shared-host scheduling. Performance and
stress tests retained upstream limits of 600 and 900 seconds. Supplemental
host commands had a 120-second limit.

Some tests contain internally skipped subcases despite returning success.
The counts in this report describe top-level commands, not a claim that every
internal subcase executed. Performance-test success is a functional result;
timing on this shared machine is not a controlled throughput comparison.

## SNP guest and hugepages

The dedicated `cvm26-snp` VM was initially off. It was started for this audit,
then shut down normally. It has eight vCPUs, 16 GiB RAM, AMD SEV-SNP at VMPL0,
kernel `7.0.0-34-generic`, and one visible NUMA node. Its existing allocation
of 1,024 2-MiB hugepages was used without changing the configuration.

The 44 supplemental commands plus `external_mem_autotest`,
`timer_secondary_autotest`, `efd_autotest` and `efd_perf_autotest` were tested
with `-l 0-7 -m 1024`, leaving hugepages available for child processes. The
guest used its normal PA IOVA mode. These tests did not use `--mp-alloc=xbuf`,
load `cvm_shared`, or exercise the testpmd private bridge or NIC DMA path.

The final guest results were **42 passed, 3 skipped, 3 failed**. Compared with
the ordinary-memory host run, the EAL parameter tests, `memzone_autotest`,
`external_mem_autotest` and `timer_secondary_autotest` could execute
successfully with the required environment. Remaining failures were
`event_eth_tx_adapter_autotest`, `efd_autotest` and `efd_perf_autotest`;
all three also failed with the pristine executable in the guest.

The guest's three skips were `compressdev_autotest`, `dispatcher_autotest`,
and `pdcp_autotest`, with the corresponding software-driver dependencies
unavailable. All 1,024 hugepages were free again at the end; the VM was off
and its passed-through PF was back on the host's `mlx5_core` driver.

## Additional software-driver profile

A separate host build enabled these additional software drivers without
changing the DPDK source or the original build:

```text
event/sw,event/skeleton,dma/skeleton,raw/skeleton,
crypto/null,crypto/openssl,compress/zlib,net/bonding
```

Twenty selected upstream commands were exercised with this profile. They are
an additional configuration, not twenty new successes to add to the original
246-command denominator. After supplying the virtual devices and memory
required by the tests and isolating the timer rerun, **18 passed and 2 failed**.

The passes include eventdev common/self-tests, event DMA/crypto/TX adapters,
NULL/OpenSSL crypto, OpenSSL asymmetric crypto, crypto crosscheck, zlib
compression, PDCP, software DMA/rawdev, and all three bonding commands.
The crosscheck used two OpenSSL virtual-device instances; it does not compare
independent cryptographic implementations. Individual unsupported algorithms
or features may still be skipped inside a passing command.

`cryptodev_null_autotest` and `cryptodev_openssl_autotest` initially skipped
because no device instance existed. Explicit `--vdev=crypto_null0` and
`--vdev=crypto_openssl0` made them pass. Compression used
`--vdev=compress_zlib0`; PDCP used `--vdev=crypto_openssl0`; event DMA used
`--vdev=dma_skeleton0`.

The two RX adapter tests allocate 524,288 mbufs per port. With ordinary 4-KiB
pages, their pools did not fit the initial 2-GiB limit. Raising that single
test's memory limit to 4 GiB made `event_eth_rx_adapter_autotest` pass on
both patched and pristine builds. The interrupt variant still failed because
`net_null` has no RX interrupt handle (`-ENOTSUP`); it was not counted as a
passing interrupt test.

The remaining software-profile failures were:

| Command | Observed failure |
|---|---|
| `ipsec_autotest` with `crypto_null0` | SIGSEGV through a null function pointer in `rte_security_session_create`, called by the inline-crypto test |
| `event_eth_rx_intr_adapter_autotest` | Virtual NIC does not provide the required RX interrupt support |

Both remaining failures were reproduced with the software-enabled pristine
build under the same device/memory conditions.

The first patched `event_timer_adapter_test` run on lcores 8–15 failed 12 of
31 internal cases. The host was shared with other workloads; CPUs were not
reserved exclusively for these tests. The pristine comparison on lcores
16–23 passed 31/31 in 224.426 seconds; a subsequent patched run on that same CPU
range passed 31/31 in 224.570 seconds. The final table uses the latter result,
and the machine-readable record retains the initial failure. This suggests
scheduling sensitivity but does not establish the initial failure's cause.
It is not evidence of a reproducible patch-specific failure or of guaranteed
timing stability.

The IPsec failure was reproduced with the software-enabled pristine build.
A debugger run of the patched executable located the call chain at
`rte_security_session_create` → `create_sa` →
`test_ipsec_inline_crypto_inb_burst_null_null_wrapper`. No workaround or
assertion removal was applied. The expanded builds and tests used CPU ranges
separate from the original long-running stress tests, but shared the host;
their durations are not comparative performance measurements.

## Failure attribution

- **Missing drivers/devices:** crypto and event tests need drivers not enabled
  in this build. Most return skip, but some event adapter setup paths return
  failure. The scheduler command is listed by Meson even though its
  conditional registration is absent from the executable. `pmd_perf_autotest`
  requires a port; the no-PCI run supplies none. These are not passing tests.
- **Hugepages and allocation assumptions:** ordinary-memory execution cannot
  satisfy some secondary-process and memzone cases. The four mempool
  performance entries failed to allocate their pools within the host run's
  configuration; the same failures occurred in the pristine build.
- **EFD correctness:** both EFD tests produced a wrong lookup value in the
  generic build, on both host and guest, with both patched and pristine code.
  This is a real failing correctness check in the tested upstream/build
  combination. It is not classified as an expected skip.

Preprocessing `rte_efd.c` with the actual compile commands confirmed that both
generic builds use `-march=corei7` and do not define `__AVX2__`. Upstream's
runtime dispatch nevertheless selects its AVX2 lookup on this AVX2-capable
CPU. In that build, `efd_lookup_internal_avx2()` contains the upstream dummy
implementation returning zero. This explains the observed lookup mismatch
and is present in the unmodified release too.

A diagnostic patched run with `--force-max-simd-bitwidth=64` passed the initial
lookup checks and reached the table-utilization stage, but exceeded its
120-second diagnostic limit. It is recorded as a timeout, not as a passing
EFD suite. No production source or upstream assertion was changed to make
this audit pass.

Harness setup errors were recorded and corrected before interpreting product
results. An excessively long temporary socket path prevented one supplemental
BPF command from starting; it passed with a shorter path. The fake PCI
allowlist used to protect recursive subprocesses conflicts with the blocklist
test. That test passed on both guest builds with standard command arguments,
without the injected allowlist or `DPDK_TEST_PARAMS`. The first harness
failures do not count as successful test runs.

## Reproduction

Build the verified release with the options above, either pristine or with
the [complete patch series](../patches/README.md). Choose eight available
lcores rather than assuming CPU IDs 0–7 are usable on every machine. Set
`XDG_RUNTIME_DIR` to an owned directory with mode 0700. A local IPC-capable
environment is required.

```sh
meson test -C /path/to/build --no-rebuild --num-processes 1 \
  --suite fast-tests --timeout-multiplier 6 \
  --test-args='-l 0-7 --no-pci'

meson test -C /path/to/build --no-rebuild --num-processes 1 \
  --suite driver-tests --suite debug-tests \
  --test-args='-l 0-7 --no-pci --no-huge -m 2048'

meson test -C /path/to/build --no-rebuild --num-processes 1 \
  --suite perf-tests --suite stress-tests \
  --test-args='-l 0-7 --no-pci --no-huge -m 2048'
```

The fast-suite command above also runs the two added mlx5 tests on a patched
build; this audit excluded them from its upstream counts. Meson's ungrouped
entries were selected explicitly from `meson-info/intro-tests.json`.
Binary registrations missing from that inventory can be found using
`nm /path/to/build/app/dpdk-test` and the `test_register_` symbols, then run
individually, for example:

```sh
DPDK_TEST=mbuf_autotest /path/to/build/app/dpdk-test \
  -l 0-7 --no-pci --no-huge -m 2048 --file-prefix=native-mbuf
```

For the guest hugepage test, omit `--no-huge`, use `-m 1024`, and provide
permission to use the existing hugepage mount. Recursive EAL tests must be
run in an isolated test environment. Preserve full logs and exit statuses;
do not turn a missing device or timeout into a pass.

The [machine-readable results](results/2026-10-21-upstream-tests.json) contain
per-command outcomes, comparisons, and the scope of the run. Raw local logs
remain under `/tmp/snpflow-native-20261021` and the host build's `meson-logs`
directory; they are not committed because they contain large dumps and
machine-local environment details.
