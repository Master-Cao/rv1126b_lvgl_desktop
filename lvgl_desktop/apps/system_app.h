#ifndef SYSTEM_APP_H
#define SYSTEM_APP_H

#include "lvgl_app_ctx.h"

/* 在 parent 下创建系统设置全屏页面（相机信息 + 系统状态）。 */
lv_obj_t *system_app_create(lv_obj_t *parent, const lvgl_app_context_t *ctx);

void system_app_destroy(void);

void system_app_show(void);

void system_app_hide(void);

#endif
