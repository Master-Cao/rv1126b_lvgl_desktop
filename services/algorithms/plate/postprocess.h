#ifndef PLATE_POSTPROCESS_H
#define PLATE_POSTPROCESS_H

#include <stdint.h>

#include "common.h"
#include "rknn_api.h"

#define OBJ_NAME_MAX_SIZE 64
#define OBJ_NUMB_MAX_SIZE 128
#define OBJ_CLASS_NUM     1
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

int plate_init_post_process(void);
void plate_deinit_post_process(void);
const char *plate_cls_to_name(int cls_id);

#ifdef __cplusplus
}
#endif

#endif /* PLATE_POSTPROCESS_H */
