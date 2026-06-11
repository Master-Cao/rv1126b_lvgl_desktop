#include "camera_parser.h"
#include "camera_types.h"

#include <linux/videodev2.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <jpeglib.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool g_yuv_tables_ready;
static int yc_table[256];
static int rv_table[256];
static int gu_table[256];
static int gv_table[256];
static int bu_table[256];

static int *src_x_lut;
static int *src_y_lut;
static int lut_dst_w;
static int lut_dst_h;

#define MJPEG_RGB_MAX (CAMERA_CAPTURE_WIDTH * CAMERA_CAPTURE_HEIGHT * 3)
static uint8_t g_mjpeg_rgb_buf[MJPEG_RGB_MAX];

typedef enum {
    CONVERT_JOB_YUV422 = 0,
    CONVERT_JOB_RGB565,
    CONVERT_JOB_RGB888,
    CONVERT_JOB_YUV422_RGB888,
    CONVERT_JOB_RGB565_RGB888,
    CONVERT_JOB_RGB888_COPY,
} convert_job_type_t;

typedef struct {
    convert_job_type_t type;
    const uint8_t *src;
    int src_w;
    int src_h;
    int src_stride;
    uint32_t pixfmt;
    lv_color_t *dst;
    uint8_t *dst_rgb;
    int dst_w;
    int dst_h;
} convert_job_t;

static pthread_mutex_t g_job_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_job_start_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_job_done_cond = PTHREAD_COND_INITIALIZER;
static convert_job_t g_current_job;
static uint32_t g_job_seq;
static int g_workers_done_count;
static volatile bool g_worker_pool_stop;
static pthread_t g_convert_workers[CAMERA_PARSER_CONVERT_WORKER_COUNT];
static int g_convert_worker_ids[CAMERA_PARSER_CONVERT_WORKER_COUNT];
static bool g_convert_workers_started;

static uint8_t clamp_u8(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

void camera_parser_init_yuv_tables(void) {
    if (g_yuv_tables_ready) {
        return;
    }
    for (int i = 0; i < 256; i++) {
        yc_table[i] = 298 * (i - 16);
        rv_table[i] = 409 * (i - 128);
        gu_table[i] = -100 * (i - 128);
        gv_table[i] = -208 * (i - 128);
        bu_table[i] = 516 * (i - 128);
    }
    g_yuv_tables_ready = true;
}

static inline lv_color_t yuv_to_lv_color_fast(uint8_t y, int rv, int g_uv, int bu) {
    int yc = yc_table[y] + 128;
    return lv_color_make(clamp_u8((yc + rv) >> 8), clamp_u8((yc + g_uv) >> 8), clamp_u8((yc + bu) >> 8));
}

int camera_parser_build_scale_lut(int src_w, int src_h, int dst_w, int dst_h) {
    camera_parser_free_scale_lut();

    src_x_lut = (int *)malloc((size_t)dst_w * sizeof(int));
    src_y_lut = (int *)malloc((size_t)dst_h * sizeof(int));
    if (!src_x_lut || !src_y_lut) {
        camera_parser_free_scale_lut();
        return -1;
    }

    for (int x = 0; x < dst_w; x++) {
        src_x_lut[x] = x * src_w / dst_w;
        if (src_x_lut[x] >= src_w) {
            src_x_lut[x] = src_w - 1;
        }
    }
    for (int y = 0; y < dst_h; y++) {
        src_y_lut[y] = y * src_h / dst_h;
        if (src_y_lut[y] >= src_h) {
            src_y_lut[y] = src_h - 1;
        }
    }

    lut_dst_w = dst_w;
    lut_dst_h = dst_h;
    return 0;
}

void camera_parser_free_scale_lut(void) {
    free(src_x_lut);
    free(src_y_lut);
    src_x_lut = NULL;
    src_y_lut = NULL;
    lut_dst_w = 0;
    lut_dst_h = 0;
}

static void convert_yuv422_strip(const convert_job_t *job, int y_start, int y_end) {
    bool is_uyvy = (job->pixfmt == V4L2_PIX_FMT_UYVY);
    bool one_to_one = (job->src_w == job->dst_w && job->src_h == job->dst_h);

    if (y_start < 0) {
        y_start = 0;
    }
    if (y_end > job->dst_h) {
        y_end = job->dst_h;
    }
    if (y_start >= y_end) {
        return;
    }

    camera_parser_init_yuv_tables();

    if (one_to_one) {
        for (int y = y_start; y < y_end; y++) {
            const uint8_t *src_row = job->src + y * job->src_stride;
            lv_color_t *dst_row = job->dst + y * job->dst_w;

            for (int x = 0; x < job->dst_w; x++) {
                const uint8_t *pair = src_row + (x & ~1) * 2;
                uint8_t y0, y1, u, v;

                if (is_uyvy) {
                    u = pair[0];
                    y0 = pair[1];
                    v = pair[2];
                    y1 = pair[3];
                } else {
                    y0 = pair[0];
                    u = pair[1];
                    y1 = pair[2];
                    v = pair[3];
                }

                int rv = rv_table[v];
                int g_uv = gu_table[u] + gv_table[v];
                int bu = bu_table[u];
                dst_row[x] = yuv_to_lv_color_fast((x & 1) ? y1 : y0, rv, g_uv, bu);
            }
        }
        return;
    }

    if (!src_x_lut || !src_y_lut) {
        return;
    }

    for (int y = y_start; y < y_end; y++) {
        int src_y = src_y_lut[y];
        const uint8_t *src_row = job->src + src_y * job->src_stride;
        lv_color_t *dst_row = job->dst + y * job->dst_w;

        for (int x = 0; x < job->dst_w; x++) {
            int src_x = src_x_lut[x];
            const uint8_t *pair = src_row + (src_x & ~1) * 2;
            uint8_t y0, y1, u, v;

            if (is_uyvy) {
                u = pair[0];
                y0 = pair[1];
                v = pair[2];
                y1 = pair[3];
            } else {
                y0 = pair[0];
                u = pair[1];
                y1 = pair[2];
                v = pair[3];
            }

            int rv = rv_table[v];
            int g_uv = gu_table[u] + gv_table[v];
            int bu = bu_table[u];
            dst_row[x] = yuv_to_lv_color_fast((src_x & 1) ? y1 : y0, rv, g_uv, bu);
        }
    }
}

static void convert_rgb565_strip(const convert_job_t *job, int y_start, int y_end) {
    bool one_to_one = (job->src_w == job->dst_w && job->src_h == job->dst_h);

    if (y_start < 0) {
        y_start = 0;
    }
    if (y_end > job->dst_h) {
        y_end = job->dst_h;
    }
    if (y_start >= y_end) {
        return;
    }

    if (one_to_one) {
        for (int y = y_start; y < y_end; y++) {
            const uint8_t *src_row = job->src + y * job->src_stride;
            lv_color_t *dst_row = job->dst + y * job->dst_w;

            for (int x = 0; x < job->dst_w; x++) {
                const uint8_t *pixel_ptr = src_row + x * 2;
                uint16_t pixel = (uint16_t)pixel_ptr[0] | ((uint16_t)pixel_ptr[1] << 8);
                uint8_t r = (uint8_t)(((pixel >> 11) & 0x1f) * 255 / 31);
                uint8_t g = (uint8_t)(((pixel >> 5) & 0x3f) * 255 / 63);
                uint8_t b = (uint8_t)((pixel & 0x1f) * 255 / 31);
                dst_row[x] = lv_color_make(r, g, b);
            }
        }
        return;
    }

    if (!src_x_lut || !src_y_lut) {
        return;
    }

    for (int y = y_start; y < y_end; y++) {
        int src_y = src_y_lut[y];
        const uint8_t *src_row = job->src + src_y * job->src_stride;
        lv_color_t *dst_row = job->dst + y * job->dst_w;

        for (int x = 0; x < job->dst_w; x++) {
            int src_x = src_x_lut[x];
            const uint8_t *pixel_ptr = src_row + src_x * 2;
            uint16_t pixel = (uint16_t)pixel_ptr[0] | ((uint16_t)pixel_ptr[1] << 8);
            uint8_t r = (uint8_t)(((pixel >> 11) & 0x1f) * 255 / 31);
            uint8_t g = (uint8_t)(((pixel >> 5) & 0x3f) * 255 / 63);
            uint8_t b = (uint8_t)((pixel & 0x1f) * 255 / 31);
            dst_row[x] = lv_color_make(r, g, b);
        }
    }
}

static void convert_rgb888_strip(const convert_job_t *job, int y_start, int y_end) {
    bool one_to_one = (job->src_w == job->dst_w && job->src_h == job->dst_h);

    if (y_start < 0) {
        y_start = 0;
    }
    if (y_end > job->dst_h) {
        y_end = job->dst_h;
    }
    if (y_start >= y_end) {
        return;
    }

    if (one_to_one) {
        for (int y = y_start; y < y_end; y++) {
            const uint8_t *src_row = job->src + y * job->src_w * 3;
            lv_color_t *dst_row = job->dst + y * job->dst_w;

            for (int x = 0; x < job->dst_w; x++) {
                const uint8_t *p = src_row + x * 3;
                dst_row[x] = lv_color_make(p[0], p[1], p[2]);
            }
        }
        return;
    }

    if (!src_x_lut || !src_y_lut) {
        return;
    }

    for (int y = y_start; y < y_end; y++) {
        int src_y = src_y_lut[y];
        const uint8_t *src_row = job->src + src_y * job->src_w * 3;
        lv_color_t *dst_row = job->dst + y * job->dst_w;

        for (int x = 0; x < job->dst_w; x++) {
            int src_x = src_x_lut[x];
            const uint8_t *p = src_row + src_x * 3;
            dst_row[x] = lv_color_make(p[0], p[1], p[2]);
        }
    }
}

/* ========= RGB888 输出路径（不缩放）============================================ */

static void convert_yuv422_to_rgb888_strip(const convert_job_t *job, int y_start, int y_end) {
    bool is_uyvy = (job->pixfmt == V4L2_PIX_FMT_UYVY);
    int w = job->src_w;

    if (y_start < 0) y_start = 0;
    if (y_end > job->src_h) y_end = job->src_h;
    if (y_start >= y_end) return;

    camera_parser_init_yuv_tables();

    for (int y = y_start; y < y_end; y++) {
        const uint8_t *src_row = job->src + y * job->src_stride;
        uint8_t *dst_row = job->dst_rgb + y * w * 3;

        for (int x = 0; x < w; x++) {
            const uint8_t *pair = src_row + (x & ~1) * 2;
            uint8_t y0, y1, u, v;
            if (is_uyvy) {
                u = pair[0]; y0 = pair[1]; v = pair[2]; y1 = pair[3];
            } else {
                y0 = pair[0]; u = pair[1]; y1 = pair[2]; v = pair[3];
            }

            int rv = rv_table[v];
            int g_uv = gu_table[u] + gv_table[v];
            int bu = bu_table[u];
            uint8_t y_v = (x & 1) ? y1 : y0;
            int yc = yc_table[y_v] + 128;
            dst_row[x * 3 + 0] = clamp_u8((yc + rv) >> 8);
            dst_row[x * 3 + 1] = clamp_u8((yc + g_uv) >> 8);
            dst_row[x * 3 + 2] = clamp_u8((yc + bu) >> 8);
        }
    }
}

static void convert_rgb565_to_rgb888_strip(const convert_job_t *job, int y_start, int y_end) {
    int w = job->src_w;

    if (y_start < 0) y_start = 0;
    if (y_end > job->src_h) y_end = job->src_h;
    if (y_start >= y_end) return;

    for (int y = y_start; y < y_end; y++) {
        const uint8_t *src_row = job->src + y * job->src_stride;
        uint8_t *dst_row = job->dst_rgb + y * w * 3;

        for (int x = 0; x < w; x++) {
            const uint8_t *pixel_ptr = src_row + x * 2;
            uint16_t pixel = (uint16_t)pixel_ptr[0] | ((uint16_t)pixel_ptr[1] << 8);
            dst_row[x * 3 + 0] = (uint8_t)(((pixel >> 11) & 0x1f) * 255 / 31);
            dst_row[x * 3 + 1] = (uint8_t)(((pixel >> 5) & 0x3f) * 255 / 63);
            dst_row[x * 3 + 2] = (uint8_t)((pixel & 0x1f) * 255 / 31);
        }
    }
}

static void convert_rgb888_copy_strip(const convert_job_t *job, int y_start, int y_end) {
    int w = job->src_w;

    if (y_start < 0) y_start = 0;
    if (y_end > job->src_h) y_end = job->src_h;
    if (y_start >= y_end) return;

    for (int y = y_start; y < y_end; y++) {
        memcpy(job->dst_rgb + y * w * 3, job->src + y * w * 3, (size_t)w * 3);
    }
}

static void run_convert_strip(const convert_job_t *job, int y_start, int y_end) {
    switch (job->type) {
    case CONVERT_JOB_RGB565:
        convert_rgb565_strip(job, y_start, y_end);
        break;
    case CONVERT_JOB_RGB888:
        convert_rgb888_strip(job, y_start, y_end);
        break;
    case CONVERT_JOB_YUV422_RGB888:
        convert_yuv422_to_rgb888_strip(job, y_start, y_end);
        break;
    case CONVERT_JOB_RGB565_RGB888:
        convert_rgb565_to_rgb888_strip(job, y_start, y_end);
        break;
    case CONVERT_JOB_RGB888_COPY:
        convert_rgb888_copy_strip(job, y_start, y_end);
        break;
    case CONVERT_JOB_YUV422:
    default:
        convert_yuv422_strip(job, y_start, y_end);
        break;
    }
}

static void *convert_worker_cb(void *arg) {
    int worker_id = *(int *)arg;
    uint32_t last_seq = 0;

    for (;;) {
        convert_job_t job;

        pthread_mutex_lock(&g_job_mutex);
        while (!g_worker_pool_stop && g_job_seq == last_seq) {
            pthread_cond_wait(&g_job_start_cond, &g_job_mutex);
        }
        if (g_worker_pool_stop) {
            pthread_mutex_unlock(&g_job_mutex);
            break;
        }
        last_seq = g_job_seq;
        job = g_current_job;
        pthread_mutex_unlock(&g_job_mutex);

        int stripe = worker_id + 1;
        int y_start = stripe * job.dst_h / CAMERA_PARSER_CONVERT_STRIPE_COUNT;
        int y_end = (stripe + 1) * job.dst_h / CAMERA_PARSER_CONVERT_STRIPE_COUNT;
        run_convert_strip(&job, y_start, y_end);

        pthread_mutex_lock(&g_job_mutex);
        g_workers_done_count++;
        pthread_cond_signal(&g_job_done_cond);
        pthread_mutex_unlock(&g_job_mutex);
    }

    return NULL;
}

int camera_parser_start_workers(void) {
    if (g_convert_workers_started) {
        return 0;
    }

    g_worker_pool_stop = false;
    g_job_seq = 0;
    g_workers_done_count = 0;

    for (int i = 0; i < CAMERA_PARSER_CONVERT_WORKER_COUNT; i++) {
        g_convert_worker_ids[i] = i;
        if (pthread_create(&g_convert_workers[i], NULL, convert_worker_cb, &g_convert_worker_ids[i]) !=
            0) {
            fprintf(stderr, "camera_parser: convert worker %d create failed\n", i);
            g_worker_pool_stop = true;
            pthread_cond_broadcast(&g_job_start_cond);
            for (int j = 0; j < i; j++) {
                pthread_join(g_convert_workers[j], NULL);
            }
            return -1;
        }
    }

    g_convert_workers_started = true;
    printf("camera_parser: %d-way parallel convert (%d workers + caller)\n",
           CAMERA_PARSER_CONVERT_STRIPE_COUNT, CAMERA_PARSER_CONVERT_WORKER_COUNT);
    return 0;
}

void camera_parser_stop_workers(void) {
    if (!g_convert_workers_started) {
        return;
    }

    pthread_mutex_lock(&g_job_mutex);
    g_worker_pool_stop = true;
    pthread_cond_broadcast(&g_job_start_cond);
    pthread_mutex_unlock(&g_job_mutex);

    for (int i = 0; i < CAMERA_PARSER_CONVERT_WORKER_COUNT; i++) {
        pthread_join(g_convert_workers[i], NULL);
    }
    g_convert_workers_started = false;
    g_worker_pool_stop = false;
}

static void parallel_convert_frame(convert_job_t *job) {
    if (!g_convert_workers_started || job->dst_h <= 0) {
        run_convert_strip(job, 0, job->dst_h);
        return;
    }

    pthread_mutex_lock(&g_job_mutex);
    g_current_job = *job;
    g_workers_done_count = 0;
    g_job_seq++;
    pthread_cond_broadcast(&g_job_start_cond);
    pthread_mutex_unlock(&g_job_mutex);

    int y_start = 0;
    int y_end = job->dst_h / CAMERA_PARSER_CONVERT_STRIPE_COUNT;
    run_convert_strip(job, y_start, y_end);

    pthread_mutex_lock(&g_job_mutex);
    while (g_workers_done_count < CAMERA_PARSER_CONVERT_WORKER_COUNT) {
        pthread_cond_wait(&g_job_done_cond, &g_job_mutex);
    }
    pthread_mutex_unlock(&g_job_mutex);
}

void camera_parser_yuv422_to_lvcolor(const uint8_t *src, int src_w, int src_h, int src_stride,
                                     uint32_t pixfmt, lv_color_t *dst, int dst_w, int dst_h) {
    convert_job_t job;

    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return;
    }

    memset(&job, 0, sizeof(job));
    job.type = CONVERT_JOB_YUV422;
    job.src = src;
    job.src_w = src_w;
    job.src_h = src_h;
    job.src_stride = src_stride;
    job.pixfmt = pixfmt;
    job.dst = dst;
    job.dst_w = dst_w;
    job.dst_h = dst_h;
    parallel_convert_frame(&job);
}

void camera_parser_rgb565_to_lvcolor(const uint8_t *src, int src_w, int src_h, int src_stride,
                                     lv_color_t *dst, int dst_w, int dst_h) {
    convert_job_t job;

    if (!src || !dst) {
        return;
    }

    memset(&job, 0, sizeof(job));
    job.type = CONVERT_JOB_RGB565;
    job.src = src;
    job.src_w = src_w;
    job.src_h = src_h;
    job.src_stride = src_stride;
    job.dst = dst;
    job.dst_w = dst_w;
    job.dst_h = dst_h;
    parallel_convert_frame(&job);
}

void camera_parser_rgb888_to_lvcolor(const uint8_t *src_rgb, int src_w, int src_h, lv_color_t *dst,
                                     int dst_w, int dst_h) {
    convert_job_t job;

    if (!src_rgb || !dst) {
        return;
    }

    memset(&job, 0, sizeof(job));
    job.type = CONVERT_JOB_RGB888;
    job.src = src_rgb;
    job.src_w = src_w;
    job.src_h = src_h;
    job.dst = dst;
    job.dst_w = dst_w;
    job.dst_h = dst_h;
    parallel_convert_frame(&job);
}

void camera_parser_yuv422_to_rgb888(const uint8_t *src, int src_w, int src_h, int src_stride,
                                    uint32_t pixfmt, uint8_t *dst_rgb) {
    convert_job_t job;

    if (!src || !dst_rgb || src_w <= 0 || src_h <= 0) {
        return;
    }

    memset(&job, 0, sizeof(job));
    job.type = CONVERT_JOB_YUV422_RGB888;
    job.src = src;
    job.src_w = src_w;
    job.src_h = src_h;
    job.src_stride = src_stride;
    job.pixfmt = pixfmt;
    job.dst_rgb = dst_rgb;
    job.dst_w = src_w;
    job.dst_h = src_h;
    parallel_convert_frame(&job);
}

void camera_parser_rgb565_to_rgb888(const uint8_t *src, int src_w, int src_h, int src_stride,
                                    uint8_t *dst_rgb) {
    convert_job_t job;

    if (!src || !dst_rgb || src_w <= 0 || src_h <= 0) {
        return;
    }

    memset(&job, 0, sizeof(job));
    job.type = CONVERT_JOB_RGB565_RGB888;
    job.src = src;
    job.src_w = src_w;
    job.src_h = src_h;
    job.src_stride = src_stride;
    job.dst_rgb = dst_rgb;
    job.dst_w = src_w;
    job.dst_h = src_h;
    parallel_convert_frame(&job);
}

void camera_parser_rgb888_copy(const uint8_t *src_rgb, int w, int h, uint8_t *dst_rgb) {
    convert_job_t job;

    if (!src_rgb || !dst_rgb || w <= 0 || h <= 0) {
        return;
    }

    memset(&job, 0, sizeof(job));
    job.type = CONVERT_JOB_RGB888_COPY;
    job.src = src_rgb;
    job.src_w = w;
    job.src_h = h;
    job.dst_rgb = dst_rgb;
    job.dst_w = w;
    job.dst_h = h;
    parallel_convert_frame(&job);
}

struct mjpeg_error_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void mjpeg_error_exit_cb(j_common_ptr cinfo) {
    struct mjpeg_error_mgr *err = (struct mjpeg_error_mgr *)cinfo->err;
    longjmp(err->setjmp_buffer, 1);
}

bool camera_parser_decode_mjpeg(const uint8_t *jpeg_data, size_t jpeg_size, uint8_t *rgb_out,
                                int dst_w, int dst_h) {
    struct jpeg_decompress_struct cinfo;
    struct mjpeg_error_mgr jerr;
    static uint8_t row_buf[2048 * 3];

    if (!jpeg_data || jpeg_size == 0 || !rgb_out) {
        return false;
    }

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = mjpeg_error_exit_cb;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpeg_data, jpeg_size);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int width = (int)cinfo.output_width;
    int height = (int)cinfo.output_height;
    int row_bytes = width * (int)cinfo.output_components;
    int copy_w = (width < dst_w) ? width : dst_w;
    int copy_h = (height < dst_h) ? height : dst_h;

    if (row_bytes > (int)sizeof(row_buf) || copy_w * copy_h * 3 > MJPEG_RGB_MAX) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    JSAMPROW row_ptr[1] = {row_buf};

    while ((int)cinfo.output_scanline < height) {
        int y = (int)cinfo.output_scanline;
        jpeg_read_scanlines(&cinfo, row_ptr, 1);

        if (y < copy_h) {
            memcpy(rgb_out + y * dst_w * 3, row_buf, (size_t)copy_w * 3);
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

uint8_t *camera_parser_mjpeg_rgb_buffer(void) {
    return g_mjpeg_rgb_buf;
}
