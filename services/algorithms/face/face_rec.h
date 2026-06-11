#ifndef FACE_REC_H
#define FACE_REC_H

/*
 * MobileFaceNet (w600k_mbf) 人脸特征提取。
 * 模型 face_rec.rknn：输入 112x112，输出 512 维（convert.py mean/std=127.5）。
 */
#include "rknn_api.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_REC_EMBED_DIM 512

typedef struct face_rec_context_t {
    rknn_context rknn_ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr *input_attrs;
    rknn_tensor_attr *output_attrs;
    int model_channel;
    int model_width;
    int model_height;
    bool input_is_nchw;

    unsigned char *input_buf;
    int input_bytes;
} face_rec_context_t;

int init_face_rec_model(const char *model_path, face_rec_context_t *app_ctx);

int release_face_rec_model(face_rec_context_t *app_ctx);

/* aligned_rgb: 112x112 RGB888 连续内存；out_embed 至少 FACE_REC_EMBED_DIM 个 float（已 L2 归一化）。 */
int inference_face_rec_model(face_rec_context_t *app_ctx, const unsigned char *aligned_rgb,
                             int width_stride, float *out_embed);

#ifdef __cplusplus
}
#endif

#endif /* FACE_REC_H */
