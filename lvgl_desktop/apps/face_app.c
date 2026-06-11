/*
 * 人脸识别应用页面：复用 OCR/检测 页面框架（相机预览 + 结果面板 + 工具栏），
 * 算法切换到 face_algo（人脸检测 + 识别，RKNN 待接入）。
 */
#include "face_app.h"

#include "camera_preview.h"
#include "camera_service.h"
#include "face_algo.h"
#include "ocr_ui_util.h"
#include "result_panel.h"

#include <stdio.h>
#include <string.h>

#define FACE_SCREEN_W 1280
#define FACE_SCREEN_H 720
#define FACE_PAGE_PAD 24
#define FACE_COL_GAP 12
#define FACE_RIGHT_W 420
#define FACE_TOOLBAR_PAD 10
#define FACE_RIGHT_SPLIT_GAP 10

#define FACE_CONTENT_H (FACE_SCREEN_H - FACE_PAGE_PAD * 2)
#define FACE_PREVIEW_W (FACE_SCREEN_W - FACE_PAGE_PAD * 2 - FACE_COL_GAP - FACE_RIGHT_W)
#define FACE_TOOLBAR_H (OCR_UI_ICON_BTN_SIZE + FACE_TOOLBAR_PAD * 2)
#define FACE_RESULT_H (FACE_CONTENT_H - FACE_RIGHT_SPLIT_GAP - FACE_TOOLBAR_H)

#define FACE_MAX_SUMMARY_LINES 8

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

    face_algo_t *algo;
    lv_timer_t *poll_timer;
    uint32_t last_rgb_seq;
    uint32_t last_result_seq;
} face_app_state_t;

static face_app_state_t g_face;

static void style_clear(lv_obj_t *obj) {
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

static void set_action_enabled(bool start_en, bool stop_en) {
    if (g_face.start_btn) {
        if (start_en) {
            lv_obj_clear_state(g_face.start_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(g_face.start_btn, LV_STATE_DISABLED);
        }
    }
    if (g_face.stop_btn) {
        if (stop_en) {
            lv_obj_clear_state(g_face.stop_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(g_face.stop_btn, LV_STATE_DISABLED);
        }
    }
}

static void stop_face_pipeline(void) {
    if (g_face.poll_timer) {
        lv_timer_del(g_face.poll_timer);
        g_face.poll_timer = NULL;
    }
    if (g_face.algo) {
        face_algo_stop(g_face.algo);
        face_algo_destroy(g_face.algo);
        g_face.algo = NULL;
    }
    g_face.last_rgb_seq = 0;
    g_face.last_result_seq = 0;
}

static void stop_camera_preview(void) {
    if (!g_face.preview) {
        return;
    }
    camera_preview_clear_boxes(g_face.preview);
    camera_preview_clear_points(g_face.preview);
    camera_preview_stop_stream(g_face.preview);
    camera_preview_set_placeholder(g_face.preview, "相机预览未连接");
    camera_preview_show_placeholder(g_face.preview, true);
    camera_preview_set_led(g_face.preview, CAMERA_PREVIEW_LED_RED);
}

static void apply_face_result(const face_result_set_t *res) {
    if (!res) {
        return;
    }

    if (g_face.preview) {
        camera_preview_box_t boxes[FACE_MAX_FACES];
        camera_preview_point_t pts[FACE_MAX_FACES * FACE_LANDMARK_NUM];
        int n = res->count > FACE_MAX_FACES ? FACE_MAX_FACES : res->count;
        int np = 0;
        for (int i = 0; i < n; i++) {
            boxes[i].x1 = res->faces[i].box.x1;
            boxes[i].y1 = res->faces[i].box.y1;
            boxes[i].x2 = res->faces[i].box.x2;
            boxes[i].y2 = res->faces[i].box.y2;
            boxes[i].x3 = res->faces[i].box.x3;
            boxes[i].y3 = res->faces[i].box.y3;
            boxes[i].x4 = res->faces[i].box.x4;
            boxes[i].y4 = res->faces[i].box.y4;
            for (int j = 0; j < FACE_LANDMARK_NUM; j++) {
                pts[np].x = (int)res->faces[i].landmarks[j].x;
                pts[np].y = (int)res->faces[i].landmarks[j].y;
                np++;
            }
        }
        camera_preview_set_boxes(g_face.preview, boxes, n, res->src_w, res->src_h);
        camera_preview_set_points(g_face.preview, pts, np, res->src_w, res->src_h);
        camera_preview_set_ocr_ms(g_face.preview, res->det_ms + res->rec_ms);
    }

    if (g_face.result) {
        if (res->count <= 0) {
            if (g_face.algo && !face_algo_models_ready(g_face.algo)) {
                result_panel_set_text(g_face.result,
                                      "预览中\n\n人脸 RKNN 模型待接入\n"
                                      "请部署 face_det.rknn / face_rec.rknn");
            } else {
                result_panel_set_text(g_face.result, "未检测到人脸");
            }
        } else {
            char buf[768];
            int off = 0;
            off += snprintf(buf + off, sizeof(buf) - off, "检测 %dms  识别 %dms\n\n", res->det_ms,
                            res->rec_ms);
            int show = res->count > FACE_MAX_SUMMARY_LINES ? FACE_MAX_SUMMARY_LINES : res->count;
            for (int i = 0; i < show && off < (int)sizeof(buf) - 48; i++) {
                const face_item_t *f = &res->faces[i];
                const char *name = (f->name[0] != '\0') ? f->name : "未知";
                off += snprintf(buf + off, sizeof(buf) - off, "%s  识别 %.1f%%  检测 %.1f%%\n",
                                name, f->match_score * 100.0f, f->det_score * 100.0f);
            }
            if (res->count > show) {
                snprintf(buf + off, sizeof(buf) - off, "...(其余 %d 张已省略)",
                         res->count - show);
            }
            result_panel_set_text(g_face.result, buf);
        }
    }
}

static void face_poll_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!g_face.running || !g_face.algo || !g_face.preview || !g_face.preview->camera_svc) {
        return;
    }

    const uint8_t *rgb = NULL;
    int w = 0;
    int h = 0;
    uint32_t seq = 0;
    if (camera_service_poll_rgb_frame(g_face.preview->camera_svc, g_face.last_rgb_seq, &rgb, &w,
                                      &h, &seq)) {
        g_face.last_rgb_seq = seq;
        face_algo_submit_rgb888(g_face.algo, rgb, w, h);
    }

    face_result_set_t result;
    if (face_algo_get_result(g_face.algo, &result) == 0) {
        if (result.seq != g_face.last_result_seq) {
            g_face.last_result_seq = result.seq;
            apply_face_result(&result);
        }
    }
}

static void on_back_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (g_face.running) {
        g_face.running = false;
        stop_face_pipeline();
        stop_camera_preview();
        set_action_enabled(true, false);
        if (g_face.result) {
            result_panel_set_text(g_face.result, "等待识别...");
        }
    }
    if (g_face.ctx && g_face.ctx->on_back) {
        g_face.ctx->on_back(g_face.ctx->user_data);
    }
}

static void on_start_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!g_face.preview || !g_face.result) {
        return;
    }

    if (camera_preview_start_stream(g_face.preview, FACE_PREVIEW_W, FACE_CONTENT_H) != 0) {
        camera_preview_set_led(g_face.preview, CAMERA_PREVIEW_LED_RED);
        camera_preview_set_placeholder(g_face.preview, "相机打开失败");
        camera_preview_show_placeholder(g_face.preview, true);
        result_panel_set_text(g_face.result, "相机打开失败");
        return;
    }

    /* 与 detect_app 一致：先开相机，再 create + start 算法（start 内加载 face_det.rknn）。 */
    g_face.algo = face_algo_create(NULL);
    if (!g_face.algo || face_algo_start(g_face.algo) != 0) {
        result_panel_set_text(g_face.result, "人脸模型加载失败");
        if (g_face.algo) {
            face_algo_destroy(g_face.algo);
            g_face.algo = NULL;
        }
        stop_camera_preview();
        return;
    }

    g_face.last_rgb_seq = 0;
    g_face.last_result_seq = 0;
    g_face.poll_timer = lv_timer_create(face_poll_timer_cb, 200, NULL);

    g_face.running = true;
    result_panel_set_text(g_face.result, "正在检测...");
    set_action_enabled(false, true);
}

static void on_stop_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!g_face.preview || !g_face.result) {
        return;
    }

    g_face.running = false;
    stop_face_pipeline();
    stop_camera_preview();
    result_panel_set_text(g_face.result, "等待识别...");
    set_action_enabled(true, false);
}

lv_obj_t *face_app_create(lv_obj_t *parent, const lvgl_app_context_t *ctx) {
    const lv_font_t *font = ctx ? ctx->font : NULL;
    const lv_font_t *caption_font = ctx ? ctx->caption_font : NULL;
    const lv_coord_t right_x = FACE_PAGE_PAD + FACE_PREVIEW_W + FACE_COL_GAP;

    g_face.ctx = ctx;
    g_face.text_font = caption_font ? caption_font : font;
    g_face.running = false;

    g_face.page = lv_obj_create(parent);
    lv_obj_set_size(g_face.page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_face.page, lv_color_hex(0xf3f4f6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_face.page, LV_OPA_COVER, LV_PART_MAIN);
    style_clear(g_face.page);
    lv_obj_clear_flag(g_face.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_face.page, LV_OBJ_FLAG_HIDDEN);

    g_face.preview =
        camera_preview_create(g_face.page, FACE_PREVIEW_W, FACE_CONTENT_H, g_face.text_font);
    if (g_face.preview) {
        lv_obj_align(g_face.preview->root, LV_ALIGN_TOP_LEFT, FACE_PAGE_PAD, FACE_PAGE_PAD);
        camera_preview_set_placeholder(g_face.preview, "相机预览未连接");
        camera_preview_set_led(g_face.preview, CAMERA_PREVIEW_LED_RED);
        camera_preview_set_metric_name(g_face.preview, "FACE");
    }

    g_face.result = result_panel_create(g_face.page, FACE_RIGHT_W, FACE_RESULT_H, g_face.text_font);
    if (g_face.result) {
        lv_obj_set_pos(g_face.result->root, right_x, FACE_PAGE_PAD);
        result_panel_set_text(g_face.result, "等待识别...");
    }

    g_face.toolbar = lv_obj_create(g_face.page);
    lv_obj_remove_style_all(g_face.toolbar);
    lv_obj_set_size(g_face.toolbar, FACE_RIGHT_W, FACE_TOOLBAR_H);
    lv_obj_set_pos(g_face.toolbar, right_x, FACE_PAGE_PAD + FACE_RESULT_H + FACE_RIGHT_SPLIT_GAP);
    lv_obj_set_style_bg_opa(g_face.toolbar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(g_face.toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_face.toolbar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_face.toolbar, OCR_UI_BTN_GAP, LV_PART_MAIN);
    lv_obj_clear_flag(g_face.toolbar, LV_OBJ_FLAG_SCROLLABLE);

    g_face.start_btn = ocr_ui_icon_button_create(g_face.toolbar, OCR_UI_ICON_BTN_SIZE,
                                                 LV_SYMBOL_PLAY, lv_color_hex(0x16a34a),
                                                 on_start_clicked, NULL, false);
    g_face.stop_btn = ocr_ui_icon_button_create(g_face.toolbar, OCR_UI_ICON_BTN_SIZE,
                                                LV_SYMBOL_STOP, lv_color_hex(0xdc2626),
                                                on_stop_clicked, NULL, false);
    g_face.back_btn = ocr_ui_icon_button_create(g_face.toolbar, OCR_UI_ICON_BTN_SIZE,
                                                LV_SYMBOL_LEFT, lv_color_hex(0x374151),
                                                on_back_clicked, NULL, false);
    set_action_enabled(true, false);

    return g_face.page;
}

void face_app_destroy(void) {
    if (g_face.running) {
        stop_face_pipeline();
        stop_camera_preview();
    }
    if (g_face.result) {
        result_panel_destroy(g_face.result);
        g_face.result = NULL;
    }
    if (g_face.preview) {
        camera_preview_destroy(g_face.preview);
        g_face.preview = NULL;
    }
    g_face.start_btn = NULL;
    g_face.stop_btn = NULL;
    g_face.back_btn = NULL;
    g_face.toolbar = NULL;
    if (g_face.page) {
        lv_obj_del(g_face.page);
        g_face.page = NULL;
    }
    g_face.ctx = NULL;
    g_face.text_font = NULL;
    g_face.running = false;
}

void face_app_show(void) {
    if (g_face.page) {
        lv_obj_clear_flag(g_face.page, LV_OBJ_FLAG_HIDDEN);
    }
}

void face_app_hide(void) {
    if (g_face.running) {
        g_face.running = false;
        stop_face_pipeline();
        stop_camera_preview();
        set_action_enabled(true, false);
    }
    if (g_face.page) {
        lv_obj_add_flag(g_face.page, LV_OBJ_FLAG_HIDDEN);
    }
}
