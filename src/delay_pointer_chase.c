#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <cpuid.h>

#include "delay_pointer_chase.h"
#include "helper.h"
#include "allocation.h"

/*
 * Auto-detect per-node L1 pointer-chase overhead in core cycles based on
 * CPU vendor, family, and model, matching intel/mlc FUN_0012fb60
 * (0x12ff30..0x130033) and CPU predicates 0x12a2e0..0x12a910.
 */
static int detect_delay_chase_overhead(void)
{
	unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
	char vendor[13] = { 0 };

	if (__get_cpuid(0, &eax, &ebx, &ecx, &edx)) {
		memcpy(vendor, &ebx, 4);
		memcpy(vendor + 4, &edx, 4);
		memcpy(vendor + 8, &ecx, 4);
		vendor[12] = '\0';
	}

	if (strcmp(vendor, "AuthenticAMD") == 0)
		return 4;

	uint32_t family = 0, model = 0;
	if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
		uint32_t base_fam = (eax >> 8) & 0xf;
		family = base_fam + ((base_fam == 0xf) ? ((eax >> 20) & 0xff) : 0);
		uint32_t base_mod = (eax >> 4) & 0xf;
		uint32_t ext_mod =
		    (family == 6 || family == 0xf) ? ((eax >> 12) & 0xf0) : 0;
		model = base_mod | ext_mod;
	}

	/* Diamond Rapids (family 19, model 0x01) */
	if (family == 0x13 && model == 0x01)
		return 4;

	/* Nova Lake (family 18, model 0x01 / 0x03) */
	if (family == 0x12 && (model == 0x01 || model == 0x03))
		return 4;

	if (family == 6) {
		switch (model) {
		/* Lunar Lake, Panther Lake, Clearwater Forest, Goldmont/Denverton */
		case 0xbc:
		case 0xbd:
		case 0xcc:
		case 0xcd:
		case 0xdd:
		case 0x5c:
		case 0x5f:
			return 4;

		/* Sierra Forest / Grand Ridge */
		case 0xaf:
		case 0xb6:
			return 3;

		/*
		 * SPR (0x8f), EMR (0xcf), GNR (0xad, 0xae), ICX (0x6a),
		 * SKX/CLX/CPX (0x55), ARL (0xc5, 0xc6), MTL (0xaa, 0xab, 0xac),
		 * ADL (0x97, 0x9a, 0xbf), RPL (0xb7, 0xba), KNL/KNM (0x57, 0x85)
		 */
		case 0x8f:
		case 0xcf:
		case 0xad:
		case 0xae:
		case 0x6a:
		case 0x55:
		case 0xc5:
		case 0xc6:
		case 0xaa:
		case 0xab:
		case 0xac:
		case 0x97:
		case 0x9a:
		case 0xbf:
		case 0xb7:
		case 0xba:
		case 0x57:
		case 0x85:
			return 5;

		default:
			break;
		}
	}

	fprintf(stderr, "Warning: Unknown CPU type, using default value\n");
	return 5;
}

/*
 * Core measurement routine matching intel/mlc FUN_0012f800 (0x12f800..0x12fb59).
 *
 * Uses 7 staggered copies of the delay sub-chain in `pool` whose terminal nodes
 * store byte offsets {8, 16, 24, 32, 40, 48, 56}. Main-buffer slot `s` points
 * at `slot[0] -> &pool[(s % 7) * chain_len]` and stores the next main-slot
 * pointer at `slot[(s % 7) + 1]`, creating a strict data dependency from the
 * end of the L1 delay sub-chain to the next DRAM slot load.
 */
static double measure_delay_pointer_chase(double ratio, uint64_t *slots,
					  uint64_t *pool, size_t nslots,
					  size_t iters, size_t chain_len,
					  int stride, int overhead,
					  double tsc_mhz)
{
	static const uint64_t terminal_offsets[7] = {
		8, 16, 24, 32, 40, 48, 56
	};

	for (size_t c = 0; c < 7; c++) {
		uint64_t *sub = pool + c * chain_len;
		for (size_t k = 0; k + 1 < chain_len; k++) {
			sub[k] = (uint64_t)(uintptr_t)&sub[k + 1];
		}
		sub[chain_len - 1] = terminal_offsets[c];
	}

	if (nslots != 0) {
		for (size_t s = 0; s < nslots; s++) {
			uint64_t *line = slots + s * 8;
			memset(line, 0, 64);
			size_t c = s % 7;
			line[0] = (uint64_t)(uintptr_t)(pool + c * chain_len);
			size_t next_s = (s + (size_t)stride) % nslots;
			line[c + 1] = (uint64_t)(uintptr_t)(slots + next_s * 8);
		}
	}

	void *p = slots;
	uint64_t elapsed_tsc = 0;
	uint64_t iters_u64 = (uint64_t)iters;
	uint64_t chain_u64 = (uint64_t)chain_len;

	__asm__ volatile (
		"lfence\n\t"
		"rdtsc\n\t"
		"shlq $32, %%rdx\n\t"
		"movl %%eax, %%esi\n\t"
		"orq %%rdx, %%rsi\n\t"
		"testq %[iters], %[iters]\n\t"
		"jz 3f\n\t"
		"xorl %%ecx, %%ecx\n\t"
		".p2align 4\n\t"
		"1:\n\t"
		"testq %[chain], %[chain]\n\t"
		"movq (%[p]), %%rdx\n\t"
		"jz 2f\n\t"
		"xorl %%eax, %%eax\n\t"
		".p2align 4\n\t"
		"4:\n\t"
		"addq $1, %%rax\n\t"
		"movq (%%rdx), %%rdx\n\t"
		"cmpq %[chain], %%rax\n\t"
		"jnz 4b\n\t"
		"addq %%rdx, %[p]\n\t"
		"2:\n\t"
		"addq $1, %%rcx\n\t"
		"movq (%[p]), %[p]\n\t"
		"cmpq %[iters], %%rcx\n\t"
		"jnz 1b\n\t"
		"3:\n\t"
		"rdtscp\n\t"
		"lfence\n\t"
		"shlq $32, %%rdx\n\t"
		"movl %%eax, %%eax\n\t"
		"orq %%rdx, %%rax\n\t"
		"subq %%rsi, %%rax\n\t"
		: [elapsed] "=&a" (elapsed_tsc),
		  [p] "+r" (p)
		: [iters] "r" (iters_u64),
		  [chain] "r" (chain_u64)
		: "rcx", "rdx", "rsi", "cc", "memory"
	);

	double tsc_cyc = (double)elapsed_tsc / (double)iters;
	double ns = (tsc_mhz > 0.0) ? ((tsc_cyc * 1000.0) / tsc_mhz) : 0.0;

	if (ratio <= 1.0) {
		printf("%4zu\t%8.1f\t%8.2f\n", chain_len, tsc_cyc, ns);
	} else {
		double core_cyc = ratio * tsc_cyc;
		double adj_core_cyc =
		    core_cyc - ((double)chain_len + 1.0) * (double)overhead;
		printf("%4zu\t%8.1f\t%8.2f\t%8.1f\t%8.1f\n",
		       chain_len, tsc_cyc, ns, core_cyc, adj_core_cyc);
	}
	fflush(stdout);

	return tsc_cyc;
}

int run_delay_pointer_chase(const delay_pc_config_t *cfg, char *err_buf,
			    size_t err_buf_len)
{
	if (cfg->target_core >= 0) {
		numa_pin_cpu(cfg->target_core);
	}

	size_t nslots =
	    (cfg->dslots != 0) ? cfg->dslots : DELAY_PC_DEFAULT_SLOTS;
	int iters = (cfg->diters > 0) ? cfg->diters : DELAY_PC_DEFAULT_ITERS;
	int stride =
	    (cfg->dstride > 0) ? cfg->dstride : DELAY_PC_DEFAULT_STRIDE;
	int sweep_max =
	    (cfg->dsweep > 0) ? cfg->dsweep : DELAY_PC_DEFAULT_SWEEP;
	int chain_cfg = cfg->dchain;
	int core_mhz = cfg->dcore_mhz;
	bool auto_overhead = (cfg->doverhead < 0);
	int overhead =
	    auto_overhead ? detect_delay_chase_overhead() : cfg->doverhead;

	double tsc_mhz = helper_get_tsc_freq_ghz() * 1000.0;
	double ratio = 0.0;
	if (core_mhz > 0 && tsc_mhz > 0.0) {
		ratio = (double)core_mhz / tsc_mhz;
	}

	printf("\nDelay Pointer Chase Latency\n");
	printf("nslots=%zu  iters=%d  stride=%d  tsc_freq=%.0f MHz",
	       nslots, iters, stride, tsc_mhz);
	if (ratio > 1.0) {
		printf("  core_freq=%d MHz  ratio=%.3f  overhead=%d%s\n\n",
		       core_mhz, ratio, overhead,
		       auto_overhead ? " (auto)" : "");
		printf("chain\tTSC_cyc/iter\tns/iter\tcore_cyc/iter\tadj_core_cyc\n");
	} else {
		printf("\n\n");
		printf("chain\tTSC_cyc/iter\tns/iter\n");
	}
	printf("----------------------------------------------------------\n");

	int max_chain = (chain_cfg > 0) ? chain_cfg : sweep_max;
	size_t pool_nodes = (size_t)max_chain * 7;
	size_t pool_bytes = (pool_nodes * sizeof(uint64_t) + 63U) & ~(size_t)63U;

	uint64_t *pool = aligned_alloc(64, pool_bytes);
	uint64_t *slots = NULL;
	size_t stride_bytes = (size_t)stride * 64U;
	if (stride_bytes != 0 && nslots <= SIZE_MAX / stride_bytes) {
		slots = aligned_alloc(64, stride_bytes * nslots);
	}

	if (!pool || !slots) {
		if (err_buf && err_buf_len > 0) {
			snprintf(err_buf, err_buf_len,
				 "delay_pointer_chase: allocation failed (nslots=%zu stride=%d pool_nodes=%zu)",
				 nslots, stride, pool_nodes);
		}
		free(pool);
		free(slots);
		return ENOMEM;
	}

	if (chain_cfg <= 0) {
		for (size_t cl = 1; cl <= (size_t)sweep_max; cl++) {
			measure_delay_pointer_chase(ratio, slots, pool, nslots,
						    (size_t)iters, cl, stride,
						    overhead, tsc_mhz);
		}
	} else {
		measure_delay_pointer_chase(ratio, slots, pool, nslots,
					    (size_t)iters, (size_t)chain_cfg,
					    stride, overhead, tsc_mhz);
	}

	free(pool);
	free(slots);
	return 0;
}
