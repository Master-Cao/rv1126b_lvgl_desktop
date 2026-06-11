#include "camera_preview.h"

#include "camera_service.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAMERA_LED_SIZE     14
#define CAMERA_LED_BOX_SIZE 28
#define CAMERA_FRAME_PERIOD_MS 16
#define CAMERA_HUD_PAD_X    10
#define CAMERA_HUD_PAD_Y    10
#define CAMERA_KP_DOT_SIZE  6
#define SEG_MASK_COLOR_COUNT 20
#define SEG_MASK_ALPHA       140 /* ~55% 不透明 */

static const uint8_t k_seg_mask_colors[SEG_MASK_COLOR_COUNT][3] = {
    {255, 56, 56},   {255, 157, 151}, {255, 112, 31},  {255, 178, 29},  {207, 210, 49},
    {72, 249, 10},   {146, 204, 23},  {61, 219, 134},  {26, 147, 52},   {0, 212, 187},
    {44, 153, 168},  {0, 194, 255},   {52, 69, 147},   {100, 115, 255}, {0, 24, 236},
    {132, 56, 255},  {82, 0, 133},    {203, 56, 255},  {255, 149, 200}, {255, 55, 199},
};

static void apply_font(lv_obj_t *obj, const lv_font_t *font) {
    if (font) {
        lv_obj_set_style_text_font(obj, font, LV_PART_MAIN);
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t color,
                            const lv_font_t *font) {
    lv_obj_t *label = lv_label_create(parent);
    apply_font(label, font);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_label_set_text(label, text);
    return label;
}

static void set_led_color(lv_obj_t *led, camera_preview_led_t state) {
    lv_color_t color = (state == CAMERA_PREVIEW_LED_GREEN) ? lv_color_hex(0x22c55e)
                                                           : lv_color_hex(0xef4444);
    lv_obj_set_style_bg_color(led, color, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(led, color, LV_PART_MAIN);
}

static void update_hud_label(camera_preview_t *preview) {
    if (!preview || !preview->hud_label) {
        return;
    }
    char buf[64];
    const char *name = preview->hud_metric_name[0] ? preview->hud_metric_name : "OCR";
    if (preview->hud_ocr_ms >= 0) {
        snprintf(buf, sizeof(buf), "%d FPS  |  %s %d ms", preview->hud_fps, name,
                 preview->hud_ocr_ms);
    } else {
        snprintf(buf, sizeof(buf), "%d FPS", preview->hud_fps);
    }
    lv_label_set_text(preview->hud_label, buf);
}

static void account_fps(camera_preview_t *preview) {
    uint32_t now = lv_tick_get();
    if (preview->fps_window_start == 0) {
        preview->fps_window_start = now;
        preview->fps_frame_count = 0;
    }
    preview->fps_frame_count++;
    uint32_t elapsed = now - preview->fps_window_start;
    if (elapsed >= 1000) {
        preview->hud_fps = (int)(preview->fps_frame_count * 1000u / elapsed);
        preview->fps_window_start = now;
        preview->fps_frame_count = 0;
        update_hud_label(preview);
    }
}

static void stream_timer_cb(lv_timer_t *timer) {
    camera_preview_t *preview = (camera_preview_t *)timer->user_data;
    int buf_index;

    if (!preview || !preview->camera_svc) {
        return;
    }

    if (camera_service_poll_frame(preview->camera_svc, &buf_index)) {
        const lv_color_t *pixels = camera_service_get_preview_buffer(preview->camera_svc, buf_index);
        if (pixels && preview->camera_img) {
            preview->img_dsc.data = (const uint8_t *)pixels;
            lv_obj_invalidate(preview->camera_img);
            camera_preview_show_placeholder(preview, false);
            camera_preview_set_led(preview, CAMERA_PREVIEW_LED_GREEN);
            account_fps(preview);
        }
        return;
    }

    if (!camera_service_is_running(preview->camera_svc)) {
        camera_preview_set_led(preview, CAMERA_PREVIEW_LED_RED);
    }
}

camera_preview_t *camera_preview_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                                        const lv_font_t *hint_font) {
    camera_preview_t *preview = (camera_preview_t *)lv_mem_alloc(sizeof(camera_preview_t));
    if (!preview) {
        return NULL;
    }

    memset(preview, 0, sizeof(*preview));

    preview->root = lv_obj_create(parent);
    lv_obj_remove_style_all(preview->root);
    lv_obj_set_size(preview->root, w, h);
    lv_obj_set_style_bg_color(preview->root, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(preview->root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(preview->root, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(preview->root, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(preview->root, lv_color_hex(0x374151), LV_PART_MAIN);
    lv_obj_set_style_pad_all(preview->root, 0, LV_PART_MAIN);
    lv_obj_clear_flag(preview->root, LV_OBJ_FLAG_SCROLLABLE);

    preview->viewport = lv_obj_create(preview->root);
    lv_obj_remove_style_all(preview->viewport);
    lv_obj_set_size(preview->viewport, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(preview->viewport, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(preview->viewport, LV_OBJ_FLAG_SCROLLABLE);

    preview->placeholder = make_label(preview->viewport, "相机预览未连接", lv_color_hex(0x9ca3af),
                                      hint_font);
    lv_obj_center(preview->placeholder);

    preview->status_box = lv_obj_create(preview->root);
    lv_obj_remove_style_all(preview->status_box);
    lv_obj_set_size(preview->status_box, CAMERA_LED_BOX_SIZE, CAMERA_LED_BOX_SIZE);
    lv_obj_set_style_bg_color(preview->status_box, lv_color_hex(0x1f2937), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(preview->status_box, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_radius(preview->status_box, CAMERA_LED_BOX_SIZE / 2, LV_PART_MAIN);
    lv_obj_clear_flag(preview->status_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(preview->status_box, LV_ALIGN_BOTTOM_LEFT, 12, -12);

    preview->status_led = lv_obj_create(preview->status_box);
    lv_obj_remove_style_all(preview->status_led);
    lv_obj_set_size(preview->status_led, CAMERA_LED_SIZE, CAMERA_LED_SIZE);
    lv_obj_set_style_radius(preview->status_led, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(preview->status_led, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(preview->status_led, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(preview->status_led, 6, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(preview->status_led, LV_OPA_50, LV_PART_MAIN);
    lv_obj_center(preview->status_led);
    set_led_color(preview->status_led, CAMERA_PREVIEW_LED_RED);

    return preview;
}

static void write_mask_pixel(uint8_t *buf, int idx, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
#if LV_COLOR_DEPTH == 32
    buf[idx * 4 + 0] = b;
    buf[idx * 4 + 1] = g;
    buf[idx * 4 + 2] = r;
    buf[idx * 4 + 3] = a;
#elif LV_COLOR_DEPTH == 16
    lv_color_t c = lv_color_make(r, g, b);
    buf[idx * 3 + 0] = (uint8_t)(c.full & 0xFF);
    buf[idx * 3 + 1] = (uint8_t)((c.full >> 8) & 0xFF);
    buf[idx * 3 + 2] = a;
#else
    buf[idx * 3 + 0] = r;
    buf[idx * 3 + 1] = g;
    buf[idx * 3 + 2] = b;
#endif
}

static size_t mask_bytes_per_pixel(void)
{
#if LV_COLOR_DEPTH == 32
    return 4;
#else
    return 3;
#endif
}

static void destroy_mask_ui(camera_preview_t *preview)
{
    if (!preview) {
        return;
    }
    if (preview->mask_img) {
        lv_img_set_src(preview->mask_img, NULL);
        lv_obj_del(preview->mask_img);
        preview->mask_img = NULL;
    }
    if (preview->mask_buf) {
        free(preview->mask_buf);
        preview->mask_buf = NULL;
        preview->mask_buf_size = 0;
    }
    lv_img_cache_invalidate_src(&preview->mask_dsc);
    memset(&preview->mask_dsc, 0, sizeof(preview->mask_dsc));
}

static void destroy_stream_ui(camera_preview_t *preview) {
    if (!preview) {
        return;
    }
    if (preview->stream_timer) {
        lv_timer_del(preview->stream_timer);
        preview->stream_timer = NULL;
    }
    camera_preview_clear_boxes(preview);
    camera_preview_clear_points(preview);
    destroy_mask_ui(preview);
    if (preview->overlay_layer) {
        lv_obj_del(preview->overlay_layer);
        preview->overlay_layer = NULL;
    }
    if (preview->hud_label) {
        lv_obj_del(preview->hud_label);
        preview->hud_label = NULL;
    }
    if (preview->camera_img) {
        /* 删除前先把 src 解绑，避免 LVGL 内部 cache / 渲染 pipeline
         * 在下一帧仍引用即将被释放的 preview_bufs。 */
        lv_img_set_src(preview->camera_img, NULL);
        lv_obj_del(preview->camera_img);
        preview->camera_img = NULL;
    }
    /* 显式清掉 image cache 中以本 dsc 为 key 的条目 */
    lv_img_cache_invalidate_src(&preview->img_dsc);

    memset(&preview->img_dsc, 0, sizeof(preview->img_dsc));
    preview->img_w = 0;
    preview->img_h = 0;
    preview->hud_fps = 0;
    preview->hud_ocr_ms = -1;
    preview->fps_window_start = 0;
    preview->fps_frame_count = 0;

    /* 强制整个预览根节点重绘，并立即 flush，确保 close 之前 LVGL 不再持有
     * 即将被释放的 preview_bufs 指针；这是修复二次启动花屏的关键步骤。 */
    if (preview->root) {
        lv_obj_invalidate(preview->root);
    }
    lv_refr_now(NULL);
}

void camera_preview_destroy(camera_preview_t *preview) {
    if (!preview) {
        return;
    }
    camera_preview_stop_stream(preview);
    if (preview->root) {
        lv_obj_del(preview->root);
    }
    lv_mem_free(preview);
}

lv_obj_t *camera_preview_get_viewport(const camera_preview_t *preview) {
    return preview ? preview->viewport : NULL;
}

void camera_preview_set_placeholder(camera_preview_t *preview, const char *text) {
    if (preview && preview->placeholder && text) {
        lv_label_set_text(preview->placeholder, text);
    }
}

void camera_preview_set_led(camera_preview_t *preview, camera_preview_led_t led) {
    if (preview && preview->status_led) {
        set_led_color(preview->status_led, led);
    }
}

void camera_preview_show_placeholder(camera_preview_t *preview, bool show) {
    if (!preview || !preview->placeholder) {
        return;
    }
    if (show) {
        lv_obj_clear_flag(preview->placeholder, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(preview->placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

int camera_preview_start_stream(camera_preview_t *preview, lv_coord_t w, lv_coord_t h) {
    if (!preview || w <= 0 || h <= 0) {
        return -1;
    }
    if (preview->camera_svc) {
        return 0;
    }

    camera_config_t cfg = {
        .device = CAMERA_DEFAULT_DEVICE,
        .width = CAMERA_CAPTURE_WIDTH,
        .height = CAMERA_CAPTURE_HEIGHT,
        .fmt = CAMERA_FMT_UNKNOWN,
    };

    preview->camera_svc = camera_service_open(&cfg);
    if (!preview->camera_svc) {
        return -1;
    }

    if (camera_service_start(preview->camera_svc, (int)w, (int)h) != 0) {
        camera_service_close(preview->camera_svc);
        preview->camera_svc = NULL;
        return -1;
    }

    const lv_color_t *initial = camera_service_get_preview_buffer(preview->camera_svc, 0);
    if (!initial) {
        camera_preview_stop_stream(preview);
        return -1;
    }

    memset(&preview->img_dsc, 0, sizeof(preview->img_dsc));
    preview->img_dsc.header.always_zero = 0;
    preview->img_dsc.header.w = (uint16_t)w;
    preview->img_dsc.header.h = (uint16_t)h;
    preview->img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    preview->img_dsc.data_size = (uint32_t)((size_t)w * (size_t)h * sizeof(lv_color_t));
    preview->img_dsc.data = (const uint8_t *)initial;

    preview->camera_img = lv_img_create(preview->viewport);
    lv_img_set_src(preview->camera_img, &preview->img_dsc);
    lv_obj_set_size(preview->camera_img, w, h);
    lv_obj_center(preview->camera_img);
    lv_obj_move_background(preview->camera_img);

    preview->img_w = (int)w;
    preview->img_h = (int)h;

    preview->overlay_layer = lv_obj_create(preview->viewport);
    lv_obj_remove_style_all(preview->overlay_layer);
    lv_obj_set_size(preview->overlay_layer, w, h);
    lv_obj_center(preview->overlay_layer);
    lv_obj_set_style_bg_opa(preview->overlay_layer, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(preview->overlay_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(preview->overlay_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(preview->overlay_layer);

    /* HUD：左上角 FPS / OCR 耗时（半透明黑底白字） */
    preview->hud_label = lv_label_create(preview->root);
    lv_obj_set_style_text_color(preview->hud_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(preview->hud_label, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(preview->hud_label, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_radius(preview->hud_label, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(preview->hud_label, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(preview->hud_label, 4, LV_PART_MAIN);
    lv_obj_align(preview->hud_label, LV_ALIGN_TOP_LEFT, CAMERA_HUD_PAD_X, CAMERA_HUD_PAD_Y);
    lv_obj_move_foreground(preview->hud_label);
    preview->hud_fps = 0;
    preview->hud_ocr_ms = -1;
    if (!preview->hud_metric_name[0]) {
        snprintf(preview->hud_metric_name, sizeof(preview->hud_metric_name), "OCR");
    }
    preview->fps_window_start = 0;
    preview->fps_frame_count = 0;
    update_hud_label(preview);

    preview->stream_timer =
        lv_timer_create(stream_timer_cb, CAMERA_FRAME_PERIOD_MS, preview);
    stream_timer_cb(preview->stream_timer);

    camera_preview_set_placeholder(preview, "正在获取相机画面...");
    camera_preview_show_placeholder(preview, true);
    camera_preview_set_led(preview, CAMERA_PREVIEW_LED_GREEN);

    return 0;
}

void camera_preview_stop_stream(camera_preview_t *preview) {
    if (!preview) {
        return;
    }

    destroy_stream_ui(preview);

    if (preview->camera_svc) {
        camera_service_close(preview->camera_svc);
        preview->camera_svc = NULL;
    }
    camera_preview_show_placeholder(preview, true);
    camera_preview_set_led(preview, CAMERA_PREVIEW_LED_RED);
}

bool camera_preview_is_streaming(const camera_preview_t *preview) {
    return preview && preview->camera_svc && camera_service_is_running(preview->camera_svc);
}

void camera_preview_clear_boxes(camera_preview_t *preview) {
    if (!preview) {
        return;
    }
    for (int i = 0; i < CAMERA_PREVIEW_MAX_BOXES; i++) {
        if (preview->box_lines[i]) {
            lv_obj_del(preview->box_lines[i]);
            preview->box_lines[i] = NULL;
        }
    }
    preview->box_count = 0;
}

void camera_preview_set_boxes(camera_preview_t *preview, const camera_preview_box_t *boxes,
                              int count, int src_w, int src_h) {
    if (!preview || !preview->overlay_layer) {
        return;
    }

    if (!boxes || count <= 0 || src_w <= 0 || src_h <= 0) {
        camera_preview_clear_boxes(preview);
        return;
    }

    if (count > CAMERA_PREVIEW_MAX_BOXES) {
        count = CAMERA_PREVIEW_MAX_BOXES;
    }

    /* 多余的旧 line 删掉，复用前 count 个 */
    for (int i = count; i < CAMERA_PREVIEW_MAX_BOXES; i++) {
        if (preview->box_lines[i]) {
            lv_obj_del(preview->box_lines[i]);
            preview->box_lines[i] = NULL;
        }
    }

    int img_w = preview->img_w > 0 ? preview->img_w : 1;
    int img_h = preview->img_h > 0 ? preview->img_h : 1;
    float sx = (float)img_w / (float)src_w;
    float sy = (float)img_h / (float)src_h;

    for (int i = 0; i < count; i++) {
        const camera_preview_box_t *b = &boxes[i];
        lv_point_t *pts = preview->box_points[i];
        pts[0].x = (lv_coord_t)(b->x1 * sx);
        pts[0].y = (lv_coord_t)(b->y1 * sy);
        pts[1].x = (lv_coord_t)(b->x2 * sx);
        pts[1].y = (lv_coord_t)(b->y2 * sy);
        pts[2].x = (lv_coord_t)(b->x3 * sx);
        pts[2].y = (lv_coord_t)(b->y3 * sy);
        pts[3].x = (lv_coord_t)(b->x4 * sx);
        pts[3].y = (lv_coord_t)(b->y4 * sy);
        pts[4] = pts[0]; /* 闭合 */

        if (!preview->box_lines[i]) {
            preview->box_lines[i] = lv_line_create(preview->overlay_layer);
            lv_obj_set_style_line_color(preview->box_lines[i], lv_color_hex(0x22c55e),
                                        LV_PART_MAIN);
            lv_obj_set_style_line_width(preview->box_lines[i], 2, LV_PART_MAIN);
            lv_obj_set_style_line_rounded(preview->box_lines[i], false, LV_PART_MAIN);
        }
        lv_line_set_points(preview->box_lines[i], pts, 5);
    }
    preview->box_count = count;
}

void camera_preview_clear_points(camera_preview_t *preview) {
    if (!preview) {
        return;
    }
    for (int i = 0; i < CAMERA_PREVIEW_MAX_POINTS; i++) {
        if (preview->kp_dots[i]) {
            lv_obj_del(preview->kp_dots[i]);
            preview->kp_dots[i] = NULL;
        }
    }
    preview->kp_count = 0;
}

void camera_preview_set_points(camera_preview_t *preview, const camera_preview_point_t *points,
                               int count, int src_w, int src_h) {
    if (!preview || !preview->overlay_layer) {
        return;
    }

    if (!points || count <= 0 || src_w <= 0 || src_h <= 0) {
        camera_preview_clear_points(preview);
        return;
    }

    if (count > CAMERA_PREVIEW_MAX_POINTS) {
        count = CAMERA_PREVIEW_MAX_POINTS;
    }

    /* 多余的旧圆点删掉，复用前 count 个 */
    for (int i = count; i < CAMERA_PREVIEW_MAX_POINTS; i++) {
        if (preview->kp_dots[i]) {
            lv_obj_del(preview->kp_dots[i]);
            preview->kp_dots[i] = NULL;
        }
    }

    int img_w = preview->img_w > 0 ? preview->img_w : 1;
    int img_h = preview->img_h > 0 ? preview->img_h : 1;
    float sx = (float)img_w / (float)src_w;
    float sy = (float)img_h / (float)src_h;

    for (int i = 0; i < count; i++) {
        int cx = (int)(points[i].x * sx);
        int cy = (int)(points[i].y * sy);

        if (!preview->kp_dots[i]) {
            preview->kp_dots[i] = lv_obj_create(preview->overlay_layer);
            lv_obj_remove_style_all(preview->kp_dots[i]);
            lv_obj_set_size(preview->kp_dots[i], CAMERA_KP_DOT_SIZE, CAMERA_KP_DOT_SIZE);
            lv_obj_set_style_radius(preview->kp_dots[i], LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_color(preview->kp_dots[i], lv_color_hex(0x00f2ff), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(preview->kp_dots[i], LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_width(preview->kp_dots[i], 0, LV_PART_MAIN);
            lv_obj_clear_flag(preview->kp_dots[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(preview->kp_dots[i], LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_set_pos(preview->kp_dots[i], cx - CAMERA_KP_DOT_SIZE / 2,
                       cy - CAMERA_KP_DOT_SIZE / 2);
    }
    preview->kp_count = count;
}

void camera_preview_set_ocr_ms(camera_preview_t *preview, int ocr_ms) {
    if (!preview) {
        return;
    }
    preview->hud_ocr_ms = ocr_ms;
    update_hud_label(preview);
}

void camera_preview_clear_seg_mask(camera_preview_t *preview)
{
    if (!preview) {
        return;
    }
    if (preview->mask_img) {
        lv_obj_add_flag(preview->mask_img, LV_OBJ_FLAG_HIDDEN);
    }
}

void camera_preview_set_seg_mask(camera_preview_t *preview, const uint8_t *mask, int src_w,
                                 int src_h)
{
    if (!preview || !preview->overlay_layer || preview->img_w <= 0 || preview->img_h <= 0) {
        return;
    }

    if (!mask || src_w <= 0 || src_h <= 0) {
        camera_preview_clear_seg_mask(preview);
        return;
    }

    int img_w = preview->img_w;
    int img_h = preview->img_h;
    size_t bpp = mask_bytes_per_pixel();
    size_t need = (size_t)img_w * (size_t)img_h * bpp;
    if (!preview->mask_buf || preview->mask_buf_size < need) {
        uint8_t *nb = (uint8_t *)realloc(preview->mask_buf, need);
        if (!nb) {
            return;
        }
        preview->mask_buf = nb;
        preview->mask_buf_size = need;
    }

    for (int dy = 0; dy < img_h; dy++) {
        int sy = (int)((int64_t)dy * src_h / img_h);
        if (sy >= src_h) {
            sy = src_h - 1;
        }
        for (int dx = 0; dx < img_w; dx++) {
            int sx = (int)((int64_t)dx * src_w / img_w);
            if (sx >= src_w) {
                sx = src_w - 1;
            }
            uint8_t m = mask[sy * src_w + sx];
            int idx = dy * img_w + dx;
            if (m == 0) {
                write_mask_pixel(preview->mask_buf, idx, 0, 0, 0, 0);
            } else {
                int ci = (int)m % SEG_MASK_COLOR_COUNT;
                write_mask_pixel(preview->mask_buf, idx, k_seg_mask_colors[ci][0],
                                 k_seg_mask_colors[ci][1], k_seg_mask_colors[ci][2],
                                 SEG_MASK_ALPHA);
            }
        }
    }

    memset(&preview->mask_dsc, 0, sizeof(preview->mask_dsc));
    preview->mask_dsc.header.always_zero = 0;
    preview->mask_dsc.header.w = (uint16_t)img_w;
    preview->mask_dsc.header.h = (uint16_t)img_h;
    preview->mask_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    preview->mask_dsc.data_size = (uint32_t)need;
    preview->mask_dsc.data = preview->mask_buf;

    if (!preview->mask_img) {
        preview->mask_img = lv_img_create(preview->overlay_layer);
        lv_obj_clear_flag(preview->mask_img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(preview->mask_img, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_img_set_src(preview->mask_img, &preview->mask_dsc);
    lv_obj_set_size(preview->mask_img, img_w, img_h);
    lv_obj_center(preview->mask_img);
    lv_obj_move_background(preview->mask_img);
    lv_obj_clear_flag(preview->mask_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(preview->mask_img);
}

void camera_preview_set_metric_name(camera_preview_t *preview, const char *name) {
    if (!preview) {
        return;
    }
    if (!name || !name[0]) {
        snprintf(preview->hud_metric_name, sizeof(preview->hud_metric_name), "OCR");
    } else {
        snprintf(preview->hud_metric_name, sizeof(preview->hud_metric_name), "%s", name);
    }
    update_hud_label(preview);
}
