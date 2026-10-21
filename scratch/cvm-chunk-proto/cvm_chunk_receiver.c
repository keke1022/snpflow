/* SPDX-License-Identifier: BSD-3-Clause */

#include "cvm_chunk_receiver.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/crypto.h>

static void
bitmap_set(struct cvm_chunk_receiver *receiver, uint64_t chunk_id)
{
	receiver->bitmap[chunk_id / 64] |= UINT64_C(1) << (chunk_id % 64);
}

bool
cvm_chunk_receiver_has(const struct cvm_chunk_receiver *receiver,
		uint64_t chunk_id)
{
	if (receiver == NULL || !receiver->initialized ||
	    chunk_id >= receiver->total_chunks)
		return false;
	return (receiver->bitmap[chunk_id / 64] &
		(UINT64_C(1) << (chunk_id % 64))) != 0;
}

static int
receiver_start_object(struct cvm_chunk_receiver *receiver,
		const struct cvm_chunk_header *header)
{
	size_t bitmap_words;

	if (header->object_len > SIZE_MAX ||
	    header->total_chunks > (uint64_t)SIZE_MAX - 63)
		return -EOVERFLOW;
	bitmap_words = (size_t)((header->total_chunks + 63) / 64);
	if (bitmap_words > SIZE_MAX / sizeof(*receiver->bitmap))
		return -EOVERFLOW;
	receiver->object = calloc(1, (size_t)header->object_len);
	receiver->bitmap = calloc(bitmap_words, sizeof(*receiver->bitmap));
	if (receiver->object == NULL || receiver->bitmap == NULL) {
		free(receiver->object);
		free(receiver->bitmap);
		receiver->object = NULL;
		receiver->bitmap = NULL;
		return -ENOMEM;
	}
	receiver->session_id = header->session_id;
	receiver->object_id = header->object_id;
	receiver->object_len = header->object_len;
	receiver->total_chunks = header->total_chunks;
	receiver->key_epoch = header->key_epoch;
	receiver->bitmap_words = bitmap_words;
	receiver->received_chunks = 0;
	receiver->ack_base = 0;
	receiver->ack_message_seq = 0;
	receiver->initialized = true;
	return 0;
}

static bool
same_object(const struct cvm_chunk_receiver *receiver,
		const struct cvm_chunk_header *header)
{
	return receiver->session_id == header->session_id &&
	       receiver->object_id == header->object_id &&
	       receiver->object_len == header->object_len &&
	       receiver->total_chunks == header->total_chunks &&
	       receiver->key_epoch == header->key_epoch;
}

int
cvm_chunk_receiver_init(struct cvm_chunk_receiver *receiver,
		const uint8_t key[CVM_CHUNK_KEY_SIZE], uint64_t max_object_len)
{
	if (receiver == NULL || key == NULL || max_object_len == 0 ||
	    max_object_len > SIZE_MAX)
		return -EINVAL;
	memset(receiver, 0, sizeof(*receiver));
	memcpy(receiver->key, key, CVM_CHUNK_KEY_SIZE);
	receiver->max_object_len = max_object_len;
	return 0;
}

void
cvm_chunk_receiver_destroy(struct cvm_chunk_receiver *receiver)
{
	if (receiver == NULL)
		return;
	if (receiver->object != NULL) {
		OPENSSL_cleanse(receiver->object, (size_t)receiver->object_len);
		free(receiver->object);
	}
	if (receiver->bitmap != NULL) {
		OPENSSL_cleanse(receiver->bitmap,
				receiver->bitmap_words * sizeof(*receiver->bitmap));
		free(receiver->bitmap);
	}
	OPENSSL_cleanse(receiver, sizeof(*receiver));
}

int
cvm_chunk_receiver_accept(struct cvm_chunk_receiver *receiver,
		const uint8_t *packet, size_t packet_len,
		enum cvm_chunk_accept_result *result)
{
	uint8_t plaintext[CVM_CHUNK_DATA_SIZE];
	struct cvm_chunk_header header;
	uint64_t offset;
	int ret;

	if (receiver == NULL || packet == NULL || result == NULL)
		return -EINVAL;
	ret = cvm_chunk_packet_open(receiver->key, packet, packet_len, &header,
				    plaintext, sizeof(plaintext));
	if (ret != 0)
		goto out;
	ret = cvm_chunk_data_header_validate(&header,
			receiver->max_object_len);
	if (ret != 0)
		goto out;
	/* No allocation or receiver-state mutation occurs before authentication. */
	if (!receiver->initialized) {
		ret = receiver_start_object(receiver, &header);
		if (ret != 0)
			goto out;
	} else if (!same_object(receiver, &header)) {
		ret = -EXDEV;
		goto out;
	}
	if (cvm_chunk_receiver_has(receiver, header.chunk_id)) {
		*result = CVM_CHUNK_ACCEPT_DUPLICATE;
		ret = 0;
		goto out;
	}
	ret = cvm_chunk_data_offset(&header, &offset);
	if (ret != 0 || offset > receiver->object_len - header.payload_len) {
		ret = -EOVERFLOW;
		goto out;
	}
	memcpy(receiver->object + offset, plaintext, header.payload_len);
	/* Commit the replay/placement state only after authentication and copy. */
	bitmap_set(receiver, header.chunk_id);
	receiver->received_chunks++;
	while (receiver->ack_base < receiver->total_chunks &&
	       cvm_chunk_receiver_has(receiver, receiver->ack_base))
		receiver->ack_base++;
	*result = receiver->received_chunks == receiver->total_chunks ?
		CVM_CHUNK_ACCEPT_COMPLETE : CVM_CHUNK_ACCEPT_NEW;
	ret = 0;
out:
	OPENSSL_cleanse(plaintext, sizeof(plaintext));
	return ret;
}

int
cvm_chunk_receiver_build_ack(struct cvm_chunk_receiver *receiver,
		uint8_t *packet, size_t packet_capacity, size_t *packet_len)
{
	uint8_t plaintext[CVM_CHUNK_ACK_BITMAP_SIZE];
	struct cvm_chunk_header header;
	uint64_t bitmap = 0;
	uint64_t i;
	int ret;

	if (receiver == NULL || !receiver->initialized || packet == NULL ||
	    packet_len == NULL ||
	    receiver->ack_message_seq > CVM_CHUNK_MAX_MESSAGE_SEQ)
		return -EINVAL;
	for (i = 0; i < 64 && receiver->ack_base + i < receiver->total_chunks;
	     i++) {
		if (cvm_chunk_receiver_has(receiver, receiver->ack_base + i))
			bitmap |= UINT64_C(1) << i;
	}
	cvm_chunk_ack_bitmap_encode(plaintext, bitmap);
	header = (struct cvm_chunk_header) {
		.flags = CVM_CHUNK_F_ACK,
		.key_epoch = receiver->key_epoch,
		.session_id = receiver->session_id,
		.object_id = receiver->object_id,
		.chunk_id = receiver->ack_base,
		.total_chunks = receiver->total_chunks,
		.object_len = receiver->object_len,
		.message_seq = receiver->ack_message_seq,
		.payload_len = CVM_CHUNK_ACK_BITMAP_SIZE,
	};
	ret = cvm_chunk_packet_build(receiver->key, &header, plaintext, packet,
				     packet_capacity, packet_len);
	if (ret == 0)
		receiver->ack_message_seq++;
	OPENSSL_cleanse(plaintext, sizeof(plaintext));
	return ret;
}
