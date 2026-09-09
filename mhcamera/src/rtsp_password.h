#ifndef MHCAMERA_RTSP_PASSWORD_H
#define MHCAMERA_RTSP_PASSWORD_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define XC_RTSP_PASSWORD_LENGTH 8u
#define XC_RTSP_PASSWORD_STORAGE (XC_RTSP_PASSWORD_LENGTH + 1u)

/* Product playback identities are exactly eight ASCII alphanumerics. */
static inline bool xc_rtsp_password_valid(const char *password)
{
    size_t length, i;
    if (!password) return false;
    length = strlen(password);
    if (length != XC_RTSP_PASSWORD_LENGTH) return false;
    for (i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)password[i];
        if (ch >= '0' && ch <= '9') continue;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) continue;
        return false;
    }
    return true;
}

#endif
