#ifndef C2C_BANDWIDTH_H
#define C2C_BANDWIDTH_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_C2C_CPUS 1024
#define MAX_C2C_GROUPS 16

typedef enum {
	C2C_ENGINE_READERBW = 0,	/* --c2c_bandwidth / --c2c_readerbw / --c2c_reader_bw */
	C2C_ENGINE_MATRIX = 1,		/* --c2c_bandwidth_matrix / --c2c_readerbw_matrix / default run */
	C2C_ENGINE_PIPELINED = 2	/* --c2c_bandwidth_pipelined */
} c2c_engine_t;

typedef struct {
	c2c_engine_t engine;
	const char *writer_str;		/* --c2cbw_writer[=]cpulist */
	const char *reader_str;		/* --c2cbw_reader[=]cpulist */
	const char *sharer_str;		/* --c2cbw_sharer[=]cpulist */
	bool has_b;
	int64_t buf_kb;			/* -b<KB> */
	bool has_C;
	int64_t chunk_kb;		/* -C<KB> */
	int time_sec;			/* -t<sec> (default 3) */
	int simd_level;			/* 2 = AVX-512, 1 = AVX2, 0 = SSE2/scalar */
	int rw_mix;			/* 0 = 100% reads, 2 = -W2, 3 = -W3, 5 = -W5, 6 = -W6 */
	bool hit_clean;			/* -H */
	bool shared_buf;		/* -u */
	bool verbose;			/* --c2cbw_verbose or -v */
	int delay;			/* --c2cbw_delay[=]N */
	bool stepped;			/* --c2cbw_stepped / --c2cbw_steppedmode */
	bool partial;			/* --c2cbw_partial */
	int ngroups;			/* --c2cbw_ngroups[=]N (default 1) */
	int64_t skip_kb;		/* --c2cbw_skip[=]KB (-1 = unset -> chunk_kb) */
	bool auto_sharer;		/* --c2cbw_auto_sharer */
} c2c_bw_options_t;

void c2c_bw_options_init(c2c_bw_options_t *opts);
size_t c2c_parse_cpulist(const char *cpustr, int *out_cpus, size_t max_cpus);

int run_c2c_readerbw(const c2c_bw_options_t *opts);
int run_c2c_bandwidth_matrix_mode(const c2c_bw_options_t *opts);
int run_c2c_bandwidth_pipelined(const c2c_bw_options_t *opts);
int run_c2c_bandwidth_dispatch(const c2c_bw_options_t *opts);
void run_c2c_bandwidth_matrix(double duration_sec, size_t buffer_kb,
			      bool pipelined);

#endif /* C2C_BANDWIDTH_H */
