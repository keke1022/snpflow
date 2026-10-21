// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CVM_SHARED_DEV "/dev/cvm_shared"
struct cvm_shared_alloc {
	__u64 size;
	__u32 flags;
	__s32 fd;
};
#define CVM_SHARED_IOC_MAGIC 'C'
#define CVM_SHARED_IOC_ALLOC \
	_IOWR(CVM_SHARED_IOC_MAGIC, 0x01, struct cvm_shared_alloc)

struct worker {
	const unsigned char *src;
	unsigned char *dst;
	size_t length;
	unsigned int repeats;
	unsigned int cpu;
	pthread_barrier_t *start;
	pthread_barrier_t *finish;
};

static double elapsed(const struct timespec *a, const struct timespec *b)
{
	return (double)(b->tv_sec - a->tv_sec) +
	       (double)(b->tv_nsec - a->tv_nsec) / 1e9;
}

static void *copy_worker(void *opaque)
{
	struct worker *w = opaque;
	cpu_set_t cpus;
	unsigned int i;

	CPU_ZERO(&cpus);
	CPU_SET(w->cpu, &cpus);
	(void)pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
	pthread_barrier_wait(w->start);
	for (i = 0; i < w->repeats; i++)
		memcpy(w->dst, w->src, w->length);
	pthread_barrier_wait(w->finish);
	return NULL;
}

static int run_one(const unsigned char *shared, unsigned char *private,
		   size_t size, unsigned int repeats, unsigned int nthreads)
{
	pthread_barrier_t start, finish;
	struct timespec begin, end;
	struct worker workers[8];
	pthread_t threads[8];
	size_t slice = size / nthreads;
	uint64_t checksum = 0;
	unsigned int i;
	double seconds;

	if (nthreads > 8 || size % nthreads)
		return -1;
	pthread_barrier_init(&start, NULL, nthreads + 1);
	pthread_barrier_init(&finish, NULL, nthreads + 1);
	for (i = 0; i < nthreads; i++) {
		workers[i] = (struct worker) {
			.src = shared + i * slice,
			.dst = private + i * slice,
			.length = slice,
			.repeats = repeats,
			.cpu = i,
			.start = &start,
			.finish = &finish,
		};
		if (pthread_create(&threads[i], NULL, copy_worker, &workers[i]))
			return -1;
	}
	pthread_barrier_wait(&start);
	clock_gettime(CLOCK_MONOTONIC, &begin);
	pthread_barrier_wait(&finish);
	clock_gettime(CLOCK_MONOTONIC, &end);
	for (i = 0; i < nthreads; i++)
		pthread_join(threads[i], NULL);

	for (size_t offset = 0; offset < size; offset += 4096)
		checksum += private[offset];
	seconds = elapsed(&begin, &end);
	printf("threads=%u seconds=%.9f copied_GiB=%.3f GiB_per_s=%.3f "
	       "Gb_per_s=%.3f checksum=%" PRIu64 "\n",
	       nthreads, seconds, (double)size * repeats / (1ULL << 30),
	       (double)size * repeats / seconds / (1ULL << 30),
	       (double)size * repeats * 8.0 / seconds / 1e9, checksum);
	pthread_barrier_destroy(&finish);
	pthread_barrier_destroy(&start);
	return 0;
}

int main(void)
{
	const size_t size = 512ULL << 20;
	const unsigned int repeats = 32;
	struct cvm_shared_alloc alloc = { .size = size };
	unsigned char *shared, *private;
	int cvm_fd;

	cvm_fd = open(CVM_SHARED_DEV, O_RDWR | O_CLOEXEC);
	if (cvm_fd < 0 || ioctl(cvm_fd, CVM_SHARED_IOC_ALLOC, &alloc)) {
		perror("allocate cvm_shared");
		return 1;
	}
	shared = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, alloc.fd, 0);
	if (shared == MAP_FAILED) {
		perror("mmap cvm_shared");
		return 1;
	}
	if (posix_memalign((void **)&private, 4096, size)) {
		perror("posix_memalign");
		return 1;
	}
	memset(shared, 0x5a, size);
	memset(private, 0, size);
	if (run_one(shared, private, size, repeats, 1) ||
	    run_one(shared, private, size, repeats, 4) ||
	    run_one(shared, private, size, repeats, 8))
		return 1;

	free(private);
	munmap(shared, size);
	close(alloc.fd);
	close(cvm_fd);
	return 0;
}
