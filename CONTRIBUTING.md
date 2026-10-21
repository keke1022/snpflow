# Contributing

This repository keeps the CVM kernel module, DPDK integration patches, and
validation evidence together. DPDK itself is maintained as an external source
tree; do not vendor it or commit a diff containing the entire upstream tree.

## Before committing

Use Git 2.43 or newer and install the portable test dependencies listed in
[README.md](README.md), then
enable the optional local hooks:

```sh
make install-hooks
```

This sets `core.hooksPath=.githooks` in this repository's local Git config;
linked worktrees share that config. It does not change your Git identity or
global config. The installer preserves existing custom hook configurations
and active default hooks, and refuses to replace them. In that case, call
`python3 scripts/git-hooks.py check-index` from your existing pre-commit hook
and `python3 scripts/git-hooks.py pre-push` from your pre-push hook, forwarding
Git's original standard input to the latter.

- **Pre-commit:** examines staged blobs, including partially staged files,
  for whitespace errors, conflict markers, files above 10 MiB, ELF binaries,
  common build outputs/captures, local `.env` files, and private-key headers.
  `.env.example` is allowed. Patch whitespace follows `.gitattributes`.
  This is a limited credential check, not a comprehensive secret scanner.
- **Pre-push:** checks each distinct pushed commit tip (including annotated
  tags) in a temporary local clone and runs `make test` and `make check` there.
  It preserves your index, uncommitted files, and checkout, and needs no NIC,
  root, or DPDK build. Ref deletions need no tests; non-commit tags are rejected.
  The check covers pushed tips, not every intermediate historical commit.
  Git history size is checked separately by `make check`.
- Full DPDK builds, production-code tests, and sanitizers run in CI. Hardware
  tests remain a separate requirement for changes that affect device behavior.

Use `PYTHON=/usr/bin/python3 git push` if the default Python lacks
`cryptography`. Hook failures block the operation and show the failing check;
they never auto-format, stash, or stage files. Git allows explicitly bypassing
hooks with `--no-verify`; contributors should explain skipped validation in
their PR. CI must still pass.

To remove the local configuration, run `make uninstall-hooks`. Cloning the
repository does not automatically enable these hooks. For manual validation:

```sh
make test
make check
git diff --check
git diff --cached --check
git status --short
```

`make test` also exercises the hooks in disposable repositories.
`make check` checks all indexed files for prohibited content, then checks
working-tree documentation, patch consistency, and reachable Git blob sizes.
CI runs these checks against the checked-out commit.

The portable tests cover protocol logic and models. Changes to mlx5, DMA
allocation, or module lifecycle also require a build and the relevant hardware
tests in [REPRODUCING.md](docs/REPRODUCING.md). Document checks that could not be
run; do not treat a model pass as evidence of real device behavior.

## Submitting changes

Open a pull request with the problem, resulting behavior, and actual test
results. Small, focused changes are easier to review. Use descriptive commit
subjects such as `fix: reject invalid completion lengths`, `feat: ...`,
`test: ...`, or `docs: ...`; hooks do not enforce a commit-message grammar.
Keep your own author identity when contributing.

For bugs, include the revision, reproduction command, expected/actual behavior,
and relevant environment. Use [SECURITY.md](SECURITY.md) for privately reporting
suspected vulnerabilities rather than posting exploit details in public issues.

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
