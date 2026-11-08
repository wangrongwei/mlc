#ifndef C2C_BANDWIDTH_H
#define C2C_BANDWIDTH_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_C2C_CPUS 1024

typedef enum {
	C2C_MODE_MATRIX = 0,
	C2C_MODE_EXPLICIT_PAIR,
	C2C_MODE_MULTI_GROUP
} c2c_bw_mode_t;

typedef struct {
	int writers[MAX_C2C_CPUS];
	size_t num_writers;
	int readers[MAX_C2C_CPUS];
	size_t num_readers;
	int sharers[MAX_C2C_CPUS];
	size_t num_sharers;

	size_t buffer_kb;
	size_t chunk_kb;
	double duration_sec;
	bool pipelined;
	bool no_chunking;
	bool unaligned;
	bool half_share;
	c2c_bw_mode_t mode;
} c2c_bw_config_t;

/* Initialize default C2C bandwidth config */
void c2c_bw_config_init(c2c_bw_config_t * cfg);

/* Parse CPU list string (e.g. "0,2,4-7") into array */
size_t c2c_parse_cpulist(const char *cpustr, int *out_cpus, size_t max_cpus);

/* Run C2C bandwidth matrix (all L3 / NUMA domain pairs) */
void run_c2c_bandwidth_matrix(double duration_sec, size_t buffer_kb,
			      bool pipelined);

/* Run explicit writer/reader/sharer C2C bandwidth benchmark */
double run_c2c_explicit_benchmark(const c2c_bw_config_t * cfg);

/* Master dispatcher matching Intel MLC CLI options */
int c2c_bandwidth_main(int argc, char **argv, double default_duration);

#endif /* C2C_BANDWIDTH_H */
