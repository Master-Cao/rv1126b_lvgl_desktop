#ifndef PLATE_ALGO_H
#define PLATE_ALGO_H

#include "../algo_types.h"
#include "../../camera/camera_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLATE_TEXT_MAX_LEN  16
#define PLATE_MAX_PLATES    8

/* 与 camera_preview_box_t 同布局，便于 UI 绘制检测框。 */
typedef struct {
    int x1, y1;
    int x2, y2;
    int x3, y3;
    int x4, y4;
} plate_box_t;

typedef struct {
    plate_box_t box;
    char text[PLATE_TEXT_MAX_LEN];
    float det_score;
    float rec_score;
} plate_item_t;

typedef struct {
    plate_item_t plates[PLATE_MAX_PLATES];
    int count;
    int src_w;
    int src_h;
    uint32_t seq;
    int det_ms;
    int rec_ms;
} plate_result_set_t;

typedef struct {
    const char *det_model_path;
    const char *rec_model_path;
} plate_algo_config_t;

typedef struct plate_algo plate_algo_t;

plate_algo_t *plate_algo_create(const plate_algo_config_t *config);

int plate_algo_start(plate_algo_t *algo);
int plate_algo_stop(plate_algo_t *algo);

bool plate_algo_is_running(const plate_algo_t *algo);
bool plate_algo_models_ready(const plate_algo_t *algo);
const char *plate_algo_get_det_model_path(const plate_algo_t *algo);

int plate_algo_submit_rgb888(plate_algo_t *algo, const uint8_t *rgb, int w, int h);
int plate_algo_get_result(plate_algo_t *algo, plate_result_set_t *out);

int plate_algo_process_frame(plate_algo_t *algo, const camera_frame_t *frame,
                             algo_result_cb_t cb, void *user_data);

void plate_algo_destroy(plate_algo_t *algo);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PLATE_ALGO_H */
