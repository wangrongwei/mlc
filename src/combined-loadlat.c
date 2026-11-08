#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <immintrin.h>
#include "combined-loadlat.h"
#include "allocation.h"
#include "helper.h"
#include "intrd.h"
#include "fastdelay_dispatch.h"

/*
 * Accurate calibrated delay injection kernel matching Intel MLC assembly:
 *   xor %eax, %eax
 * 1:
 *   inc %eax
 *   cmp %[cnt], %eax
 *   jl 1b
 */
static inline void inject_delay(int count)
{
	if (count <= 0)
		return;
	__asm__ volatile (
		"xor %%eax, %%eax\n\t"
		"1:\n\t"
		"inc %%eax\n\t"
		"cmp %[cnt], %%eax\n\t"
		"jl 1b\n\t"
		:
		: [cnt] "r"(count)
		: "eax", "cc"
	);
}

typedef struct {
	int cpu;
	int alloc_node;
	size_t buf_size;
	double duration_sec;
	traffic_type_t traffic;
	int inject_delay;
	volatile bool stop_flag;
	double mb_per_sec;
	pthread_barrier_t *start_barrier;
} bw_worker_arg_t;

static void *bw_worker(void *arg)
{
	bw_worker_arg_t *w = (bw_worker_arg_t *) arg;
	numa_pin_cpu(w->cpu);

	size_t size = w->buf_size;
	uint8_t *local_buf =
	    (uint8_t *) alloc_mem_onnode(size, w->alloc_node, false);
	uint8_t *write_buf =
	    (uint8_t *) alloc_mem_onnode(size, w->alloc_node, false);
	uint8_t *buf2 = NULL;
	if (w->traffic == TRAFFIC_STREAM_TRIAD
	    || w->traffic == TRAFFIC_STREAM_TRIAD_RFO) {
		buf2 = (uint8_t *) alloc_mem_onnode(size, w->alloc_node, false);
	}
	if (!local_buf || !write_buf
	    || ((w->traffic == TRAFFIC_STREAM_TRIAD
		 || w->traffic == TRAFFIC_STREAM_TRIAD_RFO) && !buf2)) {
		if (local_buf)
			free_mem_onnode(local_buf, size);
		if (write_buf)
			free_mem_onnode(write_buf, size);
		if (buf2)
			free_mem_onnode(buf2, size);
		w->mb_per_sec = 0;
		if (w->start_barrier)
			pthread_barrier_wait(w->start_barrier);
		return NULL;
	}

	if (w->start_barrier) {
		pthread_barrier_wait(w->start_barrier);
	}

	struct timespec t_start, t_curr;
	clock_gettime(CLOCK_MONOTONIC_RAW, &t_start);

	size_t bytes_transferred = 0;

	while (!w->stop_flag) {
		size_t offset = 0;
		switch (w->traffic) {
		case TRAFFIC_ALL_READS: {
			/* W1: 100% reads (8 cache lines = 512 bytes per iteration) */
			while (offset + 512 <= size && !w->stop_flag) {
				uint8_t *p = local_buf + offset;
				__asm__ volatile (
					"vmovdqa 0(%[p]), %%ymm0\n\t"
					"vmovdqa 32(%[p]), %%ymm1\n\t"
					"vmovdqa 64(%[p]), %%ymm2\n\t"
					"vmovdqa 96(%[p]), %%ymm3\n\t"
					"vmovdqa 128(%[p]), %%ymm0\n\t"
					"vmovdqa 160(%[p]), %%ymm1\n\t"
					"vmovdqa 192(%[p]), %%ymm2\n\t"
					"vmovdqa 224(%[p]), %%ymm3\n\t"
					"vmovdqa 256(%[p]), %%ymm0\n\t"
					"vmovdqa 288(%[p]), %%ymm1\n\t"
					"vmovdqa 320(%[p]), %%ymm2\n\t"
					"vmovdqa 352(%[p]), %%ymm3\n\t"
					"vmovdqa 384(%[p]), %%ymm0\n\t"
					"vmovdqa 416(%[p]), %%ymm1\n\t"
					"vmovdqa 448(%[p]), %%ymm2\n\t"
					"vmovdqa 480(%[p]), %%ymm3\n\t"
					:
					: [p] "r"(p)
					: "ymm0", "ymm1", "ymm2", "ymm3", "memory"
				);
				offset += 512;
				bytes_transferred += 512;
				if (w->inject_delay > 0)
					inject_delay(w->inject_delay);
			}
			break;
		}
		case TRAFFIC_2_1_READ_WRITE: {
			/* W2: 2:1 read-write (4 lines read = 256B, 2 lines written = 128B, total 384B) */
			while (offset + 256 <= size && !w->stop_flag) {
				uint8_t *p = local_buf + offset;
				uint8_t *wp = write_buf + (offset % size);
				__asm__ volatile (
					"vmovdqa 0(%[p]), %%ymm0\n\t"
					"vmovdqa 32(%[p]), %%ymm1\n\t"
					"vmovdqa 64(%[p]), %%ymm2\n\t"
					"vmovdqa 96(%[p]), %%ymm3\n\t"
					"vmovdqa %%ymm0, 0(%[wp])\n\t"
					"vmovdqa %%ymm1, 32(%[wp])\n\t"
					"vmovdqa 128(%[p]), %%ymm0\n\t"
					"vmovdqa 160(%[p]), %%ymm1\n\t"
					"vmovdqa 192(%[p]), %%ymm2\n\t"
					"vmovdqa 224(%[p]), %%ymm3\n\t"
					"vmovdqa %%ymm2, 64(%[wp])\n\t"
					"vmovdqa %%ymm3, 96(%[wp])\n\t"
					:
					: [p] "r"(p), [wp] "r"(wp)
					: "ymm0", "ymm1", "ymm2", "ymm3", "memory"
				);
				offset += 256;
				bytes_transferred += 384;
				if (w->inject_delay > 0)
					inject_delay(w->inject_delay);
			}
			break;
		}
		case TRAFFIC_3_1_READ_WRITE: {
			/* W3: 3:1 read-write (6 lines read = 384B, 2 lines written = 128B, total 512B) */
			while (offset + 384 <= size && !w->stop_flag) {
				uint8_t *p = local_buf + offset;
				uint8_t *wp = write_buf + (offset % size);
				__asm__ volatile (
					"vmovdqa 0(%[p]), %%ymm0\n\t"
					"vmovdqa 32(%[p]), %%ymm1\n\t"
					"vmovdqa 64(%[p]), %%ymm2\n\t"
					"vmovdqa 96(%[p]), %%ymm3\n\t"
					"vmovdqa 128(%[p]), %%ymm0\n\t"
					"vmovdqa 160(%[p]), %%ymm1\n\t"
					"vmovdqa %%ymm0, 0(%[wp])\n\t"
					"vmovdqa %%ymm1, 32(%[wp])\n\t"
					"vmovdqa 192(%[p]), %%ymm0\n\t"
					"vmovdqa 224(%[p]), %%ymm1\n\t"
					"vmovdqa 256(%[p]), %%ymm2\n\t"
					"vmovdqa 288(%[p]), %%ymm3\n\t"
					"vmovdqa 320(%[p]), %%ymm0\n\t"
					"vmovdqa 352(%[p]), %%ymm1\n\t"
					"vmovdqa %%ymm0, 64(%[wp])\n\t"
					"vmovdqa %%ymm1, 96(%[wp])\n\t"
					:
					: [p] "r"(p), [wp] "r"(wp)
					: "ymm0", "ymm1", "ymm2", "ymm3", "memory"
				);
				offset += 384;
				bytes_transferred += 512;
				if (w->inject_delay > 0)
					inject_delay(w->inject_delay);
			}
			break;
		}
		case TRAFFIC_3_2_READ_WRITE: {
			/* W4: 3:2 read-write (6 lines read = 384B, 4 lines written = 256B, total 640B) */
			while (offset + 384 <= size && !w->stop_flag) {
				uint8_t *p = local_buf + offset;
				uint8_t *wp = write_buf + (offset % size);
				__asm__ volatile (
					"vmovdqa 0(%[p]), %%ymm0\n\t"
					"vmovdqa 32(%[p]), %%ymm1\n\t"
					"vmovdqa 64(%[p]), %%ymm2\n\t"
					"vmovdqa 96(%[p]), %%ymm3\n\t"
					"vmovdqa %%ymm0, 0(%[wp])\n\t"
					"vmovdqa %%ymm1, 32(%[wp])\n\t"
					"vmovdqa 128(%[p]), %%ymm0\n\t"
					"vmovdqa 160(%[p]), %%ymm1\n\t"
					"vmovdqa 192(%[p]), %%ymm2\n\t"
					"vmovdqa 224(%[p]), %%ymm3\n\t"
					"vmovdqa %%ymm2, 64(%[wp])\n\t"
					"vmovdqa %%ymm3, 96(%[wp])\n\t"
					"vmovdqa 256(%[p]), %%ymm0\n\t"
					"vmovdqa 288(%[p]), %%ymm1\n\t"
					"vmovdqa 320(%[p]), %%ymm2\n\t"
					"vmovdqa 352(%[p]), %%ymm3\n\t"
					"vmovdqa %%ymm0, 128(%[wp])\n\t"
					"vmovdqa %%ymm1, 160(%[wp])\n\t"
					"vmovdqa %%ymm2, 192(%[wp])\n\t"
					"vmovdqa %%ymm3, 224(%[wp])\n\t"
					:
					: [p] "r"(p), [wp] "r"(wp)
					: "ymm0", "ymm1", "ymm2", "ymm3", "memory"
				);
				offset += 384;
				bytes_transferred += 640;
				if (w->inject_delay > 0)
					inject_delay(w->inject_delay);
			}
			break;
		}
		case TRAFFIC_1_1_READ_WRITE: {
			/* W5: 1:1 read-write (4 lines read = 256B, 4 lines written = 256B, total 512B) */
			while (offset + 256 <= size && !w->stop_flag) {
				uint8_t *p = local_buf + offset;
				uint8_t *wp = write_buf + (offset % size);
				__asm__ volatile (
					"vmovdqa 0(%[p]), %%ymm0\n\t"
					"vmovdqa 32(%[p]), %%ymm1\n\t"
					"vmovdqa %%ymm0, 0(%[wp])\n\t"
					"vmovdqa %%ymm1, 32(%[wp])\n\t"
					"vmovdqa 64(%[p]), %%ymm2\n\t"
					"vmovdqa 96(%[p]), %%ymm3\n\t"
					"vmovdqa %%ymm2, 64(%[wp])\n\t"
					"vmovdqa %%ymm3, 96(%[wp])\n\t"
					"vmovdqa 128(%[p]), %%ymm0\n\t"
					"vmovdqa 160(%[p]), %%ymm1\n\t"
					"vmovdqa %%ymm0, 128(%[wp])\n\t"
					"vmovdqa %%ymm1, 160(%[wp])\n\t"
					"vmovdqa 192(%[p]), %%ymm2\n\t"
					"vmovdqa 224(%[p]), %%ymm3\n\t"
					"vmovdqa %%ymm2, 192(%[wp])\n\t"
					"vmovdqa %%ymm3, 224(%[wp])\n\t"
					:
					: [p] "r"(p), [wp] "r"(wp)
					: "ymm0", "ymm1", "ymm2", "ymm3", "memory"
				);
				offset += 256;
				bytes_transferred += 512;
				if (w->inject_delay > 0)
					inject_delay(w->inject_delay);
			}
			break;
		}
		case TRAFFIC_0_1_NT_WRITE: {
			/* W6: 0:1 read-NT write (100% NT stores, bypass cache) */
			while (offset + 512 <= size && !w->stop_flag) {
				uint8_t *wp = write_buf + offset;
				__asm__ volatile (
					"vmovntdq %%ymm0, 0(%[wp])\n\t"
					"vmovntdq %%ymm0, 32(%[wp])\n\t"
					"vmovntdq %%ymm0, 64(%[wp])\n\t"
					"vmovntdq %%ymm0, 96(%[wp])\n\t"
					"vmovntdq %%ymm0, 128(%[wp])\n\t"
					"vmovntdq %%ymm0, 160(%[wp])\n\t"
					"vmovntdq %%ymm0, 192(%[wp])\n\t"
					"vmovntdq %%ymm0, 224(%[wp])\n\t"
					"vmovntdq %%ymm0, 256(%[wp])\n\t"
					"vmovntdq %%ymm0, 288(%[wp])\n\t"
					"vmovntdq %%ymm0, 320(%[wp])\n\t"
					"vmovntdq %%ymm0, 352(%[wp])\n\t"
					"vmovntdq %%ymm0, 384(%[wp])\n\t"
					"vmovntdq %%ymm0, 416(%[wp])\n\t"
					"vmovntdq %%ymm0, 448(%[wp])\n\t"
					"vmovntdq %%ymm0, 480(%[wp])\n\t"
					:
					: [wp] "r"(wp)
					: "memory"
				);
				offset += 512;
				bytes_transferred += 512;
				if (w->inject_delay > 0)
					inject_delay(w->inject_delay);
			}
			_mm_sfence();
			break;
		}
		case TRAFFIC_2_1_NT_WRITE: {
			/* W7: 2:1 read-NT write (4 lines read = 256B, 2 lines NT-write = 128B, total 384B) */
			while (offset + 256 <= size && !w->stop_flag) {
				uint8_t *p = local_buf + offset;
				uint8_t *wp = write_buf + (offset % size);
				__asm__ volatile (
					"vmovdqa 0(%[p]), %%ymm0\n\t"
					"vmovdqa 32(%[p]), %%ymm1\n\t"
					"vmovdqa 64(%[p]), %%ymm2\n\t"
					"vmovdqa 96(%[p]), %%ymm3\n\t"
					"vmovntdq %%ymm0, 0(%[wp])\n\t"
					"vmovntdq %%ymm1, 32(%[wp])\n\t"
					"vmovdqa 128(%[p]), %%ymm0\n\t"
					"vmovdqa 160(%[p]), %%ymm1\n\t"
					"vmovdqa 192(%[p]), %%ymm2\n\t"
					"vmovdqa 224(%[p]), %%ymm3\n\t"
					"vmovntdq %%ymm2, 64(%[wp])\n\t"
					"vmovntdq %%ymm3, 96(%[wp])\n\t"
					:
					: [p] "r"(p), [wp] "r"(wp)
					: "ymm0", "ymm1", "ymm2", "ymm3", "memory"
				);
				offset += 256;
				bytes_transferred += 384;
				if (w->inject_delay > 0)
					inject_delay(w->inject_delay);
			}
			_mm_sfence();
			break;
		}
		case TRAFFIC_1_1_NT_WRITE: {
			/* W8: 1:1 read-NT write (4 lines read = 256B, 4 lines NT-write = 256B, total 512B) */
			while (offset + 256 <= size && !w->stop_flag) {
				uint8_t *p = local_buf + offset;
				uint8_t *wp = write_buf + (offset % size);
				__asm__ volatile (
					"vmovdqa 0(%[p]), %%ymm0\n\t"
					"vmovdqa 32(%[p]), %%ymm1\n\t"
					"vmovntdq %%ymm0, 0(%[wp])\n\t"
					"vmovntdq %%ymm1, 32(%[wp])\n\t"
					"vmovdqa 64(%[p]), %%ymm2\n\t"
					"vmovdqa 96(%[p]), %%ymm3\n\t"
					"vmovntdq %%ymm2, 64(%[wp])\n\t"
					"vmovntdq %%ymm3, 96(%[wp])\n\t"
					"vmovdqa 128(%[p]), %%ymm0\n\t"
					"vmovdqa 160(%[p]), %%ymm1\n\t"
					"vmovntdq %%ymm0, 128(%[wp])\n\t"
					"vmovntdq %%ymm1, 160(%[wp])\n\t"
					"vmovdqa 192(%[p]), %%ymm2\n\t"
					"vmovdqa 224(%[p]), %%ymm3\n\t"
					"vmovntdq %%ymm2, 192(%[wp])\n\t"
					"vmovntdq %%ymm3, 224(%[wp])\n\t"
					:
					: [p] "r"(p), [wp] "r"(wp)
					: "ymm0", "ymm1", "ymm2", "ymm3", "memory"
				);
				offset += 256;
				bytes_transferred += 512;
				if (w->inject_delay > 0)
					inject_delay(w->inject_delay);
			}
			_mm_sfence();
			break;
		}
		case TRAFFIC_3_1_NT_WRITE: {
			/* W9: 3:1 read-NT write (6 lines read = 384B, 2 lines NT-write = 128B, total 512B) */
			while (offset + 384 <= size && !w->stop_flag) {
				uint8_t *p = local_buf + offset;
				uint8_t *wp = write_buf + (offset % size);
				__asm__ volatile (
					"vmovdqa 0(%[p]), %%ymm0\n\t"
					"vmovdqa 32(%[p]), %%ymm1\n\t"
					"vmovdqa 64(%[p]), %%ymm2\n\t"
					"vmovdqa 96(%[p]), %%ymm3\n\t"
					"vmovdqa 128(%[p]), %%ymm0\n\t"
					"vmovdqa 160(%[p]), %%ymm1\n\t"
					"vmovntdq %%ymm0, 0(%[wp])\n\t"
					"vmovntdq %%ymm1, 32(%[wp])\n\t"
					"vmovdqa 192(%[p]), %%ymm0\n\t"
					"vmovdqa 224(%[p]), %%ymm1\n\t"
					"vmovdqa 256(%[p]), %%ymm2\n\t"
					"vmovdqa 288(%[p]), %%ymm3\n\t"
					"vmovdqa 320(%[p]), %%ymm0\n\t"
					"vmovdqa 352(%[p]), %%ymm1\n\t"
					"vmovntdq %%ymm0, 64(%[wp])\n\t"
					"vmovntdq %%ymm1, 96(%[wp])\n\t"
					:
					: [p] "r"(p), [wp] "r"(wp)
					: "ymm0", "ymm1", "ymm2", "ymm3", "memory"
				);
				offset += 384;
				bytes_transferred += 512;
				if (w->inject_delay > 0)
					inject_delay(w->inject_delay);
			}
			_mm_sfence();
			break;
		}
		case TRAFFIC_STREAM_TRIAD: {
			/* W10: Stream Triad NT write (2 read buffers, 1 NT store write buffer) */
			while (offset + 256 <= size && !w->stop_flag) {
				uint8_t *p1 = local_buf + offset;
				uint8_t *p2 = buf2 + offset;
				uint8_t *wp = write_buf + (offset % size);
				__asm__ volatile (
					"vmovdqa 0(%[p1]), %%ymm0\n\t"
					"vmovdqa 32(%[p1]), %%ymm1\n\t"
					"vmovdqa 0(%[p2]), %%ymm2\n\t"
					"vmovdqa 32(%[p2]), %%ymm3\n\t"
					"vpaddq %%ymm2, %%ymm0, %%ymm0\n\t"
					"vpaddq %%ymm3, %%ymm1, %%ymm1\n\t"
					"vmovntdq %%ymm0, 0(%[wp])\n\t"
					"vmovntdq %%ymm1, 32(%[wp])\n\t"

					"vmovdqa 64(%[p1]), %%ymm0\n\t"
					"vmovdqa 96(%[p1]), %%ymm1\n\t"
					"vmovdqa 64(%[p2]), %%ymm2\n\t"
					"vmovdqa 96(%[p2]), %%ymm3\n\t"
					"vpaddq %%ymm2, %%ymm0, %%ymm0\n\t"
					"vpaddq %%ymm3, %%ymm1, %%ymm1\n\t"
					"vmovntdq %%ymm0, 64(%[wp])\n\t"
					"vmovntdq %%ymm1, 96(%[wp])\n\t"

					"vmovdqa 128(%[p1]), %%ymm0\n\t"
					"vmovdqa 160(%[p1]), %%ymm1\n\t"
					"vmovdqa 128(%[p2]), %%ymm2\n\t"
					"vmovdqa 160(%[p2]), %%ymm3\n\t"
					"vpaddq %%ymm2, %%ymm0, %%ymm0\n\t"
					"vpaddq %%ymm3, %%ymm1, %%ymm1\n\t"
					"vmovntdq %%ymm0, 128(%[wp])\n\t"
					"vmovntdq %%ymm1, 160(%[wp])\n\t"

					"vmovdqa 192(%[p1]), %%ymm0\n\t"
					"vmovdqa 224(%[p1]), %%ymm1\n\t"
					"vmovdqa 192(%[p2]), %%ymm2\n\t"
					"vmovdqa 224(%[p2]), %%ymm3\n\t"
					"vpaddq %%ymm2, %%ymm0, %%ymm0\n\t"
					"vpaddq %%ymm3, %%ymm1, %%ymm1\n\t"
					"vmovntdq %%ymm0, 192(%[wp])\n\t"
					"vmovntdq %%ymm1, 224(%[wp])\n\t"
					:
					: [p1] "r"(p1), [p2] "r"(p2), [wp] "r"(wp)
					: "ymm0", "ymm1", "ymm2", "ymm3", "memory"
				);
				offset += 256;
				bytes_transferred += 768;
				if (w->inject_delay > 0)
					inject_delay(w->inject_delay);
			}
			_mm_sfence();
			break;
		}
		case TRAFFIC_STREAM_TRIAD_RFO: {
			/* W11: Stream Triad standard write (triggers RFO in hardware) */
			while (offset + 256 <= size && !w->stop_flag) {
				uint8_t *p1 = local_buf + offset;
				uint8_t *p2 = buf2 + offset;
				uint8_t *wp = write_buf + (offset % size);
				__asm__ volatile (
					"vmovdqa 0(%[p1]), %%ymm0\n\t"
					"vmovdqa 32(%[p1]), %%ymm1\n\t"
					"vmovdqa 0(%[p2]), %%ymm2\n\t"
					"vmovdqa 32(%[p2]), %%ymm3\n\t"
					"vpaddq %%ymm2, %%ymm0, %%ymm0\n\t"
					"vpaddq %%ymm3, %%ymm1, %%ymm1\n\t"
					"vmovdqa %%ymm0, 0(%[wp])\n\t"
					"vmovdqa %%ymm1, 32(%[wp])\n\t"

					"vmovdqa 64(%[p1]), %%ymm0\n\t"
					"vmovdqa 96(%[p1]), %%ymm1\n\t"
					"vmovdqa 64(%[p2]), %%ymm2\n\t"
					"vmovdqa 96(%[p2]), %%ymm3\n\t"
					"vpaddq %%ymm2, %%ymm0, %%ymm0\n\t"
					"vpaddq %%ymm3, %%ymm1, %%ymm1\n\t"
					"vmovdqa %%ymm0, 64(%[wp])\n\t"
					"vmovdqa %%ymm1, 96(%[wp])\n\t"

					"vmovdqa 128(%[p1]), %%ymm0\n\t"
					"vmovdqa 160(%[p1]), %%ymm1\n\t"
					"vmovdqa 128(%[p2]), %%ymm2\n\t"
					"vmovdqa 160(%[p2]), %%ymm3\n\t"
					"vpaddq %%ymm2, %%ymm0, %%ymm0\n\t"
					"vpaddq %%ymm3, %%ymm1, %%ymm1\n\t"
					"vmovdqa %%ymm0, 128(%[wp])\n\t"
					"vmovdqa %%ymm1, 160(%[wp])\n\t"

					"vmovdqa 192(%[p1]), %%ymm0\n\t"
					"vmovdqa 224(%[p1]), %%ymm1\n\t"
					"vmovdqa 192(%[p2]), %%ymm2\n\t"
					"vmovdqa 224(%[p2]), %%ymm3\n\t"
					"vpaddq %%ymm2, %%ymm0, %%ymm0\n\t"
					"vpaddq %%ymm3, %%ymm1, %%ymm1\n\t"
					"vmovdqa %%ymm0, 192(%[wp])\n\t"
					"vmovdqa %%ymm1, 224(%[wp])\n\t"
					:
					: [p1] "r"(p1), [p2] "r"(p2), [wp] "r"(wp)
					: "ymm0", "ymm1", "ymm2", "ymm3", "memory"
				);
				offset += 256;
				bytes_transferred += 768;
				if (w->inject_delay > 0)
					inject_delay(w->inject_delay);
			}
			break;
		}
		case TRAFFIC_4_1_READ_WRITE: {
			/* W12: 4:1 read-write (8 lines read = 512B, 2 lines written = 128B, total 640B) */
			while (offset + 512 <= size && !w->stop_flag) {
				uint8_t *p = local_buf + offset;
				uint8_t *wp = write_buf + (offset % size);
				__asm__ volatile (
					"vmovdqa 0(%[p]), %%ymm0\n\t"
					"vmovdqa 32(%[p]), %%ymm1\n\t"
					"vmovdqa 64(%[p]), %%ymm2\n\t"
					"vmovdqa 96(%[p]), %%ymm3\n\t"
					"vmovdqa 128(%[p]), %%ymm0\n\t"
					"vmovdqa 160(%[p]), %%ymm1\n\t"
					"vmovdqa 192(%[p]), %%ymm2\n\t"
					"vmovdqa 224(%[p]), %%ymm3\n\t"
					"vmovdqa %%ymm0, 0(%[wp])\n\t"
					"vmovdqa %%ymm1, 32(%[wp])\n\t"
					"vmovdqa 256(%[p]), %%ymm0\n\t"
					"vmovdqa 288(%[p]), %%ymm1\n\t"
					"vmovdqa 320(%[p]), %%ymm2\n\t"
					"vmovdqa 352(%[p]), %%ymm3\n\t"
					"vmovdqa 384(%[p]), %%ymm0\n\t"
					"vmovdqa 416(%[p]), %%ymm1\n\t"
					"vmovdqa 448(%[p]), %%ymm2\n\t"
					"vmovdqa 480(%[p]), %%ymm3\n\t"
					"vmovdqa %%ymm2, 64(%[wp])\n\t"
					"vmovdqa %%ymm3, 96(%[wp])\n\t"
					:
					: [p] "r"(p), [wp] "r"(wp)
					: "ymm0", "ymm1", "ymm2", "ymm3", "memory"
				);
				offset += 512;
				bytes_transferred += 640;
				if (w->inject_delay > 0)
					inject_delay(w->inject_delay);
			}
			break;
		}
		default: {
			while (offset + 512 <= size && !w->stop_flag) {
				uint8_t *p = local_buf + offset;
				__asm__ volatile (
					"vmovdqa 0(%[p]), %%ymm0\n\t"
					"vmovdqa 32(%[p]), %%ymm1\n\t"
					"vmovdqa 64(%[p]), %%ymm2\n\t"
					"vmovdqa 96(%[p]), %%ymm3\n\t"
					"vmovdqa 128(%[p]), %%ymm0\n\t"
					"vmovdqa 160(%[p]), %%ymm1\n\t"
					"vmovdqa 192(%[p]), %%ymm2\n\t"
					"vmovdqa 224(%[p]), %%ymm3\n\t"
					"vmovdqa 256(%[p]), %%ymm0\n\t"
					"vmovdqa 288(%[p]), %%ymm1\n\t"
					"vmovdqa 320(%[p]), %%ymm2\n\t"
					"vmovdqa 352(%[p]), %%ymm3\n\t"
					"vmovdqa 384(%[p]), %%ymm0\n\t"
					"vmovdqa 416(%[p]), %%ymm1\n\t"
					"vmovdqa 448(%[p]), %%ymm2\n\t"
					"vmovdqa 480(%[p]), %%ymm3\n\t"
					:
					: [p] "r"(p)
					: "ymm0", "ymm1", "ymm2", "ymm3", "memory"
				);
				offset += 512;
				bytes_transferred += 512;
				if (w->inject_delay > 0)
					inject_delay(w->inject_delay);
			}
			break;
		}
		}

		if (w->duration_sec > 0.0) {
			clock_gettime(CLOCK_MONOTONIC_RAW, &t_curr);
			double elapsed = (t_curr.tv_sec - t_start.tv_sec) +
			    (t_curr.tv_nsec - t_start.tv_nsec) * 1e-9;
			if (elapsed >= w->duration_sec) {
				w->mb_per_sec =
				    ((double)bytes_transferred / 1000000.0) /
				    elapsed;
				break;
			}
		}
	}

	if (w->mb_per_sec == 0.0) {
		clock_gettime(CLOCK_MONOTONIC_RAW, &t_curr);
		double elapsed = (t_curr.tv_sec - t_start.tv_sec) +
		    (t_curr.tv_nsec - t_start.tv_nsec) * 1e-9;
		if (elapsed > 0.001) {
			w->mb_per_sec =
			    ((double)bytes_transferred / 1000000.0) / elapsed;
		}
	}

	free_mem_onnode(local_buf, size);
	free_mem_onnode(write_buf, size);
	if (buf2)
		free_mem_onnode(buf2, size);
	return NULL;
}

static double measure_system_bandwidth(traffic_type_t traffic,
				       double duration_sec,
				       size_t per_thread_mb)
{
	int nthreads = numa_get_total_cpus();
	int num_nodes = numa_get_num_nodes();
	pthread_t *threads = (pthread_t *) malloc(nthreads * sizeof(pthread_t));
	bw_worker_arg_t *args =
	    (bw_worker_arg_t *) malloc(nthreads * sizeof(bw_worker_arg_t));

	pthread_barrier_t start_barrier;
	pthread_barrier_init(&start_barrier, NULL, nthreads);

	int current_thread = 0;
	for (int n = 0; n < num_nodes; n++) {
		const numa_node_info_t *node = numa_get_node(n);
		for (int c = 0; c < node->num_cpus && current_thread < nthreads;
		     c++) {
			args[current_thread].cpu = node->cpus[c];
			args[current_thread].alloc_node = n;
			args[current_thread].buf_size =
			    per_thread_mb * 1024 * 1024;
			args[current_thread].duration_sec = duration_sec;
			args[current_thread].traffic = traffic;
			args[current_thread].inject_delay = 0;
			args[current_thread].stop_flag = false;
			args[current_thread].mb_per_sec = 0.0;
			args[current_thread].start_barrier = &start_barrier;
			pthread_create(&threads[current_thread], NULL,
				       bw_worker, &args[current_thread]);
			current_thread++;
		}
	}

	double total_bw = 0.0;
	for (int i = 0; i < current_thread; i++) {
		pthread_join(threads[i], NULL);
		total_bw += args[i].mb_per_sec;
	}

	pthread_barrier_destroy(&start_barrier);
	free(threads);
	free(args);
	return total_bw;
}

static double measure_node_pair_bandwidth(int cpu_node_idx, int mem_node_idx,
					  double duration_sec,
					  size_t per_thread_mb)
{
	const numa_node_info_t *node = numa_get_node(cpu_node_idx);
	if (!node || node->num_cpus <= 0)
		return 0.0;

	int nthreads = node->num_cpus;
	pthread_t *threads = (pthread_t *) malloc(nthreads * sizeof(pthread_t));
	bw_worker_arg_t *args =
	    (bw_worker_arg_t *) malloc(nthreads * sizeof(bw_worker_arg_t));

	pthread_barrier_t start_barrier;
	pthread_barrier_init(&start_barrier, NULL, nthreads);

	for (int i = 0; i < nthreads; i++) {
		args[i].cpu = node->cpus[i];
		args[i].alloc_node = mem_node_idx;
		args[i].buf_size = per_thread_mb * 1024 * 1024;
		args[i].duration_sec = duration_sec;
		args[i].traffic = TRAFFIC_ALL_READS;
		args[i].inject_delay = 0;
		args[i].stop_flag = false;
		args[i].mb_per_sec = 0.0;
		args[i].start_barrier = &start_barrier;
		pthread_create(&threads[i], NULL, bw_worker, &args[i]);
	}

	double total_bw = 0.0;
	for (int i = 0; i < nthreads; i++) {
		pthread_join(threads[i], NULL);
		total_bw += args[i].mb_per_sec;
	}

	pthread_barrier_destroy(&start_barrier);
	free(threads);
	free(args);
	return total_bw;
}

void run_bandwidth_matrix(size_t per_thread_mb, double duration_sec)
{
	printf("Measuring Memory Bandwidths between nodes within system \n");
	printf("Bandwidths are in MB/sec (1 MB/sec = 1,000,000 Bytes/sec)\n");
	printf
	    ("Using all the threads from each core if Hyper-threading is enabled\n");
	printf("Using Read-only traffic type\n");

	int num_nodes = numa_get_num_nodes();
	printf("                Numa node\nNuma node");
	for (int j = 0; j < num_nodes; j++) {
		printf("%16d", j);
	}
	printf("        \n");

	for (int i = 0; i < num_nodes; i++) {
		printf("%8d", i);
		for (int j = 0; j < num_nodes; j++) {
			double bw =
			    measure_node_pair_bandwidth(i, j, duration_sec,
							per_thread_mb);
			printf("%16.1f", bw);
			fflush(stdout);
		}
		printf("        \n");
	}
}

void run_peak_bandwidth(size_t per_thread_mb, double duration_sec)
{
	printf("Measuring Peak Injection Memory Bandwidths for the system\n");
	printf("Bandwidths are in MB/sec (1 MB/sec = 1,000,000 Bytes/sec)\n");
	printf
	    ("Using all the threads from each core if Hyper-threading is enabled\n");
	printf("Using traffic with the following read-write ratios\n");

	double bw_all =
	    measure_system_bandwidth(TRAFFIC_ALL_READS, duration_sec,
				     per_thread_mb);
	printf("ALL Reads        :\t%8.1f        \n", bw_all);

	double bw_3_1 =
	    measure_system_bandwidth(TRAFFIC_3_1_READ_WRITE, duration_sec,
				     per_thread_mb);
	printf("3:1 Reads-Writes :\t%8.1f        \n", bw_3_1);

	double bw_2_1 =
	    measure_system_bandwidth(TRAFFIC_2_1_READ_WRITE, duration_sec,
				     per_thread_mb);
	printf("2:1 Reads-Writes :\t%8.1f        \n", bw_2_1);

	double bw_1_1 =
	    measure_system_bandwidth(TRAFFIC_1_1_READ_WRITE, duration_sec,
				     per_thread_mb);
	printf("1:1 Reads-Writes :\t%8.1f        \n", bw_1_1);

	double bw_triad =
	    measure_system_bandwidth(TRAFFIC_STREAM_TRIAD, duration_sec,
				     per_thread_mb);
	printf("Stream-triad like:\t%8.1f        \n", bw_triad);
}

/*
 * Persistent worker thread structure for run_loaded_latency.
 * Pre-allocates buffers once upfront, eliminating 19x mmap/munmap churn.
 */
typedef struct {
	int cpu;
	int alloc_node;
	size_t buf_size;
	uint8_t *local_buf;
	volatile int inject_delay;
	volatile bool step_running;
	volatile bool terminate;
	volatile size_t bytes_step;
	double elapsed_sec;
	pthread_barrier_t *start_barrier;
	pthread_barrier_t *done_barrier;
} loaded_worker_arg_t;

static void *loaded_bw_worker(void *arg)
{
	loaded_worker_arg_t *w = (loaded_worker_arg_t *) arg;
	numa_pin_cpu(w->cpu);

	size_t size = w->buf_size;
	w->local_buf = (uint8_t *) alloc_mem_onnode(size, w->alloc_node, false);
	if (!w->local_buf) {
		pthread_barrier_wait(w->start_barrier);
		return NULL;
	}

	while (1) {
		/* Wait for main thread to initiate next delay step */
		pthread_barrier_wait(w->start_barrier);
		if (w->terminate)
			break;

		struct timespec t_s, t_e;
		clock_gettime(CLOCK_MONOTONIC_RAW, &t_s);

		size_t offset = 0;
		size_t bytes = 0;
		int delay = w->inject_delay;

		while (w->step_running) {
			uint8_t *p = w->local_buf + offset;
			__asm__ volatile (
				"vmovdqa 0(%[p]), %%ymm0\n\t"
				"vmovdqa 32(%[p]), %%ymm1\n\t"
				"vmovdqa 64(%[p]), %%ymm2\n\t"
				"vmovdqa 96(%[p]), %%ymm3\n\t"
				"vmovdqa 128(%[p]), %%ymm0\n\t"
				"vmovdqa 160(%[p]), %%ymm1\n\t"
				"vmovdqa 192(%[p]), %%ymm2\n\t"
				"vmovdqa 224(%[p]), %%ymm3\n\t"
				"vmovdqa 256(%[p]), %%ymm0\n\t"
				"vmovdqa 288(%[p]), %%ymm1\n\t"
				"vmovdqa 320(%[p]), %%ymm2\n\t"
				"vmovdqa 352(%[p]), %%ymm3\n\t"
				"vmovdqa 384(%[p]), %%ymm0\n\t"
				"vmovdqa 416(%[p]), %%ymm1\n\t"
				"vmovdqa 448(%[p]), %%ymm2\n\t"
				"vmovdqa 480(%[p]), %%ymm3\n\t"
				:
				: [p] "r"(p)
				: "ymm0", "ymm1", "ymm2", "ymm3", "memory"
			);
			offset += 512;
			bytes += 512;
			if (offset + 512 > size)
				offset = 0;
			if (delay > 0)
				inject_delay(delay);
		}

		clock_gettime(CLOCK_MONOTONIC_RAW, &t_e);
		w->elapsed_sec = (t_e.tv_sec - t_s.tv_sec) +
		    (t_e.tv_nsec - t_s.tv_nsec) * 1e-9;
		w->bytes_step = bytes;

		/* Notify main thread that this step is complete */
		pthread_barrier_wait(w->done_barrier);
	}

	free_mem_onnode(w->local_buf, size);
	return NULL;
}

void run_loaded_latency(size_t per_thread_mb, double duration_sec,
			bool random_latency, bool keep_prefetchers)
{
	printf("Measuring Loaded Latencies for the system\n");
	printf
	    ("Using all the threads from each core if Hyper-threading is enabled\n");
	printf("Using Read-only traffic type\n");
	printf("Inject  Latency Bandwidth\n");
	printf("Delay   (ns)    MB/sec\n");
	printf("==========================\n");

	if (!keep_prefetchers) {
		msr_disable_prefetchers(false);
	}

	int num_delays = 0;
	const int *delays = get_default_delays(&num_delays);

	int total_cpus = numa_get_total_cpus();
	int num_nodes = numa_get_num_nodes();
	int nworkers = total_cpus - 1;
	if (nworkers < 1)
		nworkers = 1;

	pthread_t *threads = (pthread_t *) malloc(nworkers * sizeof(pthread_t));
	loaded_worker_arg_t *args =
	    (loaded_worker_arg_t *) malloc(nworkers * sizeof(loaded_worker_arg_t));

	/* Barrier synchronizes nworkers + 1 (main thread) */
	pthread_barrier_t start_barrier, done_barrier;
	pthread_barrier_init(&start_barrier, NULL, nworkers + 1);
	pthread_barrier_init(&done_barrier, NULL, nworkers + 1);

	int current_worker = 0;
	for (int n = 0; n < num_nodes; n++) {
		const numa_node_info_t *node = numa_get_node(n);
		for (int c = 0; c < node->num_cpus && current_worker < nworkers; c++) {
			int cpu = node->cpus[c];
			if (cpu == 0)
				continue;

			args[current_worker].cpu = cpu;
			args[current_worker].alloc_node = n;
			args[current_worker].buf_size = per_thread_mb * 1024 * 1024;
			args[current_worker].inject_delay = 0;
			args[current_worker].step_running = false;
			args[current_worker].terminate = false;
			args[current_worker].bytes_step = 0;
			args[current_worker].elapsed_sec = 0.0;
			args[current_worker].start_barrier = &start_barrier;
			args[current_worker].done_barrier = &done_barrier;
			pthread_create(&threads[current_worker], NULL,
				       loaded_bw_worker, &args[current_worker]);
			current_worker++;
		}
	}

	/* Latency measurement buffer (200MB pointer chasing chain) */
	size_t lat_buf_size = 200 * 1024 * 1024;
	struct cache_line *lat_buf =
	    (struct cache_line *) alloc_mem_onnode(lat_buf_size, 0, false);
	if (!lat_buf) {
		for (int i = 0; i < current_worker; i++)
			args[i].terminate = true;
		pthread_barrier_wait(&start_barrier);
		for (int i = 0; i < current_worker; i++)
			pthread_join(threads[i], NULL);
		pthread_barrier_destroy(&start_barrier);
		pthread_barrier_destroy(&done_barrier);
		free(threads);
		free(args);
		return;
	}
	init_pointer_chase_chain(lat_buf, lat_buf_size, random_latency);

	double step_time = (duration_sec > 0.1) ? duration_sec : 1.0;
	numa_pin_cpu(0);

	for (int d = 0; d < num_delays; d++) {
		int delay = delays[d];

		/* Configure and launch workers simultaneously */
		for (int i = 0; i < current_worker; i++) {
			args[i].inject_delay = delay;
			args[i].step_running = true;
		}
		pthread_barrier_wait(&start_barrier);

		/* 5ms ramp-up for injection queues to saturate */
		usleep(5000);

		struct timespec t_step_start, t_now;
		clock_gettime(CLOCK_MONOTONIC_RAW, &t_step_start);

		volatile struct cache_line *curr = lat_buf;
		uint64_t total_cycles = 0;
		uint64_t total_iters = 0;

		while (true) {
			_mm_lfence();
			uint64_t t0 = __rdtsc();
			_mm_lfence();

			for (int k = 0; k < 50000; k++) {
				curr = curr->next;
			}

			_mm_lfence();
			uint64_t t1 = __rdtsc();
			_mm_lfence();

			total_cycles += (t1 - t0);
			total_iters += 50000;

			clock_gettime(CLOCK_MONOTONIC_RAW, &t_now);
			double el = (t_now.tv_sec - t_step_start.tv_sec) +
			    (t_now.tv_nsec - t_step_start.tv_nsec) * 1e-9;
			if (el >= step_time)
				break;
		}

		double lat_ns =
		    ((double)total_cycles / total_iters) /
		    helper_get_tsc_freq_ghz();

		/* Signal workers to conclude this delay step */
		for (int i = 0; i < current_worker; i++) {
			args[i].step_running = false;
		}
		pthread_barrier_wait(&done_barrier);

		/* Aggregate actual throughput across all injection workers */
		double total_bw = 0.0;
		for (int i = 0; i < current_worker; i++) {
			if (args[i].elapsed_sec > 0.001) {
				total_bw += ((double)args[i].bytes_step / 1000000.0) /
				    args[i].elapsed_sec;
			}
		}

		printf(" %05d  %6.2f  %8.1f\n", delay, lat_ns, total_bw);
		fflush(stdout);
	}

	/* Gracefully terminate persistent workers */
	for (int i = 0; i < current_worker; i++) {
		args[i].terminate = true;
	}
	pthread_barrier_wait(&start_barrier);
	for (int i = 0; i < current_worker; i++) {
		pthread_join(threads[i], NULL);
	}

	free_mem_onnode(lat_buf, lat_buf_size);
	pthread_barrier_destroy(&start_barrier);
	pthread_barrier_destroy(&done_barrier);
	free(threads);
	free(args);

	if (!keep_prefetchers) {
		msr_restore_prefetchers();
	}
}

typedef struct {
	int cpu;
	int node;
	size_t cold_bytes;
	size_t hot_bytes;
	int hot_pct;
	double duration_sec;
	volatile bool stop;
	double mb_per_sec;
} conc_worker_arg_t;

static void *conc_worker(void *arg)
{
	conc_worker_arg_t *w = (conc_worker_arg_t *) arg;
	numa_pin_cpu(w->cpu);

	uint8_t *cold_buf =
	    (uint8_t *) alloc_mem_onnode(w->cold_bytes, w->node, HUGEPAGE_NONE);
	uint8_t *hot_buf =
	    (uint8_t *) alloc_mem_onnode(w->hot_bytes, w->node, HUGEPAGE_NONE);
	if (!cold_buf || !hot_buf) {
		if (cold_buf)
			free_mem_onnode(cold_buf, w->cold_bytes);
		if (hot_buf)
			free_mem_onnode(hot_buf, w->hot_bytes);
		w->mb_per_sec = 0;
		return NULL;
	}

	volatile uint8_t *hb = hot_buf;
	for (size_t i = 0; i < w->hot_bytes; i += 64)
		hb[i] = (uint8_t) i;

	struct timespec t_start, t_curr;
	clock_gettime(CLOCK_MONOTONIC_RAW, &t_start);

	size_t bytes_transferred = 0;
	size_t cold_off = 0;
	size_t hot_off = 0;
	__m256i acc = _mm256_setzero_si256();

	while (!w->stop) {
		for (int step = 0; step < 100; step++) {
			if (step < w->hot_pct) {
				__m256i *p = (__m256i *) (hot_buf + hot_off);
				acc =
				    _mm256_add_epi64(acc,
						     _mm256_load_si256(&p[0]));
				acc =
				    _mm256_add_epi64(acc,
						     _mm256_load_si256(&p[1]));
				hot_off = (hot_off + 64) % (w->hot_bytes - 64);
			} else {
				__m256i *p = (__m256i *) (cold_buf + cold_off);
				acc =
				    _mm256_add_epi64(acc,
						     _mm256_load_si256(&p[0]));
				acc =
				    _mm256_add_epi64(acc,
						     _mm256_load_si256(&p[1]));
				cold_off =
				    (cold_off + 64) % (w->cold_bytes - 64);
			}
			bytes_transferred += 64;
		}

		if (w->duration_sec > 0.0) {
			clock_gettime(CLOCK_MONOTONIC_RAW, &t_curr);
			double elapsed = (t_curr.tv_sec - t_start.tv_sec) +
			    (t_curr.tv_nsec - t_start.tv_nsec) * 1e-9;
			if (elapsed >= w->duration_sec) {
				w->mb_per_sec =
				    ((double)bytes_transferred / 1000000.0) /
				    elapsed;
				break;
			}
		}
	}

	int64_t dummy[4];
	_mm256_storeu_si256((__m256i *) dummy, acc);
	if (dummy[0] == 999999)
		printf("d");

	free_mem_onnode(cold_buf, w->cold_bytes);
	free_mem_onnode(hot_buf, w->hot_bytes);
	return NULL;
}

void run_concurrent_bandwidth(size_t cold_kb, size_t hot_kb, int hot_pct,
			      double duration_sec)
{
	printf("Measuring Concurrent Bandwidth (hot/cold dual-buffer)\n");
	printf("  Cold buffer: %zu KiB (streams through memory)\n", cold_kb);
	printf("  Hot buffer:  %zu KiB (wraps in LLC)\n", hot_kb);
	printf("  Hot access:  %d%%\n", hot_pct);
	printf("Bandwidth is in MB/sec (1 MB/sec = 1,000,000 Bytes/sec)\n");
	printf
	    ("Using all the threads from each core if Hyper-threading is enabled\n\n");

	int total_cpus = numa_get_total_cpus();
	int num_nodes = numa_get_num_nodes();

	pthread_t *threads =
	    (pthread_t *) malloc(total_cpus * sizeof(pthread_t));
	conc_worker_arg_t *args =
	    (conc_worker_arg_t *) malloc(total_cpus *
					 sizeof(conc_worker_arg_t));

	int current_worker = 0;
	for (int n = 0; n < num_nodes; n++) {
		const numa_node_info_t *node = numa_get_node(n);
		for (int c = 0;
		     c < node->num_cpus && current_worker < total_cpus; c++) {
			args[current_worker].cpu = node->cpus[c];
			args[current_worker].node = n;
			args[current_worker].cold_bytes = cold_kb * 1024;
			args[current_worker].hot_bytes = hot_kb * 1024;
			args[current_worker].hot_pct = hot_pct;
			args[current_worker].duration_sec = duration_sec;
			args[current_worker].stop = false;
			args[current_worker].mb_per_sec = 0.0;
			pthread_create(&threads[current_worker], NULL,
				       conc_worker, &args[current_worker]);
			current_worker++;
		}
	}

	double total_bw = 0.0;
	for (int i = 0; i < current_worker; i++) {
		pthread_join(threads[i], NULL);
		total_bw += args[i].mb_per_sec;
	}

	printf("All Reads (hot+cold):\t%8.1f\n", total_bw);

	free(threads);
	free(args);
}
