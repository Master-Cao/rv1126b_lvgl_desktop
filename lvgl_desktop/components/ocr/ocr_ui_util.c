#include "ocr_ui_util.h"

#if LV_FONT_MONTSERRAT_28
LV_FONT_DECLARE(lv_font_montserrat_28);
#endif
#if LV_FONT_MONTSERRAT_24
LV_FONT_DECLARE(lv_font_montserrat_24);
#endif
#if LV_FONT_MONTSERRAT_20
LV_FONT_DECLARE(lv_font_montserrat_20);
#endif
#if LV_FONT_MONTSERRAT_16
LV_FONT_DECLARE(lv_font_montserrat_16);
#endif
#if LV_FONT_MONTSERRAT_14
LV_FONT_DECLARE(lv_font_montserrat_14);
#endif

const lv_font_t *ocr_ui_symbol_font(void) {
#if LV_FONT_MONTSERRAT_28
    return &lv_font_montserrat_28;
#elif LV_FONT_MONTSERRAT_24
    return &lv_font_montserrat_24;
#elif LV_FONT_MONTSERRAT_20
    return &lv_font_montserrat_20;
#elif LV_FONT_MONTSERRAT_16
    return &lv_font_montserrat_16;
#elif LV_FONT_MONTSERRAT_14
    return &lv_font_montserrat_14;
#else
    return LV_FONT_DEFAULT;
#endif
}

lv_obj_t *ocr_ui_symbol_label_create(lv_obj_t *parent, const char *symbol, lv_coord_t symbol_px,
                                     lv_color_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, ocr_ui_symbol_font(), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_label_set_text(label, symbol);
    (void)symbol_px;
    return label;
}

static void btn_press_visual_cb(lv_event_t *e) {
    lv_obj_t *icon = (lv_obj_t *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (!icon) {
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        lv_obj_set_style_text_opa(icon, LV_OPA_70, LV_PART_MAIN);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_set_style_text_opa(icon, LV_OPA_COVER, LV_PART_MAIN);
    }
}

static void apply_button_press_style(lv_obj_t *btn, lv_obj_t *icon) {
    static const lv_style_prop_t trans_props[] = {
        LV_STYLE_BG_COLOR,
        LV_STYLE_BG_OPA,
        LV_STYLE_TRANSFORM_ZOOM,
        LV_STYLE_SHADOW_WIDTH,
        LV_STYLE_PROP_INV,
    };
    static lv_style_transition_dsc_t trans;
    static bool trans_inited;

    if (!trans_inited) {
        lv_style_transition_dsc_init(&trans, trans_props, lv_anim_path_ease_out, 120, 0, NULL);
        trans_inited = true;
    }

    lv_obj_set_style_transform_pivot_x(btn, lv_pct(50), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(btn, lv_pct(50), LV_PART_MAIN);
    lv_obj_set_style_transition(btn, &trans, LV_STATE_DEFAULT);
    lv_obj_set_style_transition(btn, &trans, LV_STATE_PRESSED);

    /* 按下/按住：底色变深、略缩小，触摸时有反馈 */
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xe5e7eb), LV_STATE_PRESSED);
    lv_obj_set_style_transform_zoom(btn, 230, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 4, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(0x9ca3af), LV_STATE_PRESSED);

    lv_obj_set_style_bg_opa(btn, LV_OPA_50, LV_STATE_DISABLED);
    if (icon) {
        lv_obj_add_event_cb(btn, btn_press_visual_cb, LV_EVENT_PRESSED, icon);
        lv_obj_add_event_cb(btn, btn_press_visual_cb, LV_EVENT_RELEASED, icon);
        lv_obj_add_event_cb(btn, btn_press_visual_cb, LV_EVENT_PRESS_LOST, icon);
    }
}

lv_obj_t *ocr_ui_icon_button_create(lv_obj_t *parent, lv_coord_t size, const char *symbol,
                                    lv_color_t icon_color, lv_event_cb_t cb, void *user_data,
                                    bool bordered) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_bg_color(btn, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, size / 2, LV_PART_MAIN);
    if (bordered) {
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, lv_color_hex(0xe5e7eb), LV_PART_MAIN);
        lv_obj_set_style_shadow_width(btn, 8, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(btn, LV_OPA_20, LV_PART_MAIN);
    } else {
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    }
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }

    lv_obj_t *icon = ocr_ui_symbol_label_create(btn, symbol, 0, icon_color);
    if (size > 0) {
        lv_obj_set_style_transform_pivot_x(icon, lv_pct(50), LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_y(icon, lv_pct(50), LV_PART_MAIN);
        lv_obj_set_style_transform_zoom(icon, (256 * size * 55) / (100 * 32), LV_PART_MAIN);
    }
    lv_obj_center(icon);
    apply_button_press_style(btn, icon);
    return btn;
}
