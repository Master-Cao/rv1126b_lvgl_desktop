#include "action_buttons.h"

#include "ocr_ui_util.h"

#include <stdlib.h>

static void start_event_cb(lv_event_t *e) {
    ocr_action_buttons_t *btns = (ocr_action_buttons_t *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && btns && btns->on_start) {
        btns->on_start(btns->user_data);
    }
}

static void stop_event_cb(lv_event_t *e) {
    ocr_action_buttons_t *btns = (ocr_action_buttons_t *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && btns && btns->on_stop) {
        btns->on_stop(btns->user_data);
    }
}

ocr_action_buttons_t *ocr_action_buttons_create(lv_obj_t *parent, lv_coord_t btn_size,
                                                ocr_action_btn_cb_t on_start,
                                                ocr_action_btn_cb_t on_stop,
                                                void *user_data) {
    ocr_action_buttons_t *btns = (ocr_action_buttons_t *)lv_mem_alloc(sizeof(ocr_action_buttons_t));
    if (!btns) {
        return NULL;
    }

    btns->on_start = on_start;
    btns->on_stop = on_stop;
    btns->user_data = user_data;

    btns->root = lv_obj_create(parent);
    lv_obj_remove_style_all(btns->root);
    lv_obj_set_size(btns->root, btn_size * 2 + OCR_UI_BTN_GAP, btn_size);
    lv_obj_set_style_bg_opa(btns->root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(btns->root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns->root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btns->root, OCR_UI_BTN_GAP, LV_PART_MAIN);
    lv_obj_clear_flag(btns->root, LV_OBJ_FLAG_SCROLLABLE);

    btns->start_btn = ocr_ui_icon_button_create(btns->root, btn_size, LV_SYMBOL_PLAY,
                                                lv_color_hex(0x16a34a), start_event_cb, btns,
                                                false);

    btns->stop_btn = ocr_ui_icon_button_create(btns->root, btn_size, LV_SYMBOL_STOP,
                                               lv_color_hex(0xdc2626), stop_event_cb, btns,
                                               false);

    return btns;
}

void ocr_action_buttons_destroy(ocr_action_buttons_t *buttons) {
    if (!buttons) {
        return;
    }
    if (buttons->root) {
        lv_obj_del(buttons->root);
    }
    lv_mem_free(buttons);
}

void ocr_action_buttons_set_enabled(ocr_action_buttons_t *buttons, bool start_en, bool stop_en) {
    if (!buttons) {
        return;
    }
    if (start_en) {
        lv_obj_clear_state(buttons->start_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(buttons->start_btn, LV_STATE_DISABLED);
    }
    if (stop_en) {
        lv_obj_clear_state(buttons->stop_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(buttons->stop_btn, LV_STATE_DISABLED);
    }
}
