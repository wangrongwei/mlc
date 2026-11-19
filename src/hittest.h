#ifndef MLC_HITTEST_H
#define MLC_HITTEST_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
	bool summary_mode;	/* true = full Local HIT/HITM + Remote HITM matrix */
	bool has_reader_cpu;	/* -c specified */
	int reader_cpu;		/* -c <core> */
	bool has_writer_cpu;	/* -w specified */
	int writer_cpu;		/* -w <core> */
	int third_cpu;		/* -S <core>, -1 if disabled */
	bool has_init_cpu;	/* -i specified */
	int init_cpu;		/* -i <core>, default 0 */
	bool has_buf_size;	/* -b specified */
	size_t buf_size_kb;	/* -b <KB> */
	bool has_window_size;	/* -C specified */
	size_t window_size_bytes; /* -C <KB> * 1024 */
	bool has_writer_extra;	/* -E specified */
	size_t writer_extra_bytes; /* -E <KB> * 1024 */
	size_t reader_extra_bytes; /* -G <KB> * 1024 */
	bool clean_hit;		/* -H: clean transfer (HIT) instead of dirty (HITM) */
	bool reader_rfo;	/* -B: RFO write+chase in reader thread */
	bool do_clflush;	/* true by default, false when -F is specified */
	bool random_access;	/* -r */
	int random_range;	/* -D <N>, default 4096 */
	int stride;		/* -l <bytes>, default 64 */
	int64_t total_iters;	/* -x <M> * 1,000,000, default 20,000,000 */
	bool has_iters;		/* -x specified */
	bool verbose;		/* -v */
} c2c_latency_options_t;

void c2c_latency_init_defaults(c2c_latency_options_t *opts);
int run_c2c_latency_ex(const c2c_latency_options_t *opts, double tsc_freq_ghz);
void run_c2c_latency_benchmark(double tsc_freq_ghz);
void run_c2c_bandwidth_benchmark(double duration_sec);

#endif /* MLC_HITTEST_H */
