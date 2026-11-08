#ifndef MLC_FASTDELAY_DISPATCH_H
#define MLC_FASTDELAY_DISPATCH_H

#include <stddef.h>

#include <stdint.h>
#include <stdbool.h>

#define DEFAULT_NUM_DELAYS 19

const int *get_default_delays(int *count);
int load_delays_from_file(const char *filename, int *out_delays,
			  int max_delays);
int parse_fastdelay_list(const char *delay_str, int *out_delays,
			 int max_delays);

/* Micro-architectural AVX2/AVX-512 delay-injected throughput kernel worker */
uint64_t run_fastdelay_kernel_worker(void *buf, size_t size, int traffic_type,
				     int delay_val, volatile bool *stop_flag);

#endif /* MLC_FASTDELAY_DISPATCH_H */
