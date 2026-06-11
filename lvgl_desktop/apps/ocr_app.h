#ifndef OCR_APP_H
#define OCR_APP_H

#include "lvgl_app_ctx.h"

/* 在 parent 下创建 OCR 应用全屏页面。 */
lv_obj_t *ocr_app_create(lv_obj_t *parent, const lvgl_app_context_t *ctx);

void ocr_app_destroy(void);

void ocr_app_show(void);

void ocr_app_hide(void);

#endif
