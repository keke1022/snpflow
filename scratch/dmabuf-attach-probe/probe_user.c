// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "probe_uapi.h"

#define CVM_SHARED_DEV "/dev/cvm_shared"
struct cvm_shared_alloc {
	__u64 size;
	__u32 flags;
	__s32 fd;
};
#define CVM_SHARED_IOC_MAGIC 'C'
#define CVM_SHARED_IOC_ALLOC \
	_IOWR(CVM_SHARED_IOC_MAGIC, 0x01, struct cvm_shared_alloc)

static struct cvm_probe_pci_id id(unsigned int bus, unsigned int slot,
				  unsigned int function)
{
	struct cvm_probe_pci_id value = {
		.domain = 0,
		.bus = (__u8)bus,
		.devfn = (__u8)((slot << 3) | function),
	};
	return value;
}

static double elapsed(const struct timespec *start, const struct timespec *end)
{
	return (double)(end->tv_sec - start->tv_sec) +
	       (double)(end->tv_nsec - start->tv_nsec) / 1e9;
}

int main(int argc, char **argv)
{
	unsigned long long size_mib = 2;
	unsigned long iterations = 1;
	struct cvm_shared_alloc alloc;
	struct cvm_dmabuf_probe_req req = {
		.flags = CVM_PROBE_F_SECOND,
		.first = id(0x05, 0, 0),  /* H200 */
		.second = id(0x04, 0, 0), /* ConnectX */
	};
	struct timespec start, end;
	unsigned long i;
	int cvm_fd, probe_fd;

	if (argc > 1)
		size_mib = strtoull(argv[1], NULL, 0);
	if (argc > 2)
		iterations = strtoul(argv[2], NULL, 0);
	if (!size_mib || size_mib > 1024 || !iterations) {
		fprintf(stderr, "usage: %s [size_MiB<=1024] [iterations]\n", argv[0]);
		return 2;
	}
	alloc = (struct cvm_shared_alloc) { .size = size_mib << 20 };

	cvm_fd = open(CVM_SHARED_DEV, O_RDWR | O_CLOEXEC);
	if (cvm_fd < 0) {
		perror("open cvm_shared");
		return 1;
	}
	if (ioctl(cvm_fd, CVM_SHARED_IOC_ALLOC, &alloc) != 0) {
		perror("CVM_SHARED_IOC_ALLOC");
		return 1;
	}
	req.fd = alloc.fd;
	probe_fd = open(CVM_DMABUF_PROBE_DEV, O_RDWR | O_CLOEXEC);
	if (probe_fd < 0) {
		perror("open cvm_dmabuf_probe");
		return 1;
	}
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (i = 0; i < iterations; i++) {
		if (ioctl(probe_fd, CVM_DMABUF_PROBE_IOC_ATTACH, &req) != 0) {
			fprintf(stderr, "attach/map probe iteration %lu failed: %s\n",
				i, strerror(errno));
			return 1;
		}
		if (!req.layouts_equal || req.first_bytes != alloc.size ||
		    req.second_bytes != alloc.size) {
			fprintf(stderr, "layout mismatch at iteration %lu\n", i);
			return 2;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &end);
	printf("size_MiB=%llu iterations=%lu seconds=%.9f us_per_iteration=%.3f\n",
	       size_mib, iterations, elapsed(&start, &end),
	       elapsed(&start, &end) * 1e6 / iterations);
	printf("gpu_nents=%u gpu_bytes=%" PRIu64 " gpu_first_dma=%#" PRIx64 "\n",
	       req.first_nents, (uint64_t)req.first_bytes,
	       (uint64_t)req.first_dma);
	printf("nic_nents=%u nic_bytes=%" PRIu64 " nic_first_dma=%#" PRIx64 "\n",
	       req.second_nents, (uint64_t)req.second_bytes,
	       (uint64_t)req.second_dma);
	printf("layouts_equal=%u\n", req.layouts_equal);
	close(probe_fd);
	close(alloc.fd);
	close(cvm_fd);
	return req.layouts_equal ? 0 : 2;
}
