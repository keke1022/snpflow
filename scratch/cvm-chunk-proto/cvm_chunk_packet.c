/* SPDX-License-Identifier: BSD-3-Clause */

#include "cvm_chunk_packet.h"

#include <errno.h>

int
cvm_chunk_packet_build(const uint8_t key[CVM_CHUNK_KEY_SIZE],
		const struct cvm_chunk_header *header,
		const uint8_t *plaintext, uint8_t *packet, size_t packet_capacity,
		size_t *packet_len)
{
	size_t required;
	int ret;

	if (key == NULL || header == NULL || plaintext == NULL || packet == NULL ||
	    packet_len == NULL || header->payload_len > CVM_CHUNK_DATA_SIZE ||
	    header->message_seq > CVM_CHUNK_MAX_MESSAGE_SEQ ||
	    (header->flags != CVM_CHUNK_F_DATA &&
	     header->flags != CVM_CHUNK_F_ACK))
		return -EINVAL;
	required = CVM_CHUNK_HEADER_SIZE + header->payload_len +
		   CVM_CHUNK_TAG_SIZE;
	if (packet_capacity < required)
		return -ENOSPC;
	cvm_chunk_header_encode(packet, header);
	ret = cvm_chunk_seal(key, header, plaintext, header->payload_len,
			     packet + CVM_CHUNK_HEADER_SIZE,
			     packet + CVM_CHUNK_HEADER_SIZE + header->payload_len);
	if (ret != 0)
		return ret;
	*packet_len = required;
	return 0;
}

int
cvm_chunk_packet_open(const uint8_t key[CVM_CHUNK_KEY_SIZE],
		const uint8_t *packet, size_t packet_len,
		struct cvm_chunk_header *header, uint8_t *plaintext,
		size_t plaintext_capacity)
{
	size_t required;
	int ret;

	if (key == NULL || packet == NULL || header == NULL || plaintext == NULL)
		return -EINVAL;
	ret = cvm_chunk_header_decode(header, packet, packet_len);
	if (ret != 0)
		return ret;
	if (header->payload_len > CVM_CHUNK_DATA_SIZE ||
	    header->message_seq > CVM_CHUNK_MAX_MESSAGE_SEQ ||
	    (header->flags != CVM_CHUNK_F_DATA &&
	     header->flags != CVM_CHUNK_F_ACK))
		return -EPROTO;
	if (plaintext_capacity < header->payload_len)
		return -EMSGSIZE;
	required = CVM_CHUNK_HEADER_SIZE + header->payload_len +
		   CVM_CHUNK_TAG_SIZE;
	if (packet_len != required)
		return -EMSGSIZE;
	return cvm_chunk_open(key, header, packet + CVM_CHUNK_HEADER_SIZE,
			header->payload_len,
			packet + CVM_CHUNK_HEADER_SIZE + header->payload_len,
			plaintext);
}
