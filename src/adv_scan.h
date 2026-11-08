#ifndef ADV_SCAN_H
#define ADV_SCAN_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define DEFAULT_GUARDBAND_KB (10 * 1024 * 1024ULL)	// 10GB Guardband
#define DEFAULT_HISTOGRAM_BUCKETS 64

typedef struct {
	int numa_node;
	int num_threads;
	size_t scan_gb;
	int bucket_granularity_ns;
	int iterations;
	bool use_1gb_pages;
	bool verbose;
} memory_scan_config_t;

/* Translate virtual address to physical address via /proc/self/pagemap */
uint64_t pagemap_get_phys_addr(void *vaddr);

/* Query and reserve 1GB hugepages dynamically on target NUMA node */
int reserve_1gb_hugepages(int numa_node, int pages_needed);

/* Restore 1GB hugepage reservations cleanly on exit or signal */
void restore_1gb_hugepages(void);

/* Fine-grained latency histogram distribution generator */
void run_latency_histogram(size_t buf_size_mb, int bucket_granularity_ns);

/* Memory bandwidth physical scan across NUMA node */
int run_memory_bw_scan(const memory_scan_config_t * cfg);

#endif /* ADV_SCAN_H */
