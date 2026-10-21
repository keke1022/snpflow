/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef CVM_CHUNK_PACKET_H
#define CVM_CHUNK_PACKET_H

#include <stddef.h>
#include <stdint.h>

#include "cvm_chunk_crypto.h"

int cvm_chunk_packet_build(const uint8_t key[CVM_CHUNK_KEY_SIZE],
		const struct cvm_chunk_header *header,
		const uint8_t *plaintext, uint8_t *packet, size_t packet_capacity,
		size_t *packet_len);

int cvm_chunk_packet_open(const uint8_t key[CVM_CHUNK_KEY_SIZE],
		const uint8_t *packet, size_t packet_len,
		struct cvm_chunk_header *header, uint8_t *plaintext,
		size_t plaintext_capacity);

#endif /* CVM_CHUNK_PACKET_H */
