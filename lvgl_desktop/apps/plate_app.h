#ifndef PLATE_APP_H
#define PLATE_APP_H

#include "lvgl_app_ctx.h"

lv_obj_t *plate_app_create(lv_obj_t *parent, const lvgl_app_context_t *ctx);

void plate_app_destroy(void);

void plate_app_show(void);

void plate_app_hide(void);

#endif
