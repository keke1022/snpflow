#!/usr/bin/env python3
"""Test AEAD tamper and replay behavior on an actual cvm_shared mapping."""

import fcntl
import mmap
import os
import struct

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESGCM


SIZE = 2 << 20
PAYLOAD_SIZE = 1 << 20


def iowr(ioctl_type: str, number: int, size: int) -> int:
    return (3 << 30) | (ord(ioctl_type) << 8) | number | (size << 16)


CVM_SHARED_IOC_ALLOC = iowr("C", 0x01, 16)


def nonce(sequence: int) -> bytes:
    # A distinct nonce is mandatory for every message under one AES-GCM key.
    return b"\0" * 4 + sequence.to_bytes(8, "big")


def aad(cache_id: int, sequence: int, payload_size: int) -> bytes:
    # Bind routing/placement metadata to the ciphertext.
    return struct.pack(">QQII", cache_id, sequence, payload_size, 0)


class OrderedReceiver:
    """Minimal private replay state for one strictly ordered stream."""

    def __init__(self, cipher: AESGCM, cache_id: int):
        self.cipher = cipher
        self.cache_id = cache_id
        self.expected_sequence = 0

    def receive(self, sequence: int, sealed: bytes) -> bytes:
        if sequence != self.expected_sequence:
            raise ValueError(
                f"sequence {sequence} rejected; expected {self.expected_sequence}"
            )
        plaintext = self.cipher.decrypt(
            nonce(sequence),
            sealed,
            aad(self.cache_id, sequence, len(sealed) - 16),
        )
        # Advance only after authentication succeeds.
        self.expected_sequence += 1
        return plaintext


def main() -> None:
    control_fd = os.open("/dev/cvm_shared", os.O_RDWR | os.O_CLOEXEC)
    request = bytearray(struct.pack("=QIi", SIZE, 0, -1))
    fcntl.ioctl(control_fd, CVM_SHARED_IOC_ALLOC, request, True)
    _, _, dmabuf_fd = struct.unpack("=QIi", request)
    if dmabuf_fd < 0:
        raise RuntimeError("cvm_shared returned no dma-buf fd")

    shared = mmap.mmap(
        dmabuf_fd,
        SIZE,
        flags=mmap.MAP_SHARED,
        prot=mmap.PROT_READ | mmap.PROT_WRITE,
    )
    key = bytes(range(32))
    cipher = AESGCM(key)
    cache_id = 0x4B56434143484501
    plaintext0 = bytes((i * 131 + 0x5A) & 0xFF for i in range(PAYLOAD_SIZE))
    sealed0 = cipher.encrypt(nonce(0), plaintext0, aad(cache_id, 0, PAYLOAD_SIZE))
    shared[: len(sealed0)] = sealed0

    recovered = cipher.decrypt(
        nonce(0), bytes(shared[: len(sealed0)]), aad(cache_id, 0, PAYLOAD_SIZE)
    )
    print(f"authentic_chunk={'PASS' if recovered == plaintext0 else 'FAIL'}")

    original = shared[12345]
    shared[12345] = original ^ 1
    try:
        cipher.decrypt(
            nonce(0), bytes(shared[: len(sealed0)]), aad(cache_id, 0, PAYLOAD_SIZE)
        )
        print("tamper_detection=FAIL")
    except InvalidTag:
        print("tamper_detection=PASS")
    shared[12345] = original

    # Cryptographic authentication alone accepts an unchanged old message.
    replay_blob = bytes(shared[: len(sealed0)])
    replay_once = cipher.decrypt(nonce(0), replay_blob, aad(cache_id, 0, PAYLOAD_SIZE))
    replay_twice = cipher.decrypt(nonce(0), replay_blob, aad(cache_id, 0, PAYLOAD_SIZE))
    print(
        "aead_alone_replay_accepted="
        + ("YES" if replay_once == replay_twice == plaintext0 else "NO")
    )

    receiver = OrderedReceiver(cipher, cache_id)
    receiver.receive(0, replay_blob)
    try:
        receiver.receive(0, replay_blob)
        print("private_sequence_replay_rejection=FAIL")
    except ValueError:
        print("private_sequence_replay_rejection=PASS")

    plaintext1 = b"next" * (PAYLOAD_SIZE // 4)
    sealed1 = cipher.encrypt(nonce(1), plaintext1, aad(cache_id, 1, PAYLOAD_SIZE))
    corrupted1 = bytearray(sealed1)
    corrupted1[0] ^= 1
    try:
        receiver.receive(1, bytes(corrupted1))
        print("failed_auth_does_not_advance=FAIL")
    except InvalidTag:
        receiver.receive(1, sealed1)
        print(
            "failed_auth_does_not_advance="
            + ("PASS" if receiver.expected_sequence == 2 else "FAIL")
        )

    shared.close()
    os.close(dmabuf_fd)
    os.close(control_fd)


if __name__ == "__main__":
    main()
