#!/usr/bin/env python3
"""Executable model of a hostile-DMA-safe fixed-cell KV receive channel.

This is deliberately independent of DPDK and CUDA.  It tests the security
invariants that a later DPDK/CUDA implementation must preserve:

* a public completion is only a notification, never an addressing authority;
* slot, copy size, sequence, epoch and destination come from private state;
* the public cell is snapshotted before parsing variable metadata;
* final KV memory is modified only after AEAD authentication succeeds.
"""

from __future__ import annotations

from dataclasses import dataclass
import struct
from typing import Callable, Optional

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESGCM


CELL_MAGIC = b"CKV1"
CELL_VERSION = 1
PAYLOAD_SIZE = 4096
TAG_SIZE = 16
SLOT_COUNT = 4

# magic, version, stream_id, sequence, payload_length, key_epoch,
# destination tensor byte offset
HEADER = struct.Struct(">4sIQQIIQ")
SLOT_SIZE = HEADER.size + PAYLOAD_SIZE + TAG_SIZE


def nonce(key_epoch: int, sequence: int) -> bytes:
    """96-bit GCM nonce; the key must change before either field wraps."""
    return struct.pack(">IQ", key_epoch, sequence)


def make_header(
    stream_id: int,
    sequence: int,
    payload_length: int,
    key_epoch: int,
    tensor_offset: int,
) -> bytes:
    return HEADER.pack(
        CELL_MAGIC,
        CELL_VERSION,
        stream_id,
        sequence,
        payload_length,
        key_epoch,
        tensor_offset,
    )


def seal_cell(
    cipher: AESGCM,
    stream_id: int,
    sequence: int,
    key_epoch: int,
    tensor_offset: int,
    payload: bytes,
) -> bytes:
    """Create one constant-size public cell.

    The true payload length is authenticated metadata.  Padding is encrypted,
    so the public ring does not need a CQE-derived copy length.
    """
    if not 0 < len(payload) <= PAYLOAD_SIZE:
        raise ValueError("payload length outside one fixed cell")
    header = make_header(
        stream_id, sequence, len(payload), key_epoch, tensor_offset
    )
    padded = payload + bytes(PAYLOAD_SIZE - len(payload))
    sealed = cipher.encrypt(nonce(key_epoch, sequence), padded, header)
    cell = header + sealed
    if len(cell) != SLOT_SIZE:
        raise AssertionError("cell encoder violated the fixed-size contract")
    return cell


@dataclass(frozen=True)
class UntrustedCompletion:
    """Fields supplied by public DMA memory and therefore attacker-controlled."""

    slot: int
    byte_count: int
    generation: int


class PublicRing:
    def __init__(self) -> None:
        self.slots = [bytearray(SLOT_SIZE) for _ in range(SLOT_COUNT)]

    def publish(self, slot: int, cell: bytes) -> None:
        if len(cell) != SLOT_SIZE:
            raise ValueError("public cell must have the fixed wire size")
        self.slots[slot][:] = cell


class PrivateReceiver:
    """Private queue state and transactional KV destination."""

    def __init__(
        self,
        cipher: AESGCM,
        stream_id: int,
        key_epoch: int,
        destination_size: int,
    ) -> None:
        self.cipher = cipher
        self.stream_id = stream_id
        self.key_epoch = key_epoch
        self.expected_sequence = 0
        self.destination = bytearray(destination_size)
        self.last_snapshot_size = 0
        self.hint_mismatches = 0

    def receive(
        self,
        ring: PublicRing,
        hint: UntrustedCompletion,
        after_snapshot: Optional[Callable[[], None]] = None,
    ) -> bool:
        # The public CQE cannot select an address.  SPRQ posting order and the
        # private consumer sequence select exactly one fixed slot.
        expected_slot = self.expected_sequence % SLOT_COUNT
        if hint.slot != expected_slot:
            self.hint_mismatches += 1

        # Neither CQE byte_count nor generation participates in this copy.
        # If the cell is incomplete, mixed by a race, stale, or malicious, the
        # authentication below fails without changing private queue state.
        snapshot = bytes(ring.slots[expected_slot][:SLOT_SIZE])
        self.last_snapshot_size = len(snapshot)
        if after_snapshot is not None:
            after_snapshot()

        header = snapshot[: HEADER.size]
        try:
            (
                magic,
                version,
                stream_id,
                sequence,
                payload_length,
                key_epoch,
                tensor_offset,
            ) = HEADER.unpack(header)
        except struct.error:
            return False

        # Check all routing values against private expectations before they can
        # affect nonce choice, destination addressing, or slice bounds.
        expected_offset = self.expected_sequence * PAYLOAD_SIZE
        if (
            magic != CELL_MAGIC
            or version != CELL_VERSION
            or stream_id != self.stream_id
            or sequence != self.expected_sequence
            or key_epoch != self.key_epoch
            or tensor_offset != expected_offset
            or not 0 < payload_length <= PAYLOAD_SIZE
            or tensor_offset + payload_length > len(self.destination)
        ):
            return False

        try:
            padded = self.cipher.decrypt(
                nonce(self.key_epoch, self.expected_sequence),
                snapshot[HEADER.size :],
                header,
            )
        except InvalidTag:
            return False

        # Commit is deliberately last.  No unauthenticated plaintext is ever
        # exposed in the final tensor/KV layout.
        self.destination[tensor_offset : tensor_offset + payload_length] = (
            padded[:payload_length]
        )
        self.expected_sequence += 1
        return True


def expect(condition: bool, description: str) -> None:
    if not condition:
        raise AssertionError(description)
    print(f"PASS {description}")


def main() -> None:
    key = bytes(range(32))
    cipher = AESGCM(key)
    stream_id = 0x4B565F5354524541
    epoch = 7
    ring = PublicRing()
    receiver = PrivateReceiver(
        cipher, stream_id, epoch, destination_size=8 * PAYLOAD_SIZE
    )

    payload0 = b"A" * PAYLOAD_SIZE
    ring.publish(0, seal_cell(cipher, stream_id, 0, epoch, 0, payload0))
    expect(
        receiver.receive(
            ring,
            UntrustedCompletion(slot=0, byte_count=(1 << 32) - 1, generation=999),
        ),
        "CQE length/generation cannot change the fixed copy bound",
    )
    expect(receiver.last_snapshot_size == SLOT_SIZE, "snapshot size is constant")
    expect(receiver.destination[:PAYLOAD_SIZE] == payload0, "valid cell commits")

    # A completion pointing at an attacker-chosen slot cannot redirect a read.
    payload1 = b"B" * PAYLOAD_SIZE
    ring.publish(3, seal_cell(cipher, stream_id, 1, epoch, PAYLOAD_SIZE, payload1))
    before = bytes(receiver.destination)
    expect(
        not receiver.receive(
            ring, UntrustedCompletion(slot=3, byte_count=SLOT_SIZE, generation=1)
        ),
        "forged slot cannot redirect the private consumer",
    )
    expect(bytes(receiver.destination) == before, "failed receive has no KV side effect")
    expect(receiver.expected_sequence == 1, "failed receive does not advance sequence")

    ring.publish(1, seal_cell(cipher, stream_id, 1, epoch, PAYLOAD_SIZE, payload1))
    expect(
        receiver.receive(
            ring, UntrustedCompletion(slot=1, byte_count=1, generation=0)
        ),
        "correct private slot succeeds despite a forged CQE byte count",
    )

    # Mutation before the snapshot is authenticated and must not commit.
    payload2 = b"C" * PAYLOAD_SIZE
    cell2 = bytearray(
        seal_cell(cipher, stream_id, 2, epoch, 2 * PAYLOAD_SIZE, payload2)
    )
    cell2[HEADER.size + 117] ^= 1
    ring.publish(2, bytes(cell2))
    before = bytes(receiver.destination)
    expect(
        not receiver.receive(
            ring, UntrustedCompletion(slot=2, byte_count=SLOT_SIZE, generation=2)
        ),
        "ciphertext mutation before snapshot fails authentication",
    )
    expect(bytes(receiver.destination) == before, "bad tag cannot poison final KV")

    ring.publish(
        2, seal_cell(cipher, stream_id, 2, epoch, 2 * PAYLOAD_SIZE, payload2)
    )
    expect(
        receiver.receive(
            ring, UntrustedCompletion(slot=2, byte_count=SLOT_SIZE, generation=2)
        ),
        "authentic retry succeeds after a failed tag",
    )

    # A mutation after the snapshot cannot change what gets authenticated or
    # committed.  A mutation during a real copy produces a mixed snapshot and
    # is covered by the same AEAD check as the previous test.
    payload3 = b"D" * 1234
    ring.publish(
        3, seal_cell(cipher, stream_id, 3, epoch, 3 * PAYLOAD_SIZE, payload3)
    )

    def mutate_public_after_snapshot() -> None:
        ring.slots[3][HEADER.size + 9] ^= 1

    expect(
        receiver.receive(
            ring,
            UntrustedCompletion(slot=3, byte_count=SLOT_SIZE, generation=3),
            after_snapshot=mutate_public_after_snapshot,
        ),
        "post-snapshot public mutation cannot alter the committed cell",
    )
    expect(
        receiver.destination[3 * PAYLOAD_SIZE : 3 * PAYLOAD_SIZE + len(payload3)]
        == payload3,
        "transactional commit uses the authenticated snapshot",
    )

    # Sequence 4 maps back to slot 0.  Replaying sequence 0 there must fail.
    replay0 = seal_cell(cipher, stream_id, 0, epoch, 0, payload0)
    ring.publish(0, replay0)
    before = bytes(receiver.destination)
    expect(
        not receiver.receive(
            ring, UntrustedCompletion(slot=0, byte_count=SLOT_SIZE, generation=4)
        ),
        "old authentic cell is rejected as a replay",
    )
    expect(bytes(receiver.destination) == before, "replay has no KV side effect")
    expect(receiver.expected_sequence == 4, "replay does not advance private state")

    print(
        f"SUMMARY all invariants passed; slot_size={SLOT_SIZE} "
        f"hint_mismatches={receiver.hint_mismatches}"
    )


if __name__ == "__main__":
    main()
