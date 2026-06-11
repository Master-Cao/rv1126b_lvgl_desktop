#ifndef LPRNET_H
#define LPRNET_H

/*
 * LPRNet 车牌字符识别（移植自 rknn_model_zoo/LPRNet，改为 C 接口 + RKNN + OpenCV）。
 * 模型：lprnet 风格，输入 94x24 BGR(NHWC,UINT8)，输出 [68 类 x 18 位]。
 * 后处理：逐位 argmax + 去重去 blank（CTC 贪心解码）→ 车牌字符串。
 * 输入预处理（resize + RGB→BGR）在 inference 内部用 OpenCV 完成；
 * 模型输入缓冲复用 context 堆内存，避免每帧分配。
 */

#include "rknn_api.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _RKNN_MODEL_ZOO_COMMON_H_
#define _RKNN_MODEL_ZOO_COMMON_H_
typedef enum {
    IMAGE_FORMAT_GRAY8,
    IMAGE_FORMAT_RGB888,
    IMAGE_FORMAT_RGBA8888,
    IMAGE_FORMAT_YUV420SP_NV21,
    IMAGE_FORMAT_YUV420SP_NV12,
} image_format_t;

typedef struct {
    int width;
    int height;
    int width_stride;
    int height_stride;
    image_format_t format;
    unsigned char *virt_addr;
    int size;
    int fd;
} image_buffer_t;
#endif /* _RKNN_MODEL_ZOO_COMMON_H_ */

#define LPRNET_OUT_ROWS 68 /* 字典类别数（含 blank） */
#define LPRNET_OUT_COLS 18 /* 时间步/车牌位置数 */
#define LPRNET_TEXT_MAX 64 /* 输出字符串字节上限（UTF-8） */

typedef struct lprnet_context_t {
    rknn_context rknn_ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr *input_attrs;
    rknn_tensor_attr *output_attrs;
    int model_channel;
    int model_width;
    int model_height;

    unsigned char *input_buf; /* 模型输入 BGR 缓冲（堆，复用） */
    int input_bytes;
} lprnet_context_t;

int init_lprnet_model(const char *model_path, lprnet_context_t *app_ctx);

int release_lprnet_model(lprnet_context_t *app_ctx);

/* 对一帧 RGB888 图像（通常为车牌区域）识别字符。
 * out_text 写入 UTF-8 车牌串；out_conf 为各字符平均置信度(0~1)，可传 NULL。 */
int inference_lprnet_model(lprnet_context_t *app_ctx, image_buffer_t *src_img, char *out_text,
                           int out_text_size, float *out_conf);

#ifdef __cplusplus
}
#endif

#endif /* LPRNET_H */
