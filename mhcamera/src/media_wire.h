#ifndef MHCAMERA_MEDIA_WIRE_H
#define MHCAMERA_MEDIA_WIRE_H

#include <stddef.h>
#include <stdint.h>

#define XC_AIV1_VERSION 1u
#define XC_AIV1_HELLO_SIZE 32u
#define XC_AIV1_FRAME_HEADER_SIZE 48u
#define XC_AIV1_MAX_PAYLOAD (8u * 1024u * 1024u)
#define XC_AIV1_TIMESCALE 1000000u

#define XC_AIV1_CODEC_H264 1u
#define XC_AIV1_CODEC_H265 2u
#define XC_AIV1_FORMAT_AVCC 1u

#define XC_AIV1_FLAG_KEYFRAME (1u << 0)
#define XC_AIV1_FLAG_DISCONTINUITY (1u << 1)

#define XC_AIV1_NAL_H264_SPS (1u << 0)
#define XC_AIV1_NAL_H264_PPS (1u << 1)
#define XC_AIV1_NAL_H264_IDR (1u << 2)
#define XC_AIV1_NAL_H264_SYNC \
    (XC_AIV1_NAL_H264_SPS | XC_AIV1_NAL_H264_PPS | XC_AIV1_NAL_H264_IDR)
#define XC_AIV1_NAL_H265_VPS (1u << 3)
#define XC_AIV1_NAL_H265_SPS (1u << 4)
#define XC_AIV1_NAL_H265_PPS (1u << 5)
#define XC_AIV1_NAL_H265_IRAP (1u << 6)
#define XC_AIV1_NAL_H265_SYNC \
    (XC_AIV1_NAL_H265_VPS | XC_AIV1_NAL_H265_SPS | \
     XC_AIV1_NAL_H265_PPS | XC_AIV1_NAL_H265_IRAP)

struct xc_aiv1_hello {
    uint32_t codec;
    uint32_t max_payload;
    uint64_t stream_epoch;
};

struct xc_aiv1_frame {
    uint32_t codec;
    uint32_t flags;
    uint64_t stream_epoch;
    uint64_t output_seq;
    uint64_t pts_us;
    uint32_t payload_len;
};

int xc_aiv1_parse_hello(const uint8_t *data,
                        size_t size,
                        struct xc_aiv1_hello *hello);
int xc_aiv1_parse_frame_header(const uint8_t *data,
                               size_t size,
                               const struct xc_aiv1_hello *hello,
                               struct xc_aiv1_frame *frame);
int xc_aiv1_avcc_inspect(uint32_t codec,
                         const uint8_t *data,
                         size_t size,
                         unsigned int *nal_flags);
int xc_aiv1_avcc_to_annexb(uint32_t codec, uint8_t *data, size_t size);

#endif
