#include "algo_runner.h"

#include "../camera/camera_types.h"
#include "detect/detect_algo.h"
#include "face/face_algo.h"
#include "ocr/ocr_algo.h"
#include "plate/plate_algo.h"
#include "seg/seg_algo.h"

#include <stdlib.h>

struct algo_runner {
    algo_id_t id;
    void *impl;
};

algo_runner_t *algo_runner_create(algo_id_t id) {
    algo_runner_t *runner = (algo_runner_t *)calloc(1, sizeof(algo_runner_t));
    if (!runner) {
        return NULL;
    }
    runner->id = id;
    switch (id) {
    case ALGO_ID_OCR:
        /* 模型路径由 ocr_algo 内部按 OCR_DET_MODEL / OCR_REC_MODEL 环境变量回退到
         * /opt/rv1126b_desktop/models/ppocrv4_*.rknn */
        runner->impl = ocr_algo_create(NULL, NULL);
        break;
    case ALGO_ID_DETECT:
        runner->impl = detect_algo_create(NULL);
        break;
    case ALGO_ID_FACE:
        runner->impl = face_algo_create(NULL);
        break;
    case ALGO_ID_PLATE:
        runner->impl = plate_algo_create(NULL);
        break;
    case ALGO_ID_SEG:
        runner->impl = seg_algo_create(NULL);
        break;
    default:
        free(runner);
        return NULL;
    }
    if (!runner->impl) {
        free(runner);
        return NULL;
    }
    return runner;
}

int algo_runner_start(algo_runner_t *runner) {
    if (!runner || !runner->impl) {
        return -1;
    }
    switch (runner->id) {
    case ALGO_ID_OCR:
        return ocr_algo_start((ocr_algo_t *)runner->impl);
    case ALGO_ID_DETECT:
        return detect_algo_start((detect_algo_t *)runner->impl);
    case ALGO_ID_FACE:
        return face_algo_start((face_algo_t *)runner->impl);
    case ALGO_ID_PLATE:
        return plate_algo_start((plate_algo_t *)runner->impl);
    case ALGO_ID_SEG:
        return seg_algo_start((seg_algo_t *)runner->impl);
    default:
        return -1;
    }
}

int algo_runner_stop(algo_runner_t *runner) {
    if (!runner || !runner->impl) {
        return -1;
    }
    switch (runner->id) {
    case ALGO_ID_OCR:
        return ocr_algo_stop((ocr_algo_t *)runner->impl);
    case ALGO_ID_DETECT:
        return detect_algo_stop((detect_algo_t *)runner->impl);
    case ALGO_ID_FACE:
        return face_algo_stop((face_algo_t *)runner->impl);
    case ALGO_ID_PLATE:
        return plate_algo_stop((plate_algo_t *)runner->impl);
    case ALGO_ID_SEG:
        return seg_algo_stop((seg_algo_t *)runner->impl);
    default:
        return -1;
    }
}

int algo_runner_process_frame(algo_runner_t *runner, const camera_frame_t *frame,
                              algo_result_cb_t cb, void *user_data) {
    if (!runner || !runner->impl) {
        return -1;
    }
    switch (runner->id) {
    case ALGO_ID_OCR:
        return ocr_algo_process_frame((ocr_algo_t *)runner->impl, frame, cb, user_data);
    case ALGO_ID_DETECT:
        return detect_algo_process_frame((detect_algo_t *)runner->impl, frame, cb, user_data);
    case ALGO_ID_FACE:
        return face_algo_process_frame((face_algo_t *)runner->impl, frame, cb, user_data);
    case ALGO_ID_PLATE:
        return plate_algo_process_frame((plate_algo_t *)runner->impl, frame, cb, user_data);
    case ALGO_ID_SEG:
        return seg_algo_process_frame((seg_algo_t *)runner->impl, frame, cb, user_data);
    default:
        return -1;
    }
}

void algo_runner_destroy(algo_runner_t *runner) {
    if (!runner) {
        return;
    }
    if (runner->impl) {
        switch (runner->id) {
        case ALGO_ID_OCR:
            ocr_algo_destroy((ocr_algo_t *)runner->impl);
            break;
        case ALGO_ID_DETECT:
            detect_algo_destroy((detect_algo_t *)runner->impl);
            break;
        case ALGO_ID_FACE:
            face_algo_destroy((face_algo_t *)runner->impl);
            break;
        case ALGO_ID_PLATE:
            plate_algo_destroy((plate_algo_t *)runner->impl);
            break;
        case ALGO_ID_SEG:
            seg_algo_destroy((seg_algo_t *)runner->impl);
            break;
        default:
            break;
        }
    }
    free(runner);
}

algo_id_t algo_runner_get_id(const algo_runner_t *runner) {
    return runner ? runner->id : ALGO_ID_OCR;
}
