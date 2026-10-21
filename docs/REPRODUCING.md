# Build and reproduce

Commands below are run from the repository root unless a `cd` is shown.
Portable checks run on an ordinary Linux host. Hardware tests need the
SEV-SNP/ConnectX setup described in the [README](../README.md); they are not
part of hosted CI.

## 1. Portable tests

```sh
sudo apt-get install build-essential pkg-config libssl-dev python3 python3-cryptography
make test
make check
```

| Command / target | Coverage |
|---|---|
| `build/bounds-test` | Historical local boundary model: ring budgets, integer narrowing, cumulative counts, packet capacity |
| `make -C scratch/cvm-chunk-proto test` | Wire encoding, integer limits, AEAD tampering, allocation-after-authentication, replay, cross-object isolation, ACK ranges, reassembly |
| `python3 scratch/secure-kv-channel/secure_kv_channel.py` | Modeled completion forgery, slot bounds, snapshot mutation, replay, authenticate-before-commit |
| `make check` | Local documentation links, patch manifest, protocol source/patch parity, accidental binaries, oversized Git blobs |

The standalone boundary model predates the production validation headers.
The tests in the patched DPDK tree directly include those headers; use them
when changing PMD validation logic.

## 2. Build the module and standalone hardware probes

Copy the sources to a guest checkout if needed, using an explicit SSH
destination: `./sync.sh user@guest:/path/to/snp-dpdk/`. The script copies
sources without deleting destination files and preserves guest build outputs.

Inside the SNP guest, install the compiler, matching kernel build headers,
and RDMA development libraries. For a distribution-provided kernel:

```sh
sudo apt-get install build-essential libibverbs-dev libmlx5-dev
sudo apt-get install "linux-headers-$(uname -r)"
make -C kmod
make shared-probe verify-probe devx-probe snp-probe
sudo insmod kmod/cvm_shared.ko
sudo ./verify-probe
```

For a custom guest kernel, pass its prepared build tree with
`make -C kmod KDIR=/path/to/kernel/build`. The loaded module must match the
running kernel. `verify-probe` requires the passed-through mlx5 device and
root privileges. Expected coverage is the six checks documented in
[kmod/ISSUES.md](../kmod/ISSUES.md).

`nop-probe` also consumes mlx5 PRM definitions from a DPDK checkout:

```sh
make nop-probe DPDK_MLX5=/path/to/dpdk-stable-25.11.3/drivers/common/mlx5
sudo ./nop-probe
```

Use the module's identity/no-IOMMU DMA configuration; do not bypass its
IOMMU rejection. A non-identity domain requires a different address-mapping
implementation.

## 3. Apply and build DPDK

Prepare the source using the [patch guide](../patches/README.md). Inside the
guest, install the normal DPDK build dependencies, including Meson, Ninja,
pkg-config, NUMA and RDMA development libraries. `libssl-dev` must be visible
to Meson to enable `cvmchunk`.

```sh
sudo apt-get install meson ninja-build pkg-config libnuma-dev libibverbs-dev libmlx5-dev libssl-dev
cd /path/to/dpdk-stable-25.11.3
meson setup build -Dtests=true
ninja -C build
meson test -C build --list
```

Run the `mlx5_rx_validate` and `mlx5_tx_validate` tests listed by Meson. Their
DPDK command names are `mlx5_rx_validate_autotest` and
`mlx5_tx_validate_autotest`. Check that both complete successfully; they
exercise RX budgets/completions and exact TX completion matching without
injecting into real NIC queues.

## 4. Shared/private bridge smoke test

Choose the guest PCI BDF and dedicated guest CPU cores for the actual machine.
The recorded setup used:

```sh
sudo build/app/dpdk-testpmd -l 0-4 -n 4 \
  -a 0000:04:00.0,dv_flow_en=2 --iova-mode=va \
  --file-prefix=cvm-bridge -- \
  -i --mp-alloc=xbuf --total-num-mbufs=32768 --nb-cores=4 \
  --rxq=4 --txq=4 --rxd=2048 --txd=2048 --rss-udp \
  --forward-mode=macswap
```

At the testpmd prompt, enable promiscuous mode if required by the test traffic
(`set promisc 0 on`), then `start`. After sending traffic, run `stop`, inspect
port/forwarding statistics, and `quit` to release resources normally.
Use `rxonly` to test the RX copy without a TX copy.

With the full series installed, these modes use the private-mbuf bridge.
The September 24 raw-RX results used the earlier conservative baseline,
before bridge patch 0007; do not compare them as the same data path.

For traffic generation and exact accounting, see the
[UDP/GSO sender](../scratch/kv-cache-bench/README.md) and
[bridge validation](VALIDATION-2026-09-27-sync-mbuf-bridge.md).

## 5. Authenticated object transfer

Build the Linux host sender with:

```sh
make -C scratch/cvm-chunk-proto all
```

Use the complete receiver launch command and fixed endpoint configuration in
[the authenticated-chunks report](VALIDATION-2026-09-29-authenticated-chunks.md).
Run one port, one RX/TX queue, one forwarding core, and `--forward-mode=cvmchunk`.
Set `CVM_CHUNK_MAX_OBJECT` to at least `536870912` for the 512 MiB case; the
receiver's default is 64 MiB and its hard limit is 1 GiB.

The matching sender accepts:

```text
cvm_chunk_send IFACE SRC_IP DST_IP OBJECT_BYTES SRC_PORT KEY_HEX [drop:N|corrupt:N|replay:N]
```

Start with a 1 MiB baseline, then run `drop:0`, `corrupt:17`, and `replay:17`,
restarting the receiver for each object. The sender and receiver must use the
same test key and agreed endpoints. Check both completion logs, the source
and destination SHA-256, ACK/duplicate counts, and NIC counters. Then run the
512 MiB case described in [the KV report](VALIDATION-2026-09-29-kv-cache-transfer.md).

Ensure the host neighbor entry is valid before testpmd takes over reception.
The archived setup refreshed it through guest-kernel traffic between runs.
A successful socket write alone does not prove that any packet reached the
physical NIC; record host TX counters as well as CVM RX counters.

The published key is a deterministic test fixture. The prototype requires
fresh object IDs under each key and does not persist replay state across
receiver restarts. Production attestation and key management are outside this
test harness.

## 6. Performance and optional GPU experiments

Before capacity measurements, pin guest vCPUs and host senders to disjoint,
NUMA-appropriate host CPU sets. Record counters during active traffic and
repeat each offered rate. Follow the
[performance report's requirements](PERFORMANCE-2026-09-27-sync-mbuf-bridge.md)
before claiming a zero-loss rate.

The [CUDA probe](../scratch/cuda-import-probe/README.md) can be compiled without
CUDA toolkit headers:

```sh
make -C scratch/cuda-import-probe
```

Running it requires the ready, attested GPU and `cvm_shared` environment
documented there. Linux DMA attachment, a 64-byte protected GPU copy, and
full authenticated KV-to-HBM placement are different validation targets;
only the first two have recorded passes.
