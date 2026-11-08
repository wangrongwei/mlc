#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include "allocation.h"

#ifndef MPOL_BIND
#define MPOL_BIND 2
#endif

#ifndef MPOL_MF_STRICT
#define MPOL_MF_STRICT 1
#endif

static int g_num_nodes = 0;
static numa_node_info_t g_nodes[MAX_NUMA_NODES];
static int g_total_cpus = 0;

static int parse_cpulist(const char *str, int *cpus, int max_cpus)
{
	int count = 0;
	char buf[1024];
	strncpy(buf, str, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	char *token = strtok(buf, ",");
	while (token && count < max_cpus) {
		int start, end;
		if (sscanf(token, "%d-%d", &start, &end) == 2) {
			for (int i = start; i <= end && count < max_cpus; i++) {
				cpus[count++] = i;
			}
		} else if (sscanf(token, "%d", &start) == 1) {
			cpus[count++] = start;
		}
		token = strtok(NULL, ",");
	}
	return count;
}

void numa_init_topology(void)
{
	g_total_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	g_num_nodes = 0;

	for (int node = 0; node < MAX_NUMA_NODES; node++) {
		char path[256];
		snprintf(path, sizeof(path),
			 "/sys/devices/system/node/node%d/cpulist", node);
		FILE *f = fopen(path, "r");
		if (!f)
			break;

		char line[256];
		if (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = '\0';
			g_nodes[g_num_nodes].node_id = node;
			g_nodes[g_num_nodes].num_cpus =
			    parse_cpulist(line, g_nodes[g_num_nodes].cpus,
					  MAX_CPUS_PER_NODE);
			g_num_nodes++;
		}
		fclose(f);
	}

	if (g_num_nodes == 0) {
		g_num_nodes = 1;
		g_nodes[0].node_id = 0;
		g_nodes[0].num_cpus = g_total_cpus;
		for (int i = 0; i < g_total_cpus; i++) {
			g_nodes[0].cpus[i] = i;
		}
	}
}

int numa_get_num_nodes(void)
{
	return g_num_nodes;
}

const numa_node_info_t *numa_get_node(int idx)
{
	if (idx < 0 || idx >= g_num_nodes)
		return NULL;
	return &g_nodes[idx];
}

const numa_node_info_t *numa_get_node_for_cpu(int cpu)
{
	for (int n = 0; n < g_num_nodes; n++) {
		for (int c = 0; c < g_nodes[n].num_cpus; c++) {
			if (g_nodes[n].cpus[c] == cpu) {
				return &g_nodes[n];
			}
		}
	}
	return NULL;
}

int numa_get_total_cpus(void)
{
	return g_total_cpus;
}

int numa_pin_cpu(int cpu)
{
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(cpu, &cpuset);
	return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t),
				      &cpuset);
}

static hugepage_type_t g_hugepage_mode = HUGEPAGE_NONE;

void numa_set_hugepage_mode(hugepage_type_t mode)
{
	g_hugepage_mode = mode;
}

hugepage_type_t numa_get_hugepage_mode(void)
{
	return g_hugepage_mode;
}

void *alloc_mem_onnode(size_t size, int node_id, hugepage_type_t hugepage)
{
	if (hugepage == HUGEPAGE_NONE && g_hugepage_mode != HUGEPAGE_NONE) {
		hugepage = g_hugepage_mode;
	}

	int flags = MAP_PRIVATE | MAP_ANONYMOUS;
	if (hugepage == HUGEPAGE_2MB) {
#ifdef MAP_HUGETLB
		flags |= MAP_HUGETLB;
#ifdef MAP_HUGE_2MB
		flags |= MAP_HUGE_2MB;
#endif
#endif
	} else if (hugepage == HUGEPAGE_1GB) {
#ifdef MAP_HUGETLB
		flags |= MAP_HUGETLB;
#ifdef MAP_HUGE_1GB
		flags |= MAP_HUGE_1GB;
#endif
#endif
	}

	void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, -1, 0);
	if (ptr == MAP_FAILED && hugepage != HUGEPAGE_NONE) {
		/* Fall back to anonymous pages with MADV_HUGEPAGE for transparent huge pages */
		flags &= ~MAP_HUGETLB;
		ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, -1, 0);
		if (ptr != MAP_FAILED) {
#ifdef MADV_HUGEPAGE
			madvise(ptr, size, MADV_HUGEPAGE);
#endif
		}
	}

	if (ptr == MAP_FAILED) {
		return NULL;
	}

	if (node_id >= 0 && node_id < MAX_NUMA_NODES) {
		unsigned long nodemask[16] = { 0 };
		nodemask[node_id / 64] |= (1ULL << (node_id % 64));
		syscall(SYS_mbind, ptr, size, MPOL_BIND, nodemask,
			MAX_NUMA_NODES + 1, MPOL_MF_STRICT);
	}
	// Touch all pages to pre-fault them onto target NUMA node
	volatile uint8_t *p = (volatile uint8_t *)ptr;
	for (size_t offset = 0; offset < size; offset += 4096) {
		p[offset] = 0x5A;
	}

	return ptr;
}

void free_mem_onnode(void *ptr, size_t size)
{
	if (ptr && size > 0) {
		munmap(ptr, size);
	}
}
