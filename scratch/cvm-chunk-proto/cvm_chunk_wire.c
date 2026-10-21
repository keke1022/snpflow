/* SPDX-License-Identifier: BSD-3-Clause */

#include "cvm_chunk_wire.h"

#include <errno.h>
#include <limits.h>

static void
put_u16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)(value >> 8);
	p[1] = (uint8_t)value;
}

static void
put_u32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)(value >> 24);
	p[1] = (uint8_t)(value >> 16);
	p[2] = (uint8_t)(value >> 8);
	p[3] = (uint8_t)value;
}

static void
put_u64(uint8_t *p, uint64_t value)
{
	put_u32(p, (uint32_t)(value >> 32));
	put_u32(p + 4, (uint32_t)value);
}

static uint16_t
get_u16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t
get_u32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t
get_u64(const uint8_t *p)
{
	return ((uint64_t)get_u32(p) << 32) | get_u32(p + 4);
}

void
cvm_chunk_header_encode(uint8_t out[CVM_CHUNK_HEADER_SIZE],
		const struct cvm_chunk_header *header)
{
	put_u32(out + 0, CVM_CHUNK_MAGIC);
	put_u16(out + 4, CVM_CHUNK_VERSION);
	put_u16(out + 6, CVM_CHUNK_HEADER_SIZE);
	put_u32(out + 8, header->flags);
	put_u32(out + 12, header->key_epoch);
	put_u64(out + 16, header->session_id);
	put_u64(out + 24, header->object_id);
	put_u64(out + 32, header->chunk_id);
	put_u64(out + 40, header->total_chunks);
	put_u64(out + 48, header->object_len);
	put_u64(out + 56, header->message_seq);
	put_u32(out + 64, header->payload_len);
	put_u32(out + 68, 0);
}

int
cvm_chunk_header_decode(struct cvm_chunk_header *header,
		const uint8_t *wire, size_t wire_len)
{
	if (header == NULL || wire == NULL || wire_len < CVM_CHUNK_HEADER_SIZE)
		return -EMSGSIZE;
	if (get_u32(wire + 0) != CVM_CHUNK_MAGIC ||
	    get_u16(wire + 4) != CVM_CHUNK_VERSION ||
	    get_u16(wire + 6) != CVM_CHUNK_HEADER_SIZE ||
	    get_u32(wire + 68) != 0)
		return -EPROTO;
	header->flags = get_u32(wire + 8);
	header->key_epoch = get_u32(wire + 12);
	header->session_id = get_u64(wire + 16);
	header->object_id = get_u64(wire + 24);
	header->chunk_id = get_u64(wire + 32);
	header->total_chunks = get_u64(wire + 40);
	header->object_len = get_u64(wire + 48);
	header->message_seq = get_u64(wire + 56);
	header->payload_len = get_u32(wire + 64);
	return 0;
}

int
cvm_chunk_data_offset(const struct cvm_chunk_header *header, uint64_t *offset)
{
	if (header == NULL || offset == NULL ||
	    header->chunk_id > UINT64_MAX / CVM_CHUNK_DATA_SIZE)
		return -EOVERFLOW;
	*offset = header->chunk_id * CVM_CHUNK_DATA_SIZE;
	return 0;
}

int
cvm_chunk_data_header_validate(const struct cvm_chunk_header *header,
		uint64_t max_object_len)
{
	uint64_t expected_chunks;
	uint64_t expected_len;
	uint64_t offset;

	if (header == NULL || header->flags != CVM_CHUNK_F_DATA ||
	    header->session_id == 0 || header->object_id == 0 ||
	    header->object_len == 0 || header->object_len > max_object_len ||
	    header->message_seq != header->chunk_id ||
	    header->message_seq > CVM_CHUNK_MAX_MESSAGE_SEQ)
		return -EINVAL;
	/* Division plus remainder avoids overflowing object_len near UINT64_MAX. */
	expected_chunks = header->object_len / CVM_CHUNK_DATA_SIZE;
	expected_chunks += header->object_len % CVM_CHUNK_DATA_SIZE != 0;
	/* Every DATA chunk uses chunk_id as its 31-bit nonce sequence. */
	if (expected_chunks > CVM_CHUNK_MAX_MESSAGE_SEQ + 1 ||
	    header->total_chunks != expected_chunks ||
	    header->chunk_id >= header->total_chunks)
		return -EINVAL;
	if (cvm_chunk_data_offset(header, &offset) != 0 ||
	    offset >= header->object_len)
		return -EINVAL;
	expected_len = header->object_len - offset;
	if (expected_len > CVM_CHUNK_DATA_SIZE)
		expected_len = CVM_CHUNK_DATA_SIZE;
	if (header->payload_len != expected_len)
		return -EINVAL;
	return 0;
}

int
cvm_chunk_ack_header_validate(const struct cvm_chunk_header *header,
		uint64_t expected_session_id, uint64_t expected_object_id,
		uint64_t expected_total_chunks, uint64_t expected_object_len,
		uint32_t expected_key_epoch)
{
	if (header == NULL || header->flags != CVM_CHUNK_F_ACK ||
	    header->session_id != expected_session_id ||
	    header->object_id != expected_object_id ||
	    header->total_chunks != expected_total_chunks ||
	    header->object_len != expected_object_len ||
	    header->key_epoch != expected_key_epoch ||
	    header->payload_len != CVM_CHUNK_ACK_BITMAP_SIZE ||
	    header->chunk_id > header->total_chunks ||
	    header->message_seq > CVM_CHUNK_MAX_MESSAGE_SEQ)
		return -EINVAL;
	return 0;
}

void
cvm_chunk_nonce(uint8_t nonce[CVM_CHUNK_NONCE_SIZE],
		const struct cvm_chunk_header *header)
{
	uint32_t suffix = (uint32_t)header->message_seq;

	if (header->flags == CVM_CHUNK_F_ACK)
		suffix |= UINT32_C(0x80000000);
	put_u64(nonce, header->object_id);
	put_u32(nonce + 8, suffix);
}

void
cvm_chunk_ack_bitmap_encode(uint8_t out[CVM_CHUNK_ACK_BITMAP_SIZE],
		uint64_t bitmap)
{
	put_u64(out, bitmap);
}

uint64_t
cvm_chunk_ack_bitmap_decode(const uint8_t in[CVM_CHUNK_ACK_BITMAP_SIZE])
{
	return get_u64(in);
}
