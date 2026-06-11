#ifndef RESULT_PANEL_H
#define RESULT_PANEL_H

#include <lvgl/lvgl.h>

typedef struct {
    lv_obj_t *root;
    lv_obj_t *body;
} result_panel_t;

result_panel_t *result_panel_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                                    const lv_font_t *font);

void result_panel_destroy(result_panel_t *panel);

void result_panel_set_text(result_panel_t *panel, const char *text);

/* 多行文本，内部用 \n 拼接后调用 set_text。空数组等价于显示空字符串。 */
void result_panel_set_lines(result_panel_t *panel, const char *const *lines, int count);

#endif
