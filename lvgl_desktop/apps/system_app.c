/*
 * 系统设置应用页面：展示相机设备信息与系统运行状态。
 */
#include "system_app.h"

#include "camera_types.h"
#include "cpu.h"
#include "gpio.h"
#include "ocr_ui_util.h"
#include "soc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SYS_SCREEN_W 1280
#define SYS_SCREEN_H 720
#define SYS_PAGE_PAD 24
#define SYS_COL_GAP 16
#define SYS_HEADER_H 72
/* 上方两张信息卡左右并排，下方 GPIO 表格整行宽 */
#define SYS_INFO_CARD_W ((SYS_SCREEN_W - SYS_PAGE_PAD * 2 - SYS_COL_GAP) / 2)
#define SYS_INFO_CARD_H 360
#define SYS_GPIO_CARD_W (SYS_SCREEN_W - SYS_PAGE_PAD * 2)
#define SYS_REFRESH_MS 2000
#define SYS_GPIO_TEST_TOGGLES 6   /* 输出测试翻转次数 */
#define SYS_GPIO_TEST_PERIOD_MS 400

/* GPIO 表格列布局（相对卡片内容区左侧的 x 偏移） */
#define GP_ROW_Y0 88
#define GP_ROW_H 56
#define GP_COL_NAME_X 8
#define GP_COL_PIN_X 210
#define GP_COL_DIR_X 330
#define GP_COL_DIR_W 150
#define GP_COL_VAL_X 510
#define GP_COL_ACT_X 610
#define GP_COL_TEST_X 780
#define GP_BTN_W 150
#define GP_BTN_H 42

/*
 * GPIO 引脚候选列表（瑞芯微命名）。
 * !!! 硬件相关：请根据实际开发板可用、未被复用的引脚修改此表 !!!
 * 命名 -> 引脚号由 gpio_pin_from_name() 自动换算（bank*32 + group*8 + index）。
 */
static const char *const g_gpio_presets[] = {
    "GPIO5_C1", /* 5*32 + 2*8 + 1 = 177 */
    "GPIO3_A4", /* 3*32 + 0*8 + 4 = 100 */
    "GPIO3_A5", /* 3*32 + 0*8 + 5 = 101 */
    "GPIO7_A1", /* 7*32 + 0*8 + 1 = 225 */
    "GPIO7_A4", /* 7*32 + 0*8 + 4 = 228 */
    "GPIO3_A0", /* 3*32 + 0*8 + 0 = 96 */
    "GPIO3_A1", /* 3*32 + 0*8 + 1 = 97 */
};
#define GPIO_PRESET_COUNT ((int)(sizeof(g_gpio_presets) / sizeof(g_gpio_presets[0])))

/* GPIO 表格中的一行（对应一个引脚）。 */
typedef struct {
    int pin;             /* 解析得到的 sysfs 引脚号 */
    gpio_dir_t dir;      /* 当前方向缓存 */
    int out_cache;       /* 最近一次输出电平 */
    lv_obj_t *dir_label;   /* 方向按钮上的文字（输入/输出） */
    lv_obj_t *act_label;   /* 操作按钮上的文字（读取/翻转） */
    lv_obj_t *test_btn;    /* 测试输出按钮 */
    lv_obj_t *value_label; /* 电平显示 */
} gpio_row_t;

typedef struct {
    lv_obj_t *page;
    lv_obj_t *camera_card;
    lv_obj_t *camera_body;
    lv_obj_t *system_card;
    lv_obj_t *system_body;
    lv_obj_t *gpio_card;
    lv_obj_t *gpio_status;
    gpio_row_t gpio_rows[GPIO_PRESET_COUNT];
    lv_timer_t *gpio_test_timer;
    gpio_row_t *gpio_test_row;
    int gpio_test_pin;
    int gpio_test_remaining;
    lv_obj_t *back_btn;
    const lvgl_app_context_t *ctx;
    const lv_font_t *text_font;
    lv_timer_t *refresh_timer;
} system_app_state_t;

static system_app_state_t g_sys;

/* 清除对象默认边框与内边距。 */
static void style_clear(lv_obj_t *obj) {
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

/* 为标签应用当前页面字体。 */
static void apply_font(lv_obj_t *obj) {
    if (g_sys.text_font) {
        lv_obj_set_style_text_font(obj, g_sys.text_font, LV_PART_MAIN);
    }
}

/* 创建带标题的白色信息卡片容器，通过 out_body 返回正文标签。 */
static lv_obj_t *create_info_card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, const char *title,
                                  lv_obj_t **out_body) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(0xe5e7eb), LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 20, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *heading = lv_label_create(card);
    apply_font(heading);
    lv_obj_set_style_text_color(heading, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_label_set_text(heading, title);
    lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *divider = lv_obj_create(card);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, w - 40, 1);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0xe5e7eb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(divider, LV_ALIGN_TOP_LEFT, 0, 34);

    /* 正文放入可竖向滚动的容器，内容过长时卡片内部出现滚动条 */
    lv_obj_t *content = lv_obj_create(card);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, w - 40, h - 40 - 48);
    lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, 48);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *body = lv_label_create(content);
    apply_font(body);
    lv_obj_set_width(body, w - 40 - 12); /* 预留滚动条宽度 */
    lv_obj_set_style_text_color(body, lv_color_hex(0x374151), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(body, 8, LV_PART_MAIN);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 0);

    if (out_body) {
        *out_body = body;
    }
    return card;
}

/* 读取 /proc/meminfo 中指定字段（单位 KB）。 */
static long read_meminfo_kb(const char *key) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) {
        return -1;
    }

    char line[128];
    long value = -1;
    size_t key_len = strlen(key);

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, key, key_len) == 0) {
            if (sscanf(line + key_len, "%ld", &value) == 1) {
                break;
            }
        }
    }

    fclose(fp);
    return value;
}

/* 读取系统运行时长（秒）。 */
static double read_uptime_sec(void) {
    FILE *fp = fopen("/proc/uptime", "r");
    if (!fp) {
        return -1.0;
    }

    double uptime = -1.0;
    if (fscanf(fp, "%lf", &uptime) != 1) {
        uptime = -1.0;
    }
    fclose(fp);
    return uptime;
}

/* 检查相机设备节点是否可访问。 */
static bool camera_device_accessible(void) {
    return access(CAMERA_DEFAULT_DEVICE, R_OK | W_OK) == 0;
}

/* 格式化运行时长为「X 天 X 时 X 分」。 */
static void format_uptime(double sec, char *buf, size_t buf_size) {
    if (sec < 0.0) {
        snprintf(buf, buf_size, "未知");
        return;
    }

    long total = (long)sec;
    long days = total / 86400;
    long hours = (total % 86400) / 3600;
    long mins = (total % 3600) / 60;

    if (days > 0) {
        snprintf(buf, buf_size, "%ld 天 %ld 时 %ld 分", days, hours, mins);
    } else if (hours > 0) {
        snprintf(buf, buf_size, "%ld 时 %ld 分", hours, mins);
    } else {
        snprintf(buf, buf_size, "%ld 分", mins);
    }
}

/* 刷新相机信息面板文本。 */
static void refresh_camera_info(void) {
    if (!g_sys.camera_body) {
        return;
    }

    char buf[512];
    const char *status = camera_device_accessible() ? "可用" : "不可用";

    snprintf(buf, sizeof(buf),
             "设备节点：%s\n"
             "采集分辨率：%d × %d\n"
             "默认格式：由 V4L2 协商\n"
             "设备状态：%s",
             CAMERA_DEFAULT_DEVICE, CAMERA_CAPTURE_WIDTH, CAMERA_CAPTURE_HEIGHT, status);
    lv_label_set_text(g_sys.camera_body, buf);
}

/* 刷新系统信息面板文本（SOC、内核、CPU、内存等）。 */
static void refresh_system_info(void) {
    if (!g_sys.system_body) {
        return;
    }

    char *compatible = get_compatible_name();
    char *soc = compatible ? get_soc_name(compatible) : NULL;
    char *kernel = get_kernel_version(NULL);
    double cpu = get_cpu_usage();

    long mem_total = read_meminfo_kb("MemTotal:");
    long mem_avail = read_meminfo_kb("MemAvailable:");
    double uptime = read_uptime_sec();
    char uptime_str[64];
    format_uptime(uptime, uptime_str, sizeof(uptime_str));

    char buf[768];
    int off = 0;

    off += snprintf(buf + off, sizeof(buf) - off, "芯片型号：%s\n", soc ? soc : "未知");
    off += snprintf(buf + off, sizeof(buf) - off, "设备树兼容：%s\n",
                    compatible ? compatible : "未知");
    off += snprintf(buf + off, sizeof(buf) - off, "内核版本：%s\n", kernel ? kernel : "未知");
    off += snprintf(buf + off, sizeof(buf) - off, "CPU 占用：%.1f%%\n", cpu);

    if (mem_total > 0 && mem_avail >= 0) {
        long mem_used = mem_total - mem_avail;
        off += snprintf(buf + off, sizeof(buf) - off, "内存使用：%ld / %ld MB (%.0f%%)\n",
                        mem_used / 1024, mem_total / 1024,
                        100.0 * (double)mem_used / (double)mem_total);
    } else {
        off += snprintf(buf + off, sizeof(buf) - off, "内存使用：未知\n");
    }

    snprintf(buf + off, sizeof(buf) - off, "运行时长：%s", uptime_str);
    lv_label_set_text(g_sys.system_body, buf);

    if (compatible) {
        free(compatible);
    }
    if (soc) {
        free(soc);
    }
    if (kernel) {
        free(kernel);
    }
}

/* 定时刷新系统动态指标（CPU、内存、运行时长）。 */
static void refresh_timer_cb(lv_timer_t *t) {
    (void)t;
    refresh_system_info();
}

/* 启动/停止动态信息刷新定时器。 */
static void start_refresh_timer(void) {
    if (!g_sys.refresh_timer) {
        g_sys.refresh_timer = lv_timer_create(refresh_timer_cb, SYS_REFRESH_MS, NULL);
    }
}

static void stop_refresh_timer(void) {
    if (g_sys.refresh_timer) {
        lv_timer_del(g_sys.refresh_timer);
        g_sys.refresh_timer = NULL;
    }
}

/* 在状态栏显示一条提示。 */
static void gpio_set_status(const char *text) {
    if (g_sys.gpio_status) {
        lv_label_set_text(g_sys.gpio_status, text);
    }
}

/* 按当前方向刷新某行的方向/操作文字与测试按钮可用性。 */
static void gpio_row_refresh_ui(gpio_row_t *row) {
    if (!row) {
        return;
    }
    bool out = (row->dir == GPIO_DIR_OUTPUT);
    if (row->dir_label) {
        lv_label_set_text(row->dir_label, out ? "输出" : "输入");
    }
    if (row->act_label) {
        lv_label_set_text(row->act_label, out ? "翻转" : "读取");
    }
    if (row->test_btn) {
        if (out) {
            lv_obj_clear_state(row->test_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(row->test_btn, LV_STATE_DISABLED);
        }
    }
}

/* 从 sysfs 读取该行引脚的真实方向与电平并刷新显示。 */
static void gpio_row_sync_from_hw(gpio_row_t *row) {
    if (!row || row->pin < 0) {
        return;
    }

    gpio_dir_t dir;
    if (gpio_get_direction(row->pin, &dir) == 0) {
        row->dir = dir;
    }
    gpio_row_refresh_ui(row);

    int val = gpio_get_value(row->pin);
    if (val >= 0) {
        if (row->dir == GPIO_DIR_OUTPUT) {
            row->out_cache = val;
        }
        char b[8];
        snprintf(b, sizeof(b), "%d", val);
        if (row->value_label) {
            lv_label_set_text(row->value_label, b);
        }
    } else if (row->value_label) {
        lv_label_set_text(row->value_label, "--");
    }
}

/* 同步所有 GPIO 行（进入界面时调用）。测试进行中则跳过，避免干扰。 */
static void gpio_sync_all_rows(void) {
    if (g_sys.gpio_test_timer) {
        return;
    }
    for (int i = 0; i < GPIO_PRESET_COUNT; i++) {
        gpio_row_sync_from_hw(&g_sys.gpio_rows[i]);
    }
}

/* 「方向」按钮：在输入/输出间切换并立即写入硬件。 */
static void on_row_dir_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    gpio_row_t *row = (gpio_row_t *)lv_event_get_user_data(e);
    if (!row) {
        return;
    }

    gpio_dir_t next = (row->dir == GPIO_DIR_OUTPUT) ? GPIO_DIR_INPUT : GPIO_DIR_OUTPUT;
    char buf[96];
    if (gpio_set_direction(row->pin, next) != 0) {
        snprintf(buf, sizeof(buf), "gpio%d 设方向失败(权限/占用?)", row->pin);
        gpio_set_status(buf);
        return;
    }

    row->dir = next;
    row->out_cache = 0;
    if (next == GPIO_DIR_OUTPUT) {
        gpio_set_value(row->pin, 0);
    }
    gpio_row_refresh_ui(row);
    if (row->value_label) {
        lv_label_set_text(row->value_label, "--");
    }
    snprintf(buf, sizeof(buf), "gpio%d 方向已设为 %s", row->pin,
             next == GPIO_DIR_OUTPUT ? "输出" : "输入");
    gpio_set_status(buf);
}

/* 操作按钮：输出方向翻转电平，输入方向读取电平。 */
static void on_row_action_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    gpio_row_t *row = (gpio_row_t *)lv_event_get_user_data(e);
    if (!row) {
        return;
    }

    gpio_dir_t dir;
    if (gpio_get_direction(row->pin, &dir) != 0) {
        gpio_set_status("请先设置该引脚方向");
        return;
    }
    row->dir = dir;
    gpio_row_refresh_ui(row);

    char buf[64];
    if (dir == GPIO_DIR_OUTPUT) {
        int next = row->out_cache ? 0 : 1;
        if (gpio_set_value(row->pin, next) != 0) {
            gpio_set_status("写电平失败");
            return;
        }
        row->out_cache = next;
        snprintf(buf, sizeof(buf), "%d", next);
        lv_label_set_text(row->value_label, buf);
        snprintf(buf, sizeof(buf), "gpio%d 输出=%d", row->pin, next);
        gpio_set_status(buf);
    } else {
        int val = gpio_get_value(row->pin);
        if (val < 0) {
            gpio_set_status("读电平失败");
            return;
        }
        snprintf(buf, sizeof(buf), "%d", val);
        lv_label_set_text(row->value_label, buf);
        snprintf(buf, sizeof(buf), "gpio%d 输入=%d", row->pin, val);
        gpio_set_status(buf);
    }
}

/* 停止输出测试定时器并恢复测试按钮可用。 */
static void stop_gpio_test(void) {
    if (g_sys.gpio_test_timer) {
        lv_timer_del(g_sys.gpio_test_timer);
        g_sys.gpio_test_timer = NULL;
    }
    if (g_sys.gpio_test_row && g_sys.gpio_test_row->test_btn) {
        lv_obj_clear_state(g_sys.gpio_test_row->test_btn, LV_STATE_DISABLED);
    }
    g_sys.gpio_test_row = NULL;
}

/* 输出测试定时器：周期性翻转输出电平，结束后拉低。 */
static void gpio_test_timer_cb(lv_timer_t *t) {
    (void)t;
    gpio_row_t *row = g_sys.gpio_test_row;

    if (g_sys.gpio_test_remaining <= 0) {
        gpio_set_value(g_sys.gpio_test_pin, 0);
        if (row) {
            row->out_cache = 0;
            if (row->value_label) {
                lv_label_set_text(row->value_label, "0");
            }
        }
        gpio_set_status("输出测试完成");
        stop_gpio_test();
        return;
    }

    int level = (g_sys.gpio_test_remaining % 2 == 0) ? 1 : 0;
    if (gpio_set_value(g_sys.gpio_test_pin, level) != 0) {
        gpio_set_status("测试中写电平失败");
        stop_gpio_test();
        return;
    }
    if (row) {
        row->out_cache = level;
        if (row->value_label) {
            char b[8];
            snprintf(b, sizeof(b), "%d", level);
            lv_label_set_text(row->value_label, b);
        }
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "gpio%d 输出测试中…剩余 %d 次", g_sys.gpio_test_pin,
             g_sys.gpio_test_remaining);
    gpio_set_status(buf);
    g_sys.gpio_test_remaining--;
}

/* 测试按钮：仅输出方向有效，周期性翻转电平输出测试信号。 */
static void on_row_test_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (g_sys.gpio_test_timer) {
        return; /* 已有测试在进行 */
    }
    gpio_row_t *row = (gpio_row_t *)lv_event_get_user_data(e);
    if (!row) {
        return;
    }

    gpio_dir_t dir;
    if (gpio_get_direction(row->pin, &dir) != 0) {
        gpio_set_status("请先设置该引脚方向");
        return;
    }
    if (dir != GPIO_DIR_OUTPUT) {
        gpio_set_status("测试输出需先将方向设为输出");
        return;
    }

    g_sys.gpio_test_row = row;
    g_sys.gpio_test_pin = row->pin;
    g_sys.gpio_test_remaining = SYS_GPIO_TEST_TOGGLES;
    lv_obj_add_state(row->test_btn, LV_STATE_DISABLED);
    gpio_set_status("开始输出测试…");
    g_sys.gpio_test_timer = lv_timer_create(gpio_test_timer_cb, SYS_GPIO_TEST_PERIOD_MS, NULL);
}

/* 表头/单元格通用文本标签（垂直居中，按 x 偏移定位）。 */
static lv_obj_t *gpio_cell_label(lv_obj_t *parent, const char *text, lv_coord_t x,
                                 lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    apply_font(l);
    lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, x, 0);
    return l;
}

/* 单元格按钮（垂直居中），out_label 回填按钮文字标签。 */
static lv_obj_t *gpio_cell_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t w, lv_color_t bg,
                               const char *text, lv_event_cb_t cb, void *ud,
                               lv_obj_t **out_label) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, GP_BTN_H);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, x, 0);
    lv_obj_set_style_bg_color(b, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    }
    lv_obj_t *l = lv_label_create(b);
    apply_font(l);
    lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    if (out_label) {
        *out_label = l;
    }
    return b;
}

/* 构建 GPIO 设置卡片（表格：GPIO口 / 引脚 / 方向 / 操作）。 */
static void build_gpio_card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h) {
    g_sys.gpio_card = lv_obj_create(parent);
    lv_obj_remove_style_all(g_sys.gpio_card);
    lv_obj_set_size(g_sys.gpio_card, w, h);
    lv_obj_set_style_bg_color(g_sys.gpio_card, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_sys.gpio_card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g_sys.gpio_card, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_sys.gpio_card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(g_sys.gpio_card, lv_color_hex(0xe5e7eb), LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_sys.gpio_card, 20, LV_PART_MAIN);
    lv_obj_clear_flag(g_sys.gpio_card, LV_OBJ_FLAG_SCROLLABLE);

    const lv_coord_t inner_w = w - 40;

    lv_obj_t *heading = lv_label_create(g_sys.gpio_card);
    apply_font(heading);
    lv_obj_set_style_text_color(heading, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_label_set_text(heading, "GPIO 设置");
    lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *divider = lv_obj_create(g_sys.gpio_card);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, inner_w, 1);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0xe5e7eb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(divider, LV_ALIGN_TOP_LEFT, 0, 34);

    /* 表头 */
    lv_color_t head_color = lv_color_hex(0x6b7280);
    lv_obj_t *h_name = gpio_cell_label(g_sys.gpio_card, "GPIO口", GP_COL_NAME_X, head_color);
    lv_obj_align(h_name, LV_ALIGN_TOP_LEFT, GP_COL_NAME_X, 52);
    lv_obj_t *h_pin = gpio_cell_label(g_sys.gpio_card, "引脚", GP_COL_PIN_X, head_color);
    lv_obj_align(h_pin, LV_ALIGN_TOP_LEFT, GP_COL_PIN_X, 52);
    lv_obj_t *h_dir = gpio_cell_label(g_sys.gpio_card, "方向", GP_COL_DIR_X, head_color);
    lv_obj_align(h_dir, LV_ALIGN_TOP_LEFT, GP_COL_DIR_X, 52);
    lv_obj_t *h_op = gpio_cell_label(g_sys.gpio_card, "操作", GP_COL_ACT_X, head_color);
    lv_obj_align(h_op, LV_ALIGN_TOP_LEFT, GP_COL_ACT_X, 52);

    /* 数据行 */
    for (int i = 0; i < GPIO_PRESET_COUNT; i++) {
        gpio_row_t *row = &g_sys.gpio_rows[i];
        row->pin = gpio_pin_from_name(g_gpio_presets[i]);
        row->dir = GPIO_DIR_INPUT;
        row->out_cache = 0;

        lv_obj_t *rc = lv_obj_create(g_sys.gpio_card);
        lv_obj_remove_style_all(rc);
        lv_obj_set_size(rc, inner_w, GP_ROW_H);
        lv_obj_align(rc, LV_ALIGN_TOP_LEFT, 0, GP_ROW_Y0 + i * GP_ROW_H);
        lv_obj_set_style_bg_opa(rc, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_color(rc, lv_color_hex(0xeef0f2), LV_PART_MAIN);
        lv_obj_set_style_border_width(rc, 1, LV_PART_MAIN);
        lv_obj_set_style_border_side(rc, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
        lv_obj_clear_flag(rc, LV_OBJ_FLAG_SCROLLABLE);

        /* GPIO口 */
        gpio_cell_label(rc, g_gpio_presets[i], GP_COL_NAME_X, lv_color_hex(0x111827));

        /* 引脚号 */
        char pin_txt[16];
        if (row->pin >= 0) {
            snprintf(pin_txt, sizeof(pin_txt), "%d", row->pin);
        } else {
            snprintf(pin_txt, sizeof(pin_txt), "--");
        }
        gpio_cell_label(rc, pin_txt, GP_COL_PIN_X, lv_color_hex(0x374151));

        /* 方向（点击切换并应用） */
        gpio_cell_btn(rc, GP_COL_DIR_X, GP_COL_DIR_W, lv_color_hex(0x2563eb), "输入",
                      on_row_dir_clicked, row, &row->dir_label);

        /* 操作：电平显示 + 读取/翻转 + 测试 */
        row->value_label = gpio_cell_label(rc, "--", GP_COL_VAL_X, lv_color_hex(0x111827));

        gpio_cell_btn(rc, GP_COL_ACT_X, GP_BTN_W, lv_color_hex(0x16a34a), "读取",
                      on_row_action_clicked, row, &row->act_label);

        row->test_btn = gpio_cell_btn(rc, GP_COL_TEST_X, GP_BTN_W, lv_color_hex(0xf59e0b),
                                      "测试输出", on_row_test_clicked, row, NULL);

        /* 初始即读取硬件真实方向/电平显示 */
        gpio_row_sync_from_hw(row);
    }

    /* 状态提示 */
    g_sys.gpio_status = lv_label_create(g_sys.gpio_card);
    apply_font(g_sys.gpio_status);
    lv_obj_set_width(g_sys.gpio_status, inner_w);
    lv_obj_set_style_text_color(g_sys.gpio_status, lv_color_hex(0x6b7280), LV_PART_MAIN);
    lv_label_set_long_mode(g_sys.gpio_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(g_sys.gpio_status, "点击「方向」切换输入/输出；输出时可「测试输出」");
    lv_obj_align(g_sys.gpio_status, LV_ALIGN_TOP_LEFT, 0, GP_ROW_Y0 + GPIO_PRESET_COUNT * GP_ROW_H + 12);
}

/* 返回按钮：回到桌面首页。 */
static void on_back_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (g_sys.ctx && g_sys.ctx->on_back) {
        g_sys.ctx->on_back(g_sys.ctx->user_data);
    }
}

lv_obj_t *system_app_create(lv_obj_t *parent, const lvgl_app_context_t *ctx) {
    const lv_font_t *font = ctx ? ctx->font : NULL;
    const lv_font_t *caption_font = ctx ? ctx->caption_font : NULL;

    g_sys.ctx = ctx;
    g_sys.text_font = caption_font ? caption_font : font;

    g_sys.page = lv_obj_create(parent);
    lv_obj_set_size(g_sys.page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_sys.page, lv_color_hex(0xf3f4f6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_sys.page, LV_OPA_COVER, LV_PART_MAIN);
    style_clear(g_sys.page);
    lv_obj_add_flag(g_sys.page, LV_OBJ_FLAG_HIDDEN);

    /* 允许竖向滚动：内容超出屏幕时可向下滑动查看 */
    lv_obj_add_flag(g_sys.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g_sys.page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_sys.page, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_bottom(g_sys.page, SYS_PAGE_PAD, LV_PART_MAIN);

    /* 顶部标题栏 */
    lv_obj_t *header = lv_obj_create(g_sys.page);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, SYS_SCREEN_W - SYS_PAGE_PAD * 2, SYS_HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, SYS_PAGE_PAD, SYS_PAGE_PAD);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    g_sys.back_btn = ocr_ui_icon_button_create(header, OCR_UI_ICON_BTN_SIZE, LV_SYMBOL_LEFT,
                                               lv_color_hex(0x374151), on_back_clicked, NULL,
                                               false);
    lv_obj_align(g_sys.back_btn, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *title = lv_label_create(header);
    apply_font(title);
    lv_obj_set_style_text_color(title, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_label_set_text(title, "系统设置");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, OCR_UI_ICON_BTN_SIZE + 16, 0);

    /* 上方两张信息卡：相机信息 / 系统信息 */
    const lv_coord_t cards_y = SYS_PAGE_PAD + SYS_HEADER_H + 16;

    g_sys.camera_card =
        create_info_card(g_sys.page, SYS_INFO_CARD_W, SYS_INFO_CARD_H, "相机信息", &g_sys.camera_body);
    lv_obj_set_pos(g_sys.camera_card, SYS_PAGE_PAD, cards_y);

    g_sys.system_card =
        create_info_card(g_sys.page, SYS_INFO_CARD_W, SYS_INFO_CARD_H, "系统信息", &g_sys.system_body);
    lv_obj_set_pos(g_sys.system_card, SYS_PAGE_PAD + SYS_INFO_CARD_W + SYS_COL_GAP, cards_y);

    /* 下方整行宽的 GPIO 表格（高度按行数自适应，配合页面滚动查看） */
    const lv_coord_t gpio_h = 20 + GP_ROW_Y0 + GPIO_PRESET_COUNT * GP_ROW_H + 56;
    build_gpio_card(g_sys.page, SYS_GPIO_CARD_W, gpio_h);
    lv_obj_set_pos(g_sys.gpio_card, SYS_PAGE_PAD, cards_y + SYS_INFO_CARD_H + SYS_COL_GAP);

    return g_sys.page;
}

void system_app_destroy(void) {
    stop_refresh_timer();
    stop_gpio_test();
    g_sys.camera_card = NULL;
    g_sys.camera_body = NULL;
    g_sys.system_card = NULL;
    g_sys.system_body = NULL;
    g_sys.gpio_card = NULL;
    g_sys.gpio_status = NULL;
    memset(g_sys.gpio_rows, 0, sizeof(g_sys.gpio_rows));
    g_sys.back_btn = NULL;
    if (g_sys.page) {
        lv_obj_del(g_sys.page);
        g_sys.page = NULL;
    }
    g_sys.ctx = NULL;
    g_sys.text_font = NULL;
}

void system_app_show(void) {
    if (g_sys.page) {
        refresh_camera_info();
        refresh_system_info();
        gpio_sync_all_rows();
        start_refresh_timer();
        lv_obj_clear_flag(g_sys.page, LV_OBJ_FLAG_HIDDEN);
    }
}

void system_app_hide(void) {
    stop_refresh_timer();
    stop_gpio_test();
    if (g_sys.page) {
        lv_obj_add_flag(g_sys.page, LV_OBJ_FLAG_HIDDEN);
    }
}
