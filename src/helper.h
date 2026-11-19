#ifndef MLC_HELPER_H
#define MLC_HELPER_H

#include <stdbool.h>
#include <stdint.h>

#define INTEL_MSR_PREFETCH_CONTROL 0x1A4
#define AMD_ZEN3_MSR_PREFETCH_CONTROL 0xC0000108
#define INTEL_PREFETCH_DISABLE_MASK 0x0FULL

void helper_init_tsc(void);
double helper_get_tsc_freq_ghz(void);

bool msr_init(int total_cpus);
bool msr_probe_modifiable(bool verbose);
int msr_disable_prefetchers(bool verbose);
int msr_enable_prefetchers(bool verbose);
bool msr_disable_prefetcher_on_cpu(int cpu, bool verbose);
void msr_restore_prefetchers(void);
bool msr_is_available(void);
bool msr_is_amd(void);
bool msr_is_amd_zen3_plus(void);
uint64_t msr_get_disable_mask(void);
uint64_t msr_get_enable_clear_mask(void);
uint64_t msr_calc_intel_disable_mask(uint32_t family, uint32_t model);
uint64_t msr_calc_intel_enable_clear_mask(uint32_t family, uint32_t model);

void print_usage(void);

#endif /* MLC_HELPER_H */
