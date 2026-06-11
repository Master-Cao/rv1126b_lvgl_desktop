#ifndef ALGO_RUNNER_H
#define ALGO_RUNNER_H

#include "algo_types.h"
#include "../camera/camera_types.h"

typedef struct algo_runner algo_runner_t;

algo_runner_t *algo_runner_create(algo_id_t id);

int algo_runner_start(algo_runner_t *runner);

int algo_runner_stop(algo_runner_t *runner);

int algo_runner_process_frame(algo_runner_t *runner, const camera_frame_t *frame,
                              algo_result_cb_t cb, void *user_data);

void algo_runner_destroy(algo_runner_t *runner);

algo_id_t algo_runner_get_id(const algo_runner_t *runner);

#endif
