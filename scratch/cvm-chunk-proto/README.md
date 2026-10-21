# CVM authenticated chunk protocol prototype

This directory contains the protocol core used before wiring chunk reception
into testpmd.  It deliberately keeps transport protocol logic above the
shared/private mbuf bridge.

Wire properties:

- fixed 72-byte, explicitly encoded big-endian header;
- 1,472-byte maximum IPv4 UDP payload;
- 1,384 bytes of object data plus a 16-byte AES-256-GCM tag per full chunk;
- fixed-size chunks derive placement from `chunk_id`; no physical address is
  accepted from the sender;
- header fields are AEAD associated data;
- DATA and ACK packets use disjoint nonce spaces;
- receiver allocation and state mutation happen only after authentication;
- private bitmap supports out-of-order placement and replay deduplication;
- ACK carries the lowest missing chunk plus a 64-bit selective bitmap.

The sender must use a fresh random `object_id` for every object under one key
epoch.  A retransmission must reproduce the identical DATA packet; it must not
change plaintext or associated data while reusing the same chunk nonce.

Build and run the mechanism tests:

```sh
make test
```

The test intentionally delivers chunks out of order, replays one chunk,
corrupts ciphertext, mutates authenticated placement metadata, constructs a
selective ACK, and verifies the reconstructed private object byte-for-byte.

`cvm_chunk_send` is the matching Linux UDP sender.  It keeps at most 64 chunks
in flight, accepts only authenticated selective ACKs, and retransmits after a
one-second timeout.  Its optional final argument injects exactly one fault.
`drop:N` creates both loss and out-of-order delivery, `corrupt:N` verifies that
a bad AEAD record is rejected and later retransmitted, and `replay:N` sends one
authenticated duplicate:

```sh
./cvm_chunk_send IFACE SRC_IP DST_IP OBJECT_BYTES SRC_PORT KEY_HEX \
    [drop:N|corrupt:N|replay:N]
```

The prototype currently accepts one object per receiver process and uses one
queue/lcore.  Those restrictions are intentional until object lifecycle and
multi-worker ownership have an explicit design.

The DPDK integration is archived in
`../../patches/dpdk-25.11.3/0008-testpmd-cvm-authenticated-chunks.patch`; it applies on
top of the synchronous bridge patch 0007.  The exact CVM command, fault
injection results, hashes, and current limitations are recorded in
`../../docs/VALIDATION-2026-09-29-authenticated-chunks.md`.

The 512 MiB synthetic one-layer K+V transfer, the spurious-timeout correction,
and final zero-retransmission counters are recorded in
`../../docs/VALIDATION-2026-09-29-kv-cache-transfer.md`.
