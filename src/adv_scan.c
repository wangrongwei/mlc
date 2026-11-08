#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <immintrin.h>
#include "adv_scan.h"
#include "allocation.h"
#include "helper.h"

static int g_saved_1gb_node = -1;
static int g_saved_1gb_count = -1;

uint64_t pagemap_get_phys_addr(void *vaddr)
{
	int fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0)
		return 0;

	off_t offset = ((uintptr_t) vaddr / 4096) * 8;
	uint64_t entry = 0;
	ssize_t s = pread(fd, &entry, sizeof(entry), offset);
	close(fd);

	if (s == sizeof(entry) && (entry & (1ULL << 63))) {
		uint64_t pfn = entry & 0x7FFFFFFFFFFFFULL;
		return (pfn * 4096) + ((uintptr_t) vaddr & 0xFFFULL);
	}
	return 0;
}

static void hugepages_sig_handler(int sig)
{
	(void)sig;
	restore_1gb_hugepages();
	exit(1);
}

int reserve_1gb_hugepages(int numa_node, int pages_needed)
{
	char path[256];
	snprintf(path, sizeof(path),
		 "/sys/devices/system/node/node%d/hugepages/hugepages-1048576kB/nr_hugepages",
		 numa_node);

	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	int curr = 0;
	if (fscanf(f, "%d", &curr) != 1) {
		fclose(f);
		return -1;
	}
	fclose(f);

	if (g_saved_1gb_count < 0) {
		g_saved_1gb_node = numa_node;
		g_saved_1gb_count = curr;

		signal(SIGINT, hugepages_sig_handler);
		signal(SIGTERM, hugepages_sig_handler);
	}

	if (curr < pages_needed) {
		f = fopen(path, "w");
		if (!f)
			return -1;
		fprintf(f, "%d\n", pages_needed);
		fclose(f);
	}
	return 0;
}

void restore_1gb_hugepages(void)
{
	if (g_saved_1gb_count >= 0 && g_saved_1gb_node >= 0) {
		char path[256];
		snprintf(path, sizeof(path),
			 "/sys/devices/system/node/node%d/hugepages/hugepages-1048576kB/nr_hugepages",
			 g_saved_1gb_node);
		FILE *f = fopen(path, "w");
		if (f) {
			fprintf(f, "%d\n", g_saved_1gb_count);
			fclose(f);
		}
		g_saved_1gb_count = -1;
		g_saved_1gb_node = -1;
	}
}

void run_latency_histogram(size_t buf_size_mb, int bucket_granularity_ns)
{
	if (bucket_granularity_ns <= 0) {
		fprintf(stderr, "bucket granularity cannot be 0\n");
		exit(1);
	}

	size_t size = buf_size_mb * 1024 * 1024;
	void *buf = alloc_mem_onnode(size, 0, HUGEPAGE_NONE);
	if (!buf) {
		fprintf(stderr, "Buffer allocation failed!\n");
		return;
	}

	/* Initialize pointer chasing */
	size_t num_lines = size / 64;
	uintptr_t **ptrs = (uintptr_t **) buf;
	for (size_t i = 0; i < num_lines - 1; i++) {
		ptrs[i * 8] = (uintptr_t *) & ptrs[(i + 1) * 8];
	}
	ptrs[(num_lines - 1) * 8] = (uintptr_t *) & ptrs[0];

	double tsc_freq = helper_get_tsc_freq_ghz();
	if (tsc_freq <= 0.0)
		tsc_freq = 2.5;

	uint32_t buckets[DEFAULT_HISTOGRAM_BUCKETS];
	memset(buckets, 0, sizeof(buckets));

	uintptr_t *curr = (uintptr_t *) buf;
	size_t samples = 100000;

	for (size_t s = 0; s < samples; s++) {
		uint64_t t0 = __rdtsc();
		curr = (uintptr_t *) * curr;
		_mm_lfence();
		uint64_t t1 = __rdtsc();

		double ns = (double)(t1 - t0) / tsc_freq;
		int idx = (int)(ns / bucket_granularity_ns);
		if (idx < 0)
			idx = 0;
		if (idx >= DEFAULT_HISTOGRAM_BUCKETS)
			idx = DEFAULT_HISTOGRAM_BUCKETS - 1;
		buckets[idx]++;
	}

	printf("\n=== Latency Histogram (Bucket granularity = %d ns) ===\n",
	       bucket_granularity_ns);
	printf("  Bucket Range (ns)      Count      Percent\n");
	printf("---------------------------------------------\n");
	for (int b = 0; b < DEFAULT_HISTOGRAM_BUCKETS; b++) {
		if (buckets[b] > 0) {
			double pct =
			    (double)buckets[b] * 100.0 / (double)samples;
			printf("  [%4d - %4d ns]   %8u    %6.2f%%\n",
			       b * bucket_granularity_ns,
			       (b + 1) * bucket_granularity_ns, buckets[b],
			       pct);
		}
	}

	free_mem_onnode(buf, size);
}

int run_memory_bw_scan(const memory_scan_config_t *cfg)
{
	if (!cfg)
		return -1;
	if (cfg->bucket_granularity_ns <= 0) {
		fprintf(stderr, "bucket granularity cannot be 0\n");
		exit(1);
	}

	int node = cfg->numa_node;
	size_t free_kb = 0;

	char meminfo_path[256];
	snprintf(meminfo_path, sizeof(meminfo_path),
		 "/sys/devices/system/node/node%d/meminfo", node);
	FILE *f = fopen(meminfo_path, "r");
	if (f) {
		char line[256];
		while (fgets(line, sizeof(line), f)) {
			if (strstr(line, "MemFree:")) {
				sscanf(line, "%*s %*s %zu", &free_kb);
				break;
			}
		}
		fclose(f);
	}

	printf
	    ("Memory BW scan: allocation will attempt to maintain 10GB guardband on free memory.\n");
	if (free_kb <= DEFAULT_GUARDBAND_KB) {
		fprintf(stderr,
			"node%d has %zu kB free and the scan leaves %llu kB of that alone\n"
			"for the rest of the machine, so there is nothing left to scan.\n",
			node, free_kb,
			(unsigned long long)DEFAULT_GUARDBAND_KB);
		return -1;
	}

	size_t usable_kb = free_kb - DEFAULT_GUARDBAND_KB;
	if (cfg->scan_gb > 0 && (cfg->scan_gb * 1024 * 1024) < usable_kb) {
		usable_kb = cfg->scan_gb * 1024 * 1024;
	}

	printf
	    ("Running memory bandwidth scan using %d threads on numa node %d (scan size: %.2f GiB)\n",
	     cfg->num_threads, node, (double)usable_kb / (1024.0 * 1024.0));

	size_t size = usable_kb * 1024;
	hugepage_type_t hp = cfg->use_1gb_pages ? HUGEPAGE_1GB : HUGEPAGE_NONE;
	void *buf = alloc_mem_onnode(size, node, hp);
	if (!buf) {
		fprintf(stderr, "unable to allocate scan buffer .. exiting\n");
		return -1;
	}

	/* Measure bandwidth via AVX2 streaming read kernel */
	size_t count = size / sizeof(__m256i);
	__m256i *p = (__m256i *) buf;
	__m256i acc = _mm256_setzero_si256();

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC_RAW, &t0);

	int iters = (cfg->iterations > 0) ? cfg->iterations : 1;
	for (int it = 0; it < iters; it++) {
		for (size_t i = 0; i < count; i += 4) {
			acc =
			    _mm256_add_epi64(acc,
					     _mm256_stream_load_si256(&p[i]));
			acc =
			    _mm256_add_epi64(acc,
					     _mm256_stream_load_si256(&p
								      [i + 1]));
			acc =
			    _mm256_add_epi64(acc,
					     _mm256_stream_load_si256(&p
								      [i + 2]));
			acc =
			    _mm256_add_epi64(acc,
					     _mm256_stream_load_si256(&p
								      [i + 3]));
		}
	}

	clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
	double elapsed =
	    (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
	double gb_sec = (double)(size * iters) / (elapsed * 1e9);

	printf("Memory scan bandwidth: %.2f GB/s (elapsed: %.3f sec)\n", gb_sec,
	       elapsed);

	int64_t dummy[4];
	_mm256_storeu_si256((__m256i *) dummy, acc);
	if (dummy[0] == 999999999LL)
		printf(" ");

	free_mem_onnode(buf, size);
	restore_1gb_hugepages();
	return 0;
}
