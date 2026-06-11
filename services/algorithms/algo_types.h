#ifndef ALGO_TYPES_H
#define ALGO_TYPES_H

#include <stdint.h>

typedef enum {
    ALGO_ID_OCR = 0,
    ALGO_ID_DETECT,
    ALGO_ID_FACE,
    ALGO_ID_PLATE,
    ALGO_ID_SEG,
} algo_id_t;

typedef enum {
    ALGO_STATE_IDLE = 0,
    ALGO_STATE_RUNNING,
    ALGO_STATE_ERROR,
} algo_state_t;

#define ALGO_TEXT_MAX 512

typedef struct {
    algo_id_t id;
    algo_state_t state;
    char text[ALGO_TEXT_MAX]; /* OCR 文本 / 检测摘要 */
    int hit_count;              /* 检测框数等 */
} algo_result_t;

typedef void (*algo_result_cb_t)(const algo_result_t *result, void *user_data);

#endif
