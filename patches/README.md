# DPDK patch series

`dpdk-25.11.3/series` is the authoritative application order. Apply the whole
series to the exact baseline; the early patches alone do not include the
validated completion guards or resource-lifetime fixes.

## Baseline and installation

Two exact baselines are supported. The recommended one is the official
[DPDK 25.11.3 release archive](https://fast.dpdk.org/rel/dpdk-25.11.3.tar.xz).
`RELEASE_SHA256` identifies the archive; `RELEASE_BASE_TREE` and
`RELEASE_RESULT_TREE` identify the complete imported and patched trees.

The original CVM checkout imported the source as local root commit
`9d1130944dfc9ad9dc75b7b4bf37fdbd68c48c2c`, **not an upstream DPDK commit ID**.
`BASE`, `BASE_TREE`, and `RESULT_TREE` retain that historical provenance.
Unlike the official release, that import replaced `.gitignore` and included
one generated `buildtools/__pycache__/coff.cpython-314.pyc` file. All other
tracked files match the fully imported release. The historical bytecode is
not distributed by SNPFlow and is not needed for new builds.

Verify the downloaded archive against `RELEASE_SHA256`. In a freshly extracted
release directory, initialize and commit the entire tree **before building**:

```sh
cd /path/to/dpdk-stable-25.11.3
git init
git add -f .
git commit -m 'Import unmodified DPDK 25.11.3'
```

The force-add is intentional only for this fresh release: upstream ignore
patterns also match shipped source/configuration files. Ordinary `git add .`
omits them and produces a different tree. Do not force-add a directory that
already contains build outputs or local files.

From this repository, run:

```sh
bash scripts/apply-dpdk-patches.sh /path/to/dpdk-stable-25.11.3
```

The script checks that the checkout is clean and its tree matches one of the
two exact baselines, applies the series with `git am`, and checks the
corresponding result tree.
It aborts and rolls back the series if patch application fails. A source tree
that differs from the recorded release is rejected before any changes.

## Contents and provenance

| Patch | Change | Original CVM source |
|---|---|---|
| 0001 | mlx5 glue support for DevX dma-buf UMEM registration | `7345b49` |
| 0002 | dma-buf-backed memory registration and DevX mkey | `7362db7` |
| 0003 | DMA allocation and HWS resources in shared memory | `79cab7b` |
| 0004 | HWS DevX CQ, testpmd external buffers, initial PMD guards | `acaa959` |
| 0005 | Explicit diagnostics for unsupported configurations | `fc1a57c` |
| 0006 | Consolidated conservative mode, RX/TX tests, fail-stop handling, fd ownership, fail-closed allocation, UMEM alignment | Exact delta `fc1a57c..8a3a43b` |
| 0007 | Synchronous private-mbuf bridge | `466d145` |
| 0008 | Authenticated chunk receiver | `470dcb6` |
| 0009 | Private CQE snapshots, guarded error/opcode handling, nonempty RX completions | SNPFlow correctness fix, 2026-10-20 |
| 0010 | Private forwarding/GSO pools, strict buffer ownership, explicit unsupported-mode checks | SNPFlow coverage extension, 2026-10-20 |

Patch 0006 folds intermediate experiments and reversions into the tested final
state. It was recovered on 2026-10-20 by reading the powered-off CVM disk and
exporting its original Git objects. The VM and disk were not modified.

The private-mbuf bridge and authenticated receiver were previously stored as
`dpdk-patches/0006-…` and `dpdk-patches/0007-…`. They are renumbered 0007 and
0008 here because the missing conservative-baseline delta is now explicit.
Their implementation is unchanged.

| Checkpoint | Git tree hash |
|---|---|
| Official release, fully imported | `893022eb662f7517dc58f3f8bd0e4b7c417138cd` |
| Official release, through 0010 | `488ff6198a811383b3d8bb71b23b09dcbe8e1d36` |
| Archived CVM import | `d09d6d6f46cef3d6234ba7f11ee7e187e0ca4e02` |
| Conservative baseline, through 0006 | `6dc438c1e5f09ee298ef7d84f1f490e19f96b352` |
| Private bridge, through 0007 | `2b6019b4092fc728203063b0a05fa3ae899d0838` |
| Authenticated receiver, through 0008 | `0284ee73f1416fe6b1ce9c9d24070eb82b776bfd` |
| CQE snapshots and private forwarding, through 0010 | `bc671f0bfef8c50f2934d3002b76afe68397ccf4` |

The tree through 0008 corresponds to the archived receiver implementation.
Patches 0009 and 0010 extend that implementation; the earlier patches remain
unchanged. Subsequent
sender changes (one-second RTO and streaming source hash) live in
`scratch/cvm-chunk-proto/cvm_chunk_send.c`; they do not change the DPDK tree.

The new regressions compile the actual PMD RX/TX source with deterministic
mutation at the validation boundary, and link the actual testpmd engines to
ring-backed Ethernet ports. Run them against a built tree with
`python3 scripts/test-dpdk-integration.py /path/to/dpdk/build` from the SNPFlow
root. See [the test guide](../tests/dpdk/README.md) for coverage and limitations.

Historical generators and early source snapshots are retained in
[`../dpdk-patches/archive/`](../dpdk-patches/archive/README.md). Do not run them
over a patched tree.
