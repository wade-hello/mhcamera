#ifndef MHCAMERA_XIAOMI_API_CLIENT_H
#define MHCAMERA_XIAOMI_API_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The go2rtc catalog handler owns a 45 second inner transaction. The Unix
 * socket/HTTP caller must outlive it so an optional home-metadata timeout can
 * still return the authoritative device catalog. */
#define XC_XIAOMI_CATALOG_INNER_TIMEOUT_MS 45000u
#define XC_XIAOMI_CATALOG_TRANSPORT_MARGIN_MS 15000u
#define XC_XIAOMI_CAMERAS_TIMEOUT_MS \
    (XC_XIAOMI_CATALOG_INNER_TIMEOUT_MS + \
     XC_XIAOMI_CATALOG_TRANSPORT_MARGIN_MS)

enum xc_xiaomi_action {
    XC_XIAOMI_ACCOUNTS = 0,
    XC_XIAOMI_CAMERAS,
    XC_XIAOMI_PHONE_START,
    XC_XIAOMI_PHONE_VERIFY,
    XC_XIAOMI_PHONE_RESEND,
    XC_XIAOMI_PHONE_CANCEL,
};

enum xc_xiaomi_error_kind {
    XC_XIAOMI_ERROR_NONE = 0,
    XC_XIAOMI_ERROR_NETWORK = 1,
    XC_XIAOMI_ERROR_HTTP = 2,
    XC_XIAOMI_ERROR_PROVIDER = 3,
    XC_XIAOMI_ERROR_SESSION_REJECTED = 4,
    XC_XIAOMI_ERROR_CREDENTIAL_REJECTED = 5,
    XC_XIAOMI_ERROR_PROTOCOL = 6,
};

enum xc_xiaomi_request_stage {
    XC_XIAOMI_STAGE_UNKNOWN = 0,
    XC_XIAOMI_STAGE_TOKEN_LOGIN = 1,
    XC_XIAOMI_STAGE_TOKEN_FINISH = 2,
    XC_XIAOMI_STAGE_CLOUD_API = 3,
    XC_XIAOMI_STAGE_LOCAL_RPC = 4,
};

enum xc_xiaomi_network_reason {
    XC_XIAOMI_NETWORK_UNKNOWN = 0,
    XC_XIAOMI_NETWORK_DNS = 1,
    XC_XIAOMI_NETWORK_CONNECT = 2,
    XC_XIAOMI_NETWORK_TIMEOUT = 3,
    XC_XIAOMI_NETWORK_TLS_CERTIFICATE = 4,
    XC_XIAOMI_NETWORK_TLS_HANDSHAKE = 5,
    XC_XIAOMI_NETWORK_CANCELLED = 6,
    XC_XIAOMI_NETWORK_IO = 7,
};

struct xc_xiaomi_request {
    enum xc_xiaomi_action action;
    const char *account_id;
    const char *region;
    const char *calling_code;
    const char *national_number;
    const char *code;
};

struct xc_xiaomi_error {
    enum xc_xiaomi_error_kind kind;
    enum xc_xiaomi_request_stage request_stage_code;
    enum xc_xiaomi_network_reason network_reason_code;
    char category[48];
    int provider_code;
    bool has_provider_code;
    char message_key[64];
    int http_status;
    int reason_code;
    bool has_reason_code;
};

struct xc_xiaomi_account {
    char *id;
    char *label;
};

struct xc_xiaomi_camera {
    char *id;
    char *name;
    char *model;
    char *info;
    char *source;
    char *home_id;
    char *home_name;
    char *room_id;
    char *room_name;
};

struct xc_xiaomi_response {
    int http_status;
    char catalog_region[3];
    char phone_state[32];
    char phone_masked_target[96];
    unsigned int phone_code_length;
    unsigned int phone_retry_after_seconds;
    char phone_error[64];
    int phone_provider_code;
    bool has_phone_provider_code;
    struct xc_xiaomi_error error;
    struct xc_xiaomi_account *accounts;
    size_t account_count;
    struct xc_xiaomi_camera *cameras;
    size_t camera_count;
};

struct xc_xiaomi_api_client {
    char socket_path[108];
    unsigned int timeout_ms;
};

typedef int (*xc_xiaomi_ready_abort_fn)(void *userdata);

/* Classify a saved errno from a failed local Unix RPC; no I/O or state changes. */
enum xc_xiaomi_network_reason xc_xiaomi_network_reason_from_errno(int error);

int xc_xiaomi_api_client_init(struct xc_xiaomi_api_client *client,
                              const char *socket_path,
                              unsigned int timeout_ms);
unsigned int xc_xiaomi_api_timeout_for_action(
    const struct xc_xiaomi_api_client *client,
    enum xc_xiaomi_action action);
int xc_xiaomi_api_call(struct xc_xiaomi_api_client *client,
                       const struct xc_xiaomi_request *request,
                       struct xc_xiaomi_response *response);
int xc_xiaomi_api_wait_ready(struct xc_xiaomi_api_client *client,
                             unsigned int timeout_ms,
                             xc_xiaomi_ready_abort_fn should_abort,
                             void *userdata);
void xc_xiaomi_response_clear(struct xc_xiaomi_response *response);
int xc_go2rtc_source_set(struct xc_xiaomi_api_client *client,
                            const char *source);
int xc_go2rtc_source_stop(struct xc_xiaomi_api_client *client);
/* enabled == NULL reads actual state. A write uses the independent RTSP
 * playback identity and a short control-plane deadline. */
struct xc_rtsp_state;
int xc_go2rtc_rtsp(struct xc_xiaomi_api_client *client, const bool *enabled,
                   const char *password, struct xc_rtsp_state *state);

#ifdef __cplusplus
}
#endif

#endif
