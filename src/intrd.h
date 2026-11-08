#ifndef MLC_INTRD_H
#define MLC_INTRD_H

#include <stddef.h>
#include <stdbool.h>

#define CACHE_LINE_SIZE 64

struct cache_line {
	struct cache_line *next;
	char padding[CACHE_LINE_SIZE - sizeof(struct cache_line *)];
} __attribute__((aligned(CACHE_LINE_SIZE)));

void init_pointer_chase_chain(struct cache_line *buf, size_t size_bytes,
			      bool random_access);
double measure_latency_on_node(int cpu, int alloc_node, size_t buf_size_mb,
			       bool random_access);
double measure_latency(int cpu, size_t buf_size_mb, bool random_access);

void run_idle_latency(size_t buf_mb, bool random_access, bool keep_prefetchers);
void run_idle_latency_core(int cpu, size_t buf_mb, bool random_access,
			   bool keep_prefetchers);
void run_idle_latency_all_cores(size_t buf_mb, bool random_access,
				bool single_thread_per_core,
				bool keep_prefetchers);
void run_latency_matrix(size_t buf_mb, bool random_access,
			bool keep_prefetchers);

#endif /* MLC_INTRD_H */
