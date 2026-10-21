#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/udp.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef SOL_UDP
#define SOL_UDP 17
#endif

static double
elapsed(const struct timespec *start, const struct timespec *end)
{
	return (double)(end->tv_sec - start->tv_sec) +
	       (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

static void
timespec_add_ns(struct timespec *value, uint64_t ns)
{
	value->tv_sec += (time_t)(ns / 1000000000ULL);
	value->tv_nsec += (long)(ns % 1000000000ULL);
	if (value->tv_nsec >= 1000000000L) {
		value->tv_sec++;
		value->tv_nsec -= 1000000000L;
	}
}

static int
pace_after_send(struct timespec *deadline, size_t bytes, uint64_t rate_bps)
{
	uint64_t delay_ns;
	int ret;

	if (rate_bps == 0)
		return 0;
	delay_ns = ((uint64_t)bytes * 8 * 1000000000ULL + rate_bps - 1) /
		   rate_bps;
	timespec_add_ns(deadline, delay_ns);
	do {
		ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
				      deadline, NULL);
	} while (ret == EINTR);
	if (ret != 0) {
		errno = ret;
		return -1;
	}
	return 0;
}

static int
parse_u64(const char *text, uint64_t *value)
{
	char *end;
	unsigned long long parsed;

	errno = 0;
	parsed = strtoull(text, &end, 0);
	if (errno != 0 || *text == '\0' || *end != '\0')
		return -1;
	*value = (uint64_t)parsed;
	return 0;
}

static ssize_t
send_payload(int fd, struct sockaddr_in *peer, void *buffer, size_t bytes,
	     uint16_t segment_size, uint16_t segments)
{
	char control[CMSG_SPACE(sizeof(uint16_t))] = {0};
	struct iovec iov = {
		.iov_base = buffer,
		.iov_len = bytes,
	};
	struct msghdr msg = {
		.msg_name = peer,
		.msg_namelen = sizeof(*peer),
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};

	if (segments > 1) {
		struct cmsghdr *cmsg;

		msg.msg_control = control;
		msg.msg_controllen = sizeof(control);
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_UDP;
		cmsg->cmsg_type = UDP_SEGMENT;
		cmsg->cmsg_len = CMSG_LEN(sizeof(segment_size));
		memcpy(CMSG_DATA(cmsg), &segment_size, sizeof(segment_size));
	}
	return sendmsg(fd, &msg, MSG_DONTWAIT);
}

int
main(int argc, char **argv)
{
	struct sockaddr_in local = { .sin_family = AF_INET };
	struct sockaddr_in peer = { .sin_family = AF_INET };
	struct timespec start, end, pace_deadline;
	uint64_t total_bytes, sent_bytes = 0, packets = 0, blocked = 0;
	uint64_t rate_bps = 0;
	uint64_t source_port_u64, segment_size_u64, max_segments_u64;
	uint16_t source_port, segment_size, max_segments;
	size_t buffer_size;
	unsigned char *buffer;
	int fd, sndbuf = 4 * 1024 * 1024, ttl = 1;

	if (argc != 8 && argc != 9) {
		fprintf(stderr,
			"usage: %s IFACE SRC_IP DST_IP TOTAL_BYTES SRC_PORT "
			"SEGMENT_PAYLOAD GSO_SEGMENTS [RATE_BPS]\n",
			argv[0]);
		return 2;
	}
	if (parse_u64(argv[4], &total_bytes) != 0 || total_bytes == 0 ||
	    parse_u64(argv[5], &source_port_u64) != 0 ||
	    source_port_u64 > UINT16_MAX || source_port_u64 == 0 ||
	    parse_u64(argv[6], &segment_size_u64) != 0 ||
	    segment_size_u64 > UINT16_MAX || segment_size_u64 == 0 ||
	    parse_u64(argv[7], &max_segments_u64) != 0 ||
	    max_segments_u64 > UINT16_MAX || max_segments_u64 == 0) {
		fprintf(stderr, "invalid numeric argument\n");
		return 2;
	}
	if (argc == 9 &&
	    (parse_u64(argv[8], &rate_bps) != 0 || rate_bps == 0)) {
		fprintf(stderr, "invalid pacing rate\n");
		return 2;
	}
	source_port = (uint16_t)source_port_u64;
	segment_size = (uint16_t)segment_size_u64;
	max_segments = (uint16_t)max_segments_u64;
	buffer_size = (size_t)segment_size * max_segments;
	if (buffer_size > UINT16_MAX) {
		fprintf(stderr, "one UDP GSO datagram must not exceed 65535 bytes\n");
		return 2;
	}
	buffer = malloc(buffer_size);
	if (buffer == NULL) {
		perror("malloc");
		return 1;
	}
	/* Deterministic, non-compressible-looking synthetic tensor bytes. */
	for (size_t i = 0; i < buffer_size; i++)
		buffer[i] = (unsigned char)((i * 131u + source_port) & 0xffu);

	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, argv[1],
		       strlen(argv[1]) + 1) != 0) {
		perror("SO_BINDTODEVICE");
		return 1;
	}
	if (setsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) != 0) {
		perror("IP_TTL");
		return 1;
	}
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
	local.sin_port = htons(source_port);
	peer.sin_port = htons(9000);
	if (inet_pton(AF_INET, argv[2], &local.sin_addr) != 1 ||
	    inet_pton(AF_INET, argv[3], &peer.sin_addr) != 1) {
		fprintf(stderr, "invalid IP address\n");
		return 2;
	}
	if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
		perror("bind");
		return 1;
	}

	clock_gettime(CLOCK_MONOTONIC, &start);
	pace_deadline = start;
	while (sent_bytes < total_bytes) {
		uint64_t remaining = total_bytes - sent_bytes;
		size_t bytes = remaining < buffer_size ? (size_t)remaining : buffer_size;
		uint16_t segments;
		ssize_t sent;

		/* A partial final segment is sent separately from a full GSO batch. */
		if (bytes > segment_size && bytes % segment_size != 0)
			bytes -= bytes % segment_size;
		segments = (uint16_t)((bytes + segment_size - 1) / segment_size);
		sent = send_payload(fd, &peer, buffer, bytes, segment_size, segments);
		if (sent >= 0) {
			sent_bytes += (uint64_t)sent;
			packets += (uint64_t)segments;
			if (pace_after_send(&pace_deadline, (size_t)sent,
					    rate_bps) != 0) {
				perror("clock_nanosleep");
				return 1;
			}
		} else if (errno == EAGAIN || errno == ENOBUFS) {
			blocked++;
		} else {
			perror("sendmsg");
			return 1;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &end);
	printf("src_port=%u seconds=%.9f packets=%" PRIu64
	       " payload_bytes=%" PRIu64 " blocked=%" PRIu64
	       " rate_bps=%" PRIu64 "\n",
	       source_port, elapsed(&start, &end), packets, sent_bytes, blocked,
	       rate_bps);
	close(fd);
	free(buffer);
	return 0;
}
