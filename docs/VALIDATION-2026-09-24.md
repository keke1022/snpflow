# CVM DPDK validation archive — 2026-09-24

This file freezes the last known-good observations before further exploratory
work. A recorded pass is not evidence for configurations listed under
"Not covered".

## Revisions and environment

- Local integration repository: `58aad41349447c8db1f2ada08a8aefdb26146b6a`
  (`995cdf5` is the last `cvm_shared` functional commit).
- CVM DPDK tree: branch `cvm-shared`, revision `8a3a43b`.
- CVM: `cvm26-snp`, kernel `7.0.0-31-generic`, AMD SEV-SNP.
- NIC in CVM: mlx5 `0000:04:00.0`, 100 Gb/s link.
- GPU in CVM: NVIDIA H200 NVL `0000:05:00.0`, driver `595.91.07`;
  CC state ON, GPU CC capable, GPU ready state Not Ready.
- `cvm_shared` loaded. Its DMA address model requires no translating guest
  IOMMU (or an identity domain).

## Functional passes recorded in the repository

The detailed historical output is in `kmod/ISSUES.md`.

- `verify-probe`: bidirectional CPU mapping, no SWIOTLB growth, IOMMU guard,
  close-before-unmap lifetime, LOOKUP/VMA lifetime, and 3-page SG tail bound.
- Normal DPDK build and `-Dtests=true` build.
- Isolated RX/TX boundary unit tests.
- Ten testpmd port stop/start cycles without fd growth.
- Oversized 1,090,387,968-byte shared allocation fails closed instead of
  falling back to private memory.
- Conservative CVM data path accepts only scalar SPRQ and rejects unsupported
  vector RX, CQE compression, MPRQ, scatter, LRO, and shared RxQ modes.

## Externally accounted 100 Gb/s macswap run

Configuration: 4 RX queues, 4 TX queues, 4 forwarding lcores, UDP RSS,
1500-byte IPv4 packets, 1472-byte UDP payload, `macswap`, xbuf mempool with
32,768 mbufs. The host used 16 UDP GSO flows; the CVM received ordinary
1500-byte packets.

Measured over `10.011705475 s`:

```text
Host physical TX packets      65,650,023
Host physical RX packets      65,608,680
CVM RX total                  65,650,022
CVM RX accepted               65,623,596
CVM RX missed                     26,426
CVM TX requested              65,623,596
CVM TX accepted               65,608,679
CVM TX dropped                    14,917
Host TX/RX physical discards           0
```

Accounting closes exactly:

```text
65,650,023 - 65,608,680 = 41,343
26,426 + 14,917         = 41,343
```

Stable one-second intervals reached approximately `8.12–8.13 Mpps`, or
`98.3–98.4 Gb/s` using testpmd's 1514-byte accounting. Including FCS,
preamble, and IFG, this is approximately 100 Gb/s wire occupancy. This was a
short line-rate interval, not a sustained zero-loss RFC throughput result.

## Synthetic KV-cache receive run

Artificial cache definition:

```text
2 (K/V) * 80 layers * 131072 tokens * 8 KV heads
  * 128 head dimension * 2 bytes (BF16) = 40 GiB
```

Path: host synthetic bytes -> 16 UDP/GSO flows -> mlx5 -> CVM DPDK mlx5 PMD
-> `cvm_shared` xbuf -> testpmd `rxonly`.

```text
Payload                         42,949,672,960 bytes (40 GiB)
Wall time                       3.629053964 s
Expected/host physical packets  29,177,776
DPDK RX packets                 29,177,779 (3 extra 60-byte background frames)
DPDK imissed/ierrors/rx_nombuf  0 / 0 / 0
Payload rate                    11.022 GiB/s, 94.680 Gb/s
Estimated wire occupancy        98.925 Gb/s
```

The expected KV packet and byte counts arrived in full. `rxonly` accounts and
frees mbufs; it does not compare every payload byte or place data into a model's
final tensor layout. Reproducer source is under `scratch/kv-cache-bench/`.

## Negative result kept as a design constraint

An isolated one-page experiment performed three shared-write -> private-read
transitions with `set_memory_encrypted()`. All transitions returned success,
but all three changed the page contents. Therefore a device-written CQE cannot
be preserved by flipping its page in place from shared to private on this
kernel/platform. The temporary module was unloaded and deleted; no production
path was changed.

## Not covered

- Malicious/forged CQE injection into a real mlx5 CQ.
- Vector RX, MPRQ, scatter, jumbo frames, or offload modes rejected by the
  conservative configuration.
- CUDA or UVM import of `cvm_shared` dma-bufs.
- Simultaneous NIC and GPU attachment to one shared buffer.
- Long-duration, repeated, latency, or formal zero-loss performance testing.
- GPU tensor placement, GPU-side decryption, or framework-level KV migration.
