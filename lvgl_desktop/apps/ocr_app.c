#include "ocr_app.h"

#include "camera_preview.h"
#include "camera_service.h"
#include "ocr_algo.h"
#include "ocr_ui_util.h"
#include "result_panel.h"

#include <stdio.h>
#include <string.h>

#define OCR_SCREEN_W 1280
#define OCR_SCREEN_H 720
#define OCR_PAGE_PAD 24
#define OCR_COL_GAP 12
#define OCR_RIGHT_W 420
#define OCR_TOOLBAR_PAD 10
#define OCR_RIGHT_SPLIT_GAP 10

#define OCR_CONTENT_H (OCR_SCREEN_H - OCR_PAGE_PAD * 2)
#define OCR_PREVIEW_W (OCR_SCREEN_W - OCR_PAGE_PAD * 2 - OCR_COL_GAP - OCR_RIGHT_W)
#define OCR_TOOLBAR_H (OCR_UI_ICON_BTN_SIZE + OCR_TOOLBAR_PAD * 2)
#define OCR_RESULT_H (OCR_CONTENT_H - OCR_RIGHT_SPLIT_GAP - OCR_TOOLBAR_H)

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

    ocr_algo_t *algo;
    lv_timer_t *poll_timer;
    uint32_t last_rgb_seq;
    uint32_t last_result_seq;
} ocr_app_state_t;

static ocr_app_state_t g_ocr;

static void style_clear(lv_obj_t *obj) {
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

static void set_action_enabled(bool start_en, bool stop_en) {
    if (g_ocr.start_btn) {
        if (start_en) {
            lv_obj_clear_state(g_ocr.start_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(g_ocr.start_btn, LV_STATE_DISABLED);
        }
    }
    if (g_ocr.stop_btn) {
        if (stop_en) {
            lv_obj_clear_state(g_ocr.stop_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(g_ocr.stop_btn, LV_STATE_DISABLED);
        }
    }
}

static void stop_ocr_pipeline(void) {
    if (g_ocr.poll_timer) {
        lv_timer_del(g_ocr.poll_timer);
        g_ocr.poll_timer = NULL;
    }
    if (g_ocr.algo) {
        ocr_algo_stop(g_ocr.algo);
        ocr_algo_destroy(g_ocr.algo);
        g_ocr.algo = NULL;
    }
    g_ocr.last_rgb_seq = 0;
    g_ocr.last_result_seq = 0;
}

static void stop_camera_preview(void) {
    if (!g_ocr.preview) {
        return;
    }
    camera_preview_clear_boxes(g_ocr.preview);
    camera_preview_stop_stream(g_ocr.preview);
    camera_preview_set_placeholder(g_ocr.preview, "相机预览未连接");
    camera_preview_show_placeholder(g_ocr.preview, true);
    camera_preview_set_led(g_ocr.preview, CAMERA_PREVIEW_LED_RED);
}

static void apply_ocr_result(const ocr_result_set_t *res) {
    if (!res) {
        return;
    }

    if (g_ocr.preview) {
        /* lines[i].box 字段顺序与 camera_preview_box_t 一致，但 ocr_text_line_t 包含
         * 额外的 text/score 字段，box 不连续；先打包到本地数组再传入。 */
        camera_preview_box_t boxes[OCR_MAX_LINES];
        int n = res->count > OCR_MAX_LINES ? OCR_MAX_LINES : res->count;
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
        camera_preview_set_boxes(g_ocr.preview, boxes, n, res->src_w, res->src_h);
    }

    if (g_ocr.result) {
        if (res->count <= 0) {
            result_panel_set_text(g_ocr.result, "未识别到文字");
        } else {
            const char *line_ptrs[OCR_MAX_LINES];
            int n = res->count > OCR_MAX_LINES ? OCR_MAX_LINES : res->count;
            for (int i = 0; i < n; i++) {
                line_ptrs[i] = res->lines[i].text;
            }
            result_panel_set_lines(g_ocr.result, line_ptrs, n);
        }
    }

    if (g_ocr.preview) {
        camera_preview_set_ocr_ms(g_ocr.preview, res->infer_ms);
    }
}

static void ocr_poll_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!g_ocr.running || !g_ocr.algo || !g_ocr.preview || !g_ocr.preview->camera_svc) {
        return;
    }

    const uint8_t *rgb = NULL;
    int w = 0;
    int h = 0;
    uint32_t seq = 0;
    if (camera_service_poll_rgb_frame(g_ocr.preview->camera_svc, g_ocr.last_rgb_seq, &rgb, &w, &h,
                                      &seq)) {
        g_ocr.last_rgb_seq = seq;
        ocr_algo_submit_rgb888(g_ocr.algo, rgb, w, h);
    }

    ocr_result_set_t result;
    if (ocr_algo_get_result(g_ocr.algo, &result) == 0) {
        if (result.seq != g_ocr.last_result_seq) {
            g_ocr.last_result_seq = result.seq;
            apply_ocr_result(&result);
        }
    }
}

static void on_back_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (g_ocr.running) {
        g_ocr.running = false;
        stop_ocr_pipeline();
        stop_camera_preview();
        set_action_enabled(true, false);
        if (g_ocr.result) {
            result_panel_set_text(g_ocr.result, "等待识别...");
        }
    }
    if (g_ocr.ctx && g_ocr.ctx->on_back) {
        g_ocr.ctx->on_back(g_ocr.ctx->user_data);
    }
}

static void on_start_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!g_ocr.preview || !g_ocr.result) {
        return;
    }

    if (camera_preview_start_stream(g_ocr.preview, OCR_PREVIEW_W, OCR_CONTENT_H) != 0) {
        camera_preview_set_led(g_ocr.preview, CAMERA_PREVIEW_LED_RED);
        camera_preview_set_placeholder(g_ocr.preview, "相机打开失败");
        camera_preview_show_placeholder(g_ocr.preview, true);
        result_panel_set_text(g_ocr.result, "相机打开失败");
        return;
    }

    /* 启动 OCR 推理线程；模型路径走环境变量或默认 /opt/rv1126b_desktop/models/ */
    g_ocr.algo = ocr_algo_create(NULL, NULL);
    if (!g_ocr.algo || ocr_algo_start(g_ocr.algo) != 0) {
        result_panel_set_text(g_ocr.result, "OCR 模型加载失败");
        if (g_ocr.algo) {
            ocr_algo_destroy(g_ocr.algo);
            g_ocr.algo = NULL;
        }
        stop_camera_preview();
        return;
    }

    g_ocr.last_rgb_seq = 0;
    g_ocr.last_result_seq = 0;
    g_ocr.poll_timer = lv_timer_create(ocr_poll_timer_cb, 200, NULL);

    g_ocr.running = true;
    result_panel_set_text(g_ocr.result, "正在识别...");
    set_action_enabled(false, true);
}

static void on_stop_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!g_ocr.preview || !g_ocr.result) {
        return;
    }

    g_ocr.running = false;
    stop_ocr_pipeline();
    stop_camera_preview();
    result_panel_set_text(g_ocr.result, "等待识别...");
    set_action_enabled(true, false);
}

lv_obj_t *ocr_app_create(lv_obj_t *parent, const lvgl_app_context_t *ctx) {
    const lv_font_t *font = ctx ? ctx->font : NULL;
    const lv_font_t *caption_font = ctx ? ctx->caption_font : NULL;
    const lv_coord_t right_x = OCR_PAGE_PAD + OCR_PREVIEW_W + OCR_COL_GAP;

    g_ocr.ctx = ctx;
    g_ocr.text_font = caption_font ? caption_font : font;
    g_ocr.running = false;

    g_ocr.page = lv_obj_create(parent);
    lv_obj_set_size(g_ocr.page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_ocr.page, lv_color_hex(0xf3f4f6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_ocr.page, LV_OPA_COVER, LV_PART_MAIN);
    style_clear(g_ocr.page);
    lv_obj_clear_flag(g_ocr.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_ocr.page, LV_OBJ_FLAG_HIDDEN);

    g_ocr.preview = camera_preview_create(g_ocr.page, OCR_PREVIEW_W, OCR_CONTENT_H, g_ocr.text_font);
    if (g_ocr.preview) {
        lv_obj_align(g_ocr.preview->root, LV_ALIGN_TOP_LEFT, OCR_PAGE_PAD, OCR_PAGE_PAD);
        camera_preview_set_placeholder(g_ocr.preview, "相机预览未连接");
        camera_preview_set_led(g_ocr.preview, CAMERA_PREVIEW_LED_RED);
    }

    g_ocr.result = result_panel_create(g_ocr.page, OCR_RIGHT_W, OCR_RESULT_H, g_ocr.text_font);
    if (g_ocr.result) {
        lv_obj_set_pos(g_ocr.result->root, right_x, OCR_PAGE_PAD);
        result_panel_set_text(g_ocr.result, "等待识别...");
    }

    g_ocr.toolbar = lv_obj_create(g_ocr.page);
    lv_obj_remove_style_all(g_ocr.toolbar);
    lv_obj_set_size(g_ocr.toolbar, OCR_RIGHT_W, OCR_TOOLBAR_H);
    lv_obj_set_pos(g_ocr.toolbar, right_x, OCR_PAGE_PAD + OCR_RESULT_H + OCR_RIGHT_SPLIT_GAP);
    lv_obj_set_style_bg_opa(g_ocr.toolbar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(g_ocr.toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_ocr.toolbar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_ocr.toolbar, OCR_UI_BTN_GAP, LV_PART_MAIN);
    lv_obj_clear_flag(g_ocr.toolbar, LV_OBJ_FLAG_SCROLLABLE);

    g_ocr.start_btn = ocr_ui_icon_button_create(g_ocr.toolbar, OCR_UI_ICON_BTN_SIZE,
                                                LV_SYMBOL_PLAY, lv_color_hex(0x16a34a),
                                                on_start_clicked, NULL, false);
    g_ocr.stop_btn = ocr_ui_icon_button_create(g_ocr.toolbar, OCR_UI_ICON_BTN_SIZE, LV_SYMBOL_STOP,
                                               lv_color_hex(0xdc2626), on_stop_clicked, NULL,
                                               false);
    g_ocr.back_btn = ocr_ui_icon_button_create(g_ocr.toolbar, OCR_UI_ICON_BTN_SIZE, LV_SYMBOL_LEFT,
                                               lv_color_hex(0x374151), on_back_clicked, NULL,
                                               false);
    set_action_enabled(true, false);

    return g_ocr.page;
}

void ocr_app_destroy(void) {
    if (g_ocr.running) {
        stop_ocr_pipeline();
        stop_camera_preview();
    }
    if (g_ocr.result) {
        result_panel_destroy(g_ocr.result);
        g_ocr.result = NULL;
    }
    if (g_ocr.preview) {
        camera_preview_destroy(g_ocr.preview);
        g_ocr.preview = NULL;
    }
    g_ocr.start_btn = NULL;
    g_ocr.stop_btn = NULL;
    g_ocr.back_btn = NULL;
    g_ocr.toolbar = NULL;
    if (g_ocr.page) {
        lv_obj_del(g_ocr.page);
        g_ocr.page = NULL;
    }
    g_ocr.ctx = NULL;
    g_ocr.text_font = NULL;
    g_ocr.running = false;
}

void ocr_app_show(void) {
    if (g_ocr.page) {
        lv_obj_clear_flag(g_ocr.page, LV_OBJ_FLAG_HIDDEN);
    }
}

void ocr_app_hide(void) {
    if (g_ocr.running) {
        g_ocr.running = false;
        stop_ocr_pipeline();
        stop_camera_preview();
        set_action_enabled(true, false);
    }
    if (g_ocr.page) {
        lv_obj_add_flag(g_ocr.page, LV_OBJ_FLAG_HIDDEN);
    }
}
