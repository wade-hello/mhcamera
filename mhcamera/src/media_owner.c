#include "media_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#define XC_RECONNECT_ATTEMPTS 3u

void xc_media_notify_internal(struct xc_media_owner *owner,
                              enum xc_media_state state,
                              const char *codec,
                              const char *message_key)
{
    if (owner->status) owner->status(owner->userdata, state, codec, message_key);
}

static void *xc_media_thread(void *userdata)
{
    struct xc_media_owner *owner = userdata;
    unsigned int attempt = 0u;

    xc_media_notify_internal(owner, XC_MEDIA_STARTING, NULL, NULL);
    while (!atomic_load_explicit(&owner->stop, memory_order_relaxed)) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000L};
        unsigned int wait;
        unsigned int wait_ticks;

        if (xc_media_session_run_internal(owner) == 0) break;
        attempt++;
        if (attempt >= XC_RECONNECT_ATTEMPTS) {
            xc_media_notify_internal(owner, XC_MEDIA_ERROR, NULL, "media_session_failed");
            attempt = 0u;
            wait_ticks = 50u;
        } else {
            xc_media_notify_internal(owner, XC_MEDIA_STARTING, NULL, NULL);
            wait_ticks = 5u;
        }
        for (wait = 0u; wait < wait_ticks &&
                        !atomic_load_explicit(&owner->stop, memory_order_relaxed);
             ++wait) nanosleep(&delay, NULL);
        if (!atomic_load_explicit(&owner->stop, memory_order_relaxed) && attempt == 0u)
            xc_media_notify_internal(owner, XC_MEDIA_STARTING, NULL, NULL);
    }
    return NULL;
}

int xc_media_owner_create(struct xc_media_owner **owner_out,
                          const char *socket_path,
                          pid_t peer_pid,
                          xc_media_status_fn status,
                          void *userdata)
{
    struct xc_media_owner *owner;

    if (!owner_out || !socket_path || socket_path[0] != '/' ||
        strlen(socket_path) >= 108u || peer_pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    owner = calloc(1u, sizeof(*owner));
    if (!owner || pthread_mutex_init(&owner->lock, NULL) != 0) {
        free(owner);
        return -1;
    }
    if (pthread_cond_init(&owner->stopped_cond, NULL) != 0) {
        pthread_mutex_destroy(&owner->lock);
        free(owner);
        return -1;
    }
    owner->status = status;
    owner->userdata = userdata;
    owner->active_fd = -1;
    owner->peer_pid = peer_pid;
    snprintf(owner->socket_path, sizeof(owner->socket_path), "%s", socket_path);
    atomic_init(&owner->stop, false);
    *owner_out = owner;
    return 0;
}

int xc_media_owner_start(struct xc_media_owner *owner)
{
    int result;

    if (!owner) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&owner->lock);
    if (owner->joinable || owner->joining) {
        pthread_mutex_unlock(&owner->lock);
        errno = EBUSY;
        return -1;
    }
    if (xc_media_runtime_init_internal() != 0) {
        pthread_mutex_unlock(&owner->lock);
        errno = EIO;
        return -1;
    }
    atomic_store_explicit(&owner->stop, false, memory_order_relaxed);
    result = pthread_create(&owner->thread, NULL, xc_media_thread, owner);
    if (result == 0) owner->joinable = true;
    pthread_mutex_unlock(&owner->lock);
    if (result != 0) errno = result;
    return result == 0 ? 0 : -1;
}

void xc_media_owner_request_stop(struct xc_media_owner *owner)
{
    if (!owner) return;
    pthread_mutex_lock(&owner->lock);
    atomic_store_explicit(&owner->stop, true, memory_order_relaxed);
    if (owner->active_fd >= 0) shutdown(owner->active_fd, SHUT_RDWR);
    pthread_mutex_unlock(&owner->lock);
}

int xc_media_owner_rebind(struct xc_media_owner *owner, pid_t peer_pid)
{
    if (!owner || peer_pid <= 0) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&owner->lock);
    if (owner->joinable || owner->joining) {
        pthread_mutex_unlock(&owner->lock);
        errno = EBUSY;
        return -1;
    }
    owner->peer_pid = peer_pid;
    pthread_mutex_unlock(&owner->lock);
    return 0;
}

void xc_media_owner_stop(struct xc_media_owner *owner)
{
    pthread_t thread;
    bool joinable;

    if (!owner) return;
    pthread_mutex_lock(&owner->lock);
    while (owner->joining)
        pthread_cond_wait(&owner->stopped_cond, &owner->lock);
    joinable = owner->joinable;
    thread = owner->thread;
    if (joinable) {
        owner->joining = true;
        atomic_store_explicit(&owner->stop, true, memory_order_relaxed);
        if (owner->active_fd >= 0) shutdown(owner->active_fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&owner->lock);
    if (joinable) pthread_join(thread, NULL);
    pthread_mutex_lock(&owner->lock);
    if (joinable) {
        owner->joinable = false;
        owner->joining = false;
        pthread_cond_broadcast(&owner->stopped_cond);
    }
    pthread_mutex_unlock(&owner->lock);
}

void xc_media_owner_destroy(struct xc_media_owner *owner)
{
    if (!owner) return;
    xc_media_owner_stop(owner);
    pthread_cond_destroy(&owner->stopped_cond);
    pthread_mutex_destroy(&owner->lock);
    free(owner);
}
