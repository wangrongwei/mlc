#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <immintrin.h>
#include "combined-loadlat.h"
#include "allocation.h"
#include "busythread.h"
#include "helper.h"
#include "intrd.h"
#include "fastdelay_dispatch.h"
#include "ddio.h"

/*
 * Global throughput configuration matching intel/mlc globals:
 *   - Default SIMD width: SSE2 (128-bit movdqa / movntdq)
 *   - -Y: AVX2 (256-bit vmovdqa / vmovntdq)
 *   - -Z: AVX-512 (512-bit vmovdqa64 / vmovntdq), optional -Z1/-Z2/-Z4 burst
 *   - -l: stride in bytes (default 64)
 *   - -d: single delay override for loaded_latency (-1 = full sweep)
 */
static bw_simd_width_t g_simd_width = BW_SIMD_SSE2;
static int g_z_burst = 0;
static size_t g_stride_bytes = 64;
static int g_single_delay = -1;

typedef struct {
	bool enabled;
	traffic_type_t traffic;
	bool random_access;
	size_t buf_size_kb;
	int alloc_node;
	int alloc_node2;
	int mix_ratio;
} per_thread_cfg_t;

static per_thread_cfg_t g_per_thread_cfg[MAX_CPUS];
static bool g_has_per_thread_cfg = false;

static uint32_t g_pattern_words[256 * 16];
static int g_pattern_lines = 0;
static bool g_physaddr_dump = false;
static pthread_mutex_t g_physaddr_mutex = PTHREAD_MUTEX_INITIALIZER;

void bw_set_physaddr_dump(bool enabled)
{
	g_physaddr_dump = enabled;
}

/*
 * Dump virtual-to-physical 4 KiB page mapping via /proc/self/pagemap,
 * matching intel/mlc FUN_0010b8d0 (--physaddr_dump in loaded_latency).
 */
static void dump_buffer_physaddrs(void *buf, size_t len)
{
	if (!buf || len == 0) {
		fprintf(stderr, "Invalid buffer or length\n");
		return;
	}

	volatile uint8_t *p = (volatile uint8_t *)buf;
	for (size_t off = 0; off < len; off += 4096) {
		p[off] = p[off];
	}

	pthread_mutex_lock(&g_physaddr_mutex);
	int fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0) {
		perror("open /proc/self/pagemap");
		pthread_mutex_unlock(&g_physaddr_mutex);
		return;
	}

	printf("Mapping buffer at %p (length: %zu bytes):\n", buf, len);
	uint64_t first_entry = 0;
	uintptr_t first_vpn = ((uintptr_t)buf) >> 12;
	bool pfn_readable = false;
	if (pread(fd, &first_entry, sizeof(first_entry),
		  (off_t)(first_vpn * sizeof(uint64_t))) == sizeof(first_entry)) {
		bool present = ((first_entry >> 63) & 1ULL) != 0;
		uint64_t pfn = first_entry & ((1ULL << 55) - 1ULL);
		pfn_readable = (!present || pfn != 0);
	}

	if (!pfn_readable) {
		puts("Physical addresses are not readable by this process. The kernel zeroes");
		puts("the page frame numbers in /proc/self/pagemap for anyone without");
		puts("CAP_SYS_ADMIN, so --physaddr_dump has to be run as root to report");
		puts("anything. The pages themselves are present.");
		close(fd);
		pthread_mutex_unlock(&g_physaddr_mutex);
		return;
	}

	for (size_t off = 0; off < len; off += 4096) {
		uintptr_t va = (uintptr_t)buf + off;
		size_t page_idx = off >> 12;
		uint64_t entry = 0;
		if (pread(fd, &entry, sizeof(entry),
			  (off_t)((va >> 12) * sizeof(uint64_t))) == sizeof(entry) &&
		    ((entry >> 63) & 1ULL) != 0) {
			uint64_t pa = ((entry & ((1ULL << 55) - 1ULL)) << 12) |
				      (va & 0xfffULL);
			printf("Buffer_VAToPA_Info: \tPage \t%zu\t: Virtual \t%p\t -> Physical \t0x%012lx\n",
			       page_idx, (void *)va, (unsigned long)pa);
		} else {
			printf("Buffer_VAToPA_Info: \tPage \t%zu\t: Virtual \t%p\t -> \t[not present]\n",
			       page_idx, (void *)va);
		}
	}

	close(fd);
	fflush(stdout);
	pthread_mutex_unlock(&g_physaddr_mutex);
}

/*
 * Delay sweep tables matching intel/mlc FUN_001295d0 & FUN_00123c60:
 *   - .rodata 0x13d480: {0, 15, 30, 45, 60, 75, 90, -1} for --max_bandwidth
 *   - .data   0x350040 overwritten with {0, -1} for --peak_injection_bandwidth
 */
static const int g_max_bw_delays[] = { 0, 15, 30, 45, 60, 75, 90, -1 };
static const int g_peak_bw_delays[] = { 0, -1 };

static int conc_parse_cpu_mask(const char *mask_str, int total_cpus,
			       int *out_cpus, int max_out);
static int conc_parse_cpu_list(const char *list_str, int total_cpus,
			       int *out_cpus, int max_out);

void bw_clear_pattern_file(void)
{
	g_pattern_lines = 0;
}

/*
 * Parse -M <pattern-file> matching intel/mlc FUN_0011e360 & FUN_0011ce50:
 *   Up to 256 non-empty lines, each containing 128 hex characters (64B line),
 *   decoded right-to-left in 8-hex-char (32-bit) words.
 */
int bw_load_pattern_file(const char *path)
{
	bw_clear_pattern_file();
	if (!path || *path == '\0')
		return 22;

	FILE *f = fopen(path, "r");
	if (!f) {
		printf("ERROR - Unable to open the pattern input file %s. Exiting..\n",
		       path);
		return 13;
	}

	char line[256];
	int nlines = 0;
	while (nlines < 256 && fgets(line, 200, f) != NULL) {
		size_t len = strlen(line);
		if (len == 1 && line[0] == '\n')
			continue;
		if (len > 0 && line[len - 1] == '\n') {
			line[--len] = '\0';
		}
		if (len > 0 && line[len - 1] == '\r') {
			line[--len] = '\0';
		}
		if (len != 128) {
			printf("ERROR - The pattern file should have exactly 128 characters in each line (you have %zu). Exiting.\n",
			       len);
			fclose(f);
			return 22;
		}
		for (int w = 0; w < 16; w++) {
			size_t off = (size_t)(128 - (w + 1) * 8);
			char chunk[9];
			memcpy(chunk, line + off, 8);
			chunk[8] = '\0';
			g_pattern_words[nlines * 16 + w] =
			    (uint32_t)strtoul(chunk, NULL, 16);
		}
		nlines++;
	}
	fclose(f);
	g_pattern_lines = nlines;
	return 0;
}

static void fill_buffer_with_pattern(void *buf, size_t bytes)
{
	if (!buf || g_pattern_lines <= 0)
		return;
	uint32_t *dst = (uint32_t *)buf;
	size_t nwords = bytes >> 2;
	size_t pat_words = (size_t)g_pattern_lines * 16ULL;
	size_t idx = 0;
	for (size_t i = 0; i < nwords; i++) {
		dst[i] = g_pattern_words[idx++];
		if (idx >= pat_words)
			idx = 0;
	}
}

void bw_set_simd_options(bw_simd_width_t width, int z_burst)
{
	g_simd_width = width;
	g_z_burst = z_burst;
}

void bw_set_stride(int stride_bytes)
{
	if (stride_bytes > 0)
		g_stride_bytes = (size_t)stride_bytes;
}

void bw_set_single_delay(int delay_override)
{
	g_single_delay = delay_override;
}

void bw_clear_per_thread_config(void)
{
	memset(g_per_thread_cfg, 0, sizeof(g_per_thread_cfg));
	g_has_per_thread_cfg = false;
}

bool bw_has_per_thread_config(void)
{
	return g_has_per_thread_cfg;
}

static bool parse_traffic_token(const char *tok, traffic_type_t *out)
{
	if (!tok || !out)
		return false;
	if (strcmp(tok, "R") == 0 || strcmp(tok, "r") == 0 ||
	    strcmp(tok, "W1") == 0 || strcmp(tok, "w1") == 0) {
		*out = TRAFFIC_ALL_READS;
		return true;
	}
	if (tok[0] == 'W' || tok[0] == 'w') {
		char *endp = NULL;
		long v = strtol(tok + 1, &endp, 10);
		if (endp && *endp == '\0') {
			if ((v >= 2 && v <= 12) || v == 19 ||
			    v == 21 || v == 23 || v == 27) {
				*out = (traffic_type_t)v;
				return true;
			}
		}
	}
	return false;
}

static bool apply_cpu_range_cfg(const char *range_str, int total_cpus,
				const per_thread_cfg_t *tmpl)
{
	char buf[256];
	strncpy(buf, range_str, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	char *saveptr = NULL;
	char *part = strtok_r(buf, ",", &saveptr);
	while (part) {
		char *dash = strchr(part, '-');
		long first, last;
		char *end1 = NULL, *end2 = NULL;
		if (dash) {
			*dash = '\0';
			first = strtol(part, &end1, 10);
			last = strtol(dash + 1, &end2, 10);
			if (!end1 || *end1 != '\0' || !end2 || *end2 != '\0')
				return false;
		} else {
			first = strtol(part, &end1, 10);
			last = first;
			if (!end1 || *end1 != '\0')
				return false;
		}
		if (first < 0 || last < first || last >= total_cpus ||
		    last >= MAX_CPUS) {
			fprintf(stderr, "Invalid string %s in cpu range field\n",
				range_str);
			return false;
		}
		for (long c = first; c <= last; c++) {
			g_per_thread_cfg[c] = *tmpl;
			g_per_thread_cfg[c].enabled = true;
		}
		part = strtok_r(NULL, ",", &saveptr);
	}
	return true;
}

/*
 * Parse -o <per-thread-config-file> matching intel/mlc FUN_0011f210:
 *   5 fields: <cpus> <R|Wn> <seq|rand> <size_kb> <dram|pmem>
 *   6 fields: <cpus> <R|Wn> <seq|rand> <size_kb> <dram|pmem> <node1>
 *   9 fields: <cpus> <W21|W23|W27> <seq> <size_kb> <dram|pmem> <node1> <dram|pmem> <node2> <ratio>
 */
int bw_load_per_thread_config(const char *path, int total_cpus, int num_nodes)
{
	bw_clear_per_thread_config();
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "Unable to open the input perthread file: %s\n",
			path);
		return -1;
	}

	printf("Per-thread configuration from %s\n", path);

	char line[512];
	int line_no = 0;
	int configured = 0;

	while (fgets(line, sizeof(line), f)) {
		line_no++;
		char *p = line;
		while (*p && isspace((unsigned char)*p))
			p++;
		if (*p == '\0' || *p == '#')
			continue;

		char *fields[12];
		int nf = 0;
		char *saveptr = NULL;
		char *tok = strtok_r(p, " \t\r\n", &saveptr);
		while (tok && nf < 12) {
			fields[nf++] = tok;
			tok = strtok_r(NULL, " \t\r\n", &saveptr);
		}
		if (nf == 0)
			continue;

		if (nf != 5 && nf != 6 && nf != 7 && nf != 9) {
			fprintf(stderr,
				"Error in %s config file - Expected only either 5, 6, 7 or 9 parameters in line# %d \n",
				path, line_no);
			fclose(f);
			return -1;
		}

		per_thread_cfg_t cfg;
		memset(&cfg, 0, sizeof(cfg));
		cfg.alloc_node = -1;
		cfg.alloc_node2 = -1;
		cfg.mix_ratio = 0;

		if (!parse_traffic_token(fields[1], &cfg.traffic)) {
			fprintf(stderr,
				"Invalid option in for traffic type in line# %d in per-thread-config file %s\n",
				line_no, path);
			fclose(f);
			return -1;
		}

		bool is_dual = (cfg.traffic == TRAFFIC_DUAL_ALL_READS ||
				cfg.traffic == TRAFFIC_DUAL_3_1_RW ||
				cfg.traffic == TRAFFIC_DUAL_2_1_NTW);
		if (is_dual && nf != 9) {
			fprintf(stderr,
				"Invalid number of inputs for this traffic type in line# %d in per-thread-config file %s\n",
				line_no, path);
			fclose(f);
			return -1;
		}

		if (strcmp(fields[2], "seq") == 0) {
			cfg.random_access = false;
		} else if (strcmp(fields[2], "rand") == 0) {
			cfg.random_access = true;
		} else {
			fprintf(stderr,
				"Only \"seq\" or \"rand\" allowed in 3rd field in line# %d in per-thread-config file %s\n",
				line_no, path);
			fclose(f);
			return -1;
		}

		long sz_kb = strtol(fields[3], NULL, 10);
		if (sz_kb <= 0)
			sz_kb = 100000;
		cfg.buf_size_kb = (size_t)sz_kb;

		if (strcmp(fields[4], "dram") != 0 &&
		    strcmp(fields[4], "pmem") != 0) {
			fprintf(stderr,
				"Only \"dram\" or \"pmem\" allowed in 5th field in line# %d in per-thread-config file %s\n",
				line_no, path);
			fclose(f);
			return -1;
		}

		if (nf >= 6) {
			int n1 = atoi(fields[5]);
			if (num_nodes > 0 && (n1 < 0 || n1 >= num_nodes)) {
				fprintf(stderr,
					"Numa node id %d in 6th field in line# %d in per-thread-config file %s: this machine has %d nodes, so the ids run 0 to %d\n",
					n1, line_no, path, num_nodes,
					num_nodes - 1);
				fclose(f);
				return -1;
			}
			cfg.alloc_node = n1;
		}

		if (nf == 9) {
			int n2 = atoi(fields[7]);
			if (num_nodes > 0 && (n2 < 0 || n2 >= num_nodes)) {
				fprintf(stderr,
					"Numa node id %d in 8th field in line# %d in per-thread-config file %s\n",
					n2, line_no, path);
				fclose(f);
				return -1;
			}
			cfg.alloc_node2 = n2;
			cfg.mix_ratio = atoi(fields[8]);

			if (cfg.traffic == TRAFFIC_DUAL_ALL_READS) {
				if (cfg.mix_ratio != 10 &&
				    cfg.mix_ratio != 25 &&
				    cfg.mix_ratio != 50) {
					fprintf(stderr,
						"Error in per-thread-config file: address stream ratio %d is not supported for W21 traffic.\nW21 supports ratios 10, 25 and 50.\n",
						cfg.mix_ratio);
					fclose(f);
					return -1;
				}
			} else if (cfg.mix_ratio != 10 && cfg.mix_ratio != 25) {
				fprintf(stderr,
					"Error in per-thread-config file: address stream ratio %d is not supported for W%d traffic.\nW%d supports ratios 10 and 25.\n",
					cfg.mix_ratio, (int)cfg.traffic,
					(int)cfg.traffic);
				fclose(f);
				return -1;
			}
		}

		if (!apply_cpu_range_cfg(fields[0], total_cpus, &cfg)) {
			fclose(f);
			return -1;
		}
		configured++;
	}

	fclose(f);
	if (configured == 0)
		return -1;

	for (int c = 0; c < total_cpus && c < MAX_CPUS; c++) {
		if (!g_per_thread_cfg[c].enabled)
			continue;
		const per_thread_cfg_t *tc = &g_per_thread_cfg[c];
		char pat[16];
		if (tc->traffic == TRAFFIC_ALL_READS)
			snprintf(pat, sizeof(pat), "R");
		else
			snprintf(pat, sizeof(pat), "W%d", (int)tc->traffic);

		const char *acc = tc->random_access ? "rand" : "seq";
		if (tc->mix_ratio > 0) {
			printf(
			    "Thread id %3d, traffic pattern %3s, %4s access, %8zu KiB buffer, dram memory, access from numa node %d, dram memory, access from numa node %d\taddress mix ratio %d\n",
			    c, pat, acc, tc->buf_size_kb, tc->alloc_node,
			    tc->alloc_node2, tc->mix_ratio);
		} else if (tc->alloc_node >= 0) {
			printf(
			    "Thread id %3d, traffic pattern %3s, %4s access, %8zu KiB buffer, dram memory, access from numa node %d\n",
			    c, pat, acc, tc->buf_size_kb, tc->alloc_node);
		} else {
			printf(
			    "Thread id %3d, traffic pattern %3s, %4s access, %8zu KiB buffer, dram memory, access from local node\n",
			    c, pat, acc, tc->buf_size_kb);
		}
	}

	g_has_per_thread_cfg = true;
	return 0;
}

/*
 * Per-thread separated read/write buffer set matching ThruputMemAllocThread
 * (FUN_0010bc10, param_1[6]=r1, param_1[7]=r2, param_1[9]=w1, param_1[10]=w2).
 */
typedef struct {
	uint8_t *r1;
	uint8_t *r2;
	uint8_t *w1;
	uint8_t *w2;
	size_t logical_size;
	size_t r1_alloc;
	size_t r2_alloc;
	size_t w1_alloc;
	size_t w2_alloc;
} thruput_buffers_t;

static void free_thruput_buffers(thruput_buffers_t *b)
{
	if (b->r1) {
		free_mem_onnode(b->r1, b->r1_alloc);
		b->r1 = NULL;
	}
	if (b->r2) {
		free_mem_onnode(b->r2, b->r2_alloc);
		b->r2 = NULL;
	}
	if (b->w1) {
		free_mem_onnode(b->w1, b->w1_alloc);
		b->w1 = NULL;
	}
	if (b->w2) {
		free_mem_onnode(b->w2, b->w2_alloc);
		b->w2 = NULL;
	}
}

static bool alloc_thruput_buffers(thruput_buffers_t *b, size_t req_size,
				  traffic_type_t traffic, int node1, int node2,
				  size_t stride)
{
	memset(b, 0, sizeof(*b));
	if (req_size < 8192)
		req_size = 8192;
	/* Align logical size to 8 KiB so size and size/2 are 4 KiB-aligned */
	size_t size = req_size & ~(size_t)8191;
	/* Guard tail prevents unrolled multi-line steps (up to 18*stride) from faulting */
	size_t guard = 32 * stride + 4096;

	b->logical_size = size;

	bool need_r1 = false, need_r2 = false, need_w1 = false, need_w2 = false;
	size_t r_logical = size;

	switch (traffic) {
	case TRAFFIC_ALL_READS:
		need_r1 = true;
		break;
	case TRAFFIC_1_1_READ_WRITE:
	case TRAFFIC_0_1_NT_WRITE:
		need_w1 = true;
		break;
	case TRAFFIC_STREAM_TRIAD:
	case TRAFFIC_STREAM_TRIAD_RFO:
		need_r1 = true;
		need_r2 = true;
		need_w1 = true;
		r_logical = size / 2;
		break;
	case TRAFFIC_DUAL_ALL_READS:
		need_r1 = true;
		need_r2 = true;
		break;
	case TRAFFIC_DUAL_3_1_RW:
	case TRAFFIC_DUAL_2_1_NTW:
		need_r1 = true;
		need_r2 = true;
		need_w1 = true;
		need_w2 = true;
		break;
	default:
		/* W2, W3, W4, W7, W8, W9, W12, W19 */
		need_r1 = true;
		need_w1 = true;
		break;
	}

	int second_node = (traffic == TRAFFIC_DUAL_ALL_READS ||
			   traffic == TRAFFIC_DUAL_3_1_RW ||
			   traffic == TRAFFIC_DUAL_2_1_NTW) ? node2 : node1;
	hugepage_type_t hp_mode = numa_get_hugepage_mode();

	if (need_r1) {
		b->r1_alloc = r_logical + guard;
		b->r1 = (uint8_t *)alloc_mem_onnode(b->r1_alloc, node1,
						    hp_mode);
		if (!b->r1)
			goto fail;
		fill_buffer_with_pattern(b->r1, r_logical);
		if (g_physaddr_dump)
			dump_buffer_physaddrs(b->r1, b->r1_alloc);
	}
	if (need_r2) {
		b->r2_alloc = r_logical + guard;
		b->r2 = (uint8_t *)alloc_mem_onnode(b->r2_alloc, second_node,
						    hp_mode);
		if (!b->r2)
			goto fail;
		fill_buffer_with_pattern(b->r2, r_logical);
		if (g_physaddr_dump)
			dump_buffer_physaddrs(b->r2, b->r2_alloc);
	}
	if (need_w1) {
		b->w1_alloc = size + guard;
		b->w1 = (uint8_t *)alloc_mem_onnode(b->w1_alloc, node1,
						    hp_mode);
		if (!b->w1)
			goto fail;
		fill_buffer_with_pattern(b->w1, size);
		if (g_physaddr_dump)
			dump_buffer_physaddrs(b->w1, b->w1_alloc);
	}
	if (need_w2) {
		b->w2_alloc = size + guard;
		b->w2 = (uint8_t *)alloc_mem_onnode(b->w2_alloc, second_node,
						    hp_mode);
		if (!b->w2)
			goto fail;
		fill_buffer_with_pattern(b->w2, size);
		if (g_physaddr_dump)
			dump_buffer_physaddrs(b->w2, b->w2_alloc);
	}
	return true;

fail:
	free_thruput_buffers(b);
	return false;
}

/*
 * Common tail for every MLC throughput microkernel (FUN_0010e030..FUN_00119159):
 *   - Amortizes stop_flag check once every 10,000 (0x2710) inner iterations
 *   - Runs calibrated integer inc/cmp/jl delay loop
 *   - Loops back to 1b while primary pointer < end_ptr
 */
#define KERNEL_LOOP_TAIL(ptr_op) \
	"incq %[icnt]\n\t" \
	"cmpq $10000, %[icnt]\n\t" \
	"jb 2f\n\t" \
	"cmpl $0, (%[stop])\n\t" \
	"jne 4f\n\t" \
	"xorq %[icnt], %[icnt]\n\t" \
	"2:\n\t" \
	"xorl %[dcnt], %[dcnt]\n\t" \
	"3:\n\t" \
	"incl %[dcnt]\n\t" \
	"cmpl %[dly], %[dcnt]\n\t" \
	"jl 3b\n\t" \
	"cmpq %[end], " ptr_op "\n\t" \
	"jl 1b\n\t" \
	"4:\n\t"

/* SSE2 64B cache-line primitives (4x 16B) */
#define SSE_RD(reg) \
	"movdqa 0(" reg "), %%xmm1\n\t" \
	"movdqa 16(" reg "), %%xmm1\n\t" \
	"movdqa 32(" reg "), %%xmm1\n\t" \
	"movdqa 48(" reg "), %%xmm1\n\t" \
	"addq %[stride], " reg "\n\t"

#define SSE_WR(reg) \
	"movdqa %%xmm0, 0(" reg ")\n\t" \
	"movdqa %%xmm0, 16(" reg ")\n\t" \
	"movdqa %%xmm0, 32(" reg ")\n\t" \
	"movdqa %%xmm0, 48(" reg ")\n\t" \
	"addq %[stride], " reg "\n\t"

#define SSE_NTW(reg) \
	"movntdq %%xmm0, 0(" reg ")\n\t" \
	"movntdq %%xmm0, 16(" reg ")\n\t" \
	"movntdq %%xmm0, 32(" reg ")\n\t" \
	"movntdq %%xmm0, 48(" reg ")\n\t" \
	"addq %[stride], " reg "\n\t"

/* AVX2 64B cache-line primitives (2x 32B) */
#define AVX2_RD(reg) \
	"vmovdqa 0(" reg "), %%ymm1\n\t" \
	"vmovdqa 32(" reg "), %%ymm1\n\t" \
	"addq %[stride], " reg "\n\t"

#define AVX2_WR(reg) \
	"vmovdqa %%ymm0, 0(" reg ")\n\t" \
	"vmovdqa %%ymm0, 32(" reg ")\n\t" \
	"addq %[stride], " reg "\n\t"

#define AVX2_NTW(reg) \
	"vmovntdq %%ymm0, 0(" reg ")\n\t" \
	"vmovntdq %%ymm0, 32(" reg ")\n\t" \
	"addq %[stride], " reg "\n\t"

/* AVX-512 64B cache-line primitives (1x 64B) */
#define AVX512_RD(reg) \
	"vmovdqa64 0(" reg "), %%zmm1\n\t" \
	"addq %[stride], " reg "\n\t"

#define AVX512_WR(reg) \
	"vmovdqa64 %%zmm0, 0(" reg ")\n\t" \
	"addq %[stride], " reg "\n\t"

#define AVX512_NTW(reg) \
	"vmovntdq %%zmm0, 0(" reg ")\n\t" \
	"addq %[stride], " reg "\n\t"

static uint64_t exec_pass_sse2(const thruput_buffers_t *b,
			       traffic_type_t traffic, int delay,
			       size_t stride, const volatile int *stop_ptr)
{
	uint8_t *r1 = b->r1;
	uint8_t *r2 = b->r2;
	uint8_t *w1 = b->w1;
	size_t size = b->logical_size;
	uint64_t icnt = 0;
	int dcnt = 0;

	switch (traffic) {
	case TRAFFIC_ALL_READS: {
		/* FUN_00118165: 4x 1R(r1) */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"1:\n\t"
			SSE_RD("%[r1]") SSE_RD("%[r1]")
			SSE_RD("%[r1]") SSE_RD("%[r1]")
			KERNEL_LOOP_TAIL("%[r1]")
			: [r1] "+r"(r1), [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "xmm1", "cc", "memory"
		);
		return ((uint64_t)(r1 - b->r1) / stride) * 64ULL;
	}
	case TRAFFIC_2_1_READ_WRITE: {
		/* FUN_0010f909: 2x [1R(r1) + 1W(w1)], 3 DRAM lines per R+W */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"movq %[end], %%xmm0\n\t"
			"1:\n\t"
			SSE_RD("%[r1]") SSE_WR("%[w1]")
			SSE_RD("%[r1]") SSE_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "xmm0", "xmm1", "cc", "memory"
		);
		return (((uint64_t)(r1 - b->r1) / stride) * 3ULL) * 64ULL;
	}
	case TRAFFIC_3_1_READ_WRITE: {
		/* FUN_0010e4b5: 4x [2R(r1) + 1W(w1)], 4 DRAM lines per 2R+1W */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"movq %[end], %%xmm0\n\t"
			"1:\n\t"
			SSE_RD("%[r1]") SSE_RD("%[r1]") SSE_WR("%[w1]")
			SSE_RD("%[r1]") SSE_RD("%[r1]") SSE_WR("%[w1]")
			SSE_RD("%[r1]") SSE_RD("%[r1]") SSE_WR("%[w1]")
			SSE_RD("%[r1]") SSE_RD("%[r1]") SSE_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "xmm0", "xmm1", "cc", "memory"
		);
		return (((uint64_t)(r1 - b->r1) / stride) * 2ULL) * 64ULL;
	}
	case TRAFFIC_3_2_READ_WRITE: {
		/* FUN_001105e3: 1x [1R(r1) + 2W(w1)], bound w1+size, 5 DRAM lines per 2 w1 steps */
		uint8_t *end = w1 + size;
		__asm__ volatile (
			"movq %[end], %%xmm0\n\t"
			"1:\n\t"
			SSE_RD("%[r1]") SSE_WR("%[w1]") SSE_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[w1]")
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "xmm0", "xmm1", "cc", "memory"
		);
		uint64_t steps = (uint64_t)(w1 - b->w1) / stride;
		return ((steps * 5ULL) / 2ULL) * 64ULL;
	}
	case TRAFFIC_1_1_READ_WRITE: {
		/* FUN_001109da: 4x [1W(w1)] (0 explicit reads; 1 RFO + 1 write = 2 DRAM lines per step) */
		uint8_t *end = w1 + size;
		__asm__ volatile (
			"movq %[end], %%xmm0\n\t"
			"1:\n\t"
			SSE_WR("%[w1]") SSE_WR("%[w1]")
			SSE_WR("%[w1]") SSE_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[w1]")
			: [w1] "+r"(w1), [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "xmm0", "cc", "memory"
		);
		return (((uint64_t)(w1 - b->w1) / stride) * 2ULL) * 64ULL;
	}
	case TRAFFIC_0_1_NT_WRITE: {
		/* FUN_00110d36: 1x [1 NTW(w1)], 1 DRAM line per step */
		uint8_t *end = w1 + size;
		__asm__ volatile (
			"movq %[end], %%xmm0\n\t"
			"1:\n\t"
			SSE_NTW("%[w1]")
			KERNEL_LOOP_TAIL("%[w1]")
			: [w1] "+r"(w1), [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "xmm0", "cc", "memory"
		);
		return ((uint64_t)(w1 - b->w1) / stride) * 64ULL;
	}
	case TRAFFIC_2_1_NT_WRITE: {
		/* FUN_0010f00d: 2x [2R(r1) + 1 NTW(w1)], 3 DRAM lines per 2 r1 steps */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"movq %[end], %%xmm0\n\t"
			"1:\n\t"
			SSE_RD("%[r1]") SSE_RD("%[r1]") SSE_NTW("%[w1]")
			SSE_RD("%[r1]") SSE_RD("%[r1]") SSE_NTW("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "xmm0", "xmm1", "cc", "memory"
		);
		uint64_t steps = (uint64_t)(r1 - b->r1) / stride;
		return ((steps * 3ULL) / 2ULL) * 64ULL;
	}
	case TRAFFIC_1_1_NT_WRITE: {
		/* FUN_0010f597: 2x [1R(r1) + 1 NTW(w1)], 2 DRAM lines per r1 step */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"movq %[end], %%xmm0\n\t"
			"1:\n\t"
			SSE_RD("%[r1]") SSE_NTW("%[w1]")
			SSE_RD("%[r1]") SSE_NTW("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "xmm0", "xmm1", "cc", "memory"
		);
		return (((uint64_t)(r1 - b->r1) / stride) * 2ULL) * 64ULL;
	}
	case TRAFFIC_3_1_NT_WRITE: {
		/* FUN_0010f49e: 1x [3R(r1) + 1 NTW(w1)], 4 DRAM lines per 3 r1 steps */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"movq %[end], %%xmm0\n\t"
			"1:\n\t"
			SSE_RD("%[r1]") SSE_RD("%[r1]") SSE_RD("%[r1]")
			SSE_NTW("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "xmm0", "xmm1", "cc", "memory"
		);
		uint64_t steps = (uint64_t)(r1 - b->r1) / stride;
		return ((steps * 4ULL) / 3ULL) * 64ULL;
	}
	case TRAFFIC_STREAM_TRIAD: {
		/* FUN_0010e9c2: 2x [1R(r1) + 1R(r2) + 1 NTW(w1)], bound r1 + size/2, 3 DRAM lines per r1 step */
		uint8_t *end = r1 + (size / 2);
		__asm__ volatile (
			"movq %[end], %%xmm0\n\t"
			"1:\n\t"
			SSE_RD("%[r1]") SSE_RD("%[r2]") SSE_NTW("%[w1]")
			SSE_RD("%[r1]") SSE_RD("%[r2]") SSE_NTW("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			: [r1] "+r"(r1), [r2] "+r"(r2), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "xmm0", "xmm1", "cc", "memory"
		);
		return (((uint64_t)(r1 - b->r1) / stride) * 3ULL) * 64ULL;
	}
	case TRAFFIC_STREAM_TRIAD_RFO: {
		/* FUN_0010e030: 2x [1R(r1) + 1R(r2) + 1W(w1)], bound r1 + size/2, 4 DRAM lines per r1 step */
		uint8_t *end = r1 + (size / 2);
		__asm__ volatile (
			"movq %[end], %%xmm0\n\t"
			"1:\n\t"
			SSE_RD("%[r1]") SSE_RD("%[r2]") SSE_WR("%[w1]")
			SSE_RD("%[r1]") SSE_RD("%[r2]") SSE_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			: [r1] "+r"(r1), [r2] "+r"(r2), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "xmm0", "xmm1", "cc", "memory"
		);
		return (((uint64_t)(r1 - b->r1) / stride) * 4ULL) * 64ULL;
	}
	case TRAFFIC_4_1_READ_WRITE: {
		/* FUN_001101da: 1x [3R(r1) + 1W(w1)], 5 DRAM lines per 3 r1 steps */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"movq %[end], %%xmm0\n\t"
			"1:\n\t"
			SSE_RD("%[r1]") SSE_RD("%[r1]") SSE_RD("%[r1]")
			SSE_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "xmm0", "xmm1", "cc", "memory"
		);
		uint64_t steps = (uint64_t)(r1 - b->r1) / stride;
		return ((steps * 5ULL) / 3ULL) * 64ULL;
	}
	case TRAFFIC_9_1_READ_WRITE: {
		/* FUN_0010fa17: 2x [8R(r1) + 1W(w1)], 10 DRAM lines per 8 r1 steps */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"movq %[end], %%xmm0\n\t"
			"1:\n\t"
			SSE_RD("%[r1]") SSE_RD("%[r1]") SSE_RD("%[r1]") SSE_RD("%[r1]")
			SSE_RD("%[r1]") SSE_RD("%[r1]") SSE_RD("%[r1]") SSE_RD("%[r1]")
			SSE_WR("%[w1]")
			SSE_RD("%[r1]") SSE_RD("%[r1]") SSE_RD("%[r1]") SSE_RD("%[r1]")
			SSE_RD("%[r1]") SSE_RD("%[r1]") SSE_RD("%[r1]") SSE_RD("%[r1]")
			SSE_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "xmm0", "xmm1", "cc", "memory"
		);
		uint64_t steps = (uint64_t)(r1 - b->r1) / stride;
		return ((steps * 10ULL) >> 3) * 64ULL;
	}
	default:
		return 0;
	}
}

static uint64_t exec_pass_avx2(const thruput_buffers_t *b,
			       traffic_type_t traffic, int delay,
			       size_t stride, int mix_ratio,
			       const volatile int *stop_ptr)
{
	uint8_t *r1 = b->r1;
	uint8_t *r2 = b->r2;
	uint8_t *w1 = b->w1;
	uint8_t *w2 = b->w2;
	size_t size = b->logical_size;
	uint64_t icnt = 0;
	int dcnt = 0;

	switch (traffic) {
	case TRAFFIC_ALL_READS: {
		/* FUN_00118252: 8x 1R(r1) */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"1:\n\t"
			AVX2_RD("%[r1]") AVX2_RD("%[r1]")
			AVX2_RD("%[r1]") AVX2_RD("%[r1]")
			AVX2_RD("%[r1]") AVX2_RD("%[r1]")
			AVX2_RD("%[r1]") AVX2_RD("%[r1]")
			KERNEL_LOOP_TAIL("%[r1]")
			"vzeroupper\n\t"
			: [r1] "+r"(r1), [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "ymm1", "cc", "memory"
		);
		return ((uint64_t)(r1 - b->r1) / stride) * 64ULL;
	}
	case TRAFFIC_2_1_READ_WRITE: {
		/* FUN_0010fc4f: 2x [1R(r1) + 1W(w1)] */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX2_RD("%[r1]") AVX2_WR("%[w1]")
			AVX2_RD("%[r1]") AVX2_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			"vzeroupper\n\t"
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "ymm0", "ymm1", "cc", "memory"
		);
		return (((uint64_t)(r1 - b->r1) / stride) * 3ULL) * 64ULL;
	}
	case TRAFFIC_3_1_READ_WRITE: {
		/* FUN_0010e64b: 4x [2R(r1) + 1W(w1)] */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_WR("%[w1]")
			AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_WR("%[w1]")
			AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_WR("%[w1]")
			AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			"vzeroupper\n\t"
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "ymm0", "ymm1", "cc", "memory"
		);
		return (((uint64_t)(r1 - b->r1) / stride) * 2ULL) * 64ULL;
	}
	case TRAFFIC_3_2_READ_WRITE: {
		/* FUN_00110797: 1x [1R(r1) + 2W(w1)], bound w1+size */
		uint8_t *end = w1 + size;
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX2_RD("%[r1]") AVX2_WR("%[w1]") AVX2_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[w1]")
			"vzeroupper\n\t"
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "ymm0", "ymm1", "cc", "memory"
		);
		uint64_t steps = (uint64_t)(w1 - b->w1) / stride;
		return ((steps * 5ULL) / 2ULL) * 64ULL;
	}
	case TRAFFIC_1_1_READ_WRITE: {
		/* FUN_00110ad7: 4x [1W(w1)] */
		uint8_t *end = w1 + size;
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX2_WR("%[w1]") AVX2_WR("%[w1]")
			AVX2_WR("%[w1]") AVX2_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[w1]")
			"vzeroupper\n\t"
			: [w1] "+r"(w1), [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "ymm0", "cc", "memory"
		);
		return (((uint64_t)(w1 - b->w1) / stride) * 2ULL) * 64ULL;
	}
	case TRAFFIC_0_1_NT_WRITE: {
		/* FUN_00110e27: 1x [1 NTW(w1)] */
		uint8_t *end = w1 + size;
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX2_NTW("%[w1]")
			KERNEL_LOOP_TAIL("%[w1]")
			"vzeroupper\n\t"
			: [w1] "+r"(w1), [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "ymm0", "cc", "memory"
		);
		return ((uint64_t)(w1 - b->w1) / stride) * 64ULL;
	}
	case TRAFFIC_2_1_NT_WRITE: {
		/* FUN_0010f147: 2x [2R(r1) + 1 NTW(w1)] */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_NTW("%[w1]")
			AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_NTW("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			"vzeroupper\n\t"
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "ymm0", "ymm1", "cc", "memory"
		);
		uint64_t steps = (uint64_t)(r1 - b->r1) / stride;
		return ((steps * 3ULL) / 2ULL) * 64ULL;
	}
	case TRAFFIC_1_1_NT_WRITE: {
		/* FUN_0010f697: 2x [1R(r1) + 1 NTW(w1)] */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX2_RD("%[r1]") AVX2_NTW("%[w1]")
			AVX2_RD("%[r1]") AVX2_NTW("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			"vzeroupper\n\t"
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "ymm0", "ymm1", "cc", "memory"
		);
		return (((uint64_t)(r1 - b->r1) / stride) * 2ULL) * 64ULL;
	}
	case TRAFFIC_3_1_NT_WRITE:
		/* W9 has no AVX2/AVX512 kernel in intel/mlc; uses SSE2 */
		return exec_pass_sse2(b, traffic, delay, stride, stop_ptr);
	case TRAFFIC_STREAM_TRIAD: {
		/* FUN_0010eb07: 2x [1R(r1) + 1R(r2) + 1 NTW(w1)] */
		uint8_t *end = r1 + (size / 2);
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX2_RD("%[r1]") AVX2_RD("%[r2]") AVX2_NTW("%[w1]")
			AVX2_RD("%[r1]") AVX2_RD("%[r2]") AVX2_NTW("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			"vzeroupper\n\t"
			: [r1] "+r"(r1), [r2] "+r"(r2), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "ymm0", "ymm1", "cc", "memory"
		);
		return (((uint64_t)(r1 - b->r1) / stride) * 3ULL) * 64ULL;
	}
	case TRAFFIC_STREAM_TRIAD_RFO: {
		/* FUN_0010e22f: 2x [1R(r1) + 1R(r2) + 1W(w1)] */
		uint8_t *end = r1 + (size / 2);
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX2_RD("%[r1]") AVX2_RD("%[r2]") AVX2_WR("%[w1]")
			AVX2_RD("%[r1]") AVX2_RD("%[r2]") AVX2_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			"vzeroupper\n\t"
			: [r1] "+r"(r1), [r2] "+r"(r2), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "ymm0", "ymm1", "cc", "memory"
		);
		return (((uint64_t)(r1 - b->r1) / stride) * 4ULL) * 64ULL;
	}
	case TRAFFIC_4_1_READ_WRITE: {
		/* FUN_0011039f: 1x [3R(r1) + 1W(w1)] */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_RD("%[r1]")
			AVX2_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			"vzeroupper\n\t"
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "ymm0", "ymm1", "cc", "memory"
		);
		uint64_t steps = (uint64_t)(r1 - b->r1) / stride;
		return ((steps * 5ULL) / 3ULL) * 64ULL;
	}
	case TRAFFIC_9_1_READ_WRITE: {
		/* FUN_0010fd2f: 2x [8R(r1) + 1W(w1)] */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_RD("%[r1]")
			AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_RD("%[r1]")
			AVX2_WR("%[w1]")
			AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_RD("%[r1]")
			AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_RD("%[r1]")
			AVX2_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			"vzeroupper\n\t"
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "ymm0", "ymm1", "cc", "memory"
		);
		uint64_t steps = (uint64_t)(r1 - b->r1) / stride;
		return ((steps * 10ULL) >> 3) * 64ULL;
	}
	case TRAFFIC_DUAL_ALL_READS: {
		/* FUN_001189ad (50%), FUN_00118a6a (25%), FUN_00118b5e (10%) */
		uint8_t *end = r1 + size;
		if (mix_ratio == 50) {
			__asm__ volatile (
				"1:\n\t"
				AVX2_RD("%[r1]") AVX2_RD("%[r2]")
				AVX2_RD("%[r1]") AVX2_RD("%[r2]")
				KERNEL_LOOP_TAIL("%[r1]")
				"vzeroupper\n\t"
				: [r1] "+r"(r1), [r2] "+r"(r2),
				  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
				: [end] "r"(end), [stride] "r"(stride),
				  [stop] "r"(stop_ptr), [dly] "r"(delay)
				: "ymm1", "cc", "memory"
			);
			return (((uint64_t)(r1 - b->r1) / stride) * 2ULL) * 64ULL;
		} else if (mix_ratio == 25) {
			__asm__ volatile (
				"1:\n\t"
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_RD("%[r1]")
				AVX2_RD("%[r2]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_RD("%[r1]")
				AVX2_RD("%[r2]")
				KERNEL_LOOP_TAIL("%[r1]")
				"vzeroupper\n\t"
				: [r1] "+r"(r1), [r2] "+r"(r2),
				  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
				: [end] "r"(end), [stride] "r"(stride),
				  [stop] "r"(stop_ptr), [dly] "r"(delay)
				: "ymm1", "cc", "memory"
			);
			uint64_t steps = (uint64_t)(r1 - b->r1) / stride;
			return ((steps * 4ULL) / 3ULL) * 64ULL;
		} else {
			__asm__ volatile (
				"1:\n\t"
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_RD("%[r1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_RD("%[r1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_RD("%[r1]")
				AVX2_RD("%[r2]")
				KERNEL_LOOP_TAIL("%[r1]")
				"vzeroupper\n\t"
				: [r1] "+r"(r1), [r2] "+r"(r2),
				  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
				: [end] "r"(end), [stride] "r"(stride),
				  [stop] "r"(stop_ptr), [dly] "r"(delay)
				: "ymm1", "cc", "memory"
			);
			uint64_t steps = (uint64_t)(r1 - b->r1) / stride;
			return ((steps * 10ULL) / 9ULL) * 64ULL;
		}
	}
	case TRAFFIC_DUAL_3_1_RW: {
		/* FUN_00118c76 (25%), FUN_00118dd9 (10%) */
		uint8_t *end = r1 + size;
		if (mix_ratio == 25) {
			__asm__ volatile (
				"vmovq %[end], %%xmm0\n\t"
				"1:\n\t"
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_WR("%[w1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_WR("%[w1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_WR("%[w1]")
				AVX2_RD("%[r2]") AVX2_RD("%[r2]") AVX2_WR("%[w2]")
				KERNEL_LOOP_TAIL("%[r1]")
				"vzeroupper\n\t"
				: [r1] "+r"(r1), [r2] "+r"(r2),
				  [w1] "+r"(w1), [w2] "+r"(w2),
				  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
				: [end] "r"(end), [stride] "r"(stride),
				  [stop] "r"(stop_ptr), [dly] "r"(delay)
				: "ymm0", "ymm1", "cc", "memory"
			);
			uint64_t steps = (uint64_t)(r1 - b->r1) / stride;
			return ((steps * 8ULL) / 3ULL) * 64ULL;
		} else {
			__asm__ volatile (
				"vmovq %[end], %%xmm0\n\t"
				"1:\n\t"
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_WR("%[w1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_WR("%[w1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_WR("%[w1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_WR("%[w1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_WR("%[w1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_WR("%[w1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_WR("%[w1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_WR("%[w1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_WR("%[w1]")
				AVX2_RD("%[r2]") AVX2_RD("%[r2]") AVX2_WR("%[w2]")
				KERNEL_LOOP_TAIL("%[r1]")
				"vzeroupper\n\t"
				: [r1] "+r"(r1), [r2] "+r"(r2),
				  [w1] "+r"(w1), [w2] "+r"(w2),
				  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
				: [end] "r"(end), [stride] "r"(stride),
				  [stop] "r"(stop_ptr), [dly] "r"(delay)
				: "ymm0", "ymm1", "cc", "memory"
			);
			uint64_t steps = (uint64_t)(r1 - b->r1) / stride;
			return ((steps * 20ULL) / 9ULL) * 64ULL;
		}
	}
	case TRAFFIC_DUAL_2_1_NTW: {
		/* FUN_00118ff1 (25%), FUN_00119159 (10%) */
		uint8_t *end = r1 + size;
		if (mix_ratio == 25) {
			__asm__ volatile (
				"vmovq %[end], %%xmm0\n\t"
				"1:\n\t"
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_NTW("%[w1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_NTW("%[w1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_NTW("%[w1]")
				AVX2_RD("%[r2]") AVX2_RD("%[r2]") AVX2_NTW("%[w2]")
				KERNEL_LOOP_TAIL("%[r1]")
				"vzeroupper\n\t"
				: [r1] "+r"(r1), [r2] "+r"(r2),
				  [w1] "+r"(w1), [w2] "+r"(w2),
				  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
				: [end] "r"(end), [stride] "r"(stride),
				  [stop] "r"(stop_ptr), [dly] "r"(delay)
				: "ymm0", "ymm1", "cc", "memory"
			);
			return (((uint64_t)(r1 - b->r1) / stride) * 2ULL) * 64ULL;
		} else {
			__asm__ volatile (
				"vmovq %[end], %%xmm0\n\t"
				"1:\n\t"
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_NTW("%[w1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_NTW("%[w1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_NTW("%[w1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_NTW("%[w1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_NTW("%[w1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_NTW("%[w1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_NTW("%[w1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_NTW("%[w1]")
				AVX2_RD("%[r1]") AVX2_RD("%[r1]") AVX2_NTW("%[w1]")
				AVX2_RD("%[r2]") AVX2_RD("%[r2]") AVX2_NTW("%[w2]")
				KERNEL_LOOP_TAIL("%[r1]")
				"vzeroupper\n\t"
				: [r1] "+r"(r1), [r2] "+r"(r2),
				  [w1] "+r"(w1), [w2] "+r"(w2),
				  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
				: [end] "r"(end), [stride] "r"(stride),
				  [stop] "r"(stop_ptr), [dly] "r"(delay)
				: "ymm0", "ymm1", "cc", "memory"
			);
			uint64_t steps = (uint64_t)(r1 - b->r1) / stride;
			return ((steps * 5ULL) / 3ULL) * 64ULL;
		}
	}
	default:
		return 0;
	}
}

__attribute__((target("avx512f")))
static uint64_t exec_pass_avx512(const thruput_buffers_t *b,
				 traffic_type_t traffic, int delay,
				 size_t stride, int z_burst, int mix_ratio,
				 const volatile int *stop_ptr)
{
	uint8_t *r1 = b->r1;
	uint8_t *r2 = b->r2;
	uint8_t *w1 = b->w1;
	size_t size = b->logical_size;
	uint64_t icnt = 0;
	int dcnt = 0;

	switch (traffic) {
	case TRAFFIC_ALL_READS: {
		uint8_t *end = r1 + size;
		if (z_burst == 1) {
			/* FUN_0011835e: -Z1 burst=1 */
			__asm__ volatile (
				"1:\n\t"
				AVX512_RD("%[r1]")
				KERNEL_LOOP_TAIL("%[r1]")
				"vzeroupper\n\t"
				: [r1] "+r"(r1), [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
				: [end] "r"(end), [stride] "r"(stride),
				  [stop] "r"(stop_ptr), [dly] "r"(delay)
				: "zmm1", "cc", "memory"
			);
		} else if (z_burst == 2) {
			/* FUN_00118403: -Z2 burst=2 */
			__asm__ volatile (
				"1:\n\t"
				AVX512_RD("%[r1]") AVX512_RD("%[r1]")
				KERNEL_LOOP_TAIL("%[r1]")
				"vzeroupper\n\t"
				: [r1] "+r"(r1), [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
				: [end] "r"(end), [stride] "r"(stride),
				  [stop] "r"(stop_ptr), [dly] "r"(delay)
				: "zmm1", "cc", "memory"
			);
		} else if (z_burst == 4) {
			/* FUN_001184cc: -Z4 burst=4 */
			__asm__ volatile (
				"1:\n\t"
				AVX512_RD("%[r1]") AVX512_RD("%[r1]")
				AVX512_RD("%[r1]") AVX512_RD("%[r1]")
				KERNEL_LOOP_TAIL("%[r1]")
				"vzeroupper\n\t"
				: [r1] "+r"(r1), [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
				: [end] "r"(end), [stride] "r"(stride),
				  [stop] "r"(stop_ptr), [dly] "r"(delay)
				: "zmm1", "cc", "memory"
			);
		} else {
			/* FUN_0011859e: -Z default burst=8 */
			__asm__ volatile (
				"1:\n\t"
				AVX512_RD("%[r1]") AVX512_RD("%[r1]")
				AVX512_RD("%[r1]") AVX512_RD("%[r1]")
				AVX512_RD("%[r1]") AVX512_RD("%[r1]")
				AVX512_RD("%[r1]") AVX512_RD("%[r1]")
				KERNEL_LOOP_TAIL("%[r1]")
				"vzeroupper\n\t"
				: [r1] "+r"(r1), [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
				: [end] "r"(end), [stride] "r"(stride),
				  [stop] "r"(stop_ptr), [dly] "r"(delay)
				: "zmm1", "cc", "memory"
			);
		}
		return ((uint64_t)(r1 - b->r1) / stride) * 64ULL;
	}
	case TRAFFIC_2_1_READ_WRITE: {
		/* FUN_0010fe9b: 4x [1R(r1) + 1W(w1)] */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX512_RD("%[r1]") AVX512_WR("%[w1]")
			AVX512_RD("%[r1]") AVX512_WR("%[w1]")
			AVX512_RD("%[r1]") AVX512_WR("%[w1]")
			AVX512_RD("%[r1]") AVX512_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			"vzeroupper\n\t"
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "zmm0", "zmm1", "cc", "memory"
		);
		return (((uint64_t)(r1 - b->r1) / stride) * 3ULL) * 64ULL;
	}
	case TRAFFIC_3_1_READ_WRITE: {
		/* FUN_0010e793: 4x [2R(r1) + 1W(w1)] */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX512_RD("%[r1]") AVX512_RD("%[r1]") AVX512_WR("%[w1]")
			AVX512_RD("%[r1]") AVX512_RD("%[r1]") AVX512_WR("%[w1]")
			AVX512_RD("%[r1]") AVX512_RD("%[r1]") AVX512_WR("%[w1]")
			AVX512_RD("%[r1]") AVX512_RD("%[r1]") AVX512_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			"vzeroupper\n\t"
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "zmm0", "zmm1", "cc", "memory"
		);
		return (((uint64_t)(r1 - b->r1) / stride) * 2ULL) * 64ULL;
	}
	case TRAFFIC_3_2_READ_WRITE: {
		/* FUN_00110917: 1x [1R(r1) + 2W(w1)], bound w1+size */
		uint8_t *end = w1 + size;
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX512_RD("%[r1]") AVX512_WR("%[w1]") AVX512_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[w1]")
			"vzeroupper\n\t"
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "zmm0", "zmm1", "cc", "memory"
		);
		uint64_t steps = (uint64_t)(w1 - b->w1) / stride;
		return ((steps * 5ULL) / 2ULL) * 64ULL;
	}
	case TRAFFIC_1_1_READ_WRITE: {
		/* FUN_00110baf: 4x [1W(w1)] */
		uint8_t *end = w1 + size;
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX512_WR("%[w1]") AVX512_WR("%[w1]")
			AVX512_WR("%[w1]") AVX512_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[w1]")
			"vzeroupper\n\t"
			: [w1] "+r"(w1), [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "zmm0", "cc", "memory"
		);
		return (((uint64_t)(w1 - b->w1) / stride) * 2ULL) * 64ULL;
	}
	case TRAFFIC_0_1_NT_WRITE: {
		/* FUN_00110edd: 2x [1 NTW(w1)] */
		uint8_t *end = w1 + size;
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX512_NTW("%[w1]") AVX512_NTW("%[w1]")
			KERNEL_LOOP_TAIL("%[w1]")
			"vzeroupper\n\t"
			: [w1] "+r"(w1), [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "zmm0", "cc", "memory"
		);
		return ((uint64_t)(w1 - b->w1) / stride) * 64ULL;
	}
	case TRAFFIC_2_1_NT_WRITE: {
		/* FUN_0010f247: 2x [2R(r1) + 1 NTW(w1)] */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX512_RD("%[r1]") AVX512_RD("%[r1]") AVX512_NTW("%[w1]")
			AVX512_RD("%[r1]") AVX512_RD("%[r1]") AVX512_NTW("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			"vzeroupper\n\t"
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "zmm0", "zmm1", "cc", "memory"
		);
		uint64_t steps = (uint64_t)(r1 - b->r1) / stride;
		return ((steps * 3ULL) / 2ULL) * 64ULL;
	}
	case TRAFFIC_1_1_NT_WRITE: {
		/* FUN_0010f76f: 2x [1R(r1) + 1 NTW(w1)] */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX512_RD("%[r1]") AVX512_NTW("%[w1]")
			AVX512_RD("%[r1]") AVX512_NTW("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			"vzeroupper\n\t"
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "zmm0", "zmm1", "cc", "memory"
		);
		return (((uint64_t)(r1 - b->r1) / stride) * 2ULL) * 64ULL;
	}
	case TRAFFIC_3_1_NT_WRITE:
		/* W9 has no AVX2/AVX512 kernel in intel/mlc; uses SSE2 */
		return exec_pass_sse2(b, traffic, delay, stride, stop_ptr);
	case TRAFFIC_STREAM_TRIAD: {
		/* FUN_0010ec07: 2x [1R(r1) + 1R(r2) + 1 NTW(w1)] */
		uint8_t *end = r1 + (size / 2);
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX512_RD("%[r1]") AVX512_RD("%[r2]") AVX512_NTW("%[w1]")
			AVX512_RD("%[r1]") AVX512_RD("%[r2]") AVX512_NTW("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			"vzeroupper\n\t"
			: [r1] "+r"(r1), [r2] "+r"(r2), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "zmm0", "zmm1", "cc", "memory"
		);
		return (((uint64_t)(r1 - b->r1) / stride) * 3ULL) * 64ULL;
	}
	case TRAFFIC_STREAM_TRIAD_RFO: {
		/* FUN_0010e3ef: 2x [1R(r1) + 1R(r2) + 1W(w1)] */
		uint8_t *end = r1 + (size / 2);
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX512_RD("%[r1]") AVX512_RD("%[r2]") AVX512_WR("%[w1]")
			AVX512_RD("%[r1]") AVX512_RD("%[r2]") AVX512_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			"vzeroupper\n\t"
			: [r1] "+r"(r1), [r2] "+r"(r2), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "zmm0", "zmm1", "cc", "memory"
		);
		return (((uint64_t)(r1 - b->r1) / stride) * 4ULL) * 64ULL;
	}
	case TRAFFIC_4_1_READ_WRITE: {
		/* FUN_0011051f: 1x [3R(r1) + 1W(w1)] */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX512_RD("%[r1]") AVX512_RD("%[r1]") AVX512_RD("%[r1]")
			AVX512_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			"vzeroupper\n\t"
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "zmm0", "zmm1", "cc", "memory"
		);
		uint64_t steps = (uint64_t)(r1 - b->r1) / stride;
		return ((steps * 5ULL) / 3ULL) * 64ULL;
	}
	case TRAFFIC_9_1_READ_WRITE: {
		/* FUN_0010ff87: 2x [8R(r1) + 1W(w1)] */
		uint8_t *end = r1 + size;
		__asm__ volatile (
			"vmovq %[end], %%xmm0\n\t"
			"1:\n\t"
			AVX512_RD("%[r1]") AVX512_RD("%[r1]") AVX512_RD("%[r1]") AVX512_RD("%[r1]")
			AVX512_RD("%[r1]") AVX512_RD("%[r1]") AVX512_RD("%[r1]") AVX512_RD("%[r1]")
			AVX512_WR("%[w1]")
			AVX512_RD("%[r1]") AVX512_RD("%[r1]") AVX512_RD("%[r1]") AVX512_RD("%[r1]")
			AVX512_RD("%[r1]") AVX512_RD("%[r1]") AVX512_RD("%[r1]") AVX512_RD("%[r1]")
			AVX512_WR("%[w1]")
			KERNEL_LOOP_TAIL("%[r1]")
			"vzeroupper\n\t"
			: [r1] "+r"(r1), [w1] "+r"(w1),
			  [icnt] "+r"(icnt), [dcnt] "=&r"(dcnt)
			: [end] "r"(end), [stride] "r"(stride),
			  [stop] "r"(stop_ptr), [dly] "r"(delay)
			: "zmm0", "zmm1", "cc", "memory"
		);
		uint64_t steps = (uint64_t)(r1 - b->r1) / stride;
		return ((steps * 10ULL) >> 3) * 64ULL;
	}
	case TRAFFIC_DUAL_ALL_READS:
	case TRAFFIC_DUAL_3_1_RW:
	case TRAFFIC_DUAL_2_1_NTW:
		/* Dual-stream kernels in intel/mlc (FUN_001189ad..FUN_00119159) are AVX2 */
		return exec_pass_avx2(b, traffic, delay, stride, mix_ratio,
				      stop_ptr);
	default:
		return 0;
	}
}

static inline uint64_t exec_thruput_pass(const thruput_buffers_t *b,
					 traffic_type_t traffic,
					 bw_simd_width_t simd_width,
					 int z_burst, int delay, size_t stride,
					 int mix_ratio,
					 const volatile int *stop_ptr)
{
	if (traffic == TRAFFIC_DUAL_ALL_READS ||
	    traffic == TRAFFIC_DUAL_3_1_RW ||
	    traffic == TRAFFIC_DUAL_2_1_NTW) {
		return exec_pass_avx2(b, traffic, delay, stride, mix_ratio,
				      stop_ptr);
	}
	if (traffic == TRAFFIC_3_1_NT_WRITE) {
		return exec_pass_sse2(b, traffic, delay, stride, stop_ptr);
	}
	switch (simd_width) {
	case BW_SIMD_AVX512:
		return exec_pass_avx512(b, traffic, delay, stride, z_burst,
					mix_ratio, stop_ptr);
	case BW_SIMD_AVX2:
		return exec_pass_avx2(b, traffic, delay, stride, mix_ratio,
				      stop_ptr);
	case BW_SIMD_SSE2:
	default:
		return exec_pass_sse2(b, traffic, delay, stride, stop_ptr);
	}
}

typedef struct {
	int cpu;
	int alloc_node;
	int alloc_node2;
	int mix_ratio;
	size_t buf_size;
	traffic_type_t traffic;
	int inject_delay;
	const volatile int *stop_ptr;
	double mb_per_sec;
	pthread_barrier_t *alloc_barrier;
	pthread_barrier_t *start_barrier;
	pthread_barrier_t *ddio_ready_barrier;
} bw_worker_arg_t;

static void *bw_worker(void *arg)
{
	bw_worker_arg_t *w = (bw_worker_arg_t *)arg;
	numa_pin_cpu(w->cpu);

	size_t stride = g_stride_bytes;
	thruput_buffers_t bufs;
	bool alloc_ok = alloc_thruput_buffers(&bufs, w->buf_size, w->traffic,
					      w->alloc_node, w->alloc_node2,
					      stride);
	if (w->alloc_barrier)
		pthread_barrier_wait(w->alloc_barrier);

	if (w->start_barrier)
		pthread_barrier_wait(w->start_barrier);

	bool is_ddio_leader = false;
	if (ddio_is_enabled()) {
		ddio_worker_enter(&is_ddio_leader);
		if (w->ddio_ready_barrier)
			pthread_barrier_wait(w->ddio_ready_barrier);
	}

	if (!alloc_ok) {
		w->mb_per_sec = 0.0;
		if (ddio_is_enabled())
			ddio_worker_exit(is_ddio_leader);
		return NULL;
	}

	struct timespec t_start, t_end;
	clock_gettime(CLOCK_MONOTONIC_RAW, &t_start);

	uint64_t bytes_transferred = 0;
	bw_simd_width_t simd = g_simd_width;
	int z_burst = g_z_burst;

	while (*w->stop_ptr == 0) {
		bytes_transferred += exec_thruput_pass(
		    &bufs, w->traffic, simd, z_burst, w->inject_delay, stride,
		    w->mix_ratio, w->stop_ptr);
	}

	clock_gettime(CLOCK_MONOTONIC_RAW, &t_end);
	double elapsed = (t_end.tv_sec - t_start.tv_sec) +
			 (t_end.tv_nsec - t_start.tv_nsec) * 1e-9;
	if (elapsed > 1e-6) {
		w->mb_per_sec = ((double)bytes_transferred / 1000000.0) / elapsed;
	}

	if (ddio_is_enabled())
		ddio_worker_exit(is_ddio_leader);

	free_thruput_buffers(&bufs);
	return NULL;
}

static void sleep_duration_sec(double duration_sec)
{
	if (duration_sec <= 0.0)
		duration_sec = 2.0;
	struct timespec req;
	req.tv_sec = (time_t)duration_sec;
	req.tv_nsec = (long)((duration_sec - (double)req.tv_sec) * 1e9);
	if (req.tv_nsec < 0)
		req.tv_nsec = 0;
	while (nanosleep(&req, &req) != 0) {
		break;
	}
}

static double measure_node_pair_bandwidth(int cpu_node_idx, int mem_node_idx,
					  double duration_sec,
					  size_t per_thread_mb,
					  bool single_thread_per_core,
					  traffic_type_t traffic)
{
	const numa_node_info_t *cpu_node = numa_get_node(cpu_node_idx);
	const numa_node_info_t *mem_node = numa_get_node(mem_node_idx);
	if (!cpu_node || !mem_node)
		return 0.0;

	int nthreads = single_thread_per_core ? cpu_node->num_one_tpc_cpus
					      : cpu_node->num_cpus;
	const int *cpu_list =
	    single_thread_per_core ? cpu_node->one_tpc_cpus : cpu_node->cpus;
	if (nthreads <= 0)
		return 0.0;

	int alloc_node_id = mem_node->has_memory ? mem_node->node_id : -1;

	pthread_t *threads =
	    (pthread_t *)malloc((size_t)nthreads * sizeof(pthread_t));
	bw_worker_arg_t *args = (bw_worker_arg_t *)malloc(
	    (size_t)nthreads * sizeof(bw_worker_arg_t));
	if (!threads || !args) {
		free(threads);
		free(args);
		return 0.0;
	}

	volatile int stop_flag = 0;
	pthread_barrier_t alloc_barrier, start_barrier, ddio_ready_barrier;
	pthread_barrier_init(&alloc_barrier, NULL, (unsigned int)(nthreads + 1));
	pthread_barrier_init(&start_barrier, NULL, (unsigned int)(nthreads + 1));
	pthread_barrier_init(&ddio_ready_barrier, NULL,
			     (unsigned int)(nthreads + 1));

	for (int i = 0; i < nthreads; i++) {
		int cpu = cpu_list[i];
		traffic_type_t t_traffic = traffic;
		size_t t_bytes = per_thread_mb * 1024ULL * 1024ULL;
		int node2 = alloc_node_id;
		int ratio = 0;

		if (g_has_per_thread_cfg && cpu >= 0 && cpu < MAX_CPUS &&
		    g_per_thread_cfg[cpu].enabled) {
			t_traffic = g_per_thread_cfg[cpu].traffic;
			t_bytes = g_per_thread_cfg[cpu].buf_size_kb * 1024ULL;
			node2 = g_per_thread_cfg[cpu].alloc_node2;
			ratio = g_per_thread_cfg[cpu].mix_ratio;
		}

		args[i].cpu = cpu;
		args[i].alloc_node = alloc_node_id;
		args[i].alloc_node2 = node2;
		args[i].mix_ratio = ratio;
		args[i].buf_size = t_bytes;
		args[i].traffic = t_traffic;
		args[i].inject_delay = 0;
		args[i].stop_ptr = &stop_flag;
		args[i].mb_per_sec = 0.0;
		args[i].alloc_barrier = &alloc_barrier;
		args[i].start_barrier = &start_barrier;
		args[i].ddio_ready_barrier = &ddio_ready_barrier;
		pthread_create(&threads[i], NULL, bw_worker, &args[i]);
	}

	pthread_barrier_wait(&alloc_barrier);
	if (ddio_is_enabled())
		ddio_step_begin(nthreads);

	pthread_barrier_wait(&start_barrier);
	if (ddio_is_enabled())
		pthread_barrier_wait(&ddio_ready_barrier);

	sleep_duration_sec(duration_sec);
	stop_flag = 1;

	double total_bw = 0.0;
	for (int i = 0; i < nthreads; i++) {
		pthread_join(threads[i], NULL);
		total_bw += args[i].mb_per_sec;
	}
	if (ddio_is_enabled())
		ddio_step_end();

	pthread_barrier_destroy(&alloc_barrier);
	pthread_barrier_destroy(&start_barrier);
	pthread_barrier_destroy(&ddio_ready_barrier);
	free(threads);
	free(args);
	return total_bw;
}

void run_bandwidth_matrix(size_t per_thread_mb, double duration_sec,
			  bool single_thread_per_core, bool verbose,
			  traffic_type_t traffic)
{
	printf("Measuring Memory Bandwidths between nodes within system \n");
	printf("Bandwidths are in MB/sec (1 MB/sec = 1,000,000 Bytes/sec)\n");
	printf
	    ("Using %s from each core if Hyper-threading is enabled\n",
	     single_thread_per_core ? "only one thread" : "all the threads");
	if (traffic == TRAFFIC_ALL_READS && !g_has_per_thread_cfg) {
		printf("Using Read-only traffic type\n");
	}

	int num_nodes = numa_get_num_nodes();
	bool numa_mode = numa_is_numa_mode();

	if (numa_mode) {
		printf("\t\tNuma node\nNuma node\t");
	} else {
		printf("\tMemory node\nSocket\t");
	}
	for (int j = 0; j < num_nodes; j++) {
		const numa_node_info_t *node_j = numa_get_node(j);
		printf("%8d\t", node_j ? node_j->node_id : j);
	}
	printf("\n");

	for (int i = 0; i < num_nodes; i++) {
		const numa_node_info_t *cpu_node = numa_get_node(i);
		if (!cpu_node || cpu_node->num_cpus <= 0)
			continue;

		printf("%8d\t", cpu_node->node_id);
		for (int j = 0; j < num_nodes; j++) {
			const numa_node_info_t *mem_node = numa_get_node(j);
			if (!mem_node || (numa_mode && !mem_node->has_memory)) {
				printf("       -\t");
			} else {
				start_auto_busy_threads(cpu_node->socket_id, -1,
							verbose);
				double bw = measure_node_pair_bandwidth(
				    i, j, duration_sec, per_thread_mb,
				    single_thread_per_core, traffic);
				stop_auto_busy_threads();
				printf("%8.1f\t", bw);
			}
			fflush(stdout);
		}
		printf("\n");
	}
}

/*
 * Persistent worker thread structure for multi-delay sweeps (loaded_latency,
 * peak_injection_bandwidth, and max_bandwidth).
 * Pre-allocates separated read/write buffers once upfront across all delay steps
 * and defers munmap until after all timed intervals complete.
 */
typedef struct {
	int cpu;
	int alloc_node;
	int alloc_node2;
	int mix_ratio;
	size_t buf_size;
	traffic_type_t traffic;
	volatile int inject_delay;
	const volatile int *stop_ptr;
	volatile bool terminate;
	volatile uint64_t bytes_step;
	double elapsed_sec;
	bool verbose;
	pthread_barrier_t *alloc_barrier;
	pthread_barrier_t *start_barrier;
	pthread_barrier_t *ddio_ready_barrier;
	pthread_barrier_t *done_barrier;
} loaded_worker_arg_t;

static void *loaded_bw_worker(void *arg)
{
	loaded_worker_arg_t *w = (loaded_worker_arg_t *)arg;
	numa_pin_cpu(w->cpu);

	size_t stride = g_stride_bytes;
	thruput_buffers_t bufs;
	bool alloc_ok = alloc_thruput_buffers(&bufs, w->buf_size, w->traffic,
					      w->alloc_node, w->alloc_node2,
					      stride);
	if (w->alloc_barrier)
		pthread_barrier_wait(w->alloc_barrier);

	while (1) {
		pthread_barrier_wait(w->start_barrier);
		if (w->terminate)
			break;

		if (w->verbose)
			printf("Launching BW thread on cpu %d\n", w->cpu);

		bool is_ddio_leader = false;
		if (ddio_is_enabled()) {
			ddio_worker_enter(&is_ddio_leader);
			if (w->ddio_ready_barrier)
				pthread_barrier_wait(w->ddio_ready_barrier);
		}

		if (!alloc_ok) {
			w->elapsed_sec = 0.0;
			w->bytes_step = 0;
			if (ddio_is_enabled())
				ddio_worker_exit(is_ddio_leader);
			pthread_barrier_wait(w->done_barrier);
			continue;
		}

		struct timespec t_s, t_e;
		clock_gettime(CLOCK_MONOTONIC_RAW, &t_s);

		uint64_t bytes = 0;
		int delay = w->inject_delay;
		bw_simd_width_t simd = g_simd_width;
		int z_burst = g_z_burst;

		while (*w->stop_ptr == 0) {
			bytes += exec_thruput_pass(&bufs, w->traffic, simd,
						   z_burst, delay, stride,
						   w->mix_ratio, w->stop_ptr);
		}

		clock_gettime(CLOCK_MONOTONIC_RAW, &t_e);
		w->elapsed_sec = (t_e.tv_sec - t_s.tv_sec) +
				 (t_e.tv_nsec - t_s.tv_nsec) * 1e-9;
		w->bytes_step = bytes;

		if (ddio_is_enabled())
			ddio_worker_exit(is_ddio_leader);

		pthread_barrier_wait(w->done_barrier);
	}

	free_thruput_buffers(&bufs);
	return NULL;
}

/*
 * Delay sweep controller matching intel/mlc FUN_00123c60:
 *   - Allocates per-thread separated read/write buffers once for the traffic type;
 *   - Evaluates each sentinel-terminated delay in delays[] (either {0, -1} for
 *     --peak_injection_bandwidth or {0, 15, 30, 45, 60, 75, 90, -1} for
 *     --max_bandwidth);
 *   - Records maximum bandwidth and corresponding injection delay.
 */
static double sweep_system_bandwidth_for_ratio(traffic_type_t traffic,
					       const int *selected_cpus,
					       int nthreads,
					       size_t per_thread_bytes,
					       double duration_sec,
					       const int *delays,
					       int *out_best_delay)
{
	if (out_best_delay)
		*out_best_delay = 0;
	if (!selected_cpus || nthreads <= 0 || !delays)
		return 0.0;

	pthread_t *threads =
	    (pthread_t *)calloc((size_t)nthreads, sizeof(pthread_t));
	loaded_worker_arg_t *args = (loaded_worker_arg_t *)calloc(
	    (size_t)nthreads, sizeof(loaded_worker_arg_t));
	if (!threads || !args) {
		free(threads);
		free(args);
		return 0.0;
	}

	volatile int step_stop_flag = 0;
	pthread_barrier_t alloc_barrier, start_barrier, ddio_ready_barrier,
	    done_barrier;
	pthread_barrier_init(&alloc_barrier, NULL,
			     (unsigned int)(nthreads + 1));
	pthread_barrier_init(&start_barrier, NULL,
			     (unsigned int)(nthreads + 1));
	pthread_barrier_init(&ddio_ready_barrier, NULL,
			     (unsigned int)(nthreads + 1));
	pthread_barrier_init(&done_barrier, NULL,
			     (unsigned int)(nthreads + 1));

	int launched = 0;
	for (int i = 0; i < nthreads; i++) {
		int cpu = selected_cpus[i];
		const numa_node_info_t *cnode = numa_get_node_for_cpu(cpu);
		int local_nid =
		    (cnode && cnode->has_memory) ? cnode->node_id : -1;

		args[i].cpu = cpu;
		args[i].alloc_node = local_nid;
		args[i].alloc_node2 = local_nid;
		args[i].mix_ratio = 0;
		args[i].buf_size = per_thread_bytes;
		args[i].traffic = traffic;
		args[i].inject_delay = 0;
		args[i].stop_ptr = &step_stop_flag;
		args[i].terminate = false;
		args[i].bytes_step = 0;
		args[i].elapsed_sec = 0.0;
		args[i].verbose = false;
		args[i].alloc_barrier = &alloc_barrier;
		args[i].start_barrier = &start_barrier;
		args[i].ddio_ready_barrier = &ddio_ready_barrier;
		args[i].done_barrier = &done_barrier;
		if (pthread_create(&threads[i], NULL, loaded_bw_worker,
				   &args[i]) != 0) {
			break;
		}
		launched++;
	}

	double max_bw = 0.0;
	int best_delay = 0;

	if (launched == nthreads) {
		pthread_barrier_wait(&alloc_barrier);

		for (int d = 0; delays[d] != -1; d++) {
			int delay = delays[d];
			step_stop_flag = 0;
			for (int i = 0; i < nthreads; i++) {
				args[i].inject_delay = delay;
			}

			if (ddio_is_enabled())
				ddio_step_begin(nthreads);

			pthread_barrier_wait(&start_barrier);
			if (ddio_is_enabled())
				pthread_barrier_wait(&ddio_ready_barrier);

			sleep_duration_sec(duration_sec);
			step_stop_flag = 1;
			pthread_barrier_wait(&done_barrier);

			if (ddio_is_enabled())
				ddio_step_end();

			double step_bw = 0.0;
			for (int i = 0; i < nthreads; i++) {
				if (args[i].elapsed_sec > 1e-6) {
					step_bw +=
					    ((double)args[i].bytes_step /
					     1000000.0) /
					    args[i].elapsed_sec;
				}
			}

			if (step_bw > max_bw) {
				max_bw = step_bw;
				best_delay = delay;
			}
		}

		for (int i = 0; i < nthreads; i++) {
			args[i].terminate = true;
		}
		pthread_barrier_wait(&start_barrier);
	}

	for (int i = 0; i < launched; i++) {
		pthread_join(threads[i], NULL);
	}

	pthread_barrier_destroy(&alloc_barrier);
	pthread_barrier_destroy(&start_barrier);
	pthread_barrier_destroy(&ddio_ready_barrier);
	pthread_barrier_destroy(&done_barrier);
	free(threads);
	free(args);

	if (out_best_delay)
		*out_best_delay = best_delay;
	return max_bw;
}

/*
 * Unified driver for --peak_injection_bandwidth (is_max=false) and
 * --max_bandwidth (is_max=true), matching intel/mlc FUN_001295d0.
 */
int run_peak_or_max_bandwidth(const peak_bw_options_t *opts)
{
	if (!opts)
		return 22;

	if (opts->pattern_file) {
		int prc = bw_load_pattern_file(opts->pattern_file);
		if (prc != 0)
			return prc;
	} else {
		bw_clear_pattern_file();
	}

	int total_cpus = numa_get_total_cpus();
	int selected_cpus[MAX_CPUS];
	int nthreads = 0;

	/* Validate -m hex mask upfront matching main FUN_0011e050 order */
	if (opts->cpu_mask) {
		nthreads = conc_parse_cpu_mask(opts->cpu_mask, total_cpus,
					       selected_cpus, MAX_CPUS);
		if (nthreads <= 0) {
			bw_clear_pattern_file();
			return 22;
		}
	}

	if (!opts->max_bandwidth_mode) {
		puts("Measuring Peak Injection Memory Bandwidths for the system\nBandwidths are in MB/sec (1 MB/sec = 1,000,000 Bytes/sec)");
	} else {
		puts("Measuring Maximum Memory Bandwidths for the system");
		puts("Will take several minutes to complete as multiple injection rates will be tried to get the best bandwidth");
		puts("Bandwidths are in MB/sec (1 MB/sec = 1,000,000 Bytes/sec)");
	}
	puts(opts->single_thread_per_core
		 ? "Using only one thread from each core if Hyper-threading is enabled"
		 : "Using all the threads from each core if Hyper-threading is enabled");
	puts("Using traffic with the following read-write ratios");

	if (opts->cpu_list) {
		nthreads = conc_parse_cpu_list(opts->cpu_list, total_cpus,
					       selected_cpus, MAX_CPUS);
		if (nthreads <= 0) {
			bw_clear_pattern_file();
			return 22;
		}
	} else if (!opts->cpu_mask) {
		int num_nodes = numa_get_num_nodes();
		for (int n = 0; n < num_nodes; n++) {
			const numa_node_info_t *node = numa_get_node(n);
			if (!node)
				continue;
			int count = opts->single_thread_per_core
					? node->num_one_tpc_cpus
					: node->num_cpus;
			const int *cpus = opts->single_thread_per_core
					      ? node->one_tpc_cpus
					      : node->cpus;
			for (int c = 0; c < count && nthreads < MAX_CPUS; c++) {
				selected_cpus[nthreads++] = cpus[c];
			}
		}
	}

	size_t per_thread_bytes = opts->per_thread_kb * 1024ULL;
	const int *delays =
	    opts->max_bandwidth_mode ? g_max_bw_delays : g_peak_bw_delays;

	static const struct {
		const char *label;
		traffic_type_t traffic;
	} ratios[] = {
		{ "ALL Reads        :\t", TRAFFIC_ALL_READS },
		{ "3:1 Reads-Writes :\t", TRAFFIC_3_1_READ_WRITE },
		{ "2:1 Reads-Writes :\t", TRAFFIC_2_1_READ_WRITE },
		{ "1:1 Reads-Writes :\t", TRAFFIC_1_1_READ_WRITE },
		{ "Stream-triad like:\t", TRAFFIC_STREAM_TRIAD }
	};

	for (size_t r = 0; r < sizeof(ratios) / sizeof(ratios[0]); r++) {
		printf("%s", ratios[r].label);
		fflush(stdout);

		int best_delay = 0;
		double bw = sweep_system_bandwidth_for_ratio(
		    ratios[r].traffic, selected_cpus, nthreads,
		    per_thread_bytes, opts->duration_sec, delays, &best_delay);

		if (opts->max_bandwidth_mode) {
			if (opts->verbose) {
				printf("max_bandwidth -- %8.1f MB/sec measured at injection delay of %d\n",
				       bw, best_delay);
			}
			printf("%8.1f\n", bw);
		} else {
			printf("%8.1f\t\n", bw);
		}
		fflush(stdout);
	}
	putchar('\n');

	bw_clear_pattern_file();
	return 0;
}

void run_peak_bandwidth(size_t per_thread_mb, double duration_sec,
			bool single_thread_per_core)
{
	peak_bw_options_t opts = {
		.per_thread_kb = per_thread_mb * 1024ULL,
		.duration_sec = duration_sec,
		.single_thread_per_core = single_thread_per_core,
		.max_bandwidth_mode = false,
		.verbose = false,
		.cpu_list = NULL,
		.cpu_mask = NULL,
		.pattern_file = NULL
	};
	(void)run_peak_or_max_bandwidth(&opts);
}

int run_loaded_latency_ex(const loaded_lat_options_t *opts)
{
	if (!opts)
		return 22;

	if (opts->pattern_file) {
		int prc = bw_load_pattern_file(opts->pattern_file);
		if (prc != 0)
			return prc;
	} else {
		bw_clear_pattern_file();
	}

	int total_cpus = numa_get_total_cpus();
	int num_nodes = numa_get_num_nodes();
	int lat_cpu = (opts->target_core >= 0) ? opts->target_core : 0;

	int explicit_cpus[MAX_CPUS];
	int explicit_cnt = 0;

	/* Validate -m hex mask upfront matching main FUN_0011e050 order */
	if (opts->cpu_mask) {
		explicit_cnt = conc_parse_cpu_mask(opts->cpu_mask, total_cpus,
						   explicit_cpus, MAX_CPUS);
		if (explicit_cnt <= 0) {
			bw_clear_pattern_file();
			return 22;
		}
		if (!opts->throughput_only) {
			for (int i = 0; i < explicit_cnt; i++) {
				if (explicit_cpus[i] == lat_cpu) {
					printf("Exclude cpu %d from the -m mask as the latency thread runs on it\n",
					       lat_cpu);
					bw_clear_pattern_file();
					return 22;
				}
			}
		}
	}

	printf("Measuring Loaded Latencies for the system\n");
	printf
	    ("Using %s from each core if Hyper-threading is enabled\n",
	     opts->single_thread_per_core ? "only one thread"
					  : "all the threads");
	if (opts->traffic == TRAFFIC_ALL_READS && !g_has_per_thread_cfg) {
		printf("Using Read-only traffic type\n");
	}

	if (opts->cpu_list) {
		explicit_cnt = conc_parse_cpu_list(opts->cpu_list, total_cpus,
						   explicit_cpus, MAX_CPUS);
		if (explicit_cnt <= 0) {
			bw_clear_pattern_file();
			return 22;
		}
		if (!opts->throughput_only) {
			for (int i = 0; i < explicit_cnt; i++) {
				if (explicit_cpus[i] == lat_cpu) {
					printf("Exclude cpu %d from the -k parameter list as the latency thread runs on it\n",
					       lat_cpu);
					bw_clear_pattern_file();
					return 22;
				}
			}
		}
	}

	int single_d = g_single_delay;
	int num_delays = 0;
	const int *delays = NULL;
	if (single_d >= 0) {
		delays = &single_d;
		num_delays = 1;
	} else {
		delays = get_default_delays(&num_delays);
	}

	if (!opts->keep_prefetchers) {
		msr_enable_prefetchers(false);
		if (!opts->throughput_only) {
			msr_disable_prefetcher_on_cpu(lat_cpu, false);
		}
	}

	int worker_cpus[MAX_CPUS];
	int worker_nodes[MAX_CPUS];
	int worker_nodes2[MAX_CPUS];
	int worker_ratios[MAX_CPUS];
	traffic_type_t worker_traffic[MAX_CPUS];
	size_t worker_bytes[MAX_CPUS];
	int nworkers = 0;
	size_t default_bytes = opts->per_thread_kb * 1024ULL;

	if (g_has_per_thread_cfg) {
		for (int c = 0; c < total_cpus && c < MAX_CPUS &&
				nworkers < MAX_CPUS;
		     c++) {
			if (!g_per_thread_cfg[c].enabled)
				continue;
			if (!opts->throughput_only && c == lat_cpu)
				continue;
			const numa_node_info_t *cnode = numa_get_node_for_cpu(c);
			int local_nid =
			    (cnode && cnode->has_memory) ? cnode->node_id : -1;
			worker_cpus[nworkers] = c;
			worker_nodes[nworkers] =
			    (g_per_thread_cfg[c].alloc_node >= 0)
				? g_per_thread_cfg[c].alloc_node
				: local_nid;
			worker_nodes2[nworkers] =
			    (g_per_thread_cfg[c].alloc_node2 >= 0)
				? g_per_thread_cfg[c].alloc_node2
				: worker_nodes[nworkers];
			worker_ratios[nworkers] = g_per_thread_cfg[c].mix_ratio;
			worker_traffic[nworkers] = g_per_thread_cfg[c].traffic;
			worker_bytes[nworkers] =
			    g_per_thread_cfg[c].buf_size_kb * 1024ULL;
			nworkers++;
		}
	} else if (opts->cpu_list || opts->cpu_mask) {
		for (int i = 0; i < explicit_cnt && nworkers < MAX_CPUS; i++) {
			int cpu = explicit_cpus[i];
			const numa_node_info_t *cnode = numa_get_node_for_cpu(cpu);
			int local_nid =
			    (cnode && cnode->has_memory) ? cnode->node_id : -1;
			int nid = opts->has_alloc_node ? opts->alloc_node
						       : local_nid;
			worker_cpus[nworkers] = cpu;
			worker_nodes[nworkers] = nid;
			worker_nodes2[nworkers] = nid;
			worker_ratios[nworkers] = 0;
			worker_traffic[nworkers] = opts->traffic;
			worker_bytes[nworkers] = default_bytes;
			nworkers++;
		}
	} else {
		for (int n = 0; n < num_nodes; n++) {
			const numa_node_info_t *node = numa_get_node(n);
			if (!node)
				continue;
			int count = opts->single_thread_per_core
					? node->num_one_tpc_cpus
					: node->num_cpus;
			const int *cpu_list = opts->single_thread_per_core
						  ? node->one_tpc_cpus
						  : node->cpus;
			int local_nid = node->has_memory ? node->node_id : -1;
			int alloc_node_id =
			    opts->has_alloc_node ? opts->alloc_node : local_nid;

			for (int c = 0; c < count && nworkers < MAX_CPUS; c++) {
				int cpu = cpu_list[c];
				if (!opts->throughput_only &&
				    (cpu == lat_cpu ||
				     cpu_is_same_core(cpu, lat_cpu)))
					continue;
				worker_cpus[nworkers] = cpu;
				worker_nodes[nworkers] = alloc_node_id;
				worker_nodes2[nworkers] = alloc_node_id;
				worker_ratios[nworkers] = 0;
				worker_traffic[nworkers] = opts->traffic;
				worker_bytes[nworkers] = default_bytes;
				nworkers++;
			}
		}

		if (nworkers == 0 && !opts->single_thread_per_core &&
		    total_cpus > 1) {
			for (int n = 0; n < num_nodes; n++) {
				const numa_node_info_t *node = numa_get_node(n);
				if (!node)
					continue;
				int local_nid =
				    node->has_memory ? node->node_id : -1;
				int alloc_node_id = opts->has_alloc_node
							? opts->alloc_node
							: local_nid;
				for (int c = 0;
				     c < node->num_cpus && nworkers < MAX_CPUS;
				     c++) {
					int cpu = node->cpus[c];
					if (!opts->throughput_only &&
					    cpu == lat_cpu)
						continue;
					worker_cpus[nworkers] = cpu;
					worker_nodes[nworkers] = alloc_node_id;
					worker_nodes2[nworkers] = alloc_node_id;
					worker_ratios[nworkers] = 0;
					worker_traffic[nworkers] =
					    opts->traffic;
					worker_bytes[nworkers] = default_bytes;
					nworkers++;
				}
			}
		}
	}

	pthread_t *threads = (nworkers > 0)
	    ? (pthread_t *)malloc((size_t)nworkers * sizeof(pthread_t))
	    : NULL;
	loaded_worker_arg_t *args = (nworkers > 0)
	    ? (loaded_worker_arg_t *)malloc(
		  (size_t)nworkers * sizeof(loaded_worker_arg_t))
	    : NULL;

	volatile int step_stop_flag = 0;
	pthread_barrier_t alloc_barrier, start_barrier, ddio_ready_barrier,
	    done_barrier;
	pthread_barrier_init(&alloc_barrier, NULL,
			     (unsigned int)(nworkers + 1));
	pthread_barrier_init(&start_barrier, NULL,
			     (unsigned int)(nworkers + 1));
	pthread_barrier_init(&ddio_ready_barrier, NULL,
			     (unsigned int)(nworkers + 1));
	pthread_barrier_init(&done_barrier, NULL,
			     (unsigned int)(nworkers + 1));

	int current_worker = 0;
	for (int i = 0; i < nworkers; i++) {
		args[current_worker].cpu = worker_cpus[i];
		args[current_worker].alloc_node = worker_nodes[i];
		args[current_worker].alloc_node2 = worker_nodes2[i];
		args[current_worker].mix_ratio = worker_ratios[i];
		args[current_worker].buf_size = worker_bytes[i];
		args[current_worker].traffic = worker_traffic[i];
		args[current_worker].inject_delay = 0;
		args[current_worker].stop_ptr = &step_stop_flag;
		args[current_worker].terminate = false;
		args[current_worker].bytes_step = 0;
		args[current_worker].elapsed_sec = 0.0;
		args[current_worker].verbose = opts->verbose;
		args[current_worker].alloc_barrier = &alloc_barrier;
		args[current_worker].start_barrier = &start_barrier;
		args[current_worker].ddio_ready_barrier = &ddio_ready_barrier;
		args[current_worker].done_barrier = &done_barrier;
		pthread_create(&threads[current_worker], NULL, loaded_bw_worker,
			       &args[current_worker]);
		current_worker++;
	}

	/* Wait for all throughput threads to finish buffer allocation first */
	pthread_barrier_wait(&alloc_barrier);

	struct cache_line *lat_buf = NULL;
	size_t lat_buf_size = 200ULL * 1024ULL * 1024ULL;
	if (!opts->throughput_only) {
		const numa_node_info_t *lat_node = numa_get_node_for_cpu(lat_cpu);
		int lat_local_nid =
		    (lat_node && lat_node->has_memory) ? lat_node->node_id : -1;
		int lat_alloc_node =
		    opts->has_alloc_node ? opts->alloc_node : lat_local_nid;
		numa_pin_cpu(lat_cpu);

		lat_buf = (struct cache_line *)alloc_mem_onnode(
		    lat_buf_size, lat_alloc_node, numa_get_hugepage_mode());
		if (!lat_buf) {
			for (int i = 0; i < current_worker; i++)
				args[i].terminate = true;
			pthread_barrier_wait(&start_barrier);
			for (int i = 0; i < current_worker; i++)
				pthread_join(threads[i], NULL);
			pthread_barrier_destroy(&alloc_barrier);
			pthread_barrier_destroy(&start_barrier);
			pthread_barrier_destroy(&ddio_ready_barrier);
			pthread_barrier_destroy(&done_barrier);
			free(threads);
			free(args);
			bw_clear_pattern_file();
			if (!opts->keep_prefetchers)
				msr_restore_prefetchers();
			return 12;
		}
		init_pointer_chase_chain(lat_buf, lat_buf_size,
					 opts->random_latency);
		numa_pin_cpu(lat_cpu);
	}

	if (opts->verbose) {
		puts("\nAllocated Memory!");
	}
	printf("Inject\tLatency\tBandwidth\n");
	printf("Delay\t(ns)\tMB/sec\n");
	printf("==========================\n");
	fflush(stdout);

	double step_time = (opts->duration_sec > 0.1) ? opts->duration_sec : 1.0;

	for (int d = 0; d < num_delays; d++) {
		int delay = delays[d];

		step_stop_flag = 0;
		for (int i = 0; i < current_worker; i++) {
			args[i].inject_delay = delay;
		}

		if (ddio_is_enabled())
			ddio_step_begin(current_worker);
		if (opts->verbose) {
			printf("Using %d BW generation threads\n",
			       current_worker);
		}

		pthread_barrier_wait(&start_barrier);
		if (ddio_is_enabled())
			pthread_barrier_wait(&ddio_ready_barrier);

		double lat_ns = 0.0;
		if (opts->throughput_only) {
			sleep_duration_sec(step_time);
		} else {
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

			lat_ns =
			    ((double)total_cycles / (double)total_iters) /
			    helper_get_tsc_freq_ghz();
		}

		step_stop_flag = 1;
		pthread_barrier_wait(&done_barrier);

		if (ddio_is_enabled())
			ddio_step_end();

		double total_bw = 0.0;
		for (int i = 0; i < current_worker; i++) {
			if (args[i].elapsed_sec > 0.001) {
				total_bw +=
				    ((double)args[i].bytes_step / 1000000.0) /
				    args[i].elapsed_sec;
			}
		}

		printf(" %05d\t%6.2f\t%8.1f\n", delay, lat_ns, total_bw);
		fflush(stdout);
	}

	for (int i = 0; i < current_worker; i++) {
		args[i].terminate = true;
	}
	pthread_barrier_wait(&start_barrier);
	for (int i = 0; i < current_worker; i++) {
		pthread_join(threads[i], NULL);
	}

	if (lat_buf)
		free_mem_onnode(lat_buf, lat_buf_size);
	pthread_barrier_destroy(&alloc_barrier);
	pthread_barrier_destroy(&start_barrier);
	pthread_barrier_destroy(&ddio_ready_barrier);
	pthread_barrier_destroy(&done_barrier);
	free(threads);
	free(args);
	bw_clear_pattern_file();

	if (!opts->keep_prefetchers) {
		msr_restore_prefetchers();
	}
	return 0;
}

void run_loaded_latency(size_t per_thread_mb, double duration_sec,
			bool random_latency, bool keep_prefetchers,
			int target_core, bool single_thread_per_core,
			bool throughput_only, traffic_type_t traffic)
{
	loaded_lat_options_t opts = {
		.per_thread_kb = per_thread_mb * 1024ULL,
		.duration_sec = duration_sec,
		.random_latency = random_latency,
		.keep_prefetchers = keep_prefetchers,
		.target_core = target_core,
		.single_thread_per_core = single_thread_per_core,
		.throughput_only = throughput_only,
		.verbose = false,
		.has_alloc_node = false,
		.alloc_node = -1,
		.traffic = traffic,
		.cpu_list = NULL,
		.cpu_mask = NULL,
		.pattern_file = NULL
	};
	(void)run_loaded_latency_ex(&opts);
}

static inline uint64_t conc_rdtsc(void)
{
	uint32_t lo, hi;
	__asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

/*
 * Guard-padded buffer size matching intel/mlc FUN_0010baa0:
 *   - Adds 512 bytes (8 * 64B cache lines) so unrolled hot/cold microkernels
 *     (widest burst: 9 cold lines in 10% kernel, max overshoot 8 * 64 = 512B;
 *     8 hot lines in 93% kernel, max overshoot 7 * 64 = 448B) never read out
 *     of bounds before the loop-end cold_end / cmovae hot_end check.
 *   - Rounds up to 2 MiB / 1 GiB when -L / -h hugepage mode is active so
 *     munmap() receives a hugepage-aligned length.
 */
static size_t conc_padded_alloc_size(size_t usable_bytes)
{
	size_t padded = usable_bytes + 512;
	hugepage_type_t hp = numa_get_hugepage_mode();
	if (hp == HUGEPAGE_1GB) {
		const size_t align = 1ULL << 30;
		padded = (padded + align - 1) & ~(align - 1);
	} else if (hp == HUGEPAGE_2MB) {
		const size_t align = 2ULL << 20;
		padded = (padded + align - 1) & ~(align - 1);
	}
	return padded;
}

/*
 * Initialize every 8-byte word with its index and clflush all lines to DRAM,
 * matching intel/mlc FUN_0010b020.
 */
static void conc_init_and_flush_buf(uint8_t *buf, size_t usable_bytes)
{
	size_t init_bytes = usable_bytes + 512;
	uint64_t *q = (uint64_t *)buf;
	size_t n_qwords = init_bytes >> 3;
	for (size_t i = 0; i < n_qwords; i++)
		q[i] = (uint64_t)i;
	for (size_t off = 0; off < init_bytes; off += 64)
		_mm_clflush(buf + off);
	_mm_mfence();
}

#define CONC_HOT_RD(reg) \
	"vmovdqa (%[hot]), %%" reg "\n\t" \
	"vmovdqa 0x20(%[hot]), %%" reg "\n\t" \
	"addq $64, %[hot]\n\t"

#define CONC_COLD_RD(reg) \
	"vmovdqa (%[cold]), %%" reg "\n\t" \
	"vmovdqa 0x20(%[cold]), %%" reg "\n\t" \
	"addq $64, %[cold]\n\t"

#define CONC_HOT_WRAP \
	"cmpq %[hot_end], %[hot]\n\t" \
	"cmovaeq %[hot_base], %[hot]\n\t"

#define CONC_AVX_LOOP_TAIL \
	"incq %[iter]\n\t" \
	"cmpq $10000, %[iter]\n\t" \
	"jb 2f\n\t" \
	"cmpl $0, (%[stop])\n\t" \
	"jne 3f\n\t" \
	"xorq %[iter], %[iter]\n\t" \
	"2:\n\t" \
	"cmpq %[cold_end], %[cold]\n\t" \
	"jb 1b\n\t" \
	"3:\n\t" \
	"vzeroupper\n\t"

/*
 * Execute one pass over cold_buf using the exact branchless interleaved AVX/SSE2
 * microkernels from intel/mlc (FUN_00118165 and FUN_00119371..FUN_0011a24e),
 * returning total cache lines (hot + cold) transferred as accounted by
 * ThruputThread (FUN_00120430).
 */
static uint64_t conc_exec_pass(uint8_t *cold_base, size_t cold_bytes,
			       uint8_t *hot_base, size_t hot_bytes,
			       int hot_pct, const volatile int *stop_ptr)
{
	uint8_t *cold = cold_base;
	uint8_t *cold_end = cold_base + cold_bytes;
	uint64_t iter = 0;

	if (hot_pct <= 0) {
		/* 0% hot: pure cold-buffer 4x unrolled SSE2 movdqa (FUN_00118165) */
		__asm__ volatile (
			"1:\n\t"
			"movdqa (%[cold]), %%xmm0\n\t"
			"movdqa 0x10(%[cold]), %%xmm0\n\t"
			"movdqa 0x20(%[cold]), %%xmm0\n\t"
			"movdqa 0x30(%[cold]), %%xmm0\n\t"
			"addq $64, %[cold]\n\t"
			"movdqa (%[cold]), %%xmm0\n\t"
			"movdqa 0x10(%[cold]), %%xmm0\n\t"
			"movdqa 0x20(%[cold]), %%xmm0\n\t"
			"movdqa 0x30(%[cold]), %%xmm0\n\t"
			"addq $64, %[cold]\n\t"
			"movdqa (%[cold]), %%xmm0\n\t"
			"movdqa 0x10(%[cold]), %%xmm0\n\t"
			"movdqa 0x20(%[cold]), %%xmm0\n\t"
			"movdqa 0x30(%[cold]), %%xmm0\n\t"
			"addq $64, %[cold]\n\t"
			"movdqa (%[cold]), %%xmm0\n\t"
			"movdqa 0x10(%[cold]), %%xmm0\n\t"
			"movdqa 0x20(%[cold]), %%xmm0\n\t"
			"movdqa 0x30(%[cold]), %%xmm0\n\t"
			"addq $64, %[cold]\n\t"
			"incq %[iter]\n\t"
			"cmpq $10000, %[iter]\n\t"
			"jb 2f\n\t"
			"cmpl $0, (%[stop])\n\t"
			"jne 3f\n\t"
			"xorq %[iter], %[iter]\n\t"
			"2:\n\t"
			"cmpq %[cold_end], %[cold]\n\t"
			"jb 1b\n\t"
			"3:\n\t"
			: [cold] "+&r"(cold), [iter] "+&r"(iter)
			: [cold_end] "r"(cold_end), [stop] "r"(stop_ptr)
			: "xmm0", "cc", "memory"
		);
		return (uint64_t)(cold - cold_base) >> 6;
	}

	uint8_t *hot = hot_base;
	uint8_t *hot_end = hot_base + hot_bytes;

	switch (hot_pct) {
	case 10:
		/* FUN_0011960d: 1H(ymm0) + WRAP + 9C(ymm1..ymm9) */
		__asm__ volatile (
			"1:\n\t"
			CONC_HOT_RD("ymm0")
			CONC_HOT_WRAP
			CONC_COLD_RD("ymm1")
			CONC_COLD_RD("ymm2")
			CONC_COLD_RD("ymm3")
			CONC_COLD_RD("ymm4")
			CONC_COLD_RD("ymm5")
			CONC_COLD_RD("ymm6")
			CONC_COLD_RD("ymm7")
			CONC_COLD_RD("ymm8")
			CONC_COLD_RD("ymm9")
			CONC_AVX_LOOP_TAIL
			: [cold] "+&r"(cold), [hot] "+&r"(hot),
			  [iter] "+&r"(iter)
			: [cold_end] "r"(cold_end), [hot_base] "r"(hot_base),
			  [hot_end] "r"(hot_end), [stop] "r"(stop_ptr)
			: "ymm0", "ymm1", "ymm2", "ymm3", "ymm4",
			  "ymm5", "ymm6", "ymm7", "ymm8", "ymm9",
			  "cc", "memory"
		);
		return (((uint64_t)(cold - cold_base) >> 6) * 10ULL) / 9ULL;

	case 20:
		/* FUN_0011971d: 1H(ymm0) + WRAP + 4C(ymm1..ymm4) */
		__asm__ volatile (
			"1:\n\t"
			CONC_HOT_RD("ymm0")
			CONC_HOT_WRAP
			CONC_COLD_RD("ymm1")
			CONC_COLD_RD("ymm2")
			CONC_COLD_RD("ymm3")
			CONC_COLD_RD("ymm4")
			CONC_AVX_LOOP_TAIL
			: [cold] "+&r"(cold), [hot] "+&r"(hot),
			  [iter] "+&r"(iter)
			: [cold_end] "r"(cold_end), [hot_base] "r"(hot_base),
			  [hot_end] "r"(hot_end), [stop] "r"(stop_ptr)
			: "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "cc", "memory"
		);
		return (((uint64_t)(cold - cold_base) >> 6) * 5ULL) / 4ULL;

	case 25:
		/* FUN_001197e2: 1H(ymm0) + WRAP + 3C(ymm1..ymm3) */
		__asm__ volatile (
			"1:\n\t"
			CONC_HOT_RD("ymm0")
			CONC_HOT_WRAP
			CONC_COLD_RD("ymm1")
			CONC_COLD_RD("ymm2")
			CONC_COLD_RD("ymm3")
			CONC_AVX_LOOP_TAIL
			: [cold] "+&r"(cold), [hot] "+&r"(hot),
			  [iter] "+&r"(iter)
			: [cold_end] "r"(cold_end), [hot_base] "r"(hot_base),
			  [hot_end] "r"(hot_end), [stop] "r"(stop_ptr)
			: "ymm0", "ymm1", "ymm2", "ymm3", "cc", "memory"
		);
		return (((uint64_t)(cold - cold_base) >> 6) * 4ULL) / 3ULL;

	case 30:
		/* FUN_001198cd: 3H(ymm0) + WRAP + 7C(ymm1..ymm7) */
		__asm__ volatile (
			"1:\n\t"
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_WRAP
			CONC_COLD_RD("ymm1")
			CONC_COLD_RD("ymm2")
			CONC_COLD_RD("ymm3")
			CONC_COLD_RD("ymm4")
			CONC_COLD_RD("ymm5")
			CONC_COLD_RD("ymm6")
			CONC_COLD_RD("ymm7")
			CONC_AVX_LOOP_TAIL
			: [cold] "+&r"(cold), [hot] "+&r"(hot),
			  [iter] "+&r"(iter)
			: [cold_end] "r"(cold_end), [hot_base] "r"(hot_base),
			  [hot_end] "r"(hot_end), [stop] "r"(stop_ptr)
			: "ymm0", "ymm1", "ymm2", "ymm3",
			  "ymm4", "ymm5", "ymm6", "ymm7", "cc", "memory"
		);
		return (((uint64_t)(cold - cold_base) >> 6) * 10ULL) / 7ULL;

	case 40:
		/* FUN_001199dd: 2H(ymm0) + WRAP + 3C(ymm1..ymm3) */
		__asm__ volatile (
			"1:\n\t"
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_WRAP
			CONC_COLD_RD("ymm1")
			CONC_COLD_RD("ymm2")
			CONC_COLD_RD("ymm3")
			CONC_AVX_LOOP_TAIL
			: [cold] "+&r"(cold), [hot] "+&r"(hot),
			  [iter] "+&r"(iter)
			: [cold_end] "r"(cold_end), [hot_base] "r"(hot_base),
			  [hot_end] "r"(hot_end), [stop] "r"(stop_ptr)
			: "ymm0", "ymm1", "ymm2", "ymm3", "cc", "memory"
		);
		return (((uint64_t)(cold - cold_base) >> 6) * 5ULL) / 3ULL;

	case 50:
		/* FUN_00119371: (1H + 1C + 1H + 1C + WRAP) x 2 on ymm1 */
		__asm__ volatile (
			"1:\n\t"
			CONC_HOT_RD("ymm1")
			CONC_COLD_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_COLD_RD("ymm1")
			CONC_HOT_WRAP
			CONC_HOT_RD("ymm1")
			CONC_COLD_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_COLD_RD("ymm1")
			CONC_HOT_WRAP
			CONC_AVX_LOOP_TAIL
			: [cold] "+&r"(cold), [hot] "+&r"(hot),
			  [iter] "+&r"(iter)
			: [cold_end] "r"(cold_end), [hot_base] "r"(hot_base),
			  [hot_end] "r"(hot_end), [stop] "r"(stop_ptr)
			: "ymm1", "cc", "memory"
		);
		return ((uint64_t)(cold - cold_base) >> 6) * 2ULL;

	case 60:
		/* FUN_00119aad: 3H(ymm0) + WRAP + 2C(ymm1..ymm2) */
		__asm__ volatile (
			"1:\n\t"
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_WRAP
			CONC_COLD_RD("ymm1")
			CONC_COLD_RD("ymm2")
			CONC_AVX_LOOP_TAIL
			: [cold] "+&r"(cold), [hot] "+&r"(hot),
			  [iter] "+&r"(iter)
			: [cold_end] "r"(cold_end), [hot_base] "r"(hot_base),
			  [hot_end] "r"(hot_end), [stop] "r"(stop_ptr)
			: "ymm0", "ymm1", "ymm2", "cc", "memory"
		);
		return (((uint64_t)(cold - cold_base) >> 6) * 5ULL) / 2ULL;

	case 70:
		/* FUN_00119b7d: 4H(ymm0) + WRAP + 3H(ymm0) + WRAP + 3C(ymm1..ymm3) */
		__asm__ volatile (
			"1:\n\t"
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_WRAP
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_WRAP
			CONC_COLD_RD("ymm1")
			CONC_COLD_RD("ymm2")
			CONC_COLD_RD("ymm3")
			CONC_AVX_LOOP_TAIL
			: [cold] "+&r"(cold), [hot] "+&r"(hot),
			  [iter] "+&r"(iter)
			: [cold_end] "r"(cold_end), [hot_base] "r"(hot_base),
			  [hot_end] "r"(hot_end), [stop] "r"(stop_ptr)
			: "ymm0", "ymm1", "ymm2", "ymm3", "cc", "memory"
		);
		return (((uint64_t)(cold - cold_base) >> 6) * 10ULL) / 3ULL;

	case 75:
	default:
		/* FUN_00119476 (default): (3H(ymm1) + WRAP + 1C(ymm1)) x 2 */
		__asm__ volatile (
			"1:\n\t"
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_WRAP
			CONC_COLD_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_WRAP
			CONC_COLD_RD("ymm1")
			CONC_AVX_LOOP_TAIL
			: [cold] "+&r"(cold), [hot] "+&r"(hot),
			  [iter] "+&r"(iter)
			: [cold_end] "r"(cold_end), [hot_base] "r"(hot_base),
			  [hot_end] "r"(hot_end), [stop] "r"(stop_ptr)
			: "ymm1", "cc", "memory"
		);
		return ((uint64_t)(cold - cold_base) >> 6) * 4ULL;

	case 80:
		/* FUN_00119c8d: 4H(ymm0) + WRAP + 1C(ymm1) */
		__asm__ volatile (
			"1:\n\t"
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_WRAP
			CONC_COLD_RD("ymm1")
			CONC_AVX_LOOP_TAIL
			: [cold] "+&r"(cold), [hot] "+&r"(hot),
			  [iter] "+&r"(iter)
			: [cold_end] "r"(cold_end), [hot_base] "r"(hot_base),
			  [hot_end] "r"(hot_end), [stop] "r"(stop_ptr)
			: "ymm0", "ymm1", "cc", "memory"
		);
		return ((uint64_t)(cold - cold_base) >> 6) * 5ULL;

	case 87:
		/* FUN_00119d61: 7H(ymm1) + WRAP + 1C(ymm1) (7/8 = 87.5%) */
		__asm__ volatile (
			"1:\n\t"
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_WRAP
			CONC_COLD_RD("ymm1")
			CONC_AVX_LOOP_TAIL
			: [cold] "+&r"(cold), [hot] "+&r"(hot),
			  [iter] "+&r"(iter)
			: [cold_end] "r"(cold_end), [hot_base] "r"(hot_base),
			  [hot_end] "r"(hot_end), [stop] "r"(stop_ptr)
			: "ymm1", "cc", "memory"
		);
		return ((uint64_t)(cold - cold_base) >> 6) * 8ULL;

	case 90:
		/* FUN_00119e5d: 5H(ymm0) + WRAP + 4H(ymm0) + WRAP + 1C(ymm1) */
		__asm__ volatile (
			"1:\n\t"
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_WRAP
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_WRAP
			CONC_COLD_RD("ymm1")
			CONC_AVX_LOOP_TAIL
			: [cold] "+&r"(cold), [hot] "+&r"(hot),
			  [iter] "+&r"(iter)
			: [cold_end] "r"(cold_end), [hot_base] "r"(hot_base),
			  [hot_end] "r"(hot_end), [stop] "r"(stop_ptr)
			: "ymm0", "ymm1", "cc", "memory"
		);
		return ((uint64_t)(cold - cold_base) >> 6) * 10ULL;

	case 93:
		/* FUN_00119f61: 7H(ymm1) + WRAP + 8H(ymm1) + WRAP + 1C(ymm1) (15/16 = 93.75%) */
		__asm__ volatile (
			"1:\n\t"
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_WRAP
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_RD("ymm1")
			CONC_HOT_WRAP
			CONC_COLD_RD("ymm1")
			CONC_AVX_LOOP_TAIL
			: [cold] "+&r"(cold), [hot] "+&r"(hot),
			  [iter] "+&r"(iter)
			: [cold_end] "r"(cold_end), [hot_base] "r"(hot_base),
			  [hot_end] "r"(hot_end), [stop] "r"(stop_ptr)
			: "ymm1", "cc", "memory"
		);
		return ((uint64_t)(cold - cold_base) >> 6) * 16ULL;

	case 100:
		/* FUN_0011a24e: 4H(ymm0) + WRAP + 4H(ymm0) + WRAP + synthetic cold+64 */
		__asm__ volatile (
			"1:\n\t"
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_WRAP
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_RD("ymm0")
			CONC_HOT_WRAP
			"addq $64, %[cold]\n\t"
			CONC_AVX_LOOP_TAIL
			: [cold] "+&r"(cold), [hot] "+&r"(hot),
			  [iter] "+&r"(iter)
			: [cold_end] "r"(cold_end), [hot_base] "r"(hot_base),
			  [hot_end] "r"(hot_end), [stop] "r"(stop_ptr)
			: "ymm0", "cc", "memory"
		);
		return ((uint64_t)(cold - cold_base) >> 6) * 8ULL;
	}
}

static int conc_parse_cpu_mask(const char *mask_str, int total_cpus,
			       int *out_cpus, int max_out)
{
	if (!mask_str || *mask_str == '\0') {
		printf("Invalid hex string  in -m parameter value\n");
		return -1;
	}

	const char *hex = mask_str;
	if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
		hex += 2;
	if (*hex == '\0') {
		printf("Invalid hex string %s in -m parameter value\n", mask_str);
		return -1;
	}
	for (const char *p = hex; *p != '\0'; p++) {
		if (!isxdigit((unsigned char)*p)) {
			printf("Invalid hex string %s in -m parameter value\n",
			       mask_str);
			return -1;
		}
	}

	size_t len = strlen(hex);
	int count = 0;
	int bit_pos = 0;
	for (size_t idx = len; idx > 0; idx--) {
		char c = hex[idx - 1];
		unsigned int nib;
		if (c >= '0' && c <= '9')
			nib = (unsigned int)(c - '0');
		else if (c >= 'a' && c <= 'f')
			nib = 10U + (unsigned int)(c - 'a');
		else
			nib = 10U + (unsigned int)(c - 'A');

		for (int b = 0; b < 4; b++, bit_pos++) {
			if ((nib & (1U << b)) == 0)
				continue;
			if (count >= max_out) {
				printf("Error: the -m mask has bits set for more than %d cpus. cpu %d and above cannot exist on any machine MLC supports.\n",
				       max_out, bit_pos);
				return -1;
			}
			if (bit_pos >= total_cpus) {
				printf("Invalid cpu id %d in -m mask: this machine has %d cpus, so the highest valid id is %d\n",
				       bit_pos, total_cpus, total_cpus - 1);
				return -1;
			}
			out_cpus[count++] = bit_pos;
		}
	}

	if (count == 0) {
		printf("Error: -m%s selects no cpus. The value is a hex mask and every bit in it is zero.\n",
		       mask_str);
		puts("Set the bits for the cpus the throughput threads should run on, or use -k to list them instead.");
		return -1;
	}
	return count;
}

static bool conc_is_digits_only(const char *s)
{
	if (!s || *s == '\0')
		return false;
	for (const char *p = s; *p != '\0'; p++) {
		if (*p < '0' || *p > '9')
			return false;
	}
	return true;
}

static int conc_parse_cpu_list(const char *list_str, int total_cpus,
			       int *out_cpus, int max_out)
{
	if (!list_str || *list_str == '\0') {
		puts("Invalid characters in cpu_range in -k option");
		return -1;
	}
	for (const char *p = list_str; *p != '\0'; p++) {
		if ((*p < '0' || *p > '9') && *p != ',' && *p != '-') {
			puts("Invalid characters in cpu_range in -k option");
			return -1;
		}
	}

	char buf[4096];
	strncpy(buf, list_str, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	int count = 0;
	char *tok = buf;
	while (tok) {
		char *comma = strchr(tok, ',');
		char *next = NULL;
		if (comma) {
			*comma = '\0';
			next = comma + 1;
		}

		char *dash = strchr(tok, '-');
		if (!dash) {
			if (!conc_is_digits_only(tok)) {
				puts("Invalid characters in cpu_range in -k option");
				return -1;
			}
			long id = strtol(tok, NULL, 10);
			if (count >= max_out) {
				printf("Too many cpus in the -k parameter list. At most %d can be given, counting repeats and every cpu named by a range.\n",
				       max_out);
				return -1;
			}
			if (id < 0 || id >= total_cpus) {
				printf("Invalid cpu id %ld on -k parameter list\n",
				       id);
				return -1;
			}
			out_cpus[count++] = (int)id;
		} else {
			*dash = '\0';
			const char *lhs = tok;
			const char *rhs = dash + 1;
			if (!conc_is_digits_only(lhs) ||
			    !conc_is_digits_only(rhs)) {
				puts("Invalid characters in cpu_range in -k option");
				return -1;
			}
			long first = strtol(lhs, NULL, 10);
			long last = strtol(rhs, NULL, 10);
			if (first > last) {
				printf("In cpu range (-k option) field, %ld > %ld\n",
				       first, last);
				return -1;
			}
			for (long id = first; id <= last; id++) {
				if (count >= max_out) {
					printf("Too many cpus in the -k parameter list. At most %d can be given, counting repeats and every cpu named by a range.\n",
					       max_out);
					return -1;
				}
				if (id < 0 || id >= total_cpus) {
					printf("Invalid cpu id %ld on -k parameter list\n",
					       id);
					return -1;
				}
				out_cpus[count++] = (int)id;
			}
		}
		tok = next;
	}

	if (count <= 0) {
		puts("Invalid characters in cpu_range in -k option");
		return -1;
	}
	return count;
}

typedef struct {
	int cpu;
	int alloc_node;
	size_t cold_bytes;
	size_t cold_alloc_bytes;
	size_t hot_bytes;
	size_t hot_alloc_bytes;
	int hot_pct;
	bool shared_hot_buf;
	bool verbose;
	double tsc_ghz;
	uint8_t *cold_buf;
	uint8_t *hot_buf;
	uint8_t **shared_hot_ptr;
	pthread_mutex_t *shared_mutex;
	pthread_barrier_t *alloc_barrier;
	const volatile int *start_ptr;
	const volatile int *stop_ptr;
	volatile int *alloc_failed_ptr;
	double mb_per_sec;
} conc_worker_arg_t;

static void *conc_worker(void *arg)
{
	conc_worker_arg_t *w = (conc_worker_arg_t *)arg;
	numa_pin_cpu(w->cpu);

	w->cold_buf = (uint8_t *)alloc_mem_onnode(
	    w->cold_alloc_bytes, w->alloc_node, HUGEPAGE_NONE);
	if (!w->cold_buf) {
		*w->alloc_failed_ptr = 1;
		pthread_barrier_wait(w->alloc_barrier);
		return NULL;
	}
	conc_init_and_flush_buf(w->cold_buf, w->cold_bytes);

	if (w->hot_pct > 0) {
		if (w->shared_hot_buf) {
			pthread_mutex_lock(w->shared_mutex);
			if (w->verbose) {
				printf("ThruputMemAllocThread: concurrent_bw cold=%zu hot=%zu\n",
				       w->cold_bytes, w->hot_bytes);
			}
			if (*w->shared_hot_ptr == NULL) {
				uint8_t *sh = (uint8_t *)alloc_mem_onnode(
				    w->hot_alloc_bytes, w->alloc_node,
				    HUGEPAGE_NONE);
				if (sh)
					conc_init_and_flush_buf(sh,
								w->hot_bytes);
				*w->shared_hot_ptr = sh;
			}
			w->hot_buf = *w->shared_hot_ptr;
			pthread_mutex_unlock(w->shared_mutex);
		} else {
			if (w->verbose) {
				printf("ThruputMemAllocThread: concurrent_bw cold=%zu hot=%zu\n",
				       w->cold_bytes, w->hot_bytes);
			}
			w->hot_buf = (uint8_t *)alloc_mem_onnode(
			    w->hot_alloc_bytes, w->alloc_node, HUGEPAGE_NONE);
			if (w->hot_buf)
				conc_init_and_flush_buf(w->hot_buf,
							w->hot_bytes);
		}
		if (!w->hot_buf) {
			*w->alloc_failed_ptr = 1;
			pthread_barrier_wait(w->alloc_barrier);
			return NULL;
		}
	}

	if (w->verbose)
		printf("Launching BW thread on cpu %d\n", w->cpu);

	pthread_barrier_wait(w->alloc_barrier);
	if (*w->alloc_failed_ptr)
		return NULL;

	while (*w->start_ptr == 0)
		_mm_pause();

	uint64_t t0 = conc_rdtsc();
	uint64_t total_lines = 0;
	while (*w->stop_ptr == 0) {
		total_lines += conc_exec_pass(w->cold_buf, w->cold_bytes,
					      w->hot_buf, w->hot_bytes,
					      w->hot_pct, w->stop_ptr);
	}
	uint64_t t1 = conc_rdtsc();

	double elapsed_sec =
	    (w->tsc_ghz > 0.0) ? ((double)(t1 - t0) / (w->tsc_ghz * 1e9)) : 0.0;
	if (elapsed_sec > 1e-9) {
		w->mb_per_sec =
		    (((double)total_lines * 64.0) / elapsed_sec) / 1000000.0;
	} else {
		w->mb_per_sec = 0.0;
	}
	return NULL;
}

int run_concurrent_bandwidth(const conc_bw_options_t *opts)
{
	if (!opts)
		return 22;

	int total_cpus = numa_get_total_cpus();
	int selected_cpus[MAX_CPUS];
	int nthreads = 0;

	/* In intel/mlc, -m is validated before printing the mode header */
	if (opts->cpu_mask) {
		nthreads = conc_parse_cpu_mask(opts->cpu_mask, total_cpus,
					       selected_cpus, MAX_CPUS);
		if (nthreads <= 0)
			return 22;
	}

	puts("\nMeasuring Concurrent Bandwidth (hot/cold dual-buffer)");
	printf("  Cold buffer: %zu KiB (streams through memory)\n",
	       opts->cold_kb);
	if (opts->hot_pct < 1) {
		printf("  Hot buffer:  none (0%% hot \xe2\x80\x94 cold reads only)\n");
	} else {
		printf("  Hot buffer:  %zu KiB (wraps in LLC)\n",
		       opts->hot_kb);
	}
	printf("  Hot access:  %d%%\n", opts->hot_pct);
	if (opts->hot_pct > 0) {
		if (opts->hot_kb < 512) {
			puts("  NOTE: Hot buffer < 512 KiB \xe2\x80\x94 fits in L1/L2 cache; reported BW reflects cache throughput, not memory bandwidth.");
		}
		if (opts->shared_hot_buf) {
			puts("  Hot buffer shared across all threads (-u)");
		}
	}
	if (opts->has_numa_node) {
		printf("  Memory allocated from NUMA node %d (-j)\n",
		       opts->numa_node);
	}
	puts("Bandwidth is in MB/sec (1 MB/sec = 1,000,000 Bytes/sec)");
	puts(opts->single_thread_per_core
		 ? "Using only one thread from each core if Hyper-threading is enabled"
		 : "Using all the threads from each core if Hyper-threading is enabled");

	if (opts->cpu_list) {
		nthreads = conc_parse_cpu_list(opts->cpu_list, total_cpus,
					       selected_cpus, MAX_CPUS);
		if (nthreads <= 0)
			return 22;
		if (opts->single_thread_per_core) {
			int filtered = 0;
			for (int i = 0; i < nthreads; i++) {
				if (cpu_is_smt_primary(selected_cpus[i]))
					selected_cpus[filtered++] =
					    selected_cpus[i];
			}
			nthreads = filtered;
			if (nthreads == 0) {
				puts("Error: -X -k combination resulted in 0 CPUs (no smt_id==0 threads in list)");
				return 22;
			}
		}
	} else if (!opts->cpu_mask) {
		for (int cpu = 0; cpu < total_cpus && nthreads < MAX_CPUS;
		     cpu++) {
			if (!opts->single_thread_per_core ||
			    cpu_is_smt_primary(cpu)) {
				selected_cpus[nthreads++] = cpu;
			}
		}
	}

	if (opts->verbose) {
		printf("Verbose: %d bandwidth threads, LineSize=%d bytes\n",
		       nthreads, 64);
		printf("Verbose: CPUs: ");
		for (int i = 0; i < nthreads; i++) {
			printf("%d%s", selected_cpus[i],
			       (i + 1 < nthreads) ? "," : "");
		}
		putchar('\n');
		if (opts->per_thread_bw)
			puts("Verbose: Per-thread throughput enabled (-B)");
	}

	printf("\nAll Reads (hot+cold): \t");
	if (opts->per_thread_bw)
		putchar('\n');

	size_t cold_bytes = opts->cold_kb * 1024ULL;
	if (cold_bytes < 64)
		cold_bytes = 64;
	size_t cold_alloc_bytes = conc_padded_alloc_size(cold_bytes);

	size_t hot_bytes = (opts->hot_pct > 0) ? (opts->hot_kb * 1024ULL) : 0;
	if (opts->hot_pct > 0 && hot_bytes < 64)
		hot_bytes = 64;
	size_t hot_alloc_bytes =
	    (opts->hot_pct > 0) ? conc_padded_alloc_size(hot_bytes) : 0;

	pthread_t *threads =
	    (pthread_t *)calloc((size_t)nthreads, sizeof(pthread_t));
	conc_worker_arg_t *args = (conc_worker_arg_t *)calloc(
	    (size_t)nthreads, sizeof(conc_worker_arg_t));
	if (!threads || !args) {
		free(threads);
		free(args);
		fprintf(stderr, "Out of memory!\n");
		return 1;
	}

	pthread_barrier_t alloc_barrier;
	pthread_barrier_init(&alloc_barrier, NULL, (unsigned int)(nthreads + 1));
	pthread_mutex_t shared_mutex = PTHREAD_MUTEX_INITIALIZER;
	uint8_t *shared_hot_buf_ptr = NULL;
	volatile int start_flag = 0;
	volatile int stop_flag = 0;
	volatile int alloc_failed = 0;
	double tsc_ghz = helper_get_tsc_freq_ghz();
	int alloc_node = opts->has_numa_node ? opts->numa_node : -1;

	int launched = 0;
	for (int i = 0; i < nthreads; i++) {
		args[i].cpu = selected_cpus[i];
		args[i].alloc_node = alloc_node;
		args[i].cold_bytes = cold_bytes;
		args[i].cold_alloc_bytes = cold_alloc_bytes;
		args[i].hot_bytes = hot_bytes;
		args[i].hot_alloc_bytes = hot_alloc_bytes;
		args[i].hot_pct = opts->hot_pct;
		args[i].shared_hot_buf = opts->shared_hot_buf;
		args[i].verbose = opts->verbose;
		args[i].tsc_ghz = tsc_ghz;
		args[i].shared_hot_ptr = &shared_hot_buf_ptr;
		args[i].shared_mutex = &shared_mutex;
		args[i].alloc_barrier = &alloc_barrier;
		args[i].start_ptr = &start_flag;
		args[i].stop_ptr = &stop_flag;
		args[i].alloc_failed_ptr = &alloc_failed;
		args[i].mb_per_sec = 0.0;
		if (pthread_create(&threads[i], NULL, conc_worker, &args[i]) !=
		    0) {
			fprintf(stderr, "Error in create thread\n");
			alloc_failed = 1;
			break;
		}
		launched++;
	}

	if (launched == nthreads) {
		pthread_barrier_wait(&alloc_barrier);
		if (!alloc_failed) {
			__sync_synchronize();
			start_flag = 1;
			__sync_synchronize();
			sleep_duration_sec(opts->duration_sec > 0.0
					       ? opts->duration_sec
					       : 3.0);
			stop_flag = 1;
			__sync_synchronize();
		}
	} else {
		/* Unblock already-launched workers if pthread_create failed mid-way */
		stop_flag = 1;
		start_flag = 1;
	}

	double total_bw = 0.0;
	for (int i = 0; i < launched; i++) {
		pthread_join(threads[i], NULL);
		if (!alloc_failed) {
			if (opts->per_thread_bw) {
				printf("thread\t%d\t%.1f\n", args[i].cpu,
				       args[i].mb_per_sec);
			}
			total_bw += args[i].mb_per_sec;
		}
	}

	/*
	 * Unmap buffers after all workers have completed rdtsc measurement so
	 * munmap TLB shootdowns never perturb active worker threads.
	 */
	for (int i = 0; i < launched; i++) {
		if (args[i].cold_buf) {
			free_mem_onnode(args[i].cold_buf, cold_alloc_bytes);
			args[i].cold_buf = NULL;
		}
		if (!opts->shared_hot_buf && args[i].hot_buf) {
			free_mem_onnode(args[i].hot_buf, hot_alloc_bytes);
			args[i].hot_buf = NULL;
		}
	}
	if (opts->shared_hot_buf && shared_hot_buf_ptr) {
		free_mem_onnode(shared_hot_buf_ptr, hot_alloc_bytes);
	}

	pthread_barrier_destroy(&alloc_barrier);
	pthread_mutex_destroy(&shared_mutex);
	free(threads);
	free(args);

	if (alloc_failed) {
		fprintf(stderr, "Buffer allocation failed!\n");
		return 1;
	}

	printf("%8.1f\t", total_bw);
	puts("\n");
	return 0;
}
