#ifndef FACE_APP_H
#define FACE_APP_H

#include "lvgl_app_ctx.h"

/* 布局对齐 ocr_app / detect_app / hair_app：左侧相机预览 + 右上结果 + 右下工具栏。 */
lv_obj_t *face_app_create(lv_obj_t *parent, const lvgl_app_context_t *ctx);

void face_app_destroy(void);

void face_app_show(void);

void face_app_hide(void);

#endif
