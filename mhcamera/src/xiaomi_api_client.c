#define _GNU_SOURCE
#include "xiaomi_api_client.h"
#include "rtsp_password.h"
#include "rtsp.h"
#include "source.h"

#include <cjson/cJSON.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define XC_API_RESPONSE_MAX (1024u * 1024u)
#define XC_API_JSON_DEPTH_MAX 32u
#define XC_API_JSON_NODES_MAX 8192u
#define XC_PHONE_CODE_LENGTH_MAX 64u

static void xc_secure_clear(void *memory, size_t size)
{
    volatile unsigned char *bytes = memory;

    while (size-- > 0u) *bytes++ = 0u;
}

static void xc_free_secret(char *value)
{
    if (!value) return;
    xc_secure_clear(value, strlen(value));
    free(value);
}

static void xc_json_secure_clear(cJSON *item)
{
    cJSON *child;

    if (!item) return;
    for (child = item->child; child; child = child->next)
        xc_json_secure_clear(child);
    if (item->valuestring)
        xc_secure_clear(item->valuestring, strlen(item->valuestring));
}

static void xc_json_secure_delete(cJSON *item)
{
    xc_json_secure_clear(item);
    cJSON_Delete(item);
}

static int xc_now_ms(int64_t *value)
{
    struct timespec now;

    if (!value || clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    *value = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    return 0;
}

static int xc_deadline_after(unsigned int timeout_ms, int64_t *deadline)
{
    int64_t now;

    if (timeout_ms == 0u || !deadline || xc_now_ms(&now) != 0 ||
        now > INT64_MAX - (int64_t)timeout_ms) {
        errno = EINVAL;
        return -1;
    }
    *deadline = now + (int64_t)timeout_ms;
    return 0;
}

_Static_assert(XC_XIAOMI_CAMERAS_TIMEOUT_MS >
               XC_XIAOMI_CATALOG_INNER_TIMEOUT_MS,
               "camera API deadline must outlive catalog transaction");

unsigned int xc_xiaomi_api_timeout_for_action(
    const struct xc_xiaomi_api_client *client,
    enum xc_xiaomi_action action)
{
    if (!client) return 0u;
    if (action == XC_XIAOMI_CAMERAS &&
        client->timeout_ms < XC_XIAOMI_CAMERAS_TIMEOUT_MS)
        return XC_XIAOMI_CAMERAS_TIMEOUT_MS;
    return client->timeout_ms;
}

static int xc_poll_deadline(int fd, short events, int64_t deadline)
{
    for (;;) {
        struct pollfd descriptor = {.fd = fd, .events = events};
        int64_t now;
        int timeout;
        int ret;

        if (xc_now_ms(&now) != 0) return -1;
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        timeout = deadline - now > INT_MAX ? INT_MAX : (int)(deadline - now);
        ret = poll(&descriptor, 1u, timeout);
        if (ret > 0) {
            if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                if (!(descriptor.revents & events)) {
                    errno = EIO;
                    return -1;
                }
            }
            if (descriptor.revents & events) return 0;
        } else if (ret == 0) {
            errno = ETIMEDOUT;
            return -1;
        } else if (errno != EINTR) {
            return -1;
        }
    }
}

static int xc_connect_unix(const char *path, int64_t deadline)
{
    struct sockaddr_un address;
    int fd;
    int flags;
    int ret;

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    ret = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (ret != 0 && errno == EINPROGRESS) {
        int socket_error = 0;
        socklen_t error_size = sizeof(socket_error);

        if (xc_poll_deadline(fd, POLLOUT, deadline) != 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) != 0 ||
            socket_error != 0) {
            if (socket_error) errno = socket_error;
            close(fd);
            return -1;
        }
    } else if (ret != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int xc_write_all(int fd, const void *data, size_t size, int64_t deadline)
{
    const unsigned char *bytes = data;
    size_t offset = 0u;

    while (offset < size) {
        ssize_t wrote = send(fd, bytes + offset, size - offset, MSG_NOSIGNAL);

        if (wrote > 0) {
            offset += (size_t)wrote;
            continue;
        }
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (xc_poll_deadline(fd, POLLOUT, deadline) == 0) continue;
        }
        if (wrote == 0) errno = EPIPE;
        return -1;
    }
    return 0;
}

static int xc_read_all(int fd, char **data_out, size_t *size_out, int64_t deadline)
{
    char *data = NULL;
    size_t size = 0u;
    size_t capacity = 4096u;

    if (!data_out || !size_out) {
        errno = EINVAL;
        return -1;
    }
    data = malloc(capacity + 1u);
    if (!data) return -1;
    for (;;) {
        ssize_t got;

        if (size == capacity) {
            size_t grown_capacity = capacity * 2u;
            char *grown;

            if (grown_capacity > XC_API_RESPONSE_MAX) grown_capacity = XC_API_RESPONSE_MAX;
            if (grown_capacity <= capacity) {
                errno = EMSGSIZE;
                goto fail;
            }
            grown = realloc(data, grown_capacity + 1u);
            if (!grown) goto fail;
            data = grown;
            capacity = grown_capacity;
        }
        got = recv(fd, data + size, capacity - size, 0);
        if (got > 0) {
            size += (size_t)got;
            continue;
        }
        if (got == 0) break;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (xc_poll_deadline(fd, POLLIN, deadline) == 0) continue;
        }
        goto fail;
    }
    data[size] = '\0';
    *data_out = data;
    *size_out = size;
    return 0;

fail:
    xc_secure_clear(data, size);
    free(data);
    return -1;
}

static const char *xc_action_name(enum xc_xiaomi_action action)
{
    switch (action) {
    case XC_XIAOMI_ACCOUNTS: return "accounts";
    case XC_XIAOMI_CAMERAS: return "cameras";
    case XC_XIAOMI_PHONE_START: return "phone_start";
    case XC_XIAOMI_PHONE_VERIFY: return "phone_verify";
    case XC_XIAOMI_PHONE_RESEND: return "phone_resend";
    case XC_XIAOMI_PHONE_CANCEL: return "phone_cancel";
    }
    return NULL;
}

static bool xc_nonempty(const char *value)
{
    return value && value[0] != '\0';
}

static bool xc_region_valid(const char *region)
{
    enum xc_region parsed;

    return xc_region_parse(region, &parsed) == 0;
}

static bool xc_decimal_id(const char *value, size_t maximum, bool allow_empty);

static int xc_validate_request(const struct xc_xiaomi_request *request)
{
    if (!request || !xc_action_name(request->action)) goto invalid;
    switch (request->action) {
    case XC_XIAOMI_ACCOUNTS:
        return 0;
    case XC_XIAOMI_CAMERAS:
        if (xc_decimal_id(request->account_id, 64u, false) &&
            xc_region_valid(request->region))
            return 0;
        break;
    case XC_XIAOMI_PHONE_START:
        if (xc_nonempty(request->calling_code) &&
            xc_nonempty(request->national_number)) return 0;
        break;
    case XC_XIAOMI_PHONE_VERIFY:
        if (xc_nonempty(request->code)) return 0;
        break;
    case XC_XIAOMI_PHONE_RESEND:
    case XC_XIAOMI_PHONE_CANCEL:
        return 0;
    }
invalid:
    errno = EINVAL;
    return -1;
}

static char *xc_percent_encode(const char *value)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t length;
    size_t index;
    size_t out_index = 0u;
    char *out;

    if (!value) return NULL;
    length = strlen(value);
    if (length > (SIZE_MAX - 1u) / 3u) return NULL;
    out = malloc(length * 3u + 1u);
    if (!out) return NULL;
    for (index = 0u; index < length; ++index) {
        unsigned char ch = (unsigned char)value[index];

        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out[out_index++] = (char)ch;
        } else {
            out[out_index++] = '%';
            out[out_index++] = hex[ch >> 4];
            out[out_index++] = hex[ch & 15u];
        }
    }
    out[out_index] = '\0';
    return out;
}

static char *xc_build_post_body(const struct xc_xiaomi_request *request)
{
    const char *action = NULL;
    const char *first_key = NULL;
    const char *first_value = NULL;
    const char *second_key = NULL;
    const char *second_value = NULL;
    char *encoded_action = NULL;
    char *encoded_first = NULL;
    char *encoded_second = NULL;
    char *body = NULL;
    size_t size;

    switch (request->action) {
    case XC_XIAOMI_PHONE_START:
        action = "start";
        first_key = "calling_code";
        first_value = request->calling_code;
        second_key = "national_number";
        second_value = request->national_number;
        break;
    case XC_XIAOMI_PHONE_VERIFY:
        action = "verify";
        first_key = "code";
        first_value = request->code;
        break;
    case XC_XIAOMI_PHONE_RESEND:
        action = "resend";
        break;
    case XC_XIAOMI_PHONE_CANCEL:
        action = "cancel";
        break;
    default:
        errno = EINVAL;
        return NULL;
    }
    encoded_action = xc_percent_encode(action);
    if (!encoded_action) goto done;
    if (first_value) {
        encoded_first = xc_percent_encode(first_value);
        if (!encoded_first) goto done;
    }
    if (second_value) {
        encoded_second = xc_percent_encode(second_value);
        if (!encoded_second) goto done;
    }
    size = strlen("action=") + strlen(encoded_action) + 1u;
    if (encoded_first) size += strlen(first_key) + strlen(encoded_first) + 2u;
    if (encoded_second) size += strlen(second_key) + strlen(encoded_second) + 2u;
    body = malloc(size);
    if (!body) goto done;
    snprintf(body, size, "action=%s", encoded_action);
    if (encoded_first) {
        strcat(body, "&");
        strcat(body, first_key);
        strcat(body, "=");
        strcat(body, encoded_first);
    }
    if (encoded_second) {
        strcat(body, "&");
        strcat(body, second_key);
        strcat(body, "=");
        strcat(body, encoded_second);
    }
done:
    xc_free_secret(encoded_action);
    xc_free_secret(encoded_first);
    xc_free_secret(encoded_second);
    return body;
}

static int xc_build_target_body(const struct xc_xiaomi_request *request,
                                char **target_out,
                                char **body_out)
{
    char *target = NULL;
    char *body = NULL;

    if (request->action == XC_XIAOMI_ACCOUNTS ||
        request->action == XC_XIAOMI_CAMERAS) {
        char *account = NULL;
        char *region = NULL;
        size_t size;

        if (request->action == XC_XIAOMI_CAMERAS) {
            account = xc_percent_encode(request->account_id);
            region = xc_percent_encode(request->region);
            if (!account || !region) goto fail;
            size = strlen(account) + strlen(region) + 32u;
            target = malloc(size);
            if (target) snprintf(target, size,
                                 "/api/xiaomi?id=%s&region=%s", account, region);
        } else {
            target = strdup("/api/xiaomi");
        }
        xc_free_secret(account);
        xc_free_secret(region);
    } else {
        target = strdup("/api/xiaomi-phone");
        body = xc_build_post_body(request);
        if (!body) goto fail;
    }
    if (!target) goto fail;
    *target_out = target;
    *body_out = body;
    return 0;

fail:
    xc_free_secret(target);
    xc_free_secret(body);
    return -1;
}

static bool xc_json_unique(const cJSON *item,
                           unsigned int depth,
                           unsigned int *nodes)
{
    const cJSON *child;

    if (!item || depth > XC_API_JSON_DEPTH_MAX ||
        ++(*nodes) > XC_API_JSON_NODES_MAX) return false;
    if (cJSON_IsObject(item)) {
        for (child = item->child; child; child = child->next) {
            const cJSON *other;

            if (!child->string) return false;
            for (other = child->next; other; other = other->next) {
                if (!other->string || strcmp(child->string, other->string) == 0)
                    return false;
            }
            if (!xc_json_unique(child, depth + 1u, nodes)) return false;
        }
    } else if (cJSON_IsArray(item)) {
        for (child = item->child; child; child = child->next) {
            if (!xc_json_unique(child, depth + 1u, nodes)) return false;
        }
    }
    return true;
}

static cJSON *xc_parse_json(const char *text)
{
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithOpts(text, &end, 0);
    unsigned int nodes = 0u;

    while (end && isspace((unsigned char)*end)) end++;
    if (!root || !end || *end != '\0' || !xc_json_unique(root, 0u, &nodes)) {
        xc_json_secure_delete(root);
        errno = EPROTO;
        return NULL;
    }
    return root;
}

static char *xc_dup_json_text(const cJSON *object, const char *key, bool required)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);

    if (!item && !required) return strdup("");
    if (!cJSON_IsString(item) || !item->valuestring) {
        errno = EPROTO;
        return NULL;
    }
    return strdup(item->valuestring);
}

static bool xc_decimal_id(const char *value, size_t maximum, bool allow_empty)
{
    size_t index;
    size_t length;

    if (!value) return false;
    length = strnlen(value, maximum + 1u);
    if (length == 0u) return allow_empty;
    if (length > maximum) return false;
    for (index = 0u; index < length; ++index)
        if (value[index] < '0' || value[index] > '9') return false;
    return true;
}

static char *xc_account_label(const char *id)
{
    size_t length;
    char *label;

    if (!id || !(length = strlen(id))) return NULL;
    if (length <= 4u) return strdup("****");
    label = malloc(length + 1u);
    if (!label) return NULL;
    snprintf(label, length + 1u, "%.*s******%s", 2, id, id + length - 2u);
    return label;
}

static int xc_parse_accounts(const cJSON *root, struct xc_xiaomi_response *response)
{
    const cJSON *array = root;
    const cJSON *item;
    size_t count;
    size_t index = 0u;

    if (!cJSON_IsArray(array)) {
        errno = EPROTO;
        return -1;
    }
    count = (size_t)cJSON_GetArraySize(array);
    if (count > 64u) {
        errno = EOVERFLOW;
        return -1;
    }
    if (count == 0u) return 0;
    response->accounts = calloc(count, sizeof(*response->accounts));
    if (!response->accounts) return -1;
    response->account_count = count;
    cJSON_ArrayForEach(item, array) {
        if (cJSON_IsString(item)) {
            if (!item->valuestring ||
                !xc_decimal_id(item->valuestring, 64u, false)) {
                errno = EPROTO;
                return -1;
            }
            response->accounts[index].id = strdup(item->valuestring);
            response->accounts[index].label =
                xc_account_label(item->valuestring);
        } else {
            errno = EPROTO;
            return -1;
        }
        if (!response->accounts[index].id || !response->accounts[index].label)
            return -1;
        index++;
    }
    return 0;
}

static int xc_parse_cameras(const cJSON *root,
                            const char *account_id,
                            const char *region,
                            struct xc_xiaomi_response *response)
{
    const cJSON *response_region = cJSON_GetObjectItemCaseSensitive(root, "region");
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "sources");
    const cJSON *item;
    size_t count;
    size_t index = 0u;

    if (!cJSON_IsObject(root) || !cJSON_IsString(response_region) ||
        !response_region->valuestring ||
        strcmp(response_region->valuestring, region) != 0 ||
        !cJSON_IsArray(array)) {
        errno = EPROTO;
        return -1;
    }
    snprintf(response->catalog_region, sizeof(response->catalog_region), "%s", region);
    count = (size_t)cJSON_GetArraySize(array);
    if (count > 256u) {
        errno = EOVERFLOW;
        return -1;
    }
    if (count == 0u) return 0;
    response->cameras = calloc(count, sizeof(*response->cameras));
    if (!response->cameras) return -1;
    response->camera_count = count;
    cJSON_ArrayForEach(item, array) {
        struct xc_xiaomi_camera *camera = &response->cameras[index++];
        char source_id[65];
        char source_model[129];

        if (!cJSON_IsObject(item)) {
            errno = EPROTO;
            return -1;
        }
        camera->source = xc_dup_json_text(item, "url", true);
        camera->id = xc_dup_json_text(item, "id", true);
        camera->name = xc_dup_json_text(item, "name", true);
        camera->model = xc_dup_json_text(item, "model", true);
        camera->info = xc_dup_json_text(item, "info", true);
        camera->home_id = xc_dup_json_text(item, "home_id", true);
        camera->home_name = xc_dup_json_text(item, "home_name", true);
        camera->room_id = xc_dup_json_text(item, "room_id", true);
        camera->room_name = xc_dup_json_text(item, "room_name", true);
        if (!camera->id || !camera->name || !camera->model || !camera->info ||
            !camera->source || !camera->home_id || !camera->home_name ||
            !camera->room_id || !camera->room_name ||
            !xc_decimal_id(camera->id, 64u, false) ||
            !xc_decimal_id(camera->home_id, 64u, true) ||
            !xc_decimal_id(camera->room_id, 64u, true) ||
            strlen(camera->name) >= 128u || strlen(camera->model) >= 128u ||
            strlen(camera->home_name) >= 128u || strlen(camera->room_name) >= 128u ||
            xc_camera_source_identify(camera->source, account_id, region,
                                      source_id, sizeof(source_id), source_model,
                                      sizeof(source_model)) != 0 ||
            strcmp(source_id, camera->id) != 0 ||
            strcmp(source_model, camera->model) != 0) {
            errno = EPROTO;
            return -1;
        }
    }
    return 0;
}

static bool xc_object_only_keys(const cJSON *object,
                                const char *const *keys,
                                size_t key_count)
{
    const cJSON *item;

    if (!cJSON_IsObject(object)) return false;
    for (item = object->child; item; item = item->next) {
        size_t index;

        if (!item->string) return false;
        for (index = 0u; index < key_count; ++index)
            if (strcmp(item->string, keys[index]) == 0) break;
        if (index == key_count) return false;
    }
    return true;
}

static bool xc_json_positive_uint(const cJSON *item, unsigned int *out)
{
    double value;

    if (!cJSON_IsNumber(item) || !out) return false;
    value = item->valuedouble;
    if (value < 1.0 || value > (double)UINT_MAX ||
        value != (double)(unsigned int)value) return false;
    *out = (unsigned int)value;
    return true;
}

static bool xc_json_nonnegative_uint(const cJSON *item, unsigned int *out)
{
    double value;

    if (!cJSON_IsNumber(item) || !out) return false;
    value = item->valuedouble;
    if (value < 0.0 || value > (double)UINT_MAX ||
        value != (double)(unsigned int)value) return false;
    *out = (unsigned int)value;
    return true;
}

static bool xc_phone_error_valid(const char *value)
{
    static const char *const values[] = {
        "input", "network", "provider", "protocol", "rate_limit", "internal",
        "additional_verification_required", "sms_send_limit_tomorrow",
        "phone_account_not_found", "sms_code_invalid_or_expired",
        "xiaomi_token_response_invalid", "phone_info_response_invalid",
        "phone_info_ticket_token_missing", "ticket_auth_json_invalid",
        "ticket_auth_code_missing", "ticket_auth_location_missing",
        "ticket_auth_location_invalid", "passport_location_rejected",
        "passport_token_missing",
    };
    size_t index;

    if (!value) return false;
    for (index = 0u; index < sizeof(values) / sizeof(values[0]); ++index)
        if (strcmp(value, values[index]) == 0) return true;
    return false;
}

static int xc_parse_phone_response(const cJSON *root,
                                   struct xc_xiaomi_response *response)
{
    static const char *const sms_keys[] = {
        "state", "masked_target", "code_length", "retry_after_seconds"
    };
    static const char *const state_only_keys[] = {"state"};
    static const char *const error_keys[] = {"state", "error", "provider_code"};
    const cJSON *state;
    const cJSON *item;

    if (!cJSON_IsObject(root) ||
        !(state = cJSON_GetObjectItemCaseSensitive(root, "state")) ||
        !cJSON_IsString(state) || !state->valuestring) goto invalid;
    if (strcmp(state->valuestring, "sms_required") == 0) {
        item = cJSON_GetObjectItemCaseSensitive(root, "masked_target");
        if (!xc_object_only_keys(root, sms_keys, sizeof(sms_keys) / sizeof(sms_keys[0])) ||
            !cJSON_IsString(item) || !item->valuestring || !item->valuestring[0] ||
            strlen(item->valuestring) >= sizeof(response->phone_masked_target) ||
            !xc_json_positive_uint(cJSON_GetObjectItemCaseSensitive(root, "code_length"),
                                   &response->phone_code_length) ||
            response->phone_code_length > XC_PHONE_CODE_LENGTH_MAX ||
            !xc_json_nonnegative_uint(
                cJSON_GetObjectItemCaseSensitive(root, "retry_after_seconds"),
                &response->phone_retry_after_seconds))
            goto invalid;
        snprintf(response->phone_state, sizeof(response->phone_state), "%s",
                 "sms_required");
        snprintf(response->phone_masked_target, sizeof(response->phone_masked_target),
                 "%s", item->valuestring);
        return 0;
    }
    if (strcmp(state->valuestring, "authenticated") == 0) {
        if (!xc_object_only_keys(root, state_only_keys,
                                 sizeof(state_only_keys) /
                                 sizeof(state_only_keys[0])))
            goto invalid;
        snprintf(response->phone_state, sizeof(response->phone_state), "%s",
                 "authenticated");
        return 0;
    }
    if (strcmp(state->valuestring, "idle") == 0) {
        if (!xc_object_only_keys(root, state_only_keys,
                                 sizeof(state_only_keys) /
                                 sizeof(state_only_keys[0])))
            goto invalid;
        snprintf(response->phone_state, sizeof(response->phone_state), "%s", "idle");
        return 0;
    }
    if (strcmp(state->valuestring, "error") == 0) {
        const cJSON *provider_code =
            cJSON_GetObjectItemCaseSensitive(root, "provider_code");

        item = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (!xc_object_only_keys(root, error_keys,
                                 sizeof(error_keys) / sizeof(error_keys[0])) ||
            !cJSON_IsString(item) || !item->valuestring ||
            !xc_phone_error_valid(item->valuestring) ||
            strlen(item->valuestring) >= sizeof(response->phone_error) ||
            (provider_code &&
             (!cJSON_IsNumber(provider_code) || provider_code->valuedouble < 1.0 ||
              provider_code->valuedouble > (double)INT_MAX ||
              provider_code->valuedouble != (double)provider_code->valueint)))
            goto invalid;
        snprintf(response->phone_state, sizeof(response->phone_state), "%s", "error");
        snprintf(response->phone_error, sizeof(response->phone_error), "%s",
                 item->valuestring);
        if (provider_code) {
            response->phone_provider_code = provider_code->valueint;
            response->has_phone_provider_code = true;
        }
        return 0;
    }
invalid:
    errno = EPROTO;
    return -1;
}

static bool xc_catalog_error_shape(const cJSON *root)
{
    static const char *const keys[] = {
        "state", "error_kind", "message_key", "reason_code",
        "request_stage_code", "network_reason_code"
    };
    const cJSON *stage = cJSON_GetObjectItemCaseSensitive(root, "request_stage_code");
    const cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "network_reason_code");
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(root, "error_kind");

    if (!xc_object_only_keys(root, keys, sizeof(keys) / sizeof(keys[0])) ||
        !cJSON_IsNumber(stage) || stage->valuedouble != (double)stage->valueint ||
        stage->valueint < XC_XIAOMI_STAGE_UNKNOWN ||
        stage->valueint > XC_XIAOMI_STAGE_CLOUD_API ||
        !cJSON_IsNumber(reason) || reason->valuedouble != (double)reason->valueint ||
        reason->valueint < XC_XIAOMI_NETWORK_UNKNOWN ||
        reason->valueint > XC_XIAOMI_NETWORK_IO ||
        !cJSON_IsNumber(kind) ||
        (kind->valueint != XC_XIAOMI_ERROR_NETWORK &&
         reason->valueint != XC_XIAOMI_NETWORK_UNKNOWN)) return false;
    return true;
}

static int xc_parse_provider_error(const cJSON *root, int http_status, bool catalog,
                                   struct xc_xiaomi_error *error)
{
    const cJSON *code = NULL;
    const cJSON *state;
    const cJSON *kind;
    const cJSON *message_key;
    const cJSON *reason_code;

    if (catalog && !xc_catalog_error_shape(root)) goto invalid;
    if (http_status < 400) {
        if (catalog) goto invalid;
        return 0;
    }
    memset(error, 0, sizeof(*error));
    error->kind = XC_XIAOMI_ERROR_PROVIDER;
    snprintf(error->category, sizeof(error->category), "%s", "provider");
    snprintf(error->message_key, sizeof(error->message_key), "%s",
             "xiaomi_request_failed");
    error->http_status = http_status;
    if (cJSON_IsObject(root)) {
        state = cJSON_GetObjectItemCaseSensitive(root, "state");
        kind = cJSON_GetObjectItemCaseSensitive(root, "error_kind");
        message_key = cJSON_GetObjectItemCaseSensitive(root, "message_key");
        reason_code = cJSON_GetObjectItemCaseSensitive(root, "reason_code");
        if (cJSON_IsString(state) && state->valuestring &&
            strcmp(state->valuestring, "error") == 0 &&
            cJSON_IsNumber(kind) && kind->valuedouble == (double)kind->valueint &&
            kind->valueint >= XC_XIAOMI_ERROR_NONE &&
            kind->valueint <= XC_XIAOMI_ERROR_PROTOCOL &&
            cJSON_IsString(message_key) && message_key->valuestring &&
            (!strcmp(message_key->valuestring, "invalid_region") ||
             !strcmp(message_key->valuestring, "invalid_request") ||
             !strcmp(message_key->valuestring, "catalog_incomplete") ||
             !strcmp(message_key->valuestring, "xiaomi_request_failed") ||
             !strcmp(message_key->valuestring,
                     "xiaomi_reauthorization_required"))) {
            error->kind = (enum xc_xiaomi_error_kind)kind->valueint;
            if (catalog) {
                error->request_stage_code = (enum xc_xiaomi_request_stage)
                    cJSON_GetObjectItemCaseSensitive(root, "request_stage_code")->valueint;
                error->network_reason_code = (enum xc_xiaomi_network_reason)
                    cJSON_GetObjectItemCaseSensitive(root, "network_reason_code")->valueint;
            }
            snprintf(error->category, sizeof(error->category), "%s",
                     error->kind == XC_XIAOMI_ERROR_NETWORK ? "network" :
                     error->kind == XC_XIAOMI_ERROR_SESSION_REJECTED ||
                     error->kind == XC_XIAOMI_ERROR_CREDENTIAL_REJECTED ?
                     "authorization" :
                     error->kind == XC_XIAOMI_ERROR_PROTOCOL ? "protocol" :
                     "provider");
            snprintf(error->message_key, sizeof(error->message_key), "%s",
                     message_key->valuestring);
            if (reason_code) {
                if (!cJSON_IsNumber(reason_code) ||
                    reason_code->valuedouble != (double)reason_code->valueint ||
                    reason_code->valueint < 0) {
                    errno = EPROTO;
                    return -1;
                }
                error->reason_code = reason_code->valueint;
                error->has_reason_code = true;
            }
            return 1;
        }
        if (catalog) goto invalid;
        code = cJSON_GetObjectItemCaseSensitive(root, "code");
        if (cJSON_IsNumber(code) && code->valuedouble == (double)code->valueint) {
            error->provider_code = code->valueint;
            error->has_provider_code = true;
            if (error->provider_code == 70022)
                snprintf(error->message_key, sizeof(error->message_key), "%s",
                         "sms_send_limit_tomorrow");
        }
    }
    return 1;
invalid:
    errno = EPROTO;
    return -1;
}

static int xc_dechunk(char *body, size_t body_size, char **decoded_out)
{
    char *decoded;
    char *cursor = body;
    char *end = body + body_size;
    size_t used = 0u;

    if (!decoded_out) {
        errno = EINVAL;
        return -1;
    }
    decoded = malloc(body_size + 1u);
    if (!decoded) return -1;
    while (cursor < end) {
        char *line_end = strstr(cursor, "\r\n");
        char *number_end;
        unsigned long chunk;

        if (!line_end || line_end >= end) goto invalid;
        errno = 0;
        chunk = strtoul(cursor, &number_end, 16);
        if (errno || number_end == cursor || number_end != line_end ||
            chunk > (unsigned long)(end - line_end - 2)) goto invalid;
        cursor = line_end + 2;
        if (chunk == 0u) {
            if (end - cursor != 2 || memcmp(cursor, "\r\n", 2u) != 0)
                goto invalid;
            decoded[used] = '\0';
            *decoded_out = decoded;
            return 0;
        }
        memcpy(decoded + used, cursor, chunk);
        used += chunk;
        cursor += chunk;
        if (end - cursor < 2 || memcmp(cursor, "\r\n", 2u) != 0) goto invalid;
        cursor += 2;
    }
invalid:
    xc_secure_clear(decoded, body_size);
    free(decoded);
    errno = EPROTO;
    return -1;
}

static int xc_parse_http(char *raw,
                         size_t raw_size,
                         const struct xc_xiaomi_request *request,
                         struct xc_xiaomi_response *response)
{
    char *headers_end;
    char *status_end;
    char *body;
    char *decoded = NULL;
    size_t body_size;
    int status;
    cJSON *root = NULL;
    int ret = -1;

    if (!raw || raw_size < 16u) goto protocol_error;
    headers_end = strstr(raw, "\r\n\r\n");
    status_end = strstr(raw, "\r\n");
    if (!headers_end || !status_end ||
        sscanf(raw, "HTTP/1.%*u %d", &status) != 1 || status < 100 || status > 599)
        goto protocol_error;
    body = headers_end + 4u;
    body_size = (size_t)(raw + raw_size - body);
    if (strcasestr(raw, "\r\nTransfer-Encoding: chunked\r\n")) {
        if (xc_dechunk(body, body_size, &decoded) != 0) goto done;
        body = decoded;
        body_size = strlen(body);
    } else {
        char *length_header = strcasestr(raw, "\r\nContent-Length:");

        if (length_header) {
            char *length_end;
            unsigned long length;

            errno = 0;
            length = strtoul(length_header + strlen("\r\nContent-Length:"),
                             &length_end, 10);
            while (*length_end == ' ' || *length_end == '\t') length_end++;
            if (errno || *length_end != '\r' || length > XC_API_RESPONSE_MAX ||
                body_size != (size_t)length) goto protocol_error;
        }
    }
    if (memchr(body, '\0', body_size) != NULL) goto protocol_error;
    response->http_status = status;
    root = xc_parse_json(body);
    if (status < 200 || status >= 300) {
        ret = xc_parse_provider_error(root, status,
                                     request->action <= XC_XIAOMI_CAMERAS,
                                     &response->error) < 0 ? -1 : 0;
        goto done;
    }
    if (!root) goto done;
    if (request->action >= XC_XIAOMI_PHONE_START) {
        ret = xc_parse_phone_response(root, response);
    } else if (request->action == XC_XIAOMI_ACCOUNTS) {
        ret = xc_parse_accounts(root, response);
    } else if (request->action == XC_XIAOMI_CAMERAS) {
        ret = xc_parse_cameras(root, request->account_id, request->region,
                               response);
    } else {
        goto protocol_error;
    }
    goto done;

protocol_error:
    errno = EPROTO;
done:
    xc_free_secret(decoded);
    xc_json_secure_delete(root);
    return ret;
}

enum xc_xiaomi_network_reason xc_xiaomi_network_reason_from_errno(int error)
{
    switch (error) {
    case ETIMEDOUT: return XC_XIAOMI_NETWORK_TIMEOUT;
    case ECANCELED: return XC_XIAOMI_NETWORK_CANCELLED;
    case ENOENT:
    case ECONNREFUSED:
    case ENETUNREACH:
    case EHOSTUNREACH:
    case EADDRNOTAVAIL: return XC_XIAOMI_NETWORK_CONNECT;
    case ECONNRESET:
    case ECONNABORTED:
    case EPIPE:
    case ENOTCONN:
    case EIO: return XC_XIAOMI_NETWORK_IO;
    default: return XC_XIAOMI_NETWORK_UNKNOWN;
    }
}

int xc_xiaomi_api_client_init(struct xc_xiaomi_api_client *client,
                              const char *socket_path,
                              unsigned int timeout_ms)
{
    struct stat metadata;

    if (!client || !socket_path || socket_path[0] != '/' ||
        strlen(socket_path) >= sizeof(client->socket_path) || timeout_ms == 0u) {
        errno = EINVAL;
        return -1;
    }
    if (lstat(socket_path, &metadata) == 0 && !S_ISSOCK(metadata.st_mode)) {
        errno = EPERM;
        return -1;
    }
    memset(client, 0, sizeof(*client));
    snprintf(client->socket_path, sizeof(client->socket_path), "%s", socket_path);
    client->timeout_ms = timeout_ms;
    return 0;
}

int xc_xiaomi_api_call(struct xc_xiaomi_api_client *client,
                       const struct xc_xiaomi_request *request,
                       struct xc_xiaomi_response *response)
{
    char *target = NULL;
    char *body = NULL;
    char *http_request = NULL;
    char *raw_response = NULL;
    size_t raw_size = 0u;
    size_t request_size;
    int64_t deadline;
    int fd = -1;
    int saved;
    int ret = -1;

    if (!client || !response || xc_validate_request(request) != 0) return -1;
    memset(response, 0, sizeof(*response));
    if (xc_build_target_body(request, &target, &body) != 0) goto done;
    request_size = strlen(target) + (body ? strlen(body) : 0u) + 256u;
    http_request = malloc(request_size);
    if (!http_request) goto done;
    if (body) {
        snprintf(http_request, request_size,
                 "POST %s HTTP/1.0\r\nHost: localhost\r\n"
                 "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n"
                 "Connection: close\r\n\r\n%s",
                 target, strlen(body), body);
    } else {
        snprintf(http_request, request_size,
                 "GET %s HTTP/1.0\r\nHost: localhost\r\n"
                 "Connection: close\r\n\r\n", target);
    }
    if (xc_deadline_after(xc_xiaomi_api_timeout_for_action(client,
                                                            request->action),
                          &deadline) != 0) goto done;
    fd = xc_connect_unix(client->socket_path, deadline);
    if (fd < 0 ||
        xc_write_all(fd, http_request, strlen(http_request), deadline) != 0 ||
        xc_read_all(fd, &raw_response, &raw_size, deadline) != 0 ||
        xc_parse_http(raw_response, raw_size, request, response) != 0) goto done;
    ret = 0;

done:
    saved = errno;
    if (fd >= 0) close(fd);
    if (http_request) {
        xc_secure_clear(http_request, strlen(http_request));
        free(http_request);
    }
    xc_free_secret(target);
    xc_free_secret(body);
    if (raw_response) {
        xc_secure_clear(raw_response, raw_size);
        free(raw_response);
    }
    if (ret != 0) xc_xiaomi_response_clear(response);
    errno = saved;
    return ret;
}

int xc_xiaomi_api_wait_ready(struct xc_xiaomi_api_client *client,
                             unsigned int timeout_ms,
                             xc_xiaomi_ready_abort_fn should_abort,
                             void *userdata)
{
    struct xc_xiaomi_request request = {.action = XC_XIAOMI_ACCOUNTS};
    int64_t deadline;

    if (!client || timeout_ms == 0u ||
        xc_deadline_after(timeout_ms, &deadline) != 0) {
        errno = EINVAL;
        return -1;
    }
    for (;;) {
        struct xc_xiaomi_api_client probe = *client;
        struct xc_xiaomi_response response = {0};
        struct timespec delay;
        int64_t now;
        int abort_result;
        int call_result;

        abort_result = should_abort ? should_abort(userdata) : 0;
        if (abort_result != 0) {
            if (abort_result > 0) errno = ECHILD;
            else if (errno == 0) errno = EIO;
            return -1;
        }
        if (xc_now_ms(&now) != 0) return -1;
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        probe.timeout_ms = (unsigned int)(deadline - now);
        if (probe.timeout_ms > client->timeout_ms)
            probe.timeout_ms = client->timeout_ms;
        call_result = xc_xiaomi_api_call(&probe, &request, &response);
        if (call_result == 0 && !response.error.category[0] &&
            response.http_status >= 200 && response.http_status < 300) {
            xc_xiaomi_response_clear(&response);
            return 0;
        }
        xc_xiaomi_response_clear(&response);
        abort_result = should_abort ? should_abort(userdata) : 0;
        if (abort_result != 0) {
            if (abort_result > 0) errno = ECHILD;
            else if (errno == 0) errno = EIO;
            return -1;
        }
        if (xc_now_ms(&now) != 0) return -1;
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        delay.tv_sec = 0;
        delay.tv_nsec = (deadline - now < 20 ? deadline - now : 20) * 1000000L;
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
    }
}

void xc_xiaomi_response_clear(struct xc_xiaomi_response *response)
{
    size_t index;

    if (!response) return;
    if (response->accounts)
        for (index = 0u; index < response->account_count; ++index) {
            xc_free_secret(response->accounts[index].id);
            xc_free_secret(response->accounts[index].label);
        }
    free(response->accounts);
    if (response->cameras)
        for (index = 0u; index < response->camera_count; ++index) {
            xc_free_secret(response->cameras[index].id);
            xc_free_secret(response->cameras[index].name);
            xc_free_secret(response->cameras[index].model);
            xc_free_secret(response->cameras[index].info);
            xc_free_secret(response->cameras[index].source);
            xc_free_secret(response->cameras[index].home_id);
            xc_free_secret(response->cameras[index].home_name);
            xc_free_secret(response->cameras[index].room_id);
            xc_free_secret(response->cameras[index].room_name);
        }
    free(response->cameras);
    xc_secure_clear(response, sizeof(*response));
}

static int xc_source_error_json(const cJSON *json, enum xc_source_error *error)
{
    if (!cJSON_IsNull(json) && !cJSON_IsString(json)) return -1;
    return xc_source_error_parse(cJSON_IsString(json) ? json->valuestring : NULL, error);
}

static int xc_source_request(struct xc_xiaomi_api_client *client, const char *source)
{
    char request[1024], body[560] = {0};
    char *response = NULL, *payload;
    size_t response_size = 0;
    int64_t deadline;
    int fd = -1, status, result = -1;
    cJSON *root = NULL, *json = NULL, *item, *state, *generation;
    enum xc_source_state parsed_state;
    enum xc_source_error parsed_error;
    if (!client || (source && (strncmp(source, "xiaomi://", 9u) || strlen(source) >= 512u))) {
        errno = EINVAL; return -1;
    }
    if (source) {
        json = cJSON_CreateObject();
        if (!json || !cJSON_AddStringToObject(json, "source", source) ||
            !cJSON_PrintPreallocated(json, body, sizeof(body), false)) goto done;
    }
    snprintf(request, sizeof(request),
        "%s /api/camera/source HTTP/1.0\r\nHost: localhost\r\n"
        "Content-Type: application/json\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s", source ? "POST" : "DELETE", strlen(body), body);
    if (xc_deadline_after(3000u, &deadline) != 0) goto done;
    fd = xc_connect_unix(client->socket_path, deadline);
    if (fd < 0 || xc_write_all(fd, request, strlen(request), deadline) != 0 ||
        xc_read_all(fd, &response, &response_size, deadline) != 0) goto done;
    if (response_size < 16u || sscanf(response, "HTTP/1.%*u %d", &status) != 1 ||
        (status != 200 && status != 202) || !(payload = strstr(response, "\r\n\r\n"))) {
        errno = EPROTO; goto done;
    }
    root = cJSON_ParseWithOpts(payload + 4, NULL, true);
    item = cJSON_GetObjectItemCaseSensitive(root, "source");
    state = cJSON_GetObjectItemCaseSensitive(root, "state");
    generation = cJSON_GetObjectItemCaseSensitive(root, "generation");
    if (!cJSON_IsString(item) || strcmp(item->valuestring, source ? source : "") ||
        !cJSON_IsString(state) || xc_source_state_parse(state->valuestring, &parsed_state) != 0 ||
        !cJSON_IsNumber(generation) || !isfinite(generation->valuedouble) || generation->valuedouble < 0 ||
        trunc(generation->valuedouble) != generation->valuedouble ||
        !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root, "audio_known")) ||
        !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root, "audio_supported")) ||
        xc_source_error_json(cJSON_GetObjectItemCaseSensitive(root, "error"), &parsed_error) != 0) {
        errno = EPROTO; goto done;
    }
    if ((!source && !((status == 200 && parsed_state == XC_SOURCE_STOPPED) ||
                      (status == 202 && parsed_state == XC_SOURCE_STOPPING))) ||
        (source && ((status == 200 && parsed_state != XC_SOURCE_RUNNING) ||
                    (status == 202 && parsed_state == XC_SOURCE_STOPPED)))) {
        errno = EPROTO; goto done;
    }
    result = 0;
done:
    if (fd >= 0) close(fd);
    cJSON_Delete(root); cJSON_Delete(json);
    xc_secure_clear(request, sizeof(request)); xc_secure_clear(body, sizeof(body));
    if (response) { xc_secure_clear(response, response_size); free(response); }
    return result;
}

int xc_go2rtc_source_set(struct xc_xiaomi_api_client *client, const char *source)
{
    if (!source) { errno = EINVAL; return -1; }
    return xc_source_request(client, source);
}

int xc_go2rtc_source_stop(struct xc_xiaomi_api_client *client)
{
    return xc_source_request(client, NULL);
}

int xc_go2rtc_rtsp(struct xc_xiaomi_api_client *client, const bool *enabled,
                   const char *password, struct xc_rtsp_state *state)
{
    char request[512], body[160] = {0};
    char *response = NULL, *payload;
    size_t response_size = 0;
    cJSON *root = NULL, *item, *audio, *source, *known, *source_state, *source_error;
    int64_t deadline;
    int fd = -1, status, result = -1;

    if (!client || !state || (enabled && !xc_rtsp_password_valid(password))) {
        errno = EINVAL;
        return -1;
    }
    if (enabled) {
        snprintf(body, sizeof(body), "{\"enabled\":%s,\"username\":\"viewer\",\"password\":\"%s\"}",
                 *enabled ? "true" : "false", password);
    }
    snprintf(request, sizeof(request),
             "%s /api/camera/rtsp HTTP/1.0\r\nHost: localhost\r\n"
             "Content-Type: application/json\r\nContent-Length: %zu\r\n"
             "Connection: close\r\n\r\n%s", enabled ? "POST" : "GET", strlen(body), body);
    if (xc_deadline_after(3000u, &deadline) != 0) goto done;
    fd = xc_connect_unix(client->socket_path, deadline);
    if (fd < 0 || xc_write_all(fd, request, strlen(request), deadline) != 0 ||
        xc_read_all(fd, &response, &response_size, deadline) != 0) goto done;
    if (response_size < 16u || sscanf(response, "HTTP/1.%*u %d", &status) != 1 ||
        status < 200 || status >= 300 || !(payload = strstr(response, "\r\n\r\n"))) {
        errno = EPROTO;
        goto done;
    }
    root = cJSON_ParseWithOpts(payload + 4, NULL, true);
    item = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    audio = cJSON_GetObjectItemCaseSensitive(root, "audio_supported");
    source = cJSON_GetObjectItemCaseSensitive(root, "source");
    known = cJSON_GetObjectItemCaseSensitive(root, "audio_known");
    source_state = cJSON_GetObjectItemCaseSensitive(root, "source_state");
    source_error = cJSON_GetObjectItemCaseSensitive(root, "source_error");
    if (!cJSON_IsBool(item) || (enabled && cJSON_IsTrue(item) != *enabled) ||
        !cJSON_IsBool(audio) || !cJSON_IsString(source) ||
        strlen(source->valuestring) >= sizeof(state->source) || !cJSON_IsBool(known) ||
        !cJSON_IsString(source_state) || xc_source_state_parse(source_state->valuestring, &state->source_state) != 0 ||
        xc_source_error_json(source_error, &state->source_error) != 0) {
        errno = EPROTO;
        goto done;
    }
    state->listener = cJSON_IsTrue(item) ? XC_RTSP_LISTENER_RUNNING : XC_RTSP_LISTENER_STOPPED;
    state->audio_supported = cJSON_IsTrue(audio);
    state->audio_known = cJSON_IsTrue(known);
    snprintf(state->source, sizeof(state->source), "%s", source->valuestring);

    result = 0;
done:
    if (fd >= 0) close(fd);
    cJSON_Delete(root);
    xc_secure_clear(request, sizeof(request));
    xc_secure_clear(body, sizeof(body));
    if (response) { xc_secure_clear(response, response_size); free(response); }
    return result;
}
