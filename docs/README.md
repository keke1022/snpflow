# Documentation and validation index

Start with the [project README](../README.md) for the current scope and result
summary, and [REPRODUCING.md](REPRODUCING.md) for build and test commands.
Dated reports describe the configuration tested on that date; later work can
supersede their open questions.

| Record | Contents |
|---|---|
| [2026-09-24 baseline](VALIDATION-2026-09-24.md) | Kernel checks, conservative PMD mode, externally accounted macswap, 40 GiB raw RX |
| [2026-09-24 exploration](EXPLORATION-2026-09-24.md) | NIC/GPU attachment, copy and crypto microbenchmarks, initial CUDA readiness blocker |
| [2026-09-27 bridge correctness](VALIDATION-2026-09-27-sync-mbuf-bridge.md) | Shared/private mbuf ownership and four-queue regressions |
| [2026-09-27 bridge performance](PERFORMANCE-2026-09-27-sync-mbuf-bridge.md) | One/two-copy throughput, loss, and CPU scheduling limitations |
| [2026-09-29 authenticated chunks](VALIDATION-2026-09-29-authenticated-chunks.md) | Protocol tests, fault injection, exact launch command, security limits |
| [2026-09-29 synthetic KV object](VALIDATION-2026-09-29-kv-cache-transfer.md) | 512 MiB exact private reassembly, source hash, corrected sender timeout |
| [2026-09-30 CUDA probe](../scratch/cuda-import-probe/README.md) | GPU made ready; direct import rejected; protected 64-byte H2D copy passed |
| [2026-10-20 repository validation](VALIDATION-2026-10-20-repository.md) | Read-only recovery, complete patch-series equivalence, portable test results |
| [2026-10-20 correctness and coverage](VALIDATION-2026-10-20-feature-coverage.md) | CQE mutation fixes, 48 production-code tests, forwarding/GSO hardware matrix, authenticated reruns, official-release baseline |
| [Kernel issues and history](../kmod/ISSUES.md) | Allocation and lifetime behavior, design constraints, known gaps |

## Runnable experiments

| Directory | Purpose | Hardware needed |
|---|---|---|
| [cvm-chunk-proto](../scratch/cvm-chunk-proto/README.md) | C protocol tests and Linux UDP sender | None for unit tests; CVM/NIC for end-to-end transfer |
| [secure-kv-channel](../scratch/secure-kv-channel/README.md) | Python model of snapshot/authenticate/commit invariants | None |
| [kv-cache-bench](../scratch/kv-cache-bench/README.md) | UDP/GSO synthetic traffic generator | Host and CVM/NIC |
| [shared-copy-bench](../scratch/shared-copy-bench/README.md) | Shared-to-private CPU copy bandwidth | `cvm_shared` |
| [aead-ring-probe](../scratch/aead-ring-probe/README.md) | Authentication and replay on shared memory | `cvm_shared` |
| [dmabuf-attach-probe](../scratch/dmabuf-attach-probe/README.md) | Concurrent Linux DMA attachment and SG comparison | CVM/NIC/GPU and probe module |
| [cuda-import-probe](../scratch/cuda-import-probe/README.md) | CUDA import and protected-copy visibility | Ready H200 in the tested CC environment |

Figures in `figures/` retain their generating Python sources and rendered
SVG/PNG/PDF forms. They illustrate the experiments; they are not independent
performance measurements.
