# CQE correctness and forwarding coverage regression

Date: October 20, 2026, America/Los_Angeles (hardware execution continued on
October 21 UTC). This report covers patches 0009 and 0010. September reports
retain their original measurements and limitations.

## Changes

- RX snapshots a ready CQE into private stack memory, then checks ownership,
  opcode, format, length and LRO fields on that snapshot. All subsequent
  packet length and metadata handling consumes the same snapshot.
- TX validates a private snapshot and updates its completion position from
  the software-recorded counter. It saves that counter before releasing the
  CQ slot; completion flushing no longer rereads the shared CQE.
- CVM error/wrong-opcode completions stop the queue rather than entering the
  upstream recovery path. RX also rejects zero-length and CRC-only records,
  which otherwise let CQ consumption advance without a corresponding packet.
- testpmd forwarding lcores and their GSO contexts use the private peer pool.
  This covers `txonly`, `flowgen`, checksum output splitting, and GSO in
  addition to RX-driven forwarding.
- With the bridge active, unknown pools, shared TX sources, mixed-domain
  chains, and private descriptors referring to shared external data are
  rejected. TX allocation failure retains the caller's original batch.
- `ieee1588`, `recycle_mbufs`, and `shared_rxq` are explicitly unsupported with
  the bridge. Their device metadata/recycling/queue semantics need separate
  implementation and validation. Authentication still belongs to `cvmchunk`.

A snapshot is not an atomic or authenticated device statement. Its fields
remain untrusted and must be validated; packet payload authentication remains
the chunk protocol's responsibility.

## Source and build provenance

| Item | Value |
|---|---|
| Historical-base result, patches 0001–0010 | `bc671f0bfef8c50f2934d3002b76afe68397ccf4` |
| Official-release result, same patches | `488ff6198a811383b3d8bb71b23b09dcbe8e1d36` |
| Official archive SHA-256 | `3719acc586b310c4f60ba230683bf4f1e12c6f2f5bee11f7c01b1bbd0ded7490` |
| Host build | GCC 15.2.0, Meson 1.7.0, rdma-core 56.1, generic x86 target |
| Guest build | GCC 15.2.0, rdma-core 61.0, OpenSSL 3.5.5, native target |

Both builds compiled mlx5, testpmd, `dpdk-test`, `l2fwd`, and `l3fwd`. Building
the examples does not give them testpmd's private bridge.

The official archive was independently downloaded and matched the existing
guest archive's SHA-256. Inspection found that the historical import changed
`.gitignore` and tracked a Python bytecode file; all other files match the
fully imported release. The installer now recognizes both exact baselines
and verifies their separate resulting tree hashes. All ten patches applied
successfully to both. No historical bytecode is added to this repository.

The guest's original DPDK working directory had uncommitted bridge/protocol
files. Tests used a separate checkout and build; those existing changes were
preserved. The test kernel module was unloaded, and the initially powered-off
VM was shut down normally after testing. Its NIC returned to `mlx5_core` on
the host.

## Software regressions

The [production-code tests](../tests/dpdk/README.md) passed **48 cases** on
both host and SNP guest: 14 RX PMD cases, nine TX PMD cases, and 25 bridge or
forwarding cases. The same 48 passed host ASan/UBSan validation, including
instrumented bridge, generator, checksum and chunk-protocol code. Other DPDK
libraries retained normal build instrumentation.

The mutation fixtures include the actual PMD source and change shared fields
immediately after the validation helper returns. Against source through patch
0008, RX failed the expected packet-length assertion and TX failed the
expected WQE-counter assertion. Both passed with patch 0009. This negative
control demonstrates that the tests distinguish the broken and fixed paths.

`mlx5_rx_validate_autotest`, `mlx5_tx_validate_autotest`, and `mbuf_autotest`
also passed against the built DPDK libraries. `make test`, `make check`, and
patch whitespace checks passed. The portable suite remains the historical
19-case bounds model, 11 C protocol cases, and 15 Python model assertions.

The initial Meson test invocation could not create EAL's multiprocess socket
inside the host sandbox. Direct test runs with `--no-pci --no-shconf` passed.
LeakSanitizer's thread inspection was similarly blocked by the sandbox;
the sanitizer suite passed outside that restriction, with leak detection
enabled. These setup failures were not counted as successful runs.

## Hardware environment

AMD EPYC 9354; SEV-SNP at VMPL0; eight guest vCPUs and 16 GiB RAM. The guest
ran kernel **7.0.0-34-generic**, newer than the September measurements. The
shared-memory module was rebuilt for that kernel. All six existing
`verify-probe` groups passed, including zero SWIOTLB growth during the 16 MiB
registration check, mapping/fd lifetimes, and SG tail bounds.

The passed-through ConnectX-7 PF was `0000:04:00.0`, MAC
`60:5e:65:e3:b8:65`, with firmware **28.44.1036 (MT_0000000834)**.
The 100 Gb/s host peer was `enp33s0f0np0`, MAC `60:5e:65:e3:b8:64`.
Data addresses were host `172.31.255.1` and guest `172.31.255.2`.
The test configuration used HWS/DevX, `--iova-mode=va`, `--mp-alloc=xbuf`,
32,768 mbufs, and 1,024 RX/TX descriptors. Ordinary modes used four queues
and four forwarding cores; authentication used one queue/core.

No dedicated host CPU pinning was added. Other host workloads remained
running. These are functional regressions, not capacity measurements.

## Physical forwarding results

Ordinary receive tests sent 1,024 UDP packets across 16 source ports, with
512-byte deterministic payloads and a short pause after each 16 packets.
Counters were cleared between modes. All rows below had zero NIC RX/TX
errors; controlled receive tests also had zero RX misses.

| Mode/case | RX packets | TX packets | Application TX drops | Additional verification |
|---|---:|---:|---:|---|
| `rxonly` | 1,024 | 0 | 0 | Four-queue shared-to-private receive |
| `io` | 1,024 | 1,024 | 0 | Forwarded packet accounting |
| `mac` | 1,024 | 1,024 | 0 | Forwarded packet accounting |
| `macswap` | 1,024 | 1,024 | 0 | Forwarded packet accounting |
| `5tswap` | 1,024 | 1,024 | 0 | All 1,024 UDP echoes matched the full original payload |
| `csum` | 1,024 | 1,024 | 0 | Normal software checksum path |
| `noisy` | 1,024 | 1,024 | 0 | Default unbuffered forwarding path |
| `icmpecho` | 10 | 10 | 0 | Host ping received all ten replies |
| `txonly`, retry enabled | 0 | 5,811,776 | 0 | Host UDP listener received generated traffic |
| `flowgen`, retry enabled | 0 | 12,853,344 | 0 | Generator traversed private TX copying and NIC completions |
| Two-segment `txonly` | 0 | 512 | 0 | Host received all 512 UDP packets |
| `csum` output splitting | 1,024 | 1,024 | 0 | Four source segments copied to DMA buffers |
| `csum` GSO | 1,024 | 3,072 | 0 | 256-byte GSO limit produced three output packets per input |

An initial **unpaced `txonly` run without retry** transmitted 5,173,248 packets
and dropped 1,771,488 at the application TX path, with zero NIC TX errors.
It is retained as a backpressure observation, not reported as a zero-loss
pass. The retry-enabled generator rows do not establish a sustained zero-loss
capacity or verify every generated packet at the host.

`recycle_mbufs` and `shared_rxq` were explicitly rejected by the bridge on
hardware. `ieee1588` was absent from this build's available engine list; its
bridge rejection was checked in the software suite. Three consecutive port
stop/start cycles completed, and all testpmd processes exited normally.

### Commands for the additional paths

Use the four-queue command in [REPRODUCING.md](REPRODUCING.md), with
`--rxd=1024 --txd=1024`. Set the peer MAC:

```text
set eth-peer 0 60:5e:65:e3:b8:64
```

For finite two-segment TX, add
`--tx-ip=172.31.255.2,172.31.255.1 --tx-udp=9000,19000` at launch and listen
on host UDP port 19000:

```text
set fwd rxonly
set txpkts 32,32
set txsplit on
start tx_first 4
stop
```

For checksum splitting, send the 554-byte Ethernet frames described above:

```text
set fwd csum
set txpkts 128,128,128,170
set txsplit on
start
stop
```

For GSO, send the same input after starting:

```text
set fwd csum
set txsplit off
set port 0 gso on
set gso segsz 256
start
stop
```

## Authenticated reruns

The sender and endpoint/key fixture were unchanged from the
[authenticated-chunks procedure](VALIDATION-2026-09-29-authenticated-chunks.md).
The receiver was restarted for every object and configured with a 1 GiB
maximum. All five runs matched the sender and receiver SHA-256 exactly and
reported zero NIC errors/misses.

| Object/case | Accepted chunks | Duplicates | Rejected packets | Sender retransmissions | Elapsed sender time |
|---|---:|---:|---:|---:|---:|
| 1 MiB normal | 758 | 0 | 0 | 0 | 0.013273 s |
| 1 MiB `drop:0` | 758 | 0 | 0 | 1 | 1.014942 s |
| 1 MiB `corrupt:17` | 758 | 0 | 1 | 1 | 1.014865 s |
| 1 MiB `replay:17` | 758 | 1 | 0 | 0 | 0.013213 s |
| 512 MiB normal | 387,913 | 0 | 0 | 0 | 3.596480 s |

The 512 MiB result was **1.194214 Gbit/s object payload**. Its SHA-256 was
`683981ea39b2c91e1ceb7637fec51b4498eb3c7eb03f09be84ca9015b2a43d8e`.
The one-second fault-recovery time is consistent with the sender's configured
RTO. Timing differences from September are not attributed to a particular
code change because the kernel and host scheduling conditions differ.

The [machine-readable results](results/2026-10-20-feature-coverage.json)
retain sender summaries, object IDs, counters, environments and limitations.

## Remaining scope

- No live forged-CQE injection was performed against a physical CQ. The
  deterministic mutations exercise real PMD functions with synthetic rings.
- Vector RX, MPRQ, compression, LRO, scatter and shared RX queues remain
  restricted by the conservative driver configuration.
- The 11 engines were exercised in the representative configurations above,
  not every offload, NUMA, secondary-process, buffering or CLI combination.
- Other DPDK applications still require integration with the private bridge.
  Ordinary forwarding is not authenticated; `cvmchunk` remains single-object,
  single-queue/core and CPU-private reassembly only.
- GPU transport, persistent replay state, key provisioning and throughput
  tuning were not changed or newly qualified.
