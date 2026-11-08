#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#include <immintrin.h>
#include "hittest.h"
#include "allocation.h"

#define C2C_CACHE_LINE_SIZE 64
#define C2C_LOOP_KB 128
#define C2C_ROUNDS 30

struct c2c_line {
	struct c2c_line *next;
	char pad[C2C_CACHE_LINE_SIZE - sizeof(struct c2c_line *)];
} __attribute__((aligned(C2C_CACHE_LINE_SIZE)));

typedef struct {
	int writer_cpu;
	int reader_cpu;
	int alloc_node;
	bool is_hitm;
	double tsc_ghz;
	struct c2c_line *buf;
	size_t num_lines;
	volatile int sync_turn;	// 1: writer, 2: reader, 3: exit
} c2c_worker_ctx_t;

static void *c2c_writer_worker(void *arg)
{
	c2c_worker_ctx_t *ctx = (c2c_worker_ctx_t *) arg;
	numa_pin_cpu(ctx->writer_cpu);

	while (ctx->sync_turn != 3) {
		while (ctx->sync_turn != 1 && ctx->sync_turn != 3) {
			_mm_pause();
		}
		if (ctx->sync_turn == 3)
			break;

		volatile struct c2c_line *curr = ctx->buf;
		if (ctx->is_hitm) {
			// Write to modify line in Writer's L1/L2 cache (transition to Modified state)
			for (size_t i = 0; i < ctx->num_lines; i++) {
				curr->pad[0] = (char)i;
				curr = curr->next;
			}
		} else {
			// Read line without writing (transition to Shared/Clean state)
			for (size_t i = 0; i < ctx->num_lines; i++) {
				curr = curr->next;
			}
		}
		_mm_sfence();
		ctx->sync_turn = 2;	// Signal reader
	}
	return NULL;
}

static double measure_c2c_pair(int writer_cpu, int reader_cpu, int alloc_node,
			       bool is_hitm, double tsc_ghz)
{
	c2c_worker_ctx_t ctx;
	ctx.writer_cpu = writer_cpu;
	ctx.reader_cpu = reader_cpu;
	ctx.alloc_node = alloc_node;
	ctx.is_hitm = is_hitm;
	ctx.tsc_ghz = tsc_ghz;
	ctx.sync_turn = 0;

	size_t num_lines = (C2C_LOOP_KB * 1024) / C2C_CACHE_LINE_SIZE;
	size_t total_bytes = num_lines * sizeof(struct c2c_line);
	ctx.num_lines = num_lines;
	ctx.buf =
	    (struct c2c_line *)alloc_mem_onnode(total_bytes, alloc_node, false);
	if (!ctx.buf)
		return 0.0;

	// Link circular pointer-chasing list
	for (size_t i = 0; i < num_lines - 1; i++) {
		ctx.buf[i].next = &ctx.buf[i + 1];
	}
	ctx.buf[num_lines - 1].next = &ctx.buf[0];

	pthread_t writer_tid;
	pthread_create(&writer_tid, NULL, c2c_writer_worker, &ctx);

	numa_pin_cpu(reader_cpu);

	// Warm-up round
	ctx.sync_turn = 1;
	while (ctx.sync_turn != 2)
		_mm_pause();
	volatile struct c2c_line *curr = ctx.buf;
	for (size_t i = 0; i < num_lines; i++)
		curr = curr->next;

	uint64_t total_cycles = 0;
	uint64_t total_lines_read = 0;

	for (int r = 0; r < C2C_ROUNDS; r++) {
		ctx.sync_turn = 1;
		while (ctx.sync_turn != 2)
			_mm_pause();

		_mm_lfence();
		uint64_t t0 = __rdtsc();
		_mm_lfence();

		for (size_t i = 0; i < num_lines; i++) {
			curr = curr->next;
		}

		_mm_lfence();
		uint64_t t1 = __rdtsc();
		_mm_lfence();

		total_cycles += (t1 - t0);
		total_lines_read += num_lines;
	}

	ctx.sync_turn = 3;
	pthread_join(writer_tid, NULL);
	free_mem_onnode(ctx.buf, total_bytes);

	if (total_lines_read == 0)
		return 0.0;
	double cycles_per_line = (double)total_cycles / total_lines_read;
	return cycles_per_line / tsc_ghz;
}

void run_c2c_latency_benchmark(double tsc_freq_ghz)
{
	printf("Measuring cache-to-cache transfer latency (in ns)...\n");

	int num_nodes = numa_get_num_nodes();
	const numa_node_info_t *node0 = numa_get_node(0);
	if (!node0 || node0->num_cpus < 2) {
		printf
		    ("Error: Node 0 requires at least 2 CPUs for C2C measurement.\n");
		return;
	}

	int local_cpu_w = node0->cpus[0];
	int local_cpu_r = node0->cpus[1];	// Different core on same socket

	// Local Socket L2->L2 HIT and HITM
	double hit_lat =
	    measure_c2c_pair(local_cpu_w, local_cpu_r, 0, false, tsc_freq_ghz);
	double hitm_lat =
	    measure_c2c_pair(local_cpu_w, local_cpu_r, 0, true, tsc_freq_ghz);

	printf("Local Socket L2->L2 HIT  latency\t%.1f\n", hit_lat);
	printf("Local Socket L2->L2 HITM latency\t%.1f\n", hitm_lat);

	if (num_nodes < 2)
		return;

	// Remote Socket L2->L2 HITM (homed in writer socket)
	printf
	    ("Remote Socket L2->L2 HITM latency (data address homed in writer socket)\n");
	printf("                        Reader Numa Node\nWriter Numa Node");
	for (int j = 0; j < num_nodes; j++) {
		printf("%8d", j);
	}
	printf("  \n");

	for (int w = 0; w < num_nodes; w++) {
		printf("%12d        ", w);
		const numa_node_info_t *wnode = numa_get_node(w);
		int w_cpu = wnode ? wnode->cpus[0] : 0;

		for (int r = 0; r < num_nodes; r++) {
			if (w == r) {
				printf("     -  ");
			} else {
				const numa_node_info_t *rnode =
				    numa_get_node(r);
				int r_cpu = rnode ? rnode->cpus[0] : 0;
				double rem_lat =
				    measure_c2c_pair(w_cpu, r_cpu, w, true,
						     tsc_freq_ghz);
				printf("%8.1f", rem_lat);
			}
		}
		printf("  \n");
	}

	// Remote Socket L2->L2 HITM (homed in reader socket)
	printf
	    ("Remote Socket L2->L2 HITM latency (data address homed in reader socket)\n");
	printf("                        Reader Numa Node\nWriter Numa Node");
	for (int j = 0; j < num_nodes; j++) {
		printf("%8d", j);
	}
	printf("  \n");

	for (int w = 0; w < num_nodes; w++) {
		printf("%12d        ", w);
		const numa_node_info_t *wnode = numa_get_node(w);
		int w_cpu = wnode ? wnode->cpus[0] : 0;

		for (int r = 0; r < num_nodes; r++) {
			if (w == r) {
				printf("     -  ");
			} else {
				const numa_node_info_t *rnode =
				    numa_get_node(r);
				int r_cpu = rnode ? rnode->cpus[0] : 0;
				double rem_lat =
				    measure_c2c_pair(w_cpu, r_cpu, r, true,
						     tsc_freq_ghz);
				printf("%8.1f", rem_lat);
			}
		}
		printf("  \n");
	}
}

#include "c2c_bandwidth.h"

void run_c2c_bandwidth_benchmark(double duration_sec)
{
	run_c2c_bandwidth_matrix(duration_sec, 0, false);
}
