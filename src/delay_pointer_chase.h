#ifndef MLC_DELAY_POINTER_CHASE_H
#define MLC_DELAY_POINTER_CHASE_H

#include <stddef.h>
#include <stdbool.h>

#define DELAY_PC_DEFAULT_SLOTS    31250000UL
#define DELAY_PC_DEFAULT_ITERS    10000000
#define DELAY_PC_DEFAULT_STRIDE   1
#define DELAY_PC_DEFAULT_SWEEP    64
#define DELAY_PC_DEFAULT_CHAIN    0
#define DELAY_PC_DEFAULT_CORE_MHZ 0
#define DELAY_PC_DEFAULT_OVERHEAD (-1)

typedef struct {
	size_t dslots;
	int diters;
	int dstride;
	int dsweep;
	int dchain;
	int dcore_mhz;
	int doverhead;
	int target_core;
} delay_pc_config_t;

int run_delay_pointer_chase(const delay_pc_config_t *cfg, char *err_buf,
			    size_t err_buf_len);

#endif /* MLC_DELAY_POINTER_CHASE_H */
