/*
 * GPIO sysfs 易用封装：对 /sys/class/gpio/ 的 export / direction / value / unexport
 * 节点做文件 I/O 封装，方便应用层直接调用。
 * 参考瑞芯微 / 正点原子 RV1126B 平台 sysfs GPIO 用法。
 */
#include "gpio.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GPIO_SYSFS_ROOT "/sys/class/gpio"

/* 向指定文件写入字符串。成功返回 0，失败返回 -1。 */
static int write_sysfs(const char *path, const char *value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }

    size_t len = strlen(value);
    ssize_t n = write(fd, value, len);
    close(fd);

    return (n == (ssize_t)len) ? 0 : -1;
}

/* 从指定文件读取内容到 buf（末尾补 '\0'）。成功返回读取字节数，失败返回 -1。 */
static ssize_t read_sysfs(const char *path, char *buf, size_t buf_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    ssize_t n = read(fd, buf, buf_size - 1);
    close(fd);
    if (n < 0) {
        return -1;
    }
    buf[n] = '\0';
    return n;
}

int gpio_pin_from_name(const char *name) {
    if (!name) {
        return -1;
    }

    /* 跳过前缀："GPIO"（如 GPIO5_C1）或 "P"（如 P0_D0），二者等价 */
    const char *p = name;
    if (strncasecmp(p, "GPIO", 4) == 0) {
        p += 4;
    } else if ((*p == 'P' || *p == 'p') && isdigit((unsigned char)p[1])) {
        p += 1;
    }

    if (!isdigit((unsigned char)*p)) {
        return -1;
    }

    int bank = 0;
    while (isdigit((unsigned char)*p)) {
        bank = bank * 10 + (*p - '0');
        p++;
    }

    /* 可选分隔符 '_' */
    if (*p == '_') {
        p++;
    }

    char group = (char)toupper((unsigned char)*p);
    if (group < 'A' || group > 'D') {
        return -1;
    }
    p++;

    if (!isdigit((unsigned char)*p)) {
        return -1;
    }
    int index = 0;
    while (isdigit((unsigned char)*p)) {
        index = index * 10 + (*p - '0');
        p++;
    }
    if (index < 0 || index > 7) {
        return -1;
    }

    return bank * 32 + (group - 'A') * 8 + index;
}

bool gpio_is_exported(int pin) {
    if (pin < 0) {
        return false;
    }
    char path[64];
    snprintf(path, sizeof(path), GPIO_SYSFS_ROOT "/gpio%d", pin);
    struct stat st;
    return stat(path, &st) == 0;
}

int gpio_export(int pin) {
    if (pin < 0) {
        return -1;
    }
    if (gpio_is_exported(pin)) {
        return 0;
    }
    char value[16];
    snprintf(value, sizeof(value), "%d", pin);
    return write_sysfs(GPIO_SYSFS_ROOT "/export", value);
}

int gpio_unexport(int pin) {
    if (pin < 0) {
        return -1;
    }
    if (!gpio_is_exported(pin)) {
        return 0;
    }
    char value[16];
    snprintf(value, sizeof(value), "%d", pin);
    return write_sysfs(GPIO_SYSFS_ROOT "/unexport", value);
}

int gpio_set_direction(int pin, gpio_dir_t dir) {
    if (pin < 0) {
        return -1;
    }
    if (gpio_export(pin) != 0) {
        return -1;
    }

    char path[64];
    snprintf(path, sizeof(path), GPIO_SYSFS_ROOT "/gpio%d/direction", pin);
    return write_sysfs(path, (dir == GPIO_DIR_OUTPUT) ? "out" : "in");
}

int gpio_get_direction(int pin, gpio_dir_t *dir) {
    if (pin < 0 || !dir) {
        return -1;
    }
    char path[64];
    snprintf(path, sizeof(path), GPIO_SYSFS_ROOT "/gpio%d/direction", pin);

    char buf[16];
    if (read_sysfs(path, buf, sizeof(buf)) < 0) {
        return -1;
    }

    if (strncmp(buf, "out", 3) == 0) {
        *dir = GPIO_DIR_OUTPUT;
    } else {
        *dir = GPIO_DIR_INPUT;
    }
    return 0;
}

int gpio_set_value(int pin, int value) {
    if (pin < 0) {
        return -1;
    }
    char path[64];
    snprintf(path, sizeof(path), GPIO_SYSFS_ROOT "/gpio%d/value", pin);
    return write_sysfs(path, value ? "1" : "0");
}

int gpio_get_value(int pin) {
    if (pin < 0) {
        return -1;
    }
    char path[64];
    snprintf(path, sizeof(path), GPIO_SYSFS_ROOT "/gpio%d/value", pin);

    char buf[8];
    if (read_sysfs(path, buf, sizeof(buf)) < 0) {
        return -1;
    }
    return (buf[0] == '1') ? 1 : 0;
}
