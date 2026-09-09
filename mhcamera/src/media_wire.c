#include "media_wire.h"

#include <errno.h>
#include <string.h>

static uint16_t xc_u16le(const uint8_t *data)
{
    return (uint16_t)data[0] | (uint16_t)data[1] << 8;
}

static uint32_t xc_u32le(const uint8_t *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
           (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static uint64_t xc_u64le(const uint8_t *data)
{
    return (uint64_t)xc_u32le(data) | (uint64_t)xc_u32le(data + 4u) << 32;
}

static uint32_t xc_u32be(const uint8_t *data)
{
    return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 |
           (uint32_t)data[2] << 8 | (uint32_t)data[3];
}

static int xc_protocol_error(void)
{
    errno = EPROTO;
    return -1;
}

int xc_aiv1_parse_hello(const uint8_t *data,
                        size_t size,
                        struct xc_aiv1_hello *hello)
{
    uint32_t codec;
    uint32_t max_payload;
    uint64_t stream_epoch;

    if (!data || !hello) {
        errno = EINVAL;
        return -1;
    }
    if (size != XC_AIV1_HELLO_SIZE || memcmp(data, "AIVH", 4u) != 0 ||
        xc_u16le(data + 4u) != XC_AIV1_VERSION ||
        xc_u16le(data + 6u) != XC_AIV1_HELLO_SIZE ||
        xc_u32le(data + 12u) != XC_AIV1_FORMAT_AVCC ||
        xc_u32le(data + 16u) != XC_AIV1_TIMESCALE)
        return xc_protocol_error();
    codec = xc_u32le(data + 8u);
    max_payload = xc_u32le(data + 20u);
    stream_epoch = xc_u64le(data + 24u);
    if ((codec != XC_AIV1_CODEC_H264 && codec != XC_AIV1_CODEC_H265) ||
        max_payload == 0u || max_payload > XC_AIV1_MAX_PAYLOAD ||
        stream_epoch == 0u)
        return xc_protocol_error();
    hello->codec = codec;
    hello->max_payload = max_payload;
    hello->stream_epoch = stream_epoch;
    return 0;
}

int xc_aiv1_parse_frame_header(const uint8_t *data,
                               size_t size,
                               const struct xc_aiv1_hello *hello,
                               struct xc_aiv1_frame *frame)
{
    uint32_t codec;
    uint32_t flags;
    uint64_t stream_epoch;
    uint64_t output_seq;
    uint32_t payload_len;

    if (!data || !hello || !frame) {
        errno = EINVAL;
        return -1;
    }
    if (size != XC_AIV1_FRAME_HEADER_SIZE || memcmp(data, "AIVF", 4u) != 0 ||
        xc_u16le(data + 4u) != XC_AIV1_VERSION ||
        xc_u16le(data + 6u) != XC_AIV1_FRAME_HEADER_SIZE ||
        xc_u32le(data + 44u) != 0u)
        return xc_protocol_error();
    codec = xc_u32le(data + 8u);
    flags = xc_u32le(data + 12u);
    stream_epoch = xc_u64le(data + 16u);
    output_seq = xc_u64le(data + 24u);
    payload_len = xc_u32le(data + 40u);
    if (codec != hello->codec ||
        (flags & ~(XC_AIV1_FLAG_KEYFRAME | XC_AIV1_FLAG_DISCONTINUITY)) != 0u ||
        stream_epoch != hello->stream_epoch || output_seq == 0u ||
        payload_len == 0u || payload_len > hello->max_payload)
        return xc_protocol_error();
    frame->codec = codec;
    frame->flags = flags;
    frame->stream_epoch = stream_epoch;
    frame->output_seq = output_seq;
    frame->pts_us = xc_u64le(data + 32u);
    frame->payload_len = payload_len;
    return 0;
}

int xc_aiv1_avcc_inspect(uint32_t codec,
                         const uint8_t *data,
                         size_t size,
                         unsigned int *nal_flags)
{
    size_t offset = 0u;
    unsigned int flags = 0u;

    if (!data || !nal_flags || size == 0u ||
        (codec != XC_AIV1_CODEC_H264 && codec != XC_AIV1_CODEC_H265)) {
        errno = EINVAL;
        return -1;
    }
    while (offset < size) {
        uint32_t nal_size;
        unsigned int nal_type;

        if (size - offset < 4u) return xc_protocol_error();
        nal_size = xc_u32be(data + offset);
        if (nal_size == 0u || nal_size > size - offset - 4u ||
            (codec == XC_AIV1_CODEC_H265 && nal_size < 2u))
            return xc_protocol_error();
        if (codec == XC_AIV1_CODEC_H264) {
            nal_type = data[offset + 4u] & 0x1fu;
            if (nal_type == 7u) flags |= XC_AIV1_NAL_H264_SPS;
            else if (nal_type == 8u) flags |= XC_AIV1_NAL_H264_PPS;
            else if (nal_type == 5u) flags |= XC_AIV1_NAL_H264_IDR;
        } else {
            nal_type = (data[offset + 4u] >> 1) & 0x3fu;
            if (nal_type == 32u) flags |= XC_AIV1_NAL_H265_VPS;
            else if (nal_type == 33u) flags |= XC_AIV1_NAL_H265_SPS;
            else if (nal_type == 34u) flags |= XC_AIV1_NAL_H265_PPS;
            else if (nal_type >= 16u && nal_type <= 23u)
                flags |= XC_AIV1_NAL_H265_IRAP;
        }
        offset += 4u + (size_t)nal_size;
    }
    *nal_flags = flags;
    return 0;
}

int xc_aiv1_avcc_to_annexb(uint32_t codec, uint8_t *data, size_t size)
{
    size_t offset = 0u;
    unsigned int ignored;

    if (xc_aiv1_avcc_inspect(codec, data, size, &ignored) != 0) return -1;
    while (offset < size) {
        uint32_t nal_size = xc_u32be(data + offset);

        data[offset] = 0u;
        data[offset + 1u] = 0u;
        data[offset + 2u] = 0u;
        data[offset + 3u] = 1u;
        offset += 4u + (size_t)nal_size;
    }
    return 0;
}
