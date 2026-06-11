#ifndef YOLOV8_SEG_H
#define YOLOV8_SEG_H

/*
 * YOLOv8-Seg RKNN（移植自 atk_yolov8_seg_cam，OpenCV letterbox，无 RGA）。
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
    int input_image_width;
    int input_image_height;
    bool is_quant;
} rknn_app_context_t;

#include "postprocess.h"

#ifdef __cplusplus
extern "C" {
#endif

int init_yolov8_seg_model(const char *model_path, rknn_app_context_t *app_ctx);
int release_yolov8_seg_model(rknn_app_context_t *app_ctx);
int inference_yolov8_seg_model(rknn_app_context_t *app_ctx, image_buffer_t *img,
                               object_detect_result_list *od_results);

/* DFL + NMS + mask 后处理，依赖 rknn_app_context_t / letterbox_t，声明放此处。 */
int seg_post_process(rknn_app_context_t *app_ctx, rknn_output *outputs, letterbox_t *letter_box,
                     float conf_threshold, float nms_threshold, object_detect_result_list *od_results);

#ifdef __cplusplus
}
#endif

#endif /* YOLOV8_SEG_H */
