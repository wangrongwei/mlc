#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <time.h>
#include <immintrin.h>
#include <x86intrin.h>

#include "c2c_bandwidth.h"
#include "allocation.h"
#include "helper.h"

#define C2C_LINE_BYTES 64ULL
#define GIB_BYTES_DBL 1073741824.0

static volatile uint64_t g_c2c_sink = 0;

void c2c_bw_options_init(c2c_bw_options_t *opts)
{
	if (!opts)
		return;
	memset(opts, 0, sizeof(*opts));
	opts->engine = C2C_ENGINE_READERBW;
	opts->time_sec = 3;
	opts->simd_level = 2;
	opts->rw_mix = 0;
	opts->ngroups = 1;
	opts->skip_kb = -1;
}

static inline uint64_t rdtsc_start(void)
{
	_mm_lfence();
	return __rdtsc();
}

static inline uint64_t rdtscp_end(void)
{
	unsigned int aux = 0;
	uint64_t t = __rdtscp(&aux);
	_mm_lfence();
	return t;
}

static double get_ns_per_tsc(void)
{
	double ghz = helper_get_tsc_freq_ghz();
	return (ghz > 0.0) ? (1.0 / ghz) : 0.5;
}

static double bytes_tsc_to_gibs(uint64_t bytes, uint64_t tsc, double ns_per_tsc)
{
	if (bytes == 0 || tsc == 0)
		return 0.0;
	double sec = (double)tsc * ns_per_tsc * 1e-9;
	if (sec <= 0.0)
		return 0.0;
	return ((double)bytes / GIB_BYTES_DBL) / sec;
}

static inline void delay_loop(int delay)
{
	if (delay <= 0)
		return;
	for (volatile int i = 0; i < delay; i++) {
		__asm__ volatile ("" ::: "memory");
	}
}

/* Default buffer size (KB) matching intel/mlc FUN_001302f3: max(4 MiB, (L3/2) & ~4 MiB) */
static int64_t c2c_default_buf_kb(void)
{
	const cpu_cache_info_t *ci = cpu_get_cache_info();
	int64_t l3 = (ci && ci->l3_bytes > 0) ? (int64_t)ci->l3_bytes
					      : (64LL << 20);
	int64_t bytes = (l3 / 2) & ~0x3fffffLL;
	if (bytes < (4LL << 20))
		bytes = (4LL << 20);
	return bytes >> 10;
}

/* Default chunk size (KB) for pipelined ring matching FUN_0013035d: max(64 KB, (buf_kb/8) & ~63) */
static int64_t c2c_default_chunk_kb(int64_t buf_kb)
{
	int64_t ck = (buf_kb / 8) & ~63LL;
	return (ck < 64) ? 64 : ck;
}

/* Matrix 1 / chunk size (bytes) matching FUN_0013b90d: max(4 MiB, (int64_t)(L3 * 0.4) & ~4 MiB) */
static int64_t c2c_matrix1_bytes(void)
{
	const cpu_cache_info_t *ci = cpu_get_cache_info();
	int64_t l3 = (ci && ci->l3_bytes > 0) ? (int64_t)ci->l3_bytes
					      : (64LL << 20);
	int64_t bytes = ((int64_t)((double)l3 * 0.4)) & ~0x3fffffLL;
	if (bytes < (4LL << 20))
		bytes = (4LL << 20);
	return bytes;
}

size_t c2c_parse_cpulist(const char *cpustr, int *out_cpus, size_t max_cpus)
{
	if (!cpustr || !out_cpus || max_cpus == 0)
		return 0;
	size_t count = 0;
	char buf[4096];
	strncpy(buf, cpustr, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	char *saveptr = NULL;
	char *token = strtok_r(buf, ",", &saveptr);
	while (token) {
		while (*token && isspace((unsigned char)*token))
			token++;
		if (*token == '\0') {
			token = strtok_r(NULL, ",", &saveptr);
			continue;
		}
		char *dash = strchr(token, '-');
		if (dash) {
			*dash = '\0';
			char *e1 = NULL, *e2 = NULL;
			long s = strtol(token, &e1, 10);
			long e = strtol(dash + 1, &e2, 10);
			if (e1 == token || e2 == dash + 1 || s > e) {
				out_cpus[0] = -1;
				return 1;
			}
			for (long c = s; c <= e; c++) {
				if (count >= max_cpus) {
					fprintf(stderr,
						"c2c_bandwidth: cpulist has >%zu cpus, max %zu\n",
						max_cpus, max_cpus);
					return count;
				}
				out_cpus[count++] = (int)c;
			}
		} else {
			char *ep = NULL;
			long c = strtol(token, &ep, 10);
			if (ep == token) {
				out_cpus[0] = -1;
				return 1;
			}
			if (count >= max_cpus) {
				fprintf(stderr,
					"c2c_bandwidth: cpulist has >%zu cpus, max %zu\n",
					max_cpus, max_cpus);
				return count;
			}
			out_cpus[count++] = (int)c;
		}
		token = strtok_r(NULL, ",", &saveptr);
	}
	return count;
}

static int split_pipe_groups(const char *str, char groups[][1024], int max_groups)
{
	if (!str || *str == '\0')
		return 0;
	int n = 0;
	const char *p = str;
	while (n < max_groups) {
		const char *bar = strchr(p, '|');
		size_t len = bar ? (size_t)(bar - p) : strlen(p);
		if (len >= 1024)
			len = 1023;
		memcpy(groups[n], p, len);
		groups[n][len] = '\0';
		n++;
		if (!bar)
			break;
		p = bar + 1;
	}
	return n;
}

/* =========================================================================
 * SIMD Reader & Writer Kernels (AVX-512, AVX2, SSE2/scalar)
 * Matching intel/mlc 0x303c3 .. 0x31690
 * ========================================================================= */

__attribute__((target("avx512f")))
static void reader_read_100pct_avx512(const uint8_t *addr, size_t len, int delay)
{
	__m512i v0 = _mm512_setzero_si512();
	__m512i v1 = _mm512_setzero_si512();
	__m512i v2 = _mm512_setzero_si512();
	__m512i v3 = _mm512_setzero_si512();
	__m512i v4 = _mm512_setzero_si512();
	__m512i v5 = _mm512_setzero_si512();
	__m512i v6 = _mm512_setzero_si512();
	__m512i v7 = _mm512_setzero_si512();
	size_t off = 0;

	for (; off + 512 <= len; off += 512) {
		const __m512i *p = (const __m512i *)(addr + off);
		v0 = _mm512_load_si512(p + 0);
		v1 = _mm512_load_si512(p + 1);
		v2 = _mm512_load_si512(p + 2);
		v3 = _mm512_load_si512(p + 3);
		v4 = _mm512_load_si512(p + 4);
		v5 = _mm512_load_si512(p + 5);
		v6 = _mm512_load_si512(p + 6);
		v7 = _mm512_load_si512(p + 7);
		__asm__ volatile(""
				 : "+v"(v0), "+v"(v1), "+v"(v2), "+v"(v3),
				   "+v"(v4), "+v"(v5), "+v"(v6), "+v"(v7));
		if (delay > 0)
			delay_loop(delay);
	}
	for (; off + 64 <= len; off += 64) {
		const __m512i *p = (const __m512i *)(addr + off);
		v0 = _mm512_load_si512(p);
		__asm__ volatile("" : "+v"(v0));
	}
}

static void reader_read_100pct_avx2(const uint8_t *addr, size_t len, int delay)
{
	__m256i v0 = _mm256_setzero_si256();
	__m256i v1 = _mm256_setzero_si256();
	__m256i v2 = _mm256_setzero_si256();
	__m256i v3 = _mm256_setzero_si256();
	size_t off = 0;

	for (; off + 128 <= len; off += 128) {
		const __m256i *p = (const __m256i *)(addr + off);
		v0 = _mm256_load_si256(p + 0);
		v1 = _mm256_load_si256(p + 1);
		v2 = _mm256_load_si256(p + 2);
		v3 = _mm256_load_si256(p + 3);
		__asm__ volatile("" : "+x"(v0), "+x"(v1), "+x"(v2), "+x"(v3));
		if (delay > 0)
			delay_loop(delay);
	}
	for (; off + 32 <= len; off += 32) {
		const __m256i *p = (const __m256i *)(addr + off);
		v0 = _mm256_load_si256(p);
		__asm__ volatile("" : "+x"(v0));
	}
}

static void reader_read_100pct_scalar(const uint8_t *addr, size_t len, int delay)
{
	uint64_t sum = 0;
	size_t nlines = len >> 6;
	for (size_t i = 0; i < nlines; i++) {
		sum += *(const volatile uint64_t *)(addr + (i << 6));
		if (delay > 0 && (i & 7U) == 7U)
			delay_loop(delay);
	}
	if (sum == 0xdeadbeefcafebabeULL)
		g_c2c_sink = sum;
}

static void reader_read_100pct(const uint8_t *addr, size_t len, int simd, int delay)
{
	if (simd >= 2 && (((uintptr_t)addr & 63U) == 0)) {
		reader_read_100pct_avx512(addr, len, delay);
	} else if (simd >= 1 && (((uintptr_t)addr & 31U) == 0)) {
		reader_read_100pct_avx2(addr, len, delay);
	} else {
		reader_read_100pct_scalar(addr, len, delay);
	}
}

static void map_rw_mix(int rw_mix, int *r_lines, int *w_lines)
{
	switch (rw_mix) {
	case 2:
		*r_lines = 1;
		*w_lines = 1;
		break;
	case 3:
		*r_lines = 2;
		*w_lines = 1;
		break;
	case 5:
		*r_lines = 0;
		*w_lines = 1;
		break;
	case 6:
		*r_lines = 3;
		*w_lines = 1;
		break;
	default:
		*r_lines = 1;
		*w_lines = 0;
		break;
	}
}

__attribute__((target("avx512f")))
static uint64_t reader_rw_avx512(uint8_t *addr, size_t nlines, int r_lines,
				 int w_lines, uint64_t epoch, int delay)
{
	__m512i acc = _mm512_setzero_si512();
	__m512i wval = _mm512_set1_epi64((long long)epoch);
	size_t idx = 0;
	uint64_t touched = 0;
	int step = r_lines + w_lines;

	while (idx + (size_t)step <= nlines) {
		for (int r = 0; r < r_lines; r++, idx++) {
			acc = _mm512_add_epi64(
			    acc, _mm512_load_si512((const __m512i *)(addr + (idx << 6))));
			touched++;
		}
		for (int w = 0; w < w_lines; w++, idx++) {
			_mm512_store_si512((__m512i *)(addr + (idx << 6)), wval);
			touched++;
		}
		if (delay > 0)
			delay_loop(delay);
	}
	uint64_t tmp[8] __attribute__((aligned(64)));
	_mm512_store_si512((__m512i *)tmp, acc);
	if (tmp[0] == 0xdeadbeefcafebabeULL)
		g_c2c_sink = tmp[0];
	return touched << 6;
}

static uint64_t reader_rw_avx2(uint8_t *addr, size_t nlines, int r_lines,
			       int w_lines, uint64_t epoch, int delay)
{
	__m256i acc = _mm256_setzero_si256();
	__m256i wval = _mm256_set1_epi64x((long long)epoch);
	size_t idx = 0;
	uint64_t touched = 0;
	int step = r_lines + w_lines;

	while (idx + (size_t)step <= nlines) {
		for (int r = 0; r < r_lines; r++, idx++) {
			const __m256i *p = (const __m256i *)(addr + (idx << 6));
			acc = _mm256_add_epi64(acc, _mm256_load_si256(p + 0));
			acc = _mm256_add_epi64(acc, _mm256_load_si256(p + 1));
			touched++;
		}
		for (int w = 0; w < w_lines; w++, idx++) {
			__m256i *p = (__m256i *)(addr + (idx << 6));
			_mm256_store_si256(p + 0, wval);
			_mm256_store_si256(p + 1, wval);
			touched++;
		}
		if (delay > 0)
			delay_loop(delay);
	}
	uint64_t tmp[4] __attribute__((aligned(32)));
	_mm256_store_si256((__m256i *)tmp, acc);
	if (tmp[0] == 0xdeadbeefcafebabeULL)
		g_c2c_sink = tmp[0];
	return touched << 6;
}

static uint64_t reader_rw_scalar(uint8_t *addr, size_t nlines, int r_lines,
				 int w_lines, uint64_t epoch, int delay)
{
	uint64_t sum = 0;
	size_t idx = 0;
	uint64_t touched = 0;
	int step = r_lines + w_lines;

	while (idx + (size_t)step <= nlines) {
		for (int r = 0; r < r_lines; r++, idx++) {
			sum += *(const volatile uint64_t *)(addr + (idx << 6));
			touched++;
		}
		for (int w = 0; w < w_lines; w++, idx++) {
			uint64_t *q = (uint64_t *)(addr + (idx << 6));
			for (int k = 0; k < 8; k++)
				q[k] = epoch;
			touched++;
		}
		if (delay > 0)
			delay_loop(delay);
	}
	if (sum == 0xdeadbeefcafebabeULL)
		g_c2c_sink = sum;
	return touched << 6;
}

static uint64_t reader_rw_kernel(uint8_t *addr, size_t len, int simd,
				 int rw_mix, uint64_t epoch, int delay)
{
	if (rw_mix == 0) {
		reader_read_100pct(addr, len, simd, delay);
		return (uint64_t)len;
	}
	int r_lines = 1, w_lines = 0;
	map_rw_mix(rw_mix, &r_lines, &w_lines);
	size_t nlines = len >> 6;
	if (simd >= 2 && (((uintptr_t)addr & 63U) == 0))
		return reader_rw_avx512(addr, nlines, r_lines, w_lines, epoch,
					delay);
	if (simd >= 1 && (((uintptr_t)addr & 31U) == 0))
		return reader_rw_avx2(addr, nlines, r_lines, w_lines, epoch,
				      delay);
	return reader_rw_scalar(addr, nlines, r_lines, w_lines, epoch, delay);
}

__attribute__((target("avx512f")))
static void writer_install_avx512(uint8_t *addr, size_t nlines, bool hit_clean,
				  uint64_t seq)
{
	if (!hit_clean) {
		__m512i v = _mm512_set1_epi64((long long)seq);
		for (ssize_t i = (ssize_t)nlines - 1; i >= 0; i--) {
			_mm512_store_si512((__m512i *)(addr + ((size_t)i << 6)), v);
		}
	} else {
		__m512i acc = _mm512_setzero_si512();
		for (ssize_t i = (ssize_t)nlines - 1; i >= 0; i--) {
			acc = _mm512_add_epi64(
			    acc, _mm512_load_si512(
				     (const __m512i *)(addr + ((size_t)i << 6))));
		}
		uint64_t tmp[8] __attribute__((aligned(64)));
		_mm512_store_si512((__m512i *)tmp, acc);
		if (tmp[0] == 0xdeadbeefcafebabeULL)
			g_c2c_sink = tmp[0];
	}
}

static void writer_install_avx2(uint8_t *addr, size_t nlines, bool hit_clean,
				uint64_t seq)
{
	if (!hit_clean) {
		__m256i v = _mm256_set1_epi64x((long long)seq);
		for (ssize_t i = (ssize_t)nlines - 1; i >= 0; i--) {
			__m256i *p = (__m256i *)(addr + ((size_t)i << 6));
			_mm256_store_si256(p + 0, v);
			_mm256_store_si256(p + 1, v);
		}
	} else {
		__m256i acc = _mm256_setzero_si256();
		for (ssize_t i = (ssize_t)nlines - 1; i >= 0; i--) {
			const __m256i *p =
			    (const __m256i *)(addr + ((size_t)i << 6));
			acc = _mm256_add_epi64(acc, _mm256_load_si256(p + 0));
			acc = _mm256_add_epi64(acc, _mm256_load_si256(p + 1));
		}
		uint64_t tmp[4] __attribute__((aligned(32)));
		_mm256_store_si256((__m256i *)tmp, acc);
		if (tmp[0] == 0xdeadbeefcafebabeULL)
			g_c2c_sink = tmp[0];
	}
}

/* Backwards cache-line installer + mfence matching FUN_001314ab */
static void writer_install(uint8_t *addr, size_t len, bool hit_clean,
			   bool partial, uint64_t seq, int simd)
{
	size_t nlines = len >> 6;
	if (nlines == 0)
		return;

	if (!hit_clean && partial) {
		for (ssize_t i = (ssize_t)nlines - 1; i >= 0; i--) {
			*(uint64_t *)(addr + ((size_t)i << 6)) =
			    (uint64_t)i + seq;
		}
	} else if (simd >= 2 && (((uintptr_t)addr & 63U) == 0)) {
		writer_install_avx512(addr, nlines, hit_clean, seq);
	} else if (simd >= 1 && (((uintptr_t)addr & 31U) == 0)) {
		writer_install_avx2(addr, nlines, hit_clean, seq);
	} else if (!hit_clean) {
		for (ssize_t i = (ssize_t)nlines - 1; i >= 0; i--) {
			uint64_t *q = (uint64_t *)(addr + ((size_t)i << 6));
			uint64_t val = (uint64_t)i ^ seq;
			for (int k = 0; k < 8; k++)
				q[k] = val;
		}
	} else {
		uint64_t sum = 0;
		for (ssize_t i = (ssize_t)nlines - 1; i >= 0; i--) {
			sum += *(const volatile uint64_t *)(addr +
							    ((size_t)i << 6));
		}
		if (sum == 0xdeadbeefcafebabeULL)
			g_c2c_sink = sum;
	}
	_mm_mfence();
}

static const char *simd_reader_label(int simd)
{
	if (simd >= 2)
		return "AVX-512 load";
	if (simd == 1)
		return "AVX2 load";
	return "SSE2/scalar load";
}

static const char *simd_short_label(int simd)
{
	if (simd >= 2)
		return "AVX-512";
	if (simd == 1)
		return "AVX2";
	return "SSE2";
}

static const char *rw_mix_label(int rw_mix, bool pipelined_banner)
{
	switch (rw_mix) {
	case 2:
		return "1:1 R:W (-W2)";
	case 3:
		return "2:1 R:W (-W3)";
	case 5:
		return pipelined_banner ? "100% NT writes (-W5)"
					: "100% writes (-W5)";
	case 6:
		return "3:1 R:W (-W6)";
	default:
		return pipelined_banner ? "100% reads" : "100% reads (default)";
	}
}

/* =========================================================================
 * Engine 1 & 2 Lockless Epoch Coordinator (c2c_readerbw / c2c_bandwidth_matrix)
 * Matching FUN_00134068 .. FUN_00134c25 (single-pair) and FUN_00137915 (multi-group)
 * ========================================================================= */

typedef struct __attribute__((aligned(64))) {
	uint64_t val;
	uint8_t pad[56];
} cacheline_u64_t;

typedef struct __attribute__((aligned(64))) {
	cacheline_u64_t w_target;
	cacheline_u64_t r_target;
	cacheline_u64_t s_target;
	cacheline_u64_t w_done;
	cacheline_u64_t s_done;
	cacheline_u64_t stop;
	cacheline_u64_t win_base_offset;
} c2crd_ctrl_t;

typedef struct {
	c2crd_ctrl_t *ctrl;
	cacheline_u64_t *r_done;
	uint8_t *group_buf;
	size_t buf_bytes;
	size_t chunk_bytes;
	size_t skip_bytes;
	size_t slice_off;
	size_t slice_bytes;
	int cpu;
	int group_id;
	int global_reader_idx;
	int total_nw;
	int total_nr;
	int total_ns;
	int simd_level;
	int rw_mix;
	int delay;
	bool hit_clean;
	bool partial;
	uint64_t iters;
	uint64_t total_bytes;
	uint64_t work_tsc;
} c2crd_worker_t;

static inline void spin_pause_n(int n)
{
	for (int i = 0; i < n; i++)
		_mm_pause();
}

static void *c2crd_writer_fn(void *arg)
{
	c2crd_worker_t *w = (c2crd_worker_t *)arg;
	numa_pin_cpu(w->cpu);
	c2crd_ctrl_t *ctrl = w->ctrl;
	uint64_t ep = 0;

	while (!__atomic_load_n(&ctrl->stop.val, __ATOMIC_ACQUIRE)) {
		while (__atomic_load_n(&ctrl->w_target.val, __ATOMIC_ACQUIRE) <= ep) {
			if (__atomic_load_n(&ctrl->stop.val, __ATOMIC_ACQUIRE))
				return NULL;
			spin_pause_n(16);
		}
		if (__atomic_load_n(&ctrl->stop.val, __ATOMIC_ACQUIRE))
			break;

		size_t base = (size_t)__atomic_load_n(&ctrl->win_base_offset.val,
						      __ATOMIC_ACQUIRE);
		if (w->slice_bytes > 0) {
			writer_install(w->group_buf + base + w->slice_off,
				       w->slice_bytes, w->hit_clean, w->partial,
				       ep + 1ULL, w->simd_level);
		}
		ep++;
		w->iters = ep;
		_mm_mfence();

		uint64_t prev = __atomic_fetch_add(&ctrl->w_done.val, 1ULL,
						   __ATOMIC_ACQ_REL);
		if ((int)(prev + 1ULL) == w->total_nw) {
			__atomic_store_n(&ctrl->w_done.val, 0ULL,
					 __ATOMIC_RELEASE);
			_mm_mfence();
			if (w->total_ns > 0) {
				__atomic_fetch_add(&ctrl->s_target.val, 1ULL,
						   __ATOMIC_ACQ_REL);
			} else {
				__atomic_fetch_add(&ctrl->r_target.val, 1ULL,
						   __ATOMIC_ACQ_REL);
			}
		}
	}
	return NULL;
}

static void *c2crd_sharer_fn(void *arg)
{
	c2crd_worker_t *s = (c2crd_worker_t *)arg;
	numa_pin_cpu(s->cpu);
	c2crd_ctrl_t *ctrl = s->ctrl;
	uint64_t ep = 0;

	while (!__atomic_load_n(&ctrl->stop.val, __ATOMIC_ACQUIRE)) {
		while (__atomic_load_n(&ctrl->s_target.val, __ATOMIC_ACQUIRE) <= ep) {
			if (__atomic_load_n(&ctrl->stop.val, __ATOMIC_ACQUIRE))
				return NULL;
			spin_pause_n(16);
		}
		if (__atomic_load_n(&ctrl->stop.val, __ATOMIC_ACQUIRE))
			break;

		size_t base = (size_t)__atomic_load_n(&ctrl->win_base_offset.val,
						      __ATOMIC_ACQUIRE);
		reader_read_100pct(s->group_buf + base, s->chunk_bytes,
				   s->simd_level, 0);
		ep++;
		s->iters = ep;
		_mm_mfence();

		uint64_t prev = __atomic_fetch_add(&ctrl->s_done.val, 1ULL,
						   __ATOMIC_ACQ_REL);
		if ((int)(prev + 1ULL) == s->total_ns) {
			__atomic_store_n(&ctrl->s_done.val, 0ULL,
					 __ATOMIC_RELEASE);
			_mm_mfence();
			__atomic_fetch_add(&ctrl->r_target.val, 1ULL,
					   __ATOMIC_ACQ_REL);
		}
	}
	return NULL;
}

static void *c2crd_follower_reader_fn(void *arg)
{
	c2crd_worker_t *r = (c2crd_worker_t *)arg;
	numa_pin_cpu(r->cpu);
	c2crd_ctrl_t *ctrl = r->ctrl;
	uint64_t ep = 0;

	while (!__atomic_load_n(&ctrl->stop.val, __ATOMIC_ACQUIRE)) {
		while (__atomic_load_n(&ctrl->r_target.val, __ATOMIC_ACQUIRE) <= ep) {
			if (__atomic_load_n(&ctrl->stop.val, __ATOMIC_ACQUIRE))
				return NULL;
			spin_pause_n(16);
		}
		if (__atomic_load_n(&ctrl->stop.val, __ATOMIC_ACQUIRE))
			break;

		size_t base = (size_t)__atomic_load_n(&ctrl->win_base_offset.val,
						      __ATOMIC_ACQUIRE);
		if (r->slice_bytes > 0) {
			uint64_t t0 = rdtsc_start();
			uint64_t b = reader_rw_kernel(
			    r->group_buf + base + r->slice_off, r->slice_bytes,
			    r->simd_level, r->rw_mix, ep + 1ULL, r->delay);
			uint64_t t1 = rdtscp_end();
			r->work_tsc += (t1 - t0);
			r->total_bytes += b;
		}
		ep++;
		r->iters = ep;
		__atomic_store_n(&r->r_done[r->global_reader_idx].val, ep,
				 __ATOMIC_RELEASE);
	}
	return NULL;
}

static void *c2crd_lead_reader_fn(void *arg)
{
	c2crd_worker_t *r = (c2crd_worker_t *)arg;
	numa_pin_cpu(r->cpu);
	c2crd_ctrl_t *ctrl = r->ctrl;
	uint64_t ep = 0;

	while (!__atomic_load_n(&ctrl->stop.val, __ATOMIC_ACQUIRE)) {
		while (__atomic_load_n(&ctrl->r_target.val, __ATOMIC_ACQUIRE) <= ep) {
			if (__atomic_load_n(&ctrl->stop.val, __ATOMIC_ACQUIRE))
				return NULL;
			spin_pause_n(16);
		}
		if (__atomic_load_n(&ctrl->stop.val, __ATOMIC_ACQUIRE))
			break;

		size_t base = (size_t)__atomic_load_n(&ctrl->win_base_offset.val,
						      __ATOMIC_ACQUIRE);
		if (r->slice_bytes > 0) {
			uint64_t t0 = rdtsc_start();
			uint64_t b = reader_rw_kernel(
			    r->group_buf + base + r->slice_off, r->slice_bytes,
			    r->simd_level, r->rw_mix, ep + 1ULL, r->delay);
			uint64_t t1 = rdtscp_end();
			r->work_tsc += (t1 - t0);
			r->total_bytes += b;
		}
		ep++;
		r->iters = ep;
		__atomic_store_n(&r->r_done[0].val, ep, __ATOMIC_RELEASE);

		for (int k = 1; k < r->total_nr; k++) {
			while (__atomic_load_n(&r->r_done[k].val,
					       __ATOMIC_ACQUIRE) < ep) {
				if (__atomic_load_n(&ctrl->stop.val,
						    __ATOMIC_ACQUIRE))
					return NULL;
				spin_pause_n(8);
			}
		}

		if (r->chunk_bytes < r->buf_bytes) {
			size_t next = base + r->skip_bytes;
			if (next + r->chunk_bytes > r->buf_bytes)
				next = 0;
			__atomic_store_n(&ctrl->win_base_offset.val,
					 (uint64_t)next, __ATOMIC_RELEASE);
		}
		_mm_mfence();
		__atomic_fetch_add(&ctrl->w_target.val, 1ULL, __ATOMIC_ACQ_REL);
	}
	return NULL;
}

typedef struct {
	int nw;
	int nr;
	int ns;
	int numa_id;
	const int *w_cpus;
	const int *r_cpus;
	const int *s_cpus;
} c2crd_group_spec_t;

static double c2crd_execute_groups(const c2crd_group_spec_t *groups,
				   int ngroups, size_t buf_bytes,
				   size_t chunk_bytes, size_t skip_bytes,
				   const c2c_bw_options_t *opts,
				   bool multi_banner)
{
	int total_nw = 0, total_nr = 0, total_ns = 0;
	for (int g = 0; g < ngroups; g++) {
		total_nw += groups[g].nw;
		total_nr += groups[g].nr;
		total_ns += groups[g].ns;
	}
	if (total_nw <= 0 || total_nr <= 0) {
		if (multi_banner) {
			fprintf(stderr,
				"c2crd_run_multi: need at least one writer and one reader across all groups\n");
		}
		return 0.0;
	}

	buf_bytes = (buf_bytes + 63ULL) & ~63ULL;
	chunk_bytes = (chunk_bytes + 63ULL) & ~63ULL;
	if (chunk_bytes == 0 || chunk_bytes > buf_bytes)
		chunk_bytes = buf_bytes;
	skip_bytes = (skip_bytes + 63ULL) & ~63ULL;
	if (skip_bytes == 0)
		skip_bytes = chunk_bytes;

	uint8_t *g_bufs[MAX_C2C_GROUPS] = { NULL };
	hugepage_type_t hp_mode =
	    (numa_get_hugepage_mode() == HUGEPAGE_1GB) ? HUGEPAGE_1GB
						       : HUGEPAGE_2MB;
	size_t hp_align =
	    (hp_mode == HUGEPAGE_1GB) ? (1ULL << 30) : (2ULL << 20);
	size_t map_bytes = (buf_bytes + hp_align - 1ULL) & ~(hp_align - 1ULL);
	for (int g = 0; g < ngroups; g++) {
		g_bufs[g] = (uint8_t *)alloc_mem_onnode(
		    map_bytes, groups[g].numa_id, hp_mode);
		if (!g_bufs[g]) {
			fprintf(stderr,
				"%s: alloc %lld bytes on numa %d failed\n",
				multi_banner ? "c2crd_run_multi"
					     : "c2c_readerbw",
				(long long)buf_bytes, groups[g].numa_id);
			for (int k = 0; k < g; k++)
				free_mem_onnode(g_bufs[k], map_bytes);
			return 0.0;
		}
		/* Pre-stage initial state across entire buffer if -H Shared install */
		if (opts->hit_clean) {
			writer_install(g_bufs[g], chunk_bytes, true, false, 0,
				       opts->simd_level);
		}
	}

	c2crd_ctrl_t *ctrl = NULL;
	cacheline_u64_t *r_done = NULL;
	if (posix_memalign((void **)&ctrl, 64, sizeof(*ctrl)) != 0 ||
	    posix_memalign((void **)&r_done, 64,
			   (size_t)total_nr * sizeof(cacheline_u64_t)) != 0) {
		free(ctrl);
		free(r_done);
		for (int g = 0; g < ngroups; g++)
			free_mem_onnode(g_bufs[g], map_bytes);
		return 0.0;
	}
	memset(ctrl, 0, sizeof(*ctrl));
	memset(r_done, 0, (size_t)total_nr * sizeof(cacheline_u64_t));
	ctrl->w_target.val = 1ULL;

	int total_threads = total_nw + total_ns + total_nr;
	c2crd_worker_t *workers =
	    (c2crd_worker_t *)calloc((size_t)total_threads, sizeof(*workers));
	pthread_t *tids =
	    (pthread_t *)calloc((size_t)total_threads, sizeof(*tids));
	if (!workers || !tids) {
		free(workers);
		free(tids);
		free(ctrl);
		free(r_done);
		for (int g = 0; g < ngroups; g++)
			free_mem_onnode(g_bufs[g], map_bytes);
		return 0.0;
	}

	if (opts->verbose) {
		if (multi_banner) {
			printf("[c2crd_multi] ngroups=%d total_w=%d total_r=%d total_s=%d\n",
			       ngroups, total_nw, total_nr, total_ns);
			for (int g = 0; g < ngroups; g++) {
				printf("[c2crd_multi]   g%d numa=%d alloc=%lld MiB nw=%d nr=%d ns=%d  W={",
				       g, groups[g].numa_id,
				       (long long)(buf_bytes >> 20),
				       groups[g].nw, groups[g].nr,
				       groups[g].ns);
				for (int i = 0; i < groups[g].nw; i++)
					printf("%s%d", i ? "," : "",
					       groups[g].w_cpus[i]);
				printf("} R={");
				for (int i = 0; i < groups[g].nr; i++)
					printf("%s%d", i ? "," : "",
					       groups[g].r_cpus[i]);
				printf("}");
				if (groups[g].ns > 0) {
					printf(" S={");
					for (int i = 0; i < groups[g].ns; i++)
						printf("%s%d", i ? "," : "",
						       groups[g].s_cpus[i]);
					printf("}");
				}
				printf("\n");
			}
		} else {
			size_t r_slice =
			    (groups[0].nr > 0)
				? ((chunk_bytes / (size_t)groups[0].nr) & ~63ULL)
				: chunk_bytes;
			printf("[c2crd] buf=%p alloc=%lld  reader_slice=%lld (%.2f MiB)  n_w=%d n_r=%d n_s=%d numa=%d\n",
			       (void *)g_bufs[0], (long long)buf_bytes,
			       (long long)r_slice,
			       (double)r_slice / (1024.0 * 1024.0), total_nw,
			       total_nr, total_ns, groups[0].numa_id);
		}
	}

	int w_idx = 0;
	int s_base = total_nw;
	int r_base = total_nw + total_ns;
	int s_idx = 0;
	int r_idx = 0;

	for (int g = 0; g < ngroups; g++) {
		size_t w_slice_base =
		    (groups[g].nw > 0)
			? ((chunk_bytes / (size_t)groups[g].nw) & ~63ULL)
			: 0;
		for (int i = 0; i < groups[g].nw; i++) {
			c2crd_worker_t *wk = &workers[w_idx++];
			wk->ctrl = ctrl;
			wk->r_done = r_done;
			wk->group_buf = g_bufs[g];
			wk->buf_bytes = buf_bytes;
			wk->chunk_bytes = chunk_bytes;
			wk->skip_bytes = skip_bytes;
			wk->slice_off = (size_t)i * w_slice_base;
			wk->slice_bytes =
			    (i == groups[g].nw - 1)
				? (chunk_bytes - wk->slice_off)
				: w_slice_base;
			wk->cpu = groups[g].w_cpus[i];
			wk->group_id = g;
			wk->total_nw = total_nw;
			wk->total_nr = total_nr;
			wk->total_ns = total_ns;
			wk->simd_level = opts->simd_level;
			wk->rw_mix = opts->rw_mix;
			wk->delay = opts->delay;
			wk->hit_clean = opts->hit_clean;
			wk->partial = opts->partial;
		}

		for (int i = 0; i < groups[g].ns; i++) {
			c2crd_worker_t *sk = &workers[s_base + s_idx++];
			sk->ctrl = ctrl;
			sk->r_done = r_done;
			sk->group_buf = g_bufs[g];
			sk->buf_bytes = buf_bytes;
			sk->chunk_bytes = chunk_bytes;
			sk->skip_bytes = skip_bytes;
			sk->slice_off = 0;
			sk->slice_bytes = chunk_bytes;
			sk->cpu = groups[g].s_cpus[i];
			sk->group_id = g;
			sk->total_nw = total_nw;
			sk->total_nr = total_nr;
			sk->total_ns = total_ns;
			sk->simd_level = opts->simd_level;
		}

		size_t r_slice_base =
		    (groups[g].nr > 0)
			? ((chunk_bytes / (size_t)groups[g].nr) & ~63ULL)
			: 0;
		for (int i = 0; i < groups[g].nr; i++) {
			int gri = r_idx++;
			c2crd_worker_t *rk = &workers[r_base + gri];
			rk->ctrl = ctrl;
			rk->r_done = r_done;
			rk->group_buf = g_bufs[g];
			rk->buf_bytes = buf_bytes;
			rk->chunk_bytes = chunk_bytes;
			rk->skip_bytes = skip_bytes;
			rk->slice_off = (size_t)i * r_slice_base;
			rk->slice_bytes =
			    (i == groups[g].nr - 1)
				? (chunk_bytes - rk->slice_off)
				: r_slice_base;
			rk->cpu = groups[g].r_cpus[i];
			rk->group_id = g;
			rk->global_reader_idx = gri;
			rk->total_nw = total_nw;
			rk->total_nr = total_nr;
			rk->total_ns = total_ns;
			rk->simd_level = opts->simd_level;
			rk->rw_mix = opts->rw_mix;
			rk->delay = opts->delay;
		}
	}

	if (opts->verbose && !multi_banner) {
		for (int i = 0; i < total_nw; i++) {
			printf("[c2crd]   W cpu=%3d off=%lld bytes=%lld\n",
			       workers[i].cpu, (long long)workers[i].slice_off,
			       (long long)workers[i].slice_bytes);
		}
		for (int i = 0; i < total_nr; i++) {
			printf("[c2crd]   R cpu=%3d off=%lld bytes=%lld\n",
			       workers[r_base + i].cpu,
			       (long long)workers[r_base + i].slice_off,
			       (long long)workers[r_base + i].slice_bytes);
		}
		for (int i = 0; i < total_ns; i++) {
			printf("[c2crd]   S cpu=%3d off=%lld bytes=%lld\n",
			       workers[s_base + i].cpu,
			       (long long)workers[s_base + i].slice_off,
			       (long long)workers[s_base + i].slice_bytes);
		}
	}

	uint64_t wall_t0 = rdtsc_start();

	int created = 0;
	for (int i = 0; i < total_nw; i++, created++) {
		pthread_create(&tids[created], NULL, c2crd_writer_fn,
			       &workers[created]);
	}
	for (int i = 0; i < total_ns; i++, created++) {
		pthread_create(&tids[created], NULL, c2crd_sharer_fn,
			       &workers[created]);
	}
	for (int i = 0; i < total_nr; i++, created++) {
		pthread_create(&tids[created], NULL,
			       (i == 0) ? c2crd_lead_reader_fn
					: c2crd_follower_reader_fn,
			       &workers[created]);
	}

	int dur = (opts->time_sec > 0) ? opts->time_sec : 3;
	struct timespec req = { .tv_sec = dur, .tv_nsec = 0 };
	nanosleep(&req, NULL);

	__atomic_store_n(&ctrl->stop.val, 1ULL, __ATOMIC_SEQ_CST);
	for (int i = 0; i < created; i++)
		pthread_join(tids[i], NULL);

	uint64_t wall_t1 = rdtscp_end();
	double ns_per_tsc = get_ns_per_tsc();
	double wall_sec = (double)(wall_t1 - wall_t0) * ns_per_tsc * 1e-9;

	double total_bw = 0.0;
	double group_bw[MAX_C2C_GROUPS] = { 0.0 };
	uint64_t sum_bytes = 0;
	uint64_t max_r_tsc = 0;
	uint64_t sum_iters = 0;

	for (int i = 0; i < total_nr; i++) {
		const c2crd_worker_t *rk = &workers[r_base + i];
		double rbw =
		    bytes_tsc_to_gibs(rk->total_bytes, rk->work_tsc, ns_per_tsc);
		total_bw += rbw;
		if (rk->group_id >= 0 && rk->group_id < MAX_C2C_GROUPS)
			group_bw[rk->group_id] += rbw;
		sum_bytes += rk->total_bytes;
		sum_iters += rk->iters;
		if (rk->work_tsc > max_r_tsc)
			max_r_tsc = rk->work_tsc;
	}

	if (multi_banner) {
		printf("[c2crd_multi] wall=%.3f s  total_bytes=%.2f MiB\n",
		       wall_sec, (double)sum_bytes / (1024.0 * 1024.0));
		for (int g = 0; g < ngroups; g++) {
			printf("  group %d (numa %d): %.2f GiB/s\n", g,
			       groups[g].numa_id, group_bw[g]);
		}
		printf("[c2crd_multi] AGGREGATE BW = %.2f GiB/s\n", total_bw);
	} else if (opts->verbose) {
		double max_active = (double)max_r_tsc * ns_per_tsc * 1e-9;
		double pct =
		    (wall_sec > 0.0) ? (100.0 * max_active / wall_sec) : 0.0;
		printf("[c2crd] wall=%.3f s  max_reader_work=%.3f s  active/wall=%.1f%%  total_bytes=%.2f MiB  avg_iters/reader=%.0f  BW=%.2f GB/s\n",
		       wall_sec, max_active, pct,
		       (double)sum_bytes / (1024.0 * 1024.0),
		       (double)sum_iters / (double)total_nr, total_bw);
		for (int i = 0; i < total_nr; i++) {
			const c2crd_worker_t *rk = &workers[r_base + i];
			double rbw = bytes_tsc_to_gibs(
			    rk->total_bytes, rk->work_tsc, ns_per_tsc);
			printf("[c2crd]   R cpu=%3d iters=%lu slice=%.2f MiB work=%.2f ms  BW=%.2f GB/s\n",
			       rk->cpu, (unsigned long)rk->iters,
			       (double)rk->slice_bytes / (1024.0 * 1024.0),
			       (double)rk->work_tsc * ns_per_tsc * 1e-6, rbw);
		}
	}

	free(workers);
	free(tids);
	free(ctrl);
	free(r_done);
	for (int g = 0; g < ngroups; g++)
		free_mem_onnode(g_bufs[g], map_bytes);

	return total_bw;
}

static double c2crd_run_pair(const int *w_cpus, int nw, const int *r_cpus,
			     int nr, const int *s_cpus, int ns,
			     size_t buf_bytes, size_t chunk_bytes,
			     size_t skip_bytes, int numa_id,
			     const c2c_bw_options_t *opts)
{
	c2crd_group_spec_t g = {
		.nw = nw,
		.nr = nr,
		.ns = ns,
		.numa_id = numa_id,
		.w_cpus = w_cpus,
		.r_cpus = r_cpus,
		.s_cpus = s_cpus
	};
	return c2crd_execute_groups(&g, 1, buf_bytes, chunk_bytes, skip_bytes,
				    opts, false);
}

static bool cpu_in_array(int cpu, const int *arr, int n)
{
	for (int i = 0; i < n; i++) {
		if (arr[i] == cpu)
			return true;
	}
	return false;
}

int run_c2c_readerbw(const c2c_bw_options_t *opts)
{
	if (!opts)
		return 1;

	int n_l3 = numa_get_num_l3_clusters();
	bool has_explicit =
	    (opts->writer_str != NULL || opts->reader_str != NULL ||
	     opts->sharer_str != NULL || opts->ngroups >= 2);

	if (!has_explicit && n_l3 < 2) {
		fprintf(stderr,
			"c2c_readerbw: need at least 2 L3 clusters; detected %d.\n",
			n_l3);
		return 1;
	}

	int64_t buf_kb = 0, chunk_kb = 0;
	if (opts->has_b && opts->has_C) {
		buf_kb = opts->buf_kb;
		chunk_kb = (opts->chunk_kb < opts->buf_kb) ? opts->chunk_kb
							   : opts->buf_kb;
	} else if (opts->has_b) {
		buf_kb = opts->buf_kb;
		chunk_kb = opts->buf_kb;
	} else if (opts->has_C) {
		buf_kb = opts->chunk_kb;
		chunk_kb = opts->chunk_kb;
	} else {
		buf_kb = c2c_default_buf_kb();
		chunk_kb = buf_kb;
	}
	int64_t skip_kb = (opts->skip_kb > 0) ? opts->skip_kb : chunk_kb;
	bool advancing = (chunk_kb < buf_kb);

	printf("\n=== c2c_readerbw (reader-focused, minimal-sync%s) ===\n",
	       advancing ? ", advancing chunks" : ", no chunking");
	printf("  L3 domains:       %d\n", n_l3);
	printf("  total buffer:     %lld KB (%.2f MiB)\n", (long long)buf_kb,
	       (double)buf_kb / 1024.0);
	if (advancing) {
		int64_t epochs_wrap =
		    (skip_kb > 0) ? ((buf_kb - chunk_kb) / skip_kb + 1) : 1;
		printf("  epoch chunk:      %lld KB (%.2f MiB)\n",
		       (long long)chunk_kb, (double)chunk_kb / 1024.0);
		printf("  epoch skip:       %lld KB (%.2f MiB)  [%lld epochs before wrap]\n",
		       (long long)skip_kb, (double)skip_kb / 1024.0,
		       (long long)epochs_wrap);
	}
	printf("  writer mode:      %s\n",
	       opts->hit_clean ? "READ (-H, Shared install)"
			       : "WRITE (Modified install)");
	printf("  reader kernel:    %s\n", simd_reader_label(opts->simd_level));
	printf("  reader R/W mix:   %s\n", rw_mix_label(opts->rw_mix, false));
	printf("  time per pair:    %d sec\n", opts->time_sec);
	printf("  timing:           reader work cycles only (writer staging excluded)\n\n");

	size_t buf_bytes = (size_t)buf_kb << 10;
	size_t chunk_bytes = (size_t)chunk_kb << 10;
	size_t skip_bytes = (size_t)skip_kb << 10;

	/* Mode A: Multi-group concurrent execution (--c2cbw_ngroups >= 2) */
	if (opts->ngroups >= 2) {
		int ng = opts->ngroups;
		if (ng > MAX_C2C_GROUPS) {
			fprintf(stderr,
				"c2c_readerbw: --c2cbw_ngroups=%d exceeds hard cap %d\n",
				ng, MAX_C2C_GROUPS);
			return 1;
		}
		if (!opts->writer_str && !opts->reader_str && ng > n_l3) {
			fprintf(stderr,
				"c2c_readerbw: --c2cbw_ngroups=%d > n_l3_clusters=%d\n",
				ng, n_l3);
			return 1;
		}

		static int mw_cpus[MAX_C2C_GROUPS][MAX_C2C_CPUS];
		static int mr_cpus[MAX_C2C_GROUPS][MAX_C2C_CPUS];
		static int ms_cpus[MAX_C2C_GROUPS][MAX_C2C_CPUS];
		c2crd_group_spec_t specs[MAX_C2C_GROUPS];
		memset(specs, 0, sizeof(specs));

		char w_parts[MAX_C2C_GROUPS][1024];
		char r_parts[MAX_C2C_GROUPS][1024];
		char s_parts[MAX_C2C_GROUPS][1024];

		if (opts->writer_str) {
			int nw_g = split_pipe_groups(opts->writer_str, w_parts,
						     MAX_C2C_GROUPS);
			if (nw_g != ng) {
				fprintf(stderr,
					"c2c_readerbw: --c2cbw_writer has %d '|'-groups, expected %d\n",
					nw_g, ng);
				return 1;
			}
			for (int g = 0; g < ng; g++) {
				specs[g].nw = (int)c2c_parse_cpulist(
				    w_parts[g], mw_cpus[g], MAX_C2C_CPUS);
				specs[g].w_cpus = mw_cpus[g];
			}
		} else {
			for (int g = 0; g < ng; g++) {
				const l3_cluster_info_t *cl =
				    numa_get_l3_cluster(g);
				if (!cl || cl->num_cpus <= 0) {
					fprintf(stderr,
						"c2c_readerbw: L3 %d has no CPUs\n",
						g);
					return 1;
				}
				mw_cpus[g][0] = cl->cpus[0];
				specs[g].nw = 1;
				specs[g].w_cpus = mw_cpus[g];
			}
		}

		if (opts->sharer_str) {
			int ns_g = split_pipe_groups(opts->sharer_str, s_parts,
						     MAX_C2C_GROUPS);
			if (ns_g != ng) {
				fprintf(stderr,
					"c2c_readerbw: --c2cbw_sharer has %d '|'-groups, expected %d\n",
					ns_g, ng);
				return 1;
			}
			for (int g = 0; g < ng; g++) {
				specs[g].ns = (int)c2c_parse_cpulist(
				    s_parts[g], ms_cpus[g], MAX_C2C_CPUS);
				specs[g].s_cpus = ms_cpus[g];
			}
		}

		if (opts->reader_str) {
			int nr_g = split_pipe_groups(opts->reader_str, r_parts,
						     MAX_C2C_GROUPS);
			if (nr_g != ng) {
				fprintf(stderr,
					"c2c_readerbw: --c2cbw_reader has %d '|'-groups, expected %d\n",
					nr_g, ng);
				return 1;
			}
			for (int g = 0; g < ng; g++) {
				specs[g].nr = (int)c2c_parse_cpulist(
				    r_parts[g], mr_cpus[g], MAX_C2C_CPUS);
				specs[g].r_cpus = mr_cpus[g];
			}
		}

		/* Resolve NUMA node per group & handle --c2cbw_auto_sharer before default reader fill */
		for (int g = 0; g < ng; g++) {
			int w_cpu0 = (specs[g].nw > 0) ? specs[g].w_cpus[0] : 0;
			int w_cid = numa_find_l3_cluster_for_cpu(w_cpu0);
			const l3_cluster_info_t *w_cl =
			    (w_cid >= 0) ? numa_get_l3_cluster(w_cid)
					 : numa_get_l3_cluster(g % (n_l3 > 0 ? n_l3 : 1));
			specs[g].numa_id = w_cl ? w_cl->numa_id : 0;

			if (opts->auto_sharer && specs[g].ns == 0) {
				if (!w_cl || w_cl->num_cpus < 2) {
					fprintf(stderr,
						"c2c_readerbw: L3 %d needs >=2 cpus for auto-sharer\n",
						w_cid >= 0 ? w_cid : g);
					return 1;
				}
				int picked = -1;
				for (int c = 0; c < w_cl->num_cpus; c++) {
					int cand = w_cl->cpus[c];
					bool busy = false;
					for (int h = 0; h < ng && !busy; h++) {
						if (cpu_in_array(cand, specs[h].w_cpus, specs[h].nw) ||
						    cpu_in_array(cand, specs[h].r_cpus, specs[h].nr) ||
						    cpu_in_array(cand, specs[h].s_cpus, specs[h].ns)) {
							busy = true;
						}
					}
					if (!busy) {
						picked = cand;
						break;
					}
				}
				if (picked < 0) {
					fprintf(stderr,
						"c2c_readerbw: L3 %d needs >=2 cpus for auto-sharer\n",
						w_cid >= 0 ? w_cid : g);
					return 1;
				}
				ms_cpus[g][0] = picked;
				specs[g].ns = 1;
				specs[g].s_cpus = ms_cpus[g];
			}
		}

		if (!opts->reader_str) {
			for (int g = 0; g < ng; g++) {
				int r_cid = (g + 1) % n_l3;
				int raw_cpus[MAX_C2C_CPUS];
				int raw_cnt = numa_copy_l3_cluster_cpus(
				    r_cid, raw_cpus, MAX_C2C_CPUS);
				int cnt = 0;
				for (int c = 0; c < raw_cnt; c++) {
					int cand = raw_cpus[c];
					bool used = false;
					for (int h = 0; h < ng && !used; h++) {
						if (cpu_in_array(cand, specs[h].w_cpus, specs[h].nw) ||
						    cpu_in_array(cand, specs[h].s_cpus, specs[h].ns)) {
							used = true;
						}
					}
					if (!used)
						mr_cpus[g][cnt++] = cand;
				}
				if (cnt <= 0) {
					fprintf(stderr,
						"c2c_readerbw: L3 %d has no CPUs\n",
						r_cid);
					return 1;
				}
				specs[g].nr = cnt;
				specs[g].r_cpus = mr_cpus[g];
			}
		}

		/* Validate zero CPU overlap across writer / reader / sharer of all groups */
		for (int g1 = 0; g1 < ng; g1++) {
			for (int g2 = 0; g2 < ng; g2++) {
				for (int i = 0; i < specs[g1].nw; i++) {
					int c = specs[g1].w_cpus[i];
					if (cpu_in_array(c, specs[g2].r_cpus,
							 specs[g2].nr)) {
						fprintf(stderr,
							"c2c_readerbw: cpu %d in both writer(g%d) and reader(g%d)\n",
							c, g1, g2);
						return 1;
					}
					if (cpu_in_array(c, specs[g2].s_cpus,
							 specs[g2].ns)) {
						fprintf(stderr,
							"c2c_readerbw: cpu %d in both writer(g%d) and sharer(g%d)\n",
							c, g1, g2);
						return 1;
					}
				}
				for (int i = 0; i < specs[g1].nr; i++) {
					int c = specs[g1].r_cpus[i];
					if (cpu_in_array(c, specs[g2].s_cpus,
							 specs[g2].ns)) {
						fprintf(stderr,
							"c2c_readerbw: cpu %d in both reader(g%d) and sharer(g%d)\n",
							c, g1, g2);
						return 1;
					}
				}
			}
		}

		printf("Multi-group mode: ngroups=%d  per-group buf=%lld MiB  chunk=%lld MiB\n",
		       ng, (long long)(buf_kb >> 10),
		       (long long)(chunk_kb >> 10));
		double bw = c2crd_execute_groups(specs, ng, buf_bytes,
						 chunk_bytes, skip_bytes, opts,
						 true);
		printf("BW = %.2f GiB/s (aggregate over %d groups)\n", bw, ng);
		return 0;
	}

	/* Mode B: Explicit single-pair mode */
	if (opts->writer_str || opts->reader_str || opts->sharer_str) {
		if (!opts->writer_str || !opts->reader_str) {
			fprintf(stderr,
				"c2c_readerbw: need both --c2cbw_writer and --c2cbw_reader\n");
			return 1;
		}
		int w_cpus[MAX_C2C_CPUS], r_cpus[MAX_C2C_CPUS], s_cpus[MAX_C2C_CPUS];
		int nw = (int)c2c_parse_cpulist(opts->writer_str, w_cpus, MAX_C2C_CPUS);
		int nr = (int)c2c_parse_cpulist(opts->reader_str, r_cpus, MAX_C2C_CPUS);
		int ns = opts->sharer_str
			     ? (int)c2c_parse_cpulist(opts->sharer_str, s_cpus,
						      MAX_C2C_CPUS)
			     : 0;
		if (nw <= 0 || nr <= 0) {
			fprintf(stderr,
				"c2c_readerbw: need both --c2cbw_writer and --c2cbw_reader\n");
			return 1;
		}

		int w_cid = numa_find_l3_cluster_for_cpu(w_cpus[0]);
		const l3_cluster_info_t *w_cl = numa_get_l3_cluster(w_cid);
		int numa_id = 0;
		if (w_cl) {
			numa_id = w_cl->numa_id;
		} else {
			const numa_node_info_t *wn =
			    numa_get_node_for_cpu(w_cpus[0]);
			if (wn)
				numa_id = wn->node_id;
		}

		if (opts->auto_sharer && ns == 0 && w_cl && w_cl->num_cpus >= 2) {
			for (int c = 0; c < w_cl->num_cpus; c++) {
				int cand = w_cl->cpus[c];
				if (!cpu_in_array(cand, w_cpus, nw) &&
				    !cpu_in_array(cand, r_cpus, nr)) {
					s_cpus[ns++] = cand;
					break;
				}
			}
		}

		for (int i = 0; i < nw; i++) {
			if (cpu_in_array(w_cpus[i], r_cpus, nr)) {
				fprintf(stderr,
					"c2c_readerbw: cpu %d in both writer and reader\n",
					w_cpus[i]);
				return 1;
			}
			if (cpu_in_array(w_cpus[i], s_cpus, ns)) {
				fprintf(stderr,
					"c2c_readerbw: cpu %d in both writer and sharer\n",
					w_cpus[i]);
				return 1;
			}
		}
		for (int i = 0; i < nr; i++) {
			if (cpu_in_array(r_cpus[i], s_cpus, ns)) {
				fprintf(stderr,
					"c2c_readerbw: cpu %d in both reader and sharer\n",
					r_cpus[i]);
				return 1;
			}
		}

		printf("Explicit groups: writer[%d]={", nw);
		for (int i = 0; i < nw; i++)
			printf("%s%d", i ? "," : "", w_cpus[i]);
		printf("} reader[%d]={", nr);
		for (int i = 0; i < nr; i++)
			printf("%s%d", i ? "," : "", r_cpus[i]);
		printf("}");
		if (ns > 0) {
			printf(" sharer[%d]={", ns);
			for (int i = 0; i < ns; i++)
				printf("%s%d", i ? "," : "", s_cpus[i]);
			printf("}");
		}
		printf("\n");

		double bw = c2crd_run_pair(w_cpus, nw, r_cpus, nr, s_cpus, ns,
					   buf_bytes, chunk_bytes, skip_bytes,
					   numa_id, opts);
		printf("BW = %.2f GB/s\n", bw);
		return 0;
	}

	/* Mode C: Default NxN L3 domain matrix (1W + up to 4 sharers vs ALL readers in L3) */
	printf("%-13s Reader L3 domain\n", "");
	printf("%-13s", "Writer");
	for (int j = 0; j < n_l3; j++) {
		const l3_cluster_info_t *cl = numa_get_l3_cluster(j);
		char lbl[32];
		snprintf(lbl, sizeof(lbl), "NUMA%d:LLC%d",
			 cl ? cl->numa_id : j, j);
		printf("%13s", lbl);
	}
	printf("\n");

	int w_cpus[MAX_C2C_CPUS], r_cpus[MAX_C2C_CPUS], s_cpus[4];
	for (int w = 0; w < n_l3; w++) {
		const l3_cluster_info_t *wcl = numa_get_l3_cluster(w);
		char lbl[32];
		snprintf(lbl, sizeof(lbl), "NUMA%d:LLC%d",
			 wcl ? wcl->numa_id : w, w);
		printf("%-13s", lbl);

		int w_cnt = numa_copy_l3_cluster_cpus(w, w_cpus, MAX_C2C_CPUS);
		int ns = 0;
		if (w_cnt > 1) {
			ns = (w_cnt - 1 > 4) ? 4 : (w_cnt - 1);
			for (int k = 0; k < ns; k++)
				s_cpus[k] = w_cpus[k + 1];
		}

		for (int r = 0; r < n_l3; r++) {
			if (w == r) {
				printf("      -      ");
				continue;
			}
			int r_cnt =
			    numa_copy_l3_cluster_cpus(r, r_cpus, MAX_C2C_CPUS);
			if (w_cnt <= 0 || r_cnt <= 0) {
				printf("      -      ");
				continue;
			}
			double bw = c2crd_run_pair(
			    w_cpus, 1, r_cpus, r_cnt, s_cpus, ns, buf_bytes,
			    chunk_bytes, skip_bytes, wcl ? wcl->numa_id : 0,
			    opts);
			printf("%12.1f ", bw);
			fflush(stdout);
		}
		printf("\n");
	}
	puts("(units: GB/s; rows = writer L3 domain, cols = reader L3 domain)");
	return 0;
}

/* =========================================================================
 * Engine 2: Dual-Matrix Mode (--c2c_bandwidth_matrix / default suite tail)
 * Matching FUN_0013b90d
 * ========================================================================= */

static void run_single_c2c_matrix_pass(int eff_n, size_t buf_bytes,
				       size_t chunk_bytes, size_t skip_bytes,
				       const c2c_bw_options_t *opts)
{
	printf("%-13s Reader L3 domain\n", "");
	printf("%-13s", "Writer");
	for (int j = 0; j < eff_n; j++) {
		int cid = numa_get_effective_l3_cluster_id(j);
		const l3_cluster_info_t *cl = numa_get_l3_cluster(cid);
		char lbl[32];
		snprintf(lbl, sizeof(lbl), "NUMA%d:LLC%d",
			 cl ? cl->numa_id : cid, cid);
		printf("%13s", lbl);
	}
	printf("\n");

	int w_cpus[MAX_C2C_CPUS], r_cpus[MAX_C2C_CPUS], s_cpus[4];
	for (int wi = 0; wi < eff_n; wi++) {
		int w_cid = numa_get_effective_l3_cluster_id(wi);
		const l3_cluster_info_t *wcl = numa_get_l3_cluster(w_cid);
		char lbl[32];
		snprintf(lbl, sizeof(lbl), "NUMA%d:LLC%d",
			 wcl ? wcl->numa_id : w_cid, w_cid);
		printf("%-13s", lbl);

		int w_cnt =
		    numa_copy_l3_cluster_cpus(w_cid, w_cpus, MAX_C2C_CPUS);
		int ns = 0;
		if (w_cnt > 1) {
			ns = (w_cnt - 1 > 4) ? 4 : (w_cnt - 1);
			for (int k = 0; k < ns; k++)
				s_cpus[k] = w_cpus[k + 1];
		}

		for (int ri = 0; ri < eff_n; ri++) {
			int r_cid = numa_get_effective_l3_cluster_id(ri);
			if (w_cid == r_cid) {
				printf("      -      ");
				continue;
			}
			int r_cnt = numa_copy_l3_cluster_cpus(r_cid, r_cpus,
							      MAX_C2C_CPUS);
			if (w_cnt <= 0 || r_cnt <= 0) {
				printf("      -      ");
				continue;
			}
			double bw = c2crd_run_pair(
			    w_cpus, 1, r_cpus, r_cnt, s_cpus, ns, buf_bytes,
			    chunk_bytes, skip_bytes, wcl ? wcl->numa_id : 0,
			    opts);
			printf("%12.1f ", bw);
			fflush(stdout);
		}
		printf("\n");
	}
}

int run_c2c_bandwidth_matrix_mode(const c2c_bw_options_t *opts)
{
	if (!opts)
		return 1;

	int eff_n = numa_get_effective_l3_count();
	if (eff_n < 2) {
		fprintf(stderr,
			"c2c_bandwidth_matrix: need at least 2 L3 clusters; detected %d.\n",
			eff_n);
		return 1;
	}

	int64_t mat1_bytes = c2c_matrix1_bytes();
	int64_t mat1_kb = mat1_bytes >> 10;
	int64_t mat2_bytes = 1LL << 32;	/* 4 GiB */

	printf("\n=== c2c_bandwidth_matrix (reader-focused NxN, %d L3 domains) ===\n",
	       eff_n);
	numa_print_l3_sampling_notice();
	printf("  time per pair:  %d sec\n", opts->time_sec);
	printf("  reader kernel:  %s\n", simd_short_label(opts->simd_level));

	/* Matrix 1: Modified install (1W + 4 sharers), -b = -C = mat1_bytes */
	c2c_bw_options_t m1 = *opts;
	m1.hit_clean = false;
	printf("\n--- Matrix 1: Modified install (1W + 4 sharers), -b = -C = %lld KB (%.1f MiB) ---\n\n",
	       (long long)mat1_kb, (double)mat1_bytes / (1024.0 * 1024.0));
	run_single_c2c_matrix_pass(eff_n, (size_t)mat1_bytes,
				   (size_t)mat1_bytes, (size_t)mat1_bytes, &m1);
	puts("(units: GB/s; rows = writer L3, cols = reader L3)");

	/* Matrix 2: Shared install (-H, read-only), -b = 4096 MiB, -C = mat1_kb, skip = C */
	c2c_bw_options_t m2 = *opts;
	m2.hit_clean = true;
	printf("\n--- Matrix 2: Shared install (-H, read-only), 1W + 4 sharers, -b = %lld MiB, -C = %lld KB (%.1f MiB), skip = C ---\n\n",
	       (long long)(mat2_bytes >> 20), (long long)mat1_kb,
	       (double)mat1_bytes / (1024.0 * 1024.0));
	run_single_c2c_matrix_pass(eff_n, (size_t)mat2_bytes,
				   (size_t)mat1_bytes, (size_t)mat1_bytes, &m2);
	printf("(units: GB/s; rows = writer L3, cols = reader L3)\n\n");
	return 0;
}

/* =========================================================================
 * Engine 3: Per-Chunk Ring & Stepped Barrier (--c2c_bandwidth_pipelined)
 * Matching FUN_00131b5e .. FUN_0013274b .. FUN_0013c23d
 * ========================================================================= */

typedef struct __attribute__((aligned(64))) {
	uint32_t epoch;
	uint32_t r_cnt;
	uint32_t s_cnt;
	uint8_t pad[52];
} c2cbw_chunk_state_t;

typedef struct __attribute__((aligned(64))) {
	uint32_t w_ep;
	uint32_t s_ep;
	uint32_t r_ep;
	uint32_t w_done;
	uint32_t s_done;
	uint32_t r_done;
	uint8_t pad[40];
} c2cbw_step_bar_t;

typedef struct {
	uint8_t *buf;
	size_t total_bytes;
	size_t slot_bytes;
	size_t chunk_bytes;
	int n_slots;
	int nchunks;
	int total_chunks;
	int nw;
	int nr;
	int ns;
	int readers_per_slot;
	int *slot_sharers;
	c2cbw_chunk_state_t *chunks;
	c2cbw_step_bar_t *bar;
	volatile int stop;
	int simd_level;
	int rw_mix;
	int delay;
	bool hit_clean;
	bool partial;
	bool stepped;
} c2cbw_ring_ctx_t;

typedef struct {
	c2cbw_ring_ctx_t *ctx;
	int cpu;
	int role_idx;
	int slot;
	int pair_bit;
	uint64_t total_bytes;
	uint64_t work_tsc;
} c2cbw_ring_worker_t;

/* Partner slot mapping for -u -H matching FUN_0013270b */
static int pair_partner_slot(int idx, int n_slots)
{
	if (n_slots < 2)
		return idx;
	if (n_slots == 2)
		return 1 - idx;
	return (idx + (n_slots / 2)) % n_slots;
}

static void *c2cbw_ring_writer_fn(void *arg)
{
	c2cbw_ring_worker_t *w = (c2cbw_ring_worker_t *)arg;
	c2cbw_ring_ctx_t *ctx = w->ctx;
	numa_pin_cpu(w->cpu);

	if (ctx->stepped) {
		size_t base_slice = (ctx->total_bytes / (size_t)ctx->nw) & ~63ULL;
		size_t off = (size_t)w->role_idx * base_slice;
		size_t len = (w->role_idx == ctx->nw - 1)
				 ? (ctx->total_bytes - off)
				 : base_slice;
		uint32_t ep = 0;
		while (!__atomic_load_n(&ctx->stop, __ATOMIC_ACQUIRE)) {
			while (__atomic_load_n(&ctx->bar->w_ep, __ATOMIC_ACQUIRE) <= ep) {
				if (__atomic_load_n(&ctx->stop, __ATOMIC_ACQUIRE))
					return NULL;
				_mm_pause();
			}
			if (__atomic_load_n(&ctx->stop, __ATOMIC_ACQUIRE))
				break;
			if (len > 0) {
				writer_install(ctx->buf + off, len,
					       ctx->hit_clean, ctx->partial,
					       (uint64_t)(ep + 1U),
					       ctx->simd_level);
			}
			ep++;
			uint32_t prev = __atomic_fetch_add(&ctx->bar->w_done, 1U,
							   __ATOMIC_ACQ_REL);
			if ((int)(prev + 1U) == ctx->nw) {
				__atomic_store_n(&ctx->bar->w_done, 0U,
						 __ATOMIC_RELEASE);
				if (ctx->ns > 0) {
					__atomic_fetch_add(&ctx->bar->s_ep, 1U,
							   __ATOMIC_ACQ_REL);
				} else {
					__atomic_fetch_add(&ctx->bar->r_ep, 1U,
							   __ATOMIC_ACQ_REL);
				}
			}
		}
		return NULL;
	}

	uint64_t seq = 1ULL;
	while (!__atomic_load_n(&ctx->stop, __ATOMIC_ACQUIRE)) {
		for (int flat = w->role_idx; flat < ctx->total_chunks;
		     flat += ctx->nw) {
			if (__atomic_load_n(&ctx->stop, __ATOMIC_ACQUIRE))
				return NULL;
			int slot = flat / ctx->nchunks;
			uint32_t need_r = (uint32_t)ctx->readers_per_slot;
			uint32_t need_s = (uint32_t)ctx->slot_sharers[slot];
			c2cbw_chunk_state_t *cs = &ctx->chunks[flat];

			while (__atomic_load_n(&cs->r_cnt, __ATOMIC_ACQUIRE) < need_r ||
			       __atomic_load_n(&cs->s_cnt, __ATOMIC_ACQUIRE) < need_s) {
				if (__atomic_load_n(&ctx->stop, __ATOMIC_ACQUIRE))
					return NULL;
				_mm_pause();
			}

			uint8_t *addr =
			    ctx->buf + (size_t)flat * ctx->chunk_bytes;
			writer_install(addr, ctx->chunk_bytes, ctx->hit_clean,
				       ctx->partial, seq++, ctx->simd_level);

			__atomic_store_n(&cs->r_cnt, 0U, __ATOMIC_RELEASE);
			__atomic_store_n(&cs->s_cnt, 0U, __ATOMIC_RELEASE);
			__atomic_fetch_add(&cs->epoch, 1U, __ATOMIC_ACQ_REL);
		}
	}
	return NULL;
}

static void *c2cbw_ring_sharer_fn(void *arg)
{
	c2cbw_ring_worker_t *s = (c2cbw_ring_worker_t *)arg;
	c2cbw_ring_ctx_t *ctx = s->ctx;
	numa_pin_cpu(s->cpu);

	if (ctx->stepped) {
		size_t base_slice = (ctx->total_bytes / (size_t)ctx->ns) & ~63ULL;
		size_t off = (size_t)s->role_idx * base_slice;
		size_t len = (s->role_idx == ctx->ns - 1)
				 ? (ctx->total_bytes - off)
				 : base_slice;
		uint32_t ep = 0;
		while (!__atomic_load_n(&ctx->stop, __ATOMIC_ACQUIRE)) {
			while (__atomic_load_n(&ctx->bar->s_ep, __ATOMIC_ACQUIRE) <= ep) {
				if (__atomic_load_n(&ctx->stop, __ATOMIC_ACQUIRE))
					return NULL;
				_mm_pause();
			}
			if (__atomic_load_n(&ctx->stop, __ATOMIC_ACQUIRE))
				break;
			if (len > 0) {
				reader_read_100pct(ctx->buf + off, len,
						   ctx->simd_level, 0);
			}
			ep++;
			uint32_t prev = __atomic_fetch_add(&ctx->bar->s_done, 1U,
							   __ATOMIC_ACQ_REL);
			if ((int)(prev + 1U) == ctx->ns) {
				__atomic_store_n(&ctx->bar->s_done, 0U,
						 __ATOMIC_RELEASE);
				__atomic_fetch_add(&ctx->bar->r_ep, 1U,
						   __ATOMIC_ACQ_REL);
			}
		}
		return NULL;
	}

	uint32_t *last_ep =
	    (uint32_t *)calloc((size_t)ctx->nchunks, sizeof(uint32_t));
	if (!last_ep)
		return NULL;
	int base_flat = s->slot * ctx->nchunks;

	while (!__atomic_load_n(&ctx->stop, __ATOMIC_ACQUIRE)) {
		for (int k = 0; k < ctx->nchunks; k++) {
			int flat = base_flat + k;
			c2cbw_chunk_state_t *cs = &ctx->chunks[flat];
			uint32_t ep;
			while ((ep = __atomic_load_n(&cs->epoch,
						     __ATOMIC_ACQUIRE)) ==
			       last_ep[k]) {
				if (__atomic_load_n(&ctx->stop, __ATOMIC_ACQUIRE)) {
					free(last_ep);
					return NULL;
				}
				_mm_pause();
			}
			uint8_t *addr =
			    ctx->buf + (size_t)flat * ctx->chunk_bytes;
			reader_read_100pct(addr, ctx->chunk_bytes,
					   ctx->simd_level, 0);
			last_ep[k] = ep;
			__atomic_fetch_add(&cs->s_cnt, 1U, __ATOMIC_ACQ_REL);
		}
	}
	free(last_ep);
	return NULL;
}

static void *c2cbw_ring_reader_fn(void *arg)
{
	c2cbw_ring_worker_t *r = (c2cbw_ring_worker_t *)arg;
	c2cbw_ring_ctx_t *ctx = r->ctx;
	numa_pin_cpu(r->cpu);

	if (ctx->stepped) {
		size_t off = (size_t)r->slot * ctx->slot_bytes;
		size_t len = ctx->slot_bytes;
		uint32_t ep = 0;
		while (!__atomic_load_n(&ctx->stop, __ATOMIC_ACQUIRE)) {
			while (__atomic_load_n(&ctx->bar->r_ep, __ATOMIC_ACQUIRE) <= ep) {
				if (__atomic_load_n(&ctx->stop, __ATOMIC_ACQUIRE))
					return NULL;
				_mm_pause();
			}
			if (__atomic_load_n(&ctx->stop, __ATOMIC_ACQUIRE))
				break;
			uint64_t t0 = rdtsc_start();
			uint64_t b = reader_rw_kernel(
			    ctx->buf + off, len, ctx->simd_level, ctx->rw_mix,
			    (uint64_t)(ep + 1U), ctx->delay);
			uint64_t t1 = rdtscp_end();
			r->work_tsc += (t1 - t0);
			r->total_bytes += b;
			ep++;
			uint32_t prev = __atomic_fetch_add(&ctx->bar->r_done, 1U,
							   __ATOMIC_ACQ_REL);
			if ((int)(prev + 1U) == ctx->nr) {
				__atomic_store_n(&ctx->bar->r_done, 0U,
						 __ATOMIC_RELEASE);
				__atomic_fetch_add(&ctx->bar->w_ep, 1U,
						   __ATOMIC_ACQ_REL);
			}
		}
		return NULL;
	}

	uint32_t *last_ep =
	    (uint32_t *)calloc((size_t)ctx->nchunks, sizeof(uint32_t));
	if (!last_ep)
		return NULL;
	int base_flat = r->slot * ctx->nchunks;
	uint32_t need_s = (uint32_t)ctx->slot_sharers[r->slot];

	while (!__atomic_load_n(&ctx->stop, __ATOMIC_ACQUIRE)) {
		for (int k = 0; k < ctx->nchunks; k++) {
			int flat = base_flat + k;
			c2cbw_chunk_state_t *cs = &ctx->chunks[flat];
			uint32_t ep;
			while (true) {
				if (__atomic_load_n(&ctx->stop, __ATOMIC_ACQUIRE)) {
					free(last_ep);
					return NULL;
				}
				ep = __atomic_load_n(&cs->epoch,
						     __ATOMIC_ACQUIRE);
				if (ep != last_ep[k] &&
				    __atomic_load_n(&cs->s_cnt,
						    __ATOMIC_ACQUIRE) >= need_s) {
					break;
				}
				_mm_pause();
			}

			uint8_t *addr =
			    ctx->buf + (size_t)flat * ctx->chunk_bytes;
			uint64_t t0 = rdtsc_start();
			uint64_t b = reader_rw_kernel(addr, ctx->chunk_bytes,
						      ctx->simd_level,
						      ctx->rw_mix,
						      (uint64_t)ep, ctx->delay);
			uint64_t t1 = rdtscp_end();
			r->work_tsc += (t1 - t0);
			r->total_bytes += b;
			last_ep[k] = ep;
			__atomic_fetch_add(&cs->r_cnt, 1U, __ATOMIC_ACQ_REL);
		}
	}
	free(last_ep);
	return NULL;
}

static double c2cbw_run_pair(const int *w_cpus, int nw, const int *r_cpus,
			     int nr, const int *s_cpus, int ns,
			     size_t buf_bytes, size_t chunk_bytes, int numa_id,
			     const c2c_bw_options_t *opts)
{
	if (nw <= 0 || nr <= 0)
		return 0.0;

	bool pair_shared = (opts->shared_buf && opts->hit_clean && nr >= 2 &&
			    (nr % 2) == 0);
	int readers_per_slot = pair_shared ? 2 : 1;
	int n_slots = pair_shared ? (nr / 2) : nr;
	if (n_slots < 1)
		n_slots = 1;

	chunk_bytes = chunk_bytes & ~63ULL;
	if (chunk_bytes < 64ULL)
		chunk_bytes = 64ULL;
	size_t nchunks_sz = (buf_bytes / (size_t)n_slots) / chunk_bytes;
	if (nchunks_sz < 2)
		nchunks_sz = 2;
	int nchunks = (int)nchunks_sz;
	size_t slot_bytes = (size_t)nchunks * chunk_bytes;
	size_t total_bytes = (size_t)n_slots * slot_bytes;
	int total_chunks = n_slots * nchunks;

	hugepage_type_t hp_mode =
	    (numa_get_hugepage_mode() == HUGEPAGE_1GB) ? HUGEPAGE_1GB
						       : HUGEPAGE_2MB;
	size_t hp_align =
	    (hp_mode == HUGEPAGE_1GB) ? (1ULL << 30) : (2ULL << 20);
	size_t map_bytes = (total_bytes + hp_align - 1ULL) & ~(hp_align - 1ULL);
	uint8_t *buf =
	    (uint8_t *)alloc_mem_onnode(map_bytes, numa_id, hp_mode);
	if (!buf) {
		fprintf(stderr,
			"c2c_bandwidth: alloc %lld bytes on numa %d failed\n",
			(long long)total_bytes, numa_id);
		return 0.0;
	}

	c2cbw_chunk_state_t *chunks = NULL;
	c2cbw_step_bar_t *bar = NULL;
	int *slot_sharers = (int *)calloc((size_t)n_slots, sizeof(int));
	if (!slot_sharers ||
	    posix_memalign((void **)&chunks, 64,
			   (size_t)total_chunks * sizeof(*chunks)) != 0 ||
	    posix_memalign((void **)&bar, 64, sizeof(*bar)) != 0) {
		fprintf(stderr, "c2c_bandwidth: sync alloc failed\n");
		free(slot_sharers);
		free(chunks);
		free(bar);
		free_mem_onnode(buf, map_bytes);
		return 0.0;
	}
	memset(chunks, 0, (size_t)total_chunks * sizeof(*chunks));
	memset(bar, 0, sizeof(*bar));
	bar->w_ep = 1U;

	for (int i = 0; i < ns; i++)
		slot_sharers[i % n_slots]++;

	for (int f = 0; f < total_chunks; f++) {
		int sl = f / nchunks;
		chunks[f].epoch = 0U;
		chunks[f].r_cnt = (uint32_t)readers_per_slot;
		chunks[f].s_cnt = (uint32_t)slot_sharers[sl];
	}

	c2cbw_ring_ctx_t ctx = {
		.buf = buf,
		.total_bytes = total_bytes,
		.slot_bytes = slot_bytes,
		.chunk_bytes = chunk_bytes,
		.n_slots = n_slots,
		.nchunks = nchunks,
		.total_chunks = total_chunks,
		.nw = nw,
		.nr = nr,
		.ns = ns,
		.readers_per_slot = readers_per_slot,
		.slot_sharers = slot_sharers,
		.chunks = chunks,
		.bar = bar,
		.stop = 0,
		.simd_level = opts->simd_level,
		.rw_mix = opts->rw_mix,
		.delay = opts->delay,
		.hit_clean = opts->hit_clean,
		.partial = opts->partial,
		.stepped = opts->stepped
	};

	if (opts->verbose) {
		int s_per_slot =
		    (ns > 0) ? ((ns + n_slots - 1) / n_slots) : 0;
		printf("[c2cbw] buf=%p total_bytes=%lld  n_slots=%d  slot_bytes=%lld  nchunks=%d  chunk_bytes=%lld  ring_depth=%d\n",
		       (void *)buf, (long long)total_bytes, n_slots,
		       (long long)slot_bytes, nchunks, (long long)chunk_bytes,
		       nchunks);
		printf("[c2cbw] mode=%s  writers=%d  readers=%d  sharers=%d  readers_per_slot=%d  sharers_per_slot=%d  delay=%d\n",
		       opts->stepped ? "STEPPED" : "PIPELINED", nw, nr, ns,
		       readers_per_slot, s_per_slot, opts->delay);
	}

	int total_threads = nw + ns + nr;
	c2cbw_ring_worker_t *workers =
	    (c2cbw_ring_worker_t *)calloc((size_t)total_threads, sizeof(*workers));
	pthread_t *tids =
	    (pthread_t *)calloc((size_t)total_threads, sizeof(*tids));
	if (!workers || !tids) {
		free(workers);
		free(tids);
		free(slot_sharers);
		free(chunks);
		free(bar);
		free_mem_onnode(buf, map_bytes);
		return 0.0;
	}

	uint64_t wall_t0 = rdtsc_start();

	int idx = 0;
	for (int i = 0; i < nw; i++, idx++) {
		workers[idx].ctx = &ctx;
		workers[idx].cpu = w_cpus[i];
		workers[idx].role_idx = i;
		pthread_create(&tids[idx], NULL, c2cbw_ring_writer_fn,
			       &workers[idx]);
	}
	for (int i = 0; i < ns; i++, idx++) {
		workers[idx].ctx = &ctx;
		workers[idx].cpu = s_cpus[i];
		workers[idx].role_idx = i;
		workers[idx].slot = i % n_slots;
		pthread_create(&tids[idx], NULL, c2cbw_ring_sharer_fn,
			       &workers[idx]);
	}
	int r_base = idx;
	for (int i = 0; i < nr; i++, idx++) {
		workers[idx].ctx = &ctx;
		workers[idx].cpu = r_cpus[i];
		workers[idx].role_idx = i;
		if (pair_shared) {
			if (i < n_slots) {
				workers[idx].slot = i;
				workers[idx].pair_bit = 0;
			} else {
				workers[idx].slot =
				    pair_partner_slot(i - n_slots, n_slots);
				workers[idx].pair_bit = 1;
			}
		} else {
			workers[idx].slot = i;
			workers[idx].pair_bit = 0;
		}
		pthread_create(&tids[idx], NULL, c2cbw_ring_reader_fn,
			       &workers[idx]);
	}

	int dur = (opts->time_sec > 0) ? opts->time_sec : 3;
	struct timespec req = { .tv_sec = dur, .tv_nsec = 0 };
	nanosleep(&req, NULL);

	__atomic_store_n(&ctx.stop, 1, __ATOMIC_SEQ_CST);
	for (int i = 0; i < total_threads; i++)
		pthread_join(tids[i], NULL);

	uint64_t wall_t1 = rdtscp_end();
	double ns_per_tsc = get_ns_per_tsc();
	double wall_sec = (double)(wall_t1 - wall_t0) * ns_per_tsc * 1e-9;

	double total_bw = 0.0;
	uint64_t sum_bytes = 0;
	uint64_t max_r_tsc = 0;
	for (int i = 0; i < nr; i++) {
		const c2cbw_ring_worker_t *rk = &workers[r_base + i];
		total_bw +=
		    bytes_tsc_to_gibs(rk->total_bytes, rk->work_tsc, ns_per_tsc);
		sum_bytes += rk->total_bytes;
		if (rk->work_tsc > max_r_tsc)
			max_r_tsc = rk->work_tsc;
	}

	if (opts->verbose) {
		double max_active = (double)max_r_tsc * ns_per_tsc * 1e-9;
		double pct =
		    (wall_sec > 0.0) ? (100.0 * max_active / wall_sec) : 0.0;
		printf("[c2cbw] reader total_bytes=%.2f MiB  max_active=%.3f s  wall=%.3f s  active/wall=%.1f%%  BW=%.2f GB/s\n",
		       (double)sum_bytes / (1024.0 * 1024.0), max_active,
		       wall_sec, pct, total_bw);
	}

	free(workers);
	free(tids);
	free(slot_sharers);
	free(chunks);
	free(bar);
	free_mem_onnode(buf, map_bytes);
	return total_bw;
}

static bool validate_pipelined_cpulist(const char *role, const int *cpus, int n,
				       int total_cpus)
{
	for (int i = 0; i < n; i++) {
		if (cpus[i] < 0 || cpus[i] >= total_cpus) {
			fprintf(stderr,
				"c2c_bandwidth: %s cpulist has invalid cpu %d\n",
				role, cpus[i]);
			return false;
		}
		for (int j = 0; j < i; j++) {
			if (cpus[j] == cpus[i]) {
				fprintf(stderr,
					"c2c_bandwidth: %s cpulist has duplicate cpu %d\n",
					role, cpus[i]);
				return false;
			}
		}
	}
	return true;
}

int run_c2c_bandwidth_pipelined(const c2c_bw_options_t *opts)
{
	if (!opts)
		return 1;
	if (opts->shared_buf && !opts->hit_clean) {
		fprintf(stderr, "c2c_bandwidth: -u requires -H.\n");
		return 1;
	}

	int n_l3 = numa_get_num_l3_clusters();
	bool explicit_pair =
	    (opts->writer_str != NULL || opts->reader_str != NULL ||
	     opts->sharer_str != NULL);
	if (!explicit_pair && n_l3 < 2) {
		fprintf(stderr,
			"c2c_bandwidth: need at least 2 L3 clusters; detected %d.\n",
			n_l3);
		return 1;
	}

	int64_t buf_kb = opts->has_b ? opts->buf_kb : c2c_default_buf_kb();
	int64_t chunk_kb =
	    opts->has_C ? opts->chunk_kb : c2c_default_chunk_kb(buf_kb);
	if (chunk_kb > buf_kb)
		chunk_kb = buf_kb;
	if (chunk_kb < 1)
		chunk_kb = 1;
	int64_t ring_depth = buf_kb / chunk_kb;

	puts("\n=== c2c_bandwidth ===");
	printf("  L3 domains:          %d (across all NUMA)\n", n_l3);
	printf("  total buffer/group:  %lld KB\n", (long long)buf_kb);
	printf("  total chunk/group:   %lld KB  (ring depth = b/C = %lld)\n",
	       (long long)chunk_kb, (long long)ring_depth);
	printf("  writer mode:         %s\n",
	       opts->hit_clean ? "READ (-H, Shared install)"
			       : "WRITE (Modified install)");
	printf("  pair-shared buffers: %s\n",
	       (opts->shared_buf && opts->hit_clean) ? "yes (-u -H)" : "no");
	printf("  reader kernel:       %s\n",
	       (opts->simd_level == 0) ? "SSE2 / scalar load"
				       : simd_reader_label(opts->simd_level));
	printf("  reader R/W mix:      %s\n", rw_mix_label(opts->rw_mix, true));
	printf("  time per pair:       %d sec\n", opts->time_sec);
	printf("  schedule:            %s\n",
	       opts->stepped ? "STEPPED (W->S->R barrier)"
			     : "PIPELINED (per-chunk ring)");
	printf("  verbose:             %s\n\n",
	       opts->verbose ? "on (--c2cbw_verbose)" : "off");

	size_t buf_bytes = (size_t)buf_kb << 10;
	size_t chunk_bytes = (size_t)chunk_kb << 10;

	if (explicit_pair) {
		if (!opts->writer_str || !opts->reader_str) {
			fprintf(stderr,
				"c2c_bandwidth: need both --c2cbw_writer and --c2cbw_reader (you gave writer=%s reader=%s sharer=%s).\n",
				opts->writer_str ? opts->writer_str : "<none>",
				opts->reader_str ? opts->reader_str : "<none>",
				opts->sharer_str ? opts->sharer_str : "<none>");
			return 1;
		}
		int total_cpus = numa_get_total_cpus();
		int w_cpus[MAX_C2C_CPUS], r_cpus[MAX_C2C_CPUS], s_cpus[MAX_C2C_CPUS];
		int nw = (int)c2c_parse_cpulist(opts->writer_str, w_cpus, MAX_C2C_CPUS);
		int nr = (int)c2c_parse_cpulist(opts->reader_str, r_cpus, MAX_C2C_CPUS);
		int ns = opts->sharer_str
			     ? (int)c2c_parse_cpulist(opts->sharer_str, s_cpus,
						      MAX_C2C_CPUS)
			     : 0;
		if (!validate_pipelined_cpulist("writer", w_cpus, nw, total_cpus) ||
		    !validate_pipelined_cpulist("reader", r_cpus, nr, total_cpus) ||
		    !validate_pipelined_cpulist("sharer", s_cpus, ns, total_cpus)) {
			return 1;
		}
		if (opts->shared_buf && opts->hit_clean &&
		    (nr < 2 || (nr % 2) != 0)) {
			fprintf(stderr,
				"c2c_bandwidth: -u -H requires an even number of readers (>=2); got %d\n",
				nr);
			return 1;
		}

		int w_cid = numa_find_l3_cluster_for_cpu(w_cpus[0]);
		const l3_cluster_info_t *w_cl = numa_get_l3_cluster(w_cid);
		int numa_id = w_cl ? w_cl->numa_id : 0;

		if (opts->auto_sharer && ns == 0 && w_cl && w_cl->num_cpus >= 2) {
			for (int c = 0; c < w_cl->num_cpus; c++) {
				int cand = w_cl->cpus[c];
				if (!cpu_in_array(cand, w_cpus, nw) &&
				    !cpu_in_array(cand, r_cpus, nr)) {
					s_cpus[ns++] = cand;
					break;
				}
			}
		}

		for (int i = 0; i < nw; i++) {
			if (cpu_in_array(w_cpus[i], r_cpus, nr)) {
				fprintf(stderr,
					"c2c_bandwidth: cpu %d in both writer and reader groups (overlap not allowed)\n",
					w_cpus[i]);
				return 1;
			}
			if (cpu_in_array(w_cpus[i], s_cpus, ns)) {
				fprintf(stderr,
					"c2c_bandwidth: cpu %d in both writer and sharer groups (overlap not allowed)\n",
					w_cpus[i]);
				return 1;
			}
		}
		for (int i = 0; i < nr; i++) {
			if (cpu_in_array(r_cpus[i], s_cpus, ns)) {
				fprintf(stderr,
					"c2c_bandwidth: cpu %d in both reader and sharer groups (overlap not allowed)\n",
					r_cpus[i]);
				return 1;
			}
		}

		printf("Explicit groups: writer[%d]={", nw);
		for (int i = 0; i < nw; i++)
			printf("%s%d", i ? "," : "", w_cpus[i]);
		printf("} reader[%d]={", nr);
		for (int i = 0; i < nr; i++)
			printf("%s%d", i ? "," : "", r_cpus[i]);
		printf("}");
		if (ns > 0) {
			printf(" sharer[%d]={", ns);
			for (int i = 0; i < ns; i++)
				printf("%s%d", i ? "," : "", s_cpus[i]);
			printf("}");
		}
		printf("\n");

		double bw = c2cbw_run_pair(w_cpus, nw, r_cpus, nr,
					   ns > 0 ? s_cpus : NULL, ns,
					   buf_bytes, chunk_bytes, numa_id,
					   opts);
		printf("BW = %.2f GB/s\n", bw);
		return 0;
	}

	puts("         Reader L3 domain");
	printf("Writer  ");
	for (int j = 0; j < n_l3; j++) {
		const l3_cluster_info_t *cl = numa_get_l3_cluster(j);
		printf(" N%dL%-2d ", cl ? cl->numa_id : j, j);
	}
	printf("\n");

	int w_cpus[MAX_C2C_CPUS], r_cpus[MAX_C2C_CPUS];
	for (int w = 0; w < n_l3; w++) {
		const l3_cluster_info_t *wcl = numa_get_l3_cluster(w);
		printf("N%dL%-2d  ", wcl ? wcl->numa_id : w, w);
		int w_cnt = numa_copy_l3_cluster_cpus(w, w_cpus, MAX_C2C_CPUS);

		for (int r = 0; r < n_l3; r++) {
			if (w == r) {
				printf("   -   ");
				continue;
			}
			int r_cnt =
			    numa_copy_l3_cluster_cpus(r, r_cpus, MAX_C2C_CPUS);
			if (w_cnt <= 0 || r_cnt <= 0) {
				printf("   -   ");
				continue;
			}
			double bw = c2cbw_run_pair(
			    w_cpus, w_cnt, r_cpus, r_cnt, NULL, 0, buf_bytes,
			    chunk_bytes, wcl ? wcl->numa_id : 0, opts);
			printf("%6.1f ", bw);
			fflush(stdout);
		}
		printf("\n");
	}
	puts("\n(units: GB/s; rows = writer L3 domain, cols = reader L3 domain)");
	return 0;
}

int run_c2c_bandwidth_dispatch(const c2c_bw_options_t *opts)
{
	if (!opts)
		return 1;
	switch (opts->engine) {
	case C2C_ENGINE_MATRIX:
		return run_c2c_bandwidth_matrix_mode(opts);
	case C2C_ENGINE_PIPELINED:
		return run_c2c_bandwidth_pipelined(opts);
	case C2C_ENGINE_READERBW:
	default:
		return run_c2c_readerbw(opts);
	}
}

void run_c2c_bandwidth_matrix(double duration_sec, size_t buffer_kb,
			      bool pipelined)
{
	c2c_bw_options_t opts;
	c2c_bw_options_init(&opts);
	opts.engine = pipelined ? C2C_ENGINE_PIPELINED : C2C_ENGINE_READERBW;
	opts.time_sec = (duration_sec > 0.0) ? (int)duration_sec : 3;
	if (buffer_kb > 0) {
		opts.has_b = true;
		opts.buf_kb = (int64_t)buffer_kb;
	}
	(void)run_c2c_bandwidth_dispatch(&opts);
}
