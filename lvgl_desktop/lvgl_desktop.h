#ifndef LVGL_DESKTOP_H
#define LVGL_DESKTOP_H

#include <lvgl/lvgl.h>

typedef struct {
    const lv_font_t *font;         /* 详情页等正文 */
    const lv_font_t *caption_font; /* 图标下方应用名称，字号较小 */
} lvgl_desktop_config_t;

void lvgl_desktop_create(const lvgl_desktop_config_t *config);

#endif
