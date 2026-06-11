#ifndef LVGL_APP_CTX_H
#define LVGL_APP_CTX_H

#include <lvgl/lvgl.h>

/* 各算法应用页面共用的字体与返回回调。 */
typedef struct {
    const lv_font_t *font;
    const lv_font_t *caption_font;
    void (*on_back)(void *user_data);
    void *user_data;
} lvgl_app_context_t;

#endif
