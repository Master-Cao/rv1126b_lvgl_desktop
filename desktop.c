/*
 * Standalone LVGL desktop entry.
 */

#include "lvgl_desktop.h"
#include "lv_port_init.h"

#include <lvgl/lv_conf.h>
#include <lvgl/lvgl.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#define FREETYPE_FONT_FILE ("/usr/share/fonts/source-han-sans-cn/SourceHanSansCN-Normal.otf")

static volatile sig_atomic_t quit = 0;
static lv_ft_info_t ft_info;
static lv_ft_info_t ft_caption_info;

static void sigterm_handler(int sig) {
    fprintf(stderr, "signal %d\n", sig);
    quit = 1;
}

#if LV_USE_LOG
static void lvgl_log_print_cb(const char *buf) {
    fputs(buf, stdout);
    fflush(stdout);
}
#endif

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    signal(SIGINT, sigterm_handler);
    signal(SIGTERM, sigterm_handler);

    /* 当前板端之前使用 0,0,90 作为横屏逻辑入口；如目标板方向不同，可在这里单独调整。 */
    lv_port_init(0, 0, 90);

#if LV_USE_LOG
    lv_log_register_print_cb(lvgl_log_print_cb);
#endif

    ft_info.name = FREETYPE_FONT_FILE;
    ft_info.weight = 42;
    ft_info.style = FT_FONT_STYLE_NORMAL;
    ft_info.mem = NULL;

    if (!lv_ft_font_init(&ft_info)) {
        printf("font init failed.\n");
    }

    ft_caption_info.name = FREETYPE_FONT_FILE;
    ft_caption_info.weight = 26;
    ft_caption_info.style = FT_FONT_STYLE_NORMAL;
    ft_caption_info.mem = NULL;
    if (!lv_ft_font_init(&ft_caption_info)) {
        printf("caption font init failed, fallback to main font.\n");
    }

    lvgl_desktop_config_t desktop_config = {
        .font = ft_info.font,
        .caption_font = ft_caption_info.font ? ft_caption_info.font : ft_info.font,
    };
    lvgl_desktop_create(&desktop_config);

    while (!quit) {
        lv_task_handler();
        usleep(5 * 1000);
    }

    return 0;
}
