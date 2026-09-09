#ifndef MHCAMERA_HOST_INPUT_H
#define MHCAMERA_HOST_INPUT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* patch_allowed is checked after config.get and immediately before the only
 * config.patch. A rejected guard returns ECANCELED without changing input. */
int xc_host_input_ensure_bitstream(bool (*patch_allowed)(void *userdata),
                                   void *guard_userdata);
int xc_host_input_is_bitstream(bool *bitstream_out);

#ifdef __cplusplus
}
#endif

#endif
