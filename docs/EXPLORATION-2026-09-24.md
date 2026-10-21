# Shared NIC/GPU mechanism exploration — 2026-09-24

> Later result: the [2026-09-30 CUDA probe](../scratch/cuda-import-probe/README.md)
> resolved GPU readiness and tested the APIs. Preserve H7 below as the original
> observation, not the current blocker.

This document records isolated mechanism probes performed after freezing the
baseline in `VALIDATION-2026-09-24.md`. None of these probes changed the DPDK,
mlx5, CQ, `cvm_shared`, or NVIDIA production paths.

## Hypotheses and verdicts

### H1 — H200 can attach/map a `cvm_shared` dma-buf: confirmed at Linux DMA layer

A temporary misc driver called `dma_buf_attach()` and
`dma_buf_map_attachment()` with the H200 PCI `struct device`. A 2 MiB buffer
mapped successfully as one SG entry. No GPU command or DMA was submitted.

This establishes physical/exporter compatibility, not CUDA/UVM integration.

### H2 — one buffer can be attached to H200 and ConnectX concurrently: confirmed

The same 2 MiB dma-buf was mapped simultaneously to H200 `0000:05:00.0` and
ConnectX `0000:04:00.0`:

```text
GPU: nents=1 bytes=2097152 first_dma=0x112400000
NIC: nents=1 bytes=2097152 first_dma=0x112400000
layouts_equal=1
```

Both devices have no translating guest IOMMU domain. The equality therefore
matches `cvm_shared`'s current GPA/physical-address model.

### H3 — the dual attachment scales beyond one ideal 2 MiB chunk: confirmed

A 512 MiB allocation produced 256 2 MiB SG entries. H200 and ConnectX returned
identical layouts for 100 consecutive attach/map/unmap/detach cycles:

```text
size=512 MiB, iterations=100, average=12.920 us/iteration
GPU/NIC nents=256, bytes=536870912, first_dma=0x1e6800000
layouts_equal=1
```

Persistent mappings are still preferable; this test validates lifecycle and
layout behavior, not the cost or correctness of live DMA.

### H4 — a conservative shared-to-private copy can keep up with 100 GbE: confirmed with multiple cores

A resident 512 MiB shared source and 512 MiB private destination were used to
copy 16 GiB per configuration:

```text
1 thread   10.470 GiB/s    89.939 Gb/s
4 threads  34.002 GiB/s   292.075 Gb/s
8 threads  61.123 GiB/s   525.039 Gb/s
```

One core is slightly below the measured 11.022 GiB/s KV payload rate. Four
cores have roughly 3x payload-bandwidth headroom. This does not include GPU
driver submission or cache interference with a real inference workload.

### H5 — CPU AES-256-GCM can authenticate a 100 GbE stream: confirmed with multiple cores

OpenSSL 3.5.5, 1 MiB AEAD records, three-second runs:

```text
1-core encryption   10.716 GB/s   85.7 Gb/s
1-core decryption   10.659 GB/s   85.3 Gb/s
4-core encryption   42.836 GB/s  342.7 Gb/s
4-core decryption   42.170 GB/s  337.4 Gb/s
8-core decryption   84.547 GB/s  676.4 Gb/s
```

The OpenSSL speed buffers were not `cvm_shared` mappings, so this establishes
crypto compute headroom rather than complete pipeline throughput.

### H6 — AEAD alone prevents both tamper and replay: falsified

Using a real 2 MiB `cvm_shared` mapping and a 1 MiB AES-256-GCM record:

- authentic ciphertext and bound metadata passed;
- a one-bit ciphertext mutation failed authentication;
- an unchanged old ciphertext authenticated again;
- a private `expected_sequence` rejected the replay;
- a failed tag check did not advance the private sequence.

AEAD provides integrity but replay protection requires protected receiver
state. Cache ID, sequence, byte length, tensor range, and key epoch should be
bound as AAD.

### H7 — public CUDA APIs can directly import the buffer: not yet testable

The probe contains both `cuMemHostRegister()` and
`cuImportExternalMemory(DMABUF_FD)` tests, but `cuInit()` currently returns
`CUDA_ERROR_SYSTEM_NOT_READY (802)`. The H200 reports CC ON but GPU Ready State
Not Ready. No GPU state was changed or reset. CUDA documentation also limits
the public `DMABUF_FD` import path to Tegra Thor, so H200 success is not
expected without a UVM integration.

## Proposed mechanism: authenticated ciphertext ring

The combined results support this design for a next prototype:

```text
Remote sender
  -> AEAD-sealed fixed-size KV chunks
  -> ConnectX writes persistent cvm_shared slots
  -> untrusted CQE only nominates a bounded slot
  -> private control state checks slot/sequence/key epoch
  -> authenticate before committing receiver state
  -> CPU fallback copies/decrypts to private memory, or
     future UVM mapping lets H200 decrypt directly into protected HBM
```

Design rules:

1. Pre-register a fixed slot ring with both devices; do not attach per chunk.
2. Derive slot address and maximum length from private configuration, never
   directly from an untrusted CQE field.
3. Keep producer/consumer sequence, accepted key epoch, and slot ownership in
   private CVM memory.
4. Bind cache identity, model/tensor coordinates, exact length, sequence, and
   key epoch into AEAD AAD.
5. Advance private state only after authentication succeeds.
6. Keep ciphertext slots shared for their entire lifetime; never flip a live
   page private.
7. Treat CQE arrival as a hint, not authenticity evidence. Fail closed on any
   malformed length, slot, sequence, or tag.

The immediate conservative implementation is NIC -> shared ciphertext ->
four-core CPU AEAD/copy -> NVIDIA's native CC path. Direct NIC -> shared ring
-> GPU decryption remains the higher-value follow-up once GPU Ready State and
UVM mapping are available.

## Reproducers

- `scratch/dmabuf-attach-probe/`
- `scratch/cuda-import-probe/`
- `scratch/shared-copy-bench/`
- `scratch/aead-ring-probe/`
