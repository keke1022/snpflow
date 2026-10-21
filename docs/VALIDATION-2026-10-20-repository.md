# Repository reconstruction and portable validation — 2026-10-20

This pass reorganizes and validates the repository for publication. It does
not rerun the historical NIC/GPU performance experiments.

## Source recovery and patch equivalence

The validation VM `cvm26-snp` was powered off. Its qcow2 image and backing
image were opened read-only; only the DPDK Git metadata and source files were
extracted into a temporary directory. No VM was started and no disk contents
were modified. `git fsck --full` on the recovered repository completed without
object errors; it also found the original bridge and authenticated-chunk
export commits as dangling commits.

The local repository previously retained only the first five integration
patches, plus two ignored application patches. The missing final-baseline
delta was exported from original CVM revisions `fc1a57c..8a3a43b` and inserted
as patch 0006. The bridge and authenticated receiver became 0007 and 0008.

All eight patches applied successfully to a clean worktree of the original
DPDK 25.11.3 import. The resulting Git tree was:

```text
0284ee73f1416fe6b1ce9c9d24070eb82b776bfd
```

This exactly matches original receiver export `470dcb6`, including all tracked
files and modes. The recovered testpmd working files also matched the exported
receiver. Standalone protocol-core files are checked against the patch by
`make check`.

## Portable checks

On the repository host, with OpenSSL 3.5.3 and Python cryptography 43.0.0:

- The standalone boundary model passed all 19 cases.
- The C protocol suite passed wire encoding, integer boundaries, header and
  payload tampering, out-of-order placement, replay deduplication,
  authenticate-before-allocation/commit, corrupt replay rejection,
  cross-object isolation, ACK range, and exact reassembly checks.
- The Python fixed-cell channel model passed all 15 assertions.
- The Linux UDP sender compiled with the protocol Makefile's strict warnings.
- The full patch application reproduced the archived DPDK tree exactly.

The host lacks the guest's RDMA development/build environment. Kernel-module
and full DPDK builds, actual CQ fault injection, and NIC/GPU runs were not
repeated during this repository-only pass. Their historical results remain
in their dated reports.

## Publication hygiene

Compiled probes and kernel build outputs are ignored. Dated reports, source
experiments, and the complete patch series are versioned. Old generators and
superseded snapshots are isolated under `dpdk-patches/archive/`.

The obsolete `dpdk-patches/all.patch` was 118,863,169 bytes and remained
reachable in the original history despite its deletion from the working
tree. The original Git history was backed up outside the repository before
removing that path from history. Consequently, integration-repository commit
IDs mentioned in older reports are historical provenance. DPDK source
revision IDs and the tree hashes above are unchanged.
