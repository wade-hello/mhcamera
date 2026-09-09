#include "host_input.h"

#include <ainice/api.h>

#include <cjson/cJSON.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define XC_HOST_API_TIMEOUT_MS 5000

static void xc_host_secure_clear(void *memory, size_t size)
{
    volatile unsigned char *bytes = memory;

    while (size-- > 0u) *bytes++ = 0u;
}

static void xc_host_json_secure_clear(cJSON *item)
{
    cJSON *child;

    if (!item) return;
    for (child = item->child; child; child = child->next)
        xc_host_json_secure_clear(child);
    if (item->valuestring)
        xc_host_secure_clear(item->valuestring, strlen(item->valuestring));
}

static void xc_host_json_secure_delete(cJSON *item)
{
    xc_host_json_secure_clear(item);
    cJSON_Delete(item);
}

static cJSON *xc_host_json_parse(const char *text)
{
    const char *end = NULL;
    cJSON *root;

    if (!text || text[0] == '\0') {
        errno = EPROTO;
        return NULL;
    }
    root = cJSON_ParseWithOpts(text, &end, 0);
    if (!root) {
        errno = EPROTO;
        return NULL;
    }
    while (end && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        end++;
    if (!end || *end != '\0' || !cJSON_IsObject(root)) {
        xc_host_json_secure_delete(root);
        errno = EPROTO;
        return NULL;
    }
    return root;
}

static cJSON *xc_host_api_call(const char *method, cJSON *params)
{
    cJSON *request = NULL;
    char *request_text = NULL;
    char *response_text = NULL;
    cJSON *response = NULL;
    int saved;

    request = cJSON_CreateObject();
    if (!params) params = cJSON_CreateObject();
    if (!request || !params ||
        !cJSON_AddStringToObject(request, "id", "mhcamera-input") ||
        !cJSON_AddStringToObject(request, "method", method) ||
        !cJSON_AddItemToObject(request, "params", params)) {
        cJSON_Delete(params);
        cJSON_Delete(request);
        errno = ENOMEM;
        return NULL;
    }
    params = NULL;
    request_text = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!request_text) {
        errno = ENOMEM;
        return NULL;
    }
    if (ainice_api_call_json(request_text, XC_HOST_API_TIMEOUT_MS,
                             &response_text) != 0 || !response_text) {
        saved = errno ? errno : EIO;
        xc_host_secure_clear(request_text, strlen(request_text));
        cJSON_free(request_text);
        if (response_text) {
            xc_host_secure_clear(response_text, strlen(response_text));
            ainice_api_response_free(response_text);
        }
        errno = saved;
        return NULL;
    }
    xc_host_secure_clear(request_text, strlen(request_text));
    cJSON_free(request_text);
    response = xc_host_json_parse(response_text);
    saved = errno;
    xc_host_secure_clear(response_text, strlen(response_text));
    ainice_api_response_free(response_text);
    if (!response) errno = saved;
    return response;
}

static cJSON *xc_host_api_result(cJSON *response)
{
    cJSON *ok;
    cJSON *result;

    if (!cJSON_IsObject(response)) return NULL;
    ok = cJSON_GetObjectItemCaseSensitive(response, "ok");
    result = cJSON_GetObjectItemCaseSensitive(response, "result");
    return cJSON_IsTrue(ok) && cJSON_IsObject(result) ? result : NULL;
}

int xc_host_input_is_bitstream(bool *bitstream_out)
{
    cJSON *response;
    cJSON *result;
    cJSON *input;
    cJSON *mode;

    if (!bitstream_out) {
        errno = EINVAL;
        return -1;
    }

    response = xc_host_api_call("config.get", NULL);
    result = xc_host_api_result(response);
    input = result ? cJSON_GetObjectItemCaseSensitive(result, "input") : NULL;
    mode = input ? cJSON_GetObjectItemCaseSensitive(input, "mode") : NULL;
    if (!cJSON_IsString(mode) || !mode->valuestring) {
        xc_host_json_secure_delete(response);
        errno = EPROTO;
        return -1;
    }
    *bitstream_out = strcmp(mode->valuestring, "bitstream") == 0;
    xc_host_json_secure_delete(response);
    return 0;
}

int xc_host_input_ensure_bitstream(bool (*patch_allowed)(void *userdata),
                                   void *guard_userdata)
{
    bool bitstream;
    cJSON *response;
    cJSON *params;

    if (!patch_allowed) {
        errno = EINVAL;
        return -1;
    }
    if (xc_host_input_is_bitstream(&bitstream) != 0) return -1;
    if (bitstream) return 0;

    params = cJSON_CreateObject();
    if (!params ||
        !cJSON_AddStringToObject(params, "key", "input.mode") ||
        !cJSON_AddStringToObject(params, "value", "bitstream")) {
        cJSON_Delete(params);
        errno = ENOMEM;
        return -1;
    }
    if (!patch_allowed(guard_userdata)) {
        cJSON_Delete(params);
        errno = ECANCELED;
        return -1;
    }
    response = xc_host_api_call("config.patch", params);
    if (!xc_host_api_result(response)) {
        xc_host_json_secure_delete(response);
        errno = EIO;
        return -1;
    }
    xc_host_json_secure_delete(response);
    return 0;
}
