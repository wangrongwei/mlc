#ifndef MLC_ALLOCATION_H
#define MLC_ALLOCATION_H

#include <stddef.h>
#include <stdbool.h>

#define MAX_NUMA_NODES 64
#define MAX_CPUS_PER_NODE 256

typedef struct {
	int node_id;
	int num_cpus;
	int cpus[MAX_CPUS_PER_NODE];
} numa_node_info_t;

void numa_init_topology(void);
int numa_get_num_nodes(void);
const numa_node_info_t *numa_get_node(int idx);
const numa_node_info_t *numa_get_node_for_cpu(int cpu);
int numa_get_total_cpus(void);
int numa_pin_cpu(int cpu);

typedef enum {
	HUGEPAGE_NONE = 0,
	HUGEPAGE_2MB,
	HUGEPAGE_1GB
} hugepage_type_t;

void numa_set_hugepage_mode(hugepage_type_t mode);
hugepage_type_t numa_get_hugepage_mode(void);

void *alloc_mem_onnode(size_t size, int node_id, hugepage_type_t hugepage);
void free_mem_onnode(void *ptr, size_t size);

#endif /* MLC_ALLOCATION_H */
