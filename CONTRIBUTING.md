# Contributing

This repository keeps the CVM kernel module, DPDK integration patches, and
validation evidence together. DPDK itself is maintained as an external source
tree; do not vendor it or commit a diff containing the entire upstream tree.

## Before committing

```sh
make test
make check
git diff --check
git status --short
```

The portable tests cover protocol logic and models. Changes to mlx5, DMA
allocation, or module lifecycle also require a build and the relevant hardware
tests in [REPRODUCING.md](docs/REPRODUCING.md). Document checks that could not be
run; do not treat a model pass as evidence of real device behavior.

## Maintaining patches

- `patches/dpdk-25.11.3/series` is the application order. Add or regenerate
  patches there, and record the base and resulting tree in `patches/README.md`.
- Check the full series against a fresh DPDK source checkout. Verify the
  resulting files, not just whether individual patches parse.
- The C protocol files under `scratch/cvm-chunk-proto/` mirror files added to
  testpmd by the authenticated-chunks patch. Update both representations.
- `dpdk-patches/archive/` contains old generators and snapshots. They are
  historical evidence and must not be used to install or regenerate the
  supported series.
- Keep binaries, kernel build outputs, captures, keys, and local environment
  files out of Git. Retain small documentation figures with their sources.

## Reporting measurements

Record the source revision, hardware and firmware, guest/kernel configuration,
queue/core counts, CPU affinity, packet size, commands, elapsed-time basis,
and counters. Separate UDP payload, L2 accounting, and on-wire rates. Include
loss, failed runs, and any exclusions. A short line-rate interval is not a
sustained zero-loss result. Keep original dated observations and add a new
report when the implementation or setup changes.

Original project material defaults to MIT, subject to the component scopes
in [LICENSING.md](LICENSING.md). Preserve existing SPDX notices, upstream
copyrights, and component licenses. DPDK integration changes remain
BSD-3-Clause; kernel-module changes remain GPL-2.0. A contribution must not
silently relicense imported or derived code under the default MIT grant.
