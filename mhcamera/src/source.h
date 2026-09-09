#ifndef MHCAMERA_SOURCE_H
#define MHCAMERA_SOURCE_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum xc_region {
    XC_REGION_CN = 0,
    XC_REGION_DE = 1,
    XC_REGION_I2 = 2,
    XC_REGION_RU = 3,
    XC_REGION_SG = 4,
    XC_REGION_US = 5,
    XC_REGION_COUNT = 6,
};

enum xc_source_state {
    XC_SOURCE_UNKNOWN = 0,
    XC_SOURCE_STOPPED,
    XC_SOURCE_STARTING,
    XC_SOURCE_RUNNING,
    XC_SOURCE_RETRYING,
    XC_SOURCE_STOPPING,
    XC_SOURCE_ERROR,
    XC_SOURCE_STATE_COUNT,
};
enum xc_source_error {
    XC_SOURCE_ERROR_NONE = 0,
    XC_SOURCE_ERROR_DIAL,
    XC_SOURCE_ERROR_READ,
    XC_SOURCE_ERROR_AUDIO_UNAVAILABLE,
    XC_SOURCE_ERROR_CREDENTIAL_REJECTED,
    XC_SOURCE_ERROR_COUNT,
};
const char *xc_source_state_text(enum xc_source_state state);
int xc_source_state_parse(const char *text, enum xc_source_state *state);
const char *xc_source_error_text(enum xc_source_error error);
int xc_source_error_parse(const char *text, enum xc_source_error *error);

const char *xc_region_text(enum xc_region region);
int xc_region_parse(const char *text, enum xc_region *region_out);

/* Validates the opaque source returned by the Xiaomi core against the
 * selected identities, then reconstructs the only product source form. */
int xc_camera_source_rebuild(const char *core_source,
                             const char *expected_account_id,
                             const char *expected_region,
                             const char *expected_camera_id,
                             const char *expected_model,
                             unsigned int channel,
                             bool audio_enabled,
                             char *output,
                             size_t output_size);
int xc_camera_source_identify(const char *core_source,
                              const char *expected_account_id,
                              const char *expected_region,
                              char *camera_id,
                              size_t camera_id_size,
                              char *model,
                              size_t model_size);

#ifdef __cplusplus
}
#endif

#endif
