#ifndef MHCAMERA_MEDIA_H
#define MHCAMERA_MEDIA_H

#include <sys/types.h>
#include "media_state.h"

struct xc_media_owner;

typedef void (*xc_media_status_fn)(void *userdata,
                                   enum xc_media_state state,
                                   const char *codec,
                                   const char *message_key);

int xc_media_owner_create(struct xc_media_owner **owner_out,
                          const char *socket_path,
                          pid_t peer_pid,
                          xc_media_status_fn status,
                          void *userdata);
int xc_media_owner_start(struct xc_media_owner *owner);
/* Nonblocking cancellation only: no join, callbacks, cloud or source control. */
void xc_media_owner_request_stop(struct xc_media_owner *owner);
/* Rebind after stop/join. The owner address stays stable across sidecar resets. */
int xc_media_owner_rebind(struct xc_media_owner *owner, pid_t peer_pid);
void xc_media_owner_stop(struct xc_media_owner *owner);
void xc_media_owner_destroy(struct xc_media_owner *owner);

#endif
