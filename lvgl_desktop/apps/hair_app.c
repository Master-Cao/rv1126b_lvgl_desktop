/*
 * 头发丝检测应用页面：复用 OCR 页面的整体框架（相机预览 + 结果面板 + 工具栏），
 * 算法切换到 hair_algo（OpenCV 多角度 Black-hat 管线）。
 */
#include "hair_app.h"

#include "camera_preview.h"
#include "camera_service.h"
#include "hair_algo.h"
#include "ocr_ui_util.h"
#include "result_panel.h"

#include <stdio.h>
#include <string.h>

#define HAIR_SCREEN_W 1280
#define HAIR_SCREEN_H 720
#define HAIR_PAGE_PAD 24
#define HAIR_COL_GAP 12
#define HAIR_RIGHT_W 420
#define HAIR_TOOLBAR_PAD 10
#define HAIR_RIGHT_SPLIT_GAP 10

#define HAIR_CONTENT_H (HAIR_SCREEN_H - HAIR_PAGE_PAD * 2)
#define HAIR_PREVIEW_W (HAIR_SCREEN_W - HAIR_PAGE_PAD * 2 - HAIR_COL_GAP - HAIR_RIGHT_W)
#define HAIR_TOOLBAR_H (OCR_UI_ICON_BTN_SIZE + HAIR_TOOLBAR_PAD * 2)
#define HAIR_RESULT_H (HAIR_CONTENT_H - HAIR_RIGHT_SPLIT_GAP - HAIR_TOOLBAR_H)

#define HAIR_MAX_SUMMARY_LINES 6

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

    hair_algo_t *algo;
    lv_timer_t *poll_timer;
    uint32_t last_rgb_seq;
    uint32_t last_result_seq;
} hair_app_state_t;

static hair_app_state_t g_hair;

static void style_clear(lv_obj_t *obj) {
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

static void set_action_enabled(bool start_en, bool stop_en) {
    if (g_hair.start_btn) {
        if (start_en) {
            lv_obj_clear_state(g_hair.start_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(g_hair.start_btn, LV_STATE_DISABLED);
        }
    }
    if (g_hair.stop_btn) {
        if (stop_en) {
            lv_obj_clear_state(g_hair.stop_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(g_hair.stop_btn, LV_STATE_DISABLED);
        }
    }
}

static void stop_hair_pipeline(void) {
    if (g_hair.poll_timer) {
        lv_timer_del(g_hair.poll_timer);
        g_hair.poll_timer = NULL;
    }
    if (g_hair.algo) {
        hair_algo_stop(g_hair.algo);
        hair_algo_destroy(g_hair.algo);
        g_hair.algo = NULL;
    }
    g_hair.last_rgb_seq = 0;
    g_hair.last_result_seq = 0;
}

static void stop_camera_preview(void) {
    if (!g_hair.preview) {
        return;
    }
    camera_preview_clear_boxes(g_hair.preview);
    camera_preview_stop_stream(g_hair.preview);
    camera_preview_set_placeholder(g_hair.preview, "相机预览未连接");
    camera_preview_show_placeholder(g_hair.preview, true);
    camera_preview_set_led(g_hair.preview, CAMERA_PREVIEW_LED_RED);
}

static void apply_hair_result(const hair_result_set_t *res) {
    if (!res) {
        return;
    }

    if (g_hair.preview) {
        /* hair_line_t.box 与 camera_preview_box_t 同布局；逐条拷贝以解耦内存 */
        camera_preview_box_t boxes[HAIR_MAX_LINES];
        int n = res->count > HAIR_MAX_LINES ? HAIR_MAX_LINES : res->count;
        for (int i = 0; i < n; i++) {
            boxes[i].x1 = res->lines[i].box.x1;
            boxes[i].y1 = res->lines[i].box.y1;
            boxes[i].x2 = res->lines[i].box.x2;
            boxes[i].y2 = res->lines[i].box.y2;
            boxes[i].x3 = res->lines[i].box.x3;
            boxes[i].y3 = res->lines[i].box.y3;
            boxes[i].x4 = res->lines[i].box.x4;
            boxes[i].y4 = res->lines[i].box.y4;
        }
        camera_preview_set_boxes(g_hair.preview, boxes, n, res->src_w, res->src_h);
        camera_preview_set_ocr_ms(g_hair.preview, res->infer_ms);
    }

    if (g_hair.result) {
        if (res->count <= 0) {
            result_panel_set_text(g_hair.result, "未发现头发丝");
        } else {
            char buf[512];
            int off = 0;
            off += snprintf(buf + off, sizeof(buf) - off,
                            "检测到 %d 根候选发丝\n\n", res->count);
            int show = res->count > HAIR_MAX_SUMMARY_LINES ? HAIR_MAX_SUMMARY_LINES : res->count;
            for (int i = 0; i < show && off < (int)sizeof(buf) - 32; i++) {
                off += snprintf(buf + off, sizeof(buf) - off,
                                "#%d  长 %.0fpx  宽 %.1fpx  置信 %.2f\n", i + 1,
                                res->lines[i].length_px, res->lines[i].width_px,
                                res->lines[i].score);
            }
            if (res->count > show) {
                snprintf(buf + off, sizeof(buf) - off, "...(其余 %d 条已省略)",
                         res->count - show);
            }
            result_panel_set_text(g_hair.result, buf);
        }
    }
}

static void hair_poll_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!g_hair.running || !g_hair.algo || !g_hair.preview || !g_hair.preview->camera_svc) {
        return;
    }

    const uint8_t *rgb = NULL;
    int w = 0;
    int h = 0;
    uint32_t seq = 0;
    if (camera_service_poll_rgb_frame(g_hair.preview->camera_svc, g_hair.last_rgb_seq, &rgb, &w, &h,
                                      &seq)) {
        g_hair.last_rgb_seq = seq;
        hair_algo_submit_rgb888(g_hair.algo, rgb, w, h);
    }

    hair_result_set_t result;
    if (hair_algo_get_result(g_hair.algo, &result) == 0) {
        if (result.seq != g_hair.last_result_seq) {
            g_hair.last_result_seq = result.seq;
            apply_hair_result(&result);
        }
    }
}

static void on_back_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (g_hair.running) {
        g_hair.running = false;
        stop_hair_pipeline();
        stop_camera_preview();
        set_action_enabled(true, false);
        if (g_hair.result) {
            result_panel_set_text(g_hair.result, "等待检测...");
        }
    }
    if (g_hair.ctx && g_hair.ctx->on_back) {
        g_hair.ctx->on_back(g_hair.ctx->user_data);
    }
}

static void on_start_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!g_hair.preview || !g_hair.result) {
        return;
    }

    if (camera_preview_start_stream(g_hair.preview, HAIR_PREVIEW_W, HAIR_CONTENT_H) != 0) {
        camera_preview_set_led(g_hair.preview, CAMERA_PREVIEW_LED_RED);
        camera_preview_set_placeholder(g_hair.preview, "相机打开失败");
        camera_preview_show_placeholder(g_hair.preview, true);
        result_panel_set_text(g_hair.result, "相机打开失败");
        return;
    }

    /* 纯 CV 管线无外部模型，传 NULL 走默认参数即可 */
    g_hair.algo = hair_algo_create(NULL);
    if (!g_hair.algo || hair_algo_start(g_hair.algo) != 0) {
        result_panel_set_text(g_hair.result, "头发检测算法启动失败");
        if (g_hair.algo) {
            hair_algo_destroy(g_hair.algo);
            g_hair.algo = NULL;
        }
        stop_camera_preview();
        return;
    }

    g_hair.last_rgb_seq = 0;
    g_hair.last_result_seq = 0;
    g_hair.poll_timer = lv_timer_create(hair_poll_timer_cb, 200, NULL);

    g_hair.running = true;
    result_panel_set_text(g_hair.result, "正在检测...");
    set_action_enabled(false, true);
}

static void on_stop_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!g_hair.preview || !g_hair.result) {
        return;
    }

    g_hair.running = false;
    stop_hair_pipeline();
    stop_camera_preview();
    result_panel_set_text(g_hair.result, "等待检测...");
    set_action_enabled(true, false);
}

lv_obj_t *hair_app_create(lv_obj_t *parent, const lvgl_app_context_t *ctx) {
    const lv_font_t *font = ctx ? ctx->font : NULL;
    const lv_font_t *caption_font = ctx ? ctx->caption_font : NULL;
    const lv_coord_t right_x = HAIR_PAGE_PAD + HAIR_PREVIEW_W + HAIR_COL_GAP;

    g_hair.ctx = ctx;
    g_hair.text_font = caption_font ? caption_font : font;
    g_hair.running = false;

    g_hair.page = lv_obj_create(parent);
    lv_obj_set_size(g_hair.page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_hair.page, lv_color_hex(0xf3f4f6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_hair.page, LV_OPA_COVER, LV_PART_MAIN);
    style_clear(g_hair.page);
    lv_obj_clear_flag(g_hair.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_hair.page, LV_OBJ_FLAG_HIDDEN);

    g_hair.preview =
        camera_preview_create(g_hair.page, HAIR_PREVIEW_W, HAIR_CONTENT_H, g_hair.text_font);
    if (g_hair.preview) {
        lv_obj_align(g_hair.preview->root, LV_ALIGN_TOP_LEFT, HAIR_PAGE_PAD, HAIR_PAGE_PAD);
        camera_preview_set_placeholder(g_hair.preview, "相机预览未连接");
        camera_preview_set_led(g_hair.preview, CAMERA_PREVIEW_LED_RED);
        camera_preview_set_metric_name(g_hair.preview, "HAIR");
    }

    g_hair.result =
        result_panel_create(g_hair.page, HAIR_RIGHT_W, HAIR_RESULT_H, g_hair.text_font);
    if (g_hair.result) {
        lv_obj_set_pos(g_hair.result->root, right_x, HAIR_PAGE_PAD);
        result_panel_set_text(g_hair.result, "等待检测...");
    }

    g_hair.toolbar = lv_obj_create(g_hair.page);
    lv_obj_remove_style_all(g_hair.toolbar);
    lv_obj_set_size(g_hair.toolbar, HAIR_RIGHT_W, HAIR_TOOLBAR_H);
    lv_obj_set_pos(g_hair.toolbar, right_x, HAIR_PAGE_PAD + HAIR_RESULT_H + HAIR_RIGHT_SPLIT_GAP);
    lv_obj_set_style_bg_opa(g_hair.toolbar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(g_hair.toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_hair.toolbar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_hair.toolbar, OCR_UI_BTN_GAP, LV_PART_MAIN);
    lv_obj_clear_flag(g_hair.toolbar, LV_OBJ_FLAG_SCROLLABLE);

    g_hair.start_btn = ocr_ui_icon_button_create(g_hair.toolbar, OCR_UI_ICON_BTN_SIZE,
                                                 LV_SYMBOL_PLAY, lv_color_hex(0x16a34a),
                                                 on_start_clicked, NULL, false);
    g_hair.stop_btn = ocr_ui_icon_button_create(g_hair.toolbar, OCR_UI_ICON_BTN_SIZE,
                                                LV_SYMBOL_STOP, lv_color_hex(0xdc2626),
                                                on_stop_clicked, NULL, false);
    g_hair.back_btn = ocr_ui_icon_button_create(g_hair.toolbar, OCR_UI_ICON_BTN_SIZE,
                                                LV_SYMBOL_LEFT, lv_color_hex(0x374151),
                                                on_back_clicked, NULL, false);
    set_action_enabled(true, false);

    return g_hair.page;
}

void hair_app_destroy(void) {
    if (g_hair.running) {
        stop_hair_pipeline();
        stop_camera_preview();
    }
    if (g_hair.result) {
        result_panel_destroy(g_hair.result);
        g_hair.result = NULL;
    }
    if (g_hair.preview) {
        camera_preview_destroy(g_hair.preview);
        g_hair.preview = NULL;
    }
    g_hair.start_btn = NULL;
    g_hair.stop_btn = NULL;
    g_hair.back_btn = NULL;
    g_hair.toolbar = NULL;
    if (g_hair.page) {
        lv_obj_del(g_hair.page);
        g_hair.page = NULL;
    }
    g_hair.ctx = NULL;
    g_hair.text_font = NULL;
    g_hair.running = false;
}

void hair_app_show(void) {
    if (g_hair.page) {
        lv_obj_clear_flag(g_hair.page, LV_OBJ_FLAG_HIDDEN);
    }
}

void hair_app_hide(void) {
    if (g_hair.running) {
        g_hair.running = false;
        stop_hair_pipeline();
        stop_camera_preview();
        set_action_enabled(true, false);
    }
    if (g_hair.page) {
        lv_obj_add_flag(g_hair.page, LV_OBJ_FLAG_HIDDEN);
    }
}
