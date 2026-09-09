#define _GNU_SOURCE
#include "bridge.h"
#include "host_input.h"
#include "media.h"
#include "process.h"
#include "rtsp.h"
#include "service.h"
#include "store.h"
#include "xiaomi_api_client.h"

#include <ainice/bridge.h>
#include <ainice/event.h>

#include <cjson/cJSON.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct xc_app {
    struct xc_process child;
    struct xc_xiaomi_api_client api;
    struct xc_media_owner *media;
    struct xc_service *service;
    struct xc_rtsp *rtsp;
    char status_path[PATH_MAX];
    char selection_path[PATH_MAX];
    char phone_rate_path[PATH_MAX];
    char executable[PATH_MAX];
    char config_path[PATH_MAX];
    char api_socket_path[108];
    char media_socket_path[108];
    bool child_started;
    bool child_failed;
    atomic_bool sidecar_resetting;
    pthread_t event_thread;
    bool event_thread_started;
};

static volatile sig_atomic_t xc_stopping;

#define XC_XIAOMI_DEFAULT_API_TIMEOUT_MS 20000u
#define XC_DATA_DIRECTORY "/data/plugins/mhcamera"

static int xc_ready_abort(void *userdata);
static void xc_media_status(void *userdata,
                            enum xc_media_state state,
                            const char *codec,
                            const char *message_key);

static void xc_secure_clear(void *memory, size_t size)
{
    volatile unsigned char *bytes = memory;

    while (size-- > 0u) *bytes++ = 0u;
}

static void xc_signal_handler(int signal_number)
{
    (void)signal_number;
    xc_stopping = 1;
}

static int xc_install_signals(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = xc_signal_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) != 0 ||
        sigaction(SIGINT, &action, NULL) != 0) return -1;
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

static int xc_api_call_adapter(void *userdata,
                               const struct xc_xiaomi_request *request,
                               struct xc_xiaomi_response *response)
{
    struct xc_app *app = userdata;

    return xc_xiaomi_api_call(&app->api, request, response);
}

static int xc_source_set_adapter(void *userdata, const char *source)
{
    struct xc_app *app = userdata;

    return xc_go2rtc_source_set(&app->api, source);
}

static int xc_source_stop_adapter(void *userdata)
{
    struct xc_app *app = userdata;

    return xc_go2rtc_source_stop(&app->api);
}

static int xc_media_start_adapter(void *userdata)
{
    struct xc_app *app = userdata;
    return xc_media_owner_start(app->media);
}

static int xc_rtsp_control_adapter(void *userdata, const bool *enabled,
                                   const char *password, struct xc_rtsp_state *state)
{
    struct xc_app *app = userdata;
    return xc_go2rtc_rtsp(&app->api, enabled, password, state);
}

static void *xc_catalog_alloc_adapter(void *userdata, size_t size)
{
    (void)userdata;
    return malloc(size);
}

static int xc_sidecar_start(struct xc_app *app)
{
    char *argv[] = {app->executable, "-config", app->config_path, NULL};

    if (xc_process_start(&app->child, app->executable, argv,
                         app->api_socket_path, 10000u) != 0)
        return -1;
    app->child_started = true;
    if (xc_xiaomi_api_wait_ready(&app->api, 10000u, xc_ready_abort, app) != 0) {
        (void)xc_process_stop(&app->child, 5000u);
        app->child_started = false;
        return -1;
    }
    if ((app->media ? xc_media_owner_rebind(app->media, app->child.pid) :
         xc_media_owner_create(&app->media, app->media_socket_path, app->child.pid,
                               xc_media_status, app)) != 0) {
        (void)xc_process_stop(&app->child, 5000u);
        app->child_started = false;
        return -1;
    }
    return 0;
}

static int xc_sidecar_reset_adapter(void *userdata, bool clear_token)
{
    struct xc_app *app = userdata;
    int result = -1;

    atomic_store_explicit(&app->sidecar_resetting, true, memory_order_release);
    xc_media_owner_stop(app->media);
    if (app->child_started && !app->child.exited &&
        xc_process_stop(&app->child, 5000u) != 0)
        goto done;
    app->child_started = false;
    if (clear_token && xc_store_clear_xiaomi_token(app->config_path,
                                                   app->api_socket_path) != 0)
        goto done;
    result = xc_sidecar_start(app);
done:
    atomic_store_explicit(&app->sidecar_resetting, false, memory_order_release);
    return result;
}

static int xc_input_prepare_adapter(void *userdata,
                                    xc_input_prepare_guard_fn patch_allowed,
                                    void *guard_userdata)
{
    (void)userdata;
    return xc_host_input_ensure_bitstream(patch_allowed, guard_userdata);
}

static void xc_media_stop_adapter(void *userdata)
{
    struct xc_app *app = userdata;

    xc_media_owner_stop(app->media);
}

static void xc_media_request_stop_adapter(void *userdata)
{
    struct xc_app *app = userdata;
    xc_media_owner_request_stop(app->media);
}

static void xc_media_status(void *userdata,
                            enum xc_media_state state,
                            const char *codec,
                            const char *message_key)
{
    struct xc_app *app = userdata;

    xc_service_media_state(app->service, state, codec, message_key);
}

static int xc_persist_status(void *userdata, const char *json)
{
    struct xc_app *app = userdata;

    return xc_store_atomic_write(app->status_path, json, strlen(json));
}

static int xc_persist_selection(void *userdata,
                                const char *account_id,
                                const char *region,
                                bool exists,
                                bool enabled,
                                const char *camera_id,
                                const char *name,
                                const char *model,
                                unsigned int channel)
{
    struct xc_app *app = userdata;
    struct xc_saved_selection selection;

    if (!account_id || !region)
        return xc_store_selection_remove(app->selection_path);
    memset(&selection, 0, sizeof(selection));
    selection.configured = true;
    snprintf(selection.account_id, sizeof(selection.account_id), "%s", account_id);
    snprintf(selection.region, sizeof(selection.region), "%s", region);
    selection.exists = exists;
    selection.enabled = enabled;
    snprintf(selection.id, sizeof(selection.id), "%s", camera_id ? camera_id : "");
    snprintf(selection.name, sizeof(selection.name), "%s", name ? name : "");
    snprintf(selection.model, sizeof(selection.model), "%s", model ? model : "");
    selection.channel = channel == 2u ? 2u : 0u;
    return xc_store_selection_write(app->selection_path, &selection);
}

static int xc_phone_rate_load(void *userdata, uint64_t *retry_after_wall_ms)
{
    struct xc_app *app = userdata;

    return xc_store_phone_rate_read(app->phone_rate_path, retry_after_wall_ms);
}

static int xc_phone_rate_save(void *userdata, uint64_t retry_after_wall_ms)
{
    struct xc_app *app = userdata;

    return xc_store_phone_rate_write(app->phone_rate_path, retry_after_wall_ms);
}

static cJSON *xc_app_route(const struct xc_route_request *request, void *userdata)
{
    struct xc_app *app = userdata;
    return xc_service_route(request, app->service);
}

static int xc_bridge_handler(const char *request_json,
                             ainice_bridge_response_t *response,
                             void *userdata)
{
    struct xc_app *app = userdata;
    char *response_json = NULL;
    int result;

    result = xc_bridge_dispatch_json(request_json, xc_app_route,
                                     app, &response_json);
    if (result == 0)
        result = ainice_bridge_response_json(response, response_json);
    if (response_json) {
        xc_secure_clear(response_json, strlen(response_json));
        cJSON_free(response_json);
    }
    return result;
}

static int xc_should_stop(void *userdata)
{
    struct xc_app *app = userdata;
    bool exited = false;

    if (xc_stopping) return 1;
    if (atomic_load_explicit(&app->sidecar_resetting, memory_order_acquire)) return 0;
    if (app->child_started && xc_process_poll(&app->child, &exited, NULL) != 0) {
        app->child_failed = true;
        return 1;
    }
    if (exited) {
        app->child_failed = true;
        fprintf(stderr, "mhcamera: event=go2rtc_exit result=unexpected\n");
        return 1;
    }
    return 0;
}

static int xc_event_should_stop(void *userdata)
{
    return xc_should_stop(userdata);
}

static int xc_config_changed(const ainice_config_changed_t *event, void *userdata)
{
    struct xc_app *app = userdata;
    bool bitstream;

    (void)event;
    if (xc_host_input_is_bitstream(&bitstream) == 0) {
        fprintf(stderr, "mhcamera: event=input_mode source=config_changed mode=%s\n",
                bitstream ? "bitstream" : "other");
        xc_service_input_changed(app->service, bitstream);
    } else
        fprintf(stderr, "mhcamera: event=input_mode_read result=failed\n");
    return xc_should_stop(app);
}

static void *xc_event_main(void *userdata)
{
    struct xc_app *app = userdata;

    while (!xc_should_stop(app)) {
        if (ainice_config_changed_subscribe_until(xc_config_changed, app,
                                                  xc_event_should_stop, app) != 0 &&
            !xc_should_stop(app))
            sleep(1u);
    }
    return NULL;
}

static int xc_ready_abort(void *userdata)
{
    struct xc_app *app = userdata;
    bool exited = false;

    if (xc_stopping) return 1;
    if (xc_process_poll(&app->child, &exited, NULL) != 0) return -1;
    if (exited) {
        app->child_failed = true;
        return 1;
    }
    return 0;
}

static int xc_regular_executable(const char *path)
{
    struct stat metadata;

    if (lstat(path, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        (metadata.st_mode & 0111) == 0) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

int main(void)
{
    const char *plugin_dir = getenv("AINICE_PLUGIN_DIR");
    const char *data_dir = getenv("AINICE_PLUGIN_DATA");
    struct xc_app app;
    struct xc_service_ops service_ops;
    struct xc_rtsp_ops rtsp_ops;
    struct xc_saved_selection saved_selection;
    int bridge_result = -1;
    int stop_result = 0;

    memset(&app, 0, sizeof(app));
    atomic_init(&app.sidecar_resetting, false);
    if (geteuid() == 0 || !plugin_dir || !data_dir || plugin_dir[0] != '/' ||
        data_dir[0] != '/' || strcmp(plugin_dir, "/") == 0 ||
        strcmp(data_dir, XC_DATA_DIRECTORY) != 0 || xc_install_signals() != 0) {
        fprintf(stderr, "mhcamera: event=startup_failed stage=environment\n");
        return 1;
    }
    if (snprintf(app.executable, sizeof(app.executable), "%s/bin/go2rtc", plugin_dir) >=
            (int)sizeof(app.executable) || xc_regular_executable(app.executable) != 0 ||
        xc_store_prepare(data_dir, app.config_path, sizeof(app.config_path), app.api_socket_path,
                         sizeof(app.api_socket_path), app.status_path,
                         sizeof(app.status_path)) != 0 ||
        xc_store_selection_path(data_dir, app.selection_path,
                                sizeof(app.selection_path)) != 0 ||
        xc_store_phone_rate_path(data_dir, app.phone_rate_path,
                                 sizeof(app.phone_rate_path)) != 0 ||
        xc_store_selection_read(app.selection_path, &saved_selection) != 0) {
        fprintf(stderr, "mhcamera: event=startup_failed stage=layout\n");
        return 1;
    }
    if (snprintf(app.media_socket_path, sizeof(app.media_socket_path),
                 "%s/tmp/go2rtc-media.sock", data_dir) >=
            (int)sizeof(app.media_socket_path) ||
        xc_xiaomi_api_client_init(&app.api, app.api_socket_path,
                                  XC_XIAOMI_DEFAULT_API_TIMEOUT_MS) != 0 ||
        xc_sidecar_start(&app) != 0) {
        fprintf(stderr, "mhcamera: event=startup_failed stage=go2rtc_api\n");
        goto shutdown;
    }
    memset(&rtsp_ops, 0, sizeof(rtsp_ops));
    rtsp_ops.control = xc_rtsp_control_adapter;
    rtsp_ops.userdata = &app;
    if (xc_rtsp_create(&app.rtsp, data_dir, &rtsp_ops) != 0) {
        fprintf(stderr, "mhcamera: event=startup_failed stage=rtsp_config\n");
        goto shutdown;
    }
    memset(&service_ops, 0, sizeof(service_ops));
    service_ops.rtsp = app.rtsp;
    service_ops.catalog_alloc = xc_catalog_alloc_adapter;
    service_ops.xiaomi_call = xc_api_call_adapter;
    service_ops.source_set = xc_source_set_adapter;
    service_ops.source_stop = xc_source_stop_adapter;
    service_ops.input_prepare = xc_input_prepare_adapter;
    service_ops.media_start = xc_media_start_adapter;
    service_ops.media_stop = xc_media_stop_adapter;
    service_ops.media_request_stop = xc_media_request_stop_adapter;
    service_ops.sidecar_reset = xc_sidecar_reset_adapter;
    service_ops.persist_selection = xc_persist_selection;
    service_ops.phone_rate_load = xc_phone_rate_load;
    service_ops.phone_rate_save = xc_phone_rate_save;
    service_ops.persist_status = xc_persist_status;
    service_ops.userdata = &app;
    if (xc_service_create(&app.service, &service_ops) != 0) {
        fprintf(stderr, "mhcamera: event=startup_failed stage=service\n");
        goto shutdown;
    }
    if (saved_selection.configured) {
        xc_service_restore_selection(app.service, saved_selection.account_id,
                                     saved_selection.region,
                                     saved_selection.exists,
                                     saved_selection.enabled,
                                     saved_selection.id,
                                     saved_selection.name,
                                     saved_selection.model,
                                     saved_selection.channel);
    }
    {
        bool bitstream;

        if (xc_host_input_is_bitstream(&bitstream) == 0) {
            fprintf(stderr, "mhcamera: event=input_mode source=startup mode=%s\n",
                    bitstream ? "bitstream" : "other");
            xc_service_input_changed(app.service, bitstream);
        } else
            fprintf(stderr, "mhcamera: event=input_mode_read result=failed\n");
    }
    if (xc_service_refresh_auth(app.service) != 0)
        fprintf(stderr, "mhcamera: event=auth_refresh result=not_started\n");
    if (pthread_create(&app.event_thread, NULL, xc_event_main, &app) != 0) {
        fprintf(stderr, "mhcamera: event=startup_failed stage=config_events\n");
        goto shutdown;
    }
    app.event_thread_started = true;
    fprintf(stderr, "mhcamera: event=ready\n");
    bridge_result = ainice_bridge_serve_until(XC_BRIDGE_MODULE,
                                              xc_bridge_handler, &app,
                                              xc_should_stop, &app);
    if (bridge_result != 0)
        fprintf(stderr, "mhcamera: event=bridge_exit result=error\n");

shutdown:
    xc_stopping = 1;
    if (app.event_thread_started) pthread_join(app.event_thread, NULL);
    xc_service_stop(app.service);
    if (app.child_started && !app.child.exited)
        (void)xc_rtsp_source(app.rtsp, false);
    xc_media_owner_stop(app.media);
    if (app.child_started && !app.child.exited)
        stop_result = xc_process_stop(&app.child, 5000u);
    if (stop_result != 0)
        fprintf(stderr, "mhcamera: event=go2rtc_stop result=timeout\n");
    xc_service_destroy(app.service);
    xc_media_owner_destroy(app.media);
    xc_rtsp_destroy(app.rtsp);
    return bridge_result == 0 && !app.child_failed && stop_result == 0 ? 0 : 1;
}
