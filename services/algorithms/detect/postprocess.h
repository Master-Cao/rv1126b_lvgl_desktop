#ifndef _RKNN_YOLOV8_DEMO_POSTPROCESS_H_
#define _RKNN_YOLOV8_DEMO_POSTPROCESS_H_

/* 取自 rknn_model_zoo / yolov8/cpp/postprocess.h，去掉 RV1106 分支。
 * 调用方必须先 include "yolov8.h"——这里复用其中定义的 rknn_app_context_t 和 letterbox_t，
 * 头文件加载顺序与原 demo 一致。 */

#include <stdint.h>
#include "rknn_api.h"
#include "common.h"

#define OBJ_NAME_MAX_SIZE 64
#define OBJ_NUMB_MAX_SIZE 128
#define OBJ_CLASS_NUM     80
#define NMS_THRESH        0.45f
#define BOX_THRESH        0.25f

typedef struct {
    image_rect_t box;
    float prop;
    int cls_id;
} object_detect_result;

typedef struct {
    int id;
    int count;
    object_detect_result results[OBJ_NUMB_MAX_SIZE];
} object_detect_result_list;

#ifdef __cplusplus
extern "C" {
#endif

int init_post_process(void);
void deinit_post_process(void);
const char *coco_cls_to_name(int cls_id);

/* post_process 声明在 yolov8.h，因为它的签名同时需要 rknn_app_context_t 与 letterbox_t，
 * 这两个类型由 yolov8.h 定义。 */

#ifdef __cplusplus
}
#endif

#endif /* _RKNN_YOLOV8_DEMO_POSTPROCESS_H_ */
