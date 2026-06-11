#ifndef YOLOV8_PLATE_H
#define YOLOV8_PLATE_H

/*
 * 车牌 YOLOv8 检测 RKNN（移植自 detect/yolov8，后处理对齐 yolov8 工程）。
 */
#include <stdbool.h>

#include "common.h"
#include "rknn_api.h"

typedef struct {
    int x_pad;
    int y_pad;
    float scale;
} letterbox_t;

typedef struct {
    rknn_context rknn_ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr *input_attrs;
    rknn_tensor_attr *output_attrs;
    int model_channel;
    int model_width;
    int model_height;
    bool is_quant;
} rknn_app_context_t;

#include "postprocess.h"

#ifdef __cplusplus
extern "C" {
#endif

int plate_init_yolov8_model(const char *model_path, rknn_app_context_t *app_ctx);
int plate_release_yolov8_model(rknn_app_context_t *app_ctx);
int plate_inference_yolov8_model(rknn_app_context_t *app_ctx, image_buffer_t *img,
                                 object_detect_result_list *od_results);

int plate_post_process(rknn_app_context_t *app_ctx, void *outputs, letterbox_t *letter_box,
                       float conf_threshold, float nms_threshold,
                       object_detect_result_list *od_results);

#ifdef __cplusplus
}
#endif

#endif /* YOLOV8_PLATE_H */
