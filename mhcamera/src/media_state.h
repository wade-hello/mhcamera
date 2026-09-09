#ifndef MHCAMERA_MEDIA_STATE_H
#define MHCAMERA_MEDIA_STATE_H

/* AI attachment only; independent of source and RTSP listener state. */
enum xc_media_state {
    XC_MEDIA_STOPPED = 0,
    XC_MEDIA_STARTING = 1,
    XC_MEDIA_RUNNING = 2,
    XC_MEDIA_WAITING_INPUT = 3,
    XC_MEDIA_ERROR = 4,
};

#endif
