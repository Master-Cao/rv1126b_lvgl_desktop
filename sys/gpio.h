#ifndef __GPIO_H__
#define __GPIO_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GPIO 方向 */
typedef enum {
    GPIO_DIR_INPUT = 0,
    GPIO_DIR_OUTPUT,
} gpio_dir_t;

/* GPIO 电平 */
typedef enum {
    GPIO_LEVEL_LOW = 0,
    GPIO_LEVEL_HIGH = 1,
} gpio_level_t;

/*
 * 将瑞芯微风格的引脚名转换为 sysfs 引脚号。
 * 命名规则：GPIO<bank>_<group><index>，其中 group 为 A/B/C/D。
 * 计算公式：bank * 32 + (group - 'A') * 8 + index。
 * 例如 "GPIO5_C0" -> 5*32 + 2*8 + 0 = 176。
 * 解析失败返回 -1。
 */
int gpio_pin_from_name(const char *name);

/* 导出引脚（写 /sys/class/gpio/export）。已导出时返回 0。成功返回 0，失败返回 -1。 */
int gpio_export(int pin);

/* 释放引脚（写 /sys/class/gpio/unexport）。成功返回 0，失败返回 -1。 */
int gpio_unexport(int pin);

/* 判断引脚是否已导出（/sys/class/gpio/gpio<pin> 是否存在）。 */
bool gpio_is_exported(int pin);

/* 设置方向（写 direction）。内部会在需要时自动 export。成功返回 0，失败返回 -1。 */
int gpio_set_direction(int pin, gpio_dir_t dir);

/* 读取方向（读 direction）。成功返回 0 并回填 *dir，失败返回 -1。 */
int gpio_get_direction(int pin, gpio_dir_t *dir);

/* 设置输出电平（写 value）。要求引脚为输出方向。成功返回 0，失败返回 -1。 */
int gpio_set_value(int pin, int value);

/* 读取电平（读 value）。成功返回 0/1，失败返回 -1。 */
int gpio_get_value(int pin);

#ifdef __cplusplus
}
#endif

#endif
