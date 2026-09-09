#ifndef MHCAMERA_RTSP_H
#define MHCAMERA_RTSP_H

#include "bridge.h"
#include "source.h"
#include <stdbool.h>

struct xc_rtsp;
enum xc_rtsp_listener_state { XC_RTSP_LISTENER_UNKNOWN = 0, XC_RTSP_LISTENER_STOPPED, XC_RTSP_LISTENER_RUNNING };
enum xc_rtsp_status { XC_RTSP_DISABLED = 0, XC_RTSP_WAITING_SOURCE, XC_RTSP_RUNNING, XC_RTSP_ERROR };
enum xc_rtsp_error {
    XC_RTSP_ERROR_NONE = 0, XC_RTSP_ERROR_CONTROL, XC_RTSP_ERROR_PERSIST,
    XC_RTSP_ERROR_NOT_DURABLE, XC_RTSP_ERROR_ROLLBACK, XC_RTSP_ERROR_RECONNECT,
};
const char *xc_rtsp_error_text(enum xc_rtsp_error error);
struct xc_rtsp_preferences {
    bool enabled;
    bool audio_enabled;
};
struct xc_rtsp_state {
    enum xc_rtsp_listener_state listener;
    bool audio_supported;
    bool audio_known;
    char source[512];
    enum xc_source_state source_state;
    enum xc_source_error source_error;
};
struct xc_rtsp_ops {
    int (*control)(void *userdata, const bool *enabled, const char *password,
                    struct xc_rtsp_state *state);
    int (*save)(const char *path, const void *data, size_t size);
    void *userdata;
};

int xc_rtsp_create(struct xc_rtsp **out, const char *data_root,
                    const struct xc_rtsp_ops *ops);
/* The source coordinator owns availability. This never starts/stops AIV1. */
int xc_rtsp_source(struct xc_rtsp *rtsp, bool available);
void xc_rtsp_preferences(struct xc_rtsp *rtsp, struct xc_rtsp_preferences *out);
bool xc_rtsp_effective_audio(struct xc_rtsp *rtsp);
void xc_rtsp_error(struct xc_rtsp *rtsp, enum xc_rtsp_error error);
cJSON *xc_rtsp_read(struct xc_rtsp *rtsp, struct xc_rtsp_state *state);
void xc_rtsp_destroy(struct xc_rtsp *rtsp);
cJSON *xc_rtsp_route(struct xc_rtsp *rtsp, const struct xc_route_request *request);

#endif
