/*
 * 车牌检测应用页面：相机预览画框 + 结果面板，算法为 plate_algo（仅检测，不识别）。
 */
#include "plate_app.h"

#include "camera_preview.h"
#include "camera_service.h"
#include "ocr_ui_util.h"
#include "plate_algo.h"
#include "result_panel.h"

#include <stdio.h>
#include <string.h>

#define PLATE_SCREEN_W 1280
#define PLATE_SCREEN_H 720
#define PLATE_PAGE_PAD 24
#define PLATE_COL_GAP 12
#define PLATE_RIGHT_W 420
#define PLATE_TOOLBAR_PAD 10
#define PLATE_RIGHT_SPLIT_GAP 10

#define PLATE_CONTENT_H (PLATE_SCREEN_H - PLATE_PAGE_PAD * 2)
#define PLATE_PREVIEW_W (PLATE_SCREEN_W - PLATE_PAGE_PAD * 2 - PLATE_COL_GAP - PLATE_RIGHT_W)
#define PLATE_TOOLBAR_H (OCR_UI_ICON_BTN_SIZE + PLATE_TOOLBAR_PAD * 2)
#define PLATE_RESULT_H (PLATE_CONTENT_H - PLATE_RIGHT_SPLIT_GAP - PLATE_TOOLBAR_H)

#define PLATE_MAX_SUMMARY_LINES 8

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

    plate_algo_t *algo;
    lv_timer_t *poll_timer;
    uint32_t last_rgb_seq;
    uint32_t last_result_seq;
} plate_app_state_t;

static plate_app_state_t g_plate;

static void style_clear(lv_obj_t *obj) {
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

static void set_action_enabled(bool start_en, bool stop_en) {
    if (g_plate.start_btn) {
        if (start_en) {
            lv_obj_clear_state(g_plate.start_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(g_plate.start_btn, LV_STATE_DISABLED);
        }
    }
    if (g_plate.stop_btn) {
        if (stop_en) {
            lv_obj_clear_state(g_plate.stop_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(g_plate.stop_btn, LV_STATE_DISABLED);
        }
    }
}

static void stop_plate_pipeline(void) {
    if (g_plate.poll_timer) {
        lv_timer_del(g_plate.poll_timer);
        g_plate.poll_timer = NULL;
    }
    if (g_plate.algo) {
        plate_algo_stop(g_plate.algo);
        plate_algo_destroy(g_plate.algo);
        g_plate.algo = NULL;
    }
    g_plate.last_rgb_seq = 0;
    g_plate.last_result_seq = 0;
}

static void stop_camera_preview(void) {
    if (!g_plate.preview) {
        return;
    }
    camera_preview_clear_boxes(g_plate.preview);
    camera_preview_stop_stream(g_plate.preview);
    camera_preview_set_placeholder(g_plate.preview, "相机预览未连接");
    camera_preview_show_placeholder(g_plate.preview, true);
    camera_preview_set_led(g_plate.preview, CAMERA_PREVIEW_LED_RED);
}

static void apply_plate_result(const plate_result_set_t *res) {
    if (!res) {
        return;
    }

    if (g_plate.preview) {
        camera_preview_box_t boxes[PLATE_MAX_PLATES];
        int n = res->count > PLATE_MAX_PLATES ? PLATE_MAX_PLATES : res->count;
        for (int i = 0; i < n; i++) {
            boxes[i].x1 = res->plates[i].box.x1;
            boxes[i].y1 = res->plates[i].box.y1;
            boxes[i].x2 = res->plates[i].box.x2;
            boxes[i].y2 = res->plates[i].box.y2;
            boxes[i].x3 = res->plates[i].box.x3;
            boxes[i].y3 = res->plates[i].box.y3;
            boxes[i].x4 = res->plates[i].box.x4;
            boxes[i].y4 = res->plates[i].box.y4;
        }
        /* n==0 时也会清除预览上的旧框 */
        camera_preview_set_boxes(g_plate.preview, boxes, n, res->src_w, res->src_h);
        camera_preview_set_ocr_ms(g_plate.preview, res->det_ms);
    }

    if (g_plate.result) {
        if (res->count <= 0) {
            result_panel_set_text(g_plate.result, "未检测到车牌");
        } else {
            char buf[768];
            int off = 0;
            off += snprintf(buf + off, sizeof(buf) - off, "检测到 %d 个目标\n\n", res->count);
            int show =
                res->count > PLATE_MAX_SUMMARY_LINES ? PLATE_MAX_SUMMARY_LINES : res->count;
            for (int i = 0; i < show && off < (int)sizeof(buf) - 48; i++) {
                const plate_item_t *p = &res->plates[i];
                off += snprintf(buf + off, sizeof(buf) - off, "目标 %d  %.1f%%\n", i + 1,
                                p->det_score * 100.0f);
            }
            if (res->count > show) {
                snprintf(buf + off, sizeof(buf) - off, "...(其余 %d 个已省略)",
                         res->count - show);
            }
            result_panel_set_text(g_plate.result, buf);
        }
    }
}

static void plate_poll_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!g_plate.running || !g_plate.algo || !g_plate.preview || !g_plate.preview->camera_svc) {
        return;
    }

    const uint8_t *rgb = NULL;
    int w = 0;
    int h = 0;
    uint32_t seq = 0;
    if (camera_service_poll_rgb_frame(g_plate.preview->camera_svc, g_plate.last_rgb_seq, &rgb,
                                      &w, &h, &seq)) {
        g_plate.last_rgb_seq = seq;
        plate_algo_submit_rgb888(g_plate.algo, rgb, w, h);
    }

    plate_result_set_t result;
    if (plate_algo_get_result(g_plate.algo, &result) == 0) {
        if (result.seq != g_plate.last_result_seq) {
            g_plate.last_result_seq = result.seq;
            apply_plate_result(&result);
        }
    }
}

static void on_back_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (g_plate.running) {
        g_plate.running = false;
        stop_plate_pipeline();
        stop_camera_preview();
        set_action_enabled(true, false);
        if (g_plate.result) {
            result_panel_set_text(g_plate.result, "等待检测...");
        }
    }
    if (g_plate.ctx && g_plate.ctx->on_back) {
        g_plate.ctx->on_back(g_plate.ctx->user_data);
    }
}

static void on_start_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!g_plate.preview || !g_plate.result) {
        return;
    }

    if (camera_preview_start_stream(g_plate.preview, PLATE_PREVIEW_W, PLATE_CONTENT_H) != 0) {
        camera_preview_set_led(g_plate.preview, CAMERA_PREVIEW_LED_RED);
        camera_preview_set_placeholder(g_plate.preview, "相机打开失败");
        camera_preview_show_placeholder(g_plate.preview, true);
        result_panel_set_text(g_plate.result, "相机打开失败");
        return;
    }

    g_plate.algo = plate_algo_create(NULL);
    if (!g_plate.algo || plate_algo_start(g_plate.algo) != 0) {
        char err[320];
        const char *mpath = g_plate.algo ? plate_algo_get_det_model_path(g_plate.algo) : NULL;
        snprintf(err, sizeof(err), "车牌检测启动失败\n\n请确认模型存在:\n%s",
                 mpath ? mpath : "/opt/rv1126b_desktop/models/plate_det.rknn");
        result_panel_set_text(g_plate.result, err);
        if (g_plate.algo) {
            plate_algo_destroy(g_plate.algo);
            g_plate.algo = NULL;
        }
        stop_camera_preview();
        return;
    }

    g_plate.last_rgb_seq = 0;
    g_plate.last_result_seq = 0;
    g_plate.poll_timer = lv_timer_create(plate_poll_timer_cb, 200, NULL);

    g_plate.running = true;
    result_panel_set_text(g_plate.result, "正在检测车牌...");
    set_action_enabled(false, true);
}

static void on_stop_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!g_plate.preview || !g_plate.result) {
        return;
    }

    g_plate.running = false;
    stop_plate_pipeline();
    stop_camera_preview();
    result_panel_set_text(g_plate.result, "等待检测...");
    set_action_enabled(true, false);
}

lv_obj_t *plate_app_create(lv_obj_t *parent, const lvgl_app_context_t *ctx) {
    const lv_font_t *font = ctx ? ctx->font : NULL;
    const lv_font_t *caption_font = ctx ? ctx->caption_font : NULL;
    const lv_coord_t right_x = PLATE_PAGE_PAD + PLATE_PREVIEW_W + PLATE_COL_GAP;

    g_plate.ctx = ctx;
    g_plate.text_font = caption_font ? caption_font : font;
    g_plate.running = false;

    g_plate.page = lv_obj_create(parent);
    lv_obj_set_size(g_plate.page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_plate.page, lv_color_hex(0xf3f4f6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_plate.page, LV_OPA_COVER, LV_PART_MAIN);
    style_clear(g_plate.page);
    lv_obj_clear_flag(g_plate.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_plate.page, LV_OBJ_FLAG_HIDDEN);

    g_plate.preview =
        camera_preview_create(g_plate.page, PLATE_PREVIEW_W, PLATE_CONTENT_H, g_plate.text_font);
    if (g_plate.preview) {
        lv_obj_align(g_plate.preview->root, LV_ALIGN_TOP_LEFT, PLATE_PAGE_PAD, PLATE_PAGE_PAD);
        camera_preview_set_placeholder(g_plate.preview, "相机预览未连接");
        camera_preview_set_led(g_plate.preview, CAMERA_PREVIEW_LED_RED);
        camera_preview_set_metric_name(g_plate.preview, "PLATE");
    }

    g_plate.result =
        result_panel_create(g_plate.page, PLATE_RIGHT_W, PLATE_RESULT_H, g_plate.text_font);
    if (g_plate.result) {
        lv_obj_set_pos(g_plate.result->root, right_x, PLATE_PAGE_PAD);
        result_panel_set_text(g_plate.result, "等待检测...");
    }

    g_plate.toolbar = lv_obj_create(g_plate.page);
    lv_obj_remove_style_all(g_plate.toolbar);
    lv_obj_set_size(g_plate.toolbar, PLATE_RIGHT_W, PLATE_TOOLBAR_H);
    lv_obj_set_pos(g_plate.toolbar, right_x, PLATE_PAGE_PAD + PLATE_RESULT_H + PLATE_RIGHT_SPLIT_GAP);
    lv_obj_set_style_bg_opa(g_plate.toolbar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(g_plate.toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_plate.toolbar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_plate.toolbar, OCR_UI_BTN_GAP, LV_PART_MAIN);
    lv_obj_clear_flag(g_plate.toolbar, LV_OBJ_FLAG_SCROLLABLE);

    g_plate.start_btn = ocr_ui_icon_button_create(g_plate.toolbar, OCR_UI_ICON_BTN_SIZE,
                                                  LV_SYMBOL_PLAY, lv_color_hex(0x16a34a),
                                                  on_start_clicked, NULL, false);
    g_plate.stop_btn = ocr_ui_icon_button_create(g_plate.toolbar, OCR_UI_ICON_BTN_SIZE,
                                                 LV_SYMBOL_STOP, lv_color_hex(0xdc2626),
                                                 on_stop_clicked, NULL, false);
    g_plate.back_btn = ocr_ui_icon_button_create(g_plate.toolbar, OCR_UI_ICON_BTN_SIZE,
                                                 LV_SYMBOL_LEFT, lv_color_hex(0x374151),
                                                 on_back_clicked, NULL, false);
    set_action_enabled(true, false);

    return g_plate.page;
}

void plate_app_destroy(void) {
    if (g_plate.running) {
        stop_plate_pipeline();
        stop_camera_preview();
    }
    if (g_plate.result) {
        result_panel_destroy(g_plate.result);
        g_plate.result = NULL;
    }
    if (g_plate.preview) {
        camera_preview_destroy(g_plate.preview);
        g_plate.preview = NULL;
    }
    g_plate.start_btn = NULL;
    g_plate.stop_btn = NULL;
    g_plate.back_btn = NULL;
    g_plate.toolbar = NULL;
    if (g_plate.page) {
        lv_obj_del(g_plate.page);
        g_plate.page = NULL;
    }
    g_plate.ctx = NULL;
    g_plate.text_font = NULL;
    g_plate.running = false;
}

void plate_app_show(void) {
    if (g_plate.page) {
        lv_obj_clear_flag(g_plate.page, LV_OBJ_FLAG_HIDDEN);
    }
}

void plate_app_hide(void) {
    if (g_plate.running) {
        g_plate.running = false;
        stop_plate_pipeline();
        stop_camera_preview();
        set_action_enabled(true, false);
    }
    if (g_plate.page) {
        lv_obj_add_flag(g_plate.page, LV_OBJ_FLAG_HIDDEN);
    }
}
