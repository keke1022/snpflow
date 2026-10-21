# Synchronous private-mbuf bridge performance (2026-09-27)

## Configuration

- CVM: `cvm26-snp`
- DPDK: `~/dpdk-stable-25.11.3`, base commit `8a3a43b` plus
  `patches/dpdk-25.11.3/0007-testpmd-cvm-private-mbuf-bridge.patch`
- NIC: 100 Gbit/s mlx5 device `0000:04:00.0`
- Packet: IPv4/UDP, 1,472-byte UDP payload, 1,514 bytes counted by the NIC
  for a full frame
- Four RX queues, four TX queues, four forwarding lcores, RSS over UDP
- 2,048 RX and TX descriptors per queue
- 32,768 shared DMA mbufs and 32,768 private application mbufs
- RX remains on the conservative scalar SPRQ configuration

`rxonly` performs one shared-to-private deep copy per packet.  `macswap`
performs that RX copy, modifies the private Ethernet header, then performs a
private-to-shared deep copy before TX.

## Unpaced 40 GiB offered-load runs

Sixteen host senders each sent 2.5 GiB using a distinct UDP source port.  The
host sender uses UDP GSO with 32 segments per send call.  The reported window
is the slowest sender's elapsed time; it is a useful end-to-end estimate, but
not a hardware timestamp measurement.

| Mode | Sender window | RX packets | RX missed | Miss rate | RX/TX bytes | Estimated L2 rate | Estimated received UDP payload rate |
|---|---:|---:|---:|---:|---:|---:|---:|
| rxonly | 3.610124 s | 29,133,133 | 44,644 | 0.1530% | 44,107,548,596 RX | 97.742 Gbit/s | 95.030 Gbit/s |
| macswap | 3.777578 s | 27,743,981 | 1,433,797 | 4.9140% | 42,004,371,014 RX and TX | 88.955 Gbit/s | 86.487 Gbit/s |

For macswap, every packet accepted by RX was transmitted: TX packets equal RX
packets, with zero TX errors and zero TX drops.  Loss occurred at the NIC RX
queue before the software bridge, not in the private-to-shared TX copy.

These numbers support two practical conclusions for this exact run:

1. One synchronous copy sustained approximately 97.7 Gbit/s of received L2
   traffic, with 0.153% RX miss under an approximately line-rate offer.
2. Two synchronous copies sustained approximately 89.0 Gbit/s averaged over
   the complete sender window, but a 100 Gbit/s offer was not lossless.

## Rate-controlled macswap runs

The scratch sender was extended with an optional per-process payload-rate
argument.  It uses an absolute `CLOCK_MONOTONIC` schedule after each GSO batch.
Sixteen senders each sent 2 GiB, for 32 GiB total.

| Configured aggregate UDP payload | Sender window | RX packets | RX missed | Miss rate | Observed RX L2 rate |
|---:|---:|---:|---:|---:|---:|
| 72.0 Gbit/s | 3.817963 s | 23,326,165 | 16,061 | 0.0688% | 73.999 Gbit/s |
| 70.4 Gbit/s | 3.904993 s | 23,307,555 | 34,671 | 0.1485% | 72.292 Gbit/s |
| 60.0 Gbit/s | 4.581407 s | 21,589,459 | 1,752,765 | 7.5090% | 57.077 Gbit/s |

The non-monotonic loss proves that these runs cannot be used to locate a
bridge throughput threshold.  In each run almost all loss concentrated in one
RX queue, but the affected queue changed between runs.

## Scheduling limitation discovered

The eight KVM vCPU threads for `cvm26-snp` all had host affinity `0-127`; none
was pinned.  Host sender processes were pinned to CPUs 32-47.  A read after the
tests showed one guest vCPU on host CPU 40, demonstrating that sender and guest
worker affinities overlap.  Guest vCPUs can migrate and directly contend with
the traffic generators.  This explains the changing slow queue and why a
lower offered rate could lose more packets than a higher rate.

Consequently, no strict long-duration zero-loss rate is claimed from this
environment.  The unpaced values above are observed performance, not an RFC
2544 result or a guaranteed capacity.

## Requirements for a repeatable capacity result

1. Pin guest vCPUs 0-4 to dedicated, isolated physical CPUs.
2. Pin host traffic generators to a disjoint, NUMA-appropriate CPU set.
3. Ensure other QEMU guests and host tasks cannot run on the guest worker
   CPUs.
4. Record counters at fixed intervals while traffic is active, rather than
   deriving the interval solely from userspace sender completion.
5. Sweep offered rate and require repeated zero `RX-missed`, `RX-nombuf`, RX
   errors, TX errors, and TX drops before declaring a no-loss rate.
6. Repeat with larger RX descriptor rings to separate brief scheduling stalls
   from steady-state copy capacity.

Changing QEMU affinity requires host administrative control and was not done
during this test.  All testpmd instances were stopped and closed normally at
the end.
