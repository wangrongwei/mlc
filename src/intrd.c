#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <immintrin.h>
#include "intrd.h"
#include "allocation.h"
#include "helper.h"

void init_pointer_chase_chain(struct cache_line *buf, size_t size_bytes,
			      bool random_access)
{
	size_t num_lines = size_bytes / CACHE_LINE_SIZE;
	if (num_lines < 100)
		return;

	if (!random_access) {
		for (size_t i = 0; i < num_lines - 1; i++) {
			buf[i].next = &buf[i + 1];
		}
		buf[num_lines - 1].next = &buf[0];
	} else {
		size_t *indices = (size_t *)malloc(num_lines * sizeof(size_t));
		if (!indices)
			return;
		for (size_t i = 0; i < num_lines; i++)
			indices[i] = i;

		for (size_t i = num_lines - 1; i > 0; i--) {
			size_t j = rand() % i;
			size_t tmp = indices[i];
			indices[i] = indices[j];
			indices[j] = tmp;
		}
		for (size_t i = 0; i < num_lines - 1; i++) {
			buf[indices[i]].next = &buf[indices[i + 1]];
		}
		buf[indices[num_lines - 1]].next = &buf[indices[0]];
		free(indices);
	}
}

double measure_latency_on_node(int cpu, int alloc_node, size_t buf_size_mb,
			       bool random_access)
{
	// Phase 1: Pin to target allocation node's CPU to ensure pages are physically homed on alloc_node
	const numa_node_info_t *alloc_node_info = numa_get_node(alloc_node);
	if (alloc_node_info && alloc_node_info->num_cpus > 0) {
		numa_pin_cpu(alloc_node_info->cpus[0]);
	} else {
		numa_pin_cpu(cpu);
	}

	size_t size_bytes = buf_size_mb * 1024 * 1024;
	struct cache_line *head =
	    (struct cache_line *)alloc_mem_onnode(size_bytes, alloc_node,
						  false);
	if (!head)
		return 0.0;

	init_pointer_chase_chain(head, size_bytes, random_access);

	// Phase 2: Switch affinity to the measurement CPU for latency sampling
	numa_pin_cpu(cpu);

	// Warm up cache and TLB
	volatile struct cache_line *curr = head;
	for (int i = 0; i < 50000; i++) {
		curr = curr->next;
	}

	const uint64_t iterations = 2000000;
	_mm_lfence();
	uint64_t tsc_start = __rdtsc();
	_mm_lfence();

	for (uint64_t i = 0; i < iterations; i++) {
		curr = curr->next;
	}

	_mm_lfence();
	uint64_t tsc_end = __rdtsc();
	_mm_lfence();

	uint64_t total_cycles = tsc_end - tsc_start;
	double cycles_per_iter = (double)total_cycles / iterations;
	double latency_ns = cycles_per_iter / helper_get_tsc_freq_ghz();

	free_mem_onnode(head, size_bytes);
	return latency_ns;
}

double measure_latency(int cpu, size_t buf_size_mb, bool random_access)
{
	return measure_latency_on_node(cpu, 0, buf_size_mb, random_access);
}

void run_idle_latency(size_t buf_mb, bool random_access, bool keep_prefetchers)
{
	printf("Command line parameters: --idle_latency \n\n");
	printf("Using buffer size of %.3fMiB\n", (double)buf_mb);

	if (!keep_prefetchers) {
		msr_disable_prefetchers(false);
	}

	double lat_ns = measure_latency(0, buf_mb, random_access);
	double cycles = lat_ns * helper_get_tsc_freq_ghz();
	printf
	    ("Each iteration took %.1f base frequency clocks (       %.1f    ns)\n",
	     cycles, lat_ns);

	if (!keep_prefetchers) {
		msr_restore_prefetchers();
	}
}

void run_idle_latency_core(int cpu, size_t buf_mb, bool random_access,
			   bool keep_prefetchers)
{
	printf("Command line parameters: --idle_latency -c%d\n\n", cpu);
	printf("Using buffer size of %.3fMiB\n", (double)buf_mb);

	if (!keep_prefetchers) {
		msr_disable_prefetchers(false);
	}

	double lat_ns = measure_latency(cpu, buf_mb, random_access);
	double cycles = lat_ns * helper_get_tsc_freq_ghz();
	printf
	    ("Each iteration took %.1f base frequency clocks (       %.1f    ns)\n",
	     cycles, lat_ns);

	if (!keep_prefetchers) {
		msr_restore_prefetchers();
	}
}

void run_idle_latency_all_cores(size_t buf_mb, bool random_access,
				bool single_thread_per_core,
				bool keep_prefetchers)
{
	printf
	    ("Measuring idle latencies for %s access (in ns) for all cores...\n",
	     random_access ? "random" : "sequential");

	if (!keep_prefetchers) {
		msr_disable_prefetchers(false);
	}

	int num_nodes = numa_get_num_nodes();
	int total_cpus = numa_get_total_cpus();

	printf("                Numa node\n");
	printf("CPU      ");
	for (int j = 0; j < num_nodes; j++) {
		printf("%8d", j);
	}
	printf("  \n");

	for (int cpu = 0; cpu < total_cpus; cpu++) {
		if (single_thread_per_core && (cpu >= total_cpus / 2)) {
			continue;
		}
		printf("%6d   ", cpu);
		for (int j = 0; j < num_nodes; j++) {
			double lat =
			    measure_latency_on_node(cpu, j, buf_mb,
						    random_access);
			printf("%8.1f", lat);
			fflush(stdout);
		}
		printf("  \n");
	}

	if (!keep_prefetchers) {
		msr_restore_prefetchers();
	}
}

void run_latency_matrix(size_t buf_mb, bool random_access,
			bool keep_prefetchers)
{
	printf("Measuring idle latencies for %s access (in ns)...\n",
	       random_access ? "random" : "sequential");

	if (!keep_prefetchers) {
		msr_disable_prefetchers(false);
	}

	int num_nodes = numa_get_num_nodes();
	printf("                Numa node\nNuma node");
	for (int j = 0; j < num_nodes; j++) {
		printf("%8d", j);
	}
	printf("  \n");

	for (int i = 0; i < num_nodes; i++) {
		printf("%8d", i);
		const numa_node_info_t *node_i = numa_get_node(i);
		int cpu = node_i ? node_i->cpus[0] : 0;

		for (int j = 0; j < num_nodes; j++) {
			double lat =
			    measure_latency_on_node(cpu, j, buf_mb,
						    random_access);
			printf("%12.1f", lat);
			fflush(stdout);
		}
		printf("  \n");
	}

	if (!keep_prefetchers) {
		msr_restore_prefetchers();
	}
}
