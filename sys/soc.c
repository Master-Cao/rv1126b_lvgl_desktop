#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "soc.h"

char *get_soc_name(char *buf)
{
    const char *start;
    char *ret;
    int i;

    if (!buf) {
        return strdup("未知");
    }

    /* RV1126 系列为 rvxxxx；RK3568 等为 rkxxxx */
    start = strstr(buf, "rv");
    if (!start) {
        start = strstr(buf, "rk");
    }
    if (!start) {
        return strdup("未知");
    }

    ret = strdup(start);
    if (!ret) {
        return NULL;
    }

    ret[0] = (char)toupper((unsigned char)ret[0]);
    if (ret[1] != '\0') {
        ret[1] = (char)toupper((unsigned char)ret[1]);
    }

    for (i = 2; ret[i] != '\0'; i++) {
        char c = ret[i];
        if (c == '-' || c == ',' || c == ' ') {
            ret[i] = '\0';
            break;
        }
        ret[i] = (char)toupper((unsigned char)c);
    }

    return ret;
}

char *get_compatible_name(void)
{
    const char *path = "/proc/device-tree/compatible";
    int fd = open(path, O_RDONLY);
    char name[128];
    int size = sizeof(name);
    ssize_t soc_name_len = 0;

    if (fd < 0)
    {
        printf("open %s error\n", path);
        return NULL;
    }

    snprintf(name, size - 1, "unknown");
    soc_name_len = read(fd, name, size - 1);
    if (soc_name_len > 0)
    {
        name[soc_name_len] = '\0';
        /* replacing the termination character to space */
        for (char *ptr = name;; ptr = name)
        {
            ptr += strnlen(name, size);
            if (ptr >= name + soc_name_len - 1)
                break;
            *ptr = ' ';
        }

        printf("chip name: %s\n", name);
    }
    else
    {
        printf("read failed %d\n", soc_name_len);
    }

    close(fd);

    return strdup(name);
}

char *get_kernel_version(char *buf)
{
    char *info = NULL;
    char *ret;
    const char *ver_start;
    int size;

    if (!buf) {
        info = get_system_version();
    } else {
        info = buf;
    }

    if (!info) {
        return strdup("未知");
    }

    ver_start = strstr(info, "Linux version ");
    if (ver_start) {
        ver_start += sizeof("Linux version ") - 1;
    } else {
        ver_start = info;
    }

    ret = strdup(ver_start);
    if (!ret) {
        if (!buf) {
            free(info);
        }
        return NULL;
    }

    size = (int)strlen(ret);
    for (int i = 0; i < size; i++) {
        char c = *(ret + i);
        if (c == ' ') {
            *(ret + i) = '\0';
            break;
        }
    }

    if (!buf) {
        free(info);
    }

    return ret;
}

char *get_system_version(void)
{
    const char *path = "/proc/version";
    int fd = open(path, O_RDONLY);
    char info[1024];
    int size = sizeof(info);
    ssize_t len = 0;

    if (fd < 0)
    {
        printf("open %s error\n", path);
        return NULL;
    }

    snprintf(info, size - 1, "unknown");
    len = read(fd, info, size - 1);
    if (len <= 0)
    {
        printf("read failed %d\n", len);
        return NULL;
    }
    info[len] = '\0';

    close(fd);

    return strdup(info);
}

