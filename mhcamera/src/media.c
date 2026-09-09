#define _GNU_SOURCE
#include "media_logic.h"
#include "media_wire.h"
#include "media_internal.h"

#include <ainice/video.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/mem.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define XC_MEDIA_CONNECT_TIMEOUT_MS 20000u
#define XC_MEDIA_HELLO_TIMEOUT_MS 20000u
#define XC_MEDIA_FRAME_TIMEOUT_MS 15000u
#define XC_MEDIA_POLL_SLICE_MS 250
#define XC_MEDIA_MAX_LAG_US 1500000u

enum xc_media_delivery_result {
    XC_MEDIA_DELIVERY_FATAL = -1,
    XC_MEDIA_DELIVERY_OK = 0,
    XC_MEDIA_DELIVERY_DROPPED = 1,
};

struct xc_media_stats {
    uint64_t input_frames;
    uint64_t input_bytes;
    uint64_t invalid_avcc;
    uint64_t sequence_gaps;
    uint64_t resync_started;
    uint64_t resync_completed;
    uint64_t decoder_resets;
    uint64_t decoded_frames;
    uint64_t send_failures;
};

struct xc_media_session {
    struct xc_media_owner *owner;
    int fd;
    struct xc_aiv1_hello hello;
    AVCodecContext *decoder;
    AVFrame *decoded;
    uint8_t *compressed;
    size_t compressed_capacity;
    uint8_t *packed;
    size_t packed_capacity;
    ainice_video_session_t *video;
    struct xc_media_freshness freshness;
    struct xc_media_rate_gate yuv_rate_gate;
    struct xc_media_stats stats;
    uint64_t last_sequence;
    bool have_sequence;
    bool waiting_sync;
};

static uint64_t xc_monotonic_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0u;
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

static int64_t xc_monotonic_ms(void)
{
    uint64_t value = xc_monotonic_us();

    return value > (uint64_t)INT64_MAX * 1000u ? -1 : (int64_t)(value / 1000u);
}

static int xc_deadline_after(unsigned int timeout_ms, int64_t *deadline)
{
    int64_t now = xc_monotonic_ms();

    if (!deadline || now < 0 || now > INT64_MAX - (int64_t)timeout_ms) {
        errno = EINVAL;
        return -1;
    }
    *deadline = now + (int64_t)timeout_ms;
    return 0;
}

static int xc_poll(struct xc_media_owner *owner, int fd, short events,
                   int64_t deadline)
{
    for (;;) {
        struct pollfd descriptor = {.fd = fd, .events = events};
        int64_t now;
        int timeout;
        int result;

        if (atomic_load_explicit(&owner->stop, memory_order_relaxed)) {
            errno = ECANCELED;
            return -1;
        }
        now = xc_monotonic_ms();
        if (now < 0 || now >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        timeout = deadline - now > XC_MEDIA_POLL_SLICE_MS ?
                  XC_MEDIA_POLL_SLICE_MS : (int)(deadline - now);
        result = poll(&descriptor, 1u, timeout);
        if (result > 0) {
            if (descriptor.revents & events) return 0;
            if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                errno = EPIPE;
                return -1;
            }
        } else if (result < 0 && errno != EINTR) {
            return -1;
        }
    }
}

static void xc_owner_set_fd(struct xc_media_owner *owner, int fd)
{
    pthread_mutex_lock(&owner->lock);
    owner->active_fd = fd;
    pthread_mutex_unlock(&owner->lock);
}

static void xc_owner_clear_fd(struct xc_media_owner *owner, int fd)
{
    pthread_mutex_lock(&owner->lock);
    if (owner->active_fd == fd) owner->active_fd = -1;
    pthread_mutex_unlock(&owner->lock);
}

static int xc_connect(struct xc_media_owner *owner)
{
    struct sockaddr_un address;
    struct ucred peer;
    socklen_t peer_size = sizeof(peer);
    int64_t deadline;
    int fd;
    int flags;
    int result;

    if (xc_deadline_after(XC_MEDIA_CONNECT_TIMEOUT_MS, &deadline) != 0) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) goto fail;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", owner->socket_path);
    result = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (result != 0 && errno == EINPROGRESS) {
        int socket_error = 0;
        socklen_t error_size = sizeof(socket_error);

        if (xc_poll(owner, fd, POLLOUT, deadline) != 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) != 0)
            goto fail;
        if (socket_error != 0) {
            errno = socket_error;
            goto fail;
        }
    } else if (result != 0) {
        goto fail;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0 ||
        peer_size != sizeof(peer) || peer.uid != geteuid() ||
        peer.pid != owner->peer_pid) {
        errno = EPERM;
        goto fail;
    }
    xc_owner_set_fd(owner, fd);
    return fd;
fail:
    {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
}

static int xc_read_exact(struct xc_media_owner *owner, int fd, void *data,
                         size_t size, unsigned int timeout_ms)
{
    uint8_t *bytes = data;
    size_t offset = 0u;
    int64_t deadline;

    if (xc_deadline_after(timeout_ms, &deadline) != 0) return -1;
    while (offset < size) {
        ssize_t got = recv(fd, bytes + offset, size - offset, 0);

        if (got > 0) {
            offset += (size_t)got;
            continue;
        }
        if (got == 0) {
            errno = EPIPE;
            return -1;
        }
        if (errno == EINTR) continue;
        if ((errno == EAGAIN || errno == EWOULDBLOCK) &&
            xc_poll(owner, fd, POLLIN, deadline) == 0)
            continue;
        return -1;
    }
    return 0;
}

static int xc_decoder_open(struct xc_media_session *session)
{
    const AVCodec *codec;

    if (session->hello.codec != XC_AIV1_CODEC_H265) return 0;
    codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!codec) {
        errno = ENOTSUP;
        return -1;
    }
    session->decoder = avcodec_alloc_context3(codec);
    if (!session->decoder) return -1;
    session->decoder->pkt_timebase = (AVRational){1, XC_AIV1_TIMESCALE};
    session->decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (avcodec_open2(session->decoder, codec, NULL) < 0) {
        avcodec_free_context(&session->decoder);
        errno = EPROTO;
        return -1;
    }
    return 0;
}

static void xc_video_close(struct xc_media_session *session)
{
    if (session->video) ainice_video_close(session->video);
    session->video = NULL;
}

static int xc_video_open(struct xc_media_session *session)
{
    ainice_video_session_t *video = NULL;
    int open_errno;
    int open_result;

    if (session->video) return 0;
    errno = 0;
    open_result = ainice_video_open(&video);
    open_errno = errno;
    if (open_result != 0 || !video) {
        if (video) ainice_video_close(video);
        errno = open_errno != 0 ? open_errno : EIO;
        return -1;
    }
    session->video = video;
    return 0;
}

static enum xc_media_delivery_result xc_deliver(
    struct xc_media_session *session, uint32_t format,
    unsigned int width, unsigned int height,
    const void *data, size_t size, uint64_t pts_us)
{
    struct ainice_video_frame frame;
    uint64_t now = xc_monotonic_us();
    int send_result;

    if (!data || size == 0u || size > AINICE_VIDEO_PACKET_MAX ||
        size > UINT32_MAX || now == 0u ||
        !xc_media_timestamp_fresh(&session->freshness, pts_us, now,
                                  XC_MEDIA_MAX_LAG_US)) {
        errno = ESTALE;
        return XC_MEDIA_DELIVERY_FATAL;
    }
    if (xc_video_open(session) != 0) {
        int open_errno = errno;

        xc_video_close(session);
        errno = open_errno;
        return XC_MEDIA_DELIVERY_DROPPED;
    }
    memset(&frame, 0, sizeof(frame));
    frame.format = format;
    frame.width = width;
    frame.height = height;
    frame.size = (uint32_t)size;
    frame.pts = pts_us;
    frame.data = data;
    errno = 0;
    send_result = ainice_video_send_frame(session->video, &frame);
    if (send_result != 0) {
        int send_errno = errno;

        session->stats.send_failures++;
        xc_video_close(session);
        errno = send_errno != 0 ? send_errno : EIO;
        return XC_MEDIA_DELIVERY_DROPPED;
    }
    return XC_MEDIA_DELIVERY_OK;
}

static int xc_emit_decoded(struct xc_media_session *session, AVFrame *frame)
{
    const uint8_t *planes[3] = {frame->data[0], frame->data[1], frame->data[2]};
    int strides[3] = {frame->linesize[0], frame->linesize[1], frame->linesize[2]};
    int64_t best_effort = frame->best_effort_timestamp;
    size_t size;
    /* AIV1 PTS is a relative microsecond timeline, not CLOCK_MONOTONIC.
     * AV_NOPTS (and invalid negative decoder output) is intentionally sent
     * as zero: freshness accepts it rather than rejecting a valid HEVC frame. */
    uint64_t pts_us = best_effort == AV_NOPTS_VALUE || best_effort < 0 ?
                      0u : (uint64_t)best_effort;
    uint64_t rate_timestamp_us;
    enum xc_media_delivery_result delivery_result;

    if (frame->format != AV_PIX_FMT_YUV420P || frame->width <= 0 ||
        frame->height <= 0 ||
        xc_yuv420p_size((unsigned int)frame->width,
                        (unsigned int)frame->height, &size) != 0)
        return -1;
    rate_timestamp_us = pts_us != 0u ? pts_us : xc_monotonic_us();
    if (rate_timestamp_us == 0u) {
        errno = EIO;
        return -1;
    }
    if (!xc_media_rate_gate_accept(&session->yuv_rate_gate,
                                   rate_timestamp_us)) {
        return 0;
    }
    if (size > session->packed_capacity) {
        uint8_t *grown = av_realloc(session->packed, size);

        if (!grown) return -1;
        session->packed = grown;
        session->packed_capacity = size;
    }
    if (xc_yuv420p_pack(planes, strides, (unsigned int)frame->width,
                        (unsigned int)frame->height, session->packed, size) < 0)
        return -1;
    delivery_result = xc_deliver(session, AINICE_VIDEO_FRAME_FORMAT_YUV420P,
                                 (unsigned int)frame->width,
                                 (unsigned int)frame->height,
                                 session->packed, size, pts_us);
    if (delivery_result == XC_MEDIA_DELIVERY_FATAL) return -1;
    /* Decoded YUV frames are self-contained. A downstream open/send failure
     * drops only this frame; the retained decoder can deliver the next frame
     * through a newly opened libainice session. */
    if (delivery_result == XC_MEDIA_DELIVERY_OK)
        session->stats.decoded_frames++;
    return 0;
}

static int xc_decoder_drain(struct xc_media_session *session)
{
    for (;;) {
        int result;

        av_frame_unref(session->decoded);
        result = avcodec_receive_frame(session->decoder, session->decoded);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return 0;
        if (result < 0) {
            errno = EPROTO;
            return -1;
        }
        if (xc_emit_decoded(session, session->decoded) != 0) return -1;
    }
}

static int xc_decode(struct xc_media_session *session, size_t size,
                     uint64_t pts_us)
{
    AVPacket packet;
    int result;

    memset(&packet, 0, sizeof(packet));
    packet.data = session->compressed;
    packet.size = (int)size;
    packet.pts = packet.dts = pts_us > (uint64_t)INT64_MAX ?
                            AV_NOPTS_VALUE : (int64_t)pts_us;
    result = avcodec_send_packet(session->decoder, &packet);
    if (result == AVERROR(EAGAIN)) {
        if (xc_decoder_drain(session) != 0) return -1;
        result = avcodec_send_packet(session->decoder, &packet);
    }
    if (result < 0) {
        errno = EPROTO;
        return -1;
    }
    return xc_decoder_drain(session);
}

static int xc_reset(struct xc_media_session *session)
{
    /* H.264 is forwarded as a predictive compressed stream, so a source gap
     * must also reset the downstream decoder by closing its video session.
     * H.265 is decoded locally into self-contained YUV420P frames: reset only
     * the private HEVC decoder and keep the downstream YUV session alive. */
    if (session->hello.codec == XC_AIV1_CODEC_H264)
        xc_video_close(session);
    memset(&session->freshness, 0, sizeof(session->freshness));
    xc_media_rate_gate_reset(&session->yuv_rate_gate);
    session->waiting_sync = true;
    session->stats.resync_started++;
    if (session->hello.codec == XC_AIV1_CODEC_H265) {
        avcodec_free_context(&session->decoder);
        session->stats.decoder_resets++;
        return xc_decoder_open(session);
    }
    return 0;
}

static bool xc_sync_complete(uint32_t codec, unsigned int nal_flags)
{
    if (codec == XC_AIV1_CODEC_H264)
        return (nal_flags & XC_AIV1_NAL_H264_SYNC) == XC_AIV1_NAL_H264_SYNC;
    return (nal_flags & XC_AIV1_NAL_H265_SYNC) == XC_AIV1_NAL_H265_SYNC;
}

static int xc_buffer_reserve(struct xc_media_session *session, size_t size)
{
    size_t capacity;
    uint8_t *grown;

    if (size > SIZE_MAX - AV_INPUT_BUFFER_PADDING_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }
    capacity = size + AV_INPUT_BUFFER_PADDING_SIZE;
    if (capacity <= session->compressed_capacity) return 0;
    grown = av_realloc(session->compressed, capacity);
    if (!grown) return -1;
    session->compressed = grown;
    session->compressed_capacity = capacity;
    return 0;
}

static int xc_process_frame(struct xc_media_session *session,
                            const struct xc_aiv1_frame *frame)
{
    unsigned int nal_flags;
    bool keyframe = (frame->flags & XC_AIV1_FLAG_KEYFRAME) != 0u;
    bool discontinuity = (frame->flags & XC_AIV1_FLAG_DISCONTINUITY) != 0u;
    bool sequence_gap = false;
    bool complete;

    if (xc_aiv1_avcc_inspect(frame->codec, session->compressed,
                             frame->payload_len, &nal_flags) != 0) {
        session->stats.invalid_avcc++;
        return -1;
    }
    complete = xc_sync_complete(frame->codec, nal_flags);
    if (keyframe != complete || (discontinuity && !keyframe)) {
        errno = EPROTO;
        return -1;
    }
    if (session->have_sequence &&
        (session->last_sequence == UINT64_MAX ||
         frame->output_seq != session->last_sequence + 1u)) {
        sequence_gap = true;
        session->stats.sequence_gaps++;
    }
    session->last_sequence = frame->output_seq;
    session->have_sequence = true;
    if (sequence_gap || discontinuity) {
        if (xc_reset(session) != 0) return -1;
    }
    if (session->waiting_sync) {
        if (!keyframe) return 0;
        session->waiting_sync = false;
        session->stats.resync_completed++;
    }
    if (xc_aiv1_avcc_to_annexb(frame->codec, session->compressed,
                                frame->payload_len) != 0)
        return -1;
    if (frame->codec == XC_AIV1_CODEC_H264) {
        enum xc_media_delivery_result delivery_result =
            xc_deliver(session, AINICE_VIDEO_FRAME_FORMAT_H264,
                       0u, 0u, session->compressed,
                       frame->payload_len, frame->pts_us);

        if (delivery_result == XC_MEDIA_DELIVERY_DROPPED) {
            /* A new compressed stream may not begin with a delta access unit.
             * Keep consuming the same AIV1 producer but wait for the next
             * independently decodable SPS/PPS/IDR unit before reopening the
             * downstream stream. The failed frame is never retried. */
            session->waiting_sync = true;
            return 0;
        }
        return delivery_result;
    }
    return xc_decode(session, frame->payload_len, frame->pts_us);
}

static void xc_session_close(struct xc_media_session *session)
{
    if (session->fd >= 0) {
        xc_owner_clear_fd(session->owner, session->fd);
        close(session->fd);
        session->fd = -1;
    }
    xc_video_close(session);
    av_freep(&session->packed);
    av_freep(&session->compressed);
    av_frame_free(&session->decoded);
    avcodec_free_context(&session->decoder);
}

static void xc_summary(const struct xc_media_session *session, int result)
{
    fprintf(stderr,
            "mhcamera: event=media_session_summary result=%s codec=%s "
            "input_frames=%llu input_bytes=%llu invalid_avcc=%llu "
            "sequence_gaps=%llu resync_started=%llu resync_completed=%llu "
            "decoder_resets=%llu decoded_frames=%llu send_failures=%llu\n",
            result == 0 ? "stopped" : "error",
            session->hello.codec == XC_AIV1_CODEC_H264 ? "h264" :
            session->hello.codec == XC_AIV1_CODEC_H265 ? "h265" : "unknown",
            (unsigned long long)session->stats.input_frames,
            (unsigned long long)session->stats.input_bytes,
            (unsigned long long)session->stats.invalid_avcc,
            (unsigned long long)session->stats.sequence_gaps,
            (unsigned long long)session->stats.resync_started,
            (unsigned long long)session->stats.resync_completed,
            (unsigned long long)session->stats.decoder_resets,
            (unsigned long long)session->stats.decoded_frames,
            (unsigned long long)session->stats.send_failures);
}

int xc_media_session_run_internal(struct xc_media_owner *owner)
{
    struct xc_media_session session = {.owner = owner, .fd = -1,
                                       .waiting_sync = true};
    uint8_t hello_header[XC_AIV1_HELLO_SIZE];
    uint8_t frame_header[XC_AIV1_FRAME_HEADER_SIZE];
    int result = -1;
    int result_errno = 0;

    session.fd = xc_connect(owner);
    if (session.fd < 0) goto done;
    if (xc_read_exact(owner, session.fd, hello_header, sizeof(hello_header),
                      XC_MEDIA_HELLO_TIMEOUT_MS) != 0 ||
        xc_aiv1_parse_hello(hello_header, sizeof(hello_header),
                            &session.hello) != 0)
        goto done;
    session.decoded = av_frame_alloc();
    if (!session.decoded || xc_decoder_open(&session) != 0) goto done;
    xc_media_notify_internal(owner, XC_MEDIA_RUNNING,
                             session.hello.codec == XC_AIV1_CODEC_H264 ?
                             "h264" : "h265", NULL);
    while (!atomic_load_explicit(&owner->stop, memory_order_relaxed)) {
        struct xc_aiv1_frame frame;

        if (xc_read_exact(owner, session.fd, frame_header,
                          sizeof(frame_header), XC_MEDIA_FRAME_TIMEOUT_MS) != 0 ||
            xc_aiv1_parse_frame_header(frame_header, sizeof(frame_header),
                                        &session.hello, &frame) != 0 ||
            xc_buffer_reserve(&session, frame.payload_len) != 0 ||
            xc_read_exact(owner, session.fd, session.compressed,
                          frame.payload_len, XC_MEDIA_FRAME_TIMEOUT_MS) != 0)
            goto done;
        memset(session.compressed + frame.payload_len, 0,
               AV_INPUT_BUFFER_PADDING_SIZE);
        session.stats.input_frames++;
        session.stats.input_bytes += frame.payload_len;
        if (xc_process_frame(&session, &frame) != 0) goto done;
    }
    result = 0;
done:
    result_errno = errno;
    if (atomic_load_explicit(&owner->stop, memory_order_relaxed)) result = 0;
    xc_summary(&session, result);
    xc_session_close(&session);
    if (result != 0) errno = result_errno;
    return result;
}

int xc_media_runtime_init_internal(void)
{
    return avcodec_find_decoder(AV_CODEC_ID_HEVC) ? 0 : -1;
}
