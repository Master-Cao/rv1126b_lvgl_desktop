#ifndef _RKNN_DEMO_YOLOV8_H_
#define _RKNN_DEMO_YOLOV8_H_

/* 取自 rknn_model_zoo / yolov8/cpp/yolov8.h，去掉 RV1106 专用 dma_buf 字段，
 * 仅保留 rv1126b 通用走 RKNN_TENSOR_NHWC + UINT8 输入的最小集。
 *
 * letterbox_t 原本在 image_utils.h，本工程把 yolov8 演示需要的预处理改成
 * 纯 OpenCV 实现（见 yolov8.cc），因此把这个结构搬到这里，避免引入 librga。
 */

#include <stdbool.h>

#include "rknn_api.h"
#include "common.h"

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

int init_yolov8_model(const char *model_path, rknn_app_context_t *app_ctx);

int release_yolov8_model(rknn_app_context_t *app_ctx);

int inference_yolov8_model(rknn_app_context_t *app_ctx, image_buffer_t *img,
                           object_detect_result_list *od_results);

/* 三分支 DFL 解码 + NMS 后处理，依赖 rknn_app_context_t / letterbox_t，
 * 因此声明放在 yolov8.h，实现在 postprocess.cc。 */
int post_process(rknn_app_context_t *app_ctx, void *outputs, letterbox_t *letter_box,
                 float conf_threshold, float nms_threshold,
                 object_detect_result_list *od_results);

#ifdef __cplusplus
}
#endif

#endif /* _RKNN_DEMO_YOLOV8_H_ */
