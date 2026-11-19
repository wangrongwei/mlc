#ifndef DDIO_H
#define DDIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define DDIO_DEFAULT_SHM_NAME "ddio_default_sharedmem"
#define DDIO_SHM_TRUNC_BYTES  0x1000
#define DDIO_SHM_MAP_BYTES    0x28

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

typedef enum {
	DDIO_SESSION_NONE = 0,
	DDIO_SESSION_SHM_VALID = 1,
	DDIO_SESSION_MLC_ALLOC_DONE = 2,
	DDIO_SESSION_PRODUCER_DONE = 3,
	DDIO_SESSION_CONSUMER_DONE = 4,
	DDIO_SESSION_TEST_DONE = 5,
	DDIO_SESSION_UNLINKED = 6
} ddio_session_state_t;

typedef enum {
	DDIO_TEST_TYPE_UNSET = 0,
	DDIO_TEST_TYPE_MLC_PRODUCER = 1,
	DDIO_TEST_TYPE_PCIE_PRODUCER = 2
} ddio_test_type_t;

/*
 * Exact 40-byte (0x28) shared-memory ABI matching intel/mlc FUN_0012f1e0 &
 * FUN_0012f6f0.
 */
typedef struct {
	int32_t session_id;	/* +0x00 */
	int32_t mlc_state;	/* +0x04 */
	int32_t pcie_state;	/* +0x08 */
	int32_t lock;		/* +0x0C: atomic xchg spinlock */
	int32_t session_state;	/* +0x10 */
	int32_t type;		/* +0x14: 1=MLC setup for PCIe, 2=PCIe setup for MLC */
	int32_t numcpus;	/* +0x18 */
	int32_t currcpus;	/* +0x1C */
	int32_t mlc_unlink;	/* +0x20 */
	int32_t pcie_unlink;	/* +0x24 */
} ddio_shm_config_t;

_Static_assert(sizeof(ddio_shm_config_t) == DDIO_SHM_MAP_BYTES,
	       "ddio_shm_config_t must be exactly 40 (0x28) bytes");

/* Global CLI configuration setters & query */
void ddio_configure(bool enabled, const char *shm_name, int32_t session_id,
		    int32_t test_type, int32_t slowmode_ms, bool verbose);
bool ddio_is_enabled(void);

/* Atomic spinlock primitives matching FUN_0012f180 / FUN_0012f1b0 */
void ddio_lock(volatile int32_t *lock_word);
void ddio_unlock(volatile int32_t *lock_word);

/* Print DDIO session configuration and state status (FUN_0012f1e0) */
void ddio_print_config(const volatile ddio_shm_config_t *cfg);

/* Lifecycle hooks for get_bw_and_latency (FUN_001229e0) and ThruputThread (FUN_00120430) */
int ddio_open_shm(void);
void ddio_close_shm(void);
int ddio_step_begin(int numcpus);
void ddio_worker_enter(bool *out_is_leader);
void ddio_worker_exit(bool is_leader);
void ddio_step_end(void);

#endif /* DDIO_H */
