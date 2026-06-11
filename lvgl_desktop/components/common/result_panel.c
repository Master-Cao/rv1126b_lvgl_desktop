#include "result_panel.h"

#include <stdlib.h>
#include <string.h>

static void apply_font(lv_obj_t *obj, const lv_font_t *font) {
    if (font) {
        lv_obj_set_style_text_font(obj, font, LV_PART_MAIN);
    }
}

result_panel_t *result_panel_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                                    const lv_font_t *font) {
    result_panel_t *panel = (result_panel_t *)lv_mem_alloc(sizeof(result_panel_t));
    if (!panel) {
        return NULL;
    }

    panel->root = lv_obj_create(parent);
    lv_obj_remove_style_all(panel->root);
    lv_obj_set_size(panel->root, w, h);
    lv_obj_set_style_bg_color(panel->root, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel->root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(panel->root, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel->root, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel->root, lv_color_hex(0xe5e7eb), LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel->root, 16, LV_PART_MAIN);
    lv_obj_clear_flag(panel->root, LV_OBJ_FLAG_SCROLLABLE);

    panel->body = lv_label_create(panel->root);
    apply_font(panel->body, font);
    lv_obj_set_width(panel->body, w - 32);
    lv_obj_set_style_text_color(panel->body, lv_color_hex(0x6b7280), LV_PART_MAIN);
    lv_label_set_long_mode(panel->body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(panel->body, "");
    lv_obj_align(panel->body, LV_ALIGN_TOP_LEFT, 0, 0);

    return panel;
}

void result_panel_destroy(result_panel_t *panel) {
    if (!panel) {
        return;
    }
    if (panel->root) {
        lv_obj_del(panel->root);
    }
    lv_mem_free(panel);
}

void result_panel_set_text(result_panel_t *panel, const char *text) {
    if (panel && panel->body && text) {
        lv_label_set_text(panel->body, text);
    }
}

void result_panel_set_lines(result_panel_t *panel, const char *const *lines, int count) {
    if (!panel || !panel->body) {
        return;
    }
    if (!lines || count <= 0) {
        lv_label_set_text(panel->body, "");
        return;
    }

    size_t total = 1;
    for (int i = 0; i < count; i++) {
        if (lines[i]) {
            total += strlen(lines[i]) + 1; /* line + '\n' */
        }
    }

    char *buf = (char *)lv_mem_alloc(total);
    if (!buf) {
        return;
    }
    size_t off = 0;
    for (int i = 0; i < count; i++) {
        if (!lines[i]) {
            continue;
        }
        size_t n = strlen(lines[i]);
        memcpy(buf + off, lines[i], n);
        off += n;
        if (i + 1 < count) {
            buf[off++] = '\n';
        }
    }
    buf[off] = '\0';

    lv_label_set_text(panel->body, buf);
    lv_mem_free(buf);
}
