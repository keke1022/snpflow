# Upstream test setup fixes — 2026-10-21

This follows the [failure analysis](VALIDATION-2026-10-21-upstream-fixes.md)
and applies small, independently verifiable fixes to test discovery and setup.
The baseline repository revision is `7260aef5ddeb`; the original full-suite
audit remains unchanged. This is targeted validation, not a new full-suite run.

## BPF tests were silently excluded

Twelve BPF registrations used the identifiers `NOHUGE_OK` and `ASAN_OK`.
The C registration macro ignores those arguments, so compilation succeeds.
However, `buildtools/get-test-suites.py` passes their spelling to Meson, which
only recognizes the literal `true`. Consequently these twelve commands were
omitted when hugepages were unavailable, and their ASan compatibility was
also misreported.

[Patch 0013](../patches/dpdk-25.11.3/0013-test-registration-metadata.patch)
uses the same literal booleans as the other upstream registrations. It changes
no BPF instructions, expected values, or test assertions.

In the local software build, Meson's inventory increased from 208 to 220
entries. All twelve restored entries have `--no-huge` arguments and all
twelve passed through `meson test`, without increasing its original timeout.
The registration regression rejects the preserved pre-fix inventory.
The earlier audit already ran these commands manually; their restoration to
Meson must not be counted as twelve additional distinct tests in that audit.

## Disabled crypto scheduler had no command

Meson extracts registrations from source text without evaluating C
preprocessor conditions. It listed `cryptodev_scheduler_autotest` even when
the scheduler driver was disabled and the command was absent from the binary.
The old binary returned failure for an unknown command.

Patch 0013 keeps the registration available and provides a disabled-driver
implementation that prints the missing condition and returns `TEST_SKIPPED`
(77). The enabled-driver implementation is unchanged. The check against the
preserved old binary fails; the corrected binary returns 77 with an explicit
message. This is a fix to availability reporting, **not a scheduler functional
pass**, and it does not enable the scheduler or its AESNI-MB worker dependency.

## Software-device prerequisites are now reproducible

The previous extended-driver audit proved that several failures were caused
by missing software devices, but those build/run settings were not in CI.
The CI build now includes `event/sw`, `event/skeleton`, and `dma/skeleton`,
alongside the already enabled `crypto/null`, `net/null`, and `net/ring`.

The optional `--software` profile selects two CPUs from the process's allowed
affinity and supplies `crypto_null0` or `dma_skeleton0` where required.
It runs the existing four EFD/security/IPsec commands plus six commands:

- `cryptodev_null_autotest`
- `eventdev_common_autotest`
- `eventdev_selftest_sw`
- `event_crypto_adapter_autotest`
- `event_dma_adapter_autotest`
- `event_eth_tx_adapter_autotest`

All ten commands passed locally with two lcores, 2 GiB ordinary memory, PCI
probing disabled, and the exact runner used by CI. Skips still fail this
functional group. Passing top-level commands may contain internally skipped
subcases for unsupported features.

```sh
bash scripts/test-dpdk-upstream.sh /path/to/dpdk-build --software
python3 scripts/test-dpdk-registration.py /path/to/dpdk-build
```

The basic four-command invocation remains supported for builds without the
extra event/DMA drivers. The registration check specifically requires a build
with the scheduler disabled. CI retains the commands' logs, status table,
Meson BPF results and scheduler availability evidence.

## Verification and limits

All thirteen patches were reapplied from each recorded clean baseline; both
resulting tree hashes match the manifests. As a separate compile-only check,
`test_cryptodev.c` also compiled with `RTE_CRYPTO_SCHEDULER` defined and the
scheduler headers available. This checks the enabled preprocessor branch;
it is not a link or runtime test of the scheduler PMD.

Code revision `09bc6e24f78081b0157de8eacd04eea1d40061e0` passed both
[portable checks](https://github.com/keke1022/snpflow/actions/runs/39576345432)
and [DPDK integration](https://github.com/keke1022/snpflow/actions/runs/39576345533)
on GitHub. The integration run includes all ten native commands, twelve BPF
commands, the missing-scheduler check, existing production-code regressions,
and existing ASan/UBSan regressions. The latter instrument SNPFlow's regression
targets, not every upstream library or BPF test.

This does not add RX interrupt support to `net_null`, configure host hugepages,
shorten the long mempool benchmarks, or alter timer deadlines. Those cases
need separate environments or further diagnosis. No NIC DMA, xbuf or private
bridge behavior was changed by this patch.

The [machine-readable results](results/2026-10-21-test-setup.json) record the
targeted outcomes and before/after checks. Raw local logs are under
`/tmp/snpflow-easy-fixes` and the software build's `meson-logs` directory.
