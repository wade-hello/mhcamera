#ifndef MHCAMERA_PROCESS_H
#define MHCAMERA_PROCESS_H

#include <stdbool.h>
#include <sys/types.h>

struct xc_process {
    pid_t pid;
    bool exited;
    int status;
};

int xc_process_start(struct xc_process *process,
                     const char *executable,
                     char *const argv[],
                     const char *ready_socket,
                     unsigned int timeout_ms);
int xc_process_poll(struct xc_process *process, bool *exited, int *status);
int xc_process_stop(struct xc_process *process, unsigned int timeout_ms);

#endif
