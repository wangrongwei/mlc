#ifndef MLC_BUSYTHREAD_H
#define MLC_BUSYTHREAD_H

void create_pstate_threads_on_other_cores(const char *core_list_str);
void stop_pstate_threads(void);

#endif /* MLC_BUSYTHREAD_H */
