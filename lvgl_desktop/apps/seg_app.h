#ifndef SEG_APP_H
#define SEG_APP_H

#include "lvgl_app_ctx.h"

lv_obj_t *seg_app_create(lv_obj_t *parent, const lvgl_app_context_t *ctx);

void seg_app_destroy(void);

void seg_app_show(void);

void seg_app_hide(void);

#endif
