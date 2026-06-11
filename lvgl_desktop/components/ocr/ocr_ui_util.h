#ifndef OCR_UI_UTIL_H
#define OCR_UI_UTIL_H

#include <lvgl/lvgl.h>

#define OCR_UI_BTN_GAP 36
#define OCR_UI_ICON_BTN_SIZE 80

/* LVGL 内置符号字体（与中文 FreeType 分离，避免方框）。 */
const lv_font_t *ocr_ui_symbol_font(void);

lv_obj_t *ocr_ui_icon_button_create(lv_obj_t *parent, lv_coord_t size, const char *symbol,
                                    lv_color_t icon_color, lv_event_cb_t cb, void *user_data,
                                    bool bordered);

lv_obj_t *ocr_ui_symbol_label_create(lv_obj_t *parent, const char *symbol, lv_coord_t symbol_px,
                                     lv_color_t color);

#endif
