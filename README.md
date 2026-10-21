# SNPFlow

**Shared-memory DMA and authenticated object transfer for DPDK inside AMD SEV-SNP confidential VMs.**

SNPFlow extends **DPDK 25.11.3's mlx5 driver and testpmd** with a shared-memory
DMA path, a shared/private packet-buffer bridge, and an authenticated chunk
transport. A companion Linux kernel module exports explicitly shared guest
memory as dma-bufs. The implementation has been tested with an
**NVIDIA ConnectX-7 PF passed through to a SEV-SNP guest on a 100 Gb/s link**.

This repository contains the kernel module, an ordered DPDK patch series,
standalone probes, protocol tests, and dated validation reports. It is a
research prototype; the measured capabilities and remaining limitations are
listed below. Hardware results were recorded on September 21–30, 2026.

## What we add to DPDK

| Component | Our extension | Purpose |
|---|---|---|
| Linux `cvm_shared` module | Shared-page allocation, dma-buf export, VMA-based address lookup, and lifetime tracking | Give the NIC DMA-accessible memory without SWIOTLB bouncing |
| mlx5 memory and DevX integration | DMA-specific allocation and dma-buf registration for packet buffers, CQ/SQ/RQ/QP memory, doorbells, and HWS resources | Make the mlx5 data path work inside the SNP guest |
| Conservative mlx5 mode | Scalar SPRQ, completion validation, exact TX completion tracking, and fail-stop handling | Bound the supported configuration and reject invalid device-written state |
| testpmd private-mbuf bridge | Copy shared RX data into private application mbufs; copy private TX data into shared DMA mbufs | Keep application processing and mbuf metadata in private guest memory |
| testpmd `cvmchunk` mode | AES-256-GCM chunks, authenticated headers and ACKs, private reassembly bitmap, duplicate handling, and sender retransmission | Authenticate data before committing it to a private object |

The original probes found that ordinary private guest memory could not be
registered for mlx5 DMA in the tested environment. System dma-heap mappings
used SWIOTLB and failed at large contiguous mapping sizes. Our module and
mlx5 changes provide a separate, explicitly shared DMA allocation path.

The authenticated receive path is:

```text
Host sender: object -> AES-256-GCM chunks -> Ethernet / IPv4 / UDP
                                                |
ConnectX-7 -> shared DMA mbuf -> private packet snapshot
                                                |
                            authenticate + decrypt -> private object
                                                |
                            authenticated ACK -> shared TX mbuf -> NIC
```

Shared DMA pages remain untrusted. Copying creates a private snapshot;
authentication determines whether its contents may update the object. This
path currently ends in CPU private memory. GPU transfer is a separate probe,
not part of the complete authenticated transport.

## Tested environment

| Item | Recorded configuration |
|---|---|
| CPU / confidential VM | AMD EPYC 9354; AMD SEV-SNP, VMPL0 |
| NIC | NVIDIA ConnectX-7, PF1 passthrough; mlx5; guest BDF `0000:04:00.0` |
| Link / packet format | 100 Gb/s; IPv4/UDP; 1,500-byte IP packets, 1,472-byte UDP payloads |
| Guest kernel | `7.0.0-31-generic` |
| DPDK / RDMA userspace | DPDK `25.11.3`; rdma-core `61.0` |
| mlx5 / EAL configuration | DevX + HWS (`dv_flow_en=2`), `--iova-mode=va`, `--mp-alloc=xbuf` |
| Guest DMA translation | No translating guest IOMMU; the module rejects non-identity domains |
| Bulk traffic tests | Four RX/TX queues and four forwarding lcores; 16 host UDP/GSO flows |
| Authenticated object tests | One RX/TX queue and one forwarding lcore; OpenSSL `3.5.5` |
| Optional GPU probe | NVIDIA H200 NVL; driver `595.91.07`; CC mode ON |

These results establish behavior on this configuration. Other NIC models,
firmware revisions, guest kernels, and DPDK releases have not been qualified.
The NIC firmware version was not captured in the archived reports.

## Test cases and results

### Functional correctness

| Test case | Acceptance criterion | Recorded result |
|---|---|---|
| Shared-memory mechanism (`verify-probe`) | Bidirectional CPU mapping, zero SWIOTLB growth, IOMMU guard, fd/mmap lifetime, LOOKUP lifetime, SG tail bounds | All six checks passed |
| NIC DMA and macswap | Hardware completion matches the submitted operation; returned packet differs only in swapped MAC addresses | Byte-level round trip passed |
| Resource lifecycle | Repeated port stop/start without fd growth; oversized shared allocation fails closed | Ten restart cycles passed; allocation above 1 GiB rejected |
| Private-mbuf bridge, four queues | RX copy and RX/private-processing/TX copy complete without errors or drops | RX-only: 729,448 packets; macswap: 91,184 RX and TX packets; zero misses/errors/drops |
| Authenticated protocol | Reject tampered headers/payloads before allocation or commit; accept out-of-order chunks; deduplicate authentic replays | Host and CVM protocol tests passed; host ASan/UBSan and static analysis passed |
| Protocol fault injection | Recover from a missing or corrupted chunk; preserve final object hash after replay | All three 1 MiB end-to-end cases passed |
| 512 MiB synthetic KV object | Reassemble 387,913 chunks in private memory and match sender SHA-256 | Exact hash match; zero retransmissions, duplicates, NIC misses, or NIC errors |

Evidence: [kernel and baseline checks](kmod/ISSUES.md),
[bridge validation](docs/VALIDATION-2026-09-27-sync-mbuf-bridge.md),
[protocol and fault injection](docs/VALIDATION-2026-09-29-authenticated-chunks.md),
[512 MiB transfer](docs/VALIDATION-2026-09-29-kv-cache-transfer.md).

### Performance by data path

The rows below measure different stages. Raw receive throughput is not the
throughput of authenticated object transfer.

| Data path | Workload | Observed result | Interpretation |
|---|---|---|---|
| Shared-buffer RX, before the private bridge | 40 GiB synthetic KV stream, four queues | 3.629 s; **94.680 Gbit/s UDP payload**; zero RX misses/errors/nombuf | Packet/byte accounting only; no bytewise reassembly or authentication |
| Shared-buffer macswap, before the private bridge | Approximately 10 s of offered traffic | Stable intervals: **98.3–98.4 Gbit/s** at 1,514-byte accounting, approximately 100 Gb/s wire occupancy | 41,343 packets lost over the complete run; not a sustained zero-loss result |
| Private bridge, RX-only | 40 GiB offered load, one copy per packet | **97.742 Gbit/s estimated L2 receive rate**; 0.1530% RX miss | No AEAD or object reassembly |
| Private bridge, macswap | 40 GiB offered load, two copies per packet | **88.955 Gbit/s estimated L2 rate**; 4.9140% RX miss | All accepted RX packets transmitted; no TX errors/drops |
| Authenticated private-object transfer | 512 MiB, single queue/core, per-packet AES-GCM and ACK | 3.305 s; **1.300 Gbit/s object payload**; exact SHA-256; zero retransmissions | Correctness prototype, not a tuned transport |

See the [baseline measurements](docs/VALIDATION-2026-09-24.md),
[bridge performance report](docs/PERFORMANCE-2026-09-27-sync-mbuf-bridge.md), and
[authenticated KV result](docs/VALIDATION-2026-09-29-kv-cache-transfer.md).
Guest vCPUs were not pinned to dedicated host CPUs, and their affinity
overlapped the senders. A repeatable, long-duration zero-loss capacity result
remains open.

The 40 GiB workload models BF16 K+V for 80 layers, 131,072 tokens, eight KV
heads, and head dimension 128. The 512 MiB object models one such layer.
Both contain deterministic synthetic bytes, not tensors serialized by a
serving framework.

### GPU feasibility probes

A 512 MiB dma-buf attached to both H200 and ConnectX with identical SG layouts
for 100 attach/map/unmap/detach cycles. This checks Linux DMA attachment;
it does not demonstrate GPU execution or simultaneous device DMA.

On September 30, after GPU attestation and ReadyState setup,
`cuMemHostRegister` and `cuImportExternalMemory(DMABUF_FD)` returned
`CUDA_ERROR_NOT_SUPPORTED` in the tested H200 CC configuration. A
`cuMemcpyHtoD` copy from the shared mapping succeeded, and GPU code read the
expected 64 bytes from protected HBM. Full KV placement in HBM and framework
integration are not implemented. See the [GPU probe report](scratch/cuda-import-probe/README.md).

## Build and reproduce

Hardware-independent checks require a C compiler, Make, pkg-config, OpenSSL
development files, Python 3, and Python `cryptography`. On Debian/Ubuntu:

```sh
sudo apt-get install build-essential pkg-config libssl-dev python3 python3-cryptography
make test
make check
```

`make test` runs the standalone boundary model, C authenticated-protocol tests,
and Python channel model. These do not require root, DPDK, a NIC, or a GPU.
They do not substitute for the tests compiled against the actual mlx5 headers
or the recorded hardware runs.

For the complete data path, follow:

1. [DPDK patch series and exact baselines](patches/README.md).
2. [Build, hardware setup, and test commands](docs/REPRODUCING.md).
3. [Validation reports and experiment index](docs/README.md).

## Repository layout

```text
kmod/                 cvm_shared kernel module and UAPI
patches/              ordered DPDK 25.11.3 patch series and provenance
scratch/              runnable protocol prototypes, benchmarks, and probes
docs/                 dated validation reports, reproduction guide, figures
dpdk-patches/          historical implementation notes and archived generators
prm/                  headers used by the standalone DevX probe
*-probe.c             standalone memory-registration and DMA probes
bounds-test.c         historical boundary-logic model (not production PMD code)
```

## Current scope

- The supported receive configuration is scalar SPRQ. CVM mode disables CQE
  compression, vector RX, and MPRQ, and rejects scatter, LRO, and shared RX queues.
- Forged CQE behavior is covered by isolated logic tests, not malicious
  completion injection into a live mlx5 CQ.
- `cvmchunk` supports one object per receiver start, one queue/core, fixed
  endpoints, and unfragmented IPv4/UDP. It has no congestion control or RTT
  estimator; the test sender uses a one-second retransmission timeout.
- Production key provisioning/rotation, persistent nonce allocation, and
  replay protection across receiver restarts remain unimplemented. Published
  test keys are fixtures, not deployment credentials.
- Kernel memory accounting, NUMA allocation policy, and broader lifecycle
  stress coverage remain open; see [module issues](kmod/ISSUES.md).

## Contributing and licensing

See [CONTRIBUTING.md](CONTRIBUTING.md) for validation and patch maintenance.
Original project material is available under the [MIT License](LICENSE),
except for the components listed in [LICENSING.md](LICENSING.md). The kernel
module and DMA attachment probe retain GPL-2.0; DPDK patches and the chunk
protocol implementation retain BSD-3-Clause. This is a mixed-license source
repository, not an MIT-only distribution of DPDK or Linux kernel code.
