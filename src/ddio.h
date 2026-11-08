#ifndef DDIO_H
#define DDIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
	MLC_STATE_INIT = 0,
	MLC_STATE_MEM_ALLOC_DONE = 1,
	MLC_STATE_THRUPUT_THREADS_CREATED = 2,
	MLC_STATE_WAITING_ON_TEST_CARD_INIT = 3,
	MLC_STATE_WARMUP_TYPE1_DONE = 4,
	MLC_STATE_WAITING_PCIE_WARMUP_TYPE2 = 5,
	MLC_STATE_DONE_USING_MEM_TYPE2 = 6,
	MLC_STATE_ACK_TEST_DONE = 7,
	MLC_STATE_DEALLOC_AND_UNLINK = 8
} mlc_ddio_state_t;

typedef enum {
	PCIE_STATE_NONE = 0,
	PCIE_STATE_INIT_DONE = 1,
	PCIE_STATE_READY_FOR_TEST = 2,
	PCIE_STATE_WARMING_UP_TYPE2 = 3,
	PCIE_STATE_DONE_WARMUP_TYPE2 = 4,
	PCIE_STATE_DONE_CONSUMING_TYPE1 = 5,
	PCIE_STATE_TEST_DONE = 6,
	PCIE_STATE_PROGRAM_DONE = 7
} pcie_ddio_state_t;

typedef struct {
	int32_t session_id;	// +0x00
	int32_t mlc_state;	// +0x04
	int32_t pcie_state;	// +0x08
	int32_t lock;		// +0x0C
	int32_t session_state;	// +0x10
	char reserved[44];
} __attribute__((packed)) ddio_shm_config_t;

/* Print DDIO session configuration and state status */
void ddio_print_config(const ddio_shm_config_t * cfg);

/* Run Intel DDIO shared memory PCIe handshake benchmark */
int run_ddio_benchmark(const char *shm_name, double duration_sec);

#endif /* DDIO_H */
