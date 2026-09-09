#ifndef MHCAMERA_MEDIA_LOGIC_H
#define MHCAMERA_MEDIA_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XC_MEDIA_YUV_OUTPUT_FPS 15u

struct xc_media_rate_gate {
    uint64_t last_timestamp_us;
    uint64_t next_timestamp_us;
    bool started;
};

struct xc_media_freshness {
    bool started;
    uint64_t first_pts_us;
    uint64_t first_monotonic_us;
};

bool xc_media_rate_gate_accept(struct xc_media_rate_gate *gate,
                               uint64_t timestamp_us);
void xc_media_rate_gate_reset(struct xc_media_rate_gate *gate);

int xc_yuv420p_size(unsigned int width, unsigned int height, size_t *size_out);
int xc_yuv420p_pack(const uint8_t *const planes[3],
                    const int strides[3],
                    unsigned int width,
                    unsigned int height,
                    uint8_t *out,
                    size_t out_size);
bool xc_media_timestamp_fresh(struct xc_media_freshness *freshness,
                              uint64_t pts_us,
                              uint64_t monotonic_us,
                              uint64_t max_lag_us);

#endif
