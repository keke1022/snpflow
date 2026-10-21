/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef CVM_CHUNK_RECEIVER_H
#define CVM_CHUNK_RECEIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cvm_chunk_packet.h"

enum cvm_chunk_accept_result {
	CVM_CHUNK_ACCEPT_NEW = 0,
	CVM_CHUNK_ACCEPT_DUPLICATE,
	CVM_CHUNK_ACCEPT_COMPLETE,
};

struct cvm_chunk_receiver {
	uint8_t key[CVM_CHUNK_KEY_SIZE];
	uint64_t max_object_len;
	uint64_t session_id;
	uint64_t object_id;
	uint64_t object_len;
	uint64_t total_chunks;
	uint64_t received_chunks;
	uint64_t ack_base;
	uint64_t ack_message_seq;
	uint32_t key_epoch;
	uint8_t *object;
	uint64_t *bitmap;
	size_t bitmap_words;
	bool initialized;
};

int cvm_chunk_receiver_init(struct cvm_chunk_receiver *receiver,
		const uint8_t key[CVM_CHUNK_KEY_SIZE], uint64_t max_object_len);

void cvm_chunk_receiver_destroy(struct cvm_chunk_receiver *receiver);

int cvm_chunk_receiver_accept(struct cvm_chunk_receiver *receiver,
		const uint8_t *packet, size_t packet_len,
		enum cvm_chunk_accept_result *result);

int cvm_chunk_receiver_build_ack(struct cvm_chunk_receiver *receiver,
		uint8_t *packet, size_t packet_capacity, size_t *packet_len);

bool cvm_chunk_receiver_has(const struct cvm_chunk_receiver *receiver,
		uint64_t chunk_id);

#endif /* CVM_CHUNK_RECEIVER_H */
