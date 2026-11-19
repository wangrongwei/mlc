#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "ddio.h"

static bool g_ddio_enabled = false;
static const char *g_ddio_shm_name = DDIO_DEFAULT_SHM_NAME;
static int32_t g_ddio_session_id = 1;	/* Matches intel/mlc .data 0x350038 */
static int32_t g_ddio_test_type = 0;	/* Matches intel/mlc .bss  0x3582bc */
static int32_t g_ddio_slowmode_ms = 0;	/* Matches intel/mlc .bss  0x3582c0 */
static bool g_ddio_verbose = false;

static int g_ddio_fd = -1;
static volatile ddio_shm_config_t *g_ddio_shm = NULL;

void ddio_configure(bool enabled, const char *shm_name, int32_t session_id,
		    int32_t test_type, int32_t slowmode_ms, bool verbose)
{
	g_ddio_enabled = enabled;
	g_ddio_shm_name =
	    (shm_name && *shm_name != '\0') ? shm_name : DDIO_DEFAULT_SHM_NAME;
	g_ddio_session_id = session_id;
	g_ddio_test_type = test_type;
	g_ddio_slowmode_ms = slowmode_ms;
	g_ddio_verbose = verbose;
}

bool ddio_is_enabled(void)
{
	return g_ddio_enabled;
}

/*
 * Atomic spinlock acquire matching intel/mlc FUN_0012f180:
 *   xchg $1, (%rbx); if old != 0 -> usleep(100000)
 */
void ddio_lock(volatile int32_t *lock_word)
{
	if (!lock_word)
		return;
	while (__atomic_exchange_n(lock_word, 1, __ATOMIC_ACQ_REL) != 0) {
		usleep(100000);
	}
}

/*
 * Spinlock release + optional slowmode delay matching intel/mlc FUN_0012f1b0:
 *   movl $0, (%rdi); if (slowmode_ms != 0) usleep(slowmode_ms * 1000)
 */
void ddio_unlock(volatile int32_t *lock_word)
{
	if (!lock_word)
		return;
	__atomic_store_n(lock_word, 0, __ATOMIC_RELEASE);
	if (g_ddio_slowmode_ms != 0) {
		usleep((useconds_t)g_ddio_slowmode_ms * 1000U);
	}
}

/*
 * Print full 10-field DDIO_testconfig structure matching intel/mlc FUN_0012f1e0.
 */
void ddio_print_config(const volatile ddio_shm_config_t *cfg)
{
	if (!cfg) {
		puts("DDIO_testconfig: <NULL pointer>");
		return;
	}

	puts("DDIO_testconfig:");
	printf("        session_id       = %d\n", cfg->session_id);

	int32_t mstate = cfg->mlc_state;
	printf("        mlc_state        = %d", mstate);
	switch (mstate) {
	case MLC_STATE_MEM_ALLOC_DONE:
		printf(" (MLC memory address allocation done)");
		break;
	case MLC_STATE_THRUPUT_THREADS_CREATED:
		printf(" (MLC throughput threads created)");
		break;
	case MLC_STATE_WAITING_ON_TEST_CARD_INIT:
		printf(" (MLC waiting on test card to init)");
		break;
	case MLC_STATE_WARMUP_TYPE1_DONE:
		printf(" (MLC done warming up memory for test type 1 init)");
		break;
	case MLC_STATE_WAITING_PCIE_WARMUP_TYPE2:
		printf(" (MLC waiting on PCIE card to warm up memory for test type 2)");
		break;
	case MLC_STATE_DONE_USING_MEM_TYPE2:
		printf(" (MLC done using memory for test type 2)");
		break;
	case MLC_STATE_ACK_TEST_DONE:
		printf(" (MLC ack test done)");
		break;
	case MLC_STATE_DEALLOC_AND_UNLINK:
		printf(" (MLC dealloc and unlink momentarily)");
		break;
	default:
		break;
	}
	putchar('\n');

	int32_t pstate = cfg->pcie_state;
	printf("        pcie_state       = %d", pstate);
	switch (pstate) {
	case PCIE_STATE_INIT_DONE:
		printf(" (Found shared memory, PCIE init done)");
		break;
	case PCIE_STATE_READY_FOR_TEST:
		printf(" (Ready for test)");
		break;
	case PCIE_STATE_WARMING_UP_TYPE2:
		printf(" (Warming up memory for type 2)");
		break;
	case PCIE_STATE_DONE_WARMUP_TYPE2:
		printf(" (Done warming up for type 2)");
		break;
	case PCIE_STATE_DONE_CONSUMING_TYPE1:
		printf(" (Done consuming memory for type 1)");
		break;
	case PCIE_STATE_TEST_DONE:
		printf(" (Test is done)");
		break;
	case PCIE_STATE_PROGRAM_DONE:
		printf(" (Program done)");
		break;
	default:
		break;
	}
	putchar('\n');

	printf("        lock             = %d\n", cfg->lock);

	int32_t sstate = cfg->session_state;
	printf("        session_state    = %d", sstate);
	switch (sstate) {
	case DDIO_SESSION_SHM_VALID:
		printf(" (Shared memory created, valid to use)");
		break;
	case DDIO_SESSION_MLC_ALLOC_DONE:
		printf(" (MLC has allocated memory)");
		break;
	case DDIO_SESSION_PRODUCER_DONE:
		printf(" (Producer is done)");
		break;
	case DDIO_SESSION_CONSUMER_DONE:
		printf(" (Consumer is done)");
		break;
	case DDIO_SESSION_TEST_DONE:
		printf(" (Test is done)");
		break;
	case DDIO_SESSION_UNLINKED:
		printf(" (Unlinked momentarily)");
		break;
	default:
		break;
	}
	putchar('\n');

	int32_t t = cfg->type;
	printf("        type             = %d", t);
	if (t == DDIO_TEST_TYPE_MLC_PRODUCER) {
		printf(" (MLC will setup memory for PCIE test card to use)");
	} else if (t == DDIO_TEST_TYPE_PCIE_PRODUCER) {
		printf(" (PCIE test card will setup memory for MLC to use)");
	}
	putchar('\n');

	printf("        numcpus          = %d\n", cfg->numcpus);
	printf("        currcpus         = %d\n", cfg->currcpus);
	printf("        mlc_unlink       = %d\n", cfg->mlc_unlink);
	printf("        pcie_unlink      = %d\n", cfg->pcie_unlink);
}

/*
 * Open and map 40-byte DDIO shared-memory region matching intel/mlc FUN_0012f6f0.
 */
int ddio_open_shm(void)
{
	g_ddio_fd = shm_open(g_ddio_shm_name, O_CREAT | O_RDWR, 0666);
	if (g_ddio_fd == -1) {
		perror("shm_open failed");
		return -1;
	}

	if (ftruncate(g_ddio_fd, DDIO_SHM_TRUNC_BYTES) == -1) {
		perror("ftruncate");
		close(g_ddio_fd);
		g_ddio_fd = -1;
		return -1;
	}

	void *mapped = mmap(NULL, DDIO_SHM_MAP_BYTES, PROT_READ | PROT_WRITE,
			    MAP_SHARED, g_ddio_fd, 0);
	if (mapped == MAP_FAILED) {
		perror("mmap failed");
		close(g_ddio_fd);
		g_ddio_fd = -1;
		g_ddio_shm = NULL;
		return -1;
	}

	g_ddio_shm = (volatile ddio_shm_config_t *)mapped;
	if (g_ddio_verbose) {
		printf("Shared Memory Name: %s, size :%ld\n", g_ddio_shm_name,
		       (long)DDIO_SHM_MAP_BYTES);
		ddio_print_config(g_ddio_shm);
	}
	return 0;
}

/*
 * Unmap, close, and unlink DDIO shared memory matching intel/mlc FUN_0012f140.
 */
void ddio_close_shm(void)
{
	if (g_ddio_shm != NULL) {
		munmap((void *)g_ddio_shm, DDIO_SHM_MAP_BYTES);
		g_ddio_shm = NULL;
	}
	if (g_ddio_fd >= 0) {
		close(g_ddio_fd);
		g_ddio_fd = -1;
	}
	shm_unlink(g_ddio_shm_name);
}

/*
 * Step initialization in get_bw_and_latency (FUN_001229e0 lines 1315-1359)
 * after worker memory buffers have been allocated.
 */
int ddio_step_begin(int numcpus)
{
	if (!g_ddio_enabled)
		return 0;

	if (g_ddio_verbose) {
		puts("Creating DDIO shared memory region");
	}
	if (ddio_open_shm() != 0 || !g_ddio_shm)
		return -1;

	if (g_ddio_verbose) {
		puts("Locking DDIO shared region for initialization");
	}
	ddio_lock(&g_ddio_shm->lock);
	if (g_ddio_verbose) {
		puts("Acquired the DDIO lock after allocation of memory");
	}

	g_ddio_shm->mlc_state = MLC_STATE_MEM_ALLOC_DONE;
	g_ddio_shm->mlc_unlink = 0;
	g_ddio_shm->pcie_unlink = 0;
	g_ddio_shm->pcie_state = PCIE_STATE_NONE;
	g_ddio_shm->currcpus = 0;
	g_ddio_shm->session_id = g_ddio_session_id;
	g_ddio_shm->type = g_ddio_test_type;
	g_ddio_shm->numcpus = numcpus;

	if (g_ddio_verbose) {
		ddio_print_config(g_ddio_shm);
	}

	g_ddio_shm->session_state = DDIO_SESSION_SHM_VALID;
	ddio_unlock(&g_ddio_shm->lock);
	fflush(stdout);
	return 0;
}

/*
 * Entry handshake in ThruputThread (FUN_00120430 lines 70-136).
 * Elects the last arriving worker (currcpus == numcpus) as the DDIO leader,
 * transitions mlc_state 2 -> 3, waits for pcie_state >= 1, and for type == 2
 * transitions mlc_state -> 5 and waits for pcie_state >= 4.
 */
void ddio_worker_enter(bool *out_is_leader)
{
	if (out_is_leader)
		*out_is_leader = false;
	if (!g_ddio_enabled || !g_ddio_shm)
		return;

	ddio_lock(&g_ddio_shm->lock);
	if (g_ddio_verbose) {
		puts("Acquired the DDIO lock after thread creation");
	}

	int32_t arrived = g_ddio_shm->currcpus + 1;
	g_ddio_shm->currcpus = arrived;

	if (arrived == g_ddio_shm->numcpus) {
		if (out_is_leader)
			*out_is_leader = true;

		g_ddio_shm->mlc_state = MLC_STATE_THRUPUT_THREADS_CREATED;
		if (g_ddio_verbose) {
			ddio_print_config(g_ddio_shm);
		}
		ddio_unlock(&g_ddio_shm->lock);

		ddio_lock(&g_ddio_shm->lock);
		g_ddio_shm->mlc_state = MLC_STATE_WAITING_ON_TEST_CARD_INIT;
		if (g_ddio_verbose) {
			puts("MLC DDIO main thread: mlc_state 3");
		}
		ddio_unlock(&g_ddio_shm->lock);
		fflush(stdout);

		while (__atomic_load_n(&g_ddio_shm->pcie_state,
				       __ATOMIC_ACQUIRE) < PCIE_STATE_INIT_DONE) {
			usleep(10);
		}
		if (g_ddio_verbose) {
			puts("MLC DDIO main thread: PCIE state is initialized");
		}

		if (g_ddio_test_type == DDIO_TEST_TYPE_PCIE_PRODUCER) {
			ddio_lock(&g_ddio_shm->lock);
			g_ddio_shm->mlc_state =
			    MLC_STATE_WAITING_PCIE_WARMUP_TYPE2;
			if (g_ddio_verbose) {
				puts("MLC DDIO main thread: mlc_state 5, waiting for PCIE card to set state 4");
			}
			ddio_unlock(&g_ddio_shm->lock);
			fflush(stdout);

			while (__atomic_load_n(&g_ddio_shm->pcie_state,
					       __ATOMIC_ACQUIRE) <
			       PCIE_STATE_DONE_WARMUP_TYPE2) {
				usleep(1000);
			}
			puts("MLC DDIO main thread: mlc_state 5, PCIE card is done warming up, running test ");
			fflush(stdout);
		}
	} else {
		ddio_unlock(&g_ddio_shm->lock);

		while (__atomic_load_n(&g_ddio_shm->pcie_state,
				       __ATOMIC_ACQUIRE) < PCIE_STATE_INIT_DONE) {
			usleep(10);
		}
		if (g_ddio_test_type == DDIO_TEST_TYPE_PCIE_PRODUCER) {
			while (__atomic_load_n(&g_ddio_shm->pcie_state,
					       __ATOMIC_ACQUIRE) <
			       PCIE_STATE_DONE_WARMUP_TYPE2) {
				usleep(1000);
			}
		}
	}
}

/*
 * Exit handshake in ThruputThread (FUN_00120430 lines 1178-1245), executed
 * exclusively by the elected DDIO leader thread after all workers complete
 * their timed memory traffic pass.
 */
void ddio_worker_exit(bool is_leader)
{
	if (!g_ddio_enabled || !is_leader || !g_ddio_shm)
		return;

	int32_t t = g_ddio_shm->type;
	if (t == DDIO_TEST_TYPE_PCIE_PRODUCER) {
		ddio_lock(&g_ddio_shm->lock);
		g_ddio_shm->mlc_state = MLC_STATE_DONE_USING_MEM_TYPE2;
		if (g_ddio_verbose) {
			puts("MLC DDIO main thread: mlc_state 6, waiting for PCIE card to set state 6 to mark test done");
		}
		ddio_unlock(&g_ddio_shm->lock);
		fflush(stdout);

		while (__atomic_load_n(&g_ddio_shm->pcie_state,
				       __ATOMIC_ACQUIRE) <
		       PCIE_STATE_TEST_DONE) {
			usleep(1000);
		}

		ddio_lock(&g_ddio_shm->lock);
		g_ddio_shm->mlc_state = MLC_STATE_ACK_TEST_DONE;
		if (g_ddio_verbose) {
			puts("MLC DDIO main thread: mlc_state 7");
		}
		ddio_unlock(&g_ddio_shm->lock);
		fflush(stdout);

		while (__atomic_load_n(&g_ddio_shm->pcie_state,
				       __ATOMIC_ACQUIRE) <
		       PCIE_STATE_PROGRAM_DONE) {
			usleep(1000);
		}
	} else if (t == DDIO_TEST_TYPE_MLC_PRODUCER) {
		ddio_lock(&g_ddio_shm->lock);
		g_ddio_shm->mlc_state = MLC_STATE_WARMUP_TYPE1_DONE;
		if (g_ddio_verbose) {
			puts("MLC DDIO main thread: mlc_state 4, done warming up data");
		}
		ddio_unlock(&g_ddio_shm->lock);
		fflush(stdout);

		while (__atomic_load_n(&g_ddio_shm->pcie_state,
				       __ATOMIC_ACQUIRE) <
		       PCIE_STATE_TEST_DONE) {
			usleep(1000);
		}

		ddio_lock(&g_ddio_shm->lock);
		g_ddio_shm->mlc_state = MLC_STATE_ACK_TEST_DONE;
		if (g_ddio_verbose) {
			puts("MLC DDIO main thread: mlc_state 7");
		}
		ddio_unlock(&g_ddio_shm->lock);
		fflush(stdout);

		while (__atomic_load_n(&g_ddio_shm->pcie_state,
				       __ATOMIC_ACQUIRE) <
		       PCIE_STATE_PROGRAM_DONE) {
			usleep(1000);
		}
	}

	puts("MLC DDIO main thread: Closing DDIO shared memory");
	fflush(stdout);
	ddio_close_shm();
}

void ddio_step_end(void)
{
	if (!g_ddio_enabled)
		return;
	if (g_ddio_shm != NULL || g_ddio_fd >= 0) {
		ddio_close_shm();
	}
}
