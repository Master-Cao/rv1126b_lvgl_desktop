/*
 * 目标分割应用页面：相机预览 + 结果面板 + 工具栏，算法为 seg_algo。
 */
#include "seg_app.h"

#include "camera_preview.h"
#include "camera_service.h"
#include "ocr_ui_util.h"
#include "result_panel.h"
#include "seg_algo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEG_SCREEN_W 1280
#define SEG_SCREEN_H 720
#define SEG_PAGE_PAD 24
#define SEG_COL_GAP 12
#define SEG_RIGHT_W 420
#define SEG_TOOLBAR_PAD 10
#define SEG_RIGHT_SPLIT_GAP 10

#define SEG_CONTENT_H (SEG_SCREEN_H - SEG_PAGE_PAD * 2)
#define SEG_PREVIEW_W (SEG_SCREEN_W - SEG_PAGE_PAD * 2 - SEG_COL_GAP - SEG_RIGHT_W)
#define SEG_TOOLBAR_H (OCR_UI_ICON_BTN_SIZE + SEG_TOOLBAR_PAD * 2)
#define SEG_RESULT_H (SEG_CONTENT_H - SEG_RIGHT_SPLIT_GAP - SEG_TOOLBAR_H)

#define SEG_MAX_SUMMARY_LINES 8

typedef struct {
    lv_obj_t *page;
    lv_obj_t *toolbar;
    lv_obj_t *start_btn;
    lv_obj_t *stop_btn;
    lv_obj_t *back_btn;
    camera_preview_t *preview;
    result_panel_t *result;
    const lvgl_app_context_t *ctx;
    const lv_font_t *text_font;
    bool running;

    seg_algo_t *algo;
    lv_timer_t *poll_timer;
    uint32_t last_rgb_seq;
    uint32_t last_result_seq;
} seg_app_state_t;

static seg_app_state_t g_seg;

static void style_clear(lv_obj_t *obj) {
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

static void set_action_enabled(bool start_en, bool stop_en) {
    if (g_seg.start_btn) {
        if (start_en) {
            lv_obj_clear_state(g_seg.start_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(g_seg.start_btn, LV_STATE_DISABLED);
        }
    }
    if (g_seg.stop_btn) {
        if (stop_en) {
            lv_obj_clear_state(g_seg.stop_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(g_seg.stop_btn, LV_STATE_DISABLED);
        }
    }
}

static void stop_seg_pipeline(void) {
    if (g_seg.poll_timer) {
        lv_timer_del(g_seg.poll_timer);
        g_seg.poll_timer = NULL;
    }
    if (g_seg.algo) {
        seg_algo_stop(g_seg.algo);
        seg_algo_destroy(g_seg.algo);
        g_seg.algo = NULL;
    }
    g_seg.last_rgb_seq = 0;
    g_seg.last_result_seq = 0;
}

static void stop_camera_preview(void) {
    if (!g_seg.preview) {
        return;
    }
    camera_preview_clear_boxes(g_seg.preview);
    camera_preview_clear_seg_mask(g_seg.preview);
    camera_preview_stop_stream(g_seg.preview);
    camera_preview_set_placeholder(g_seg.preview, "相机预览未连接");
    camera_preview_show_placeholder(g_seg.preview, true);
    camera_preview_set_led(g_seg.preview, CAMERA_PREVIEW_LED_RED);
}

static void apply_seg_result(const seg_result_set_t *res) {
    if (!res) {
        return;
    }

    if (g_seg.preview) {
        camera_preview_box_t boxes[SEG_MAX_INSTANCES];
        int n = res->count > SEG_MAX_INSTANCES ? SEG_MAX_INSTANCES : res->count;
        for (int i = 0; i < n; i++) {
            boxes[i].x1 = res->instances[i].box.x1;
            boxes[i].y1 = res->instances[i].box.y1;
            boxes[i].x2 = res->instances[i].box.x2;
            boxes[i].y2 = res->instances[i].box.y2;
            boxes[i].x3 = res->instances[i].box.x3;
            boxes[i].y3 = res->instances[i].box.y3;
            boxes[i].x4 = res->instances[i].box.x4;
            boxes[i].y4 = res->instances[i].box.y4;
        }
        camera_preview_set_boxes(g_seg.preview, boxes, n, res->src_w, res->src_h);
        camera_preview_set_ocr_ms(g_seg.preview, res->infer_ms);
        if (res->has_mask && res->seg_mask) {
            camera_preview_set_seg_mask(g_seg.preview, res->seg_mask, res->src_w, res->src_h);
        } else {
            camera_preview_clear_seg_mask(g_seg.preview);
        }
    }

    if (g_seg.result) {
        if (res->count <= 0) {
            result_panel_set_text(g_seg.result, "未检测到目标");
        } else {
            char buf[768];
            int off = 0;
            off += snprintf(buf + off, sizeof(buf) - off, "检测到 %d 个实例\n\n", res->count);
            int show = res->count > SEG_MAX_SUMMARY_LINES ? SEG_MAX_SUMMARY_LINES : res->count;
            for (int i = 0; i < show && off < (int)sizeof(buf) - 48; i++) {
                const seg_item_t *s = &res->instances[i];
                const char *name = (s->class_name[0] != '\0') ? s->class_name : "object";
                off += snprintf(buf + off, sizeof(buf) - off, "%s  %.1f%%\n", name,
                                s->score * 100.0f);
            }
            if (res->count > show) {
                snprintf(buf + off, sizeof(buf) - off, "...(其余 %d 个已省略)",
                         res->count - show);
            }
            result_panel_set_text(g_seg.result, buf);
        }
    }
}

static void seg_poll_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!g_seg.running || !g_seg.algo || !g_seg.preview || !g_seg.preview->camera_svc) {
        return;
    }

    const uint8_t *rgb = NULL;
    int w = 0;
    int h = 0;
    uint32_t seq = 0;
    if (camera_service_poll_rgb_frame(g_seg.preview->camera_svc, g_seg.last_rgb_seq, &rgb, &w,
                                      &h, &seq)) {
        g_seg.last_rgb_seq = seq;
        seg_algo_submit_rgb888(g_seg.algo, rgb, w, h);
    }

    seg_result_set_t result;
    if (seg_algo_get_result(g_seg.algo, &result) == 0) {
        if (result.seq != g_seg.last_result_seq) {
            g_seg.last_result_seq = result.seq;

            /* 立即拷贝 mask，避免 worker 下一帧释放指针后 UI 仍访问 */
            uint8_t *mask_copy = NULL;
            if (result.has_mask && result.seg_mask && result.src_w > 0 && result.src_h > 0) {
                size_t mask_bytes = (size_t)result.src_w * (size_t)result.src_h;
                mask_copy = (uint8_t *)malloc(mask_bytes);
                if (mask_copy) {
                    memcpy(mask_copy, result.seg_mask, mask_bytes);
                    result.seg_mask = mask_copy;
                } else {
                    result.has_mask = false;
                    result.seg_mask = NULL;
                }
            }

            apply_seg_result(&result);
            free(mask_copy);
        }
    }
}

static void on_back_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (g_seg.running) {
        g_seg.running = false;
        stop_seg_pipeline();
        stop_camera_preview();
        set_action_enabled(true, false);
        if (g_seg.result) {
            result_panel_set_text(g_seg.result, "等待分割...");
        }
    }
    if (g_seg.ctx && g_seg.ctx->on_back) {
        g_seg.ctx->on_back(g_seg.ctx->user_data);
    }
}

static void on_start_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!g_seg.preview || !g_seg.result) {
        return;
    }

    if (camera_preview_start_stream(g_seg.preview, SEG_PREVIEW_W, SEG_CONTENT_H) != 0) {
        camera_preview_set_led(g_seg.preview, CAMERA_PREVIEW_LED_RED);
        camera_preview_set_placeholder(g_seg.preview, "相机打开失败");
        camera_preview_show_placeholder(g_seg.preview, true);
        result_panel_set_text(g_seg.result, "相机打开失败");
        return;
    }

    g_seg.algo = seg_algo_create(NULL);
    if (!g_seg.algo || seg_algo_start(g_seg.algo) != 0) {
        char err[320];
        const char *mpath = g_seg.algo ? seg_algo_get_model_path(g_seg.algo) : NULL;
        snprintf(err, sizeof(err),
                 "分割算法启动失败\n\n请确认模型存在:\n%s",
                 mpath ? mpath : "/opt/rv1126b_desktop/models/yolov8_seg.rknn");
        result_panel_set_text(g_seg.result, err);
        if (g_seg.algo) {
            seg_algo_destroy(g_seg.algo);
            g_seg.algo = NULL;
        }
        stop_camera_preview();
        return;
    }

    g_seg.last_rgb_seq = 0;
    g_seg.last_result_seq = 0;
    g_seg.poll_timer = lv_timer_create(seg_poll_timer_cb, 200, NULL);

    g_seg.running = true;
    result_panel_set_text(g_seg.result, "正在分割...");
    set_action_enabled(false, true);
}

static void on_stop_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!g_seg.preview || !g_seg.result) {
        return;
    }

    g_seg.running = false;
    stop_seg_pipeline();
    stop_camera_preview();
    result_panel_set_text(g_seg.result, "等待分割...");
    set_action_enabled(true, false);
}

lv_obj_t *seg_app_create(lv_obj_t *parent, const lvgl_app_context_t *ctx) {
    const lv_font_t *font = ctx ? ctx->font : NULL;
    const lv_font_t *caption_font = ctx ? ctx->caption_font : NULL;
    const lv_coord_t right_x = SEG_PAGE_PAD + SEG_PREVIEW_W + SEG_COL_GAP;

    g_seg.ctx = ctx;
    g_seg.text_font = caption_font ? caption_font : font;
    g_seg.running = false;

    g_seg.page = lv_obj_create(parent);
    lv_obj_set_size(g_seg.page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_seg.page, lv_color_hex(0xf3f4f6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_seg.page, LV_OPA_COVER, LV_PART_MAIN);
    style_clear(g_seg.page);
    lv_obj_clear_flag(g_seg.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_seg.page, LV_OBJ_FLAG_HIDDEN);

    g_seg.preview =
        camera_preview_create(g_seg.page, SEG_PREVIEW_W, SEG_CONTENT_H, g_seg.text_font);
    if (g_seg.preview) {
        lv_obj_align(g_seg.preview->root, LV_ALIGN_TOP_LEFT, SEG_PAGE_PAD, SEG_PAGE_PAD);
        camera_preview_set_placeholder(g_seg.preview, "相机预览未连接");
        camera_preview_set_led(g_seg.preview, CAMERA_PREVIEW_LED_RED);
        camera_preview_set_metric_name(g_seg.preview, "SEG");
    }

    g_seg.result = result_panel_create(g_seg.page, SEG_RIGHT_W, SEG_RESULT_H, g_seg.text_font);
    if (g_seg.result) {
        lv_obj_set_pos(g_seg.result->root, right_x, SEG_PAGE_PAD);
        result_panel_set_text(g_seg.result, "等待分割...");
    }

    g_seg.toolbar = lv_obj_create(g_seg.page);
    lv_obj_remove_style_all(g_seg.toolbar);
    lv_obj_set_size(g_seg.toolbar, SEG_RIGHT_W, SEG_TOOLBAR_H);
    lv_obj_set_pos(g_seg.toolbar, right_x, SEG_PAGE_PAD + SEG_RESULT_H + SEG_RIGHT_SPLIT_GAP);
    lv_obj_set_style_bg_opa(g_seg.toolbar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(g_seg.toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_seg.toolbar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_seg.toolbar, OCR_UI_BTN_GAP, LV_PART_MAIN);
    lv_obj_clear_flag(g_seg.toolbar, LV_OBJ_FLAG_SCROLLABLE);

    g_seg.start_btn = ocr_ui_icon_button_create(g_seg.toolbar, OCR_UI_ICON_BTN_SIZE,
                                                LV_SYMBOL_PLAY, lv_color_hex(0x16a34a),
                                                on_start_clicked, NULL, false);
    g_seg.stop_btn = ocr_ui_icon_button_create(g_seg.toolbar, OCR_UI_ICON_BTN_SIZE,
                                              LV_SYMBOL_STOP, lv_color_hex(0xdc2626),
                                              on_stop_clicked, NULL, false);
    g_seg.back_btn = ocr_ui_icon_button_create(g_seg.toolbar, OCR_UI_ICON_BTN_SIZE,
                                               LV_SYMBOL_LEFT, lv_color_hex(0x374151),
                                               on_back_clicked, NULL, false);
    set_action_enabled(true, false);

    return g_seg.page;
}

void seg_app_destroy(void) {
    if (g_seg.running) {
        stop_seg_pipeline();
        stop_camera_preview();
    }
    if (g_seg.result) {
        result_panel_destroy(g_seg.result);
        g_seg.result = NULL;
    }
    if (g_seg.preview) {
        camera_preview_destroy(g_seg.preview);
        g_seg.preview = NULL;
    }
    g_seg.start_btn = NULL;
    g_seg.stop_btn = NULL;
    g_seg.back_btn = NULL;
    g_seg.toolbar = NULL;
    if (g_seg.page) {
        lv_obj_del(g_seg.page);
        g_seg.page = NULL;
    }
    g_seg.ctx = NULL;
    g_seg.text_font = NULL;
    g_seg.running = false;
}

void seg_app_show(void) {
    if (g_seg.page) {
        lv_obj_clear_flag(g_seg.page, LV_OBJ_FLAG_HIDDEN);
    }
}

void seg_app_hide(void) {
    if (g_seg.running) {
        g_seg.running = false;
        stop_seg_pipeline();
        stop_camera_preview();
        set_action_enabled(true, false);
    }
    if (g_seg.page) {
        lv_obj_add_flag(g_seg.page, LV_OBJ_FLAG_HIDDEN);
    }
}
