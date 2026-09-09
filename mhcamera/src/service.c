#define _GNU_SOURCE
#include "service.h"
#include "source.h"
#include "store.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ainice/protocol_error_codes.h"

#define XC_OPERATION_ID_MAX 128u
#define XC_ACCOUNT_ID_MAX 64u
#define XC_CALLING_CODE_MAX 4u
#define XC_NATIONAL_NUMBER_MAX 15u
#define XC_CODE_MAX 64u
#define XC_CAMERA_ID_MAX 64u
#define XC_CAMERA_MODEL_MAX 127u
#define XC_OPERATION_CACHE_MAX 16u
#define XC_CATALOG_RETRY_LIMIT 3u

static const unsigned int xc_catalog_retry_delays_ms[XC_CATALOG_RETRY_LIMIT] = {
    1000u, 3000u, 10000u,
};

enum xc_work_kind {
    XC_WORK_NONE = 0,
    XC_WORK_REFRESH,
    XC_WORK_SMS_START,
    XC_WORK_SMS_VERIFY,
    XC_WORK_SMS_RESEND,
    XC_WORK_CANCEL,
    XC_WORK_CLEAR,
    XC_WORK_CAMERA_LIST,
    XC_WORK_CAMERA_REGION,
    XC_WORK_CAMERA_SELECT,
    XC_WORK_CAMERA_STOP,
    XC_WORK_RECONCILE,
    XC_WORK_RTSP,
};

struct xc_rtsp_job {
    bool credentials;
    bool audio_field;
    bool value;
    bool done;
    cJSON *response;
};

struct xc_work {
    enum xc_work_kind kind;
    char operation_id[XC_OPERATION_ID_MAX + 1u];
    char calling_code[XC_CALLING_CODE_MAX + 1u];
    char national_number[XC_NATIONAL_NUMBER_MAX + 1u];
    char code[XC_CODE_MAX + 1u];
    char account_id[XC_ACCOUNT_ID_MAX + 1u];
    char camera_id[XC_CAMERA_ID_MAX + 1u];
    char source[512];
    char core_source[512];
    struct xc_rtsp_job *rtsp_job;
    char name[128];
    char model[128];
    enum xc_region region;
    unsigned int channel;
    bool verify_restored_selection;
    uint64_t recovery_generation;
    uint64_t input_generation;
    unsigned int incomplete_retries;
    unsigned int network_retries;
    unsigned int retry_attempt;
    bool background;
};

enum xc_recovery_policy { XC_RECOVERY_POLICY_NONE = 0, XC_RECOVERY_NETWORK, XC_RECOVERY_INCOMPLETE };
struct xc_recovery {
    enum xc_recovery_policy policy;
    bool scheduled;
    uint64_t due_ms;
    unsigned int attempt;
    unsigned int delay_ms;
    struct xc_work work;
};

struct xc_service {
    pthread_mutex_t lock;
    pthread_cond_t work_cond;
    pthread_t worker;
    bool worker_started;
    bool stopping;
    bool pending;
    bool active;
    enum xc_work_kind active_kind;
    struct xc_work work;
    uint64_t recovery_generation;
    struct xc_recovery recovery;
    struct xc_service_ops ops;
    /* Only the worker writes media ownership and the last validated core source. */
    bool media_active;
    char active_core_source[512];
    char active_source[512];
    char active_camera_id[XC_CAMERA_ID_MAX + 1u];
    unsigned int route_waiters;
    bool reconcile_requested;
    bool ai_cancel_requested;
    uint64_t source_generation;

    enum xc_auth_state auth_state;
    enum xc_auth_reason auth_reason;
    struct xc_xiaomi_account account;
    bool has_account;
    char sms_target[96];
    unsigned int sms_code_length;
    uint64_t rate_deadline_monotonic_ms;
    struct xc_xiaomi_error auth_error;
    bool has_auth_error;
    char operation_cache[XC_OPERATION_CACHE_MAX][XC_OPERATION_ID_MAX + 1u];
    size_t operation_cache_count;
    size_t operation_cache_next;

    struct xc_xiaomi_camera *cameras;
    size_t camera_count;
    enum xc_catalog_state catalog_state;
    enum xc_region region;
    enum xc_region catalog_region;
    enum xc_region pending_region;
    bool has_pending_region;
    bool has_queued_region;
    enum xc_region queued_region;
    char queued_region_operation_id[XC_OPERATION_ID_MAX + 1u];
    bool selection_configured;
    char selection_account_id[XC_ACCOUNT_ID_MAX + 1u];
    struct xc_xiaomi_error list_error;
    bool has_list_error;

    enum xc_media_state camera_state;
    char camera_codec[8];
    char selected_id[XC_CAMERA_ID_MAX + 1u];
    char selected_name[128];
    char selected_model[128];
    unsigned int selected_channel;
    bool selection_enabled;
    bool input_available;
    uint64_t input_generation;
    struct xc_xiaomi_error camera_error;
    bool has_camera_error;
};

static const char *xc_work_kind_name(enum xc_work_kind kind)
{
    switch (kind) {
    case XC_WORK_REFRESH: return "refresh";
    case XC_WORK_SMS_START: return "sms_start";
    case XC_WORK_SMS_VERIFY: return "sms_verify";
    case XC_WORK_SMS_RESEND: return "sms_resend";
    case XC_WORK_CANCEL: return "cancel";
    case XC_WORK_CLEAR: return "clear";
    case XC_WORK_CAMERA_LIST: return "camera_list";
    case XC_WORK_CAMERA_REGION: return "camera_region";
    case XC_WORK_CAMERA_SELECT: return "camera_select";
    case XC_WORK_CAMERA_STOP: return "camera_stop";
    case XC_WORK_RECONCILE: return "reconcile";
    case XC_WORK_RTSP: return "rtsp";
    case XC_WORK_NONE:
    default: return "none";
    }
}

const char *xc_auth_state_text(enum xc_auth_state state)
{
    static const char *const names[] = {
        "idle", "working", "sms_required", "authenticated",
        "reauth_required", "error", "retrying"
    };

    return state >= XC_AUTH_IDLE && state <= XC_AUTH_RETRYING ? names[state] : NULL;
}

const char *xc_auth_reason_text(enum xc_auth_reason reason)
{
    static const char *const names[] = {
        "none", "sms_start", "sms_verify", "cloud_session_rejected",
        "pass_token_rejected", "auth_clear"
    };

    return reason >= XC_AUTH_REASON_NONE && reason <= XC_AUTH_REASON_AUTH_CLEAR ?
           names[reason] : NULL;
}

const char *xc_catalog_state_text(enum xc_catalog_state state)
{
    static const char *const names[] = {
        "idle", "queued", "loading", "ready", "error", "retrying"
    };

    return state >= XC_CATALOG_IDLE && state <= XC_CATALOG_RETRYING ? names[state] : NULL;
}

const char *xc_media_state_text(enum xc_media_state state)
{
    static const char *const names[] = {
        "stopped", "starting", "running", "waiting_input", "error"
    };

    return state >= XC_MEDIA_STOPPED && state <= XC_MEDIA_ERROR ? names[state] : NULL;
}


static void xc_secure_clear(void *memory, size_t size)
{
    volatile unsigned char *bytes = memory;

    while (size-- > 0u) *bytes++ = 0u;
}

static void xc_secure_free(char *value)
{
    if (!value) return;
    xc_secure_clear(value, strlen(value));
    free(value);
}

static bool xc_text(const char *value, size_t maximum)
{
    return value && value[0] && strnlen(value, maximum + 1u) <= maximum;
}

static bool xc_decimal_text(const char *value, size_t maximum)
{
    size_t index;

    if (!xc_text(value, maximum)) return false;
    for (index = 0u; value[index]; ++index)
        if (value[index] < '0' || value[index] > '9') return false;
    return true;
}

static bool xc_operation_id(const char *value)
{
    size_t index;

    if (!xc_text(value, XC_OPERATION_ID_MAX) || strlen(value) < 8u) return false;
    for (index = 0u; value[index]; ++index) {
        unsigned char ch = (unsigned char)value[index];

        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.')
            continue;
        return false;
    }
    return true;
}

static bool xc_operation_cached_locked(const struct xc_service *service,
                                       const char *operation_id)
{
    size_t index;

    for (index = 0u; index < service->operation_cache_count; ++index)
        if (!strcmp(service->operation_cache[index], operation_id)) return true;
    return false;
}

static void xc_operation_record_locked(struct xc_service *service,
                                       const char *operation_id)
{
    size_t index = service->operation_cache_next;

    snprintf(service->operation_cache[index], sizeof(service->operation_cache[index]),
             "%s", operation_id);
    if (service->operation_cache_count < XC_OPERATION_CACHE_MAX)
        service->operation_cache_count++;
    service->operation_cache_next =
        (service->operation_cache_next + 1u) % XC_OPERATION_CACHE_MAX;
}

static const char *xc_json_string(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);

    return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

static bool xc_only_keys(const cJSON *object, const char *const *keys, size_t key_count)
{
    const cJSON *item;

    if (!cJSON_IsObject(object)) return false;
    for (item = object->child; item; item = item->next) {
        size_t index;

        for (index = 0u; index < key_count; ++index)
            if (item->string && strcmp(item->string, keys[index]) == 0) break;
        if (index == key_count) return false;
    }
    return true;
}

static cJSON *xc_route_error(int code, const char *reason, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *error = cJSON_CreateObject();
    cJSON *details = cJSON_CreateObject();

    if (!root || !error || !details ||
        !cJSON_AddNumberToObject(error, "code", code) ||
        !cJSON_AddStringToObject(error, "message", message) ||
        !cJSON_AddStringToObject(details, "reason", reason)) goto fail;
    if (!cJSON_AddItemToObject(error, "details", details)) goto fail;
    details = NULL;
    if (!cJSON_AddItemToObject(root, "error", error)) goto fail;
    error = NULL;
    return root;
fail:
    cJSON_Delete(details);
    cJSON_Delete(error);
    cJSON_Delete(root);
    return NULL;
}

static cJSON *xc_public_error(const struct xc_xiaomi_error *error)
{
    cJSON *root = cJSON_CreateObject();

    if (!root ||
        !cJSON_AddStringToObject(root, "category",
                                 error->category[0] ? error->category : "internal") ||
        !cJSON_AddStringToObject(root, "message_key",
                                 error->message_key[0] ? error->message_key :
                                 "operation_failed") ||
        !cJSON_AddNumberToObject(root, "request_stage_code", error->request_stage_code) ||
        !cJSON_AddNumberToObject(root, "network_reason_code", error->network_reason_code) ||
        (error->has_provider_code &&
         !cJSON_AddNumberToObject(root, "provider_code", error->provider_code)) ||
        (error->http_status > 0 &&
         !cJSON_AddNumberToObject(root, "http_status", error->http_status))) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static cJSON *xc_operation(const char *operation_id, const char *state,
                           const struct xc_xiaomi_error *error)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *operation = cJSON_CreateObject();

    if (!root || !operation ||
        !cJSON_AddStringToObject(operation, "id", operation_id) ||
        !cJSON_AddStringToObject(operation, "state", state) ||
        !cJSON_AddItemToObject(root, "operation", operation)) {
        cJSON_Delete(operation);
        cJSON_Delete(root);
        return NULL;
    }
    if (error) {
        cJSON *projected = xc_public_error(error);

        if (!projected || !cJSON_AddItemToObject(root, "last_error", projected)) {
            cJSON_Delete(projected);
            cJSON_Delete(root);
            return NULL;
        }
    }
    return root;
}

static void xc_set_error(struct xc_xiaomi_error *error, const char *category,
                         const char *key, int status)
{
    memset(error, 0, sizeof(*error));
    error->kind = !strcmp(category, "network") ? XC_XIAOMI_ERROR_NETWORK :
                  !strcmp(category, "protocol") ? XC_XIAOMI_ERROR_PROTOCOL :
                  !strcmp(category, "authorization") ?
                  XC_XIAOMI_ERROR_CREDENTIAL_REJECTED :
                  XC_XIAOMI_ERROR_PROVIDER;
    snprintf(error->category, sizeof(error->category), "%.47s", category);
    snprintf(error->message_key, sizeof(error->message_key), "%.63s", key);
    error->http_status = status;
}

static bool xc_reauth_required(const struct xc_xiaomi_error *error)
{
    return error && error->http_status == 401 &&
           error->kind == XC_XIAOMI_ERROR_CREDENTIAL_REJECTED &&
           error->has_reason_code &&
           error->reason_code == XC_AUTH_REASON_PASS_TOKEN_REJECTED &&
           strcmp(error->message_key, "xiaomi_reauthorization_required") == 0;
}

static uint64_t xc_default_clock(clockid_t clock_id)
{
    struct timespec now;

    if (clock_gettime(clock_id, &now) != 0 || now.tv_sec < 0) return 0u;
    if ((uint64_t)now.tv_sec > UINT64_MAX / 1000u) return UINT64_MAX;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static uint64_t xc_monotonic_ms(struct xc_service *service)
{
    return service->ops.monotonic_ms ? service->ops.monotonic_ms(service->ops.userdata) :
           xc_default_clock(CLOCK_MONOTONIC);
}

static uint64_t xc_wallclock_ms(struct xc_service *service)
{
    return service->ops.wallclock_ms ? service->ops.wallclock_ms(service->ops.userdata) :
           xc_default_clock(CLOCK_REALTIME);
}

static bool xc_catalog_work(enum xc_work_kind kind)
{
    return kind == XC_WORK_CAMERA_LIST || kind == XC_WORK_CAMERA_REGION;
}

static bool xc_catalog_incomplete(const struct xc_xiaomi_error *error)
{
    return error && error->kind == XC_XIAOMI_ERROR_PROTOCOL &&
           strcmp(error->category, "protocol") == 0 &&
           strcmp(error->message_key, "catalog_incomplete") == 0;
}

static void xc_recovery_cancel_locked(struct xc_service *service)
{
    if (service->auth_state == XC_AUTH_RETRYING)
        service->auth_state = service->has_account ? XC_AUTH_AUTHENTICATED : XC_AUTH_IDLE;
    if (service->catalog_state == XC_CATALOG_RETRYING)
        service->catalog_state = service->has_list_error ? XC_CATALOG_ERROR :
                                 service->camera_count ? XC_CATALOG_READY : XC_CATALOG_IDLE;
    service->has_pending_region = false;
    xc_secure_clear(&service->recovery, sizeof(service->recovery));
}

static void xc_recovery_generation_advance_locked(struct xc_service *service)
{
    xc_recovery_cancel_locked(service);
    service->recovery_generation++;
    if (service->recovery_generation == 0u) service->recovery_generation = 1u;
}

static bool xc_recovery_current_locked(
    const struct xc_service *service, const struct xc_work *work)
{
    if (!work || service->stopping ||
        work->recovery_generation != service->recovery_generation) return false;
    if (work->kind == XC_WORK_REFRESH) return true;
    if (!xc_catalog_work(work->kind) || !service->has_account || !service->account.id ||
        strcmp(service->account.id, work->account_id) != 0) return false;
    if (work->kind == XC_WORK_CAMERA_LIST && service->region != work->region) return false;
    if (work->kind == XC_WORK_CAMERA_REGION && !work->background &&
        work->input_generation != service->input_generation) return false;
    return true;
}

enum xc_recovery_result { XC_RECOVERY_NONE = 0, XC_RECOVERY_SCHEDULED, XC_RECOVERY_EXHAUSTED };

static enum xc_recovery_result xc_recovery_schedule_locked(
    struct xc_service *service, const struct xc_work *work,
    const struct xc_xiaomi_error *error)
{
    static const unsigned int network_delays[] = {1000u, 3000u, 10000u, 30000u, 60000u};
    struct xc_work next = *work;
    enum xc_recovery_policy policy;
    unsigned int delay;
    uint64_t now;
    if (!xc_recovery_current_locked(service, work)) return XC_RECOVERY_NONE;
    if (error->kind == XC_XIAOMI_ERROR_NETWORK) {
        policy = XC_RECOVERY_NETWORK;
        delay = network_delays[next.network_retries];
        if (next.network_retries < 4u) next.network_retries++;
    } else if (xc_catalog_work(work->kind) && xc_catalog_incomplete(error)) {
        if (next.incomplete_retries >= XC_CATALOG_RETRY_LIMIT) return XC_RECOVERY_EXHAUSTED;
        policy = XC_RECOVERY_INCOMPLETE;
        delay = xc_catalog_retry_delays_ms[next.incomplete_retries++];
    } else return XC_RECOVERY_NONE;
    if (next.retry_attempt < UINT_MAX) next.retry_attempt++;
    next.background = true; /* Recovery cannot exercise an old user input-patch intent. */
    now = xc_monotonic_ms(service);
    service->recovery = (struct xc_recovery){
        .policy=policy, .scheduled=true, .due_ms=now > UINT64_MAX-delay ? UINT64_MAX : now+delay,
        .attempt=next.retry_attempt, .delay_ms=delay, .work=next,
    };
    pthread_cond_signal(&service->work_cond);
    return XC_RECOVERY_SCHEDULED;
}

static void xc_timespec_add_ms(struct timespec *deadline, uint64_t delay_ms)
{
    deadline->tv_sec += (time_t)(delay_ms / 1000u);
    deadline->tv_nsec += (long)(delay_ms % 1000u) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

static bool xc_recovery_promote_locked(struct xc_service *service)
{
    uint64_t now;
    struct timespec deadline;

    if (!service->recovery.scheduled) return false;
    if (!xc_recovery_current_locked(service,
                                         &service->recovery.work)) {
        xc_recovery_cancel_locked(service);
        return false;
    }
    now = xc_monotonic_ms(service);
    if (now < service->recovery.due_ms) {
        if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
            (void)pthread_cond_wait(&service->work_cond, &service->lock);
            return false;
        }
        xc_timespec_add_ms(&deadline, service->recovery.due_ms - now);
        (void)pthread_cond_timedwait(&service->work_cond, &service->lock,
                                     &deadline);
        return false;
    }
    service->work = service->recovery.work;
    service->recovery.scheduled = false;
    service->recovery.due_ms = 0u;
    service->pending = true;
    if (service->work.kind == XC_WORK_REFRESH) service->auth_state = XC_AUTH_RETRYING;
    else service->catalog_state = XC_CATALOG_RETRYING;
    if (service->work.kind == XC_WORK_CAMERA_REGION) {
        service->pending_region = service->work.region;
        service->has_pending_region = true;
    }
    return true;
}

static unsigned int xc_rate_remaining_seconds_locked(struct xc_service *service)
{
    uint64_t now = xc_monotonic_ms(service);
    uint64_t remaining;

    if (service->rate_deadline_monotonic_ms <= now) return 0u;
    remaining = service->rate_deadline_monotonic_ms - now;
    return remaining > (uint64_t)UINT_MAX * 1000u ? UINT_MAX :
           (unsigned int)((remaining + 999u) / 1000u);
}

static bool xc_rate_active_locked(struct xc_service *service)
{
    return xc_rate_remaining_seconds_locked(service) != 0u;
}

static int xc_set_rate_deadline(struct xc_service *service,
                                unsigned int retry_after_seconds,
                                struct xc_xiaomi_error *error)
{
    uint64_t now_wall = xc_wallclock_ms(service);
    uint64_t now_monotonic = xc_monotonic_ms(service);
    uint64_t wait_ms;
    uint64_t deadline_wall;

    if (now_wall > UINT64_C(9007199254740991) -
                   (uint64_t)retry_after_seconds * 1000u ||
        now_monotonic > UINT64_MAX - (uint64_t)retry_after_seconds * 1000u) {
        xc_set_error(error, "protocol", "xiaomi_response_invalid", 0);
        return -1;
    }
    wait_ms = (uint64_t)retry_after_seconds * 1000u;
    deadline_wall = now_wall + wait_ms;
    pthread_mutex_lock(&service->lock);
    service->rate_deadline_monotonic_ms = now_monotonic + wait_ms;
    pthread_mutex_unlock(&service->lock);
    if (!service->ops.phone_rate_save ||
        !xc_store_result_committed(
            service->ops.phone_rate_save(service->ops.userdata, deadline_wall))) {
        xc_set_error(error, "internal", "phone_rate_limit_persist_failed", 0);
        return -1;
    }
    return 0;
}

static void xc_accounts_clear(struct xc_service *service)
{
    xc_secure_free(service->account.id);
    xc_secure_free(service->account.label);
    memset(&service->account, 0, sizeof(service->account));
    service->has_account = false;
}

static void xc_challenge_clear(struct xc_service *service)
{
    xc_secure_clear(service->sms_target, sizeof(service->sms_target));
    service->sms_code_length = 0u;
}

static void xc_camera_array_clear(struct xc_xiaomi_camera *cameras, size_t count)
{
    size_t index;

    for (index = 0u; index < count; ++index) {
        xc_secure_free(cameras[index].id);
        xc_secure_free(cameras[index].name);
        xc_secure_free(cameras[index].model);
        xc_secure_free(cameras[index].info);
        xc_secure_free(cameras[index].source);
        xc_secure_free(cameras[index].home_id);
        xc_secure_free(cameras[index].home_name);
        xc_secure_free(cameras[index].room_id);
        xc_secure_free(cameras[index].room_name);
    }
    free(cameras);
}

static void xc_cameras_clear(struct xc_service *service)
{
    xc_camera_array_clear(service->cameras, service->camera_count);
    service->cameras = NULL;
    service->camera_count = 0u;
}

static char *xc_staging_strdup(struct xc_service *service, const char *source)
{
    size_t size = strlen(source) + 1u;
    char *copy = service->ops.catalog_alloc(service->ops.userdata, size);

    if (!copy) {
        errno = ENOMEM;
        return NULL;
    }
    memcpy(copy, source, size);
    return copy;
}

static int xc_accounts_apply_locked(struct xc_service *service,
                                    const struct xc_xiaomi_response *response,
                                    struct xc_xiaomi_error *error)
{
    char *id;
    char *label;

    if (response->account_count > 1u) {
        xc_set_error(error, "configuration", "multiple_accounts_configured", 0);
        return -1;
    }
    if (response->account_count == 0u) {
        if (service->has_account)
            xc_recovery_generation_advance_locked(service);
        xc_accounts_clear(service);
        xc_challenge_clear(service);
        service->auth_state = XC_AUTH_IDLE;
        service->auth_reason = XC_AUTH_REASON_NONE;
        return 0;
    }
    if (!response->accounts ||
        !xc_decimal_text(response->accounts[0].id, XC_ACCOUNT_ID_MAX) ||
        !xc_text(response->accounts[0].label, XC_ACCOUNT_ID_MAX)) {
        xc_set_error(error, "protocol", "xiaomi_response_invalid", 0);
        return -1;
    }
    id = xc_staging_strdup(service, response->accounts[0].id);
    label = xc_staging_strdup(service, response->accounts[0].label);
    if (!id || !label) {
        xc_secure_free(id);
        xc_secure_free(label);
        xc_set_error(error, "internal", "out_of_memory", 0);
        return -1;
    }
    if (service->has_account && strcmp(service->account.id, id) != 0)
        xc_recovery_generation_advance_locked(service);
    xc_accounts_clear(service);
    xc_challenge_clear(service);
    service->account.id = id;
    service->account.label = label;
    service->has_account = true;
    service->auth_state = XC_AUTH_AUTHENTICATED;
    service->auth_reason = XC_AUTH_REASON_NONE;
    return 0;
}

static int xc_apply_auth_response_locked(struct xc_service *service,
                                         const struct xc_xiaomi_response *response,
                                         struct xc_xiaomi_error *error)
{
    if (response->error.category[0]) {
        *error = response->error;
        return -1;
    }
    if (!strcmp(response->phone_state, "error")) {
        const char *category = "provider";

        if (!strcmp(response->phone_error, "sms_send_limit_tomorrow") ||
            !strcmp(response->phone_error, "rate_limit"))
            category = "rate_limit";
        else if (!strcmp(response->phone_error, "input") ||
                 !strcmp(response->phone_error, "phone_account_not_found"))
            category = "input";
        else if (!strcmp(response->phone_error, "network") ||
                 !strcmp(response->phone_error, "protocol") ||
                 !strcmp(response->phone_error, "internal") ||
                 !strcmp(response->phone_error, "additional_verification_required"))
            category = response->phone_error;
        else if (!strcmp(response->phone_error, "phone_info_response_invalid") ||
                 !strcmp(response->phone_error, "phone_info_ticket_token_missing") ||
                 !strcmp(response->phone_error, "ticket_auth_json_invalid") ||
                 !strcmp(response->phone_error, "ticket_auth_code_missing") ||
                 !strcmp(response->phone_error, "ticket_auth_location_missing") ||
                 !strcmp(response->phone_error, "ticket_auth_location_invalid") ||
                 !strcmp(response->phone_error, "passport_location_rejected") ||
                 !strcmp(response->phone_error, "passport_token_missing") ||
                 !strcmp(response->phone_error, "xiaomi_token_response_invalid"))
            category = "protocol";
        xc_set_error(error, category, response->phone_error, response->http_status);
        if (response->has_phone_provider_code) {
            error->provider_code = response->phone_provider_code;
            error->has_provider_code = true;
        }
        return -1;
    }
    if (!strcmp(response->phone_state, "sms_required")) {
        xc_challenge_clear(service);
        snprintf(service->sms_target, sizeof(service->sms_target), "%s",
                 response->phone_masked_target);
        service->sms_code_length = response->phone_code_length;
        service->auth_state = XC_AUTH_SMS_REQUIRED;
        return 0;
    }
    if (!strcmp(response->phone_state, "authenticated")) {
        xc_challenge_clear(service);
        return 0;
    }
    xc_set_error(error, "protocol", "xiaomi_response_invalid", 0);
    return -1;
}

static int xc_cameras_prepare(struct xc_service *service,
                              const struct xc_xiaomi_response *response,
                              struct xc_xiaomi_camera **cameras_out)
{
    struct xc_xiaomi_camera *cameras = NULL;
    size_t index;

    if (!cameras_out) {
        errno = EINVAL;
        return -1;
    }
    if (response->camera_count > 256u) {
        errno = EOVERFLOW;
        return -1;
    }
    if (response->camera_count && !response->cameras) {
        errno = EPROTO;
        return -1;
    }
    if (response->camera_count) {
        cameras = service->ops.catalog_alloc(
            service->ops.userdata, response->camera_count * sizeof(*cameras));
        if (!cameras) {
            errno = ENOMEM;
            return -1;
        }
        memset(cameras, 0, response->camera_count * sizeof(*cameras));
        for (index = 0u; index < response->camera_count; ++index) {
            const struct xc_xiaomi_camera *source = &response->cameras[index];

            if (!source->id || !source->name || !source->model ||
                !source->info || !source->source || !source->home_id ||
                !source->home_name || !source->room_id || !source->room_name) {
                errno = EPROTO;
                goto fail;
            }
            cameras[index].id = xc_staging_strdup(service, source->id);
            if (!cameras[index].id) goto fail;
            cameras[index].name = xc_staging_strdup(service, source->name);
            if (!cameras[index].name) goto fail;
            cameras[index].model = xc_staging_strdup(service, source->model);
            if (!cameras[index].model) goto fail;
            cameras[index].info = xc_staging_strdup(service, source->info);
            if (!cameras[index].info) goto fail;
            cameras[index].source = xc_staging_strdup(service, source->source);
            if (!cameras[index].source) goto fail;
            cameras[index].home_id = xc_staging_strdup(service, source->home_id);
            if (!cameras[index].home_id) goto fail;
            cameras[index].home_name = xc_staging_strdup(service, source->home_name);
            if (!cameras[index].home_name) goto fail;
            cameras[index].room_id = xc_staging_strdup(service, source->room_id);
            if (!cameras[index].room_id) goto fail;
            cameras[index].room_name = xc_staging_strdup(service, source->room_name);
            if (!cameras[index].room_name) goto fail;
        }
    }
    *cameras_out = cameras;
    return 0;
fail:
    {
        int saved = errno;

        xc_camera_array_clear(cameras, response->camera_count);
        errno = saved;
    }
    return -1;
}

static void xc_set_catalog_prepare_error(struct xc_xiaomi_error *error)
{
    if (errno == ENOMEM)
        xc_set_error(error, "internal", "out_of_memory", 0);
    else {
        assert(errno == EPROTO || errno == EOVERFLOW);
        xc_set_error(error, "protocol", "xiaomi_response_invalid", 0);
    }
}

static void xc_cameras_commit(struct xc_service *service,
                              struct xc_xiaomi_camera *cameras,
                              size_t camera_count)
{
    xc_cameras_clear(service);
    service->cameras = cameras;
    service->camera_count = camera_count;
}

static int xc_cameras_apply(struct xc_service *service,
                            const struct xc_xiaomi_response *response,
                            struct xc_xiaomi_error *error)
{
    struct xc_xiaomi_camera *cameras = NULL;

    if (xc_cameras_prepare(service, response, &cameras) != 0) {
        xc_set_catalog_prepare_error(error);
        return -1;
    }
    xc_cameras_commit(service, cameras, response->camera_count);
    return 0;
}

static bool xc_add_recovery_locked(struct xc_service *service, cJSON *object, bool auth)
{
    bool retrying = auth ? service->auth_state == XC_AUTH_RETRYING :
                          service->catalog_state == XC_CATALOG_RETRYING;
    cJSON *retry;
    if (!retrying) return cJSON_AddNullToObject(object, "retry") != NULL;
    retry = cJSON_AddObjectToObject(object, "retry");
    return retry && cJSON_AddNumberToObject(retry, "attempt", service->recovery.attempt) &&
           cJSON_AddNumberToObject(retry, "delay_ms", service->recovery.delay_ms);
}

static bool xc_add_last_error(cJSON *object, bool present, const struct xc_xiaomi_error *error)
{
    cJSON *value = present ? xc_public_error(error) : cJSON_CreateNull();
    if (value && cJSON_AddItemToObject(object, "last_error", value)) return true;
    cJSON_Delete(value);
    return false;
}

static void xc_persist_status(struct xc_service *service)
{
    cJSON *root;
    cJSON *auth;
    cJSON *camera;
    cJSON *catalog;
    char *text;
    const char *auth_text;
    const char *reason_text;
    const char *media_text;
    const char *catalog_text;

    if (!service->ops.persist_status) return;
    pthread_mutex_lock(&service->lock);
    root = cJSON_CreateObject();
    auth = cJSON_CreateObject();
    camera = cJSON_CreateObject();
    catalog = cJSON_CreateObject();
    auth_text = xc_auth_state_text(service->auth_state);
    reason_text = xc_auth_reason_text(service->auth_reason);
    media_text = xc_media_state_text(service->camera_state);
    catalog_text = xc_catalog_state_text(service->catalog_state);
    if (!root || !auth || !camera || !catalog || !auth_text || !reason_text ||
        !media_text || !catalog_text ||
        !cJSON_AddNumberToObject(auth, "state_code", service->auth_state) ||
        !cJSON_AddStringToObject(auth, "state_text", auth_text) ||
        !cJSON_AddNumberToObject(auth, "reason_code", service->auth_reason) ||
        !cJSON_AddStringToObject(auth, "reason_text", reason_text) ||
        !cJSON_AddNumberToObject(camera, "state_code", service->camera_state) ||
        !cJSON_AddStringToObject(camera, "state_text", media_text) ||
        !cJSON_AddNumberToObject(catalog, "state_code", service->catalog_state) ||
        !cJSON_AddStringToObject(catalog, "state_text", catalog_text) ||
        !xc_add_recovery_locked(service, auth, true) ||
        !xc_add_recovery_locked(service, catalog, false) ||
        !xc_add_last_error(auth, service->has_auth_error, &service->auth_error) ||
        !xc_add_last_error(catalog, service->has_list_error, &service->list_error) ||
        !xc_add_last_error(camera, service->has_camera_error, &service->camera_error)) {
        cJSON_Delete(auth); cJSON_Delete(camera); cJSON_Delete(catalog);
        cJSON_Delete(root);
        pthread_mutex_unlock(&service->lock);
        return;
    }
    if (!cJSON_AddItemToObject(root, "auth", auth)) goto attach_fail;
    auth = NULL;
    if (!cJSON_AddItemToObject(root, "camera_list", catalog)) goto attach_fail;
    catalog = NULL;
    if (!cJSON_AddItemToObject(root, "camera", camera)) goto attach_fail;
    camera = NULL;
    text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    pthread_mutex_unlock(&service->lock);
    if (text) {
        if (service->ops.persist_status(service->ops.userdata, text) != 0)
            fprintf(stderr, "mhcamera: event=status_persist result=failed\n");
        xc_secure_clear(text, strlen(text));
        cJSON_free(text);
    }
    return;

attach_fail:
    cJSON_Delete(auth);
    cJSON_Delete(camera);
    cJSON_Delete(catalog);
    cJSON_Delete(root);
    pthread_mutex_unlock(&service->lock);
}

static int xc_reconcile(struct xc_service *service);
static void xc_rtsp_reconnect_error(struct xc_service *service, enum xc_rtsp_error error);

struct xc_cloud_task {
    struct xc_service *service;
    const struct xc_xiaomi_request *request;
    struct xc_xiaomi_response *response;
    int result;
    int error_number;
    bool done;
};

static void *xc_cloud_call(void *userdata)
{
    struct xc_cloud_task *task = userdata;
    int result = task->service->ops.xiaomi_call(task->service->ops.userdata, task->request, task->response);
    int error_number = errno;
    pthread_mutex_lock(&task->service->lock);
    task->result = result;
    task->error_number = error_number;
    task->done = true;
    pthread_cond_broadcast(&task->service->work_cond);
    pthread_mutex_unlock(&task->service->lock);
    return NULL;
}

/* Exactly one bounded cloud transaction can be in flight. Its thread owns
 * only the response; this worker remains the sole lifecycle owner while
 * waiting and can release AI/P2P without waiting for a metadata deadline. */
static int xc_call(struct xc_service *service, struct xc_xiaomi_request *request,
                   struct xc_xiaomi_response *response, struct xc_xiaomi_error *error)
{
    struct xc_cloud_task task = {.service=service, .request=request, .response=response};
    pthread_t thread;
    bool stopped = false, cancelled;
    int created = pthread_create(&thread, NULL, xc_cloud_call, &task);
    if (created != 0) {
        xc_set_error(error, "internal", "out_of_memory", 0);
        errno = created;
        return -1;
    }
    pthread_mutex_lock(&service->lock);
    while (!task.done) {
        if (service->reconcile_requested || (service->stopping && !stopped)) {
            service->reconcile_requested = false;
            stopped = service->stopping;
            pthread_mutex_unlock(&service->lock);
            if (xc_reconcile(service) != 0)
                xc_rtsp_reconnect_error(service, XC_RTSP_ERROR_RECONNECT);
            pthread_mutex_lock(&service->lock);
        } else pthread_cond_wait(&service->work_cond, &service->lock);
    }
    cancelled = service->stopping;
    pthread_mutex_unlock(&service->lock);
    pthread_join(thread, NULL);
    if (cancelled) {
        xc_set_error(error, "conflict", "operation_cancelled", 0);
        errno = ECANCELED;
        return -1;
    }
    if (task.result != 0) {
        errno = task.error_number;
        if (errno == EPROTO || errno == EOVERFLOW || errno == EMSGSIZE)
            xc_set_error(error, "protocol", "xiaomi_response_invalid", 0);
        else if (errno == ENOMEM)
            xc_set_error(error, "internal", "out_of_memory", 0);
        else {
            xc_set_error(error, "network", "xiaomi_api_unavailable", 0);
            error->request_stage_code = XC_XIAOMI_STAGE_LOCAL_RPC;
            error->network_reason_code = xc_xiaomi_network_reason_from_errno(task.error_number);
        }
        return -1;
    }
    if (response->error.category[0]) { *error = response->error; return -1; }
    return 0;
}

static void xc_queue_recover_locked(struct xc_service *service)
{
    if (!service->reconcile_requested || service->pending || service->active || service->stopping)
        return;
    /* Startup observes host input before queueing the account refresh. There
     * is no lifecycle to reconcile yet; do not occupy that initial work slot. */
    if (!service->has_account && !service->active_source[0]) return;
    memset(&service->work, 0, sizeof(service->work));
    service->work.kind = XC_WORK_RECONCILE;
    service->reconcile_requested = false;
    service->pending = true;
    pthread_cond_signal(&service->work_cond);
}

static void xc_queue_catalog_locked(struct xc_service *service,
                                    enum xc_work_kind kind,
                                    enum xc_region region)
{
    if (!service->has_account || service->pending || service->active ||
        service->stopping ||
        (kind != XC_WORK_CAMERA_LIST && kind != XC_WORK_CAMERA_REGION))
        return;
    xc_recovery_generation_advance_locked(service);
    memset(&service->work, 0, sizeof(service->work));
    service->work.kind = kind;
    service->work.region = region;
    snprintf(service->work.account_id, sizeof(service->work.account_id), "%s",
             service->account.id);
    service->work.recovery_generation = service->recovery_generation;
    if (kind == XC_WORK_CAMERA_REGION)
        service->work.input_generation = service->input_generation;
    service->pending = true;
    service->catalog_state = XC_CATALOG_QUEUED;
    if (kind == XC_WORK_CAMERA_REGION) {
        service->pending_region = region;
        service->has_pending_region = true;
    }
    pthread_cond_signal(&service->work_cond);
}

static void xc_queue_latest_region_locked(struct xc_service *service)
{
    enum xc_region region;
    char operation_id[XC_OPERATION_ID_MAX + 1u];

    if (!service->has_queued_region) return;
    region = service->queued_region;
    snprintf(operation_id, sizeof(operation_id), "%s",
             service->queued_region_operation_id);
    service->has_queued_region = false;
    service->queued_region_operation_id[0] = '\0';
    if (service->stopping || !service->has_account ||
        service->auth_state != XC_AUTH_AUTHENTICATED)
        return;
    xc_queue_catalog_locked(service,
                            region == service->region ? XC_WORK_CAMERA_LIST :
                            XC_WORK_CAMERA_REGION,
                            region);
    if (service->pending)
        snprintf(service->work.operation_id,
                 sizeof(service->work.operation_id), "%s", operation_id);
}

static bool xc_input_generation_current(struct xc_service *service,
                                        uint64_t generation)
{
    bool current;

    pthread_mutex_lock(&service->lock);
    current = !service->stopping && service->input_available && service->input_generation == generation;
    pthread_mutex_unlock(&service->lock);
    return current;
}

static uint64_t xc_input_prepare_begin_locked(struct xc_service *service)
{
    service->input_available = true;
    service->input_generation++;
    if (service->input_generation == 0u) service->input_generation = 1u;
    return service->input_generation;
}

static void xc_input_prepare_failed(struct xc_service *service,
                                    uint64_t generation)
{
    pthread_mutex_lock(&service->lock);
    if (service->input_generation == generation) {
        service->input_available = false;
        service->input_generation++;
        if (service->input_generation == 0u) service->input_generation = 1u;
    }
    pthread_mutex_unlock(&service->lock);
}

struct xc_input_prepare_guard {
    struct xc_service *service;
    uint64_t generation;
};

static bool xc_input_patch_allowed(void *userdata)
{
    const struct xc_input_prepare_guard *guard = userdata;

    return guard && xc_input_generation_current(guard->service,
                                                guard->generation);
}

static int xc_input_prepare_guarded(struct xc_service *service,
                                    uint64_t generation,
                                    struct xc_xiaomi_error *error)
{
    struct xc_input_prepare_guard guard = {
        .service = service,
        .generation = generation,
    };
    int saved_errno;

    if (!service->ops.input_prepare) {
        saved_errno = ENOSYS;
    } else {
        errno = 0;
        if (service->ops.input_prepare(service->ops.userdata,
                                       xc_input_patch_allowed, &guard) == 0)
            return 0;
        saved_errno = errno != 0 ? errno : EIO;
    }
    xc_input_prepare_failed(service, generation);
    if (saved_errno == ECANCELED)
        xc_set_error(error, "conflict", "input_mode_changed", 0);
    else
        xc_set_error(error, "internal", "input_mode_switch_failed", 0);
    errno = saved_errno;
    return -1;
}

static void xc_finish_locked(struct xc_service *service, const struct xc_work *work,
                             bool ok, const struct xc_xiaomi_error *error,
                             enum xc_recovery_result recovery_result)
{
    service->pending = false;
    service->active = false;
    service->active_kind = XC_WORK_NONE;
    if (!ok && recovery_result == XC_RECOVERY_SCHEDULED && work->kind == XC_WORK_REFRESH) {
        service->auth_error = *error;
        service->has_auth_error = true;
        service->auth_state = XC_AUTH_RETRYING;
        service->auth_reason = XC_AUTH_REASON_NONE;
    } else if (!ok && xc_reauth_required(error)) {
        service->auth_error = *error;
        service->has_auth_error = true;
        service->auth_state = XC_AUTH_REAUTH_REQUIRED;
        service->auth_reason = XC_AUTH_REASON_PASS_TOKEN_REJECTED;
        xc_challenge_clear(service);
    } else if (!ok && error &&
        (work->kind == XC_WORK_CAMERA_SELECT) &&
        !service->input_available &&
        (!strcmp(error->message_key, "input_mode_changed") ||
         !strcmp(error->message_key, "input_mode_switch_failed"))) {
        service->camera_state = XC_MEDIA_WAITING_INPUT;
        if (!strcmp(error->message_key, "input_mode_switch_failed")) {
            service->camera_error = *error;
            service->has_camera_error = true;
        }
    } else if (!ok && error &&
               recovery_result != XC_RECOVERY_SCHEDULED &&
               work->kind != XC_WORK_CAMERA_LIST &&
               work->kind != XC_WORK_CAMERA_REGION) {
        if (work->kind <= XC_WORK_CLEAR) {
            service->auth_error = *error;
            service->has_auth_error = true;
            service->auth_state = XC_AUTH_ERROR;
        } else {
            service->camera_error = *error;
            service->has_camera_error = true;
            service->camera_state = XC_MEDIA_ERROR;
        }
    }
    xc_queue_latest_region_locked(service);
}

static void xc_ai_stop(struct xc_service *service)
{
    if (!service->media_active) return;
    service->ops.media_stop(service->ops.userdata);
    service->media_active = false;
}

static int xc_source_stop(struct xc_service *service)
{
    int result;
    if (xc_rtsp_source(service->ops.rtsp, false) != 0)
        fprintf(stderr, "mhcamera: event=rtsp_stop result=failed\n");
    xc_ai_stop(service);
    result = service->ops.source_stop(service->ops.userdata);
    if (result == 0) {
        pthread_mutex_lock(&service->lock);
        service->active_source[0] = '\0';
        service->active_core_source[0] = '\0';
        service->active_camera_id[0] = '\0';
        service->source_generation++;
        pthread_mutex_unlock(&service->lock);
    }
    return result;
}

static bool xc_selected_work_locked(struct xc_service *service, struct xc_work *work)
{
    size_t i;
    if (service->stopping || service->auth_state != XC_AUTH_AUTHENTICATED ||
        !service->has_account || !service->selected_id[0] ||
        strcmp(service->selection_account_id, service->account.id) ||
        service->catalog_region != service->region) return false;
    for (i = 0; i < service->camera_count; ++i) {
        const struct xc_xiaomi_camera *camera = &service->cameras[i];
        if (strcmp(camera->id, service->selected_id) || strcmp(camera->model, service->selected_model)) continue;
        snprintf(work->account_id, sizeof(work->account_id), "%s", service->account.id);
        snprintf(work->camera_id, sizeof(work->camera_id), "%s", camera->id);
        snprintf(work->model, sizeof(work->model), "%s", camera->model);
        snprintf(work->core_source, sizeof(work->core_source), "%s", camera->source);
        work->region = service->region;
        work->channel = service->selected_channel;
        return true;
    }
    return false;
}

/* Sole owner of source/AI/listener transitions. Inputs change intent only.
 * Camera/cloud selection and settings all converge here on the same worker.
 * source_generation describes accepted upstream identities, independently of
 * input_generation, which can only authorize an AI attachment or host patch. */
static int xc_reconcile(struct xc_service *service)
{
    struct xc_rtsp_preferences preferences;
    struct xc_work target = {0};
    bool valid, ai, source, changed, had_source, audio, cancelled;
    uint64_t input_generation;
    int result = 0;
    xc_rtsp_preferences(service->ops.rtsp, &preferences);
    pthread_mutex_lock(&service->lock);
    valid = xc_selected_work_locked(service, &target);
    ai = valid && service->selection_enabled && service->input_available;
    source = valid && (ai || preferences.enabled);
    input_generation = service->input_generation;
    cancelled = service->ai_cancel_requested;
    service->ai_cancel_requested = false;
    pthread_mutex_unlock(&service->lock);
    if (cancelled) xc_ai_stop(service);
    audio = preferences.enabled && preferences.audio_enabled;
    if (source && service->active_source[0] &&
        !strcmp(target.core_source, service->active_core_source)) {
        struct xc_rtsp_state observed;
        cJSON *snapshot = xc_rtsp_read(service->ops.rtsp, &observed);
        cJSON_Delete(snapshot);
        if (observed.audio_known && !observed.audio_supported &&
            !strcmp(observed.source, service->active_source))
            audio = strstr(service->active_source, "&audio=1&") != NULL;
    }
    if (source && xc_camera_source_rebuild(target.core_source, target.account_id,
        xc_region_text(target.region), target.camera_id, target.model,
        target.channel == 2u ? 2u : 1u, audio,
        target.source, sizeof(target.source)) != 0) return -1;
    had_source = service->active_source[0] != '\0';
    changed = strcmp(target.source, service->active_source) != 0;
    if (changed && had_source && xc_source_stop(service) != 0) return -1;
    if (source && changed) {
        /* Remember uncertain mutations as well, so a lost reply cannot orphan
         * an accepted Go target outside the next stop/reconcile operation. */
        pthread_mutex_lock(&service->lock);
        snprintf(service->active_source, sizeof(service->active_source), "%s", target.source);
        snprintf(service->active_core_source, sizeof(service->active_core_source), "%s", target.core_source);
        snprintf(service->active_camera_id, sizeof(service->active_camera_id), "%s", target.camera_id);
        service->source_generation++;
        pthread_mutex_unlock(&service->lock);
        if (service->ops.source_set(service->ops.userdata, target.source) != 0) {
            (void)xc_source_stop(service);
            return -1;
        }
    }
    if (source && xc_rtsp_source(service->ops.rtsp, true) != 0) result = -1;
    /* An input event during a local control RPC must not reattach stale AI.
     * It does not revoke the independent RTSP source demand. */
    if (!xc_input_generation_current(service, input_generation)) ai = false;
    if (!ai) xc_ai_stop(service);
    else if (!service->media_active) {
        if (service->ops.media_start(service->ops.userdata) != 0) result = -1;
        else service->media_active = true;
        if (!xc_input_generation_current(service, input_generation)) xc_ai_stop(service);
    }
    pthread_mutex_lock(&service->lock);
    if (!service->media_active) {
        service->camera_state = service->selection_enabled && service->selected_id[0] ?
                                XC_MEDIA_WAITING_INPUT : XC_MEDIA_STOPPED;
        service->camera_codec[0] = '\0';
    }
    if (changed || result)
        fprintf(stderr, "mhcamera: event=source_reconcile generation=%llu ai=%u rtsp=%u source=%u result=%s\n",
            (unsigned long long)service->source_generation, ai, preferences.enabled, source,
            result ? "failed" : "accepted");
    pthread_mutex_unlock(&service->lock);
    xc_secure_clear(&target, sizeof(target));
    return result;
}

/* One sidecar snapshot supplies both listener state and source-bound capability.
 * No service lock is held over control I/O. Revalidate the identity afterwards. */
static cJSON *xc_rtsp_status(struct xc_service *service, bool *known)
{
    struct xc_rtsp_state observed = {0};
    char expected_source[512] = {0};
    bool matches, supported;
    cJSON *result;
    pthread_mutex_lock(&service->lock);
    if (service->auth_state == XC_AUTH_AUTHENTICATED && service->has_account && service->selected_id[0] &&
        strcmp(service->selected_id, service->active_camera_id) == 0 &&
        strcmp(service->account.id, service->selection_account_id) == 0)
        snprintf(expected_source, sizeof(expected_source), "%s", service->active_source);
    pthread_mutex_unlock(&service->lock);
    result = xc_rtsp_read(service->ops.rtsp, &observed);
    pthread_mutex_lock(&service->lock);
    matches = expected_source[0] && service->auth_state == XC_AUTH_AUTHENTICATED && service->has_account &&
              strcmp(service->selected_id, service->active_camera_id) == 0 &&
              strcmp(expected_source, service->active_source) == 0 &&
              observed.audio_known && strcmp(expected_source, observed.source) == 0;
    pthread_mutex_unlock(&service->lock);
    supported = matches && observed.audio_supported;
    if (known) *known = matches;
    if (result && !cJSON_AddBoolToObject(result, "audio_supported", supported)) {
        cJSON_Delete(result);
        result = NULL;
    }
    if (result) {
        cJSON *error = cJSON_GetObjectItemCaseSensitive(result, "error");
        if (!cJSON_AddStringToObject(result, "source_state", xc_source_state_text(observed.source_state)) ||
            ((observed.source_error != XC_SOURCE_ERROR_NONE) ? !cJSON_AddStringToObject(result, "source_error", xc_source_error_text(observed.source_error)) :
             !cJSON_AddNullToObject(result, "source_error"))) {
            cJSON_Delete(result); result = NULL;
        } else if ((observed.source_error != XC_SOURCE_ERROR_NONE) &&
                   cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "enabled")) && cJSON_IsNull(error)) {
            cJSON *failed = cJSON_CreateString("error");
            if (!failed || !cJSON_ReplaceItemInObjectCaseSensitive(result, "state", failed)) {
                cJSON_Delete(failed); cJSON_Delete(result); result = NULL;
                goto done;
            }
            cJSON_DeleteItemFromObjectCaseSensitive(result, "error");
            error = cJSON_AddObjectToObject(result, "error");
            if (!error || !cJSON_AddStringToObject(error, "message_key", "rtsp_source_failed")) {
                cJSON_Delete(result); result = NULL;
            }
        }
    }
done:
    xc_secure_clear(expected_source, sizeof(expected_source));
    return result;
}

static void xc_rtsp_reconnect_error(struct xc_service *service, enum xc_rtsp_error error)
{
    const char *reason = xc_rtsp_error_text(error);
    xc_rtsp_error(service->ops.rtsp, error);
    pthread_mutex_lock(&service->lock);
    service->camera_state = service->input_available ? XC_MEDIA_ERROR : XC_MEDIA_WAITING_INPUT;
    xc_set_error(&service->camera_error, "media", reason, 0);
    service->has_camera_error = true;
    pthread_mutex_unlock(&service->lock);
}

/* Runs only on the service worker. No RTSP lock is held while invoking media
 * callbacks (media stop joins a thread that can report back into service). */
static cJSON *xc_rtsp_execute(struct xc_service *service, const struct xc_rtsp_job *job)
{
    struct xc_rtsp_preferences before, after;
    cJSON *empty = cJSON_CreateObject(), *body = cJSON_CreateObject(), *response;
    struct xc_route_request request = {
        .path = job->credentials ? "/rtsp/credentials" : "/rtsp",
        .http_method = "POST", .query = empty, .body = body
    };
    bool supported = false, known = false, replace = false, accepted = false;
    bool desired_audio;
    if (!empty || !body) { cJSON_Delete(empty); cJSON_Delete(body); return NULL; }
    if (!job->credentials && !cJSON_AddBoolToObject(body, job->audio_field ? "audio_enabled" : "enabled", job->value)) {
        cJSON_Delete(empty); cJSON_Delete(body); return NULL;
    }
    xc_rtsp_preferences(service->ops.rtsp, &before);
    desired_audio = job->audio_field ? before.enabled && job->value : job->value && before.audio_enabled;
    if (!job->credentials) {
        cJSON *snapshot = xc_rtsp_status(service, &known);
        supported = snapshot && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(snapshot, "audio_supported"));
        cJSON_Delete(snapshot);
    }
    if (job->audio_field && !supported) {
        response = xc_route_error(AINICE_PROTOCOL_ERROR_CONFLICT, "rtsp_audio_unsupported", "audio control is unavailable for this camera");
        goto done;
    }
    replace = !job->credentials && service->active_source[0] && (!known || supported) &&
              (strstr(service->active_source, "&audio=1&") != NULL) != desired_audio;
    /* Release the old audio request before committing a new preference.
     * Failed persistence leaves both branches stopped for this operation. */
    if (replace && xc_source_stop(service) != 0) {
        xc_rtsp_reconnect_error(service, XC_RTSP_ERROR_RECONNECT);
        response = xc_route_error(AINICE_PROTOCOL_ERROR_INTERNAL_ERROR, "rtsp_reconnect_failed", "previous camera source could not be released");
        goto done;
    }
    response = xc_rtsp_route(service->ops.rtsp, &request);
    xc_rtsp_preferences(service->ops.rtsp, &after);
    if (!job->credentials && response &&
        (!cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(response, "error")) ||
         before.enabled != after.enabled || before.audio_enabled != after.audio_enabled)) {
        if (xc_reconcile(service) != 0) {
            xc_rtsp_reconnect_error(service, XC_RTSP_ERROR_RECONNECT);
            if (!cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(response, "error"))) {
                cJSON_Delete(response);
                response = xc_route_error(AINICE_PROTOCOL_ERROR_INTERNAL_ERROR, "rtsp_reconnect_failed", "saved source target could not be accepted");
            }
        } else accepted = replace && service->active_source[0];
    } else if (replace) {
        xc_rtsp_reconnect_error(service, XC_RTSP_ERROR_PERSIST);
    }
    if (!job->credentials && response && !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(response, "error"))) {
        cJSON_Delete(response);
        response = xc_rtsp_status(service, NULL);
        if (response && !cJSON_AddBoolToObject(response, "reconnect_requested", accepted)) {
            cJSON_Delete(response); response = NULL;
        }
    }
done:
    cJSON_Delete(empty);
    cJSON_Delete(body);
    return response;
}

static void *xc_worker(void *userdata)
{
    struct xc_service *service = userdata;

    for (;;) {
        struct xc_work work;
        struct xc_xiaomi_request request = {0};
        struct xc_xiaomi_response response = {0};
        struct xc_xiaomi_error error = {0};
        enum xc_recovery_result recovery_result =
            XC_RECOVERY_NONE;
        bool ok = false;
        bool catalog_loaded = false;
        bool region_committed = false;

        pthread_mutex_lock(&service->lock);
        while (!service->stopping && !service->pending) {
            if (service->recovery.scheduled) {
                (void)xc_recovery_promote_locked(service);
                continue;
            }
            pthread_cond_wait(&service->work_cond, &service->lock);
        }
        if (service->stopping && !service->pending) {
            pthread_mutex_unlock(&service->lock);
            break;
        }
        work = service->work;
        xc_secure_clear(&service->work, sizeof(service->work));
        service->pending = false;
        service->active = true;
        service->active_kind = work.kind;
        if (!work.background && (work.kind == XC_WORK_CAMERA_LIST ||
            work.kind == XC_WORK_CAMERA_REGION))
            service->catalog_state = XC_CATALOG_LOADING;
        pthread_mutex_unlock(&service->lock);

        if (work.kind == XC_WORK_RTSP) {
            cJSON *reply = xc_rtsp_execute(service, work.rtsp_job);
            pthread_mutex_lock(&service->lock);
            xc_finish_locked(service, &work, true, NULL, XC_RECOVERY_NONE);
            xc_queue_recover_locked(service);
            work.rtsp_job->response = reply;
            work.rtsp_job->done = true;
            pthread_cond_broadcast(&service->work_cond);
            pthread_mutex_unlock(&service->lock);
            xc_persist_status(service);
            xc_secure_clear(&work, sizeof(work));
            continue;
        }

        if (work.kind == XC_WORK_CAMERA_SELECT) {
            uint64_t input_generation = 0u;

            /* The durable selection is written before changing the host
             * input. This makes a successful user choice recoverable after
             * any subsequent input-mode transition or sidecar reconnect. */
            if (!service->ops.persist_selection ||
                service->ops.persist_selection(service->ops.userdata,
                                               work.account_id,
                                               xc_region_text(work.region), true, true,
                                               work.camera_id, work.name, work.model,
                                               work.channel) < 0) {
                xc_set_error(&error, "internal", "selection_persist_failed", 0);
            } else {
                pthread_mutex_lock(&service->lock);
                snprintf(service->selected_id, sizeof(service->selected_id), "%s",
                         work.camera_id);
                snprintf(service->selected_name, sizeof(service->selected_name), "%s",
                         work.name);
                snprintf(service->selected_model, sizeof(service->selected_model), "%s",
                         work.model);
                service->selected_channel = work.channel;
                service->selection_enabled = true;
                service->selection_configured = true;
                service->region = work.region;
                snprintf(service->selection_account_id,
                         sizeof(service->selection_account_id), "%s",
                         work.account_id);
                if (work.input_generation != service->input_generation) {
                    service->camera_state = XC_MEDIA_WAITING_INPUT;
                    xc_set_error(&error, "conflict", "input_mode_changed", 0);
                } else {
                    input_generation = xc_input_prepare_begin_locked(service);
                }
                pthread_mutex_unlock(&service->lock);
            }
            if (!error.category[0] && xc_input_prepare_guarded(service, input_generation, &error) == 0)
                ok = true;
        } else if (work.kind == XC_WORK_RECONCILE) {
            ok = true;
        } else if (work.kind == XC_WORK_CAMERA_STOP) {
            if (!work.camera_id[0]) {
                ok = true;
            } else if (!service->ops.persist_selection ||
                       service->ops.persist_selection(service->ops.userdata,
                                                      work.account_id,
                                                      xc_region_text(work.region),
                                                      work.camera_id[0] != '\0', false,
                                                      work.camera_id, work.name, work.model,
                                                      work.channel) < 0) {
                xc_set_error(&error, "internal", "selection_persist_failed", 0);
            } else ok = true;
        } else if (work.kind == XC_WORK_CLEAR) {
            bool has_account = false;

            (void)xc_source_stop(service);
            if (!service->ops.sidecar_reset ||
                service->ops.sidecar_reset(service->ops.userdata, true) != 0) {
                xc_set_error(&error, "internal", "sidecar_reset_failed", 0);
            } else if (service->ops.persist_selection &&
                       service->ops.persist_selection(service->ops.userdata, NULL, NULL,
                                                      false, false,
                                                      "", "", "", 0u) < 0) {
                xc_set_error(&error, "internal", "selection_persist_failed", 0);
            } else {
                request.action = XC_XIAOMI_ACCOUNTS;
                if (xc_call(service, &request, &response, &error) == 0) {
                    pthread_mutex_lock(&service->lock);
                    if (xc_accounts_apply_locked(service, &response, &error) == 0) {
                        has_account = service->has_account;
                        ok = true;
                    }
                    pthread_mutex_unlock(&service->lock);
                }
                if (ok) {
                    if (has_account) {
                        xc_set_error(&error, "protocol", "xiaomi_token_not_cleared", 0);
                        ok = false;
                    }
                }
            }
        } else if (work.kind == XC_WORK_CAMERA_LIST) {
            size_t selected_index = 0u;

            pthread_mutex_lock(&service->lock);
            if (!service->has_account ||
                strcmp(service->account.id, work.account_id) != 0) {
                pthread_mutex_unlock(&service->lock);
                xc_set_error(&error, "conflict", "authorization_required", 0);
            } else {
                pthread_mutex_unlock(&service->lock);
                request.action = XC_XIAOMI_CAMERAS;
                request.account_id = work.account_id;
                request.region = xc_region_text(work.region);
                if (xc_call(service, &request, &response, &error) == 0) {
                    pthread_mutex_lock(&service->lock);
                    if (xc_recovery_current_locked(service, &work) &&
                        xc_cameras_apply(service, &response, &error) == 0) {
                        service->catalog_region = work.region;
                        catalog_loaded = true;
                        ok = true;
                        if (work.verify_restored_selection) {
                            for (selected_index = 0u;
                                 selected_index < service->camera_count;
                                 ++selected_index)
                                if (!strcmp(service->cameras[selected_index].id,
                                            work.camera_id) &&
                                    !strcmp(service->cameras[selected_index].model,
                                            work.model))
                                    break;
                            if (selected_index == service->camera_count ||
                                xc_camera_source_rebuild(
                                    service->cameras[selected_index].source,
                                    work.account_id, request.region,
                                    work.camera_id, work.model,
                                    work.channel == 2u ? 2u : 1u,
                                    xc_rtsp_effective_audio(service->ops.rtsp),
                                    work.source, sizeof(work.source)) != 0) {
                                xc_set_error(&service->camera_error, "media",
                                             "selected_camera_unavailable", 0);
                                service->has_camera_error = true;
                                service->camera_state = XC_MEDIA_ERROR;
                                service->camera_codec[0] = '\0';
                            } else {
                                snprintf(service->selected_name,
                                         sizeof(service->selected_name), "%s",
                                         service->cameras[selected_index].name);
                                snprintf(service->selected_model,
                                         sizeof(service->selected_model), "%s",
                                         service->cameras[selected_index].model);
                                service->has_camera_error = false;
                                memset(&service->camera_error, 0,
                                       sizeof(service->camera_error));
                                if (!service->input_available)
                                    service->camera_state =
                                        service->selection_enabled ?
                                        XC_MEDIA_WAITING_INPUT : XC_MEDIA_STOPPED;
                            }
                        }
                    }
                    pthread_mutex_unlock(&service->lock);
                }
                if (!ok && !error.category[0])
                    xc_set_error(&error, "protocol", "xiaomi_response_invalid", 0);
            }
        } else if (work.kind == XC_WORK_CAMERA_REGION) {
            struct xc_xiaomi_camera *staged_cameras = NULL;
            size_t staged_camera_count = 0u;
            uint64_t input_generation = 0u;
            bool has_camera = false;
            bool enable_selection = false;

            request.action = XC_XIAOMI_CAMERAS;
            request.account_id = work.account_id;
            request.region = xc_region_text(work.region);
            if (xc_call(service, &request, &response, &error) == 0) {
                if (xc_cameras_prepare(service, &response,
                                       &staged_cameras) != 0)
                    xc_set_catalog_prepare_error(&error);
                else
                    staged_camera_count = response.camera_count;
                has_camera = staged_camera_count > 0u;
                if (!error.category[0] && has_camera) {
                    const struct xc_xiaomi_camera *camera = &staged_cameras[0];

                    snprintf(work.camera_id, sizeof(work.camera_id), "%s", camera->id);
                    snprintf(work.name, sizeof(work.name), "%s", camera->name);
                    snprintf(work.model, sizeof(work.model), "%s", camera->model);
                    snprintf(work.core_source, sizeof(work.core_source), "%s", camera->source);
                    if (xc_camera_source_rebuild(camera->source, work.account_id,
                                                 request.region, camera->id,
                                                 camera->model, 1u,
                                                 xc_rtsp_effective_audio(service->ops.rtsp), work.source,
                                                 sizeof(work.source)) != 0)
                        xc_set_error(&error, "protocol", "xiaomi_response_invalid", 0);
                }
                if (!error.category[0]) {
                    pthread_mutex_lock(&service->lock);
                    enable_selection = has_camera &&
                        (!work.background || service->selection_enabled);
                    if (!xc_recovery_current_locked(service, &work)) {
                        xc_set_error(&error, "conflict",
                                     "catalog_request_superseded", 0);
                    } else if (!service->ops.persist_selection ||
                               service->ops.persist_selection(
                                   service->ops.userdata,
                                   work.account_id, request.region,
                                   has_camera, enable_selection,
                                   has_camera ? work.camera_id : "",
                                   has_camera ? work.name : "",
                                   has_camera ? work.model : "", 0u) < 0) {
                        xc_set_error(&error, "internal",
                                     "selection_persist_failed", 0);
                    } else {
                        xc_cameras_commit(service, staged_cameras,
                                          staged_camera_count);
                        staged_cameras = NULL;
                        staged_camera_count = 0u;
                        service->region = work.region;
                        service->catalog_region = work.region;
                        service->selection_configured = true;
                        snprintf(service->selection_account_id,
                                 sizeof(service->selection_account_id), "%s",
                                 work.account_id);
                        if (has_camera) {
                            snprintf(service->selected_id,
                                     sizeof(service->selected_id), "%s",
                                     work.camera_id);
                            snprintf(service->selected_name,
                                     sizeof(service->selected_name), "%s",
                                     work.name);
                            snprintf(service->selected_model,
                                     sizeof(service->selected_model), "%s",
                                     work.model);
                            service->selected_channel = 0u;
                            service->selection_enabled = enable_selection;
                            if (!work.background) input_generation =
                                xc_input_prepare_begin_locked(service);
                            service->camera_state = XC_MEDIA_STARTING;
                        } else {
                            service->selected_id[0] = '\0';
                            service->selected_name[0] = '\0';
                            service->selected_model[0] = '\0';
                            service->selected_channel = 0u;
                            service->selection_enabled = false;
                            service->camera_state = XC_MEDIA_STOPPED;
                            service->camera_codec[0] = '\0';
                        }
                        catalog_loaded = true;
                        region_committed = true;
                    }
                    pthread_mutex_unlock(&service->lock);
                }
                if (region_committed)
                    ok = !has_camera || work.background || xc_input_prepare_guarded(service, input_generation, &error) == 0;
                xc_camera_array_clear(staged_cameras, staged_camera_count);
            }
        } else {
            bool needs_accounts = false;
            bool accounts_response = false;
            bool local_rate_limited = false;

            if (work.kind == XC_WORK_REFRESH) {
                request.action = XC_XIAOMI_ACCOUNTS;
                accounts_response = true;
            } else if (work.kind == XC_WORK_SMS_START) {
                pthread_mutex_lock(&service->lock);
                local_rate_limited = xc_rate_active_locked(service);
                pthread_mutex_unlock(&service->lock);
                if (local_rate_limited)
                    xc_set_error(&error, "rate_limit", "sms_retry_later", 0);
                else {
                    request.action = XC_XIAOMI_PHONE_START;
                    request.calling_code = work.calling_code;
                    request.national_number = work.national_number;
                }
            } else if (work.kind == XC_WORK_SMS_VERIFY) {
                request.action = XC_XIAOMI_PHONE_VERIFY;
                request.code = work.code;
            } else if (work.kind == XC_WORK_SMS_RESEND) {
                pthread_mutex_lock(&service->lock);
                local_rate_limited = xc_rate_active_locked(service);
                pthread_mutex_unlock(&service->lock);
                if (local_rate_limited)
                    xc_set_error(&error, "rate_limit", "sms_retry_later", 0);
                else request.action = XC_XIAOMI_PHONE_RESEND;
            } else if (work.kind == XC_WORK_CANCEL) {
                request.action = XC_XIAOMI_PHONE_CANCEL;
                needs_accounts = true;
            } else {
                xc_set_error(&error, "internal", "unknown_operation", 0);
            }
            if (!error.category[0] && xc_call(service, &request, &response, &error) == 0) {
                if (!accounts_response &&
                    !strcmp(response.phone_state, "authenticated"))
                    needs_accounts = true;
                if (!strcmp(response.phone_state, "sms_required") &&
                    xc_set_rate_deadline(service, response.phone_retry_after_seconds,
                                         &error) != 0) {
                    /* The in-memory deadline was set before this durability
                     * failure, so a retry cannot emit another SMS. */
                } else if (accounts_response) {
                    pthread_mutex_lock(&service->lock);
                    if (!xc_recovery_current_locked(service, &work))
                        xc_set_error(&error, "conflict", "operation_cancelled", 0);
                    else if (xc_accounts_apply_locked(service, &response, &error) == 0) {
                        ok = true;
                        /* Applying a different account invalidates the former
                         * account's recovery, not this accepted refresh. */
                        work.recovery_generation = service->recovery_generation;
                    }
                    pthread_mutex_unlock(&service->lock);

                } else if (needs_accounts) {
                    xc_xiaomi_response_clear(&response);
                    memset(&response, 0, sizeof(response));
                    request.action = XC_XIAOMI_ACCOUNTS;
                    if (xc_call(service, &request, &response, &error) == 0) {
                        pthread_mutex_lock(&service->lock);
                        if (xc_accounts_apply_locked(service, &response, &error) == 0)
                            ok = true;
                        pthread_mutex_unlock(&service->lock);
                    }
                } else {
                    pthread_mutex_lock(&service->lock);
                    if (xc_apply_auth_response_locked(service, &response, &error) == 0)
                        ok = true;
                    pthread_mutex_unlock(&service->lock);
                }
            }
        }
        xc_xiaomi_response_clear(&response);

        pthread_mutex_lock(&service->lock);
        if ((xc_catalog_work(work.kind) || work.kind == XC_WORK_REFRESH) &&
            (service->stopping || work.recovery_generation != service->recovery_generation)) {
            /* A new region/selection/stop owns the state. Discard the old
             * completion instead of publishing its failure or resurrecting it. */
            xc_finish_locked(service, &work, true, NULL, XC_RECOVERY_NONE);
            pthread_mutex_unlock(&service->lock);
            xc_secure_clear(&work, sizeof(work));
            continue;
        }
        if (!ok && (xc_catalog_work(work.kind) || work.kind == XC_WORK_REFRESH)) {
            recovery_result =
                xc_recovery_schedule_locked(service, &work, &error);

            if (recovery_result == XC_RECOVERY_EXHAUSTED)
                xc_set_error(&error, "protocol", "catalog_retry_exhausted",
                             error.http_status);
        }
        if (recovery_result != XC_RECOVERY_SCHEDULED &&
            service->recovery.policy != XC_RECOVERY_POLICY_NONE &&
            work.recovery_generation == service->recovery.work.recovery_generation &&
            work.kind == service->recovery.work.kind)
            xc_recovery_cancel_locked(service);
        if (ok) {
            if (work.kind <= XC_WORK_CLEAR) {
                service->has_auth_error = false;
                memset(&service->auth_error, 0, sizeof(service->auth_error));
                if (work.kind == XC_WORK_CLEAR) {
                    xc_cameras_clear(service);
                    service->selection_configured = false;
                    service->selection_account_id[0] = '\0';
                    service->region = XC_REGION_CN;
                    service->catalog_region = XC_REGION_CN;
                    service->catalog_state = XC_CATALOG_IDLE;
                    service->has_pending_region = false;
                    service->has_list_error = false;
                    memset(&service->list_error, 0, sizeof(service->list_error));
                    service->selection_enabled = false;
                    service->selected_id[0] = '\0';
                    service->selected_name[0] = '\0';
                    service->selected_model[0] = '\0';
                    service->selected_channel = 0u;
                    service->camera_state = XC_MEDIA_STOPPED;
                    service->camera_codec[0] = '\0';
                }
            } else if (work.kind == XC_WORK_CAMERA_LIST ||
                       work.kind == XC_WORK_CAMERA_REGION) {
                service->has_list_error = false;
                memset(&service->list_error, 0, sizeof(service->list_error));
                service->catalog_state = XC_CATALOG_READY;
                service->has_pending_region = false;
                if (work.kind == XC_WORK_CAMERA_REGION) {
                    service->has_camera_error = false;
                    memset(&service->camera_error, 0,
                           sizeof(service->camera_error));
                }
            } else {
                service->has_camera_error = false;
                memset(&service->camera_error, 0, sizeof(service->camera_error));
                if (work.kind == XC_WORK_CAMERA_SELECT) {
                    snprintf(service->selected_id, sizeof(service->selected_id), "%s",
                             work.camera_id);
                    snprintf(service->selected_name, sizeof(service->selected_name), "%s",
                             work.name);
                    snprintf(service->selected_model, sizeof(service->selected_model), "%s",
                             work.model);
                    service->selected_channel = work.channel;
                    service->selection_enabled = true;
                    service->camera_state = XC_MEDIA_STARTING;
                } else if (work.kind == XC_WORK_CAMERA_STOP) {
                    service->selection_enabled = false;
                    service->camera_state = XC_MEDIA_STOPPED;

                }
            }
        }
        if (catalog_loaded) {
            service->catalog_state = XC_CATALOG_READY;
            service->has_list_error = false;
            memset(&service->list_error, 0, sizeof(service->list_error));
            service->has_pending_region = false;
        } else if (!ok && (work.kind == XC_WORK_CAMERA_LIST ||
                           work.kind == XC_WORK_CAMERA_REGION)) {
            service->catalog_state =
                recovery_result == XC_RECOVERY_SCHEDULED ?
                XC_CATALOG_RETRYING : XC_CATALOG_ERROR;
            service->list_error = error;
            service->has_list_error = true;
            if (recovery_result == XC_RECOVERY_SCHEDULED &&
                work.kind == XC_WORK_CAMERA_REGION) {
                service->pending_region = work.region;
                service->has_pending_region = true;
            } else {
                service->has_pending_region = false;
            }
        }
        if (region_committed && !ok) {
            service->camera_error = error;
            service->has_camera_error = true;
            service->camera_state = XC_MEDIA_ERROR;
        }
        xc_finish_locked(service, &work, ok, &error,
                         recovery_result);
        if (ok && work.kind <= XC_WORK_CLEAR && work.kind != XC_WORK_CLEAR &&
            service->has_account && !service->pending) {
            if (service->selection_configured &&
                strcmp(service->selection_account_id, service->account.id) != 0) {
                xc_set_error(&service->auth_error, "configuration",
                             "selection_account_mismatch", 0);
                service->has_auth_error = true;
                service->auth_state = XC_AUTH_ERROR;
                service->auth_reason = XC_AUTH_REASON_NONE;
            } else if (service->selection_configured) {
                xc_queue_catalog_locked(service, XC_WORK_CAMERA_LIST,
                                        service->region);
                if (service->pending && service->selected_id[0]) {
                    service->work.verify_restored_selection = true;
                    snprintf(service->work.camera_id,
                             sizeof(service->work.camera_id), "%s",
                             service->selected_id);
                    snprintf(service->work.name,
                             sizeof(service->work.name), "%s",
                             service->selected_name);
                    snprintf(service->work.model,
                             sizeof(service->work.model), "%s",
                             service->selected_model);
                    service->work.channel = service->selected_channel;
                }
            } else {
                xc_queue_catalog_locked(service, XC_WORK_CAMERA_LIST,
                                        XC_REGION_CN);
            }
        }
        xc_queue_recover_locked(service);
        if (work.kind >= XC_WORK_REFRESH && work.kind <= XC_WORK_CLEAR) {
            fprintf(stderr,
                    "mhcamera: event=auth_operation phase=completed kind=%s state=%s result=%s error=%s provider_code=%d\n",
                    xc_work_kind_name(work.kind),
                    xc_auth_state_text(service->auth_state),
                    ok ? "ok" : "error",
                    error.message_key[0] ? error.message_key : "none",
                    error.has_provider_code ? error.provider_code : 0);
        }
        pthread_mutex_unlock(&service->lock);
        if (xc_reconcile(service) != 0)
            xc_rtsp_reconnect_error(service, XC_RTSP_ERROR_RECONNECT);
        xc_persist_status(service);
        xc_secure_clear(&work, sizeof(work));
    }
    (void)xc_source_stop(service);
    return NULL;
}

static cJSON *xc_auth_status(struct xc_service *service)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *auth = cJSON_CreateObject();
    cJSON *account = NULL;
    cJSON *challenge = NULL;
    const char *state_text;
    const char *reason_text;

    pthread_mutex_lock(&service->lock);
    state_text = xc_auth_state_text(service->auth_state);
    reason_text = xc_auth_reason_text(service->auth_reason);
    if (!root || !auth || !state_text || !reason_text ||
        !cJSON_AddNumberToObject(auth, "state_code", service->auth_state) ||
        !cJSON_AddStringToObject(auth, "state_text", state_text) ||
        !cJSON_AddNumberToObject(auth, "reason_code", service->auth_reason) ||
        !cJSON_AddStringToObject(auth, "reason_text", reason_text) ||
        !xc_add_recovery_locked(service, auth, true)) goto fail;
    if (service->has_account) {
        account = cJSON_CreateObject();
        if (!account || !cJSON_AddStringToObject(account, "masked_id",
                                                  service->account.label) ||
            !cJSON_AddItemToObject(auth, "account", account)) goto fail;
        account = NULL;
    } else if (!cJSON_AddNullToObject(auth, "account")) goto fail;
    if (service->auth_state == XC_AUTH_SMS_REQUIRED) {
        challenge = cJSON_CreateObject();
        if (!challenge ||
            !cJSON_AddStringToObject(challenge, "kind", "sms") ||
            !cJSON_AddStringToObject(challenge, "masked_target", service->sms_target) ||
            !cJSON_AddNumberToObject(challenge, "code_length",
                                     (double)service->sms_code_length) ||
            !cJSON_AddNumberToObject(challenge, "retry_after_seconds",
                                     (double)xc_rate_remaining_seconds_locked(service))) {
            goto fail;
        }
        if (!cJSON_AddItemToObject(auth, "challenge", challenge)) goto fail;
        challenge = NULL;
    } else if (!cJSON_AddNullToObject(auth, "challenge")) goto fail;
    if (service->has_auth_error) {
        cJSON *error = xc_public_error(&service->auth_error);
        if (!error || !cJSON_AddItemToObject(auth, "last_error", error)) {
            cJSON_Delete(error); goto fail;
        }
    } else if (!cJSON_AddNullToObject(auth, "last_error")) goto fail;
    if (!cJSON_AddItemToObject(root, "auth", auth)) goto fail;
    pthread_mutex_unlock(&service->lock);
    return root;
fail:
    pthread_mutex_unlock(&service->lock);
    cJSON_Delete(account); cJSON_Delete(challenge); cJSON_Delete(auth); cJSON_Delete(root);
    return NULL;
}

static cJSON *xc_camera_list(struct xc_service *service, const cJSON *query)
{
    const cJSON *refresh = cJSON_GetObjectItemCaseSensitive(query, "refresh");
    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    cJSON *regions = cJSON_CreateArray();
    const char *state_text;
    size_t index;

    if (!xc_only_keys(query, (const char *const[]){ "refresh" }, 1u) ||
        (refresh && (!cJSON_IsString(refresh) || strcmp(refresh->valuestring, "true")))) {
        cJSON_Delete(items);
        cJSON_Delete(regions);
        cJSON_Delete(list);
        cJSON_Delete(root);
        return xc_route_error(AINICE_PROTOCOL_ERROR_ROUTE_QUERY_INVALID,
                              "route_query_invalid", "camera query is invalid");
    }
    pthread_mutex_lock(&service->lock);
    if (refresh && (service->pending || service->active)) {
        pthread_mutex_unlock(&service->lock);
        cJSON_Delete(items);
        cJSON_Delete(regions);
        cJSON_Delete(list);
        cJSON_Delete(root);
        return xc_route_error(AINICE_PROTOCOL_ERROR_BUSY,
                              "busy", "another operation is working");
    }
    if (refresh && !service->has_account) {
        pthread_mutex_unlock(&service->lock);
        cJSON_Delete(items);
        cJSON_Delete(regions);
        cJSON_Delete(list);
        cJSON_Delete(root);
        return xc_route_error(AINICE_PROTOCOL_ERROR_CONFLICT,
                              "authorization_required",
                              "authorization is required");
    }
    if (refresh)
        xc_queue_catalog_locked(service, XC_WORK_CAMERA_LIST, service->region);
    state_text = xc_catalog_state_text(service->catalog_state);
    if (!root || !list || !items || !regions || !state_text ||
        !cJSON_AddNumberToObject(list, "state_code", service->catalog_state) ||
        !cJSON_AddStringToObject(list, "state_text", state_text) ||
        !xc_add_recovery_locked(service, list, false) ||
        !cJSON_AddStringToObject(list, "region", xc_region_text(service->region)) ||
        (service->has_pending_region ?
         !cJSON_AddStringToObject(list, "pending_region",
                                 xc_region_text(service->pending_region)) :
         !cJSON_AddNullToObject(list, "pending_region")))
        goto fail;
    for (index = 0u; index < XC_REGION_COUNT; ++index)
        if (!cJSON_AddItemToArray(regions,
                                 cJSON_CreateString(xc_region_text((enum xc_region)index))))
            goto fail;
    if (!cJSON_AddItemToObject(list, "regions", regions)) goto fail;
    regions = NULL;
    for (index = 0u; index < service->camera_count; ++index) {
        cJSON *item = cJSON_CreateObject();
        if (!item || !cJSON_AddStringToObject(item, "id", service->cameras[index].id) ||
            !cJSON_AddStringToObject(item, "name", service->cameras[index].name) ||
            !cJSON_AddStringToObject(item, "model", service->cameras[index].model) ||
            !cJSON_AddStringToObject(item, "home_id", service->cameras[index].home_id) ||
            !cJSON_AddStringToObject(item, "home_name", service->cameras[index].home_name) ||
            !cJSON_AddStringToObject(item, "room_id", service->cameras[index].room_id) ||
            !cJSON_AddStringToObject(item, "room_name", service->cameras[index].room_name) ||
            !cJSON_AddItemToArray(items, item)) { cJSON_Delete(item); goto fail; }
    }
    if (!cJSON_AddItemToObject(list, "items", items)) goto fail;
    items = NULL;
    if (service->has_list_error) {
        cJSON *error = xc_public_error(&service->list_error);
        if (!error || !cJSON_AddItemToObject(list, "last_error", error)) {
            cJSON_Delete(error); goto fail;
        }
    } else if (!cJSON_AddNullToObject(list, "last_error")) goto fail;
    if (!cJSON_AddItemToObject(root, "camera_list", list)) goto fail;
    pthread_mutex_unlock(&service->lock);
    return root;
fail:
    pthread_mutex_unlock(&service->lock);
    cJSON_Delete(items); cJSON_Delete(regions); cJSON_Delete(list); cJSON_Delete(root);
    return NULL;
}

static cJSON *xc_camera_status(struct xc_service *service)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *camera = cJSON_CreateObject();
    const char *state_text;

    pthread_mutex_lock(&service->lock);
    state_text = xc_media_state_text(service->camera_state);
    if (!root || !camera || !state_text ||
        !cJSON_AddNumberToObject(camera, "state_code", service->camera_state) ||
        !cJSON_AddStringToObject(camera, "state_text", state_text) ||
        (service->camera_codec[0] ?
         !cJSON_AddStringToObject(camera, "codec", service->camera_codec) :
         !cJSON_AddNullToObject(camera, "codec"))) goto fail;
    if (service->selected_id[0]) {
        cJSON *selected = cJSON_CreateObject();
        if (!selected || !cJSON_AddStringToObject(selected, "id", service->selected_id) ||
            !cJSON_AddStringToObject(selected, "region",
                                    xc_region_text(service->region)) ||
            !cJSON_AddStringToObject(selected, "name", service->selected_name) ||
            !cJSON_AddStringToObject(selected, "model", service->selected_model) ||
            (service->selected_channel == 2u &&
             !cJSON_AddNumberToObject(selected, "channel", 2.0)) ||
            !cJSON_AddItemToObject(camera, "selected", selected)) {
            cJSON_Delete(selected); goto fail;
        }
    } else if (!cJSON_AddNullToObject(camera, "selected")) goto fail;
    if (service->has_camera_error) {
        cJSON *error = xc_public_error(&service->camera_error);
        if (!error || !cJSON_AddItemToObject(camera, "last_error", error)) {
            cJSON_Delete(error); goto fail;
        }
    } else if (!cJSON_AddNullToObject(camera, "last_error")) goto fail;
    if (!cJSON_AddItemToObject(root, "camera", camera)) goto fail;
    pthread_mutex_unlock(&service->lock);
    return root;
fail:
    pthread_mutex_unlock(&service->lock);
    cJSON_Delete(camera); cJSON_Delete(root);
    return NULL;
}

static bool xc_calling_code_valid(const char *value)
{
    size_t index;
    size_t length;

    if (!value || value[0] != '+' || !(length = strlen(value)) ||
        length < 2u || length > XC_CALLING_CODE_MAX) return false;
    if (value[1] < '1' || value[1] > '9') return false;
    for (index = 2u; index < length; ++index)
        if (value[index] < '0' || value[index] > '9') return false;
    return true;
}

static bool xc_national_number_copy(const char *value, char *out, size_t out_size)
{
    const char *begin;
    const char *end;
    size_t index;
    size_t length;

    if (!value || !out || out_size < 2u) return false;
    begin = value;
    while (*begin == ' ' || *begin == '\t' || *begin == '\r' || *begin == '\n')
        begin++;
    end = begin + strlen(begin);
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t' ||
                           end[-1] == '\r' || end[-1] == '\n'))
        end--;
    length = (size_t)(end - begin);
    if (length == 0u || length >= out_size || length > XC_NATIONAL_NUMBER_MAX)
        return false;
    for (index = 0u; index < length; ++index)
        if (begin[index] < '0' || begin[index] > '9') return false;
    memcpy(out, begin, length);
    out[length] = '\0';
    return true;
}

static cJSON *xc_queue_auth(struct xc_service *service, enum xc_work_kind kind,
                            const cJSON *body)
{
    const char *operation_id = xc_json_string(body, "operation_id");
    const char *calling_code = xc_json_string(body, "calling_code");
    const char *national_number = xc_json_string(body, "national_number");
    const char *code = xc_json_string(body, "code");
    char national_copy[XC_NATIONAL_NUMBER_MAX + 1u] = {0};
    size_t code_length;
    bool valid;

    valid = xc_operation_id(operation_id) &&
            (kind != XC_WORK_SMS_START ||
             (xc_calling_code_valid(calling_code) &&
              xc_national_number_copy(national_number, national_copy,
                                      sizeof(national_copy)) &&
              strlen(calling_code) - 1u + strlen(national_copy) <= 15u)) &&
            (kind != XC_WORK_SMS_VERIFY ||
             (xc_text(code, XC_CODE_MAX) &&
              ((code_length = strlen(code)) != 0u && code_length <= XC_CODE_MAX)));
    if (!valid) {
        xc_secure_clear(national_copy, sizeof(national_copy));
        return xc_route_error(AINICE_PROTOCOL_ERROR_ROUTE_BODY_INVALID,
                              "route_body_invalid", "mutation body is invalid");
    }
    pthread_mutex_lock(&service->lock);
    if (xc_operation_cached_locked(service, operation_id)) {
        pthread_mutex_unlock(&service->lock);
        xc_secure_clear(national_copy, sizeof(national_copy));
        return xc_operation(operation_id, "working", NULL);
    }
    if (service->pending || service->active) {
        pthread_mutex_unlock(&service->lock);
        xc_secure_clear(national_copy, sizeof(national_copy));
        return xc_route_error(AINICE_PROTOCOL_ERROR_BUSY,
                              "busy", "another operation is working");
    }
    if (kind == XC_WORK_SMS_START && service->has_account) {
        pthread_mutex_unlock(&service->lock);
        xc_secure_clear(national_copy, sizeof(national_copy));
        return xc_route_error(AINICE_PROTOCOL_ERROR_CONFLICT,
                              "conflict", "an account is already authorized");
    }
    if ((kind == XC_WORK_SMS_VERIFY || kind == XC_WORK_SMS_RESEND) &&
        service->auth_state != XC_AUTH_SMS_REQUIRED) {
        pthread_mutex_unlock(&service->lock);
        xc_secure_clear(national_copy, sizeof(national_copy));
        return xc_route_error(AINICE_PROTOCOL_ERROR_CONFLICT,
                              "conflict", "sms step does not match");
    }
    if ((kind == XC_WORK_SMS_START || kind == XC_WORK_SMS_RESEND) &&
        xc_rate_active_locked(service)) {
        pthread_mutex_unlock(&service->lock);
        xc_secure_clear(national_copy, sizeof(national_copy));
        return xc_route_error(AINICE_PROTOCOL_ERROR_CONFLICT,
                              "sms_retry_later",
                              "SMS retry window is still active");
    }
    memset(&service->work, 0, sizeof(service->work));
    service->work.kind = kind;
    snprintf(service->work.operation_id, sizeof(service->work.operation_id), "%s", operation_id);
    if (calling_code)
        snprintf(service->work.calling_code, sizeof(service->work.calling_code), "%s",
                 calling_code);
    if (national_copy[0])
        memcpy(service->work.national_number, national_copy, sizeof(national_copy));
    if (code)
        snprintf(service->work.code, sizeof(service->work.code), "%s", code);
    xc_recovery_generation_advance_locked(service);
    xc_operation_record_locked(service, operation_id);
    service->pending = true;
    xc_challenge_clear(service);
    service->auth_state = XC_AUTH_WORKING;
    service->auth_reason = kind == XC_WORK_SMS_VERIFY ?
                           XC_AUTH_REASON_SMS_VERIFY :
                           kind == XC_WORK_CLEAR ? XC_AUTH_REASON_AUTH_CLEAR :
                           (kind == XC_WORK_SMS_START ||
                            kind == XC_WORK_SMS_RESEND) ?
                           XC_AUTH_REASON_SMS_START : XC_AUTH_REASON_NONE;
    service->has_auth_error = false;
    memset(&service->auth_error, 0, sizeof(service->auth_error));
    fprintf(stderr, "mhcamera: event=auth_operation phase=queued kind=%s state=working\n",
            xc_work_kind_name(kind));
    pthread_cond_signal(&service->work_cond);
    pthread_mutex_unlock(&service->lock);
    xc_secure_clear(national_copy, sizeof(national_copy));
    return xc_operation(operation_id, "working", NULL);
}

static cJSON *xc_queue_camera(struct xc_service *service, enum xc_work_kind kind,
                              const cJSON *body)
{
    const char *operation_id = xc_json_string(body, "operation_id");
    const char *camera_id = xc_json_string(body, "camera_id");
    const char *region_text = xc_json_string(body, "region");
    const cJSON *channel_item = cJSON_GetObjectItemCaseSensitive(body, "channel");
    enum xc_region region = XC_REGION_CN;
    unsigned int channel = 0u;
    size_t index;

    if (!xc_operation_id(operation_id) ||
        (kind == XC_WORK_CAMERA_SELECT &&
         (!xc_text(camera_id, XC_CAMERA_ID_MAX) ||
          xc_region_parse(region_text, &region) != 0 ||
          (channel_item && (!cJSON_IsNumber(channel_item) ||
                            channel_item->valuedouble != (double)channel_item->valueint ||
                            channel_item->valueint != 2)))))
        return xc_route_error(AINICE_PROTOCOL_ERROR_ROUTE_BODY_INVALID,
                              "route_body_invalid", "mutation body is invalid");
    if (kind == XC_WORK_CAMERA_SELECT && channel_item) channel = 2u;
    pthread_mutex_lock(&service->lock);
    if (xc_operation_cached_locked(service, operation_id)) {
        pthread_mutex_unlock(&service->lock);
        return xc_operation(operation_id, "working", NULL);
    }
    if (service->pending || service->active) {
        pthread_mutex_unlock(&service->lock);
        return xc_route_error(AINICE_PROTOCOL_ERROR_BUSY,
                              "busy", "another operation is working");
    }
    if (kind == XC_WORK_CAMERA_SELECT && !service->has_account) {
        pthread_mutex_unlock(&service->lock);
        return xc_route_error(AINICE_PROTOCOL_ERROR_CONFLICT,
                              "conflict", "authorization is required");
    }
    memset(&service->work, 0, sizeof(service->work));
    service->work.kind = kind;
    service->work.region = service->region;
    if (kind == XC_WORK_CAMERA_SELECT)
        service->work.input_generation = service->input_generation;
    snprintf(service->work.account_id, sizeof(service->work.account_id), "%s",
             service->has_account ? service->account.id :
             service->selection_account_id);
    snprintf(service->work.operation_id, sizeof(service->work.operation_id), "%s", operation_id);
    if (kind == XC_WORK_CAMERA_SELECT) {
        if (region != service->region || service->catalog_region != service->region) {
            pthread_mutex_unlock(&service->lock);
            return xc_route_error(AINICE_PROTOCOL_ERROR_CONFLICT,
                                  "invalid_region",
                                  "camera region does not match the current catalog");
        }
        for (index = 0u; index < service->camera_count; ++index)
            if (!strcmp(camera_id, service->cameras[index].id)) break;
        if (strcmp(camera_id, service->selected_id) != 0)
            channel = 0u;
        if (index < service->camera_count)
            snprintf(service->work.core_source, sizeof(service->work.core_source), "%s", service->cameras[index].source);
        if (index == service->camera_count ||
            xc_camera_source_rebuild(service->cameras[index].source, service->account.id,
                                     region_text, camera_id,
                                     service->cameras[index].model,
                                     channel == 2u ? 2u : 1u,
                                     xc_rtsp_effective_audio(service->ops.rtsp),
                                     service->work.source,
                                     sizeof(service->work.source)) != 0) {
            pthread_mutex_unlock(&service->lock);
            return xc_route_error(AINICE_PROTOCOL_ERROR_CONFLICT,
                                  "conflict", "camera is not selectable");
        }
        snprintf(service->work.camera_id, sizeof(service->work.camera_id), "%s", camera_id);
        service->work.region = region;
        service->work.channel = channel;
        snprintf(service->work.name, sizeof(service->work.name), "%s", service->cameras[index].name);
        snprintf(service->work.model, sizeof(service->work.model), "%s", service->cameras[index].model);
    } else {
        snprintf(service->work.camera_id, sizeof(service->work.camera_id), "%s",
                 service->selected_id);
        snprintf(service->work.name, sizeof(service->work.name), "%s", service->selected_name);
        snprintf(service->work.model, sizeof(service->work.model), "%s", service->selected_model);
        service->work.channel = service->selected_channel == 2u ? 2u : 0u;
    }
    if (kind == XC_WORK_CAMERA_SELECT) xc_recovery_generation_advance_locked(service);
    xc_operation_record_locked(service, operation_id);
    service->pending = true;
    if (kind == XC_WORK_CAMERA_SELECT) {
        service->camera_state = XC_MEDIA_STARTING;
        service->has_camera_error = false;
        memset(&service->camera_error, 0, sizeof(service->camera_error));
    }
    pthread_cond_signal(&service->work_cond);
    pthread_mutex_unlock(&service->lock);
    return xc_operation(operation_id, "working", NULL);
}

static cJSON *xc_queue_region(struct xc_service *service, const cJSON *body)
{
    const char *operation_id = xc_json_string(body, "operation_id");
    const char *region_text = xc_json_string(body, "region");
    enum xc_region region;

    if (!xc_operation_id(operation_id) ||
        xc_region_parse(region_text, &region) != 0)
        return xc_route_error(AINICE_PROTOCOL_ERROR_ROUTE_BODY_INVALID,
                              "invalid_region", "region is invalid");
    pthread_mutex_lock(&service->lock);
    if (xc_operation_cached_locked(service, operation_id)) {
        pthread_mutex_unlock(&service->lock);
        return xc_operation(operation_id, "working", NULL);
    }
    if (!service->has_account) {
        pthread_mutex_unlock(&service->lock);
        return xc_route_error(AINICE_PROTOCOL_ERROR_CONFLICT,
                              "authorization_required",
                              "authorization is required");
    }
    if (service->pending || service->active) {
        bool region_work =
            (service->pending &&
             (service->work.kind == XC_WORK_CAMERA_LIST ||
              service->work.kind == XC_WORK_CAMERA_REGION)) ||
            (service->active &&
             (service->active_kind == XC_WORK_CAMERA_LIST ||
              service->active_kind == XC_WORK_CAMERA_REGION));

        if (!region_work) {
            pthread_mutex_unlock(&service->lock);
            return xc_route_error(AINICE_PROTOCOL_ERROR_BUSY,
                                  "busy", "another operation is working");
        }
        xc_recovery_generation_advance_locked(service);
        xc_operation_record_locked(service, operation_id);
        service->has_queued_region = true;
        service->queued_region = region;
        snprintf(service->queued_region_operation_id,
                 sizeof(service->queued_region_operation_id), "%s",
                 operation_id);
        service->pending_region = region;
        service->has_pending_region = true;
        pthread_mutex_unlock(&service->lock);
        return xc_operation(operation_id, "working", NULL);
    }
    xc_operation_record_locked(service, operation_id);
    if (region == service->region) {
        xc_queue_catalog_locked(service, XC_WORK_CAMERA_LIST, region);
        snprintf(service->work.operation_id,
                 sizeof(service->work.operation_id), "%s", operation_id);
    } else {
        xc_queue_catalog_locked(service, XC_WORK_CAMERA_REGION, region);
        snprintf(service->work.operation_id,
                 sizeof(service->work.operation_id), "%s", operation_id);
    }
    pthread_mutex_unlock(&service->lock);
    return xc_operation(operation_id, "working", NULL);
}

static cJSON *xc_queue_rtsp(struct xc_service *service, const struct xc_route_request *request)
{
    struct xc_rtsp_job job = {0};
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(request->body, "enabled");
    const cJSON *audio = cJSON_GetObjectItemCaseSensitive(request->body, "audio_enabled");
    if (!cJSON_IsObject(request->query) || request->query->child)
        return xc_route_error(AINICE_PROTOCOL_ERROR_ROUTE_QUERY_INVALID, "rtsp_invalid_query", "RTSP query must be empty");
    job.credentials = strcmp(request->path, "/rtsp/credentials") == 0;
    if (!cJSON_IsObject(request->body) ||
        (job.credentials ? request->body->child != NULL :
         cJSON_GetArraySize(request->body) != 1 || (!cJSON_IsBool(enabled) && !cJSON_IsBool(audio))))
        return xc_route_error(AINICE_PROTOCOL_ERROR_ROUTE_BODY_INVALID, "rtsp_invalid_body", "RTSP mutation body is invalid");
    job.audio_field = audio != NULL;
    job.value = cJSON_IsTrue(job.audio_field ? audio : enabled);
    pthread_mutex_lock(&service->lock);
    if (service->stopping || service->pending || service->active) {
        pthread_mutex_unlock(&service->lock);
        return xc_route_error(AINICE_PROTOCOL_ERROR_BUSY, "busy", "another operation is working");
    }
    memset(&service->work, 0, sizeof(service->work));
    service->work.kind = XC_WORK_RTSP;
    service->work.rtsp_job = &job;
    service->pending = true;
    service->route_waiters++;
    pthread_cond_broadcast(&service->work_cond);
    /* The bridge handler is synchronous and never cancels this wait on client
     * disconnect. Only copied scalar input reaches the worker; this stack job
     * remains alive until completion, including during shutdown. */
    while (!job.done) pthread_cond_wait(&service->work_cond, &service->lock);
    service->route_waiters--;
    pthread_cond_broadcast(&service->work_cond);
    pthread_mutex_unlock(&service->lock);
    return job.response;
}

cJSON *xc_service_route(const struct xc_route_request *request, void *userdata)
{
    static const char *const empty[] = {};
    static const char *const sms_start[] = {
        "operation_id", "calling_code", "national_number"
    };
    static const char *const code[] = {"operation_id", "code"};
    static const char *const operation[] = {"operation_id"};
    static const char *const region[] = {"operation_id", "region"};
    static const char *const select[] = {
        "operation_id", "region", "camera_id", "channel"
    };
    struct xc_service *service = userdata;

    if (!service || !request || !request->http_method || !request->path ||
        !request->query || !request->body) return NULL;
    if (!strcmp(request->path, "/rtsp") || !strcmp(request->path, "/rtsp/credentials")) {
        if (!strcmp(request->http_method, "POST")) return xc_queue_rtsp(service, request);
        if (!cJSON_IsObject(request->query) || request->query->child)
            return xc_route_error(AINICE_PROTOCOL_ERROR_ROUTE_QUERY_INVALID, "rtsp_invalid_query", "RTSP query is invalid");
        if (!strcmp(request->http_method, "GET") && cJSON_IsObject(request->body) && !request->body->child) {
            if (!strcmp(request->path, "/rtsp")) return xc_rtsp_status(service, NULL);
            return xc_rtsp_route(service->ops.rtsp, request);
        }
        return xc_route_error(AINICE_PROTOCOL_ERROR_ROUTE_BODY_INVALID, "rtsp_invalid_body", "RTSP request is invalid");
    }
    if (!strcmp(request->http_method, "GET") && !strcmp(request->path, "/auth/status") &&
        xc_only_keys(request->query, empty, 0u) && xc_only_keys(request->body, empty, 0u))
        return xc_auth_status(service);
    if (!strcmp(request->http_method, "POST") && !request->query->child) {
        if (!strcmp(request->path, "/auth/sms/start") &&
            xc_only_keys(request->body, sms_start, 3u))
            return xc_queue_auth(service, XC_WORK_SMS_START, request->body);
        if (!strcmp(request->path, "/auth/sms/verify") &&
            xc_only_keys(request->body, code, 2u))
            return xc_queue_auth(service, XC_WORK_SMS_VERIFY, request->body);
        if (!strcmp(request->path, "/auth/sms/resend") &&
            xc_only_keys(request->body, operation, 1u))
            return xc_queue_auth(service, XC_WORK_SMS_RESEND, request->body);
        if (!strcmp(request->path, "/auth/cancel") && xc_only_keys(request->body, operation, 1u))
            return xc_queue_auth(service, XC_WORK_CANCEL, request->body);
        if (!strcmp(request->path, "/auth/clear") && xc_only_keys(request->body, operation, 1u))
            return xc_queue_auth(service, XC_WORK_CLEAR, request->body);
        if (!strcmp(request->path, "/camera/region") &&
            xc_only_keys(request->body, region, 2u))
            return xc_queue_region(service, request->body);
        if (!strcmp(request->path, "/camera/select") &&
            xc_only_keys(request->body, select, 4u))
            return xc_queue_camera(service, XC_WORK_CAMERA_SELECT, request->body);
        if (!strcmp(request->path, "/camera/stop") && xc_only_keys(request->body, operation, 1u))
            return xc_queue_camera(service, XC_WORK_CAMERA_STOP, request->body);
    }
    if (!strcmp(request->http_method, "GET") && !strcmp(request->path, "/camera/list") &&
        xc_only_keys(request->body, empty, 0u)) return xc_camera_list(service, request->query);
    if (!strcmp(request->http_method, "GET") && !strcmp(request->path, "/camera/status") &&
        xc_only_keys(request->query, empty, 0u) && xc_only_keys(request->body, empty, 0u))
        return xc_camera_status(service);
    return NULL;
}

int xc_service_create(struct xc_service **service_out, const struct xc_service_ops *ops)
{
    struct xc_service *service;
    pthread_condattr_t condition_attributes;
    int condition_result;

    if (!service_out || !ops || !ops->rtsp || !ops->catalog_alloc || !ops->xiaomi_call ||
        !ops->media_start || !ops->media_stop || !ops->media_request_stop || !ops->source_set || !ops->source_stop) {
        errno = EINVAL;
        return -1;
    }
    service = calloc(1u, sizeof(*service));
    if (!service) return -1;
    if (pthread_mutex_init(&service->lock, NULL) != 0) {
        free(service);
        return -1;
    }
    condition_result = pthread_condattr_init(&condition_attributes);
    if (condition_result == 0) {
        condition_result = pthread_condattr_setclock(&condition_attributes, CLOCK_MONOTONIC);
        if (condition_result == 0)
            condition_result = pthread_cond_init(&service->work_cond, &condition_attributes);
        pthread_condattr_destroy(&condition_attributes);
    }
    if (condition_result != 0) {
        pthread_mutex_destroy(&service->lock);
        free(service);
        return -1;
    }
    service->ops = *ops;
    service->auth_state = XC_AUTH_IDLE;
    service->auth_reason = XC_AUTH_REASON_NONE;
    service->catalog_state = XC_CATALOG_IDLE;
    service->region = XC_REGION_CN;
    service->catalog_region = XC_REGION_CN;
    service->camera_state = XC_MEDIA_STOPPED;
    if (service->ops.phone_rate_load) {
        uint64_t persisted_deadline = 0u;
        uint64_t now_wall;
        uint64_t now_monotonic;

        if (service->ops.phone_rate_load(service->ops.userdata,
                                         &persisted_deadline) != 0) {
            pthread_cond_destroy(&service->work_cond);
            pthread_mutex_destroy(&service->lock);
            xc_secure_clear(service, sizeof(*service));
            free(service);
            return -1;
        }
        now_wall = xc_wallclock_ms(service);
        now_monotonic = xc_monotonic_ms(service);
        if (persisted_deadline > now_wall &&
            persisted_deadline - now_wall <= UINT64_MAX - now_monotonic) {
            service->rate_deadline_monotonic_ms =
                now_monotonic + (persisted_deadline - now_wall);
        }
    }
    if (pthread_create(&service->worker, NULL, xc_worker, service) != 0) {
        pthread_cond_destroy(&service->work_cond);
        pthread_mutex_destroy(&service->lock); free(service); return -1;
    }
    service->worker_started = true;
    *service_out = service;
    return 0;
}

void xc_service_media_state(struct xc_service *service, enum xc_media_state state,
                            const char *codec, const char *message_key)
{
    if (!service) return;
    pthread_mutex_lock(&service->lock);
    if (state >= XC_MEDIA_STOPPED && state <= XC_MEDIA_ERROR) {
        if (state != XC_MEDIA_ERROR && !service->input_available &&
            service->selection_enabled)
            service->camera_state = XC_MEDIA_WAITING_INPUT;
        else
            service->camera_state = state;
        /* Only an actual AI running notification resolves a previous media
         * failure. Accepted/starting source transitions are not recovery, and
         * RTSP persistence/control errors remain owned by the RTSP manager. */
        if (state == XC_MEDIA_RUNNING && service->input_available && !message_key &&
            service->has_camera_error && !strcmp(service->camera_error.category, "media")) {
            service->has_camera_error = false;
            memset(&service->camera_error, 0, sizeof(service->camera_error));
        }
    } else {
        service->camera_state = XC_MEDIA_ERROR;
        xc_set_error(&service->camera_error, "protocol", "media_state_invalid", 0);
        service->has_camera_error = true;
    }
    if (service->input_available && codec &&
        (!strcmp(codec, "h264") || !strcmp(codec, "h265")))
        snprintf(service->camera_codec, sizeof(service->camera_codec), "%s", codec);
    else service->camera_codec[0] = '\0';
    if (message_key) {
        xc_set_error(&service->camera_error, "media", message_key, 0);
        service->has_camera_error = true;
    }
    pthread_mutex_unlock(&service->lock);
    xc_persist_status(service);
}

void xc_service_restore_selection(struct xc_service *service,
                                  const char *account_id,
                                  const char *region,
                                  bool selected,
                                  bool enabled,
                                  const char *camera_id, const char *name,
                                  const char *model,
                                  unsigned int channel)
{
    enum xc_region parsed_region;

    if (!service || !xc_decimal_text(account_id, XC_ACCOUNT_ID_MAX) ||
        xc_region_parse(region, &parsed_region) != 0 ||
        (enabled && !selected) ||
        (selected && (!xc_decimal_text(camera_id, XC_CAMERA_ID_MAX) ||
                      !xc_text(model, XC_CAMERA_MODEL_MAX))) ||
        (!selected && (enabled || (camera_id && camera_id[0]) || channel != 0u)) ||
        (channel != 0u && channel != 2u)) return;
    pthread_mutex_lock(&service->lock);
    service->selection_configured = true;
    snprintf(service->selection_account_id,
             sizeof(service->selection_account_id), "%s", account_id);
    service->region = parsed_region;
    service->catalog_region = parsed_region;
    if (!selected) {
        service->selected_id[0] = '\0';
        service->selected_name[0] = '\0';
        service->selected_model[0] = '\0';
        service->selected_channel = 0u;
        service->selection_enabled = false;
        pthread_mutex_unlock(&service->lock);
        return;
    }
    snprintf(service->selected_id, sizeof(service->selected_id), "%s", camera_id);
    snprintf(service->selected_name, sizeof(service->selected_name), "%s", name ? name : "");
    snprintf(service->selected_model, sizeof(service->selected_model), "%s", model ? model : "");
    service->selected_channel = channel == 2u ? 2u : 0u;
    service->selection_enabled = enabled;
    service->camera_state = enabled ? XC_MEDIA_WAITING_INPUT : XC_MEDIA_STOPPED;
    pthread_mutex_unlock(&service->lock);
}

void xc_service_input_changed(struct xc_service *service, bool bitstream)
{
    if (!service) return;
    pthread_mutex_lock(&service->lock);
    if (service->input_available != bitstream) {
        service->input_available = bitstream;
        service->input_generation++;
        service->reconcile_requested = true;
        if (!bitstream) {
            service->ai_cancel_requested = true;
            service->ops.media_request_stop(service->ops.userdata);
            service->camera_codec[0] = '\0';
            if (service->selected_id[0] && service->selection_enabled)
                service->camera_state = XC_MEDIA_WAITING_INPUT;
        }
        xc_queue_recover_locked(service);
        pthread_cond_broadcast(&service->work_cond);
    }
    pthread_mutex_unlock(&service->lock);
    xc_persist_status(service);
}

void xc_service_stop(struct xc_service *service)
{
    if (!service) return;
    pthread_mutex_lock(&service->lock);
    xc_recovery_generation_advance_locked(service);
    service->stopping = true;
    service->ai_cancel_requested = true;
    service->ops.media_request_stop(service->ops.userdata);
    pthread_cond_broadcast(&service->work_cond);
    pthread_mutex_unlock(&service->lock);
    if (service->worker_started) pthread_join(service->worker, NULL);
    service->worker_started = false;
}

int xc_service_refresh_auth(struct xc_service *service)
{
    if (!service) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&service->lock);
    if (service->pending || service->active) { pthread_mutex_unlock(&service->lock); errno = EBUSY; return -1; }
    memset(&service->work, 0, sizeof(service->work));
    xc_recovery_generation_advance_locked(service);
    service->work.kind = XC_WORK_REFRESH;
    service->work.recovery_generation = service->recovery_generation;
    service->pending = true;
    pthread_cond_signal(&service->work_cond);
    pthread_mutex_unlock(&service->lock);
    return 0;
}

void xc_service_destroy(struct xc_service *service)
{
    if (!service) return;
    xc_service_stop(service);
    pthread_mutex_lock(&service->lock);
    while (service->route_waiters) pthread_cond_wait(&service->work_cond, &service->lock);
    pthread_mutex_unlock(&service->lock);
    xc_accounts_clear(service);
    xc_challenge_clear(service);
    xc_cameras_clear(service);
    pthread_cond_destroy(&service->work_cond);
    pthread_mutex_destroy(&service->lock);
    xc_secure_clear(service, sizeof(*service));
    free(service);
}
