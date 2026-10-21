# SNPFlow

**Shared-memory DMA and authenticated object transfer for DPDK in AMD SEV-SNP VMs.**

[![Portable checks](https://github.com/keke1022/snpflow/actions/workflows/ci.yml/badge.svg)](https://github.com/keke1022/snpflow/actions/workflows/ci.yml)
[![DPDK integration](https://github.com/keke1022/snpflow/actions/workflows/dpdk.yml/badge.svg)](https://github.com/keke1022/snpflow/actions/workflows/dpdk.yml)

SNPFlow extends **DPDK 25.11.3's mlx5 driver and testpmd** so a passed-through
NIC can use explicitly shared guest memory for DMA while packet processing
and authenticated object reassembly stay in private guest memory.

This is a research prototype, validated with an **NVIDIA ConnectX-7 on a
100 Gb/s link inside an AMD SEV-SNP guest**.

## What we add to DPDK

- **Shared DMA allocation:** a Linux `cvm_shared` module exports shared pages
  as dma-bufs; mlx5 patches register packet buffers and device resources.
- **Private packet processing:** validated private CQE snapshots and a testpmd
  bridge keep application buffers and mbuf metadata in private memory.
- **Authenticated object transfer:** `cvmchunk` adds AES-256-GCM chunks,
  authenticated ACKs, retransmission and private object reassembly.

Shared DMA memory remains untrusted. The bridge copies packets into private
memory; `cvmchunk` authenticates data before committing it to an object.

## Get started

Run the portable checks on Debian/Ubuntu with Git 2.43 or newer:

```sh
git clone https://github.com/keke1022/snpflow.git
cd snpflow
sudo apt-get install build-essential pkg-config libssl-dev python3 python3-cryptography
make test
make check
```

These checks need no DPDK installation, NIC or SNP guest. To build the full
path, follow the [DPDK patch guide](patches/README.md) and
[hardware setup and reproduction guide](docs/REPRODUCING.md).

## Validation

Hardware tests used AMD EPYC 9354, a SEV-SNP guest at VMPL0, and a
passed-through ConnectX-7 PF. Exact kernels, firmware and runtime settings
are in the [tested configuration](docs/REPRODUCING.md#recorded-hardware-configuration).

| Test | Recorded result |
|---|---|
| Shared memory and NIC DMA | Six module checks and a byte-level macswap round trip passed |
| [Production-code regressions](tests/dpdk/README.md) | 48 checks covering CQE mutation, buffer ownership and forwarding passed |
| [Selected upstream tests](docs/VALIDATION-2026-10-21-test-setup.md) | 22 DPDK commands pass in CI; the full upstream suite still has failures and skips |
| [Raw receive, before the private bridge](docs/VALIDATION-2026-09-24.md) | 40 GiB synthetic stream: **94.680 Gbit/s UDP payload**, no authentication |
| [Authenticated private-object transfer](docs/VALIDATION-2026-10-20-feature-coverage.md) | 512 MiB synthetic object: **3.596 s / 1.194 Gbit/s**, exact SHA-256, zero retransmissions; one queue/core |

These measurements cover different data paths and are not a controlled
performance comparison. See the [validation index](docs/README.md) for
workloads, loss counters, negative controls and unresolved issues.

## Scope

The CVM path currently supports scalar SPRQ and testpmd's private-buffer
bridge. Production key management and replay protection across restarts are
unfinished. GPU experiments are separate; authenticated delivery into GPU
memory is not implemented. See [configuration limits](docs/REPRODUCING.md#configuration-limits).

## Help and contributions

Open an [issue](https://github.com/keke1022/snpflow/issues) for questions or
bugs. See [CONTRIBUTING.md](CONTRIBUTING.md) for development and local Git hooks,
and [SECURITY.md](SECURITY.md) for private vulnerability reports.

## License

Original project material uses [MIT](LICENSE). DPDK patches and protocol code
retain BSD-3-Clause; kernel components retain GPL-2.0.
See [LICENSING.md](LICENSING.md) for component boundaries.
