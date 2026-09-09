#ifndef MHCAMERA_BRIDGE_H
#define MHCAMERA_BRIDGE_H

#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XC_BRIDGE_MODULE "mhcamera"

struct xc_route_request {
    const char *http_method;
    const char *path;
    const cJSON *query;
    const cJSON *body;
};

typedef cJSON *(*xc_route_handler_fn)(const struct xc_route_request *request,
                                      void *userdata);

/* Parses one complete, duplicate-key-free Bridge request and returns one
 * complete JSON response. The caller owns *response_json via cJSON_free(). */
int xc_bridge_dispatch_json(const char *request_json,
                            xc_route_handler_fn route_handler,
                            void *userdata,
                            char **response_json);

#ifdef __cplusplus
}
#endif

#endif
