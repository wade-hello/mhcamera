#ifndef MHCAMERA_STORE_H
#define MHCAMERA_STORE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

struct xc_saved_selection {
    bool configured;
    char account_id[65];
    char region[3];
    bool exists;
    bool enabled;
    char id[65];
    char name[128];
    char model[128];
    unsigned int channel;
};

enum xc_store_result {
    XC_STORE_FAILED = -1,
    XC_STORE_COMMITTED_DURABLE = 0,
    XC_STORE_COMMITTED_NOT_DURABLE = 1,
};

static inline bool xc_store_result_committed(int result)
{
    return result == XC_STORE_COMMITTED_DURABLE ||
           result == XC_STORE_COMMITTED_NOT_DURABLE;
}

int xc_store_prepare(const char *data_root,
                     char *config_path,
                     size_t config_path_size,
                     char *socket_path,
                     size_t socket_path_size,
                     char *status_path,
                     size_t status_path_size);
int xc_store_atomic_write(const char *path, const void *data, size_t size);
int xc_store_clear_xiaomi_token(const char *config_path, const char *api_socket_path);
int xc_store_selection_path(const char *data_root, char *out, size_t out_size);
int xc_store_phone_rate_path(const char *data_root, char *out, size_t out_size);
int xc_store_phone_rate_read(const char *path, uint64_t *retry_after_wall_ms);
int xc_store_phone_rate_write(const char *path, uint64_t retry_after_wall_ms);
int xc_store_selection_read(const char *path,
                            struct xc_saved_selection *selection);
int xc_store_selection_write(const char *path,
                             const struct xc_saved_selection *selection);
int xc_store_selection_remove(const char *path);

#endif
