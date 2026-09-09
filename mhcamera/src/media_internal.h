#ifndef MHCAMERA_MEDIA_INTERNAL_H
#define MHCAMERA_MEDIA_INTERNAL_H

#include "media.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/types.h>

struct xc_media_owner {
    pthread_mutex_t lock;
    pthread_cond_t stopped_cond;
    pthread_t thread;
    bool joinable;
    bool joining;
    int active_fd;
    atomic_bool stop;
    char socket_path[108];
    pid_t peer_pid;
    xc_media_status_fn status;
    void *userdata;
};

void xc_media_notify_internal(struct xc_media_owner *owner,
                              enum xc_media_state state,
                              const char *codec,
                              const char *message_key);
int xc_media_session_run_internal(struct xc_media_owner *owner);
int xc_media_runtime_init_internal(void);

#endif
