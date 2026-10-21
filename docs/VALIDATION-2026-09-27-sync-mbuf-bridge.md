# CVM synchronous private-mbuf bridge validation (2026-09-27)

## Scope

Patch under test:

`patches/dpdk-25.11.3/0007-testpmd-cvm-private-mbuf-bridge.patch`

The bridge keeps the mlx5-facing data rooms in the `cvm_shared` pinned
external-buffer pool.  At the common testpmd boundary it synchronously copies
RX packets into an ordinary private mbuf pool, releases the shared RX mbuf,
and passes only the private copy to the forwarding engine.  Before TX it
copies private packets into the paired shared pool and releases the private
source.

Both mbuf headers remain private.  No fake mbuf header is stored in shared
memory.

## Environment

- CVM: `cvm26-snp` (connected as SSH host `cvm26`)
- DPDK tree: `~/dpdk-stable-25.11.3`
- Base commit: `8a3a43b`
- NIC: `0000:04:00.0`, MAC `60:5e:65:e3:b8:65`
- Host traffic interface: `enp33s0f0np0`, `172.31.255.1/30`
- CVM data address: `172.31.255.2/30`
- Four RX/TX queues and four forwarding lcores

## Implementation checks performed during review

1. The private peer pool is created with
   `RTE_MEMPOOL_F_NO_IOVA_CONTIG`.  DPDK therefore retains
   `RTE_MEMPOOL_F_NON_IO`, and mlx5 skips MR registration for the private
   pool.  Before this correction, port startup logged a failed attempt to
   register the private pool with the CVM dma-buf path.
2. Registration rejects a DMA pool unless it is a pinned-external-buffer
   mbuf pool.
3. RX validates the complete mbuf chain before `rte_pktmbuf_copy()`, deep
   copies it, releases the shared source only after the copy, and clears
   device-derived offload metadata.
4. TX does not release private sources until all required shared copies for
   the burst exist.  Allocation failure is explicitly reported as `ENOMEM`;
   `rte_pktmbuf_copy()` itself does not set `rte_errno` on an empty pool.
5. `rte_pktmbuf_copy()` clears `RTE_MBUF_F_EXTERNAL` on the destination head.
   TX restores this ownership flag because the DMA destination pool has
   permanently pinned external data rooms.
6. A source audit found no forwarding data-path calls that bypass
   `common_fwd_stream_receive()` or `common_fwd_stream_transmit()`.  The one
   other direct RX call only drains and frees queues before forwarding.
7. The regenerated patch applies to a clean worktree with plain
   `git apply --check`; `git diff --check` is clean.  The five patched-file
   hashes in the clean worktree match the files used for the successful
   build and runtime tests.

## Build and startup smoke test

Build:

```sh
meson compile -C build app/dpdk-testpmd
```

The first smoke run exposed the private-pool MR-registration problem noted
above.  After marking the private pool non-I/O, a second run created both
pools, configured port 0, started one forwarding lcore, stopped, and closed
the port without mlx5 registration/unregistration errors.

## Four-queue RX-only data test

Receiver:

```sh
sudo build/app/dpdk-testpmd -l 0-4 -n 4 \
  -a 0000:04:00.0,dv_flow_en=2 --iova-mode=va \
  --file-prefix=cvm-sync-data -- \
  -i --mp-alloc=xbuf --total-num-mbufs=32768 --nb-cores=4 \
  --rxq=4 --txq=4 --rxd=2048 --txd=2048 --rss-udp \
  --forward-mode=rxonly
```

Four host senders each supplied 256 MiB of deterministic payload using
different UDP source ports.  Result:

| Counter | Value |
|---|---:|
| Sender packets | 729,448 |
| testpmd RX packets | 729,448 |
| testpmd RX bytes | 1,104,378,640 |
| RX missed/errors/nombuf | 0 / 0 / 0 |
| Bridge/forward drops | 0 |

This covers shared RX mbuf to private application mbuf copies and immediate
return of shared RX mbufs to their pool.

The sender timings are socket/GSO enqueue timings, not receiver throughput,
so they are intentionally not reported as link speed.

## Four-queue macswap regression after TX ownership fix

Receiver used the same queue and pool settings with
`--forward-mode=macswap`.  Four host senders each supplied 32 MiB.

| Counter | Value |
|---|---:|
| Sender packets | 91,184 |
| testpmd RX packets | 91,184 |
| testpmd TX packets | 91,184 |
| testpmd RX bytes | 138,047,456 |
| testpmd TX bytes | 138,047,456 |
| RX missed/errors/nombuf | 0 / 0 / 0 |
| TX errors/drops | 0 / 0 |

Each of the four queues received and transmitted exactly 22,796 packets.
The test covers shared RX to private copy, private macswap access, private to
shared TX copy, pinned-external ownership restoration, and normal port
shutdown.

## Burst-pressure observation

A separate 512 MiB four-flow macswap burst offered 364,724 packets.  The CVM
received and retransmitted 335,026 and reported 29,698 `RX-missed`; every
packet that reached the software RX path was transmitted, with zero TX
errors or drops.  This is not a correctness failure in the bridge, but it
shows that two synchronous deep copies reduce burst headroom.  The current
implementation must not be described as lossless at an unconstrained offered
rate.

## Excluded invalid run

One earlier macswap attempt received only three 60-byte ARP/background
packets.  The host neighbor entry for `172.31.255.2` was `FAILED`; UDP writes
had only been accepted by the host socket and were not transmitted.  Host TX
counters and the failed neighbor state confirmed this.  That run is excluded
from all bridge results.  Later runs refreshed the neighbor through normal
CVM kernel traffic and checked for a valid MAC before sending.

## Remaining limitations

- This is a correctness-first synchronous implementation using a full deep
  copy in each direction.  It is not yet a tuned bulk-copy pipeline.
- The existing conservative CVM mlx5 configuration does not enable scatter,
  MPRQ, or vector RX, so those layouts are not covered here.
- Allocation-failure and malformed-chain branches were reviewed but do not
  yet have a fault-injection test harness.
- Secondary-process attachment is implemented but not runtime-tested.
- A private snapshot closes post-copy TOCTOU exposure.  It does not prove
  that shared bytes were stable during the copy and does not authenticate
  their contents; applications needing authenticity still require a MAC or
  AEAD check over the private snapshot.
