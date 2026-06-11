#include "lvgl_desktop.h"

#include "detect_app.h"
#include "face_app.h"
#include "plate_app.h"
#include "seg_app.h"
#include "hair_app.h"
#include "ocr_app.h"
#include "system_app.h"
#include "lvgl_app_ctx.h"

#define DESKTOP_SCREEN_W 1280
#define DESKTOP_SCREEN_H 720
#define DESKTOP_ICON_SIZE 140
#define DESKTOP_ICON_RADIUS 34
#define DESKTOP_TITLE_GAP 10
#define DESKTOP_CARD_W DESKTOP_ICON_SIZE
#define DESKTOP_CARD_H (DESKTOP_ICON_SIZE + DESKTOP_TITLE_GAP + 32)

typedef enum {
    APP_ID_OCR = 0,
    APP_ID_DETECT,
    APP_ID_FACE,
    APP_ID_PLATE,
    APP_ID_SEG,
    APP_ID_HAIR,
    APP_ID_SYSTEM,
} desktop_app_id_t;

/* 每个桌面 APP 图标的数据：标题、说明、图标文字和渐变颜色。 */
typedef struct {
    desktop_app_id_t id;
    const char *title;
    const char *subtitle;
    const char *icon_text;
    lv_color_t color_start;
    lv_color_t color_end;
} desktop_app_t;

static lv_obj_t *root;
static lv_obj_t *page_stack;
static lv_obj_t *home_page;
static lv_obj_t *detail_page;
static lv_obj_t *detail_title;
static lv_obj_t *detail_body;
static const lv_font_t *desktop_font;
static const lv_font_t *caption_font;

/* 首页显示的 APP 列表，后续新增应用主要改这里。 */
static const desktop_app_t apps[] = {
    {APP_ID_OCR, "OCR识别", "相机文字检测", "OCR",
     LV_COLOR_MAKE(0x38, 0x7d, 0xff), LV_COLOR_MAKE(0x1d, 0x4e, 0xd8)},
    {APP_ID_DETECT, "目标检测", "检测物体/缺陷", "DET",
     LV_COLOR_MAKE(0x2e, 0xd5, 0x73), LV_COLOR_MAKE(0x11, 0x8c, 0x4f)},
    {APP_ID_FACE, "人脸识别", "人脸检测与身份识别", "FACE",
     LV_COLOR_MAKE(0xf5, 0x9e, 0x0b), LV_COLOR_MAKE(0xd9, 0x77, 0x06)},
    {APP_ID_PLATE, "车牌识别", "车牌检测与号牌识别", "LPR",
     LV_COLOR_MAKE(0x06, 0xb6, 0xd4), LV_COLOR_MAKE(0x0e, 0x94, 0x99)},
    {APP_ID_SEG, "目标分割", "实例分割与掩码", "SEG",
     LV_COLOR_MAKE(0xa8, 0x55, 0xf7), LV_COLOR_MAKE(0x7c, 0x3a, 0xed)},
    {APP_ID_HAIR, "头发检测", "头发区域识别", "HAIR",
     LV_COLOR_MAKE(0xff, 0x6b, 0x9d), LV_COLOR_MAKE(0xc1, 0x43, 0x7a)},
    {APP_ID_SYSTEM, "系统设置", "相机信息与系统状态", "SYS",
     LV_COLOR_MAKE(0xb4, 0x6d, 0xff), LV_COLOR_MAKE(0x7e, 0x22, 0xce)},
};

static lvgl_app_context_t app_ctx;

/* 公共样式工具：统一字体、清理默认边框和内边距。 */
static void apply_font(lv_obj_t *obj) {
    if (desktop_font) {
        lv_obj_set_style_text_font(obj, desktop_font, LV_PART_MAIN);
    }
}

static void style_clear(lv_obj_t *obj) {
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t color) {
    lv_obj_t *label = lv_label_create(parent);
    apply_font(label);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_label_set_text(label, text);
    return label;
}

/* 页面切换：回到 APP 首页。 */
static void show_home(void) {
    lv_obj_clear_flag(home_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(detail_page, LV_OBJ_FLAG_HIDDEN);
    ocr_app_hide();
    detect_app_hide();
    face_app_hide();
    plate_app_hide();
    seg_app_hide();
    hair_app_hide();
    system_app_hide();
}

static void show_ocr_app(void) {
    lv_obj_add_flag(home_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(detail_page, LV_OBJ_FLAG_HIDDEN);
    detect_app_hide();
    face_app_hide();
    plate_app_hide();
    seg_app_hide();
    hair_app_hide();
    system_app_hide();
    ocr_app_show();
}

static void show_detect_app(void) {
    lv_obj_add_flag(home_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(detail_page, LV_OBJ_FLAG_HIDDEN);
    ocr_app_hide();
    face_app_hide();
    plate_app_hide();
    seg_app_hide();
    hair_app_hide();
    system_app_hide();
    detect_app_show();
}

static void show_face_app(void) {
    lv_obj_add_flag(home_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(detail_page, LV_OBJ_FLAG_HIDDEN);
    ocr_app_hide();
    detect_app_hide();
    plate_app_hide();
    seg_app_hide();
    hair_app_hide();
    system_app_hide();
    face_app_show();
}

static void show_plate_app(void) {
    lv_obj_add_flag(home_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(detail_page, LV_OBJ_FLAG_HIDDEN);
    ocr_app_hide();
    detect_app_hide();
    face_app_hide();
    seg_app_hide();
    hair_app_hide();
    system_app_hide();
    plate_app_show();
}

static void show_seg_app(void) {
    lv_obj_add_flag(home_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(detail_page, LV_OBJ_FLAG_HIDDEN);
    ocr_app_hide();
    detect_app_hide();
    face_app_hide();
    plate_app_hide();
    hair_app_hide();
    system_app_hide();
    seg_app_show();
}

static void show_hair_app(void) {
    lv_obj_add_flag(home_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(detail_page, LV_OBJ_FLAG_HIDDEN);
    ocr_app_hide();
    detect_app_hide();
    face_app_hide();
    plate_app_hide();
    seg_app_hide();
    system_app_hide();
    hair_app_show();
}

static void show_system_app(void) {
    lv_obj_add_flag(home_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(detail_page, LV_OBJ_FLAG_HIDDEN);
    ocr_app_hide();
    detect_app_hide();
    face_app_hide();
    plate_app_hide();
    seg_app_hide();
    hair_app_hide();
    system_app_show();
}

static void app_back_cb(void *user_data) {
    (void)user_data;
    show_home();
}

/* 页面切换：点击某个 APP 后进入对应详情占位页。 */
static void show_detail(const desktop_app_t *app) {
    lv_label_set_text(detail_title, app->title);
    lv_label_set_text_fmt(detail_body,
                          "%s\n\n这里是算法应用占位页面。\n\n后续可以在这里接入：\n"
                          "1. 相机预览\n2. 参数配置\n3. 开始/停止按钮\n4. 算法结果显示",
                          app->subtitle);
    lv_obj_add_flag(home_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(detail_page, LV_OBJ_FLAG_HIDDEN);
}

/* APP 图标点击事件：按应用类型打开对应页面。 */
static void app_card_event_cb(lv_event_t *event) {
    const desktop_app_t *app = (const desktop_app_t *)lv_event_get_user_data(event);
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !app) {
        return;
    }
    if (app->id == APP_ID_OCR) {
        show_ocr_app();
    } else if (app->id == APP_ID_DETECT) {
        show_detect_app();
    } else if (app->id == APP_ID_FACE) {
        show_face_app();
    } else if (app->id == APP_ID_PLATE) {
        show_plate_app();
    } else if (app->id == APP_ID_SEG) {
        show_seg_app();
    } else if (app->id == APP_ID_HAIR) {
        show_hair_app();
    } else if (app->id == APP_ID_SYSTEM) {
        show_system_app();
    } else {
        show_detail(app);
    }
}

/* 详情页返回按钮事件。 */
static void back_event_cb(lv_event_t *event) {
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        show_home();
    }
}

/* 创建首页里的单个 APP 项：仅图标可点击，下方为小号标题（宽度与图标一致）。 */
static lv_obj_t *create_app_card(lv_obj_t *parent, const desktop_app_t *app) {
    /* 外层容器不参与点击，避免点到文字也进入应用。 */
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, DESKTOP_CARD_W, DESKTOP_CARD_H);
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, LV_PART_MAIN);
    style_clear(card);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);

    /* 可点击的图标按钮，负责进入详情页。 */
    lv_obj_t *icon = lv_btn_create(card);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(icon, app->color_start, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(icon, app->color_end, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(icon, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(icon, DESKTOP_ICON_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_border_width(icon, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(icon, 20, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(icon, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(icon, 10, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(icon, app->color_end, LV_PART_MAIN);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(icon, app_card_event_cb, LV_EVENT_CLICKED, (void *)app);

    /* 图标中心缩写文字。 */
    lv_obj_t *icon_label = make_label(icon, app->icon_text, lv_color_white());
    lv_obj_set_style_text_letter_space(icon_label, 1, LV_PART_MAIN);
    lv_obj_center(icon_label);

    /* 图标下方名称：小号字体，宽度与图标相同，不可点击。 */
    lv_obj_t *title = lv_label_create(card);
    if (caption_font) {
        lv_obj_set_style_text_font(title, caption_font, LV_PART_MAIN);
    } else {
        apply_font(title);
    }
    lv_obj_set_style_text_color(title, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_label_set_text(title, app->title);
    lv_obj_set_width(title, DESKTOP_ICON_SIZE);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, DESKTOP_ICON_SIZE + DESKTOP_TITLE_GAP);

    return card;
}

/* APP 首页：全屏浅色背景，中间显示应用图标网格。 */
static void create_home_page(lv_obj_t *parent) {
    home_page = lv_obj_create(parent);
    lv_obj_set_size(home_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(home_page, lv_color_hex(0xf3f4f6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(home_page, LV_OPA_COVER, LV_PART_MAIN);
    style_clear(home_page);
    lv_obj_clear_flag(home_page, LV_OBJ_FLAG_SCROLLABLE);

    /* 图标网格容器，控制 APP 图标的居中、换行和间距。 */
    lv_obj_t *grid = lv_obj_create(home_page);
    lv_obj_set_size(grid, DESKTOP_SCREEN_W - 120, 460);
    lv_obj_align(grid, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, LV_PART_MAIN);
    style_clear(grid);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(grid, 42, LV_PART_MAIN);
    lv_obj_set_style_pad_column(grid, 56, LV_PART_MAIN);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    for (unsigned int i = 0; i < sizeof(apps) / sizeof(apps[0]); i++) {
        create_app_card(grid, &apps[i]);
    }
}

/* APP 详情页：点击图标后进入，当前保留返回按钮和功能占位面板。 */
static void create_detail_page(lv_obj_t *parent) {
    detail_page = lv_obj_create(parent);
    lv_obj_set_size(detail_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(detail_page, lv_color_hex(0xf9fafb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(detail_page, LV_OPA_COVER, LV_PART_MAIN);
    style_clear(detail_page);
    lv_obj_clear_flag(detail_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(detail_page, LV_OBJ_FLAG_HIDDEN);

    /* 左上角返回按钮。 */
    lv_obj_t *back = lv_btn_create(detail_page);
    lv_obj_set_size(back, 120, 48);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 34, 26);
    lv_obj_add_event_cb(back, back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = make_label(back, "返回", lv_color_white());
    lv_obj_center(back_label);

    detail_title = make_label(detail_page, "算法应用", lv_color_hex(0x111827));
    lv_obj_align(detail_title, LV_ALIGN_TOP_LEFT, 180, 34);

    /* 中间白色内容面板，后续可放相机预览、参数、按钮和结果。 */
    lv_obj_t *panel = lv_obj_create(detail_page);
    lv_obj_set_size(panel, LV_PCT(92), 420);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_set_style_bg_color(panel, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 16, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(0xe5e7eb), LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 24, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    detail_body = make_label(panel, "", lv_color_hex(0x374151));
    lv_obj_set_width(detail_body, LV_PCT(100));
    lv_label_set_long_mode(detail_body, LV_LABEL_LONG_WRAP);
    lv_obj_align(detail_body, LV_ALIGN_TOP_LEFT, 0, 0);
}

/* 桌面 UI 总入口：清屏后创建根容器、页面栈、首页和详情页。 */
void lvgl_desktop_create(const lvgl_desktop_config_t *config) {
    desktop_font = config ? config->font : NULL;
    caption_font = config ? config->caption_font : NULL;
    if (!caption_font) {
        caption_font = desktop_font;
    }

    /* LVGL 当前屏幕，作为整个桌面的最外层画布。 */
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xf3f4f6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 固定 1280x720 根容器，适配当前横屏桌面布局。 */
    root = lv_obj_create(scr);
    lv_obj_set_size(root, DESKTOP_SCREEN_W, DESKTOP_SCREEN_H);
    lv_obj_align(root, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(0xf3f4f6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    style_clear(root);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* 页面栈占满全屏，用隐藏/显示切换首页和详情页。 */
    page_stack = lv_obj_create(root);
    lv_obj_set_size(page_stack, LV_PCT(100), LV_PCT(100));
    lv_obj_align(page_stack, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(page_stack, LV_OPA_TRANSP, LV_PART_MAIN);
    style_clear(page_stack);
    lv_obj_clear_flag(page_stack, LV_OBJ_FLAG_SCROLLABLE);

    create_home_page(page_stack);
    create_detail_page(page_stack);

    app_ctx.font = desktop_font;
    app_ctx.caption_font = caption_font;
    app_ctx.on_back = app_back_cb;
    app_ctx.user_data = NULL;
    ocr_app_create(page_stack, &app_ctx);
    detect_app_create(page_stack, &app_ctx);
    face_app_create(page_stack, &app_ctx);
    plate_app_create(page_stack, &app_ctx);
    seg_app_create(page_stack, &app_ctx);
    hair_app_create(page_stack, &app_ctx);
    system_app_create(page_stack, &app_ctx);
}
