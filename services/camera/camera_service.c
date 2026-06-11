/*
 * USB UVC 取流（V4L2 MMAP），逻辑对齐 ppocr/cpp/usb_camera.cc
 */
#include "camera_service.h"

#include "camera_parser.h"

#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define CAMERA_BUFFER_COUNT      4
#define PREVIEW_BUFFER_COUNT     3
#define RGB_BUFFER_COUNT         2

typedef struct {
    void *start;
    size_t length;
} camera_buffer_t;

typedef struct {
    int fd;
    int width;
    int height;
    int bytesperline;
    uint32_t pixfmt;
    enum v4l2_buf_type buf_type;
    bool multiplanar;
    bool streaming;
    camera_buffer_t buffers[CAMERA_BUFFER_COUNT];
    unsigned int buffer_count;
} camera_ctx_t;

struct camera_service {
    camera_config_t cfg;
    camera_ctx_t camera;

    int preview_w;
    int preview_h;
    lv_color_t *preview_bufs[PREVIEW_BUFFER_COUNT];

    /* RGB888 备份缓冲（OCR 路径用）：双缓冲 + write/read 交换 */
    uint8_t *rgb_bufs[RGB_BUFFER_COUNT];
    int rgb_buf_w;
    int rgb_buf_h;
    int rgb_write_idx;
    int rgb_read_idx;
    uint32_t rgb_seq;

    pthread_t capture_thread;
    pthread_mutex_t frame_mutex;
    volatile bool thread_stop;
    volatile bool running;
    volatile bool connected;

    int ready_frame_index;
    uint32_t ready_frame_seq;
    int displayed_frame_index;
    uint32_t displayed_frame_seq;
};

static int xioctl(int fd, unsigned long request, void *arg) {
    int ret;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

static const char *pixfmt_fourcc_name(uint32_t fmt) {
    static char name[5];
    name[0] = (char)(fmt & 0xff);
    name[1] = (char)((fmt >> 8) & 0xff);
    name[2] = (char)((fmt >> 16) & 0xff);
    name[3] = (char)((fmt >> 24) & 0xff);
    name[4] = '\0';
    return name;
}

static void camera_release_ctx(camera_ctx_t *camera) {
    if (!camera) {
        return;
    }

    if (camera->fd >= 0 && camera->streaming) {
        enum v4l2_buf_type type = camera->buf_type;
        xioctl(camera->fd, VIDIOC_STREAMOFF, &type);
    }

    for (unsigned int i = 0; i < camera->buffer_count; i++) {
        if (camera->buffers[i].start && camera->buffers[i].length > 0) {
            munmap(camera->buffers[i].start, camera->buffers[i].length);
        }
    }

    if (camera->fd >= 0) {
        close(camera->fd);
    }

    memset(camera, 0, sizeof(*camera));
    camera->fd = -1;
}

static int camera_set_format(camera_ctx_t *camera, uint32_t pixfmt, int req_w, int req_h) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = camera->buf_type;

    if (camera->multiplanar) {
        fmt.fmt.pix_mp.width = (uint32_t)req_w;
        fmt.fmt.pix_mp.height = (uint32_t)req_h;
        fmt.fmt.pix_mp.pixelformat = pixfmt;
        fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
        fmt.fmt.pix_mp.num_planes = 1;
    } else {
        fmt.fmt.pix.width = (uint32_t)req_w;
        fmt.fmt.pix.height = (uint32_t)req_h;
        fmt.fmt.pix.pixelformat = pixfmt;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;
    }

    if (xioctl(camera->fd, VIDIOC_S_FMT, &fmt) == -1) {
        return -1;
    }

    if (camera->multiplanar) {
        if (fmt.fmt.pix_mp.pixelformat != pixfmt) {
            return -1;
        }
        camera->width = (int)fmt.fmt.pix_mp.width;
        camera->height = (int)fmt.fmt.pix_mp.height;
        camera->bytesperline = (int)fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
        camera->pixfmt = fmt.fmt.pix_mp.pixelformat;
    } else {
        if (fmt.fmt.pix.pixelformat != pixfmt) {
            return -1;
        }
        camera->width = (int)fmt.fmt.pix.width;
        camera->height = (int)fmt.fmt.pix.height;
        camera->bytesperline = (int)fmt.fmt.pix.bytesperline;
        camera->pixfmt = fmt.fmt.pix.pixelformat;
    }

    if (camera->bytesperline <= 0) {
        camera->bytesperline = camera->width * 2;
    }
    return 0;
}

static int camera_init_device(camera_ctx_t *camera, const char *device, int req_w, int req_h) {
    struct v4l2_capability cap;
    struct v4l2_requestbuffers req;
    enum v4l2_buf_type type;
    uint32_t caps;

    camera_release_ctx(camera);

    if (!device || access(device, F_OK) != 0) {
        fprintf(stderr, "camera: device not found: %s\n", device ? device : "(null)");
        return -1;
    }

    camera->fd = open(device, O_RDWR | O_NONBLOCK, 0);
    if (camera->fd < 0) {
        fprintf(stderr, "camera: open %s failed: %s\n", device, strerror(errno));
        return -1;
    }

    if (xioctl(camera->fd, VIDIOC_QUERYCAP, &cap) == -1) {
        fprintf(stderr, "camera: VIDIOC_QUERYCAP failed: %s\n", strerror(errno));
        camera_release_ctx(camera);
        return -1;
    }

    caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "camera: %s is not a streaming device\n", device);
        camera_release_ctx(camera);
        return -1;
    }

    if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
        camera->multiplanar = true;
        camera->buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    } else if (caps & V4L2_CAP_VIDEO_CAPTURE) {
        camera->multiplanar = false;
        camera->buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    } else {
        fprintf(stderr, "camera: %s is not a capture device\n", device);
        camera_release_ctx(camera);
        return -1;
    }

    static const uint32_t fmt_order[] = {
        V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_UYVY, V4L2_PIX_FMT_RGB565, V4L2_PIX_FMT_MJPEG, 0};
    bool format_ok = false;
    for (int i = 0; fmt_order[i] != 0; i++) {
        if (camera_set_format(camera, fmt_order[i], req_w, req_h) == 0) {
            format_ok = true;
            break;
        }
    }
    if (!format_ok) {
        fprintf(stderr, "camera: no supported pixel format on %s\n", device);
        camera_release_ctx(camera);
        return -1;
    }

    memset(&req, 0, sizeof(req));
    req.count = CAMERA_BUFFER_COUNT;
    req.type = camera->buf_type;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(camera->fd, VIDIOC_REQBUFS, &req) == -1 || req.count < 2) {
        fprintf(stderr, "camera: VIDIOC_REQBUFS failed: %s\n", strerror(errno));
        camera_release_ctx(camera);
        return -1;
    }

    camera->buffer_count = req.count > CAMERA_BUFFER_COUNT ? CAMERA_BUFFER_COUNT : req.count;

    for (unsigned int i = 0; i < camera->buffer_count; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = camera->buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (camera->multiplanar) {
            buf.length = VIDEO_MAX_PLANES;
            buf.m.planes = planes;
        }

        if (xioctl(camera->fd, VIDIOC_QUERYBUF, &buf) == -1) {
            camera_release_ctx(camera);
            return -1;
        }

        camera->buffers[i].length = camera->multiplanar ? planes[0].length : buf.length;
        camera->buffers[i].start =
            mmap(NULL, camera->buffers[i].length, PROT_READ | PROT_WRITE, MAP_SHARED, camera->fd,
                 camera->multiplanar ? planes[0].m.mem_offset : buf.m.offset);
        if (camera->buffers[i].start == MAP_FAILED) {
            camera->buffers[i].start = NULL;
            camera_release_ctx(camera);
            return -1;
        }

        if (xioctl(camera->fd, VIDIOC_QBUF, &buf) == -1) {
            camera_release_ctx(camera);
            return -1;
        }
    }

    type = camera->buf_type;
    if (xioctl(camera->fd, VIDIOC_STREAMON, &type) == -1) {
        fprintf(stderr, "camera: VIDIOC_STREAMON failed: %s\n", strerror(errno));
        camera_release_ctx(camera);
        return -1;
    }

    camera->streaming = true;
    printf("camera: opened %s %dx%d %s\n", device, camera->width, camera->height,
           pixfmt_fourcc_name(camera->pixfmt));
    return 0;
}

static int camera_grab_raw(camera_ctx_t *camera, const uint8_t **frame_data, size_t *bytesused,
                           int poll_ms) {
    struct v4l2_buffer buf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    struct pollfd pfd;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = camera->fd;
    pfd.events = POLLIN;

    int poll_ret = poll(&pfd, 1, poll_ms);
    if (poll_ret < 0) {
        return (errno == EINTR) ? 1 : -1;
    }
    if (poll_ret == 0) {
        return 1;
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        return -1;
    }

    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = camera->buf_type;
    buf.memory = V4L2_MEMORY_MMAP;
    if (camera->multiplanar) {
        buf.length = VIDEO_MAX_PLANES;
        buf.m.planes = planes;
    }

    if (xioctl(camera->fd, VIDIOC_DQBUF, &buf) == -1) {
        return (errno == EAGAIN) ? 1 : -1;
    }

    if (buf.index < camera->buffer_count && camera->buffers[buf.index].start) {
        *frame_data = (const uint8_t *)camera->buffers[buf.index].start;
        *bytesused = camera->multiplanar ? planes[0].bytesused : buf.bytesused;
    } else {
        *frame_data = NULL;
        *bytesused = 0;
    }

    if (xioctl(camera->fd, VIDIOC_QBUF, &buf) == -1) {
        return -1;
    }
    return 0;
}

static void convert_frame_to_preview(camera_service_t *svc, const uint8_t *frame_data,
                                     size_t bytesused, lv_color_t *dst) {
    camera_ctx_t *cam = &svc->camera;

    if (!frame_data || bytesused == 0 || !dst) {
        return;
    }

    if (cam->pixfmt == V4L2_PIX_FMT_MJPEG) {
        uint8_t *rgb = camera_parser_mjpeg_rgb_buffer();
        if (camera_parser_decode_mjpeg(frame_data, bytesused, rgb, cam->width, cam->height)) {
            camera_parser_rgb888_to_lvcolor(rgb, cam->width, cam->height, dst, svc->preview_w,
                                            svc->preview_h);
        }
    } else if (cam->pixfmt == V4L2_PIX_FMT_RGB565) {
        camera_parser_rgb565_to_lvcolor(frame_data, cam->width, cam->height, cam->bytesperline, dst,
                                        svc->preview_w, svc->preview_h);
    } else {
        camera_parser_yuv422_to_lvcolor(frame_data, cam->width, cam->height, cam->bytesperline,
                                        cam->pixfmt, dst, svc->preview_w, svc->preview_h);
    }
}

static void convert_frame_to_rgb888(camera_service_t *svc, const uint8_t *frame_data,
                                    size_t bytesused, uint8_t *dst_rgb) {
    camera_ctx_t *cam = &svc->camera;

    if (!frame_data || bytesused == 0 || !dst_rgb) {
        return;
    }

    if (cam->pixfmt == V4L2_PIX_FMT_MJPEG) {
        uint8_t *rgb = camera_parser_mjpeg_rgb_buffer();
        /* MJPEG 路径已经在 convert_frame_to_preview 中解码到 g_mjpeg_rgb_buf；这里再 memcpy 一次。 */
        camera_parser_rgb888_copy(rgb, cam->width, cam->height, dst_rgb);
    } else if (cam->pixfmt == V4L2_PIX_FMT_RGB565) {
        camera_parser_rgb565_to_rgb888(frame_data, cam->width, cam->height, cam->bytesperline,
                                       dst_rgb);
    } else {
        camera_parser_yuv422_to_rgb888(frame_data, cam->width, cam->height, cam->bytesperline,
                                       cam->pixfmt, dst_rgb);
    }
}

static int ensure_rgb_buffers(camera_service_t *svc) {
    int w = svc->camera.width;
    int h = svc->camera.height;

    if (w <= 0 || h <= 0) {
        return -1;
    }
    if (svc->rgb_bufs[0] && svc->rgb_buf_w == w && svc->rgb_buf_h == h) {
        return 0;
    }

    for (int i = 0; i < RGB_BUFFER_COUNT; i++) {
        free(svc->rgb_bufs[i]);
        svc->rgb_bufs[i] = (uint8_t *)malloc((size_t)w * (size_t)h * 3);
        if (!svc->rgb_bufs[i]) {
            return -1;
        }
    }
    svc->rgb_buf_w = w;
    svc->rgb_buf_h = h;
    svc->rgb_write_idx = 0;
    svc->rgb_read_idx = 1;
    svc->rgb_seq = 0;
    return 0;
}

static void free_rgb_buffers(camera_service_t *svc) {
    for (int i = 0; i < RGB_BUFFER_COUNT; i++) {
        free(svc->rgb_bufs[i]);
        svc->rgb_bufs[i] = NULL;
    }
    svc->rgb_buf_w = 0;
    svc->rgb_buf_h = 0;
    svc->rgb_write_idx = 0;
    svc->rgb_read_idx = 1;
    svc->rgb_seq = 0;
}

static int pick_write_frame_index(camera_service_t *svc) {
    int reserved_a;
    int reserved_b;

    pthread_mutex_lock(&svc->frame_mutex);
    reserved_a = svc->ready_frame_index;
    reserved_b = svc->displayed_frame_index;
    pthread_mutex_unlock(&svc->frame_mutex);

    for (int i = 0; i < PREVIEW_BUFFER_COUNT; i++) {
        if (i != reserved_a && i != reserved_b) {
            return i;
        }
    }
    return 0;
}

static void publish_frame(camera_service_t *svc, int frame_index) {
    pthread_mutex_lock(&svc->frame_mutex);
    svc->ready_frame_index = frame_index;
    svc->ready_frame_seq++;
    svc->connected = true;
    pthread_mutex_unlock(&svc->frame_mutex);
}

static void publish_rgb(camera_service_t *svc) {
    pthread_mutex_lock(&svc->frame_mutex);
    int new_read = svc->rgb_write_idx;
    svc->rgb_write_idx = svc->rgb_read_idx;
    svc->rgb_read_idx = new_read;
    svc->rgb_seq++;
    pthread_mutex_unlock(&svc->frame_mutex);
}

static void *capture_thread_cb(void *arg) {
    camera_service_t *svc = (camera_service_t *)arg;

    while (!svc->thread_stop) {
        if (svc->camera.fd < 0) {
            const char *dev = svc->cfg.device ? svc->cfg.device : CAMERA_DEFAULT_DEVICE;
            int w = svc->cfg.width > 0 ? svc->cfg.width : CAMERA_CAPTURE_WIDTH;
            int h = svc->cfg.height > 0 ? svc->cfg.height : CAMERA_CAPTURE_HEIGHT;

            if (camera_init_device(&svc->camera, dev, w, h) != 0) {
                svc->connected = false;
                usleep(500 * 1000);
                continue;
            }

            if (camera_parser_build_scale_lut(svc->camera.width, svc->camera.height, svc->preview_w,
                                              svc->preview_h) != 0) {
                camera_release_ctx(&svc->camera);
                usleep(500 * 1000);
                continue;
            }

            if (ensure_rgb_buffers(svc) != 0) {
                fprintf(stderr, "camera: alloc rgb backup buffer failed\n");
            }
        }

        bool got = false;
        const uint8_t *frame_data = NULL;
        size_t bytesused = 0;
        lv_color_t *write_buf = NULL;
        int write_index = pick_write_frame_index(svc);

        write_buf = svc->preview_bufs[write_index];

        for (;;) {
            int poll_ms = got ? 0 : 50;
            int ret = camera_grab_raw(&svc->camera, &frame_data, &bytesused, poll_ms);
            if (ret < 0) {
                camera_release_ctx(&svc->camera);
                svc->connected = false;
                break;
            }
            if (ret > 0) {
                break;
            }

            convert_frame_to_preview(svc, frame_data, bytesused, write_buf);
            if (svc->rgb_bufs[svc->rgb_write_idx]) {
                convert_frame_to_rgb888(svc, frame_data, bytesused,
                                        svc->rgb_bufs[svc->rgb_write_idx]);
            }
            got = true;
        }

        if (got) {
            publish_frame(svc, write_index);
            if (svc->rgb_bufs[0]) {
                publish_rgb(svc);
            }
        }
    }

    camera_release_ctx(&svc->camera);
    svc->connected = false;
    return NULL;
}

static void free_preview_buffers(camera_service_t *svc) {
    if (!svc) {
        return;
    }
    for (int i = 0; i < PREVIEW_BUFFER_COUNT; i++) {
        free(svc->preview_bufs[i]);
        svc->preview_bufs[i] = NULL;
    }
}

camera_service_t *camera_service_open(const camera_config_t *cfg) {
    camera_service_t *svc = (camera_service_t *)calloc(1, sizeof(camera_service_t));
    if (!svc) {
        return NULL;
    }

    pthread_mutex_init(&svc->frame_mutex, NULL);
    svc->ready_frame_index = -1;
    svc->displayed_frame_index = 0;
    svc->camera.fd = -1;

    if (cfg) {
        svc->cfg = *cfg;
    } else {
        svc->cfg.device = CAMERA_DEFAULT_DEVICE;
        svc->cfg.width = CAMERA_CAPTURE_WIDTH;
        svc->cfg.height = CAMERA_CAPTURE_HEIGHT;
        svc->cfg.fmt = CAMERA_FMT_UNKNOWN;
    }

    camera_parser_init_yuv_tables();
    return svc;
}

int camera_service_start(camera_service_t *svc, int preview_w, int preview_h) {
    if (!svc || preview_w <= 0 || preview_h <= 0) {
        return -1;
    }
    if (svc->running) {
        return 0;
    }

    svc->preview_w = preview_w;
    svc->preview_h = preview_h;

    for (int i = 0; i < PREVIEW_BUFFER_COUNT; i++) {
        svc->preview_bufs[i] =
            (lv_color_t *)calloc((size_t)preview_w * (size_t)preview_h, sizeof(lv_color_t));
        if (!svc->preview_bufs[i]) {
            free_preview_buffers(svc);
            return -1;
        }
    }

    svc->ready_frame_index = -1;
    svc->ready_frame_seq = 0;
    svc->displayed_frame_index = 0;
    svc->displayed_frame_seq = 0;
    svc->thread_stop = false;
    svc->running = true;

    if (camera_parser_start_workers() != 0) {
        fprintf(stderr, "camera: parallel convert workers failed, fallback to single-core\n");
    }

    if (pthread_create(&svc->capture_thread, NULL, capture_thread_cb, svc) != 0) {
        svc->running = false;
        camera_parser_stop_workers();
        free_preview_buffers(svc);
        return -1;
    }

    return 0;
}

int camera_service_stop(camera_service_t *svc) {
    if (!svc || !svc->running) {
        return 0;
    }

    svc->thread_stop = true;
    pthread_join(svc->capture_thread, NULL);
    svc->running = false;

    camera_parser_stop_workers();
    camera_release_ctx(&svc->camera);
    free_preview_buffers(svc);
    free_rgb_buffers(svc);
    camera_parser_free_scale_lut();

    pthread_mutex_lock(&svc->frame_mutex);
    svc->ready_frame_index = -1;
    svc->connected = false;
    pthread_mutex_unlock(&svc->frame_mutex);

    return 0;
}

bool camera_service_is_running(const camera_service_t *svc) {
    return svc && svc->running;
}

bool camera_service_poll_frame(camera_service_t *svc, int *out_buf_index) {
    bool has_new = false;

    if (!svc || !out_buf_index) {
        return false;
    }

    pthread_mutex_lock(&svc->frame_mutex);
    if (svc->ready_frame_index >= 0 && svc->ready_frame_seq != svc->displayed_frame_seq) {
        *out_buf_index = svc->ready_frame_index;
        svc->displayed_frame_index = *out_buf_index;
        svc->displayed_frame_seq = svc->ready_frame_seq;
        has_new = true;
    }
    pthread_mutex_unlock(&svc->frame_mutex);

    return has_new;
}

const lv_color_t *camera_service_get_preview_buffer(const camera_service_t *svc, int index) {
    if (!svc || index < 0 || index >= PREVIEW_BUFFER_COUNT) {
        return NULL;
    }
    return svc->preview_bufs[index];
}

int camera_service_get_preview_width(const camera_service_t *svc) {
    return svc ? svc->preview_w : 0;
}

int camera_service_get_preview_height(const camera_service_t *svc) {
    return svc ? svc->preview_h : 0;
}

const char *camera_service_pixel_format_name(const camera_service_t *svc) {
    if (!svc || svc->camera.fd < 0) {
        return "none";
    }
    return pixfmt_fourcc_name(svc->camera.pixfmt);
}

bool camera_service_poll_rgb_frame(camera_service_t *svc, uint32_t last_seq, const uint8_t **rgb,
                                   int *w, int *h, uint32_t *out_seq) {
    if (!svc || !rgb || !w || !h || !out_seq) {
        return false;
    }

    bool fresh = false;
    pthread_mutex_lock(&svc->frame_mutex);
    if (svc->rgb_bufs[svc->rgb_read_idx] && svc->rgb_seq != last_seq) {
        *rgb = svc->rgb_bufs[svc->rgb_read_idx];
        *w = svc->rgb_buf_w;
        *h = svc->rgb_buf_h;
        *out_seq = svc->rgb_seq;
        fresh = true;
    }
    pthread_mutex_unlock(&svc->frame_mutex);
    return fresh;
}

void camera_service_close(camera_service_t *svc) {
    if (!svc) {
        return;
    }
    camera_service_stop(svc);
    pthread_mutex_destroy(&svc->frame_mutex);
    free(svc);
}
