#ifndef CAMERA_TYPES_H
#define CAMERA_TYPES_H

#include <stdint.h>

#define CAMERA_DEFAULT_DEVICE "/dev/video52"
#define CAMERA_CAPTURE_WIDTH  640
#define CAMERA_CAPTURE_HEIGHT 480

typedef enum {
    CAMERA_FMT_UNKNOWN = 0,
    CAMERA_FMT_NV12,
    CAMERA_FMT_RGB888,
    CAMERA_FMT_BGR888,
} camera_fmt_t;

typedef struct {
    void *data;
    int width;
    int height;
    int stride;
    camera_fmt_t fmt;
    uint64_t timestamp_us;
    int fd; /* DMA-BUF / 零拷贝时可用，-1 表示无效 */
} camera_frame_t;

#endif
