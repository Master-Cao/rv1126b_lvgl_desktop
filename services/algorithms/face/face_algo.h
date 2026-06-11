#ifndef FACE_ALGO_H
#define FACE_ALGO_H

#include "../algo_types.h"
#include "../../camera/camera_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_LANDMARK_NUM   5
#define FACE_EMBED_DIM      512
#define FACE_ALIGN_SIZE     112
#define FACE_NAME_MAX_LEN   64
#define FACE_MAX_FACES      16

typedef struct {
    float x;
    float y;
} face_point_t;

typedef struct {
    int left;
    int top;
    int right;
    int bottom;
} face_rect_t;

/* 与 camera_preview_box_t 同布局，便于 UI 绘制检测框。 */
typedef struct {
    int x1, y1;
    int x2, y2;
    int x3, y3;
    int x4, y4;
} face_box_t;

typedef struct {
    face_box_t box;
    face_point_t landmarks[FACE_LANDMARK_NUM];
    float det_score;
    char name[FACE_NAME_MAX_LEN];
    float match_score;
    int person_id; /* 底库 ID，-1 表示未匹配 */
    /* 识别特征向量后续接入时单独分配，避免每帧在栈上占用 512*4 字节 */
} face_item_t;

typedef struct {
    face_item_t faces[FACE_MAX_FACES];
    int count;
    int src_w;
    int src_h;
    uint32_t seq;
    int det_ms;
    int rec_ms;
} face_result_set_t;

typedef struct {
    const char *det_model_path;
    const char *rec_model_path;
    const char *gallery_path;
    float match_threshold; /* 余弦相似度阈值，默认 0.5 (50%) */
} face_algo_config_t;

typedef struct face_algo face_algo_t;

face_algo_t *face_algo_create(const face_algo_config_t *config);

int face_algo_start(face_algo_t *algo);
int face_algo_stop(face_algo_t *algo);

bool face_algo_is_running(const face_algo_t *algo);
bool face_algo_models_ready(const face_algo_t *algo);

int face_algo_submit_rgb888(face_algo_t *algo, const uint8_t *rgb, int w, int h);
int face_algo_get_result(face_algo_t *algo, face_result_set_t *out);

int face_algo_process_frame(face_algo_t *algo, const camera_frame_t *frame,
                            algo_result_cb_t cb, void *user_data);

void face_algo_destroy(face_algo_t *algo);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FACE_ALGO_H */
