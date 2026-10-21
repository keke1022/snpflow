# Authenticated chunk transfer validation (2026-09-29)

## Scope

This record covers the correctness-first authenticated object-transfer layer
on top of the synchronous private-mbuf bridge.  The DPDK change is archived as
`patches/dpdk-25.11.3/0008-testpmd-cvm-authenticated-chunks.patch` and applies after
`0007-testpmd-cvm-private-mbuf-bridge.patch`.

The change does not modify mlx5, the CVM kernel module, the shared allocator,
or the mbuf ABI.  It adds an OpenSSL-backed `cvmchunk` testpmd forwarding mode
only when Meson finds `libcrypto`.

## Trust-boundary flow

1. The host splits one logical object into fixed 1,384-byte chunks.
2. Each DATA record carries a fixed 72-byte big-endian header, ciphertext, and
   a 16-byte AES-256-GCM tag.  The entire header is associated data.
3. The NIC writes the Ethernet frame into an untrusted shared DMA mbuf.
4. The existing synchronous bridge snapshots the complete frame into a private
   non-I/O mbuf and returns the shared RX mbuf.
5. `cvmchunk` checks a statically configured peer MAC, peer/local IPv4 pair,
   UDP ports, lengths, and fragmentation state on that private snapshot.
6. It authenticates and decrypts into a bounded private stack buffer.  Only
   after successful authentication does it allocate or update the private
   object and private received bitmap.
7. Placement is derived as `chunk_id * 1384`; no address or offset supplied by
   the peer is used directly.
8. An authenticated ACK reports the lowest missing chunk and the following
   64-bit selective bitmap.  The ACK is built in a private mbuf, then copied by
   the bridge into a shared TX mbuf for NIC DMA.

DATA and ACK records use disjoint 31-bit sequence spaces within a 64-bit
per-object nonce prefix.  The packet and crypto APIs reject unknown flags and
sequence values outside that range before constructing a nonce.

## Deliberately conservative restrictions

- exactly one port, RX queue, TX queue, forwarding stream, and forwarding
  lcore;
- one object for each receiver start;
- IPv4/UDP only, no VLAN, IP options, or fragmented input;
- one ACK per authenticated DATA record;
- 64 MiB default and 1 GiB hard receiver object limits;
- fixed peer MAC, peer/local IPv4 addresses, and peer UDP port supplied through
  the environment;
- synchronous CPU AES-GCM and synchronous private/shared copies.

These constraints avoid claiming unimplemented multi-worker ownership or
object-lifecycle semantics.  This is a generalized byte-object protocol, not a
KV-cache-specific memory layout.

## Protocol tests

Both the host and `cvm26-snp` compiled the core with:

```sh
cc -O2 -g -std=c11 -Wall -Wextra -Werror -Wformat=2 -Wshadow \
  cvm_chunk_test.c cvm_chunk_wire.c cvm_chunk_crypto.c \
  cvm_chunk_packet.c cvm_chunk_receiver.c -lcrypto
```

The CVM used OpenSSL 3.5.5.  Both executions reported:

```text
wire_roundtrip=PASS
integer_boundaries=PASS
aead_header_and_payload_tamper=PASS
out_of_order_placement=PASS
authenticated_replay_dedup=PASS
failed_auth_does_not_commit=PASS
failed_first_auth_does_not_allocate=PASS
corrupt_replay_is_not_deduplicated=PASS
authenticated_cross_object_isolation=PASS
authenticated_ack_range=PASS
object_reassembly=PASS bytes=4169 chunks=4
```

The host additionally passed GCC `-fanalyzer` and ASan/UBSan builds.  Leak
Sanitizer cannot run under the managed ptrace environment, so only leak
detection was disabled; address and undefined-behavior instrumentation ran.

An incremental CVM DPDK build linked `build/app/dpdk-testpmd` successfully.
The final patch was also apply-checked in a clean temporary worktree on top of
bridge patch 0007.

## End-to-end setup

- CVM: `cvm26-snp`
- NIC: `0000:04:00.0`, MAC `60:5e:65:e3:b8:65`
- Host interface: `enp33s0f0np0`, MAC `60:5e:65:e3:b8:64`,
  `172.31.255.1/30`
- CVM data address: `172.31.255.2/30`
- sender UDP port: 9101; receiver UDP port: 9000
- object: 1,048,576 deterministic synthetic bytes, 758 chunks
- expected SHA-256:
  `7954efb1472b152e716f5375e33c3aacb16216e147ac5d5b78342b634ded3bff`

The CVM receiver was started with one forwarding core and queue:

```sh
sudo env \
  CVM_CHUNK_KEY_HEX=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f \
  CVM_CHUNK_PEER_IPV4=172.31.255.1 \
  CVM_CHUNK_LOCAL_IPV4=172.31.255.2 \
  CVM_CHUNK_PEER_MAC=60:5e:65:e3:b8:64 \
  CVM_CHUNK_PEER_UDP_PORT=9101 \
  CVM_CHUNK_MAX_OBJECT=16777216 \
  build/app/dpdk-testpmd -l 0-1 -n 4 \
  -a 0000:04:00.0,dv_flow_en=2 --iova-mode=va \
  --file-prefix=cvm-chunk-e2e -- \
  -i --mp-alloc=xbuf --total-num-mbufs=8192 --nb-cores=1 \
  --rxq=1 --txq=1 --rxd=1024 --txd=1024 \
  --forward-mode=cvmchunk --auto-start
```

The normal host neighbor entry must be valid before testpmd takes the data
path.  A ping from the CVM kernel refreshed it between receiver restarts.  One
excluded run timed out with the host neighbor in `FAILED`; the CVM received no
protocol packets, so that was a setup failure rather than a protocol result.

## End-to-end results

### Loss plus out-of-order delivery

`drop:0` skipped chunk 0 on its first transmission, allowing the rest of the
initial window to arrive first.

```text
cvm_chunk_send=PASS object_bytes=1048576 chunks=758 transmissions=758
retransmissions=1 wire_bytes=1115280 seconds=0.027491146
payload_gbps=0.305139 object_id=4ea70b639a314de1
```

The CVM completed the object with the expected SHA-256.  Receiver state was
`accepted=758, duplicates=0, acks=758`.

### Ciphertext corruption plus recovery

`corrupt:17` flipped one ciphertext bit on the first version of chunk 17.  The
CVM did not ACK or commit it; the sender then retransmitted the authentic
record.

```text
cvm_chunk_send=PASS object_bytes=1048576 chunks=758 transmissions=759
retransmissions=1 wire_bytes=1116752 seconds=0.026311128
payload_gbps=0.318824 object_id=b5180f11a23db145
```

The reconstructed object again had the expected SHA-256.  The receiver's
rejected count included the corrupted record plus unrelated 60-byte background
frames on the physical port.

### Authenticated replay

`replay:17` sent the exact valid encrypted record twice.

```text
cvm_chunk_send=PASS object_bytes=1048576 chunks=758 transmissions=759
retransmissions=0 wire_bytes=1116752 seconds=0.010816143
payload_gbps=0.775564 object_id=8a018d77f97103e2
```

Receiver state was exactly `accepted=758, duplicates=1, acks=759`, and the
final SHA-256 remained unchanged.

After adding the final fail-closed sequence/flag checks, the exact rebuilt
binary also completed a 65,536-byte `drop:0` smoke test: 48 chunks, one
retransmission, `accepted=48`, and SHA-256
`334c9aad74eb835c8b8fdd595438c6187aec1a5f6b36e384ecf35f487bb5e3d6`.
An independent generation of the deterministic source bytes produced the same
digest.

The timing values demonstrate functional completion only.  They are not a
throughput characterization: the implementation creates an EVP context and
ACK for every packet, uses one lcore, and the host sender is a kernel UDP test
program.

## Security and lifecycle limitations

- AES-GCM requires a globally fresh `object_id` for every object under one key
  epoch, including across process and VM restarts.  The test sender uses a
  random 64-bit value; production needs persistent allocation or a key/nonce
  lifecycle with a stronger uniqueness argument.
- The test key is passed in an environment variable.  Production attestation,
  key provisioning, rotation, and key erasure are not implemented here.
- Receiver restart replay protection is not persistent.  The in-memory bitmap
  prevents replay only during the current object lifetime.
- The bridge snapshot plus AEAD prevents unauthenticated/torn shared data from
  being committed to the private object.  It does not make the shared DMA
  staging area secret.
- There is no congestion-control algorithm, RTT estimator, receiver-advertised
  memory window, multi-object scheduling, or multi-queue flow ownership yet.
- The fixed endpoint check prevents outer-header rewriting from redirecting
  ACKs in this setup, but endpoint provisioning itself is outside this patch.
