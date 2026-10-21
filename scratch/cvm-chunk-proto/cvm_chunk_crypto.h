/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef CVM_CHUNK_CRYPTO_H
#define CVM_CHUNK_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#include "cvm_chunk_wire.h"

#define CVM_CHUNK_KEY_SIZE 32U

int cvm_chunk_key_parse(uint8_t key[CVM_CHUNK_KEY_SIZE], const char *hex);

int cvm_chunk_seal(const uint8_t key[CVM_CHUNK_KEY_SIZE],
		const struct cvm_chunk_header *header,
		const uint8_t *plaintext, size_t plaintext_len,
		uint8_t *ciphertext, uint8_t tag[CVM_CHUNK_TAG_SIZE]);

int cvm_chunk_open(const uint8_t key[CVM_CHUNK_KEY_SIZE],
		const struct cvm_chunk_header *header,
		const uint8_t *ciphertext, size_t ciphertext_len,
		const uint8_t tag[CVM_CHUNK_TAG_SIZE], uint8_t *plaintext);

#endif /* CVM_CHUNK_CRYPTO_H */
