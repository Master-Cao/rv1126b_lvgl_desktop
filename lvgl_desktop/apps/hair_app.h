#ifndef HAIR_APP_H
#define HAIR_APP_H

#include "lvgl_app_ctx.h"

/* 在 parent 下创建头发检测应用全屏页面。
 * 布局/样式完全对齐 ocr_app：左侧相机预览 + 右上结果面板 + 右下工具栏。 */
lv_obj_t *hair_app_create(lv_obj_t *parent, const lvgl_app_context_t *ctx);

void hair_app_destroy(void);

void hair_app_show(void);

void hair_app_hide(void);

#endif
