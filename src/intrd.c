#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <immintrin.h>
#include "intrd.h"
#include "allocation.h"
#include "busythread.h"
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
			size_t j = (size_t)rand() % i;
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

double measure_latency_on_node(int cpu, int alloc_node_id, size_t buf_size_mb,
			       bool random_access)
{
	/*
	 * Phase 1: Pin to target allocation node's reserved[1] core (when
	 * available) so pages are homed on alloc_node_id without colliding
	 * with reserved[0]'s package-wakeup busythread. For CPU-less memory
	 * nodes (CXL/HBM), keep affinity on `cpu` and rely on mbind().
	 */
	const numa_node_info_t *alloc_node_info =
	    numa_get_node_by_id(alloc_node_id);
	if (alloc_node_info && alloc_node_info->num_cpus > 0) {
		int init_cpu = (alloc_node_info->reserved_cpus[1] >= 0)
		    ? alloc_node_info->reserved_cpus[1]
		    : alloc_node_info->cpus[0];
		numa_pin_cpu(init_cpu);
	} else {
		numa_pin_cpu(cpu);
	}

	size_t size_bytes = buf_size_mb * 1024 * 1024;
	struct cache_line *head =
	    (struct cache_line *)alloc_mem_onnode(size_bytes, alloc_node_id,
						  HUGEPAGE_NONE);
	if (!head)
		return 0.0;

	init_pointer_chase_chain(head, size_bytes, random_access);

	/* Phase 2: Switch affinity to the measurement CPU for latency sampling */
	numa_pin_cpu(cpu);

	/* Warm up cache and TLB */
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
	double cycles_per_iter = (double)total_cycles / (double)iterations;
	double latency_ns = cycles_per_iter / helper_get_tsc_freq_ghz();

	free_mem_onnode(head, size_bytes);
	return latency_ns;
}

static int resolve_idle_alloc_node_id(int cpu, int alloc_node_override)
{
	if (alloc_node_override >= 0)
		return alloc_node_override;
	const numa_node_info_t *local_node = numa_get_node_for_cpu(cpu);
	if (local_node && local_node->has_memory)
		return local_node->node_id;
	return -1;
}

double measure_latency(int cpu, size_t buf_size_mb, bool random_access)
{
	int alloc_node_id = resolve_idle_alloc_node_id(cpu, -1);
	return measure_latency_on_node(cpu, alloc_node_id, buf_size_mb,
				       random_access);
}

void run_idle_latency(size_t buf_mb, bool random_access, bool keep_prefetchers,
		      int alloc_node_override, bool verbose)
{
	printf("Command line parameters: --idle_latency \n\n");
	printf("Using buffer size of %.3fMiB\n", (double)buf_mb);

	if (!keep_prefetchers) {
		msr_disable_prefetchers(false);
	}

	int cpu = 0;
	const socket_info_t *sock0 = numa_get_socket(0);
	if (sock0 && sock0->reserved_cpus[0] >= 0)
		cpu = sock0->reserved_cpus[0];

	start_auto_busy_threads(-1, cpu, verbose);

	int alloc_node_id = resolve_idle_alloc_node_id(cpu, alloc_node_override);
	double lat_ns =
	    measure_latency_on_node(cpu, alloc_node_id, buf_mb, random_access);
	double cycles = lat_ns * helper_get_tsc_freq_ghz();
	printf
	    ("Each iteration took %.1f base frequency clocks (\t%.1f\tns)\n",
	     cycles, lat_ns);

	stop_auto_busy_threads();

	if (!keep_prefetchers) {
		msr_restore_prefetchers();
	}
}

void run_idle_latency_core(int cpu, size_t buf_mb, bool random_access,
			   bool keep_prefetchers, int alloc_node_override,
			   bool verbose)
{
	printf("Command line parameters: --idle_latency -c%d\n\n", cpu);
	printf("Using buffer size of %.3fMiB\n", (double)buf_mb);

	if (!keep_prefetchers) {
		msr_disable_prefetchers(false);
	}

	start_auto_busy_threads(-1, cpu, verbose);

	int alloc_node_id = resolve_idle_alloc_node_id(cpu, alloc_node_override);
	double lat_ns =
	    measure_latency_on_node(cpu, alloc_node_id, buf_mb, random_access);
	double cycles = lat_ns * helper_get_tsc_freq_ghz();
	printf
	    ("Each iteration took %.1f base frequency clocks (\t%.1f\tns)\n",
	     cycles, lat_ns);

	stop_auto_busy_threads();

	if (!keep_prefetchers) {
		msr_restore_prefetchers();
	}
}

void run_idle_latency_all_cores(size_t buf_mb, bool random_access,
				bool single_thread_per_core,
				bool keep_prefetchers, bool verbose)
{
	printf
	    ("Measuring idle latencies for %s access (in ns) for all cores...\n",
	     random_access ? "random" : "sequential");

	if (!keep_prefetchers) {
		msr_disable_prefetchers(false);
	}

	int num_nodes = numa_get_num_nodes();
	int total_cpus = numa_get_total_cpus();
	bool numa_mode = numa_is_numa_mode();

	if (numa_mode) {
		printf("\t\tNuma node\nCPU\t");
	} else {
		printf("\tMemory node\nCPU\t");
	}
	for (int j = 0; j < num_nodes; j++) {
		const numa_node_info_t *node_j = numa_get_node(j);
		printf("%6d\t", node_j ? node_j->node_id : j);
	}
	printf("\n");

	for (int cpu = 0; cpu < total_cpus; cpu++) {
		if (single_thread_per_core && !cpu_is_smt_primary(cpu)) {
			continue;
		}

		start_auto_busy_threads(cpu_get_socket_id(cpu), cpu, verbose);

		printf("%4d\t", cpu);
		for (int j = 0; j < num_nodes; j++) {
			const numa_node_info_t *node_j = numa_get_node(j);
			if (!node_j || (numa_mode && !node_j->has_memory)) {
				printf("     -\t");
			} else {
				double lat = measure_latency_on_node(
				    cpu, node_j->node_id, buf_mb, random_access);
				printf("%6.1f\t", lat);
			}
			fflush(stdout);
		}
		printf("\n");

		stop_auto_busy_threads();
	}

	if (!keep_prefetchers) {
		msr_restore_prefetchers();
	}
}

void run_latency_matrix(size_t buf_mb, bool random_access,
			bool keep_prefetchers, bool verbose)
{
	printf("Measuring idle latencies for %s access (in ns)...\n",
	       random_access ? "random" : "sequential");

	if (!keep_prefetchers) {
		msr_disable_prefetchers(false);
	}

	start_auto_busy_threads(-1, -1, verbose);

	int num_nodes = numa_get_num_nodes();
	bool numa_mode = numa_is_numa_mode();

	if (numa_mode) {
		printf("\t\tNuma node\nNuma node\t");
	} else {
		printf("\tMemory node\nSocket\t");
	}
	for (int j = 0; j < num_nodes; j++) {
		const numa_node_info_t *node_j = numa_get_node(j);
		printf("%6d\t", node_j ? node_j->node_id : j);
	}
	printf("\n");

	for (int i = 0; i < num_nodes; i++) {
		const numa_node_info_t *node_i = numa_get_node(i);
		if (!node_i || node_i->num_cpus <= 0) {
			/* Skip CPU-less memory nodes (CXL/HBM) on initiator rows */
			continue;
		}

		int cpu = (node_i->reserved_cpus[1] >= 0)
		    ? node_i->reserved_cpus[1]
		    : node_i->cpus[0];

		printf("%8d\t", node_i->node_id);
		for (int j = 0; j < num_nodes; j++) {
			const numa_node_info_t *node_j = numa_get_node(j);
			if (!node_j || (numa_mode && !node_j->has_memory)) {
				printf("     -\t");
			} else {
				double lat = measure_latency_on_node(
				    cpu, node_j->node_id, buf_mb, random_access);
				printf("%6.1f\t", lat);
			}
			fflush(stdout);
		}
		printf("\n");
	}

	stop_auto_busy_threads();

	if (!keep_prefetchers) {
		msr_restore_prefetchers();
	}
}
