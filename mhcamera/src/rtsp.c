#define _GNU_SOURCE
#include "rtsp.h"
#include "rtsp_password.h"
#include "store.h"
#include <ainice/protocol_error_codes.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

struct xc_rtsp {
    pthread_mutex_t lock;
    struct xc_rtsp_ops ops;
    char path[PATH_MAX];
    char password[XC_RTSP_PASSWORD_STORAGE];
    bool enabled;
    bool audio_enabled;
    bool source_available;
    enum xc_rtsp_listener_state listener;
    bool query_failed;
    struct xc_rtsp_state observed;
    enum xc_rtsp_error error;
};

static const char *const rtsp_statuses[] = {"disabled", "waiting_media", "running", "error"};

const char *xc_rtsp_error_text(enum xc_rtsp_error error)
{
    static const char *const names[] = {
        NULL, "rtsp_control_failed", "rtsp_persist_failed", "rtsp_persist_not_durable",
        "rtsp_rollback_failed", "rtsp_reconnect_failed"
    };
    return error >= XC_RTSP_ERROR_NONE && error <= XC_RTSP_ERROR_RECONNECT ? names[error] : NULL;
}

static void clear_secret(void *data, size_t size)
{
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}

static int generate_password(char password[XC_RTSP_PASSWORD_STORAGE])
{
    unsigned char random[16];
    size_t used = 0, i;
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    memset(password, 0, XC_RTSP_PASSWORD_STORAGE);
    while (used < XC_RTSP_PASSWORD_LENGTH) {
        ssize_t count = getrandom(random, sizeof(random), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { clear_secret(random, sizeof(random)); return -1; }
        for (i = 0; i < (size_t)count && used < XC_RTSP_PASSWORD_LENGTH; ++i) {
            /* 248 is exactly 4 * 62: rejection avoids modulo bias. */
            if (random[i] < 248u) password[used++] = alphabet[random[i] % 62u];
        }
    }
    clear_secret(random, sizeof(random));
    return 0;
}

static int save_config(struct xc_rtsp *rtsp, bool enabled, bool audio_enabled, const char *password)
{
    char json[128];
    int length = snprintf(json, sizeof(json),
                           "{\"enabled\":%s,\"audio_enabled\":%s,\"password\":\"%s\"}\n",
                           enabled ? "true" : "false", audio_enabled ? "true" : "false", password);
    int result = rtsp->ops.save(rtsp->path, json, (size_t)length);
    clear_secret(json, sizeof(json));
    return result;
}

static int load_config(struct xc_rtsp *rtsp)
{
    /* Opening a malformed FIFO must not block before the regular-file check. */
    int fd = open(rtsp->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    char json[256] = {0};
    struct stat st;
    cJSON *root = NULL, *enabled, *audio_enabled, *password;
    ssize_t count;
    int result = -1;
    if (fd < 0) {
        if (errno != ENOENT) return -1;
        if (generate_password(rtsp->password) != 0) return -1;
        rtsp->audio_enabled = true;
        return save_config(rtsp, false, true, rtsp->password) == XC_STORE_COMMITTED_DURABLE ? 0 : -1;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 0777) != 0600 || st.st_size <= 0 ||
        st.st_size >= (off_t)sizeof(json)) goto done;
    {
        size_t offset = 0;
        while (offset < (size_t)st.st_size) {
            count = read(fd, json + offset, (size_t)st.st_size - offset);
            if (count > 0) offset += (size_t)count;
            else if (count < 0 && errno == EINTR) continue;
            else goto done;
        }
        if (strlen(json) != offset) goto done;
    }
    root = cJSON_ParseWithOpts(json, NULL, true);
    enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    audio_enabled = cJSON_GetObjectItemCaseSensitive(root, "audio_enabled");
    password = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (!cJSON_IsObject(root) || cJSON_GetArraySize(root) != 3 ||
        !cJSON_IsBool(enabled) || !cJSON_IsBool(audio_enabled) || !cJSON_IsString(password) ||
        !xc_rtsp_password_valid(password->valuestring)) goto done;
    rtsp->enabled = cJSON_IsTrue(enabled);
    rtsp->audio_enabled = cJSON_IsTrue(audio_enabled);
    snprintf(rtsp->password, sizeof(rtsp->password), "%s", password->valuestring);
    result = 0;
done:
    if (root) {
        password = cJSON_GetObjectItemCaseSensitive(root, "password");
        if (cJSON_IsString(password)) clear_secret(password->valuestring, strlen(password->valuestring));
    }
    cJSON_Delete(root);
    clear_secret(json, sizeof(json));
    close(fd);
    return result;
}

int xc_rtsp_create(struct xc_rtsp **out, const char *data_root,
                    const struct xc_rtsp_ops *ops)
{
    struct xc_rtsp *rtsp;
    if (!out || !data_root || !ops || !ops->control) return -1;
    *out = NULL;
    rtsp = calloc(1, sizeof(*rtsp));
    if (!rtsp) return -1;
    rtsp->ops = *ops;
    if (!rtsp->ops.save) rtsp->ops.save = xc_store_atomic_write;
    if (snprintf(rtsp->path, sizeof(rtsp->path), "%s/rtsp.json", data_root) >=
        (int)sizeof(rtsp->path) || load_config(rtsp) != 0 ||
        pthread_mutex_init(&rtsp->lock, NULL) != 0) {
        clear_secret(rtsp, sizeof(*rtsp)); free(rtsp); return -1;
    }
    *out = rtsp;
    return 0;
}

static int control_locked(struct xc_rtsp *rtsp, const bool *enabled)
{
    struct xc_rtsp_state state = {0};
    if (rtsp->ops.control(rtsp->ops.userdata, enabled, rtsp->password, &state) != 0) {
        rtsp->listener = XC_RTSP_LISTENER_UNKNOWN;
        if (enabled) rtsp->error = XC_RTSP_ERROR_CONTROL;
        else rtsp->query_failed = true;
        return -1;
    }
    rtsp->observed = state;
    rtsp->listener = state.listener;
    rtsp->query_failed = false;
    /* A successful read resolves transport uncertainty, but says nothing
     * about a previous save/durability/rollback failure. */
    if (!enabled && rtsp->error && rtsp->error == XC_RTSP_ERROR_CONTROL &&
        (state.listener == XC_RTSP_LISTENER_RUNNING) == (rtsp->enabled && rtsp->source_available))
        rtsp->error = XC_RTSP_ERROR_NONE;
    return 0;
}

int xc_rtsp_source(struct xc_rtsp *rtsp, bool available)
{
    bool target;
    int result;
    if (!rtsp) return 0;
    pthread_mutex_lock(&rtsp->lock);
    if (rtsp->source_available == available && !rtsp->error && (rtsp->listener != XC_RTSP_LISTENER_UNKNOWN) &&
        (rtsp->listener == XC_RTSP_LISTENER_RUNNING) == (available && rtsp->enabled)) {
        pthread_mutex_unlock(&rtsp->lock);
        return 0;
    }
    rtsp->source_available = available;
    target = available && rtsp->enabled;
    result = control_locked(rtsp, &target);
    if (result == 0 && (!rtsp->error ||
        (rtsp->error != XC_RTSP_ERROR_PERSIST &&
         rtsp->error != XC_RTSP_ERROR_NOT_DURABLE))) rtsp->error = XC_RTSP_ERROR_NONE;
    pthread_mutex_unlock(&rtsp->lock);
    return result;
}

void xc_rtsp_preferences(struct xc_rtsp *rtsp, struct xc_rtsp_preferences *out)
{
    pthread_mutex_lock(&rtsp->lock);
    out->enabled = rtsp->enabled;
    out->audio_enabled = rtsp->audio_enabled;
    pthread_mutex_unlock(&rtsp->lock);
}

bool xc_rtsp_effective_audio(struct xc_rtsp *rtsp)
{
    struct xc_rtsp_preferences preferences;
    xc_rtsp_preferences(rtsp, &preferences);
    return preferences.enabled && preferences.audio_enabled;
}

void xc_rtsp_error(struct xc_rtsp *rtsp, enum xc_rtsp_error error)
{
    pthread_mutex_lock(&rtsp->lock);
    if (!rtsp->error || rtsp->error != XC_RTSP_ERROR_NOT_DURABLE)
        rtsp->error = error;
    pthread_mutex_unlock(&rtsp->lock);
}

static void device_host(char host[INET_ADDRSTRLEN])
{
    struct ifaddrs *list, *item;
    char preferred[IFNAMSIZ] = {0}, iface[IFNAMSIZ];
    unsigned long destination, gateway, flags;
    char line[256];
    FILE *routes = fopen("/proc/net/route", "r");
    host[0] = 0;
    if (routes) {
        while (fgets(line, sizeof(line), routes))
            if (sscanf(line, "%15s %lx %lx %lx", iface, &destination, &gateway, &flags) == 4 &&
                destination == 0 && (flags & 1u)) {
                snprintf(preferred, sizeof(preferred), "%s", iface); break;
            }
        fclose(routes);
    }
    if (getifaddrs(&list) != 0) return;
    for (item = list; item; item = item->ifa_next) {
        struct sockaddr_in *address;
        uint32_t value;
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
            !(item->ifa_flags & IFF_UP) || (item->ifa_flags & IFF_LOOPBACK)) continue;
        address = (struct sockaddr_in *)item->ifa_addr;
        value = ntohl(address->sin_addr.s_addr);
        if (!value || (value >> 24) == 127 || value >= 0xe0000000u) continue;
        if (!host[0] || strcmp(preferred, item->ifa_name) == 0)
            inet_ntop(AF_INET, &address->sin_addr, host, INET_ADDRSTRLEN);
        if (strcmp(preferred, item->ifa_name) == 0) break;
    }
    freeifaddrs(list);
}

static cJSON *status_locked(struct xc_rtsp *rtsp)
{
    cJSON *root = cJSON_CreateObject(), *error;
    char host[INET_ADDRSTRLEN], url[64] = {0};
    const char *error_key = rtsp->error ? xc_rtsp_error_text(rtsp->error) :
                            rtsp->query_failed ? "rtsp_control_failed" : NULL;
    enum xc_rtsp_status state = error_key ? XC_RTSP_ERROR : !rtsp->enabled ? XC_RTSP_DISABLED :
                        rtsp->listener == XC_RTSP_LISTENER_RUNNING ? XC_RTSP_RUNNING : XC_RTSP_WAITING_SOURCE;
    if (!root) return NULL;
    device_host(host);
    if (host[0]) snprintf(url, sizeof(url), "rtsp://%s:8554/stream", host);
    if (!cJSON_AddBoolToObject(root, "enabled", rtsp->enabled) ||
        !cJSON_AddBoolToObject(root, "audio_enabled", rtsp->audio_enabled) ||
        !cJSON_AddBoolToObject(root, "running", (rtsp->listener == XC_RTSP_LISTENER_RUNNING)) ||
        !cJSON_AddBoolToObject(root, "running_known", (rtsp->listener != XC_RTSP_LISTENER_UNKNOWN)) ||
        !cJSON_AddStringToObject(root, "state", rtsp_statuses[state]) ||
        !cJSON_AddStringToObject(root, "host", host) ||
        !cJSON_AddNumberToObject(root, "port", 8554) ||
        !cJSON_AddStringToObject(root, "url", url) ||
        !cJSON_AddStringToObject(root, "username", "viewer")) goto fail;
    if (error_key) {
        error = cJSON_AddObjectToObject(root, "error");
        if (!error || !cJSON_AddStringToObject(error, "message_key", error_key)) goto fail;
    } else if (!cJSON_AddNullToObject(root, "error")) goto fail;
    return root;
fail:
    cJSON_Delete(root);
    return NULL;
}

cJSON *xc_rtsp_read(struct xc_rtsp *rtsp, struct xc_rtsp_state *state)
{
    cJSON *result;
    memset(state, 0, sizeof(*state));
    pthread_mutex_lock(&rtsp->lock);
    if (control_locked(rtsp, NULL) == 0) *state = rtsp->observed;
    result = status_locked(rtsp);
    pthread_mutex_unlock(&rtsp->lock);
    return result;
}

static cJSON *route_error(int code, const char *reason)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *error = cJSON_AddObjectToObject(root, "error");
    cJSON *details = cJSON_AddObjectToObject(error, "details");
    if (!root || !error || !details || !cJSON_AddNumberToObject(error, "code", code) ||
        !cJSON_AddStringToObject(error, "message", reason) ||
        !cJSON_AddStringToObject(details, "reason", reason)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static cJSON *credentials_locked(struct xc_rtsp *rtsp)
{
    cJSON *result = cJSON_CreateObject();
    if (!result || !cJSON_AddStringToObject(result, "username", "viewer") ||
        !cJSON_AddStringToObject(result, "password", rtsp->password)) {
        cJSON_Delete(result);
        return NULL;
    }
    return result;
}

static cJSON *refresh_password_locked(struct xc_rtsp *rtsp)
{
    char candidate[XC_RTSP_PASSWORD_STORAGE] = {0};
    bool disabled = false, target = rtsp->enabled && rtsp->source_available;
    int saved, applied;
    cJSON *result;

    /* Stage the new identity before disrupting any listener. It cannot be
     * returned by GET until the atomic store reports a committed rename. */
    if (generate_password(candidate) != 0) {
        result = route_error(AINICE_PROTOCOL_ERROR_INTERNAL_ERROR, "rtsp_random_failed");
        goto done;
    }
    if (control_locked(rtsp, &disabled) != 0) {
        result = route_error(AINICE_PROTOCOL_ERROR_INTERNAL_ERROR, "rtsp_control_failed");
        goto done;
    }
    saved = save_config(rtsp, rtsp->enabled, rtsp->audio_enabled, candidate);
    if (saved < 0) {
        /* Keep the listener closed: a refresh must never silently restore a
         * potentially exposed old identity after failing to save its replacement. */
        rtsp->error = XC_RTSP_ERROR_PERSIST;
        result = route_error(AINICE_PROTOCOL_ERROR_INTERNAL_ERROR, xc_rtsp_error_text(rtsp->error));
        goto done;
    }
    memcpy(rtsp->password, candidate, sizeof(rtsp->password));
    applied = target ? control_locked(rtsp, &target) : 0;
    if (saved != XC_STORE_COMMITTED_DURABLE) {
        rtsp->error = XC_RTSP_ERROR_NOT_DURABLE;
        result = route_error(AINICE_PROTOCOL_ERROR_INTERNAL_ERROR, xc_rtsp_error_text(rtsp->error));
        if (result && applied != 0 &&
            !cJSON_AddBoolToObject(cJSON_GetObjectItemCaseSensitive(
                cJSON_GetObjectItemCaseSensitive(result, "error"), "details"),
                "control_failed", true)) {
            cJSON_Delete(result);
            result = NULL;
        }
        goto done;
    }
    if (applied != 0) {
        result = route_error(AINICE_PROTOCOL_ERROR_INTERNAL_ERROR, "rtsp_control_failed");
        goto done;
    }
    rtsp->error = XC_RTSP_ERROR_NONE;
    result = credentials_locked(rtsp);
done:
    clear_secret(candidate, sizeof(candidate));
    return result;
}

cJSON *xc_rtsp_route(struct xc_rtsp *rtsp, const struct xc_route_request *request)
{
    cJSON *result;
    const cJSON *enabled, *audio_enabled;
    bool target, desired, desired_audio, previous;
    int saved;
    if (!rtsp || !request) return NULL;
    if (strcmp(request->path, "/rtsp") != 0 &&
        strcmp(request->path, "/rtsp/credentials") != 0) return NULL;
    if (!cJSON_IsObject(request->query) || request->query->child)
        return route_error(AINICE_PROTOCOL_ERROR_ROUTE_QUERY_INVALID, "rtsp_invalid_query");
    if (!cJSON_IsObject(request->body))
        return route_error(AINICE_PROTOCOL_ERROR_ROUTE_BODY_INVALID, "rtsp_invalid_body");
    pthread_mutex_lock(&rtsp->lock);
    if (strcmp(request->http_method, "GET") == 0 && !request->body->child) {
        if (strcmp(request->path, "/rtsp/credentials") == 0) {
            result = credentials_locked(rtsp);
        } else {
            (void)control_locked(rtsp, NULL);
            result = status_locked(rtsp);
        }
        goto done;
    }
    if (strcmp(request->path, "/rtsp/credentials") == 0 &&
        strcmp(request->http_method, "POST") == 0 && !request->body->child) {
        result = refresh_password_locked(rtsp);
        goto done;
    }
    enabled = cJSON_GetObjectItemCaseSensitive(request->body, "enabled");
    audio_enabled = cJSON_GetObjectItemCaseSensitive(request->body, "audio_enabled");
    if (strcmp(request->path, "/rtsp") != 0 ||
        strcmp(request->http_method, "POST") != 0 ||
        cJSON_GetArraySize(request->body) != 1 ||
        (!cJSON_IsBool(enabled) && !cJSON_IsBool(audio_enabled))) {
        result = route_error(AINICE_PROTOCOL_ERROR_ROUTE_BODY_INVALID, "rtsp_invalid_body");
        goto done;
    }
    desired = enabled ? cJSON_IsTrue(enabled) : rtsp->enabled;
    desired_audio = audio_enabled ? cJSON_IsTrue(audio_enabled) : rtsp->audio_enabled;
    if (desired == rtsp->enabled && desired_audio == rtsp->audio_enabled && !rtsp->error) {
        result = status_locked(rtsp);
        goto done;
    }
    previous = rtsp->enabled && rtsp->source_available;
    target = desired && rtsp->source_available;
    if (rtsp->source_available && control_locked(rtsp, &target) != 0) {
        /* A lost response may follow a successful sidecar mutation. Restore
         * the committed intent; never assume a failed RPC changed nothing. */
        if (control_locked(rtsp, &previous) != 0) rtsp->error = XC_RTSP_ERROR_ROLLBACK;
        else rtsp->error = XC_RTSP_ERROR_CONTROL;
        result = route_error(AINICE_PROTOCOL_ERROR_INTERNAL_ERROR, xc_rtsp_error_text(rtsp->error));
        goto done;
    }
    saved = save_config(rtsp, desired, desired_audio, rtsp->password);
    if (saved < 0) {
        if (rtsp->source_available && control_locked(rtsp, &previous) != 0) rtsp->error = XC_RTSP_ERROR_ROLLBACK;
        else rtsp->error = XC_RTSP_ERROR_PERSIST;
        result = route_error(AINICE_PROTOCOL_ERROR_INTERNAL_ERROR, xc_rtsp_error_text(rtsp->error));
        goto done;
    }
    rtsp->enabled = desired;
    rtsp->audio_enabled = desired_audio;
    if (saved != XC_STORE_COMMITTED_DURABLE) {
        rtsp->error = XC_RTSP_ERROR_NOT_DURABLE;
        result = route_error(AINICE_PROTOCOL_ERROR_INTERNAL_ERROR, xc_rtsp_error_text(rtsp->error));
        goto done;
    }
    rtsp->error = XC_RTSP_ERROR_NONE;
    result = status_locked(rtsp);
done:
    pthread_mutex_unlock(&rtsp->lock);
    return result;
}

void xc_rtsp_destroy(struct xc_rtsp *rtsp)
{
    if (!rtsp) return;
    pthread_mutex_destroy(&rtsp->lock);
    clear_secret(rtsp, sizeof(*rtsp));
    free(rtsp);
}
