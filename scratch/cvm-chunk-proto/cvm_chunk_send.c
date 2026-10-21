/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE

#include "cvm_chunk_packet.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <net/if.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>

#define ACK_PORT 9000U
#define SEND_WINDOW 64U
/* Sub-100 ms fixed RTOs caused spurious retransmits when the unpinned host
 * processed hundreds of thousands of per-packet ACKs. Use the conservative
 * one-second initial RTO until this prototype grows a Karn/Jacobson estimator. */
#define RTO_NS UINT64_C(1000000000)
#define RUN_TIMEOUT_NS UINT64_C(30000000000)

enum fault_kind {
	FAULT_NONE = 0,
	FAULT_DROP,
	FAULT_CORRUPT,
	FAULT_REPLAY,
};

struct sender {
	int fd;
	uint8_t key[CVM_CHUNK_KEY_SIZE];
	uint8_t *acked;
	uint64_t *last_send_ns;
	uint32_t *send_count;
	uint64_t session_id;
	uint64_t object_id;
	uint64_t object_len;
	uint64_t total_chunks;
	uint64_t base;
	uint64_t transmissions;
	uint64_t retransmissions;
	uint64_t wire_bytes;
	uint64_t fault_chunk;
	uint64_t last_ack_seq;
	enum fault_kind fault_kind;
	bool have_ack_seq;
	bool fault_done;
};

static uint64_t
monotonic_ns(void)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
		return 0;
	return (uint64_t)value.tv_sec * UINT64_C(1000000000) + value.tv_nsec;
}

static int
parse_u64(const char *text, uint64_t *value)
{
	char *end;
	unsigned long long parsed;

	errno = 0;
	parsed = strtoull(text, &end, 0);
	if (errno != 0 || text[0] == '\0' || *end != '\0')
		return -EINVAL;
	*value = (uint64_t)parsed;
	return 0;
}

static int
parse_fault(const char *text, enum fault_kind *kind, uint64_t *chunk_id)
{
	const char *number;

	if (strncmp(text, "drop:", 5) == 0) {
		*kind = FAULT_DROP;
		number = text + 5;
	} else if (strncmp(text, "corrupt:", 8) == 0) {
		*kind = FAULT_CORRUPT;
		number = text + 8;
	} else if (strncmp(text, "replay:", 7) == 0) {
		*kind = FAULT_REPLAY;
		number = text + 7;
	} else {
		/* Retain the original numeric spelling as shorthand for drop:N. */
		*kind = FAULT_DROP;
		number = text;
	}
	return parse_u64(number, chunk_id);
}

static int
random_nonzero_u64(uint64_t *value)
{
	ssize_t ret;

	do {
		ret = getrandom(value, sizeof(*value), 0);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret != (ssize_t)sizeof(*value))
			return -EIO;
	} while (*value == 0);
	return 0;
}

static uint8_t
object_byte(uint64_t offset)
{
	/* Deterministic synthetic tensor-like contents, defined by global offset. */
	return (uint8_t)((offset * UINT64_C(131) + UINT64_C(0x5a)) & 0xffU);
}

static int
object_sha256(uint64_t object_len, uint8_t digest[EVP_MAX_MD_SIZE],
		unsigned int *digest_len)
{
	uint8_t buffer[64 * 1024];
	EVP_MD_CTX *ctx = NULL;
	uint64_t remaining = object_len;
	size_t i;
	int ret = -EIO;

	/* The generator has period 256; this buffer is an integral number of
	 * periods, so every subsequent full block has identical contents. */
	for (i = 0; i < sizeof(buffer); i++)
		buffer[i] = object_byte(i);
	ctx = EVP_MD_CTX_new();
	if (ctx == NULL || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1)
		goto out;
	while (remaining != 0) {
		size_t length = remaining < sizeof(buffer) ?
				(size_t)remaining : sizeof(buffer);

		if (EVP_DigestUpdate(ctx, buffer, length) != 1)
			goto out;
		remaining -= length;
	}
	if (EVP_DigestFinal_ex(ctx, digest, digest_len) != 1)
		goto out;
	ret = 0;
out:
	EVP_MD_CTX_free(ctx);
	OPENSSL_cleanse(buffer, sizeof(buffer));
	return ret;
}

static uint32_t
data_len(const struct sender *sender, uint64_t chunk_id)
{
	uint64_t offset = chunk_id * CVM_CHUNK_DATA_SIZE;
	uint64_t remaining = sender->object_len - offset;

	return (uint32_t)(remaining > CVM_CHUNK_DATA_SIZE ?
			  CVM_CHUNK_DATA_SIZE : remaining);
}

static int
send_chunk(struct sender *sender, uint64_t chunk_id, uint64_t now)
{
	uint8_t packet[CVM_CHUNK_MAX_UDP_PAYLOAD];
	uint8_t plaintext[CVM_CHUNK_DATA_SIZE];
	uint64_t offset = chunk_id * CVM_CHUNK_DATA_SIZE;
	uint32_t length = data_len(sender, chunk_id);
	struct cvm_chunk_header header = {
		.flags = CVM_CHUNK_F_DATA,
		.key_epoch = 1,
		.session_id = sender->session_id,
		.object_id = sender->object_id,
		.chunk_id = chunk_id,
		.total_chunks = sender->total_chunks,
		.object_len = sender->object_len,
		.message_seq = chunk_id,
		.payload_len = length,
	};
	size_t packet_len;
	ssize_t sent;
	ssize_t replayed;
	uint32_t i;
	int ret;

	for (i = 0; i < length; i++)
		plaintext[i] = object_byte(offset + i);
	ret = cvm_chunk_data_header_validate(&header, sender->object_len);
	if (ret != 0)
		goto out;
	ret = cvm_chunk_packet_build(sender->key, &header, plaintext, packet,
				     sizeof(packet), &packet_len);
	if (ret != 0)
		goto out;
	/* Fault injection affects exactly one initial datagram and starts its RTO. */
	if (!sender->fault_done && chunk_id == sender->fault_chunk &&
	    sender->send_count[chunk_id] == 0 &&
	    sender->fault_kind == FAULT_DROP) {
		sender->fault_done = true;
		sender->last_send_ns[chunk_id] = now;
		sender->send_count[chunk_id]++;
		ret = 0;
		goto out;
	}
	if (!sender->fault_done && chunk_id == sender->fault_chunk &&
	    sender->send_count[chunk_id] == 0 &&
	    sender->fault_kind == FAULT_CORRUPT) {
		packet[CVM_CHUNK_HEADER_SIZE] ^= 1U;
		sender->fault_done = true;
	}
	sent = send(sender->fd, packet, packet_len, MSG_DONTWAIT);
	if (sent < 0) {
		ret = errno == EAGAIN || errno == ENOBUFS ? -EAGAIN : -errno;
		goto out;
	}
	if ((size_t)sent != packet_len) {
		ret = -EIO;
		goto out;
	}
	if (sender->send_count[chunk_id] != 0)
		sender->retransmissions++;
	sender->send_count[chunk_id]++;
	sender->last_send_ns[chunk_id] = now;
	sender->transmissions++;
	sender->wire_bytes += packet_len;
	if (!sender->fault_done && chunk_id == sender->fault_chunk &&
	    sender->send_count[chunk_id] == 1 &&
	    sender->fault_kind == FAULT_REPLAY) {
		replayed = send(sender->fd, packet, packet_len, MSG_DONTWAIT);
		if (replayed != (ssize_t)packet_len) {
			ret = replayed < 0 ? -errno : -EIO;
			goto out;
		}
		sender->fault_done = true;
		sender->transmissions++;
		sender->wire_bytes += packet_len;
	}
	ret = 0;
out:
	OPENSSL_cleanse(plaintext, sizeof(plaintext));
	OPENSSL_cleanse(packet, sizeof(packet));
	return ret;
}

static void
mark_acked(struct sender *sender, uint64_t chunk_id)
{
	if (chunk_id < sender->total_chunks)
		sender->acked[chunk_id] = 1;
}

static int
receive_ack(struct sender *sender)
{
	uint8_t packet[CVM_CHUNK_MAX_UDP_PAYLOAD];
	uint8_t plaintext[CVM_CHUNK_DATA_SIZE];
	struct cvm_chunk_header header;
	uint64_t bitmap;
	ssize_t received;
	uint64_t i;
	int ret;

	received = recv(sender->fd, packet, sizeof(packet), MSG_DONTWAIT);
	if (received < 0)
		return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -errno;
	ret = cvm_chunk_packet_open(sender->key, packet, (size_t)received,
				    &header, plaintext, sizeof(plaintext));
	if (ret != 0)
		goto out;
	ret = cvm_chunk_ack_header_validate(&header, sender->session_id,
			sender->object_id, sender->total_chunks,
			sender->object_len, 1);
	if (ret != 0)
		goto out;
	/* Later ACKs contain monotonic receiver state; ignore authentic replays. */
	if (sender->have_ack_seq && header.message_seq <= sender->last_ack_seq) {
		ret = 0;
		goto out;
	}
	sender->have_ack_seq = true;
	sender->last_ack_seq = header.message_seq;
	for (i = sender->base; i < header.chunk_id; i++)
		mark_acked(sender, i);
	bitmap = cvm_chunk_ack_bitmap_decode(plaintext);
	for (i = 0; i < 64 && header.chunk_id + i < sender->total_chunks; i++)
		if ((bitmap & (UINT64_C(1) << i)) != 0)
			mark_acked(sender, header.chunk_id + i);
	while (sender->base < sender->total_chunks &&
	       sender->acked[sender->base] != 0)
		sender->base++;
	ret = 1;
out:
	OPENSSL_cleanse(plaintext, sizeof(plaintext));
	OPENSSL_cleanse(packet, sizeof(packet));
	return ret;
}

static int
run_sender(struct sender *sender)
{
	uint64_t start = monotonic_ns();
	struct pollfd pfd = { .fd = sender->fd, .events = POLLIN };

	while (sender->base < sender->total_chunks) {
		uint64_t now = monotonic_ns();
		uint64_t end = sender->base + SEND_WINDOW;
		uint64_t i;
		int ret;

		if (now == 0 || now - start > RUN_TIMEOUT_NS)
			return -ETIMEDOUT;
		if (end < sender->base || end > sender->total_chunks)
			end = sender->total_chunks;
		for (i = sender->base; i < end; i++) {
			if (sender->acked[i] != 0 ||
			    (sender->send_count[i] != 0 &&
			     now - sender->last_send_ns[i] < RTO_NS))
				continue;
			ret = send_chunk(sender, i, now);
			if (ret != 0 && ret != -EAGAIN)
				return ret;
		}
		ret = poll(&pfd, 1, 2);
		if (ret < 0 && errno != EINTR)
			return -errno;
		if (ret > 0 && (pfd.revents & POLLIN) != 0) {
			do {
				ret = receive_ack(sender);
			} while (ret > 0);
			if (ret < 0 && ret != -EBADMSG && ret != -EPROTO &&
			    ret != -EINVAL && ret != -EMSGSIZE)
				return ret;
		}
	}
	return 0;
}

int
main(int argc, char **argv)
{
	struct sockaddr_in local = { .sin_family = AF_INET };
	struct sockaddr_in peer = { .sin_family = AF_INET };
	struct sender sender = { .fd = -1, .fault_chunk = UINT64_MAX };
	uint64_t source_port;
	uint64_t start;
	uint64_t finish;
	uint8_t source_digest[EVP_MAX_MD_SIZE];
	unsigned int source_digest_len = 0;
	unsigned int i;
	double seconds;
	int ret = EXIT_FAILURE;
	int rc;

	if (argc != 7 && argc != 8) {
		fprintf(stderr, "usage: %s IFACE SRC_IP DST_IP OBJECT_BYTES "
			"SRC_PORT KEY_HEX [drop:N|corrupt:N|replay:N]\n",
			argv[0]);
		return 2;
	}
	if (parse_u64(argv[4], &sender.object_len) != 0 ||
	    sender.object_len == 0 ||
	    parse_u64(argv[5], &source_port) != 0 || source_port == 0 ||
	    source_port > UINT16_MAX ||
	    cvm_chunk_key_parse(sender.key, argv[6]) != 0 ||
	    (argc == 8 && parse_fault(argv[7], &sender.fault_kind,
				     &sender.fault_chunk) != 0)) {
		fprintf(stderr, "invalid argument\n");
		return 2;
	}
	sender.total_chunks = sender.object_len / CVM_CHUNK_DATA_SIZE;
	sender.total_chunks += sender.object_len % CVM_CHUNK_DATA_SIZE != 0;
	if (sender.total_chunks > CVM_CHUNK_MAX_MESSAGE_SEQ + 1 ||
	    (sender.fault_chunk != UINT64_MAX &&
	     sender.fault_chunk >= sender.total_chunks) ||
	    sender.total_chunks > SIZE_MAX / sizeof(*sender.acked) ||
	    sender.total_chunks > SIZE_MAX / sizeof(*sender.last_send_ns) ||
	    sender.total_chunks > SIZE_MAX / sizeof(*sender.send_count)) {
		fprintf(stderr, "object or fault-injection chunk is too large\n");
		goto out;
	}
	sender.acked = calloc((size_t)sender.total_chunks,
			      sizeof(*sender.acked));
	sender.last_send_ns = calloc((size_t)sender.total_chunks,
				     sizeof(*sender.last_send_ns));
	sender.send_count = calloc((size_t)sender.total_chunks,
				   sizeof(*sender.send_count));
	if (sender.acked == NULL || sender.last_send_ns == NULL ||
	    sender.send_count == NULL) {
		perror("calloc");
		goto out;
	}
	if (random_nonzero_u64(&sender.session_id) != 0 ||
	    random_nonzero_u64(&sender.object_id) != 0) {
		fprintf(stderr, "getrandom failed\n");
		goto out;
	}
	if (object_sha256(sender.object_len, source_digest,
			  &source_digest_len) != 0) {
		fprintf(stderr, "source SHA-256 failed\n");
		goto out;
	}
	sender.fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (sender.fd < 0) {
		perror("socket");
		goto out;
	}
	if (setsockopt(sender.fd, SOL_SOCKET, SO_BINDTODEVICE, argv[1],
		       strlen(argv[1]) + 1) != 0) {
		perror("SO_BINDTODEVICE");
		goto out;
	}
	local.sin_port = htons((uint16_t)source_port);
	peer.sin_port = htons(ACK_PORT);
	if (inet_pton(AF_INET, argv[2], &local.sin_addr) != 1 ||
	    inet_pton(AF_INET, argv[3], &peer.sin_addr) != 1) {
		fprintf(stderr, "invalid IP address\n");
		goto out;
	}
	if (bind(sender.fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
		perror("bind");
		goto out;
	}
	if (connect(sender.fd, (struct sockaddr *)&peer, sizeof(peer)) != 0) {
		perror("connect");
		goto out;
	}
	start = monotonic_ns();
	rc = run_sender(&sender);
	finish = monotonic_ns();
	if (rc != 0) {
		fprintf(stderr, "transfer failed: %s (%d)\n", strerror(-rc), rc);
		goto out;
	}
	seconds = (double)(finish - start) / 1000000000.0;
	printf("cvm_chunk_send=PASS object_bytes=%" PRIu64
	       " chunks=%" PRIu64 " transmissions=%" PRIu64
	       " retransmissions=%" PRIu64 " wire_bytes=%" PRIu64
	       " seconds=%.9f payload_gbps=%.6f object_id=%016" PRIx64
	       " sha256=",
	       sender.object_len, sender.total_chunks, sender.transmissions,
	       sender.retransmissions, sender.wire_bytes, seconds,
	       (double)sender.object_len * 8.0 / seconds / 1e9,
	       sender.object_id);
	for (i = 0; i < source_digest_len; i++)
		printf("%02x", source_digest[i]);
	printf("\n");
	ret = EXIT_SUCCESS;
out:
	if (sender.fd >= 0)
		close(sender.fd);
	free(sender.acked);
	free(sender.last_send_ns);
	free(sender.send_count);
	OPENSSL_cleanse(sender.key, sizeof(sender.key));
	OPENSSL_cleanse(source_digest, sizeof(source_digest));
	return ret;
}
