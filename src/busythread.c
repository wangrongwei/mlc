#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include "busythread.h"
#include "allocation.h"

#define MAX_BUSY_THREADS 256

static volatile bool g_busy_stop = false;
static pthread_t g_busy_threads[MAX_BUSY_THREADS];
static int g_busy_count = 0;

static void *busy_worker(void *arg)
{
	int cpu = (int)(intptr_t) arg;
	numa_pin_cpu(cpu);

	while (!g_busy_stop) {
		sched_yield();
	}
	return NULL;
}

void create_pstate_threads_on_other_cores(const char *core_list_str)
{
	if (!core_list_str)
		return;

	g_busy_stop = false;
	g_busy_count = 0;

	char buf[512];
	strncpy(buf, core_list_str, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	char *saveptr = NULL;
	char *token = strtok_r(buf, ",", &saveptr);
	while (token && g_busy_count < MAX_BUSY_THREADS) {
		int start, end;
		if (sscanf(token, "%d-%d", &start, &end) == 2) {
			for (int cpu = start;
			     cpu <= end && g_busy_count < MAX_BUSY_THREADS;
			     cpu++) {
				pthread_create(&g_busy_threads[g_busy_count++],
					       NULL, busy_worker,
					       (void *)(intptr_t) cpu);
			}
		} else if (sscanf(token, "%d", &start) == 1) {
			pthread_create(&g_busy_threads[g_busy_count++], NULL,
				       busy_worker, (void *)(intptr_t) start);
		}
		token = strtok_r(NULL, ",", &saveptr);
	}
}

void stop_pstate_threads(void)
{
	if (g_busy_count == 0)
		return;
	g_busy_stop = true;
	for (int i = 0; i < g_busy_count; i++) {
		pthread_join(g_busy_threads[i], NULL);
	}
	g_busy_count = 0;
}
