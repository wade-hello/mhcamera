#ifndef MHCAMERA_SERVICE_H
#define MHCAMERA_SERVICE_H

#include "bridge.h"
#include "xiaomi_api_client.h"
#include "rtsp.h"
#include "media_state.h"

#include <cjson/cJSON.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xc_service;
typedef bool (*xc_input_prepare_guard_fn)(void *userdata);

enum xc_auth_state {
    XC_AUTH_IDLE = 0,
    XC_AUTH_WORKING = 1,
    XC_AUTH_SMS_REQUIRED = 2,
    XC_AUTH_AUTHENTICATED = 3,
    XC_AUTH_REAUTH_REQUIRED = 4,
    XC_AUTH_ERROR = 5,
    XC_AUTH_RETRYING = 6,
};

enum xc_auth_reason {
    XC_AUTH_REASON_NONE = 0,
    XC_AUTH_REASON_SMS_START = 1,
    XC_AUTH_REASON_SMS_VERIFY = 2,
    XC_AUTH_REASON_CLOUD_SESSION_REJECTED = 3,
    XC_AUTH_REASON_PASS_TOKEN_REJECTED = 4,
    XC_AUTH_REASON_AUTH_CLEAR = 5,
};

enum xc_catalog_state {
    XC_CATALOG_IDLE = 0,
    XC_CATALOG_QUEUED = 1,
    XC_CATALOG_LOADING = 2,
    XC_CATALOG_READY = 3,
    XC_CATALOG_ERROR = 4,
    XC_CATALOG_RETRYING = 5,
};

const char *xc_auth_state_text(enum xc_auth_state state);
const char *xc_auth_reason_text(enum xc_auth_reason reason);
const char *xc_catalog_state_text(enum xc_catalog_state state);
const char *xc_media_state_text(enum xc_media_state state);

struct xc_service_ops {
    struct xc_rtsp *rtsp;
    /* Allocates account and catalog staging blocks released with free().
     * The callback must return NULL with errno=ENOMEM on allocation failure. */
    void *(*catalog_alloc)(void *userdata, size_t size);
    int (*xiaomi_call)(void *userdata,
                       const struct xc_xiaomi_request *request,
                       struct xc_xiaomi_response *response);
    int (*source_set)(void *userdata, const char *source);
    int (*source_stop)(void *userdata);
    /* A synchronous input preparation must invoke patch_allowed immediately
     * after reading host input.mode and before issuing config.patch. The guard
     * and its userdata are borrowed for this call only. */
    int (*input_prepare)(void *userdata,
                         xc_input_prepare_guard_fn patch_allowed,
                         void *guard_userdata);
    int (*media_start)(void *userdata);
    void (*media_stop)(void *userdata);
    /* Called while service.lock is held: MUST only set cancellation/shutdown,
     * never join, invoke service callbacks, or perform source/network control. */
    void (*media_request_stop)(void *userdata);
    /* Restarts the private sidecar. clear_token atomically removes the only
     * Xiaomi credential before the replacement process is started. */
    int (*sidecar_reset)(void *userdata, bool clear_token);
    /* <0 means not committed, 0 durable, >0 committed with durability not
     * confirmed. A committed result must never be retried as uncommitted. */
    int (*persist_selection)(void *userdata,
                             const char *account_id,
                             const char *region,
                             bool exists,
                             bool enabled,
                             const char *camera_id,
                             const char *name,
                             const char *model,
                             unsigned int channel);
    /* retry_after_wall_ms is a PII-free wall-clock deadline. A missing
     * file is reported as zero. Both calls use the store's atomic contract. */
    int (*phone_rate_load)(void *userdata, uint64_t *retry_after_wall_ms);
    int (*phone_rate_save)(void *userdata, uint64_t retry_after_wall_ms);
    int (*persist_status)(void *userdata, const char *status_json);
    uint64_t (*monotonic_ms)(void *userdata);
    uint64_t (*wallclock_ms)(void *userdata);
    void *userdata;
};

int xc_service_create(struct xc_service **service_out,
                      const struct xc_service_ops *ops);
void xc_service_stop(struct xc_service *service);
void xc_service_destroy(struct xc_service *service);
int xc_service_refresh_auth(struct xc_service *service);
void xc_service_media_state(struct xc_service *service,
                            enum xc_media_state state,
                            const char *codec,
                            const char *message_key);
void xc_service_restore_selection(struct xc_service *service,
                                  const char *account_id,
                                  const char *region,
                                  bool selected,
                                  bool enabled,
                                  const char *camera_id,
                                  const char *name,
                                  const char *model,
                                  unsigned int channel);
/* Called for every observed host input.mode state. A non-bitstream observation
 * requests cancellation of only the AI attachment and queues reconciliation;
 * independent RTSP demand is preserved. No join, source control or cloud work
 * runs synchronously in the config callback. */
void xc_service_input_changed(struct xc_service *service, bool bitstream);

/* Compatible with xc_bridge_dispatch_json(). The returned cJSON is owned by
 * the caller. */
cJSON *xc_service_route(const struct xc_route_request *request, void *userdata);

#ifdef __cplusplus
}
#endif

#endif
