#ifndef SEG_ALGO_H
#define SEG_ALGO_H

#include "../algo_types.h"
#include "../../camera/camera_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEG_CLASS_NAME_MAX_LEN 32
#define SEG_MAX_INSTANCES      16

/* 与 camera_preview_box_t 同布局，便于 UI 绘制实例外接框。 */
typedef struct {
    int x1, y1;
    int x2, y2;
    int x3, y3;
    int x4, y4;
} seg_box_t;

typedef struct {
    seg_box_t box;
    int class_id;
    char class_name[SEG_CLASS_NAME_MAX_LEN];
    float score;
    /* 分割 mask：全图合并二值图，由 seg_algo 持有，get_result 返回指针在下一帧前有效 */
} seg_item_t;

typedef struct {
    seg_item_t instances[SEG_MAX_INSTANCES];
    int count;
    int src_w;
    int src_h;
    uint32_t seq;
    int infer_ms;
    uint8_t *seg_mask; /* 全图合并 mask，0=背景；由 seg_algo 管理生命周期 */
    bool has_mask;
} seg_result_set_t;

typedef struct {
    const char *model_path;
} seg_algo_config_t;

typedef struct seg_algo seg_algo_t;

seg_algo_t *seg_algo_create(const seg_algo_config_t *config);

int seg_algo_start(seg_algo_t *algo);
int seg_algo_stop(seg_algo_t *algo);

bool seg_algo_is_running(const seg_algo_t *algo);
bool seg_algo_models_ready(const seg_algo_t *algo);
const char *seg_algo_get_model_path(const seg_algo_t *algo);

int seg_algo_submit_rgb888(seg_algo_t *algo, const uint8_t *rgb, int w, int h);
int seg_algo_get_result(seg_algo_t *algo, seg_result_set_t *out);

int seg_algo_process_frame(seg_algo_t *algo, const camera_frame_t *frame,
                           algo_result_cb_t cb, void *user_data);

void seg_algo_destroy(seg_algo_t *algo);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SEG_ALGO_H */
