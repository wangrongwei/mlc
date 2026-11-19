#ifndef MLC_BUSYTHREAD_H
#define MLC_BUSYTHREAD_H

#include <stdbool.h>

int create_pstate_threads_on_other_cores(const char *core_list_str,
					 int target_core, bool verbose);
bool is_manual_pstate_active(void);

void start_auto_busy_threads(int omit_socket_id, int omit_cpu, bool verbose);
void stop_auto_busy_threads(void);
void stop_pstate_threads(void);

#endif /* MLC_BUSYTHREAD_H */
