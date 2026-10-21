# DPDK patch series

`dpdk-25.11.3/series` is the authoritative application order. Apply the whole
series to the exact baseline; the early patches alone do not include the
validated completion guards or resource-lifetime fixes.

## Baseline and installation

The original CVM checkout imported the DPDK 25.11.3 source as a local root
commit, `9d1130944dfc9ad9dc75b7b4bf37fdbd68c48c2c`. This is **not an upstream
DPDK commit ID**. `BASE` records that provenance; `BASE_TREE` identifies the
actual source tree independently of commit author and timestamp.

Start from an unmodified DPDK 25.11.3 release source tree. If using a release
archive, initialize and commit that tree before building it:

```sh
cd /path/to/dpdk-stable-25.11.3
git init
git add .
git commit -m 'Import unmodified DPDK 25.11.3'
```

From this repository, run:

```sh
bash scripts/apply-dpdk-patches.sh /path/to/dpdk-stable-25.11.3
```

The script checks that the checkout is clean and its tree matches `BASE_TREE`,
applies the series with `git am`, and compares the result with `RESULT_TREE`.
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

Patch 0006 folds intermediate experiments and reversions into the tested final
state. It was recovered on 2026-10-20 by reading the powered-off CVM disk and
exporting its original Git objects. The VM and disk were not modified.

The private-mbuf bridge and authenticated receiver were previously stored as
`dpdk-patches/0006-…` and `dpdk-patches/0007-…`. They are renumbered 0007 and
0008 here because the missing conservative-baseline delta is now explicit.
Their implementation is unchanged.

| Checkpoint | Git tree hash |
|---|---|
| Unmodified imported DPDK 25.11.3 | `d09d6d6f46cef3d6234ba7f11ee7e187e0ca4e02` |
| Conservative baseline, through 0006 | `6dc438c1e5f09ee298ef7d84f1f490e19f96b352` |
| Private bridge, through 0007 | `2b6019b4092fc728203063b0a05fa3ae899d0838` |
| Authenticated receiver, through 0008 | `0284ee73f1416fe6b1ce9c9d24070eb82b776bfd` |

The final tree corresponds to the archived receiver implementation. Subsequent
sender changes (one-second RTO and streaming source hash) live in
`scratch/cvm-chunk-proto/cvm_chunk_send.c`; they do not change the DPDK tree.

Historical generators and early source snapshots are retained in
[`../dpdk-patches/archive/`](../dpdk-patches/archive/README.md). Do not run them
over a patched tree.
