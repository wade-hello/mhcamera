#include "media_logic.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#define XC_MAX_WIDTH 2880u
#define XC_MAX_HEIGHT 1620u
#define XC_MEDIA_YUV_OUTPUT_INTERVAL_US \
    ((1000000u + XC_MEDIA_YUV_OUTPUT_FPS - 1u) / XC_MEDIA_YUV_OUTPUT_FPS)

bool xc_media_rate_gate_accept(struct xc_media_rate_gate *gate,
                               uint64_t timestamp_us)
{
    uint64_t interval = XC_MEDIA_YUV_OUTPUT_INTERVAL_US;
    uint64_t steps;

    if (!gate) return false;
    if (gate->started && timestamp_us < gate->last_timestamp_us) {
        xc_media_rate_gate_reset(gate);
    }
    gate->last_timestamp_us = timestamp_us;
    if (!gate->started) {
        gate->started = true;
        gate->next_timestamp_us = timestamp_us > UINT64_MAX - interval ?
                                  UINT64_MAX : timestamp_us + interval;
        return true;
    }
    if (timestamp_us < gate->next_timestamp_us) return false;
    steps = (timestamp_us - gate->next_timestamp_us) / interval + 1u;
    if (steps > (UINT64_MAX - gate->next_timestamp_us) / interval)
        gate->next_timestamp_us = UINT64_MAX;
    else
        gate->next_timestamp_us += steps * interval;
    return true;
}

void xc_media_rate_gate_reset(struct xc_media_rate_gate *gate)
{
    if (gate) memset(gate, 0, sizeof(*gate));
}

int xc_yuv420p_size(unsigned int width, unsigned int height, size_t *size_out)
{
    size_t pixels;

    if (!size_out || width == 0u || height == 0u ||
        width > XC_MAX_WIDTH || height > XC_MAX_HEIGHT ||
        (width & 1u) || (height & 1u)) {
        errno = EINVAL;
        return -1;
    }
    pixels = (size_t)width * (size_t)height;
    *size_out = pixels + pixels / 2u;
    return 0;
}

int xc_yuv420p_pack(const uint8_t *const planes[3],
                    const int strides[3],
                    unsigned int width,
                    unsigned int height,
                    uint8_t *out,
                    size_t out_size)
{
    size_t total;
    size_t offsets[3];
    unsigned int rows[3];
    unsigned int columns[3];
    unsigned int plane;

    if (!planes || !strides || !out ||
        xc_yuv420p_size(width, height, &total) != 0 || out_size < total) {
        if (errno == 0) errno = ENOSPC;
        return -1;
    }
    offsets[0] = 0u;
    offsets[1] = (size_t)width * height;
    offsets[2] = offsets[1] + (size_t)(width / 2u) * (height / 2u);
    rows[0] = height;
    rows[1] = rows[2] = height / 2u;
    columns[0] = width;
    columns[1] = columns[2] = width / 2u;
    for (plane = 0u; plane < 3u; ++plane) {
        unsigned int row;

        if (!planes[plane] || strides[plane] < (int)columns[plane]) {
            errno = EINVAL;
            return -1;
        }
        for (row = 0u; row < rows[plane]; ++row) {
            memcpy(out + offsets[plane] + (size_t)row * columns[plane],
                   planes[plane] + (size_t)row * (size_t)strides[plane],
                   columns[plane]);
        }
    }
    return (int)total;
}

bool xc_media_timestamp_fresh(struct xc_media_freshness *freshness,
                              uint64_t pts_us,
                              uint64_t monotonic_us,
                              uint64_t max_lag_us)
{
    uint64_t media_elapsed;
    uint64_t wall_elapsed;

    if (!freshness || max_lag_us == 0u) return false;
    if (pts_us == 0u) return true;
    if (!freshness->started) {
        freshness->started = true;
        freshness->first_pts_us = pts_us;
        freshness->first_monotonic_us = monotonic_us;
        return true;
    }
    if (pts_us < freshness->first_pts_us ||
        monotonic_us < freshness->first_monotonic_us) return false;
    media_elapsed = pts_us - freshness->first_pts_us;
    wall_elapsed = monotonic_us - freshness->first_monotonic_us;
    return wall_elapsed <= media_elapsed || wall_elapsed - media_elapsed <= max_lag_us;
}
