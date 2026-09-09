#define _GNU_SOURCE
#include "store.h"
#include "source.h"

#include <cjson/cJSON.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int xc_safe_path(const char *path)
{
    const unsigned char *cursor = (const unsigned char *)path;

    if (!path || path[0] != '/' || strstr(path, "/../") ||
        (strlen(path) >= 3u && strcmp(path + strlen(path) - 3u, "/..") == 0) ||
        strcmp(path, "/") == 0) return -1;
    for (; *cursor; ++cursor) {
        if ((*cursor >= 'a' && *cursor <= 'z') ||
            (*cursor >= 'A' && *cursor <= 'Z') ||
            (*cursor >= '0' && *cursor <= '9') ||
            *cursor == '/' || *cursor == '.' || *cursor == '_' || *cursor == '-')
            continue;
        return -1;
    }
    return 0;
}

static int xc_secure_directory(const char *path)
{
    struct stat metadata;

    if (lstat(path, &metadata) != 0) {
        if (errno != ENOENT || mkdir(path, 0700) != 0) return -1;
        if (lstat(path, &metadata) != 0) return -1;
    }
    if (!S_ISDIR(metadata.st_mode) || metadata.st_uid != geteuid()) {
        errno = EPERM;
        return -1;
    }
    if ((metadata.st_mode & 0777) != 0700 && chmod(path, 0700) != 0) return -1;
    return 0;
}

int xc_store_atomic_write(const char *path, const void *data, size_t size)
{
    char directory[PATH_MAX];
    char temporary[PATH_MAX] = {0};
    char *slash;
    int dir_fd = -1;
    int file_fd = -1;
    int result = -1;
    size_t offset = 0u;
    unsigned int attempt;

    if (!path || !data || xc_safe_path(path) != 0 || strlen(path) >= PATH_MAX) {
        errno = EINVAL;
        return -1;
    }
    snprintf(directory, sizeof(directory), "%s", path);
    slash = strrchr(directory, '/');
    if (!slash || slash == directory) {
        errno = EINVAL;
        return -1;
    }
    *slash = '\0';
    dir_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir_fd < 0) goto done;
    for (attempt = 0u; attempt < 100u; ++attempt) {
        int length = snprintf(temporary, sizeof(temporary), ".xc-state.%ld.%u.tmp",
                              (long)getpid(), attempt);

        if (length < 0 || (size_t)length >= sizeof(temporary)) {
            errno = ENAMETOOLONG;
            goto done;
        }
        file_fd = openat(dir_fd, temporary,
                         O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                         0600);
        if (file_fd >= 0 || errno != EEXIST) break;
    }
    if (file_fd < 0) goto done;
    while (offset < size) {
        ssize_t wrote = write(file_fd, (const unsigned char *)data + offset,
                              size - offset);

        if (wrote > 0) offset += (size_t)wrote;
        else if (wrote < 0 && errno == EINTR) continue;
        else goto done;
    }
    if (fchmod(file_fd, 0600) != 0 || fsync(file_fd) != 0) goto done;
    if (close(file_fd) != 0) {
        file_fd = -1;
        goto done;
    }
    file_fd = -1;
    if (renameat(dir_fd, temporary, dir_fd, slash + 1) != 0) goto done;
    if (fsync(dir_fd) != 0) result = XC_STORE_COMMITTED_NOT_DURABLE;
    else result = XC_STORE_COMMITTED_DURABLE;
done:
    {
        int saved = errno;
        if (file_fd >= 0) close(file_fd);
        if (result == XC_STORE_FAILED && dir_fd >= 0 && temporary[0])
            (void)unlinkat(dir_fd, temporary, 0);
        if (dir_fd >= 0) close(dir_fd);
        errno = saved;
    }
    return result;
}

static int xc_config_ensure(const char *path, const char *socket_path)
{
    struct stat metadata;
    char config[512];
    int length;

    length = snprintf(config, sizeof(config),
                      "api:\n  listen: \"\"\n  unix_listen: %s\n"
                      "  allow_paths:\n    - /api/xiaomi\n    - /api/camera/source\n"
                      "    - /api/xiaomi-phone\n    - /api/camera/rtsp\n"
                      "xiaomi:\n", socket_path);
    if (length < 0 || (size_t)length >= sizeof(config)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (lstat(path, &metadata) == 0) {
        if (!S_ISREG(metadata.st_mode) || metadata.st_uid != geteuid() ||
            (metadata.st_mode & 0777) != 0600) {
            errno = EPERM;
            return -1;
        }
        return 0;
    }
    if (errno != ENOENT) return -1;
    {
        int result = xc_store_atomic_write(path, config, (size_t)length);

        return xc_store_result_committed(result) ? 0 : -1;
    }
}

int xc_store_clear_xiaomi_token(const char *config_path, const char *api_socket_path)
{
    char config[512];
    int length;
    int result;

    if (xc_safe_path(config_path) != 0 || !api_socket_path ||
        api_socket_path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    length = snprintf(config, sizeof(config),
                      "api:\n  listen: \"\"\n  unix_listen: %s\n"
                      "  allow_paths:\n    - /api/xiaomi\n    - /api/camera/source\n"
                      "    - /api/xiaomi-phone\n    - /api/camera/rtsp\n"
                      "xiaomi:\n", api_socket_path);
    if (length < 0 || (size_t)length >= sizeof(config)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    result = xc_store_atomic_write(config_path, config, (size_t)length);
    return xc_store_result_committed(result) ? 0 : -1;
}

int xc_store_prepare(const char *data_root,
                     char *config_path,
                     size_t config_path_size,
                     char *socket_path,
                     size_t socket_path_size,
                     char *status_path,
                     size_t status_path_size)
{
    char tmp_path[PATH_MAX];
    int config_length;
    int socket_length;
    int status_length;

    if (!config_path || !socket_path || !status_path ||
        xc_safe_path(data_root) != 0) {
        errno = EINVAL;
        return -1;
    }
    (void)umask(0077);
    if (xc_secure_directory(data_root) != 0) return -1;
    if (snprintf(tmp_path, sizeof(tmp_path), "%s/tmp", data_root) >=
        (int)sizeof(tmp_path) || xc_secure_directory(tmp_path) != 0) return -1;
    config_length = snprintf(config_path, config_path_size, "%s/go2rtc.yaml", data_root);
    socket_length = snprintf(socket_path, socket_path_size,
                             "%s/go2rtc-api.sock", tmp_path);
    status_length = snprintf(status_path, status_path_size, "%s/status.json", data_root);
    if (config_length < 0 || socket_length < 0 || status_length < 0 ||
        (size_t)config_length >= config_path_size ||
        (size_t)socket_length >= socket_path_size ||
        (size_t)status_length >= status_path_size ||
        strlen(socket_path) >= 108u) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return xc_config_ensure(config_path, socket_path);
}

int xc_store_selection_path(const char *data_root, char *out, size_t out_size)
{
    int length;

    if (!out || xc_safe_path(data_root) != 0) {
        errno = EINVAL;
        return -1;
    }
    length = snprintf(out, out_size, "%s/selection.json", data_root);
    if (length < 0 || (size_t)length >= out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int xc_store_phone_rate_path(const char *data_root, char *out, size_t out_size)
{
    int length;

    if (!out || xc_safe_path(data_root) != 0) {
        errno = EINVAL;
        return -1;
    }
    length = snprintf(out, out_size, "%s/phone-rate-limit.json", data_root);
    if (length < 0 || (size_t)length >= out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static bool xc_phone_rate_object_exact(const cJSON *root)
{
    static const char *const keys[] = {"version", "retry_after_wall_ms"};
    const cJSON *item;
    unsigned int count = 0u;

    if (!cJSON_IsObject(root)) return false;
    for (item = root->child; item; item = item->next) {
        size_t index;
        const cJSON *other;

        if (!item->string) return false;
        for (other = item->next; other; other = other->next)
            if (other->string && strcmp(item->string, other->string) == 0)
                return false;
        for (index = 0u; index < sizeof(keys) / sizeof(keys[0]); ++index)
            if (strcmp(item->string, keys[index]) == 0) break;
        if (index == sizeof(keys) / sizeof(keys[0])) return false;
        count++;
    }
    return count == sizeof(keys) / sizeof(keys[0]);
}

int xc_store_phone_rate_read(const char *path, uint64_t *retry_after_wall_ms)
{
    struct stat metadata;
    char data[257];
    const char *end = NULL;
    cJSON *root = NULL;
    const cJSON *version;
    const cJSON *deadline;
    int fd = -1;
    ssize_t used = 0;
    int result = -1;

    if (!retry_after_wall_ms || xc_safe_path(path) != 0) {
        errno = EINVAL;
        return -1;
    }
    *retry_after_wall_ms = 0u;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) return 0;
    if (fd < 0 || fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_uid != geteuid() || (metadata.st_mode & 0777) != 0600 ||
        metadata.st_size < 0 || metadata.st_size >= (off_t)sizeof(data)) {
        errno = EPERM;
        goto done;
    }
    while ((size_t)used < sizeof(data) - 1u) {
        ssize_t got = read(fd, data + used, sizeof(data) - 1u - (size_t)used);

        if (got > 0) used += got;
        else if (got == 0) break;
        else if (errno == EINTR) continue;
        else goto done;
    }
    data[used] = '\0';
    root = cJSON_ParseWithOpts(data, &end, 0);
    while (end && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        end++;
    version = root ? cJSON_GetObjectItemCaseSensitive(root, "version") : NULL;
    deadline = root ? cJSON_GetObjectItemCaseSensitive(root,
                                                        "retry_after_wall_ms") : NULL;
    if (!root || !end || *end || !xc_phone_rate_object_exact(root) ||
        !cJSON_IsNumber(version) || version->valuedouble != 1.0 ||
        !cJSON_IsNumber(deadline) || deadline->valuedouble < 0.0 ||
        deadline->valuedouble > 9007199254740991.0 ||
        deadline->valuedouble != (double)(uint64_t)deadline->valuedouble) {
        errno = EPROTO;
        goto done;
    }
    *retry_after_wall_ms = (uint64_t)deadline->valuedouble;
    result = 0;
done:
    {
        int saved = errno;
        if (root) cJSON_Delete(root);
        if (fd >= 0) close(fd);
        memset(data, 0, sizeof(data));
        errno = saved;
    }
    return result;
}

int xc_store_phone_rate_write(const char *path, uint64_t retry_after_wall_ms)
{
    cJSON *root;
    char *json;
    int result;

    if (xc_safe_path(path) != 0 || retry_after_wall_ms > UINT64_C(9007199254740991)) {
        errno = EINVAL;
        return -1;
    }
    root = cJSON_CreateObject();
    if (!root ||
        !cJSON_AddNumberToObject(root, "version", 1.0) ||
        !cJSON_AddNumberToObject(root, "retry_after_wall_ms",
                                 (double)retry_after_wall_ms)) {
        cJSON_Delete(root);
        return -1;
    }
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return -1;
    result = xc_store_atomic_write(path, json, strlen(json));
    memset(json, 0, strlen(json));
    cJSON_free(json);
    return result;
}

static bool xc_selection_object_exact(const cJSON *root, bool *has_channel)
{
    static const char *const keys[] = {
        "version", "account_id", "region", "selected", "enabled", "id",
        "name", "model", "channel"
    };
    const cJSON *item;
    unsigned int count = 0u;
    bool channel_found = false;

    if (!cJSON_IsObject(root) || !has_channel) return false;
    for (item = root->child; item; item = item->next) {
        size_t index;
        const cJSON *other;

        for (other = item->next; other; other = other->next) {
            if (item->string && other->string &&
                strcmp(item->string, other->string) == 0) return false;
        }
        for (index = 0u; index < sizeof(keys) / sizeof(keys[0]); ++index) {
            if (item->string && strcmp(item->string, keys[index]) == 0) break;
        }
        if (index == sizeof(keys) / sizeof(keys[0])) return false;
        if (strcmp(item->string, "channel") == 0) channel_found = true;
        count++;
    }
    if ((count == sizeof(keys) / sizeof(keys[0]) - 1u && channel_found) ||
        (count == sizeof(keys) / sizeof(keys[0]) && !channel_found)) return false;
    *has_channel = channel_found;
    return count == sizeof(keys) / sizeof(keys[0]) - 1u ||
           count == sizeof(keys) / sizeof(keys[0]);
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

int xc_store_selection_read(const char *path,
                            struct xc_saved_selection *selection)
{
    struct stat metadata;
    char data[4097];
    const char *end = NULL;
    cJSON *root = NULL;
    cJSON *version;
    cJSON *account_id;
    cJSON *region;
    cJSON *selected;
    cJSON *enabled;
    cJSON *id;
    cJSON *name;
    cJSON *model;
    cJSON *channel;
    bool has_channel;
    int fd = -1;
    ssize_t used = 0;
    int result = -1;

    if (!selection || xc_safe_path(path) != 0) {
        errno = EINVAL;
        return -1;
    }
    memset(selection, 0, sizeof(*selection));
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) return 0;
    if (fd < 0 || fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_uid != geteuid() || (metadata.st_mode & 0777) != 0600 ||
        metadata.st_size < 0 || metadata.st_size > 4096) {
        errno = EPERM;
        goto done;
    }
    while ((size_t)used < sizeof(data) - 1u) {
        ssize_t got = read(fd, data + used, sizeof(data) - 1u - (size_t)used);

        if (got > 0) used += got;
        else if (got == 0) break;
        else if (errno == EINTR) continue;
        else goto done;
    }
    data[used] = '\0';
    root = cJSON_ParseWithOpts(data, &end, 0);
    while (end && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        end++;
    if (!root || !end || *end || !xc_selection_object_exact(root, &has_channel)) goto protocol;
    version = cJSON_GetObjectItemCaseSensitive(root, "version");
    account_id = cJSON_GetObjectItemCaseSensitive(root, "account_id");
    region = cJSON_GetObjectItemCaseSensitive(root, "region");
    selected = cJSON_GetObjectItemCaseSensitive(root, "selected");
    enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    id = cJSON_GetObjectItemCaseSensitive(root, "id");
    name = cJSON_GetObjectItemCaseSensitive(root, "name");
    model = cJSON_GetObjectItemCaseSensitive(root, "model");
    channel = cJSON_GetObjectItemCaseSensitive(root, "channel");
    {
        enum xc_region parsed_region;

        if (!cJSON_IsNumber(version) || version->valuedouble != 2.0 ||
            !cJSON_IsString(account_id) || !account_id->valuestring ||
            !xc_decimal_id(account_id->valuestring, 64u, false) ||
            !cJSON_IsString(region) || !region->valuestring ||
            xc_region_parse(region->valuestring, &parsed_region) != 0 ||
            (!cJSON_IsTrue(selected) && !cJSON_IsFalse(selected)) ||
            (!cJSON_IsTrue(enabled) && !cJSON_IsFalse(enabled)) ||
            !cJSON_IsString(id) || !id->valuestring ||
            strlen(id->valuestring) >= 65u ||
            !cJSON_IsString(name) || !name->valuestring ||
            strlen(name->valuestring) >= 128u ||
            !cJSON_IsString(model) || !model->valuestring ||
            strlen(model->valuestring) >= 128u ||
            (has_channel && (!cJSON_IsNumber(channel) ||
                             channel->valuedouble != (double)channel->valueint ||
                             channel->valueint != 2)) ||
            (cJSON_IsTrue(enabled) && !cJSON_IsTrue(selected)) ||
            (cJSON_IsTrue(selected) &&
             (!xc_decimal_id(id->valuestring, 64u, false) ||
              !model->valuestring[0])) ||
            (!cJSON_IsTrue(selected) && has_channel) ||
            (!cJSON_IsTrue(selected) &&
             (id->valuestring[0] || name->valuestring[0] ||
              model->valuestring[0])))
            goto protocol;
    }
    snprintf(selection->account_id, sizeof(selection->account_id), "%s",
             account_id->valuestring);
    snprintf(selection->region, sizeof(selection->region), "%s",
             region->valuestring);
    selection->configured = true;
    selection->exists = cJSON_IsTrue(selected);
    selection->enabled = cJSON_IsTrue(enabled);
    snprintf(selection->id, sizeof(selection->id), "%s", id->valuestring);
    snprintf(selection->name, sizeof(selection->name), "%s", name->valuestring);
    snprintf(selection->model, sizeof(selection->model), "%s", model->valuestring);
    selection->channel = has_channel ? 2u : 0u;
    result = 0;
    goto done;
protocol:
    errno = EPROTO;
done:
    {
        int saved = errno;
        if (root) cJSON_Delete(root);
        if (fd >= 0) close(fd);
        memset(data, 0, sizeof(data));
        errno = saved;
    }
    return result;
}

int xc_store_selection_write(const char *path,
                             const struct xc_saved_selection *selection)
{
    cJSON *root;
    char *json;
    int result;

    {
        enum xc_region parsed_region;

        if (!selection || !selection->configured ||
            !xc_decimal_id(selection->account_id, 64u, false) ||
            xc_region_parse(selection->region, &parsed_region) != 0 ||
            (selection->enabled && !selection->exists) ||
            (selection->exists &&
             (!xc_decimal_id(selection->id, 64u, false) ||
              !selection->model[0])) ||
            (!selection->exists &&
             (selection->id[0] || selection->name[0] ||
              selection->model[0] || selection->channel != 0u)) ||
            strnlen(selection->name, sizeof(selection->name)) >=
                sizeof(selection->name) ||
            strnlen(selection->id, sizeof(selection->id)) >=
                sizeof(selection->id) ||
            strnlen(selection->model, sizeof(selection->model)) >=
                sizeof(selection->model) ||
            (selection->channel != 0u && selection->channel != 2u)) {
            errno = EINVAL;
            return -1;
        }
    }
    root = cJSON_CreateObject();
    if (!root || !cJSON_AddNumberToObject(root, "version", 2) ||
        !cJSON_AddStringToObject(root, "account_id", selection->account_id) ||
        !cJSON_AddStringToObject(root, "region", selection->region) ||
        !cJSON_AddBoolToObject(root, "selected", selection->exists) ||
        !cJSON_AddBoolToObject(root, "enabled", selection->enabled) ||
        !cJSON_AddStringToObject(root, "id", selection->exists ? selection->id : "") ||
        !cJSON_AddStringToObject(root, "name", selection->exists ? selection->name : "") ||
        !cJSON_AddStringToObject(root, "model", selection->exists ? selection->model : "") ||
        (selection->exists && selection->channel == 2u &&
         !cJSON_AddNumberToObject(root, "channel", 2.0))) {
        cJSON_Delete(root);
        return -1;
    }
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return -1;
    result = xc_store_atomic_write(path, json, strlen(json));
    memset(json, 0, strlen(json));
    cJSON_free(json);
    return result;
}

int xc_store_selection_remove(const char *path)
{
    struct stat metadata;
    char directory[PATH_MAX];
    char *slash;
    int dir_fd;
    int result;

    if (xc_safe_path(path) != 0 || strlen(path) >= sizeof(directory)) {
        errno = EINVAL;
        return XC_STORE_FAILED;
    }
    if (lstat(path, &metadata) != 0) {
        if (errno == ENOENT) return XC_STORE_COMMITTED_DURABLE;
        return XC_STORE_FAILED;
    }
    if (!S_ISREG(metadata.st_mode) || metadata.st_uid != geteuid()) {
        errno = EPERM;
        return XC_STORE_FAILED;
    }
    snprintf(directory, sizeof(directory), "%s", path);
    slash = strrchr(directory, '/');
    if (!slash || slash == directory) {
        errno = EINVAL;
        return XC_STORE_FAILED;
    }
    *slash = '\0';
    dir_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir_fd < 0) return XC_STORE_FAILED;
    if (unlinkat(dir_fd, slash + 1, 0) != 0) {
        int saved = errno;

        close(dir_fd);
        errno = saved;
        return XC_STORE_FAILED;
    }
    result = fsync(dir_fd) == 0 ? XC_STORE_COMMITTED_DURABLE :
             XC_STORE_COMMITTED_NOT_DURABLE;
    close(dir_fd);
    return result;
}
