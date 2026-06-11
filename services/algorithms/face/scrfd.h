#ifndef SCRFD_H
#define SCRFD_H

/*
 * SCRFD 人脸检测（移植自 scrfd/main.py 的处理逻辑，改为 C + RKNN + OpenCV）。
 * 模型：face_det.rknn（SCRFD），输入 640x640，3 个 FPN stride(8/16/32)，每位置 2 anchor。
 * 输出 9 个张量：grouped 为 [score x3, bbox x3, kps x3]。
 * 推理/后处理缓冲区在 init 时分配在堆上，避免 worker 线程栈溢出。
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

#define SCRFD_FPN_NUM      3
#define SCRFD_OUTPUT_NUM   9
#define SCRFD_KPS_NUM      5
#define SCRFD_MAX_RESULTS  128

typedef struct scrfd_point_t {
    int x;
    int y;
} scrfd_point_t;

typedef struct scrfd_object_t {
    int left;
    int top;
    int right;
    int bottom;
    float score;
    scrfd_point_t point[SCRFD_KPS_NUM];
} scrfd_object_t;

typedef struct {
    int count;
    scrfd_object_t object[SCRFD_MAX_RESULTS];
} scrfd_result;

typedef struct scrfd_context_t {
    rknn_context rknn_ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr *input_attrs;
    rknn_tensor_attr *output_attrs;
    int model_channel;
    int model_width;
    int model_height;
    bool input_is_int8;

    int total_anchors;

    /* 堆上工作区 */
    unsigned char *input_u8;  /* letterbox 后 BGR */
    int8_t *input_i8;
    int input_bytes;

    float *deq[SCRFD_OUTPUT_NUM]; /* 反量化后的各输出 */

    float *cand_box;   /* [total_anchors*4]：x,y,w,h */
    float *cand_score; /* [total_anchors] */
    float *cand_kps;   /* [total_anchors*10] */
    int *cand_order;   /* 排序索引 */
    int *cand_keep;    /* NMS 保留标记 */

    scrfd_result *infer_result;
} scrfd_context_t;

int init_scrfd_model(const char *model_path, scrfd_context_t *app_ctx);

int release_scrfd_model(scrfd_context_t *app_ctx);

int inference_scrfd_model(scrfd_context_t *app_ctx, image_buffer_t *img, scrfd_result *out_result);

#ifdef __cplusplus
}
#endif

#endif /* SCRFD_H */
