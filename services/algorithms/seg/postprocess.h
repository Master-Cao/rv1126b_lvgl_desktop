#ifndef SEG_POSTPROCESS_H
#define SEG_POSTPROCESS_H

#include <stdint.h>

#include "common.h"
#include "rknn_api.h"

#define OBJ_NAME_MAX_SIZE 64
#define OBJ_NUMB_MAX_SIZE 128
#define OBJ_CLASS_NUM 80
#define NMS_THRESH 0.45f
#define BOX_THRESH 0.25f

#define PROTO_CHANNEL 32
#define PROTO_HEIGHT 160
#define PROTO_WEIGHT 160

typedef struct {
    image_rect_t box;
    float prop;
    int cls_id;
} object_detect_result;

typedef struct {
    uint8_t *seg_mask;
} object_segment_result;

typedef struct {
    int id;
    int count;
    object_detect_result results[OBJ_NUMB_MAX_SIZE];
    object_segment_result results_seg[OBJ_NUMB_MAX_SIZE];
} object_detect_result_list;

#ifdef __cplusplus
extern "C" {
#endif

int seg_init_post_process(void);
void seg_deinit_post_process(void);
const char *seg_coco_cls_to_name(int cls_id);

/* seg_post_process 声明在 yolov8_seg.h，因为签名需要 rknn_app_context_t 与 letterbox_t。 */

#ifdef __cplusplus
}
#endif

#endif /* SEG_POSTPROCESS_H */
