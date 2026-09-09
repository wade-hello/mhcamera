#include "bridge.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ainice/protocol_error_codes.h"

#define XC_JSON_MAX_DEPTH 32u
#define XC_JSON_MAX_NODES 4096u

static void xc_clear(void *memory, size_t size)
{
    volatile unsigned char *bytes = (volatile unsigned char *)memory;

    while (size-- > 0u) *bytes++ = 0u;
}

static void xc_json_secure_clear(cJSON *item)
{
    cJSON *child;

    if (!item) return;
    for (child = item->child; child; child = child->next)
        xc_json_secure_clear(child);
    if (item->valuestring)
        xc_clear(item->valuestring, strlen(item->valuestring));
}

static void xc_json_secure_delete(cJSON *item)
{
    xc_json_secure_clear(item);
    cJSON_Delete(item);
}

static bool xc_json_tree_valid(const cJSON *item,
                               unsigned int depth,
                               unsigned int *nodes)
{
    const cJSON *child;

    if (!item || !nodes || depth > XC_JSON_MAX_DEPTH ||
        ++(*nodes) > XC_JSON_MAX_NODES) return false;
    if (cJSON_IsObject(item)) {
        for (child = item->child; child; child = child->next) {
            const cJSON *other;

            if (!child->string) return false;
            for (other = child->next; other; other = other->next) {
                if (!other->string || strcmp(child->string, other->string) == 0)
                    return false;
            }
            if (!xc_json_tree_valid(child, depth + 1u, nodes)) return false;
        }
    } else if (cJSON_IsArray(item)) {
        for (child = item->child; child; child = child->next) {
            if (!xc_json_tree_valid(child, depth + 1u, nodes)) return false;
        }
    }
    return true;
}

static cJSON *xc_json_parse_strict(const char *text)
{
    const char *end = NULL;
    cJSON *root;
    unsigned int nodes = 0u;

    if (!text || text[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }
    root = cJSON_ParseWithOpts(text, &end, 0);
    if (!root) {
        errno = EINVAL;
        return NULL;
    }
    while (end && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        end++;
    if (!end || *end != '\0' || !xc_json_tree_valid(root, 0u, &nodes)) {
        xc_json_secure_delete(root);
        errno = EINVAL;
        return NULL;
    }
    return root;
}

static bool xc_json_object_has_only(const cJSON *object,
                                    const char *const *keys,
                                    size_t key_count)
{
    const cJSON *item;

    if (!cJSON_IsObject(object)) return false;
    for (item = object->child; item; item = item->next) {
        size_t index;

        for (index = 0u; index < key_count; ++index) {
            if (strcmp(item->string, keys[index]) == 0) break;
        }
        if (index == key_count) return false;
    }
    return true;
}

static const char *xc_json_string(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);

    return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

static cJSON *xc_error(int code, const char *reason, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *error = cJSON_CreateObject();
    cJSON *details = cJSON_CreateObject();

    if (!root || !error || !details ||
        !cJSON_AddNumberToObject(error, "code", code) ||
        !cJSON_AddStringToObject(error, "message", message) ||
        !cJSON_AddStringToObject(details, "reason", reason)) {
        cJSON_Delete(details);
        cJSON_Delete(error);
        cJSON_Delete(root);
        errno = ENOMEM;
        return NULL;
    }
    if (!cJSON_AddItemToObject(error, "details", details)) goto fail;
    details = NULL;
    if (!cJSON_AddItemToObject(root, "error", error)) goto fail;
    error = NULL;
    return root;
fail:
    cJSON_Delete(details);
    cJSON_Delete(error);
    cJSON_Delete(root);
    errno = ENOMEM;
    return NULL;
}

static cJSON *xc_health(void)
{
    cJSON *root = cJSON_CreateObject();

    if (!root || !cJSON_AddStringToObject(root, "status", "ok")) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static cJSON *xc_modules(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *modules = cJSON_CreateObject();
    cJSON *camera = cJSON_CreateObject();

    if (!root || !modules || !camera ||
        !cJSON_AddBoolToObject(camera, "enabled", 1) ||
        !cJSON_AddNullToObject(camera, "error")) {
        cJSON_Delete(camera);
        cJSON_Delete(modules);
        cJSON_Delete(root);
        return NULL;
    }
    if (!cJSON_AddItemToObject(modules, "camera", camera)) goto fail_modules;
    camera = NULL;
    if (!cJSON_AddItemToObject(root, "modules", modules)) goto fail_modules;
    modules = NULL;
    return root;
fail_modules:
    cJSON_Delete(camera);
    cJSON_Delete(modules);
    cJSON_Delete(root);
    return NULL;
}

static bool xc_path_valid(const char *path)
{
    size_t index;
    size_t length;

    if (!path || path[0] != '/') return false;
    length = strlen(path);
    if (length == 0u || length > 128u || strstr(path, "//") ||
        strstr(path, "..")) return false;
    for (index = 0u; index < length; ++index) {
        unsigned char ch = (unsigned char)path[index];

        if ((ch >= 'a' && ch <= 'z') || ch == '/' || ch == '-') continue;
        return false;
    }
    return true;
}

int xc_bridge_dispatch_json(const char *request_json,
                            xc_route_handler_fn route_handler,
                            void *userdata,
                            char **response_json)
{
    static const char *const request_keys[] = {"module", "method", "params"};
    static const char *const route_keys[] = {
        "path", "http_method", "query", "body"
    };
    cJSON *request = NULL;
    cJSON *result = NULL;
    cJSON *empty_query = NULL;
    cJSON *empty_body = NULL;
    const cJSON *params;
    const cJSON *query;
    const cJSON *body;
    const char *module;
    const char *method;
    char *printed;

    if (!response_json) {
        errno = EINVAL;
        return -1;
    }
    *response_json = NULL;
    request = xc_json_parse_strict(request_json);
    if (!request || !cJSON_IsObject(request) ||
        !xc_json_object_has_only(request, request_keys,
                                 sizeof(request_keys) / sizeof(request_keys[0]))) {
        result = xc_error(AINICE_PROTOCOL_ERROR_BAD_REQUEST,
                          "bad_request", "invalid bridge request");
        goto finish;
    }
    module = xc_json_string(request, "module");
    method = xc_json_string(request, "method");
    params = cJSON_GetObjectItemCaseSensitive(request, "params");
    if (!module || strcmp(module, XC_BRIDGE_MODULE) != 0 || !method ||
        (params && !cJSON_IsObject(params))) {
        result = xc_error(AINICE_PROTOCOL_ERROR_BAD_REQUEST,
                          "bad_request", "invalid bridge request");
        goto finish;
    }
    if (strcmp(method, "health.get") == 0) {
        if (params && params->child) {
            result = xc_error(AINICE_PROTOCOL_ERROR_BRIDGE_PARAMS_INVALID,
                              "bridge_params_invalid",
                              "health params must be empty");
            goto finish;
        }
        result = xc_health();
        goto finish;
    }
    if (strcmp(method, "modules.status") == 0) {
        if (params && params->child) {
            result = xc_error(AINICE_PROTOCOL_ERROR_BRIDGE_PARAMS_INVALID,
                              "bridge_params_invalid",
                              "module params must be empty");
            goto finish;
        }
        result = xc_modules();
        goto finish;
    }
    if (strcmp(method, "route") != 0) {
        result = xc_error(AINICE_PROTOCOL_ERROR_UNSUPPORTED_BRIDGE_METHOD,
                          "unsupported_bridge_method",
                          "unsupported bridge method");
        goto finish;
    }
    if (!params ||
        !xc_json_object_has_only(params, route_keys,
                                 sizeof(route_keys) / sizeof(route_keys[0]))) {
        result = xc_error(AINICE_PROTOCOL_ERROR_BRIDGE_PARAMS_INVALID,
                          "bridge_params_invalid",
                          "route params are invalid");
        goto finish;
    }
    {
        struct xc_route_request route;

        route.path = xc_json_string(params, "path");
        route.http_method = xc_json_string(params, "http_method");
        query = cJSON_GetObjectItemCaseSensitive(params, "query");
        body = cJSON_GetObjectItemCaseSensitive(params, "body");
        if (!xc_path_valid(route.path)) {
            result = xc_error(AINICE_PROTOCOL_ERROR_ROUTE_PATH_INVALID,
                              "route_path_invalid",
                              "route path is invalid");
            goto finish;
        }
        if (!route.http_method ||
            (strcmp(route.http_method, "GET") != 0 &&
             strcmp(route.http_method, "POST") != 0)) {
            result = xc_error(AINICE_PROTOCOL_ERROR_ROUTE_METHOD_REQUIRED,
                              "route_method_required",
                              "route method must be GET or POST");
            goto finish;
        }
        if (query && !cJSON_IsObject(query)) {
            result = xc_error(AINICE_PROTOCOL_ERROR_ROUTE_QUERY_INVALID,
                              "route_query_invalid",
                              "route query must be an object");
            goto finish;
        }
        if (body && !cJSON_IsObject(body)) {
            result = xc_error(AINICE_PROTOCOL_ERROR_ROUTE_BODY_INVALID,
                              "route_body_invalid",
                              "route body must be an object");
            goto finish;
        }
        empty_query = query ? NULL : cJSON_CreateObject();
        empty_body = body ? NULL : cJSON_CreateObject();
        if ((!query && !empty_query) || (!body && !empty_body)) goto finish;
        route.query = query ? query : empty_query;
        route.body = body ? body : empty_body;
        result = route_handler ? route_handler(&route, userdata) : NULL;
        if (!result) {
            result = xc_error(AINICE_PROTOCOL_ERROR_ROUTE_NOT_FOUND,
                              "route_not_found", "route not found");
        }
    }

finish:
    cJSON_Delete(empty_query);
    cJSON_Delete(empty_body);
    if (!result) {
        result = xc_error(AINICE_PROTOCOL_ERROR_NO_MEMORY,
                          "no_memory", "out of memory");
    }
    printed = result ? cJSON_PrintUnformatted(result) : NULL;
    xc_json_secure_delete(result);
    xc_json_secure_delete(request);
    if (!printed) return -1;
    *response_json = printed;
    return 0;
}
