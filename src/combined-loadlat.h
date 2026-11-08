#ifndef MLC_COMBINED_LOADLAT_H
#define MLC_COMBINED_LOADLAT_H

#include <stddef.h>
#include <stdbool.h>

typedef enum {
	TRAFFIC_ALL_READS = 0,	/* -W1: 100% reads */
	TRAFFIC_2_1_READ_WRITE,	/* -W2: 2:1 read-write ratio */
	TRAFFIC_3_1_READ_WRITE,	/* -W3: 3:1 read-write ratio */
	TRAFFIC_3_2_READ_WRITE,	/* -W4: 3:2 read-write ratio */
	TRAFFIC_1_1_READ_WRITE,	/* -W5: 1:1 read-write ratio */
	TRAFFIC_0_1_NT_WRITE,	/* -W6: 0:1 read-NT write (100% NT write) */
	TRAFFIC_2_1_NT_WRITE,	/* -W7: 2:1 read-NT write */
	TRAFFIC_1_1_NT_WRITE,	/* -W8: 1:1 read-NT write */
	TRAFFIC_3_1_NT_WRITE,	/* -W9: 3:1 read-NT write */
	TRAFFIC_STREAM_TRIAD,	/* -W10: 2:1 read-NT write (stream triad-like, 2 read buffers) */
	TRAFFIC_STREAM_TRIAD_RFO,	/* -W11: 3:1 read-write (stream triad-like with RFO) */
	TRAFFIC_4_1_READ_WRITE	/* -W12: 4:1 read-write ratio */
} traffic_type_t;

void run_bandwidth_matrix(size_t per_thread_mb, double duration_sec);
void run_peak_bandwidth(size_t per_thread_mb, double duration_sec);
void run_loaded_latency(size_t per_thread_mb, double duration_sec,
			bool random_latency, bool keep_prefetchers);
void run_concurrent_bandwidth(size_t cold_kb, size_t hot_kb, int hot_pct,
			      double duration_sec);

#endif /* MLC_COMBINED_LOADLAT_H */
