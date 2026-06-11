#ifndef CAMERA_PREVIEW_H
#define CAMERA_PREVIEW_H

#include <lvgl/lvgl.h>
#include <stdbool.h>

struct camera_service;

#define CAMERA_PREVIEW_MAX_BOXES 64
#define CAMERA_PREVIEW_MAX_POINTS 128

typedef enum {
    CAMERA_PREVIEW_LED_RED = 0,
    CAMERA_PREVIEW_LED_GREEN,
} camera_preview_led_t;

/* 与 ocr_algo.h::ocr_box_t 字段完全一致；调用方可直接强转。 */
typedef struct {
    int x1, y1;
    int x2, y2;
    int x3, y3;
    int x4, y4;
} camera_preview_box_t;

/* 关键点（如人脸 5 点），坐标按 (src_w,src_h) 映射到预览图。 */
typedef struct {
    int x;
    int y;
} camera_preview_point_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *viewport;
    lv_obj_t *placeholder;
    lv_obj_t *status_box;
    lv_obj_t *status_led;
    lv_obj_t *camera_img;
    lv_img_dsc_t img_dsc;
    lv_timer_t *stream_timer;
    struct camera_service *camera_svc;

    /* OCR 检测框叠加层 */
    lv_obj_t *overlay_layer;
    lv_obj_t *box_lines[CAMERA_PREVIEW_MAX_BOXES];
    lv_point_t box_points[CAMERA_PREVIEW_MAX_BOXES][5];
    int box_count;
    /* 关键点圆点叠加（如人脸 5 点） */
    lv_obj_t *kp_dots[CAMERA_PREVIEW_MAX_POINTS];
    int kp_count;
    /* 实例分割蒙版叠加（半透明彩色，0=透明） */
    lv_obj_t *mask_img;
    lv_img_dsc_t mask_dsc;
    uint8_t *mask_buf;
    size_t mask_buf_size;
    int img_w;
    int img_h;

    /* 左上角 HUD：FPS + 算法耗时 */
    lv_obj_t *hud_label;
    int hud_fps;
    int hud_ocr_ms;
    char hud_metric_name[16]; /* 默认 "OCR"，可由 camera_preview_set_metric_name 修改 */

    /* FPS 统计窗口 */
    uint32_t fps_window_start;
    uint32_t fps_frame_count;
} camera_preview_t;

/* 创建相机预览区域；hint_font 用于中间占位小字。 */
camera_preview_t *camera_preview_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                                        const lv_font_t *hint_font);

void camera_preview_destroy(camera_preview_t *preview);

lv_obj_t *camera_preview_get_viewport(const camera_preview_t *preview);

void camera_preview_set_placeholder(camera_preview_t *preview, const char *text);

void camera_preview_set_led(camera_preview_t *preview, camera_preview_led_t led);

void camera_preview_show_placeholder(camera_preview_t *preview, bool show);

/* 启动 V4L2 取流并在 viewport 内显示；preview 区域尺寸为 w x h */
int camera_preview_start_stream(camera_preview_t *preview, lv_coord_t w, lv_coord_t h);

void camera_preview_stop_stream(camera_preview_t *preview);

bool camera_preview_is_streaming(const camera_preview_t *preview);

/* 在 viewport 上绘制检测框；boxes 坐标系按 (src_w,src_h) 归一化后映射到 img_w x img_h。 */
void camera_preview_set_boxes(camera_preview_t *preview, const camera_preview_box_t *boxes,
                              int count, int src_w, int src_h);

void camera_preview_clear_boxes(camera_preview_t *preview);

/* 在 viewport 上绘制关键点圆点；points 坐标系按 (src_w,src_h) 映射到 img_w x img_h。 */
void camera_preview_set_points(camera_preview_t *preview, const camera_preview_point_t *points,
                               int count, int src_w, int src_h);

void camera_preview_clear_points(camera_preview_t *preview);

/* 在预览上叠加分割蒙版；mask 为原图尺寸，非 0 像素为 class_id+1。 */
void camera_preview_set_seg_mask(camera_preview_t *preview, const uint8_t *mask, int src_w,
                                 int src_h);

void camera_preview_clear_seg_mask(camera_preview_t *preview);

/* 更新左上角 HUD；ocr_ms < 0 时仅显示 FPS。 */
void camera_preview_set_ocr_ms(camera_preview_t *preview, int ocr_ms);

/* 修改 HUD 中显示在耗时前的算法名（如 "OCR"、"HAIR"），默认 "OCR"。 */
void camera_preview_set_metric_name(camera_preview_t *preview, const char *name);

#endif
