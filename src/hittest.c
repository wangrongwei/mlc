#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <x86intrin.h>
#include "hittest.h"
#include "allocation.h"
#include "c2c_bandwidth.h"

#define C2C_DEFAULT_ITERS 20000000LL
#define C2C_DEFAULT_RAND_RANGE 4096
#define C2C_DEFAULT_STRIDE 64

typedef enum {
	C2C_PRINT_MATRIX_CELL = 0,	/* "%6.1f\t" */
	C2C_PRINT_SUMMARY_LINE = 1,	/* "\t%.1f\n" */
	C2C_PRINT_SILENT = 2,		/* Sharer thread (-S): no output */
	C2C_PRINT_DIRECTED = 3		/* "\nLatency = %.1f base frequency clocks (%.1f ns)\n" */
} c2c_print_mode_t;

/*
 * Serial read pointer-chase microkernel (matches intel/mlc 0x1884f):
 * Chases `count` dependent pointers `mov (%p), %p` and returns the ending
 * pointer so sliding windows advance continuously across the multi-LLC buffer.
 */
static inline void *c2c_read_chain(void *ptr, uint64_t count)
{
	if (count == 0)
		return ptr;
	uint64_t idx = 0;
	__asm__ volatile (
		"1:\n\t"
		"mov (%[p]), %[p]\n\t"
		"inc %[i]\n\t"
		"cmp %[cnt], %[i]\n\t"
		"jb 1b\n\t"
		: [p] "+r" (ptr), [i] "+r" (idx)
		: [cnt] "r" (count)
		: "cc", "memory"
	);
	return ptr;
}

/*
 * Serial write+chase pointer-chase microkernel (matches intel/mlc 0x18928):
 * Modifies byte offset +0x20 of each cache line (`mov %p, 0x20(%p)`) to place
 * the line in Modified (M) state without corrupting the next-pointer at +0x0,
 * then chases `mov (%p), %p` for `count` steps and returns the ending pointer.
 * Falls back to +0x8 when stride < 40 bytes so sub-line strides remain safe.
 */
static inline void *c2c_write_chain(void *ptr, uint64_t count, size_t step_bytes)
{
	if (count == 0)
		return ptr;
	uint64_t idx = 0;
	if (step_bytes >= 40) {
		__asm__ volatile (
			"1:\n\t"
			"inc %[i]\n\t"
			"mov %[p], 0x20(%[p])\n\t"
			"mov (%[p]), %[p]\n\t"
			"cmp %[cnt], %[i]\n\t"
			"jb 1b\n\t"
			: [p] "+r" (ptr), [i] "+r" (idx)
			: [cnt] "r" (count)
			: "cc", "memory"
		);
	} else {
		__asm__ volatile (
			"1:\n\t"
			"inc %[i]\n\t"
			"mov %[p], 0x8(%[p])\n\t"
			"mov (%[p]), %[p]\n\t"
			"cmp %[cnt], %[i]\n\t"
			"jb 1b\n\t"
			: [p] "+r" (ptr), [i] "+r" (idx)
			: [cnt] "r" (count)
			: "cc", "memory"
		);
	}
	return ptr;
}

typedef struct {
	void *buf;
	size_t step_bytes;
	size_t window_lines;
	size_t num_windows;
	int64_t total_iters;
	bool clean_hit;
	bool reader_rfo;
	size_t writer_extra_bytes;
	double tsc_ghz;
	int writer_cpu;
	int writer_id;
	int writer_next_id;
	volatile int turn;
	volatile uint64_t evict_sink;
} c2c_shared_ctx_t;

typedef struct {
	c2c_shared_ctx_t *shared;
	int cpu;
	int my_id;
	int next_id;
	size_t extra_bytes;
	c2c_print_mode_t print_mode;
	double avg_clocks;
	double avg_ns;
	volatile uint64_t extra_sink;
} c2c_reader_arg_t;

static int get_alloc_node_for_cpu(int cpu)
{
	const numa_node_info_t *n = numa_get_node_for_cpu(cpu);
	if (n && n->has_memory)
		return n->node_id;
	return -1;
}

/*
 * Build a single-cycle block-random pointer chain matching intel/mlc
 * FUN_0011d0a0 + FUN_0011cd80:
 * Within each block of `D` lines (`D` = random_range, default 4096), visits all
 * `D` lines in a single random cycle whose final step advances into slot 0 of
 * the next `D`-line block, wrapping back to `buf` at the end of the last block.
 * Returns the exact number of lines linked into the cycle.
 */
static size_t init_c2c_pointer_chain(uint8_t *buf, size_t buf_bytes,
				     size_t step_bytes, bool random_access,
				     int random_range)
{
	size_t total_lines = buf_bytes / step_bytes;
	if (total_lines < 2) {
		*(void **)buf = buf;
		return 1;
	}

	if (!random_access) {
		for (size_t i = 0; i < total_lines - 1; i++) {
			*(void **)(buf + i * step_bytes) =
			    (void *)(buf + (i + 1) * step_bytes);
		}
		*(void **)(buf + (total_lines - 1) * step_bytes) = (void *)buf;
		return total_lines;
	}

	size_t d_lines = (random_range >= 2) ? (size_t)random_range
					     : (size_t)C2C_DEFAULT_RAND_RANGE;
	size_t num_blocks = total_lines / d_lines;
	if (num_blocks < 1) {
		d_lines = total_lines;
		num_blocks = 1;
	}

	size_t *order = (size_t *)malloc(d_lines * sizeof(size_t));
	size_t *perm = (size_t *)malloc(d_lines * sizeof(size_t));
	if (!order || !perm) {
		free(order);
		free(perm);
		for (size_t i = 0; i < total_lines - 1; i++) {
			*(void **)(buf + i * step_bytes) =
			    (void *)(buf + (i + 1) * step_bytes);
		}
		*(void **)(buf + (total_lines - 1) * step_bytes) = (void *)buf;
		return total_lines;
	}

	for (size_t i = 0; i < d_lines; i++)
		order[i] = i;

	/* Fisher-Yates shuffle on order[1 .. d_lines-1] keeping slot 0 as entry */
	unsigned int seed = ((unsigned int)time(NULL) ^ 0x9e3779b9U) | 1U;
	for (size_t i = d_lines - 1; i > 1; i--) {
		seed = seed * 1103515245U + 12345U;
		size_t j = 1 + ((size_t)(seed >> 16) % i);
		size_t tmp = order[i];
		order[i] = order[j];
		order[j] = tmp;
	}

	for (size_t i = 0; i < d_lines - 1; i++)
		perm[order[i]] = order[i + 1];
	perm[order[d_lines - 1]] = d_lines;

	void **last_slot = NULL;
	for (size_t b = 0; b < num_blocks; b++) {
		uint8_t *blk = buf + b * d_lines * step_bytes;
		for (size_t i = 0; i < d_lines; i++) {
			void **slot = (void **)(blk + i * step_bytes);
			*slot = (void *)(blk + perm[i] * step_bytes);
			if (perm[i] == d_lines)
				last_slot = slot;
		}
	}
	if (last_slot)
		*last_slot = (void *)buf;

	free(order);
	free(perm);
	return num_blocks * d_lines;
}

static void *c2c_writer_thread(void *arg)
{
	c2c_shared_ctx_t *ctx = (c2c_shared_ctx_t *)arg;
	numa_pin_cpu(ctx->writer_cpu);

	void *evict_buf = NULL;
	size_t evict_alloc = 0;
	size_t evict_iters = 0;

	if (ctx->writer_extra_bytes > 0) {
		evict_alloc = ctx->writer_extra_bytes + 512;
		int w_node = get_alloc_node_for_cpu(ctx->writer_cpu);
		evict_buf = alloc_mem_onnode(evict_alloc, w_node, HUGEPAGE_NONE);
		if (evict_buf) {
			uint64_t *words = (uint64_t *)evict_buf;
			size_t nwords = ctx->writer_extra_bytes / sizeof(uint64_t);
			for (size_t i = 0; i < nwords; i++)
				words[i] = (uint64_t)i;
			evict_iters = ctx->writer_extra_bytes / ctx->step_bytes;
		}
	}

	int64_t done = 0;
	while (done < ctx->total_iters) {
		void *curr = ctx->buf;
		for (size_t w = 0;
		     w < ctx->num_windows && done < ctx->total_iters; w++) {
			while (ctx->turn != ctx->writer_id)
				_mm_pause();

			if (ctx->clean_hit) {
				curr = c2c_read_chain(curr, ctx->window_lines);
			} else {
				curr = c2c_write_chain(curr, ctx->window_lines,
						       ctx->step_bytes);
			}

			/*
			 * Walk private eviction buffer (~1.2x L1D by default) so
			 * the window's E/M lines are evicted from Writer's L1D
			 * into Writer's private L2 before Reader snoops them.
			 */
			if (evict_buf && evict_iters > 0) {
				uint64_t sink = 0;
				const uint8_t *ep = (const uint8_t *)evict_buf;
				for (size_t k = 0; k < evict_iters; k++) {
					sink += *(const volatile uint64_t *)ep;
					ep += ctx->step_bytes;
				}
				ctx->evict_sink = sink;
			}

			_mm_sfence();
			ctx->turn = ctx->writer_next_id;
			done += (int64_t)ctx->window_lines;
		}
	}

	if (evict_buf)
		free_mem_onnode(evict_buf, evict_alloc);
	return NULL;
}

static void *c2c_reader_thread(void *arg)
{
	c2c_reader_arg_t *rarg = (c2c_reader_arg_t *)arg;
	c2c_shared_ctx_t *ctx = rarg->shared;
	numa_pin_cpu(rarg->cpu);

	void *extra_buf = NULL;
	size_t extra_alloc = 0;
	size_t extra_iters = 0;

	if (rarg->extra_bytes > 0) {
		extra_alloc = rarg->extra_bytes + 512;
		int r_node = get_alloc_node_for_cpu(rarg->cpu);
		extra_buf = alloc_mem_onnode(extra_alloc, r_node, HUGEPAGE_NONE);
		if (extra_buf) {
			uint64_t *words = (uint64_t *)extra_buf;
			size_t nwords = rarg->extra_bytes / sizeof(uint64_t);
			for (size_t i = 0; i < nwords; i++)
				words[i] = (uint64_t)i;
			extra_iters = rarg->extra_bytes / ctx->step_bytes;
		}
	}

	uint64_t total_cycles = 0;
	int64_t done = 0;

	while (done < ctx->total_iters) {
		void *curr = ctx->buf;
		for (size_t w = 0;
		     w < ctx->num_windows && done < ctx->total_iters; w++) {
			while (ctx->turn != rarg->my_id)
				_mm_pause();

			_mm_lfence();
			uint64_t t0 = __rdtsc();
			_mm_lfence();

			if (ctx->reader_rfo) {
				curr = c2c_write_chain(curr, ctx->window_lines,
						       ctx->step_bytes);
			} else {
				curr = c2c_read_chain(curr, ctx->window_lines);
			}

			_mm_lfence();
			uint64_t t1 = __rdtsc();
			_mm_lfence();

			total_cycles += (t1 - t0);

			if (!ctx->reader_rfo && extra_buf && extra_iters > 0) {
				uint64_t sink = 0;
				const uint8_t *ep = (const uint8_t *)extra_buf;
				for (size_t k = 0; k < extra_iters; k++) {
					sink += *(const volatile uint64_t *)ep;
					ep += ctx->step_bytes;
				}
				rarg->extra_sink = sink;
			}

			_mm_sfence();
			ctx->turn = rarg->next_id;
			done += (int64_t)ctx->window_lines;
		}
	}

	double avg_clocks =
	    (done > 0) ? ((double)total_cycles / (double)done) : 0.0;
	double avg_ns =
	    (ctx->tsc_ghz > 0.0) ? (avg_clocks / ctx->tsc_ghz) : 0.0;
	rarg->avg_clocks = avg_clocks;
	rarg->avg_ns = avg_ns;

	switch (rarg->print_mode) {
	case C2C_PRINT_MATRIX_CELL:
		printf("%6.1f\t", avg_ns);
		break;
	case C2C_PRINT_SUMMARY_LINE:
		printf("\t%.1f\n", avg_ns);
		break;
	case C2C_PRINT_DIRECTED:
		printf("\nLatency = %.1f base frequency clocks (%.1f ns)\n",
		       avg_clocks, avg_ns);
		break;
	case C2C_PRINT_SILENT:
	default:
		break;
	}

	if (extra_buf)
		free_mem_onnode(extra_buf, extra_alloc);
	return NULL;
}

/*
 * Execute one C2C transfer measurement (StartTheTest / FUN_0012d0a3 equivalent):
 * 1) Pins calling thread to `init_cpu` so pages are allocated and first-touched
 *    on the requested home socket/NUMA node.
 * 2) Initializes circular (sequential or block-random) pointer chain across the
 *    multi-LLC buffer (`buf_size_kb` KiB) and flushes all lines with `clflush`.
 * 3) Runs Writer (id=2) -> [optional Sharer (id=1)] -> Reader (id=0) in lockstep
 *    sliding windows of `window_size_bytes`.
 */
static int execute_c2c_transfer(int writer_cpu, int reader_cpu, int third_cpu,
				int init_cpu, size_t buf_size_kb,
				size_t window_size_bytes,
				size_t writer_extra_bytes,
				size_t reader_extra_bytes, bool clean_hit,
				bool reader_rfo, bool do_clflush,
				bool random_access, int random_range,
				int stride, int64_t total_iters,
				c2c_print_mode_t print_mode, bool verbose,
				double tsc_ghz)
{
	cpu_set_t saved_mask;
	CPU_ZERO(&saved_mask);
	bool have_saved =
	    (sched_getaffinity(0, sizeof(saved_mask), &saved_mask) == 0);

	numa_pin_cpu(init_cpu);

	size_t step_bytes = (stride >= 16) ? ((size_t)stride & ~7ULL)
					   : (size_t)C2C_DEFAULT_STRIDE;
	if (step_bytes < 16)
		step_bytes = C2C_DEFAULT_STRIDE;

	size_t buf_bytes = buf_size_kb << 10;
	if (buf_bytes < step_bytes * 2)
		buf_bytes = step_bytes * 2;
	size_t alloc_bytes = buf_bytes + 512;

	int home_node = get_alloc_node_for_cpu(init_cpu);
	uint8_t *buf =
	    (uint8_t *)alloc_mem_onnode(alloc_bytes, home_node, HUGEPAGE_NONE);
	if (!buf) {
		if (have_saved)
			sched_setaffinity(0, sizeof(saved_mask), &saved_mask);
		fprintf(stderr, "Buffer allocation failed!\n");
		return 12;
	}

	if (verbose) {
		printf("\tbuf_size=%zu KB, loopsize=%zu KB, extra bytes=%zu KB\n",
		       buf_size_kb, window_size_bytes >> 10,
		       writer_extra_bytes >> 10);
	}

	size_t chain_lines = init_c2c_pointer_chain(
	    buf, buf_bytes, step_bytes, random_access, random_range);

	if (do_clflush) {
		for (size_t off = 0; off < buf_bytes; off += 64)
			_mm_clflush(buf + off);
		_mm_mfence();
	}

	size_t window_lines = window_size_bytes / step_bytes;
	if (window_lines < 1)
		window_lines = 1;
	if (window_lines > chain_lines)
		window_lines = chain_lines;
	size_t num_windows = chain_lines / window_lines;
	if (num_windows < 1)
		num_windows = 1;

	bool has_sharer = (third_cpu >= 0);

	c2c_shared_ctx_t shared;
	memset(&shared, 0, sizeof(shared));
	shared.buf = buf;
	shared.step_bytes = step_bytes;
	shared.window_lines = window_lines;
	shared.num_windows = num_windows;
	shared.total_iters = total_iters;
	shared.clean_hit = clean_hit;
	shared.reader_rfo = reader_rfo;
	shared.writer_extra_bytes = writer_extra_bytes;
	shared.tsc_ghz = tsc_ghz;
	shared.writer_cpu = writer_cpu;
	shared.writer_id = 2;
	shared.writer_next_id = has_sharer ? 1 : 0;
	shared.turn = 2;

	c2c_reader_arg_t reader_arg;
	memset(&reader_arg, 0, sizeof(reader_arg));
	reader_arg.shared = &shared;
	reader_arg.cpu = reader_cpu;
	reader_arg.my_id = 0;
	reader_arg.next_id = 2;
	reader_arg.extra_bytes = reader_extra_bytes;
	reader_arg.print_mode = print_mode;

	c2c_reader_arg_t sharer_arg;
	memset(&sharer_arg, 0, sizeof(sharer_arg));
	sharer_arg.shared = &shared;
	sharer_arg.cpu = third_cpu;
	sharer_arg.my_id = 1;
	sharer_arg.next_id = 0;
	sharer_arg.extra_bytes = 0;
	sharer_arg.print_mode = C2C_PRINT_SILENT;

	pthread_t writer_tid, reader_tid, sharer_tid;
	if (pthread_create(&writer_tid, NULL, c2c_writer_thread, &shared) != 0) {
		free_mem_onnode(buf, alloc_bytes);
		if (have_saved)
			sched_setaffinity(0, sizeof(saved_mask), &saved_mask);
		return 13;
	}

	/*
	 * Wait until Writer finishes pinning, allocating its L1D eviction
	 * buffer, and preparing Window 0 before launching Reader/Sharer.
	 */
	if (total_iters > 0) {
		while (shared.turn == 2)
			usleep(200);
	}

	if (pthread_create(&reader_tid, NULL, c2c_reader_thread, &reader_arg) != 0) {
		shared.total_iters = 0;
		shared.turn = 2;
		pthread_join(writer_tid, NULL);
		free_mem_onnode(buf, alloc_bytes);
		if (have_saved)
			sched_setaffinity(0, sizeof(saved_mask), &saved_mask);
		return 13;
	}

	if (has_sharer) {
		if (pthread_create(&sharer_tid, NULL, c2c_reader_thread,
				   &sharer_arg) != 0) {
			shared.total_iters = 0;
			shared.turn = 0;
			pthread_join(reader_tid, NULL);
			shared.turn = 2;
			pthread_join(writer_tid, NULL);
			free_mem_onnode(buf, alloc_bytes);
			if (have_saved)
				sched_setaffinity(0, sizeof(saved_mask),
						  &saved_mask);
			return 13;
		}
	}

	pthread_join(writer_tid, NULL);
	if (has_sharer)
		pthread_join(sharer_tid, NULL);
	pthread_join(reader_tid, NULL);

	free_mem_onnode(buf, alloc_bytes);
	if (have_saved)
		sched_setaffinity(0, sizeof(saved_mask), &saved_mask);
	return 0;
}

static void run_remote_hitm_matrix(bool homed_in_reader,
				   const c2c_latency_options_t *opts,
				   const cpu_cache_info_t *cinfo,
				   bool is_amd, double tsc_ghz)
{
	size_t rem_buf_kb;
	size_t rem_win_bytes;
	if (!is_amd && cinfo->l2_bytes >= 0x96000U) {
		/* Intel >= 600 KiB L2 (Skylake-SP 1 MiB, Sapphire Rapids 2 MiB) */
		rem_buf_kb = 0x80000U;		/* 512 MiB */
		rem_win_bytes = 0x80000U;	/* 512 KiB */
	} else if (is_amd) {
		rem_buf_kb = 0x7d000U;		/* 500,000 KiB */
		rem_win_bytes = 0x64000U;	/* 400 KiB */
	} else {
		rem_buf_kb = 0x80000U;		/* 512 MiB */
		rem_win_bytes = 0x20000U;	/* 128 KiB */
	}
	size_t writer_extra = (size_t)((double)cinfo->l1d_bytes * 1.2);

	if (numa_is_numa_mode()) {
		int num_nodes = numa_get_num_nodes();
		if (!opts->verbose) {
			printf("\t\t\tReader Numa Node\nWriter Numa Node");
			for (int i = 0; i < num_nodes; i++) {
				const numa_node_info_t *n = numa_get_node(i);
				if (n && n->num_cpus > 0)
					printf("%6d\t", n->node_id);
			}
			putchar('\n');
		}

		for (int w = 0; w < num_nodes; w++) {
			const numa_node_info_t *wnode = numa_get_node(w);
			if (!wnode || wnode->num_cpus <= 0)
				continue;

			if (!opts->verbose)
				printf("%13d\t", wnode->node_id);

			int w_cpu = (wnode->reserved_cpus[0] >= 0)
					? wnode->reserved_cpus[0]
					: wnode->cpus[0];

			for (int r = 0; r < num_nodes; r++) {
				const numa_node_info_t *rnode = numa_get_node(r);
				if (!rnode || rnode->num_cpus <= 0)
					continue;

				if (w == r) {
					printf("     -\t");
				} else {
					int r_cpu =
					    (rnode->reserved_cpus[1] >= 0)
						? rnode->reserved_cpus[1]
						: rnode->cpus[0];
					int r_primary =
					    (rnode->reserved_cpus[0] >= 0)
						? rnode->reserved_cpus[0]
						: rnode->cpus[0];
					int home_cpu =
					    homed_in_reader ? r_primary : w_cpu;
					int home_id = homed_in_reader
							  ? rnode->node_id
							  : wnode->node_id;
					c2c_print_mode_t pmode =
					    C2C_PRINT_MATRIX_CELL;
					if (opts->verbose) {
						pmode = C2C_PRINT_SUMMARY_LINE;
						printf("\nWriter numa node%2d (core %3d) Reader numa node%2d (core %3d) Memory numa node%2d (core %3d)..",
						       wnode->node_id, w_cpu,
						       rnode->node_id,
						       r_primary, home_id,
						       home_cpu);
					}
					execute_c2c_transfer(
					    w_cpu, r_cpu, -1, home_cpu,
					    rem_buf_kb, rem_win_bytes,
					    writer_extra,
					    opts->reader_extra_bytes, false,
					    opts->reader_rfo, opts->do_clflush,
					    opts->random_access,
					    opts->random_range, opts->stride,
					    C2C_DEFAULT_ITERS, pmode,
					    opts->verbose, tsc_ghz);
				}
			}
			putchar('\n');
		}
	} else {
		int num_sockets = numa_get_num_sockets();
		if (!opts->verbose) {
			printf("\t\tReader Socket\nWriter Socket\t");
			for (int i = 0; i < num_sockets; i++) {
				const socket_info_t *s = numa_get_socket(i);
				if (s && s->num_cpus > 0)
					printf("%6d\t", s->socket_id);
			}
			putchar('\n');
		}

		for (int w = 0; w < num_sockets; w++) {
			const socket_info_t *wsock = numa_get_socket(w);
			if (!wsock || wsock->num_cpus <= 0)
				continue;

			if (!opts->verbose)
				printf("%13d\t", wsock->socket_id);

			int w_cpu = (wsock->reserved_cpus[0] >= 0)
					? wsock->reserved_cpus[0]
					: wsock->cpus[0];

			for (int r = 0; r < num_sockets; r++) {
				const socket_info_t *rsock = numa_get_socket(r);
				if (!rsock || rsock->num_cpus <= 0)
					continue;

				if (w == r) {
					printf("     -\t");
				} else {
					int r_cpu =
					    (rsock->reserved_cpus[1] >= 0)
						? rsock->reserved_cpus[1]
						: rsock->cpus[0];
					int r_primary =
					    (rsock->reserved_cpus[0] >= 0)
						? rsock->reserved_cpus[0]
						: rsock->cpus[0];
					int home_cpu =
					    homed_in_reader ? r_primary : w_cpu;
					int home_id = homed_in_reader
							  ? rsock->socket_id
							  : wsock->socket_id;
					c2c_print_mode_t pmode =
					    C2C_PRINT_MATRIX_CELL;
					if (opts->verbose) {
						pmode = C2C_PRINT_SUMMARY_LINE;
						printf("\nWriter socket%2d (core %3d) Reader socket%2d (core %3d) Memory socket%2d (core %3d)..",
						       wsock->socket_id, w_cpu,
						       rsock->socket_id,
						       r_primary, home_id,
						       home_cpu);
					}
					execute_c2c_transfer(
					    w_cpu, r_cpu, -1, home_cpu,
					    rem_buf_kb, rem_win_bytes,
					    writer_extra,
					    opts->reader_extra_bytes, false,
					    opts->reader_rfo, opts->do_clflush,
					    opts->random_access,
					    opts->random_range, opts->stride,
					    C2C_DEFAULT_ITERS, pmode,
					    opts->verbose, tsc_ghz);
				}
			}
			putchar('\n');
		}
	}
}

void c2c_latency_init_defaults(c2c_latency_options_t *opts)
{
	if (!opts)
		return;
	memset(opts, 0, sizeof(*opts));
	opts->summary_mode = true;
	opts->reader_cpu = -1;
	opts->writer_cpu = -1;
	opts->third_cpu = -1;
	opts->init_cpu = 0;
	opts->do_clflush = true;
	opts->random_range = C2C_DEFAULT_RAND_RANGE;
	opts->stride = C2C_DEFAULT_STRIDE;
	opts->total_iters = C2C_DEFAULT_ITERS;
}

int run_c2c_latency_ex(const c2c_latency_options_t *opts, double tsc_freq_ghz)
{
	c2c_latency_options_t def_opts;
	if (!opts) {
		c2c_latency_init_defaults(&def_opts);
		opts = &def_opts;
	}

	const cpu_cache_info_t *cinfo = cpu_get_cache_info();
	bool is_amd = cpu_is_amd_processor();
	int total_cpus = numa_get_total_cpus();

	const socket_info_t *sock0 = numa_get_socket(0);
	int def_reader = 0;
	int def_writer = (total_cpus > 1) ? 1 : 0;
	if (sock0 && sock0->num_one_tpc_cpus >= 2) {
		def_reader = sock0->reserved_cpus[0];
		def_writer = sock0->reserved_cpus[1];
	} else if (sock0 && sock0->num_cpus >= 2) {
		def_reader = sock0->cpus[0];
		def_writer = sock0->cpus[1];
	}
	/*
	 * On Xeon E-core platforms (Sierra Forest / Grand Ridge / Clearwater
	 * Forest) where 4 cores share an L2 module, default writer to core 4
	 * so reader and writer reside in distinct L2 modules (matches 0x1024c0).
	 */
	if (cpu_is_xeon_atom_core() && total_cpus > 4) {
		if (sock0 && sock0->num_one_tpc_cpus > 4)
			def_writer = sock0->one_tpc_cpus[4];
		else
			def_writer = 4;
	}

	int reader_cpu = opts->has_reader_cpu ? opts->reader_cpu : def_reader;
	int writer_cpu = opts->has_writer_cpu ? opts->writer_cpu : def_writer;
	int third_cpu = opts->third_cpu;
	int init_cpu = opts->has_init_cpu ? opts->init_cpu : 0;

	int64_t total_iters =
	    opts->has_iters ? opts->total_iters : C2C_DEFAULT_ITERS;
	size_t default_buf_kb = is_amd ? 0x1c2000ULL /* 1,800,000 KiB */
				       : ((cinfo->l3_bytes * 3ULL) / 1024ULL);
	if (default_buf_kb < 4096)
		default_buf_kb = 84480;
	size_t buf_size_kb =
	    opts->has_buf_size ? opts->buf_size_kb : default_buf_kb;

	size_t window_bytes =
	    opts->has_window_size
		? opts->window_size_bytes
		: (size_t)((double)cinfo->l1d_bytes * 1.2);
	size_t writer_extra_bytes =
	    opts->has_writer_extra
		? opts->writer_extra_bytes
		: (size_t)((double)cinfo->l1d_bytes * 1.2);

	if (opts->verbose) {
		printf("In c2c_latency_mode\n");
		printf("L1-I cache (%d) size:%zu KBytes\n",
		       cinfo->l1i_type, cinfo->l1i_bytes >> 10);
		printf("L1-D cache (%d) size:%zu KBytes\n",
		       cinfo->l1d_type, cinfo->l1d_bytes >> 10);
		printf("L2 cache (%d) size:%zu KBytes\n",
		       cinfo->l2_type, cinfo->l2_bytes >> 10);
		printf("L3 cache (%d) size:%zu KBytes\n",
		       cinfo->l3_type, cinfo->l3_bytes >> 10);
		printf("Writer thread ID:%d\tReader thread ID:%d\tThird thread ID:%d\n",
		       writer_cpu, reader_cpu, third_cpu);
	}

	putchar('\n');
	puts("Measuring cache-to-cache transfer latency (in ns)...");

	if (!opts->summary_mode) {
		if (writer_cpu == reader_cpu) {
			printf("The writer and the reader are both on cpu %d\n",
			       reader_cpu);
			puts("A cache-to-cache transfer needs two different cpus. Use -c or -w to separate them.");
			return 22;
		}
		if (third_cpu >= 0 &&
		    (third_cpu == reader_cpu || third_cpu == writer_cpu)) {
			const char *role =
			    (third_cpu == reader_cpu) ? "reader" : "writer";
			printf("The third thread is on cpu %d, which is already the %s\n",
			       third_cpu, role);
			puts("Use -S to move it to a cpu of its own.");
			return 22;
		}

		return execute_c2c_transfer(
		    writer_cpu, reader_cpu, third_cpu, init_cpu, buf_size_kb,
		    window_bytes, writer_extra_bytes, opts->reader_extra_bytes,
		    opts->clean_hit, opts->reader_rfo, opts->do_clflush,
		    opts->random_access, opts->random_range, opts->stride,
		    total_iters, C2C_PRINT_DIRECTED, opts->verbose,
		    tsc_freq_ghz);
	}

	/* Summary mode: Local HIT + Local HITM + optional multi-socket Remote HITM */
	if (total_cpus < 2 || writer_cpu == reader_cpu) {
		printf("Error: At least 2 CPUs on socket 0 are required for C2C measurement.\n");
		return 22;
	}

	size_t local_summary_win =
	    is_amd ? 0x64000ULL : (cinfo->l1d_bytes * 3ULL);
	size_t local_writer_extra = (size_t)((double)cinfo->l1d_bytes * 1.2);

	printf("Local Socket L2->L2 HIT  latency");
	int rc = execute_c2c_transfer(
	    writer_cpu, reader_cpu, -1, 0, default_buf_kb, local_summary_win,
	    local_writer_extra, opts->reader_extra_bytes, true,
	    opts->reader_rfo, opts->do_clflush, opts->random_access,
	    opts->random_range, opts->stride, C2C_DEFAULT_ITERS,
	    C2C_PRINT_SUMMARY_LINE, opts->verbose, tsc_freq_ghz);
	if (rc != 0)
		return rc;

	printf("Local Socket L2->L2 HITM latency");
	rc = execute_c2c_transfer(
	    writer_cpu, reader_cpu, -1, 0, default_buf_kb, local_summary_win,
	    local_writer_extra, opts->reader_extra_bytes, false,
	    opts->reader_rfo, opts->do_clflush, opts->random_access,
	    opts->random_range, opts->stride, C2C_DEFAULT_ITERS,
	    C2C_PRINT_SUMMARY_LINE, opts->verbose, tsc_freq_ghz);
	if (rc != 0)
		return rc;

	int compute_domains = 0;
	if (numa_is_numa_mode()) {
		int nn = numa_get_num_nodes();
		for (int i = 0; i < nn; i++) {
			const numa_node_info_t *n = numa_get_node(i);
			if (n && n->num_cpus > 0)
				compute_domains++;
		}
	} else {
		compute_domains = numa_get_num_sockets();
	}

	if (numa_get_num_sockets() > 1 && compute_domains > 1) {
		puts("Remote Socket L2->L2 HITM latency (data address homed in writer socket)");
		run_remote_hitm_matrix(false, opts, cinfo, is_amd, tsc_freq_ghz);
		puts("Remote Socket L2->L2 HITM latency (data address homed in reader socket)");
		run_remote_hitm_matrix(true, opts, cinfo, is_amd, tsc_freq_ghz);
	}

	return 0;
}

void run_c2c_latency_benchmark(double tsc_freq_ghz)
{
	c2c_latency_options_t opts;
	c2c_latency_init_defaults(&opts);
	(void)run_c2c_latency_ex(&opts, tsc_freq_ghz);
}

void run_c2c_bandwidth_benchmark(double duration_sec)
{
	run_c2c_bandwidth_matrix(duration_sec, 0, false);
}
