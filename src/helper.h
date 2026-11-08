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
int msr_disable_prefetchers(bool verbose);
void msr_restore_prefetchers(void);
bool msr_is_available(void);

void print_usage(void);

#endif /* MLC_HELPER_H */
