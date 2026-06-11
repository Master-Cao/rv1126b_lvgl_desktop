#ifndef CAMERA_PARSER_H
#define CAMERA_PARSER_H

#include <lvgl/lvgl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* RV1126B 四核：调用线程处理 stripe0，3 个 worker 处理 stripe1~3（对齐 ppocr） */
#define CAMERA_PARSER_CONVERT_WORKER_COUNT 3
#define CAMERA_PARSER_CONVERT_STRIPE_COUNT (CAMERA_PARSER_CONVERT_WORKER_COUNT + 1)

/* YUV 查表，进程内只需初始化一次 */
void camera_parser_init_yuv_tables(void);

/* 启动/停止并行转换 worker（camera_service 在 start/stop 时调用） */
int camera_parser_start_workers(void);
void camera_parser_stop_workers(void);

/* 为 preview_w x preview_h 建立最近邻缩放 LUT（相对 src_w x src_h） */
int camera_parser_build_scale_lut(int src_w, int src_h, int dst_w, int dst_h);

void camera_parser_free_scale_lut(void);

/* YUYV/UYVY 一帧 -> lv_color 预览缓冲（4 路条带并行） */
void camera_parser_yuv422_to_lvcolor(const uint8_t *src, int src_w, int src_h, int src_stride,
                                     uint32_t pixfmt, lv_color_t *dst, int dst_w, int dst_h);

/* RGB565 一帧 -> lv_color 预览缓冲 */
void camera_parser_rgb565_to_lvcolor(const uint8_t *src, int src_w, int src_h, int src_stride,
                                     lv_color_t *dst, int dst_w, int dst_h);

/* MJPEG 解压后的 RGB888 -> lv_color 预览缓冲 */
void camera_parser_rgb888_to_lvcolor(const uint8_t *src_rgb, int src_w, int src_h,
                                      lv_color_t *dst, int dst_w, int dst_h);

/* OCR/算法路径：YUYV/UYVY 一帧 -> RGB888（不缩放，dst 尺寸 = src 尺寸），4 路并行 */
void camera_parser_yuv422_to_rgb888(const uint8_t *src, int src_w, int src_h, int src_stride,
                                    uint32_t pixfmt, uint8_t *dst_rgb);

/* RGB565 一帧 -> RGB888（不缩放） */
void camera_parser_rgb565_to_rgb888(const uint8_t *src, int src_w, int src_h, int src_stride,
                                    uint8_t *dst_rgb);

/* MJPEG 解码后的 RGB888 -> RGB888（按行 memcpy，4 路并行） */
void camera_parser_rgb888_copy(const uint8_t *src_rgb, int w, int h, uint8_t *dst_rgb);

bool camera_parser_decode_mjpeg(const uint8_t *jpeg_data, size_t jpeg_size, uint8_t *rgb_out,
                                int dst_w, int dst_h);

uint8_t *camera_parser_mjpeg_rgb_buffer(void);

#endif
