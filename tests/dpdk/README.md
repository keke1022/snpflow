# Production-code regressions

These tests require a built, patched DPDK 25.11.3 tree. They need no NIC,
kernel module, hugepages, root privileges, or SNP guest. They use actual DPDK
mbufs, pools and forwarding functions; they do not model the PMD in a separate
implementation.

These are SNPFlow regressions, not the complete original DPDK test suite.
The separate [upstream test audit](../../docs/VALIDATION-2026-10-21-upstream-tests.md)
records the original suite's passes, skips and failures, including comparisons
against the unmodified release and supplemental SNP hugepage runs.

The [upstream failure fixes](../../docs/VALIDATION-2026-10-21-upstream-fixes.md)
add targeted EFD, security and IPsec regressions to CI. Enable `crypto/null`
alongside the build's normal drivers, apply the full patch series, and run:

```sh
bash scripts/test-dpdk-upstream.sh /path/to/dpdk/build
```

This runs four original upstream commands with a 600-second limit per
command, no NIC or hugepages, and 2 GiB of memory. It records logs and exit
statuses under the build's `snpflow-upstream-tests/` directory and fails on
skips as well as errors. This targeted group does not replace the full audit.

Apply the [complete patch series](../../patches/README.md), then build:

```sh
meson setup /path/to/dpdk/build /path/to/dpdk \
  -Dtests=true -Ddefault_library=static \
  '-Denable_drivers=bus/*,common/mlx5,net/mlx5,net/null,net/ring,mempool/ring' \
  -Denable_apps=test,test-pmd
ninja -C /path/to/dpdk/build
```

Meson, Ninja, a C compiler, binutils, pkg-config, NUMA/RDMA development files,
and OpenSSL development files are required. On Debian/Ubuntu the packages
include `meson ninja-build build-essential pkg-config libnuma-dev
libibverbs-dev libssl-dev python3-pyelftools`. If the optional installed
`libmtcr_ul.a` needs C++ symbols, add `-Dc_link_args=-lstdc++` to Meson.
OpenSSL enables the `cvmchunk` engine and its integration case.

From the SNPFlow root:

```sh
make test-dpdk DPDK_BUILD=/path/to/dpdk/build
python3 scripts/test-dpdk-integration.py /path/to/dpdk/build --sanitize
```

The runner reuses the build's compiler/linker commands, selects an allowed CPU,
disables PCI probing, and uses 256 MiB of ordinary memory. Generated executables
stay under the supplied build directory. `--only rx`, `--only tx`, and
`--only bridge` select a suite. The sanitizer mode instruments the included
PMD source, bridge, generators, checksum engine, and chunk protocol paths;
the rest of the DPDK libraries retain the original build's instrumentation.
LeakSanitizer needs permission to inspect threads; a restricted process
sandbox can prevent that check from running.

## Coverage

| Suite | Checks |
|---|---|
| RX PMD | Shared length/LRO/hash/opcode mutation after validation; owner wrap; CRC; capacity bounds; zero/CRC-only lengths; LRO/compressed/reserved formats; wrong/error opcodes; empty CQ; persistent stop and pool accounting |
| TX PMD | Shared counter mutation after validation; owner/consumer wrap; missing request; mismatched counter; wrong/error/compressed opcodes; empty CQ; no premature mbuf release |
| Bridge | Private snapshots, metadata sanitization, TX offloads, segmented packets, unknown/mixed pools, disguised shared payload, RX exhaustion, atomic TX rollback, malformed lengths, GSO indirect segments, partial TX/retry cleanup |
| Forwarding engines | `io`, `mac`, `macswap`, `5tswap`, `rxonly`, `txonly`, `flowgen`, `csum`, `icmpecho`, `noisy`, and OpenSSL-enabled `cvmchunk` |

`txonly` runs with one and two source segments; `flowgen` exercises reference
clones; `csum` runs with and without output splitting. GSO is checked through
the real segmentation library and bridge. `noisy` uses its unbuffered path.
The authenticated case sends a corrupted chunk, a valid retry, and a duplicate,
then authenticates the resulting ACKs. Every bridge case checks pool balances.

`ieee1588`, `recycle_mbufs`, and `shared_rxq` must return an explicit unsupported
result while the CVM bridge is enabled. Ordinary operation without a registered
CVM pool remains unchanged.

## Negative control

To prove the mutation tests detect the old behavior, retain an unmodified tree
through patch 0008 and run each PMD suite against that source with the current
build's compatible headers/libraries:

```sh
python3 scripts/test-dpdk-integration.py /path/to/dpdk/build \
  --only rx --pmd-source /path/to/tree-through-0008
python3 scripts/test-dpdk-integration.py /path/to/dpdk/build \
  --only tx --pmd-source /path/to/tree-through-0008
```

Both commands should fail their first assertion: RX observes the changed
packet length and TX observes the changed completion counter. Alternate-source
executables are kept separate from current-source executables.

## Limits

The external data rooms in these fixtures are ordinary host allocations, not
SNP shared pages. CQ rings and doorbells are in test memory. These tests prove
software behavior and ownership, not NIC DMA ordering or a live host attack.
Virtual forwarding exercises representative configurations, not every offload
or CLI combination. Hardware qualification and throughput measurements remain
separate from these regressions.
