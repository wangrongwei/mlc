#ifndef MLC_COMBINED_LOADLAT_H
#define MLC_COMBINED_LOADLAT_H

#include <stddef.h>
#include <stdbool.h>

typedef enum {
	TRAFFIC_ALL_READS = 1,		/* -R / default: 100% reads (W1) */
	TRAFFIC_2_1_READ_WRITE = 2,	/* -W2: 2:1 read-write ratio (1R + 1W with RFO) */
	TRAFFIC_3_1_READ_WRITE = 3,	/* -W3: 3:1 read-write ratio (2R + 1W with RFO) */
	TRAFFIC_3_2_READ_WRITE = 4,	/* -W4: 3:2 read-write ratio (1R + 2W with RFO) */
	TRAFFIC_1_1_READ_WRITE = 5,	/* -W5: 1:1 read-write ratio (0R + 1W with RFO) */
	TRAFFIC_0_1_NT_WRITE = 6,	/* -W6: 0:1 read-NT write (100% NT write) */
	TRAFFIC_2_1_NT_WRITE = 7,	/* -W7: 2:1 read-NT write (2R + 1NTW) */
	TRAFFIC_1_1_NT_WRITE = 8,	/* -W8: 1:1 read-NT write (1R + 1NTW) */
	TRAFFIC_3_1_NT_WRITE = 9,	/* -W9: 3:1 read-NT write (3R + 1NTW, SSE2 only) */
	TRAFFIC_STREAM_TRIAD = 10,	/* -W10: 2:1 read-NT write (2 read buffers + 1 NT write) */
	TRAFFIC_STREAM_TRIAD_RFO = 11,	/* -W11: 3:1 read-write (2 read buffers + 1 write with RFO) */
	TRAFFIC_4_1_READ_WRITE = 12,	/* -W12: 4:1 read-write ratio (3R + 1W with RFO) */
	TRAFFIC_9_1_READ_WRITE = 19,	/* -W19: 9:1 read-write ratio (8R + 1W with RFO) */
	TRAFFIC_DUAL_ALL_READS = 21,	/* -W21: 100% read with 2 address streams (-o) */
	TRAFFIC_DUAL_3_1_RW = 23,	/* -W23: 3:1 read-write with 2 address streams (-o) */
	TRAFFIC_DUAL_2_1_NTW = 27	/* -W27: 2:1 read-NT write with 2 address streams (-o) */
} traffic_type_t;

typedef enum {
	BW_SIMD_SSE2 = 0,	/* Default: 128-bit SSE2 movdqa / movntdq */
	BW_SIMD_AVX2 = 1,	/* -Y: 256-bit AVX2 vmovdqa / vmovntdq */
	BW_SIMD_AVX512 = 2	/* -Z: 512-bit AVX-512 vmovdqa64 / vmovntdq */
} bw_simd_width_t;

void bw_set_simd_options(bw_simd_width_t width, int z_burst);
void bw_set_stride(int stride_bytes);
void bw_set_single_delay(int delay_override);
int bw_load_per_thread_config(const char *path, int total_cpus, int num_nodes);
void bw_clear_per_thread_config(void);
bool bw_has_per_thread_config(void);
int bw_load_pattern_file(const char *path);
void bw_clear_pattern_file(void);
void bw_set_physaddr_dump(bool enabled);

typedef struct {
	size_t per_thread_kb;
	double duration_sec;
	bool single_thread_per_core;
	bool max_bandwidth_mode;
	bool verbose;
	const char *cpu_list;
	const char *cpu_mask;
	const char *pattern_file;
} peak_bw_options_t;

typedef struct {
	size_t per_thread_kb;
	double duration_sec;
	bool random_latency;
	bool keep_prefetchers;
	int target_core;
	bool single_thread_per_core;
	bool throughput_only;
	bool verbose;
	bool has_alloc_node;
	int alloc_node;
	traffic_type_t traffic;
	const char *cpu_list;
	const char *cpu_mask;
	const char *pattern_file;
} loaded_lat_options_t;

void run_bandwidth_matrix(size_t per_thread_mb, double duration_sec,
			  bool single_thread_per_core, bool verbose,
			  traffic_type_t traffic);
int run_peak_or_max_bandwidth(const peak_bw_options_t *opts);
void run_peak_bandwidth(size_t per_thread_mb, double duration_sec,
			bool single_thread_per_core);
int run_loaded_latency_ex(const loaded_lat_options_t *opts);
void run_loaded_latency(size_t per_thread_mb, double duration_sec,
			bool random_latency, bool keep_prefetchers,
			int target_core, bool single_thread_per_core,
			bool throughput_only, traffic_type_t traffic);
typedef struct {
	size_t cold_kb;
	size_t hot_kb;
	int hot_pct;
	double duration_sec;
	bool single_thread_per_core;
	bool shared_hot_buf;
	bool per_thread_bw;
	bool verbose;
	bool has_numa_node;
	int numa_node;
	const char *cpu_list;
	const char *cpu_mask;
} conc_bw_options_t;

int run_concurrent_bandwidth(const conc_bw_options_t *opts);

#endif /* MLC_COMBINED_LOADLAT_H */
