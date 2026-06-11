/*
 * 目标检测应用页面：复用 OCR 页面的整体框架（相机预览 + 结果面板 + 工具栏），
 * 算法切换到 detect_algo（YOLOv8 RKNN）。
 */
#include "detect_app.h"

#include "camera_preview.h"
#include "camera_service.h"
#include "detect_algo.h"
#include "ocr_ui_util.h"
#include "result_panel.h"

#include <stdio.h>
#include <string.h>

#define DET_SCREEN_W 1280
#define DET_SCREEN_H 720
#define DET_PAGE_PAD 24
#define DET_COL_GAP 12
#define DET_RIGHT_W 420
#define DET_TOOLBAR_PAD 10
#define DET_RIGHT_SPLIT_GAP 10

#define DET_CONTENT_H (DET_SCREEN_H - DET_PAGE_PAD * 2)
#define DET_PREVIEW_W (DET_SCREEN_W - DET_PAGE_PAD * 2 - DET_COL_GAP - DET_RIGHT_W)
#define DET_TOOLBAR_H (OCR_UI_ICON_BTN_SIZE + DET_TOOLBAR_PAD * 2)
#define DET_RESULT_H (DET_CONTENT_H - DET_RIGHT_SPLIT_GAP - DET_TOOLBAR_H)

#define DET_MAX_SUMMARY_LINES 8

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

    detect_algo_t *algo;
    lv_timer_t *poll_timer;
    uint32_t last_rgb_seq;
    uint32_t last_result_seq;
} detect_app_state_t;

static detect_app_state_t g_det;

/* 清除对象默认边框与内边距。 */
static void style_clear(lv_obj_t *obj) {
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

/* 切换开始/停止按钮的可用状态。 */
static void set_action_enabled(bool start_en, bool stop_en) {
    if (g_det.start_btn) {
        if (start_en) {
            lv_obj_clear_state(g_det.start_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(g_det.start_btn, LV_STATE_DISABLED);
        }
    }
    if (g_det.stop_btn) {
        if (stop_en) {
            lv_obj_clear_state(g_det.stop_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(g_det.stop_btn, LV_STATE_DISABLED);
        }
    }
}

/* 停止检测轮询定时器并释放 YOLOv8 算法实例。 */
static void stop_detect_pipeline(void) {
    if (g_det.poll_timer) {
        lv_timer_del(g_det.poll_timer);
        g_det.poll_timer = NULL;
    }
    if (g_det.algo) {
        detect_algo_stop(g_det.algo);
        detect_algo_destroy(g_det.algo);
        g_det.algo = NULL;
    }
    g_det.last_rgb_seq = 0;
    g_det.last_result_seq = 0;
}

/* 关闭相机预览流并恢复占位提示与红灯状态。 */
static void stop_camera_preview(void) {
    if (!g_det.preview) {
        return;
    }
    camera_preview_clear_boxes(g_det.preview);
    camera_preview_stop_stream(g_det.preview);
    camera_preview_set_placeholder(g_det.preview, "相机预览未连接");
    camera_preview_show_placeholder(g_det.preview, true);
    camera_preview_set_led(g_det.preview, CAMERA_PREVIEW_LED_RED);
}

/* 将检测结果绘制到预览框并更新右侧结果面板文本。 */
static void apply_detect_result(const detect_result_set_t *res) {
    if (!res) {
        return;
    }

    if (g_det.preview) {
        /* detect_box_t 字段顺序与 camera_preview_box_t 一致，逐个拷贝以解耦内存 */
        camera_preview_box_t boxes[DETECT_MAX_RESULTS];
        int n = res->count > DETECT_MAX_RESULTS ? DETECT_MAX_RESULTS : res->count;
        for (int i = 0; i < n; i++) {
            boxes[i].x1 = res->objects[i].box.x1;
            boxes[i].y1 = res->objects[i].box.y1;
            boxes[i].x2 = res->objects[i].box.x2;
            boxes[i].y2 = res->objects[i].box.y2;
            boxes[i].x3 = res->objects[i].box.x3;
            boxes[i].y3 = res->objects[i].box.y3;
            boxes[i].x4 = res->objects[i].box.x4;
            boxes[i].y4 = res->objects[i].box.y4;
        }
        camera_preview_set_boxes(g_det.preview, boxes, n, res->src_w, res->src_h);
        camera_preview_set_ocr_ms(g_det.preview, res->infer_ms);
    }

    if (g_det.result) {
        if (res->count <= 0) {
            result_panel_set_text(g_det.result, "未检测到目标");
        } else {
            char buf[768];
            int off = 0;
            off += snprintf(buf + off, sizeof(buf) - off, "检测到 %d 个目标\n\n", res->count);
            int show = res->count > DET_MAX_SUMMARY_LINES ? DET_MAX_SUMMARY_LINES : res->count;
            for (int i = 0; i < show && off < (int)sizeof(buf) - 32; i++) {
                off += snprintf(buf + off, sizeof(buf) - off, "%s  %.1f%%\n",
                                res->objects[i].name, res->objects[i].score * 100.0f);
            }
            if (res->count > show) {
                snprintf(buf + off, sizeof(buf) - off, "...(其余 %d 个已省略)",
                         res->count - show);
            }
            result_panel_set_text(g_det.result, buf);
        }
    }
}

/* 定时轮询 RGB 帧提交推理，并拉取最新检测结果。 */
static void detect_poll_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!g_det.running || !g_det.algo || !g_det.preview || !g_det.preview->camera_svc) {
        return;
    }

    const uint8_t *rgb = NULL;
    int w = 0;
    int h = 0;
    uint32_t seq = 0;
    if (camera_service_poll_rgb_frame(g_det.preview->camera_svc, g_det.last_rgb_seq, &rgb, &w, &h,
                                      &seq)) {
        g_det.last_rgb_seq = seq;
        detect_algo_submit_rgb888(g_det.algo, rgb, w, h);
    }

    detect_result_set_t result;
    if (detect_algo_get_result(g_det.algo, &result) == 0) {
        if (result.seq != g_det.last_result_seq) {
            g_det.last_result_seq = result.seq;
            apply_detect_result(&result);
        }
    }
}

/* 返回按钮：若正在检测则先停止，再回调桌面首页。 */
static void on_back_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (g_det.running) {
        g_det.running = false;
        stop_detect_pipeline();
        stop_camera_preview();
        set_action_enabled(true, false);
        if (g_det.result) {
            result_panel_set_text(g_det.result, "等待检测...");
        }
    }
    if (g_det.ctx && g_det.ctx->on_back) {
        g_det.ctx->on_back(g_det.ctx->user_data);
    }
}

/* 开始按钮：打开相机预览并启动 YOLOv8 检测管线。 */
static void on_start_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!g_det.preview || !g_det.result) {
        return;
    }

    if (camera_preview_start_stream(g_det.preview, DET_PREVIEW_W, DET_CONTENT_H) != 0) {
        camera_preview_set_led(g_det.preview, CAMERA_PREVIEW_LED_RED);
        camera_preview_set_placeholder(g_det.preview, "相机打开失败");
        camera_preview_show_placeholder(g_det.preview, true);
        result_panel_set_text(g_det.result, "相机打开失败");
        return;
    }

    /* 启动 YOLOv8 推理线程；模型路径走环境变量或默认 /opt/rv1126b_desktop/models/yolov8.rknn */
    g_det.algo = detect_algo_create(NULL);
    if (!g_det.algo || detect_algo_start(g_det.algo) != 0) {
        result_panel_set_text(g_det.result, "YOLOv8 模型加载失败");
        if (g_det.algo) {
            detect_algo_destroy(g_det.algo);
            g_det.algo = NULL;
        }
        stop_camera_preview();
        return;
    }

    g_det.last_rgb_seq = 0;
    g_det.last_result_seq = 0;
    g_det.poll_timer = lv_timer_create(detect_poll_timer_cb, 200, NULL);

    g_det.running = true;
    result_panel_set_text(g_det.result, "正在检测...");
    set_action_enabled(false, true);
}

/* 停止按钮：关闭检测管线与相机预览。 */
static void on_stop_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!g_det.preview || !g_det.result) {
        return;
    }

    g_det.running = false;
    stop_detect_pipeline();
    stop_camera_preview();
    result_panel_set_text(g_det.result, "等待检测...");
    set_action_enabled(true, false);
}

/* 创建目标检测应用全屏页面（预览 + 结果面板 + 工具栏）。 */
lv_obj_t *detect_app_create(lv_obj_t *parent, const lvgl_app_context_t *ctx) {
    const lv_font_t *font = ctx ? ctx->font : NULL;
    const lv_font_t *caption_font = ctx ? ctx->caption_font : NULL;
    const lv_coord_t right_x = DET_PAGE_PAD + DET_PREVIEW_W + DET_COL_GAP;

    g_det.ctx = ctx;
    g_det.text_font = caption_font ? caption_font : font;
    g_det.running = false;

    g_det.page = lv_obj_create(parent);
    lv_obj_set_size(g_det.page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_det.page, lv_color_hex(0xf3f4f6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_det.page, LV_OPA_COVER, LV_PART_MAIN);
    style_clear(g_det.page);
    lv_obj_clear_flag(g_det.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_det.page, LV_OBJ_FLAG_HIDDEN);

    g_det.preview =
        camera_preview_create(g_det.page, DET_PREVIEW_W, DET_CONTENT_H, g_det.text_font);
    if (g_det.preview) {
        lv_obj_align(g_det.preview->root, LV_ALIGN_TOP_LEFT, DET_PAGE_PAD, DET_PAGE_PAD);
        camera_preview_set_placeholder(g_det.preview, "相机预览未连接");
        camera_preview_set_led(g_det.preview, CAMERA_PREVIEW_LED_RED);
        camera_preview_set_metric_name(g_det.preview, "DET");
    }

    g_det.result =
        result_panel_create(g_det.page, DET_RIGHT_W, DET_RESULT_H, g_det.text_font);
    if (g_det.result) {
        lv_obj_set_pos(g_det.result->root, right_x, DET_PAGE_PAD);
        result_panel_set_text(g_det.result, "等待检测...");
    }

    g_det.toolbar = lv_obj_create(g_det.page);
    lv_obj_remove_style_all(g_det.toolbar);
    lv_obj_set_size(g_det.toolbar, DET_RIGHT_W, DET_TOOLBAR_H);
    lv_obj_set_pos(g_det.toolbar, right_x, DET_PAGE_PAD + DET_RESULT_H + DET_RIGHT_SPLIT_GAP);
    lv_obj_set_style_bg_opa(g_det.toolbar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(g_det.toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_det.toolbar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_det.toolbar, OCR_UI_BTN_GAP, LV_PART_MAIN);
    lv_obj_clear_flag(g_det.toolbar, LV_OBJ_FLAG_SCROLLABLE);

    g_det.start_btn = ocr_ui_icon_button_create(g_det.toolbar, OCR_UI_ICON_BTN_SIZE,
                                                LV_SYMBOL_PLAY, lv_color_hex(0x16a34a),
                                                on_start_clicked, NULL, false);
    g_det.stop_btn = ocr_ui_icon_button_create(g_det.toolbar, OCR_UI_ICON_BTN_SIZE,
                                               LV_SYMBOL_STOP, lv_color_hex(0xdc2626),
                                               on_stop_clicked, NULL, false);
    g_det.back_btn = ocr_ui_icon_button_create(g_det.toolbar, OCR_UI_ICON_BTN_SIZE,
                                               LV_SYMBOL_LEFT, lv_color_hex(0x374151),
                                               on_back_clicked, NULL, false);
    set_action_enabled(true, false);

    return g_det.page;
}

/* 销毁页面并释放预览、结果面板与算法资源。 */
void detect_app_destroy(void) {
    if (g_det.running) {
        stop_detect_pipeline();
        stop_camera_preview();
    }
    if (g_det.result) {
        result_panel_destroy(g_det.result);
        g_det.result = NULL;
    }
    if (g_det.preview) {
        camera_preview_destroy(g_det.preview);
        g_det.preview = NULL;
    }
    g_det.start_btn = NULL;
    g_det.stop_btn = NULL;
    g_det.back_btn = NULL;
    g_det.toolbar = NULL;
    if (g_det.page) {
        lv_obj_del(g_det.page);
        g_det.page = NULL;
    }
    g_det.ctx = NULL;
    g_det.text_font = NULL;
    g_det.running = false;
}

/* 显示目标检测应用页面。 */
void detect_app_show(void) {
    if (g_det.page) {
        lv_obj_clear_flag(g_det.page, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 隐藏页面；若正在检测则自动停止管线。 */
void detect_app_hide(void) {
    if (g_det.running) {
        g_det.running = false;
        stop_detect_pipeline();
        stop_camera_preview();
        set_action_enabled(true, false);
    }
    if (g_det.page) {
        lv_obj_add_flag(g_det.page, LV_OBJ_FLAG_HIDDEN);
    }
}
