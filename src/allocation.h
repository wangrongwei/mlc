#ifndef MLC_ALLOCATION_H
#define MLC_ALLOCATION_H

#include <stddef.h>
#include <stdbool.h>

#define MAX_NUMA_NODES 64
#define MAX_NUMA_NODE_ID 1024
#define MAX_SOCKETS 64
#define MAX_CPUS 1024
#define MAX_CPUS_PER_NODE 512
#define MAX_L3_CLUSTERS 256

typedef struct {
	int smt_id;	/* Thread index within physical core (0 = primary) */
	int core_id;	/* Physical core ID within package */
	int socket_id;	/* Physical package / socket ID */
	int os_id;	/* Compact online CPU index (0 .. total_cpus - 1) */
	int hwid;	/* Linux kernel CPU ID (/sys/devices/system/cpu/cpuN) */
	int numa_id;	/* Real Linux NUMA node ID (-1 if none) */
	int core_type;	/* Hybrid core type from CPUID 0x1A (0 if uniform) */
} cpu_topology_entry_t;

typedef struct {
	int socket_id;
	int num_cpus;
	int cpus[MAX_CPUS_PER_NODE];
	int num_one_tpc_cpus;
	int one_tpc_cpus[MAX_CPUS_PER_NODE];
	int reserved_cpus[2];	/* [0] = busythread core, [1] = measurement/alloc core */
} socket_info_t;

typedef struct {
	int node_id;		/* Real Linux NUMA node ID (may be sparse, e.g., 0, 4) */
	int socket_id;		/* Package ID of CPUs on this node (-1 if CPU-less) */
	bool has_memory;	/* True if node has usable memory (MemFree > 0) */
	size_t mem_total_kb;
	size_t mem_free_kb;
	int num_cpus;
	int cpus[MAX_CPUS_PER_NODE];
	int num_one_tpc_cpus;
	int one_tpc_cpus[MAX_CPUS_PER_NODE];
	int reserved_cpus[2];
} numa_node_info_t;

void numa_init_topology(void);
void numa_init_topology_ex(bool count_only_online);
void numa_print_cpuid_topology(bool include_numa);

bool numa_is_numa_mode(void);
bool numa_has_insufficient_cores_for_busy(void);

int numa_get_num_nodes(void);
const numa_node_info_t *numa_get_node(int idx);
const numa_node_info_t *numa_get_node_by_id(int node_id);
const numa_node_info_t *numa_get_node_for_cpu(int cpu);

int numa_get_num_sockets(void);
const socket_info_t *numa_get_socket(int idx);
const socket_info_t *numa_get_socket_by_id(int socket_id);

int numa_get_total_cpus(void);
const cpu_topology_entry_t *cpu_get_topology(int cpu);
int cpu_get_hwid(int cpu);
int cpu_get_socket_id(int cpu);
bool cpu_is_smt_primary(int cpu);
bool cpu_is_same_core(int cpu_a, int cpu_b);

typedef struct {
	int l1i_type;
	size_t l1i_bytes;
	int l1d_type;
	size_t l1d_bytes;
	int l2_type;
	size_t l2_bytes;
	int l3_type;
	size_t l3_bytes;
} cpu_cache_info_t;

const cpu_cache_info_t *cpu_get_cache_info(void);
bool cpu_is_amd_processor(void);
bool cpu_is_xeon_atom_core(void);

int numa_pin_cpu(int cpu);

typedef enum {
	HUGEPAGE_NONE = 0,
	HUGEPAGE_2MB,
	HUGEPAGE_1GB
} hugepage_type_t;

void numa_set_hugepage_mode(hugepage_type_t mode);
hugepage_type_t numa_get_hugepage_mode(void);

typedef struct {
	int cluster_id;		/* Compact L3 cluster index (0 .. num_l3_clusters - 1) */
	int numa_id;		/* Owning Linux NUMA node ID */
	int socket_id;		/* Physical package / socket ID */
	int cache_id;		/* Linux sysfs cache/index3/id (-1 if fallback) */
	int num_cpus;
	int cpus[MAX_CPUS_PER_NODE];
} l3_cluster_info_t;

int numa_get_num_l3_clusters(void);
const l3_cluster_info_t *numa_get_l3_cluster(int idx);
int numa_find_l3_cluster_for_cpu(int cpu);
int numa_copy_l3_cluster_cpus(int idx, int *out_cpus, int max_cpus);
int numa_sample_l3_domains(bool full_l3_matrix);
void numa_reset_l3_sampling(void);
int numa_get_effective_l3_count(void);
int numa_get_effective_l3_cluster_id(int eff_idx);
void numa_print_l3_sampling_notice(void);

size_t numa_round_alloc_size(size_t size);
void *alloc_mem_onnode(size_t size, int node_id, hugepage_type_t hugepage);
void free_mem_onnode(void *ptr, size_t size);

#endif /* MLC_ALLOCATION_H */
