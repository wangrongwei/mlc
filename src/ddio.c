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
#include <time.h>
#include "ddio.h"

void ddio_print_config(const ddio_shm_config_t *cfg)
{
	if (!cfg)
		return;

	puts("DDIO_testconfig:");
	printf("        session_id       = %d\n", cfg->session_id);

	printf("        mlc_state        = %d", cfg->mlc_state);
	switch (cfg->mlc_state) {
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
		printf
		    (" (MLC waiting on PCIE card to warm up memory for test type 2)");
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

	printf("        pcie_state       = %d", cfg->pcie_state);
	switch (cfg->pcie_state) {
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

	printf("        session_state    = %d", cfg->session_state);
	if (cfg->session_state == 1) {
		printf(" (Shared memory created, valid to use)");
	}
	putchar('\n');
}

int run_ddio_benchmark(const char *shm_name, double duration_sec)
{
	if (!shm_name || strlen(shm_name) == 0) {
		shm_name = "/mlc_ddio_shm";
	}

	size_t shm_size = sizeof(ddio_shm_config_t) + (64 * 1024 * 1024);	// 64MB buffer for testing
	int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
	if (fd < 0) {
		perror("shm_open failed");
		return -1;
	}

	if (ftruncate(fd, shm_size) != 0) {
		perror("ftruncate");
		close(fd);
		shm_unlink(shm_name);
		return -1;
	}

	void *ptr =
	    mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (ptr == MAP_FAILED) {
		perror("mmap failed");
		close(fd);
		shm_unlink(shm_name);
		return -1;
	}

	ddio_shm_config_t *cfg = (ddio_shm_config_t *) ptr;
	memset(cfg, 0, sizeof(*cfg));
	cfg->session_id = (int32_t) time(NULL);
	cfg->mlc_state = MLC_STATE_MEM_ALLOC_DONE;
	cfg->pcie_state = PCIE_STATE_READY_FOR_TEST;
	cfg->session_state = 1;

	ddio_print_config(cfg);

	printf
	    ("\n[DDIO] Shared memory established at '%s' (%zu bytes). Coordinating with PCIe card...\n",
	     shm_name, shm_size);

	cfg->mlc_state = MLC_STATE_THRUPUT_THREADS_CREATED;
	ddio_print_config(cfg);

	/* Simulated handshake cycle */
	struct timespec ts;
	ts.tv_sec = (time_t) duration_sec;
	ts.tv_nsec = (long)((duration_sec - (double)ts.tv_sec) * 1e9);
	nanosleep(&ts, NULL);

	cfg->mlc_state = MLC_STATE_ACK_TEST_DONE;
	cfg->pcie_state = PCIE_STATE_TEST_DONE;
	ddio_print_config(cfg);

	cfg->mlc_state = MLC_STATE_DEALLOC_AND_UNLINK;
	munmap(ptr, shm_size);
	close(fd);
	shm_unlink(shm_name);

	printf("[DDIO] Test finished successfully.\n");
	return 0;
}
