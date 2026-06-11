#ifndef OCR_ACTION_BUTTONS_H
#define OCR_ACTION_BUTTONS_H

#include <lvgl/lvgl.h>

typedef void (*ocr_action_btn_cb_t)(void *user_data);

typedef struct {
    lv_obj_t *root;
    lv_obj_t *start_btn;
    lv_obj_t *stop_btn;
    ocr_action_btn_cb_t on_start;
    ocr_action_btn_cb_t on_stop;
    void *user_data;
} ocr_action_buttons_t;

/* 横向排列：开始、停止 64×64 图标按钮。 */
ocr_action_buttons_t *ocr_action_buttons_create(lv_obj_t *parent, lv_coord_t btn_size,
                                                  ocr_action_btn_cb_t on_start,
                                                  ocr_action_btn_cb_t on_stop,
                                                  void *user_data);

void ocr_action_buttons_destroy(ocr_action_buttons_t *buttons);

void ocr_action_buttons_set_enabled(ocr_action_buttons_t *buttons, bool start_en, bool stop_en);

#endif
