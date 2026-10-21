# Synthetic KV-cache receive benchmark

This scratch benchmark sends a fixed number of synthetic tensor bytes from the
host to DPDK `testpmd --forward-mode=rxonly` in the CVM. It is intentionally
outside the production patch and does not modify the DPDK data path.

The default experiment models a BF16 GQA cache with:

```
2 (K and V) * 80 layers * 131072 tokens * 8 KV heads
  * 128 head dimension * 2 bytes = 40 GiB
```

Sixteen UDP flows each send 2.5 GiB. UDP GSO reduces host syscall overhead,
but the NIC emits ordinary packets with 1472-byte UDP payloads (1500-byte IPv4
packets). Compare sender payload bytes, host physical TX counters, and testpmd
RX counters. This is an unreliable one-way bulk-transfer microbenchmark; it
does not implement cache serialization, RDMA, retransmission, or receiver-side
tensor placement.

Build the host sender:

```sh
gcc -O2 -Wall -Wextra -Werror -o kv_send kv_send.c
```

The receive side used:

```sh
dpdk-testpmd -l 0-4 -n 4 -a 0000:04:00.0,dv_flow_en=2 \
  --iova-mode=va --file-prefix=kv-cache-rx-32k -- \
  -i --mp-alloc=xbuf --total-num-mbufs=32768 --nb-cores=4 \
  --rxq=4 --txq=4 --rxd=2048 --txd=2048 --rss-udp \
  --forward-mode=rxonly --stats-period=1 --auto-start
```

Each of the 16 host processes used a distinct source port and CPU:

```sh
taskset -c CPU ./kv_send enp33s0f0np0 172.31.255.1 172.31.255.2 \
  2684354560 SOURCE_PORT 1472 32
```

An optional final argument limits each sender's UDP payload rate in bits per
second using an absolute monotonic-clock schedule.  For example, sixteen
senders at `4500000000` each offer 72 Gbit/s of UDP payload in aggregate:

```sh
taskset -c CPU ./kv_send enp33s0f0np0 172.31.255.1 172.31.255.2 \
  2684354560 SOURCE_PORT 1472 32 4500000000
```

This userspace pacing is intended for finding a no-drop operating point.  It
still emits GSO batches and is not an RFC 2544 traffic generator.

`rxonly` frees received mbufs after accounting them. It verifies receipt and
exercises the CVM shared-memory RX path, but does not compare every payload byte
or copy packets into a framework-specific KV-cache layout.
