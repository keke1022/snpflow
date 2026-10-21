/* SPDX-License-Identifier: BSD-3-Clause */

#include "cvm_chunk_receiver.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/crypto.h>

#define TEST_KEY_HEX \
	"000102030405060708090a0b0c0d0e0f" \
	"101112131415161718191a1b1c1d1e1f"

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
			#condition); \
		goto fail; \
	} \
} while (0)

static uint32_t
chunk_len(uint64_t object_len, uint64_t chunk_id)
{
	uint64_t offset = chunk_id * CVM_CHUNK_DATA_SIZE;
	uint64_t remaining = object_len - offset;

	return (uint32_t)(remaining > CVM_CHUNK_DATA_SIZE ?
			  CVM_CHUNK_DATA_SIZE : remaining);
}

static int
build_data_packet(const uint8_t key[CVM_CHUNK_KEY_SIZE],
		const uint8_t *object, uint64_t object_len, uint64_t chunk_id,
		uint8_t packet[CVM_CHUNK_MAX_UDP_PAYLOAD], size_t *packet_len)
{
	uint64_t total_chunks = (object_len + CVM_CHUNK_DATA_SIZE - 1) /
				CVM_CHUNK_DATA_SIZE;
	uint64_t offset = chunk_id * CVM_CHUNK_DATA_SIZE;
	struct cvm_chunk_header header = {
		.flags = CVM_CHUNK_F_DATA,
		.key_epoch = 7,
		.session_id = UINT64_C(0x1020304050607080),
		.object_id = UINT64_C(0x8877665544332211),
		.chunk_id = chunk_id,
		.total_chunks = total_chunks,
		.object_len = object_len,
		.message_seq = chunk_id,
		.payload_len = chunk_len(object_len, chunk_id),
	};

	return cvm_chunk_packet_build(key, &header, object + offset, packet,
				      CVM_CHUNK_MAX_UDP_PAYLOAD, packet_len);
}

static int
test_wire_boundaries(void)
{
	const uint64_t max_chunks = CVM_CHUNK_MAX_MESSAGE_SEQ + 1;
	const uint64_t max_object_len = max_chunks * CVM_CHUNK_DATA_SIZE;
	struct cvm_chunk_header header = {
		.flags = CVM_CHUNK_F_DATA,
		.key_epoch = 1,
		.session_id = 1,
		.object_id = 1,
		.chunk_id = CVM_CHUNK_MAX_MESSAGE_SEQ,
		.total_chunks = max_chunks,
		.object_len = max_object_len,
		.message_seq = CVM_CHUNK_MAX_MESSAGE_SEQ,
		.payload_len = CVM_CHUNK_DATA_SIZE,
	};
	uint64_t offset;
	uint8_t key[CVM_CHUNK_KEY_SIZE] = {0};
	uint8_t plaintext[CVM_CHUNK_ACK_BITMAP_SIZE] = {0};
	uint8_t packet[CVM_CHUNK_MAX_UDP_PAYLOAD];
	size_t packet_len;

	if (cvm_chunk_data_header_validate(&header, max_object_len) != 0)
		return -1;
	header.object_len++;
	header.total_chunks++;
	if (cvm_chunk_data_header_validate(&header, header.object_len) != -EINVAL)
		return -1;
	header.object_len = UINT64_MAX;
	header.total_chunks = UINT64_MAX;
	header.chunk_id = 0;
	header.message_seq = 0;
	header.payload_len = CVM_CHUNK_DATA_SIZE;
	if (cvm_chunk_data_header_validate(&header, UINT64_MAX) != -EINVAL)
		return -1;
	header.chunk_id = UINT64_MAX;
	if (cvm_chunk_data_offset(&header, &offset) != -EOVERFLOW)
		return -1;
	header = (struct cvm_chunk_header) {
		.flags = CVM_CHUNK_F_ACK,
		.message_seq = CVM_CHUNK_MAX_MESSAGE_SEQ + 1,
		.payload_len = sizeof(plaintext),
	};
	if (cvm_chunk_packet_build(key, &header, plaintext, packet,
				   sizeof(packet), &packet_len) != -EINVAL)
		return -1;
	header.message_seq = 0;
	header.flags = UINT32_C(0x40000000);
	if (cvm_chunk_packet_build(key, &header, plaintext, packet,
				   sizeof(packet), &packet_len) != -EINVAL)
		return -1;
	return 0;
}

int
main(void)
{
	enum { TEST_CHUNKS = 4 };
	uint8_t packets[TEST_CHUNKS][CVM_CHUNK_MAX_UDP_PAYLOAD];
	size_t packet_lens[TEST_CHUNKS];
	uint8_t ack_packet[CVM_CHUNK_MAX_UDP_PAYLOAD];
	uint8_t ack_plaintext[CVM_CHUNK_DATA_SIZE];
	uint8_t tampered[CVM_CHUNK_MAX_UDP_PAYLOAD];
	uint8_t key[CVM_CHUNK_KEY_SIZE];
	struct cvm_chunk_receiver receiver;
	struct cvm_chunk_receiver probe_receiver;
	struct cvm_chunk_header ack_header;
	struct cvm_chunk_header decoded;
	enum cvm_chunk_accept_result result;
	uint64_t object_len = CVM_CHUNK_DATA_SIZE * 3U + 17U;
	uint8_t *object = NULL;
	size_t ack_len;
	size_t alien_len;
	uint64_t bitmap;
	uint8_t data_nonce[CVM_CHUNK_NONCE_SIZE];
	uint8_t ack_nonce[CVM_CHUNK_NONCE_SIZE];
	int receiver_ready = 0;
	int ret = EXIT_FAILURE;
	int rc;
	uint64_t i;

	CHECK(CVM_CHUNK_DATA_SIZE == 1384U);
	CHECK(test_wire_boundaries() == 0);
	CHECK(cvm_chunk_key_parse(key, TEST_KEY_HEX) == 0);
	CHECK(cvm_chunk_key_parse(key, "bad") == -EINVAL);
	for (i = 0; i < sizeof(key); i++)
		CHECK(key[i] == 0);
	CHECK(cvm_chunk_key_parse(key, TEST_KEY_HEX) == 0);
	object = malloc((size_t)object_len);
	CHECK(object != NULL);
	for (i = 0; i < object_len; i++)
		object[i] = (uint8_t)((i * 131U + 0x5aU) & 0xffU);
	for (i = 0; i < TEST_CHUNKS; i++)
		CHECK(build_data_packet(key, object, object_len, i, packets[i],
					&packet_lens[i]) == 0);

	/* An unauthenticated first packet must not allocate object state. */
	CHECK(cvm_chunk_receiver_init(&probe_receiver, key, object_len) == 0);
	memcpy(tampered, packets[0], packet_lens[0]);
	tampered[CVM_CHUNK_HEADER_SIZE] ^= 1U;
	CHECK(cvm_chunk_receiver_accept(&probe_receiver, tampered,
					packet_lens[0], &result) == -EBADMSG);
	CHECK(!probe_receiver.initialized);
	CHECK(probe_receiver.object == NULL && probe_receiver.bitmap == NULL);
	cvm_chunk_receiver_destroy(&probe_receiver);

	CHECK(cvm_chunk_header_decode(&decoded, packets[0], packet_lens[0]) == 0);
	CHECK(cvm_chunk_data_header_validate(&decoded, object_len) == 0);
	CHECK(decoded.payload_len == CVM_CHUNK_DATA_SIZE);
	cvm_chunk_nonce(data_nonce, &decoded);

	CHECK(cvm_chunk_receiver_init(&receiver, key, object_len) == 0);
	receiver_ready = 1;

	/* Deliberately deliver chunk 2 before chunk 0. */
	CHECK(cvm_chunk_receiver_accept(&receiver, packets[2], packet_lens[2],
					&result) == 0);
	CHECK(result == CVM_CHUNK_ACCEPT_NEW);
	CHECK(receiver.ack_base == 0);
	CHECK(cvm_chunk_receiver_accept(&receiver, packets[0], packet_lens[0],
					&result) == 0);
	CHECK(result == CVM_CHUNK_ACCEPT_NEW);
	CHECK(receiver.ack_base == 1);

	/* An authenticated replay is recognized but does not advance state. */
	CHECK(cvm_chunk_receiver_accept(&receiver, packets[2], packet_lens[2],
					&result) == 0);
	CHECK(result == CVM_CHUNK_ACCEPT_DUPLICATE);
	CHECK(receiver.received_chunks == 2);
	/* A corrupted replay is authenticated before the duplicate lookup. */
	memcpy(tampered, packets[2], packet_lens[2]);
	tampered[CVM_CHUNK_HEADER_SIZE] ^= 1U;
	CHECK(cvm_chunk_receiver_accept(&receiver, tampered, packet_lens[2],
					&result) == -EBADMSG);
	CHECK(receiver.received_chunks == 2);

	/* A separately authenticated object cannot replace the active manifest. */
	CHECK(cvm_chunk_header_decode(&decoded, packets[0], packet_lens[0]) == 0);
	decoded.object_id ^= UINT64_C(0x0100000000000000);
	CHECK(cvm_chunk_packet_build(key, &decoded, object, tampered,
				     sizeof(tampered), &alien_len) == 0);
	CHECK(cvm_chunk_receiver_accept(&receiver, tampered, alien_len,
					&result) == -EXDEV);
	CHECK(receiver.received_chunks == 2);

	/* Corruption must not set the received bit or change the object. */
	memcpy(tampered, packets[3], packet_lens[3]);
	tampered[CVM_CHUNK_HEADER_SIZE] ^= 1U;
	CHECK(cvm_chunk_receiver_accept(&receiver, tampered, packet_lens[3],
					&result) == -EBADMSG);
	CHECK(!cvm_chunk_receiver_has(&receiver, 3));
	CHECK(receiver.received_chunks == 2);

	/* A header mutation is also covered by the AEAD associated data. */
	memcpy(tampered, packets[1], packet_lens[1]);
	tampered[39] ^= 1U;
	rc = cvm_chunk_receiver_accept(&receiver, tampered, packet_lens[1],
				       &result);
	CHECK(rc == -EBADMSG);
	CHECK(!cvm_chunk_receiver_has(&receiver, 1));

	/* ACK base=1 and bit 1 acknowledges out-of-order chunk 2. */
	CHECK(cvm_chunk_receiver_build_ack(&receiver, ack_packet,
					   sizeof(ack_packet), &ack_len) == 0);
	CHECK(cvm_chunk_packet_open(key, ack_packet, ack_len, &ack_header,
				    ack_plaintext, sizeof(ack_plaintext)) == 0);
	CHECK(cvm_chunk_ack_header_validate(&ack_header, receiver.session_id,
			receiver.object_id, receiver.total_chunks,
			receiver.object_len, receiver.key_epoch) == 0);
	CHECK(ack_header.chunk_id == 1);
	bitmap = cvm_chunk_ack_bitmap_decode(ack_plaintext);
	CHECK(bitmap == UINT64_C(2));
	memcpy(tampered, ack_packet, ack_len);
	tampered[ack_len - 1] ^= 1U;
	CHECK(cvm_chunk_packet_open(key, tampered, ack_len, &decoded,
				    ack_plaintext,
				    sizeof(ack_plaintext)) == -EBADMSG);
	cvm_chunk_nonce(ack_nonce, &ack_header);
	CHECK(memcmp(data_nonce, ack_nonce, sizeof(data_nonce)) != 0);

	CHECK(cvm_chunk_receiver_accept(&receiver, packets[1], packet_lens[1],
					&result) == 0);
	CHECK(result == CVM_CHUNK_ACCEPT_NEW);
	CHECK(cvm_chunk_receiver_accept(&receiver, packets[3], packet_lens[3],
					&result) == 0);
	CHECK(result == CVM_CHUNK_ACCEPT_COMPLETE);
	CHECK(receiver.ack_base == TEST_CHUNKS);
	CHECK(receiver.received_chunks == TEST_CHUNKS);
	CHECK(memcmp(receiver.object, object, (size_t)object_len) == 0);

	/* Completion does not make a replay acceptable as new data. */
	CHECK(cvm_chunk_receiver_accept(&receiver, packets[0], packet_lens[0],
					&result) == 0);
	CHECK(result == CVM_CHUNK_ACCEPT_DUPLICATE);
	CHECK(receiver.received_chunks == TEST_CHUNKS);

	printf("wire_roundtrip=PASS\n");
	printf("integer_boundaries=PASS\n");
	printf("aead_header_and_payload_tamper=PASS\n");
	printf("out_of_order_placement=PASS\n");
	printf("authenticated_replay_dedup=PASS\n");
	printf("failed_auth_does_not_commit=PASS\n");
	printf("failed_first_auth_does_not_allocate=PASS\n");
	printf("corrupt_replay_is_not_deduplicated=PASS\n");
	printf("authenticated_cross_object_isolation=PASS\n");
	printf("authenticated_ack_range=PASS\n");
	printf("object_reassembly=PASS bytes=%" PRIu64 " chunks=%u\n",
	       object_len, TEST_CHUNKS);
	ret = EXIT_SUCCESS;
fail:
	if (receiver_ready)
		cvm_chunk_receiver_destroy(&receiver);
	if (object != NULL) {
		OPENSSL_cleanse(object, (size_t)object_len);
		free(object);
	}
	OPENSSL_cleanse(key, sizeof(key));
	OPENSSL_cleanse(ack_plaintext, sizeof(ack_plaintext));
	return ret;
}
