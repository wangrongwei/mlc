#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <immintrin.h>
#include "c2c_bandwidth.h"
#include "allocation.h"

#define DEFAULT_C2C_BUF_KB 12288
#define C2C_CACHE_LINE_SIZE 64

void c2c_bw_config_init(c2c_bw_config_t *cfg)
{
	if (!cfg)
		return;
	memset(cfg, 0, sizeof(*cfg));
	cfg->buffer_kb = DEFAULT_C2C_BUF_KB;
	cfg->chunk_kb = 0;
	cfg->duration_sec = 2.0;
	cfg->pipelined = false;
	cfg->no_chunking = true;
	cfg->unaligned = false;
	cfg->half_share = false;
	cfg->mode = C2C_MODE_MATRIX;
}

size_t c2c_parse_cpulist(const char *cpustr, int *out_cpus, size_t max_cpus)
{
	if (!cpustr || !out_cpus || max_cpus == 0)
		return 0;
	size_t count = 0;
	char buf[1024];
	strncpy(buf, cpustr, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	char *saveptr = NULL;
	char *token = strtok_r(buf, ",", &saveptr);
	while (token && count < max_cpus) {
		char *dash = strchr(token, '-');
		if (dash) {
			*dash = '\0';
			int start = atoi(token);
			int end = atoi(dash + 1);
			if (start <= end) {
				for (int c = start;
				     c <= end && count < max_cpus; c++) {
					out_cpus[count++] = c;
				}
			}
		} else {
			out_cpus[count++] = atoi(token);
		}
		token = strtok_r(NULL, ",", &saveptr);
	}
	return count;
}

typedef struct {
	uint8_t *buf;
	size_t size;
	const int *cpus;
	size_t num_cpus;
	volatile bool *stop_flag;
	volatile bool *writer_ready;
	volatile bool *reader_done;
} c2c_worker_arg_t;

static void *c2c_writer_thread_proc(void *arg)
{
	c2c_worker_arg_t *warg = (c2c_worker_arg_t *) arg;
	if (warg->num_cpus > 0) {
		numa_pin_cpu(warg->cpus[0]);
	}

	size_t count = warg->size / sizeof(__m256i);
	__m256i val = _mm256_set1_epi64x(0x5A5A5A5A5A5A5A5AULL);

	while (!(*warg->stop_flag)) {
		__m256i *p = (__m256i *) warg->buf;
		for (size_t i = 0; i < count; i += 4) {
			_mm256_store_si256(&p[i], val);
			_mm256_store_si256(&p[i + 1], val);
			_mm256_store_si256(&p[i + 2], val);
			_mm256_store_si256(&p[i + 3], val);
		}
		_mm_sfence();

		*warg->writer_ready = true;
		while (*warg->writer_ready && !(*warg->stop_flag)) {
			_mm_pause();
		}
	}
	return NULL;
}

static double measure_stepped_transfer(uint8_t *buf, size_t size,
				       const int *w_cpus, size_t nw,
				       const int *r_cpus, size_t nr,
				       double duration_sec)
{
	(void)nw;
	volatile bool stop_flag = false;
	volatile bool writer_ready = false;
	volatile bool reader_done = false;

	c2c_worker_arg_t warg = {
		.buf = buf,
		.size = size,
		.cpus = w_cpus,
		.num_cpus = nw,
		.stop_flag = &stop_flag,
		.writer_ready = &writer_ready,
		.reader_done = &reader_done
	};

	pthread_t wthread;
	if (pthread_create(&wthread, NULL, c2c_writer_thread_proc, &warg) != 0) {
		return 0.0;
	}

	if (nr > 0) {
		numa_pin_cpu(r_cpus[0]);
	}

	size_t count = size / sizeof(__m256i);
	__m256i acc = _mm256_setzero_si256();

	struct timespec t_start, t_now;
	clock_gettime(CLOCK_MONOTONIC_RAW, &t_start);

	size_t total_bytes = 0;
	double reader_time = 0.0;

	while (true) {
		while (!writer_ready && !stop_flag) {
			_mm_pause();
		}
		if (stop_flag)
			break;

		struct timespec rt0, rt1;
		clock_gettime(CLOCK_MONOTONIC_RAW, &rt0);

		__m256i *p = (__m256i *) buf;
		for (size_t i = 0; i < count; i += 4) {
			acc = _mm256_add_epi64(acc, _mm256_load_si256(&p[i]));
			acc =
			    _mm256_add_epi64(acc, _mm256_load_si256(&p[i + 1]));
			acc =
			    _mm256_add_epi64(acc, _mm256_load_si256(&p[i + 2]));
			acc =
			    _mm256_add_epi64(acc, _mm256_load_si256(&p[i + 3]));
		}

		clock_gettime(CLOCK_MONOTONIC_RAW, &rt1);
		double rd_el =
		    (rt1.tv_sec - rt0.tv_sec) + (rt1.tv_nsec -
						 rt0.tv_nsec) * 1e-9;
		reader_time += rd_el;
		total_bytes += size;

		writer_ready = false;

		clock_gettime(CLOCK_MONOTONIC_RAW, &t_now);
		double total_el =
		    (t_now.tv_sec - t_start.tv_sec) + (t_now.tv_nsec -
						       t_start.tv_nsec) * 1e-9;
		if (total_el >= duration_sec)
			break;
	}

	stop_flag = true;
	writer_ready = false;
	pthread_join(wthread, NULL);

	int64_t dummy[4];
	_mm256_storeu_si256((__m256i *) dummy, acc);
	if (dummy[0] == 999999999LL)
		printf(" ");

	if (reader_time > 0.0) {
		return (double)total_bytes / (reader_time * 1e9);
	}
	return 0.0;
}

double run_c2c_explicit_benchmark(const c2c_bw_config_t *cfg)
{
	if (!cfg || cfg->num_writers == 0 || cfg->num_readers == 0) {
		fprintf(stderr,
			"c2c_bandwidth: need both --c2cbw_writer and --c2cbw_reader\n");
		return 0.0;
	}

	size_t size = cfg->buffer_kb * 1024;
	int alloc_node = 0;
	const numa_node_info_t *wnode = numa_get_node_for_cpu(cfg->writers[0]);
	if (wnode)
		alloc_node = wnode->node_id;

	uint8_t *buf =
	    (uint8_t *) alloc_mem_onnode(size, alloc_node, HUGEPAGE_NONE);
	if (!buf) {
		fprintf(stderr,
			"c2c_bandwidth: alloc %zu bytes on numa %d failed\n",
			size, alloc_node);
		return 0.0;
	}

	printf("Explicit groups: writer[%zu]={", cfg->num_writers);
	for (size_t i = 0; i < cfg->num_writers; i++) {
		printf("%s%d", (i == 0) ? "" : ",", cfg->writers[i]);
	}
	printf("} reader[%zu]={", cfg->num_readers);
	for (size_t i = 0; i < cfg->num_readers; i++) {
		printf("%s%d", (i == 0) ? "" : ",", cfg->readers[i]);
	}
	printf("}");
	if (cfg->num_sharers > 0) {
		printf(" sharer[%zu]={", cfg->num_sharers);
		for (size_t i = 0; i < cfg->num_sharers; i++) {
			printf("%s%d", (i == 0) ? "" : ",", cfg->sharers[i]);
		}
		printf("}");
	}
	printf("\n");

	double bw = measure_stepped_transfer(buf, size,
					     cfg->writers, cfg->num_writers,
					     cfg->readers, cfg->num_readers,
					     cfg->duration_sec);

	printf("BW = %.2f GB/s\n", bw);
	free_mem_onnode(buf, size);
	return bw;
}

void run_c2c_bandwidth_matrix(double duration_sec, size_t buffer_kb,
			      bool pipelined)
{
	int num_nodes = numa_get_num_nodes();
	if (num_nodes <= 0)
		return;

	if (buffer_kb == 0)
		buffer_kb = DEFAULT_C2C_BUF_KB;
	size_t size = buffer_kb * 1024;

	printf("=== c2c_readerbw (reader-focused, minimal-sync, %s) ===\n",
	       pipelined ? "pipelined" : "no chunking");
	printf("  L3 domains:       %d\n", num_nodes);
	printf("  total buffer:     %zu KB (%.2f MiB)\n", buffer_kb,
	       (double)buffer_kb / 1024.0);
	printf("  writer mode:      WRITE (Modified install)\n");
	printf("  reader kernel:    AVX2 load\n");
	printf("  reader R/W mix:   100%% reads (default)\n");
	printf("  time per pair:    %.0f sec\n", duration_sec);
	printf
	    ("  timing:           reader work cycles only (writer staging excluded)\n\n");

	printf("              Reader L3 domain\nWriter        ");
	for (int j = 0; j < num_nodes; j++) {
		printf("  NUMA%d:LLC%d", j, j);
	}
	printf("\n");

	for (int w = 0; w < num_nodes; w++) {
		printf("NUMA%d:LLC%d  ", w, w);
		for (int r = 0; r < num_nodes; r++) {
			if (w == r) {
				printf("       -      ");
				continue;
			}

			const numa_node_info_t *wnode = numa_get_node(w);
			const numa_node_info_t *rnode = numa_get_node(r);
			if (!wnode || !rnode || wnode->num_cpus == 0
			    || rnode->num_cpus == 0) {
				printf("       -      ");
				continue;
			}

			uint8_t *buf =
			    (uint8_t *) alloc_mem_onnode(size, w,
							 HUGEPAGE_NONE);
			if (!buf) {
				printf("     N/A      ");
				continue;
			}

			int w_cpus[1] = { wnode->cpus[0] };
			int r_cpus[1] = { rnode->cpus[0] };
			double gb_sec =
			    measure_stepped_transfer(buf, size, w_cpus, 1,
						     r_cpus, 1, duration_sec);
			printf("%12.1f  ", gb_sec);
			fflush(stdout);

			free_mem_onnode(buf, size);
		}
		printf("\n");
	}
	printf
	    ("\n(units: GB/s; rows = writer L3 domain, cols = reader L3 domain)\n");
}

int c2c_bandwidth_main(int argc, char **argv, double default_duration)
{
	c2c_bw_config_t cfg;
	c2c_bw_config_init(&cfg);
	cfg.duration_sec = default_duration;

	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--c2cbw_writer=", 15) == 0) {
			cfg.num_writers =
			    c2c_parse_cpulist(argv[i] + 15, cfg.writers,
					      MAX_C2C_CPUS);
			cfg.mode = C2C_MODE_EXPLICIT_PAIR;
		} else if (strncmp(argv[i], "--c2cbw_reader=", 15) == 0) {
			cfg.num_readers =
			    c2c_parse_cpulist(argv[i] + 15, cfg.readers,
					      MAX_C2C_CPUS);
			cfg.mode = C2C_MODE_EXPLICIT_PAIR;
		} else if (strncmp(argv[i], "--c2cbw_sharer=", 15) == 0) {
			cfg.num_sharers =
			    c2c_parse_cpulist(argv[i] + 15, cfg.sharers,
					      MAX_C2C_CPUS);
		} else if (strcmp(argv[i], "--c2cbw_matrix") == 0) {
			cfg.mode = C2C_MODE_MATRIX;
		} else if (strcmp(argv[i], "-p") == 0
			   || strcmp(argv[i], "--pipelined") == 0) {
			cfg.pipelined = true;
		} else if (strcmp(argv[i], "-u") == 0) {
			cfg.unaligned = true;
		} else if (strcmp(argv[i], "-H") == 0) {
			cfg.half_share = true;
		}
	}

	if (cfg.mode == C2C_MODE_EXPLICIT_PAIR) {
		if (cfg.num_writers == 0 || cfg.num_readers == 0) {
			fprintf(stderr,
				"c2c_bandwidth: need both --c2cbw_writer and --c2cbw_reader\n");
			return 1;
		}
		run_c2c_explicit_benchmark(&cfg);
	} else {
		run_c2c_bandwidth_matrix(cfg.duration_sec, cfg.buffer_kb,
					 cfg.pipelined);
	}
	return 0;
}
