#define _GNU_SOURCE
#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int64_t xc_now_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int xc_process_poll(struct xc_process *process, bool *exited, int *status)
{
    pid_t result;

    if (!process || process->pid <= 0 || !exited) {
        errno = EINVAL;
        return -1;
    }
    if (!process->exited) {
        result = waitpid(process->pid, &process->status, WNOHANG);
        if (result < 0) return -1;
        if (result == process->pid) process->exited = true;
    }
    *exited = process->exited;
    if (status && process->exited) *status = process->status;
    return 0;
}

int xc_process_start(struct xc_process *process,
                     const char *executable,
                     char *const argv[],
                     const char *ready_socket,
                     unsigned int timeout_ms)
{
    struct stat metadata;
    int error_pipe[2];
    int64_t deadline;
    pid_t pid;

    if (!process || !executable || executable[0] != '/' || !argv || !argv[0] ||
        !ready_socket || ready_socket[0] != '/' || timeout_ms == 0u) {
        errno = EINVAL;
        return -1;
    }
    if (lstat(ready_socket, &metadata) == 0) {
        if (!S_ISSOCK(metadata.st_mode) || metadata.st_uid != geteuid()) {
            errno = EPERM;
            return -1;
        }
        if (unlink(ready_socket) != 0) return -1;
    } else if (errno != ENOENT) {
        return -1;
    }
    if (pipe2(error_pipe, O_CLOEXEC) != 0) return -1;
    pid = fork();
    if (pid < 0) goto fail;
    if (pid == 0) {
        int saved;

        close(error_pipe[0]);
        execv(executable, argv);
        saved = errno;
        {
            ssize_t ignored = write(error_pipe[1], &saved, sizeof(saved));
            (void)ignored;
        }
        _exit(127);
    }
    close(error_pipe[1]);
    error_pipe[1] = -1;
    memset(process, 0, sizeof(*process));
    process->pid = pid;
    deadline = xc_now_ms();
    if (deadline < 0 || deadline > INT64_MAX - (int64_t)timeout_ms) goto child_fail;
    deadline += timeout_ms;
    for (;;) {
        bool exited = false;
        int child_errno;
        ssize_t got = read(error_pipe[0], &child_errno, sizeof(child_errno));
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 20000000L};

        if (got == (ssize_t)sizeof(child_errno)) {
            errno = child_errno;
            goto child_fail;
        }
        if (xc_process_poll(process, &exited, NULL) != 0 || exited) {
            errno = ECHILD;
            goto child_fail;
        }
        errno = 0;
        if (lstat(ready_socket, &metadata) == 0) {
            if (!S_ISSOCK(metadata.st_mode) || metadata.st_uid != geteuid()) {
                errno = EPERM;
                goto child_fail;
            }
            if (chmod(ready_socket, 0600) != 0) goto child_fail;
            close(error_pipe[0]);
            return 0;
        }
        if (errno != ENOENT && errno != 0) goto child_fail;
        if (xc_now_ms() >= deadline) {
            errno = ETIMEDOUT;
            goto child_fail;
        }
        nanosleep(&delay, NULL);
    }

child_fail:
    {
        int saved = errno;

        (void)kill(pid, SIGTERM);
        if (!process->exited) (void)xc_process_stop(process, 1000u);
        close(error_pipe[0]);
        errno = saved;
        return -1;
    }
fail:
    {
        int saved = errno;
        close(error_pipe[0]);
        close(error_pipe[1]);
        errno = saved;
        return -1;
    }
}

int xc_process_stop(struct xc_process *process, unsigned int timeout_ms)
{
    int64_t deadline;
    bool force_sent = false;

    if (!process || process->pid <= 0 || timeout_ms == 0u) {
        errno = EINVAL;
        return -1;
    }
    if (process->exited) return 0;
    if (kill(process->pid, SIGTERM) != 0 && errno != ESRCH) return -1;
    deadline = xc_now_ms();
    if (deadline < 0 || deadline > INT64_MAX - (int64_t)timeout_ms) return -1;
    deadline += timeout_ms;
    while (!process->exited) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 20000000L};
        bool exited;

        if (xc_process_poll(process, &exited, NULL) != 0) return -1;
        if (exited) return 0;
        if (xc_now_ms() >= deadline) {
            if (force_sent) {
                errno = ETIMEDOUT;
                return -1;
            }
            if (kill(process->pid, SIGKILL) != 0 && errno != ESRCH) return -1;
            force_sent = true;
            deadline = xc_now_ms();
            if (deadline < 0 || deadline > INT64_MAX - 1000) return -1;
            deadline += 1000;
        }
        nanosleep(&delay, NULL);
    }
    return 0;
}
