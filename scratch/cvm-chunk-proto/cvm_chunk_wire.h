/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef CVM_CHUNK_WIRE_H
#define CVM_CHUNK_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CVM_CHUNK_MAGIC UINT32_C(0x43564d43) /* "CVMC" */
#define CVM_CHUNK_VERSION UINT16_C(1)
#define CVM_CHUNK_HEADER_SIZE 72U
#define CVM_CHUNK_TAG_SIZE 16U
#define CVM_CHUNK_NONCE_SIZE 12U
#define CVM_CHUNK_MAX_UDP_PAYLOAD 1472U
#define CVM_CHUNK_DATA_SIZE \
	(CVM_CHUNK_MAX_UDP_PAYLOAD - CVM_CHUNK_HEADER_SIZE - CVM_CHUNK_TAG_SIZE)
#define CVM_CHUNK_ACK_BITMAP_SIZE 8U
#define CVM_CHUNK_MAX_MESSAGE_SEQ UINT64_C(0x7fffffff)

#define CVM_CHUNK_F_DATA UINT32_C(0x00000001)
#define CVM_CHUNK_F_ACK  UINT32_C(0x00000002)

struct cvm_chunk_header {
	uint32_t flags;
	uint32_t key_epoch;
	uint64_t session_id;
	uint64_t object_id;
	uint64_t chunk_id;
	uint64_t total_chunks;
	uint64_t object_len;
	uint64_t message_seq;
	uint32_t payload_len;
};

void cvm_chunk_header_encode(uint8_t out[CVM_CHUNK_HEADER_SIZE],
		const struct cvm_chunk_header *header);

int cvm_chunk_header_decode(struct cvm_chunk_header *header,
		const uint8_t *wire, size_t wire_len);

int cvm_chunk_data_header_validate(const struct cvm_chunk_header *header,
		uint64_t max_object_len);

int cvm_chunk_ack_header_validate(const struct cvm_chunk_header *header,
		uint64_t expected_session_id, uint64_t expected_object_id,
		uint64_t expected_total_chunks, uint64_t expected_object_len,
		uint32_t expected_key_epoch);

int cvm_chunk_data_offset(const struct cvm_chunk_header *header,
		uint64_t *offset);

void cvm_chunk_nonce(uint8_t nonce[CVM_CHUNK_NONCE_SIZE],
		const struct cvm_chunk_header *header);

void cvm_chunk_ack_bitmap_encode(uint8_t out[CVM_CHUNK_ACK_BITMAP_SIZE],
		uint64_t bitmap);

uint64_t cvm_chunk_ack_bitmap_decode(
		const uint8_t in[CVM_CHUNK_ACK_BITMAP_SIZE]);

#endif /* CVM_CHUNK_WIRE_H */
