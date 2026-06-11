#ifndef CAMERA_SERVICE_H
#define CAMERA_SERVICE_H

#include "camera_types.h"

#include <lvgl/lvgl.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct camera_service camera_service_t;

typedef struct {
    const char *device;
    int width;
    int height;
    camera_fmt_t fmt; /* 采集端由 V4L2 协商，此处仅作记录 */
} camera_config_t;

camera_service_t *camera_service_open(const camera_config_t *cfg);

/* 启动采集线程；preview_w/h 为 LVGL 预览目标尺寸 */
int camera_service_start(camera_service_t *svc, int preview_w, int preview_h);

int camera_service_stop(camera_service_t *svc);

bool camera_service_is_running(const camera_service_t *svc);

/* UI 线程：若有新帧则返回 true，并给出可显示的缓冲下标 */
bool camera_service_poll_frame(camera_service_t *svc, int *out_buf_index);

const lv_color_t *camera_service_get_preview_buffer(const camera_service_t *svc, int index);

int camera_service_get_preview_width(const camera_service_t *svc);
int camera_service_get_preview_height(const camera_service_t *svc);

const char *camera_service_pixel_format_name(const camera_service_t *svc);

/* OCR/算法路径：以采集原始尺寸返回最新一帧 RGB888，并回填序号。
 * 调用方传入上次的 last_seq；若返回 true，输出指针在下一次 publish 前保持有效，
 * 不可跨线程长时间使用，建议立即拷贝/提交。*/
bool camera_service_poll_rgb_frame(camera_service_t *svc, uint32_t last_seq,
                                   const uint8_t **rgb, int *w, int *h, uint32_t *out_seq);

void camera_service_close(camera_service_t *svc);

#endif
