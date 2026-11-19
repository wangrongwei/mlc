#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <cpuid.h>
#include "busythread.h"
#include "allocation.h"

#define MAX_BUSY_THREADS 256

static volatile bool g_busy_stop = false;
static pthread_t g_busy_threads[MAX_BUSY_THREADS];
static int g_busy_count = 0;
static bool g_manual_pstate_active = false;

static void *busy_worker(void *arg)
{
	int cpu = (int)(intptr_t)arg;
	numa_pin_cpu(cpu);

	while (!g_busy_stop) {
		sched_yield();
	}
	return NULL;
}

/*
 * Check CPU architecture skip predicates matching intel/mlc FUN_0012b8f0 /
 * FUN_00125700: skip automatic busythread spawning on GNR (0xAD, 0xAE),
 * SRF/GRR (0xAF, 0xB6), CWF (0xDD), and DMR (family 0x13, model 0x01).
 */
static bool cpu_skips_auto_busy_threads(void)
{
	unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
	char vendor[13] = { 0 };

	if (__get_cpuid(0, &eax, &ebx, &ecx, &edx)) {
		memcpy(vendor, &ebx, 4);
		memcpy(vendor + 4, &edx, 4);
		memcpy(vendor + 8, &ecx, 4);
		vendor[12] = '\0';
	}

	if (strcmp(vendor, "GenuineIntel") != 0)
		return false;

	if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx))
		return false;

	uint32_t base_fam = (eax >> 8) & 0xfU;
	uint32_t family =
	    base_fam + ((base_fam == 0xfU) ? ((eax >> 20) & 0xffU) : 0U);
	uint32_t base_mod = (eax >> 4) & 0xfU;
	uint32_t ext_mod =
	    (family == 6U || family == 0xfU) ? ((eax >> 12) & 0xf0U) : 0U;
	uint32_t model = base_mod | ext_mod;

	if (family == 0x13U && model == 0x01U)
		return true;

	if (family == 6U) {
		switch (model) {
		case 0xadU:
		case 0xaeU:
		case 0xafU:
		case 0xb6U:
		case 0xddU:
			return true;
		default:
			break;
		}
	}

	return false;
}

static void join_active_busy_threads(void)
{
	if (g_busy_count == 0)
		return;
	g_busy_stop = true;
	for (int i = 0; i < g_busy_count; i++) {
		pthread_join(g_busy_threads[i], NULL);
	}
	g_busy_count = 0;
}

static bool spawn_busy_thread_on_cpu(int cpu, bool verbose)
{
	if (g_busy_count >= MAX_BUSY_THREADS)
		return false;
	if (pthread_create(&g_busy_threads[g_busy_count], NULL, busy_worker,
			   (void *)(intptr_t)cpu) != 0) {
		return false;
	}
	g_busy_count++;
	if (verbose) {
		int sock_id = cpu_get_socket_id(cpu);
		printf
		    ("Core %d is running a busy loop to keep socket %d from low frequency states\n",
		     cpu, sock_id >= 0 ? sock_id : 0);
	}
	return true;
}

int create_pstate_threads_on_other_cores(const char *core_list_str,
					 int target_core, bool verbose)
{
	if (!core_list_str || *core_list_str == '\0')
		return 0;

	stop_pstate_threads();
	g_busy_stop = false;

	int total_cpus = numa_get_total_cpus();
	char buf[1024];
	strncpy(buf, core_list_str, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	char *saveptr = NULL;
	char *token = strtok_r(buf, ",", &saveptr);
	while (token && g_busy_count < MAX_BUSY_THREADS) {
		while (*token && isspace((unsigned char)*token))
			token++;
		if (*token == '\0') {
			token = strtok_r(NULL, ",", &saveptr);
			continue;
		}

		int start = -1, end = -1;
		char extra = '\0';
		if (sscanf(token, "%d-%d%c", &start, &end, &extra) == 2) {
			if (start < 0 || end < start || end >= total_cpus) {
				fprintf(stderr,
					"Invalid core range %s in -p option (valid range: 0..%d)\n",
					token, total_cpus - 1);
				stop_pstate_threads();
				return -1;
			}
			for (int cpu = start;
			     cpu <= end && g_busy_count < MAX_BUSY_THREADS;
			     cpu++) {
				if (target_core >= 0 && cpu == target_core) {
					printf
					    ("Warning: Core %d specified in -p is also the measurement core\n",
					     cpu);
				}
				spawn_busy_thread_on_cpu(cpu, verbose);
			}
		} else if (sscanf(token, "%d%c", &start, &extra) == 1) {
			if (start < 0 || start >= total_cpus) {
				fprintf(stderr,
					"Invalid core %d in -p option (valid range: 0..%d)\n",
					start, total_cpus - 1);
				stop_pstate_threads();
				return -1;
			}
			if (target_core >= 0 && start == target_core) {
				printf
				    ("Warning: Core %d specified in -p is also the measurement core\n",
				     start);
			}
			spawn_busy_thread_on_cpu(start, verbose);
		} else {
			fprintf(stderr, "Invalid -p parameter token: %s\n",
				token);
			stop_pstate_threads();
			return -1;
		}
		token = strtok_r(NULL, ",", &saveptr);
	}

	if (g_busy_count > 0) {
		g_manual_pstate_active = true;
		sleep(1);
	}
	return 0;
}

bool is_manual_pstate_active(void)
{
	return g_manual_pstate_active;
}

void start_auto_busy_threads(int omit_socket_id, int omit_cpu, bool verbose)
{
	if (g_manual_pstate_active)
		return;
	if (numa_get_num_sockets() <= 1)
		return;
	if (numa_has_insufficient_cores_for_busy())
		return;
	if (cpu_skips_auto_busy_threads())
		return;

	join_active_busy_threads();
	g_busy_stop = false;

	int num_sockets = numa_get_num_sockets();
	for (int s = 0; s < num_sockets; s++) {
		const socket_info_t *sock = numa_get_socket(s);
		if (!sock || sock->num_cpus <= 0)
			continue;
		if (sock->socket_id == omit_socket_id)
			continue;

		int busy_cpu = sock->reserved_cpus[0];
		if (busy_cpu < 0 || busy_cpu == omit_cpu)
			continue;

		spawn_busy_thread_on_cpu(busy_cpu, verbose);
	}

	if (g_busy_count > 0) {
		sleep(1);
	}
}

void stop_auto_busy_threads(void)
{
	if (g_manual_pstate_active)
		return;
	join_active_busy_threads();
}

void stop_pstate_threads(void)
{
	join_active_busy_threads();
	g_manual_pstate_active = false;
}
