#include "source.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define XC_SOURCE_PREFIX "xiaomi://"

static const char *const source_states[XC_SOURCE_STATE_COUNT] = {
    "error", "stopped", "starting", "running", "retrying", "stopping", "error"
};
static const char *const source_errors[XC_SOURCE_ERROR_COUNT] = {
    NULL, "source_dial_failed", "source_read_failed", "audio_unavailable", "credential_rejected"
};
const char *xc_source_state_text(enum xc_source_state state)
{
    return state >= XC_SOURCE_UNKNOWN && state < XC_SOURCE_STATE_COUNT ? source_states[state] : NULL;
}
int xc_source_state_parse(const char *text, enum xc_source_state *state)
{
    enum xc_source_state value;
    if (!text || !state) return -1;
    for (value = XC_SOURCE_STOPPED; value < XC_SOURCE_STATE_COUNT; ++value)
        if (!strcmp(text, source_states[value])) { *state = value; return 0; }
    return -1;
}
const char *xc_source_error_text(enum xc_source_error error)
{
    return error >= XC_SOURCE_ERROR_NONE && error < XC_SOURCE_ERROR_COUNT ? source_errors[error] : NULL;
}
int xc_source_error_parse(const char *text, enum xc_source_error *error)
{
    enum xc_source_error value;
    if (!error) return -1;
    if (!text) { *error = XC_SOURCE_ERROR_NONE; return 0; }
    for (value = XC_SOURCE_ERROR_DIAL; value < XC_SOURCE_ERROR_COUNT; ++value)
        if (!strcmp(text, source_errors[value])) { *error = value; return 0; }
    return -1;
}

const char *xc_region_text(enum xc_region region)
{
    static const char *const names[XC_REGION_COUNT] = {
        "cn", "de", "i2", "ru", "sg", "us"
    };

    return region >= XC_REGION_CN && region < XC_REGION_COUNT ? names[region] : NULL;
}

int xc_region_parse(const char *text, enum xc_region *region_out)
{
    enum xc_region region;

    if (!text || !region_out) {
        errno = EINVAL;
        return -1;
    }
    for (region = XC_REGION_CN; region < XC_REGION_COUNT; ++region) {
        if (strcmp(text, xc_region_text(region)) == 0) {
            *region_out = region;
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}

static bool xc_region_valid(const char *region)
{
    enum xc_region parsed;

    return xc_region_parse(region, &parsed) == 0;
}

static bool xc_identity_char(unsigned char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
}

static bool xc_identity_valid(const char *text, size_t maximum)
{
    size_t index;
    size_t length;

    if (!text || text[0] == '\0') return false;
    length = strnlen(text, maximum + 1u);
    if (length == 0u || length > maximum) return false;
    for (index = 0u; index < length; ++index) {
        if (!xc_identity_char((unsigned char)text[index])) return false;
    }
    return true;
}

static bool xc_decimal_id(const char *text, size_t maximum)
{
    size_t index;
    size_t length;

    if (!text || !(length = strnlen(text, maximum + 1u)) || length > maximum)
        return false;
    for (index = 0u; index < length; ++index)
        if (text[index] < '0' || text[index] > '9') return false;
    return true;
}

static int xc_copy_range(char *output,
                         size_t output_size,
                         const char *begin,
                         const char *end)
{
    size_t length;

    if (!output || output_size == 0u || !begin || !end || end <= begin) {
        errno = EINVAL;
        return -1;
    }
    length = (size_t)(end - begin);
    if (length >= output_size) {
        errno = EINVAL;
        return -1;
    }
    memcpy(output, begin, length);
    output[length] = '\0';
    return 0;
}

static bool xc_range_equals(const char *begin, const char *end, const char *text)
{
    size_t length;

    if (!begin || !end || end < begin || !text) return false;
    length = (size_t)(end - begin);
    return strlen(text) == length && memcmp(begin, text, length) == 0;
}

static int xc_query_identity(const char *source, const char *key,
                             char *output, size_t output_size)
{
    const char *cursor = strchr(source, '?');
    bool found = false;

    if (!cursor || !output || output_size == 0u) goto invalid;
    output[0] = '\0';
    cursor++;
    while (*cursor) {
        const char *end = strchr(cursor, '&');
        const char *equals;

        if (!end) end = cursor + strlen(cursor);
        equals = memchr(cursor, '=', (size_t)(end - cursor));
        if (!equals || equals == cursor || equals + 1 == end) goto invalid;
        if (xc_range_equals(cursor, equals, key)) {
            if (found || xc_copy_range(output, output_size, equals + 1, end) != 0)
                goto invalid;
            found = true;
        }
        cursor = *end ? end + 1 : end;
    }
    if (!found) goto invalid;
    return 0;
invalid:
    if (output && output_size) output[0] = '\0';
    errno = EINVAL;
    return -1;
}

static bool xc_ipv4_allowed(const char *host, char canonical[INET_ADDRSTRLEN])
{
    struct in_addr address;
    uint32_t value;

    if (inet_pton(AF_INET, host, &address) != 1 ||
        !inet_ntop(AF_INET, &address, canonical, INET_ADDRSTRLEN)) return false;
    value = ntohl(address.s_addr);
    if (value == 0u || value == UINT32_MAX ||
        (value >> 24) == 127u || (value >> 28) == 14u) return false;
    return true;
}

int xc_camera_source_rebuild(const char *core_source,
                             const char *expected_account_id,
                             const char *expected_region,
                             const char *expected_camera_id,
                             const char *expected_model,
                             unsigned int channel,
                             bool audio_enabled,
                             char *output,
                             size_t output_size)
{
    char account[65];
    char region[3];
    char host[INET_ADDRSTRLEN];
    char canonical_host[INET_ADDRSTRLEN];
    char did[65] = {0};
    char model[129] = {0};
    const char *authority;
    const char *at;
    const char *colon;
    const char *query;
    const char *cursor;
    int written;

    if (output && output_size) output[0] = '\0';
    if (!core_source || !output || output_size == 0u ||
        !xc_decimal_id(expected_account_id, 64u) ||
        !xc_region_valid(expected_region) ||
        !xc_decimal_id(expected_camera_id, 64u) ||
        !xc_identity_valid(expected_model, 128u) ||
        (channel != 1u && channel != 2u) ||
        strncmp(core_source, XC_SOURCE_PREFIX, strlen(XC_SOURCE_PREFIX)) != 0 ||
        strchr(core_source, '#')) {
        errno = EINVAL;
        return -1;
    }
    authority = core_source + strlen(XC_SOURCE_PREFIX);
    query = strchr(authority, '?');
    at = strchr(authority, '@');
    colon = strchr(authority, ':');
    if (!query || strchr(query + 1, '?') || !at || at > query ||
        strchr(at + 1, '@') || !colon || colon > at || strchr(colon + 1, ':')) {
        errno = EINVAL;
        return -1;
    }
    if (xc_copy_range(account, sizeof(account), authority, colon) != 0 ||
        xc_copy_range(region, sizeof(region), colon + 1, at) != 0 ||
        xc_copy_range(host, sizeof(host), at + 1, query) != 0 ||
        !xc_decimal_id(account, 64u) || !xc_region_valid(region) ||
        !xc_ipv4_allowed(host, canonical_host) ||
        strcmp(account, expected_account_id) != 0 ||
        strcmp(region, expected_region) != 0) {
        errno = EPERM;
        return -1;
    }
    cursor = query + 1;
    while (*cursor) {
        const char *end = strchr(cursor, '&');
        const char *equals;

        if (!end) end = cursor + strlen(cursor);
        equals = memchr(cursor, '=', (size_t)(end - cursor));
        if (!equals || equals == cursor || equals + 1 == end) goto invalid;
        if (xc_range_equals(cursor, equals, "did")) {
            if (did[0] != '\0' ||
                xc_copy_range(did, sizeof(did), equals + 1, end) != 0)
                goto invalid;
        } else if (xc_range_equals(cursor, equals, "model")) {
            if (model[0] != '\0' ||
                xc_copy_range(model, sizeof(model), equals + 1, end) != 0)
                goto invalid;
        } else goto invalid;
        cursor = *end ? end + 1 : end;
    }
    if (!xc_decimal_id(did, 64u) || !xc_identity_valid(model, 128u) ||
        strcmp(did, expected_camera_id) != 0 ||
        strcmp(model, expected_model) != 0)
        goto invalid;
    written = snprintf(output, output_size,
                       channel == 1u ?
                       "xiaomi://%s:%s@%s?did=%s&model=%s&subtype=1&audio=%u&transport=tcp" :
                       "xiaomi://%s:%s@%s?did=%s&model=%s&channel=2&subtype=1&audio=%u&transport=tcp",
                       account, region, canonical_host, did, model, audio_enabled ? 1u : 0u);
    if (written < 0 || (size_t)written >= output_size) {
        output[0] = '\0';
        errno = ENOSPC;
        return -1;
    }
    return 0;

invalid:
    output[0] = '\0';
    errno = EINVAL;
    return -1;
}

int xc_camera_source_identify(const char *core_source,
                              const char *expected_account_id,
                              const char *expected_region,
                              char *camera_id,
                              size_t camera_id_size,
                              char *model,
                              size_t model_size)
{
    char canonical[512];

    if (!camera_id || !model ||
        xc_query_identity(core_source, "did", camera_id, camera_id_size) != 0 ||
        xc_query_identity(core_source, "model", model, model_size) != 0 ||
        !xc_decimal_id(camera_id, 64u) || !xc_identity_valid(model, 128u) ||
        xc_camera_source_rebuild(core_source, expected_account_id,
                                 expected_region, camera_id, model,
                                 1u, false,
                                 canonical, sizeof(canonical)) != 0) {
        if (camera_id && camera_id_size) camera_id[0] = '\0';
        if (model && model_size) model[0] = '\0';
        return -1;
    }
    return 0;
}
