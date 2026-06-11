#ifndef DETECT_APP_H
#define DETECT_APP_H

#include "lvgl_app_ctx.h"

/* 在 parent 下创建目标检测应用全屏页面。
 * 布局/样式完全对齐 ocr_app / hair_app：左侧相机预览 + 右上结果面板 + 右下工具栏。 */
lv_obj_t *detect_app_create(lv_obj_t *parent, const lvgl_app_context_t *ctx);

void detect_app_destroy(void);

void detect_app_show(void);

void detect_app_hide(void);

#endif
