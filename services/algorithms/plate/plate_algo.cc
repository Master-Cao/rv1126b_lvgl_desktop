/*
 * 车牌检测算法封装（YOLOv8 RKNN，暂仅检测不识别）。
 * 后处理对齐 detect/yolov8 与 yolov8 工程。
 */
#include "plate_algo.h"

#include "yolov8_plate.h"

#include <opencv2/opencv.hpp>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_DET_MODEL_PATH "/opt/rv1126b_desktop/models/plate_det.rknn"
#define ALT_DET_MODEL_PATH     "/opt/rv1126b_desktop/models/plate.rknn"

struct plate_algo {
    char det_model_path[256];

    rknn_app_context_t det_ctx;
    bool det_loaded;
    bool post_inited;

    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    volatile bool running;
    volatile bool infer_busy;
    volatile bool frame_pending;

    cv::Mat pending_frame;

    plate_result_set_t latest;
    bool result_dirty;
    uint32_t result_seq;

    algo_state_t state;
    uint32_t infer_log_seq;
};

static double now_ms()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static bool path_readable(const char *path)
{
    return path && path[0] && access(path, R_OK) == 0;
}

static const char *resolve_det_model_path(const char *prefer)
{
    if (prefer && prefer[0] && path_readable(prefer)) {
        return prefer;
    }
    const char *env = getenv("PLATE_DET_MODEL");
    if (env && env[0] && path_readable(env)) {
        return env;
    }
    if (path_readable(DEFAULT_DET_MODEL_PATH)) {
        return DEFAULT_DET_MODEL_PATH;
    }
    if (path_readable(ALT_DET_MODEL_PATH)) {
        return ALT_DET_MODEL_PATH;
    }
    /* 回退到默认名，便于 init 报错时打印期望路径 */
    return DEFAULT_DET_MODEL_PATH;
}

static void copy_results(const object_detect_result_list &src, plate_result_set_t *dst, int src_w,
                         int src_h, int det_ms, uint32_t seq)
{
    int count = src.count > PLATE_MAX_PLATES ? PLATE_MAX_PLATES : src.count;
    dst->count = count;
    dst->src_w = src_w;
    dst->src_h = src_h;
    dst->det_ms = det_ms;
    dst->rec_ms = 0;
    dst->seq = seq;

    for (int i = 0; i < count; i++) {
        const object_detect_result &r = src.results[i];
        plate_item_t &p = dst->plates[i];

        int left = r.box.left;
        int top = r.box.top;
        int right = r.box.right;
        int bottom = r.box.bottom;
        p.box.x1 = left;
        p.box.y1 = top;
        p.box.x2 = right;
        p.box.y2 = top;
        p.box.x3 = right;
        p.box.y3 = bottom;
        p.box.x4 = left;
        p.box.y4 = bottom;
        p.det_score = r.prop;
        p.rec_score = 0.0f;

        snprintf(p.text, sizeof(p.text), "plate");
    }
}

static int run_plate_pipeline(plate_algo *algo, const cv::Mat &frame, plate_result_set_t *out,
                              int *det_ms)
{
    memset(out, 0, sizeof(*out));
    out->src_w = frame.cols;
    out->src_h = frame.rows;
    out->count = 0;
    *det_ms = 0;

    if (!algo->det_loaded || frame.empty() || !frame.isContinuous() || !frame.data) {
        return 0;
    }

    image_buffer_t src_image;
    memset(&src_image, 0, sizeof(src_image));
    src_image.width = frame.cols;
    src_image.height = frame.rows;
    src_image.width_stride = (int)frame.step[0];
    src_image.virt_addr = frame.data;
    src_image.format = IMAGE_FORMAT_RGB888;
    src_image.size = (int)(frame.total() * frame.elemSize());

    object_detect_result_list local;
    memset(&local, 0, sizeof(local));

    double t0 = now_ms();
    int ret = plate_inference_yolov8_model(&algo->det_ctx, &src_image, &local);
    *det_ms = (int)(now_ms() - t0);
    if (ret < 0) {
        return -1;
    }

    copy_results(local, out, frame.cols, frame.rows, *det_ms, 0);
    return 0;
}

static bool try_load_models(plate_algo *algo)
{
    if (!algo) {
        return false;
    }

    if (!algo->post_inited) {
        if (plate_init_post_process() != 0) {
            fprintf(stderr, "plate_algo: plate_init_post_process fail\n");
            return false;
        }
        algo->post_inited = true;
    }

    if (!algo->det_loaded) {
        if (plate_init_yolov8_model(algo->det_model_path, &algo->det_ctx) != 0) {
            fprintf(stderr, "plate_algo: init det model fail: %s\n", algo->det_model_path);
            return false;
        }
        algo->det_loaded = true;
    }
    return true;
}

static void release_all_models(plate_algo *algo)
{
    if (!algo) {
        return;
    }
    if (algo->det_loaded) {
        plate_release_yolov8_model(&algo->det_ctx);
        algo->det_loaded = false;
    }
    if (algo->post_inited) {
        plate_deinit_post_process();
        algo->post_inited = false;
    }
}

static void *worker_thread_fn(void *arg)
{
    plate_algo *algo = static_cast<plate_algo *>(arg);

    while (true) {
        cv::Mat frame;

        pthread_mutex_lock(&algo->mutex);
        while (algo->running && !algo->frame_pending) {
            pthread_cond_wait(&algo->cond, &algo->mutex);
        }
        if (!algo->running) {
            pthread_mutex_unlock(&algo->mutex);
            break;
        }
        frame = algo->pending_frame;
        algo->pending_frame.release();
        algo->frame_pending = false;
        algo->infer_busy = true;
        pthread_mutex_unlock(&algo->mutex);

        plate_result_set_t local;
        memset(&local, 0, sizeof(local));
        int det_ms = 0;
        int ret = 0;

        if (algo->det_loaded) {
            ret = run_plate_pipeline(algo, frame, &local, &det_ms);
        } else {
            local.src_w = frame.cols;
            local.src_h = frame.rows;
            local.count = 0;
        }

        pthread_mutex_lock(&algo->mutex);
        if (ret == 0) {
            uint32_t seq = ++algo->result_seq;
            local.seq = seq;
            local.det_ms = det_ms;
            local.rec_ms = 0;
            algo->latest = local;
            algo->result_dirty = true;
        }
        algo->infer_busy = false;
        pthread_mutex_unlock(&algo->mutex);

        if (ret == 0 && local.count > 0) {
            printf("plate: %d boxes, %d ms\n", local.count, det_ms);
        } else if (ret == 0) {
            uint32_t n = ++algo->infer_log_seq;
            if (n <= 3 || (n % 30) == 0) {
                printf("plate: no box (frame %u, %dx%d, %d ms, class_num=%d)\n", n,
                       local.src_w, local.src_h, det_ms, algo->det_ctx.model_class_num);
            }
        }
    }

    return NULL;
}

extern "C" plate_algo_t *plate_algo_create(const plate_algo_config_t *config)
{
    plate_algo_t *algo = new (std::nothrow) plate_algo;
    if (!algo) {
        return NULL;
    }

    algo->det_model_path[0] = '\0';
    memset(&algo->det_ctx, 0, sizeof(algo->det_ctx));
    algo->det_loaded = false;
    algo->post_inited = false;
    algo->running = false;
    algo->infer_busy = false;
    algo->frame_pending = false;
    memset(&algo->latest, 0, sizeof(algo->latest));
    algo->result_dirty = false;
    algo->result_seq = 0;
    algo->infer_log_seq = 0;
    algo->state = ALGO_STATE_IDLE;

    const char *det = config ? config->det_model_path : NULL;
    snprintf(algo->det_model_path, sizeof(algo->det_model_path), "%s", resolve_det_model_path(det));

    pthread_mutex_init(&algo->mutex, NULL);
    pthread_cond_init(&algo->cond, NULL);
    printf("plate_algo: det model path %s\n", algo->det_model_path);
    return algo;
}

extern "C" int plate_algo_start(plate_algo_t *algo)
{
    if (!algo) {
        return -1;
    }
    if (algo->running) {
        return 0;
    }

    if (!try_load_models(algo)) {
        fprintf(stderr, "plate_algo: model init fail: %s\n", algo->det_model_path);
        release_all_models(algo);
        algo->state = ALGO_STATE_ERROR;
        return -1;
    }

    algo->running = true;
    algo->infer_busy = false;
    algo->frame_pending = false;
    algo->state = ALGO_STATE_RUNNING;

    if (pthread_create(&algo->worker, NULL, worker_thread_fn, algo) != 0) {
        algo->running = false;
        release_all_models(algo);
        algo->state = ALGO_STATE_ERROR;
        return -1;
    }
    return 0;
}

extern "C" int plate_algo_stop(plate_algo_t *algo)
{
    if (!algo) {
        return -1;
    }
    if (!algo->running) {
        return 0;
    }

    pthread_mutex_lock(&algo->mutex);
    algo->running = false;
    pthread_cond_broadcast(&algo->cond);
    pthread_mutex_unlock(&algo->mutex);

    pthread_join(algo->worker, NULL);

    pthread_mutex_lock(&algo->mutex);
    algo->pending_frame.release();
    algo->frame_pending = false;
    algo->infer_busy = false;
    pthread_mutex_unlock(&algo->mutex);

    release_all_models(algo);
    algo->state = ALGO_STATE_IDLE;
    return 0;
}

extern "C" bool plate_algo_is_running(const plate_algo_t *algo)
{
    return algo && algo->running;
}

extern "C" bool plate_algo_models_ready(const plate_algo_t *algo)
{
    return algo && algo->det_loaded;
}

extern "C" const char *plate_algo_get_det_model_path(const plate_algo_t *algo)
{
    return (algo && algo->det_model_path[0]) ? algo->det_model_path : NULL;
}

extern "C" int plate_algo_submit_rgb888(plate_algo_t *algo, const uint8_t *rgb, int w, int h)
{
    if (!algo || !rgb || w <= 0 || h <= 0) {
        return -1;
    }
    if (!algo->running) {
        return -1;
    }

    pthread_mutex_lock(&algo->mutex);
    if (algo->infer_busy || algo->frame_pending) {
        pthread_mutex_unlock(&algo->mutex);
        return 1;
    }

    cv::Mat tmp(h, w, CV_8UC3);
    memcpy(tmp.data, rgb, (size_t)w * (size_t)h * 3);
    algo->pending_frame = tmp;
    algo->frame_pending = true;
    pthread_cond_signal(&algo->cond);
    pthread_mutex_unlock(&algo->mutex);
    return 0;
}

extern "C" int plate_algo_get_result(plate_algo_t *algo, plate_result_set_t *out)
{
    if (!algo || !out) {
        return -1;
    }

    pthread_mutex_lock(&algo->mutex);
    if (!algo->result_dirty) {
        pthread_mutex_unlock(&algo->mutex);
        return 1;
    }
    *out = algo->latest;
    algo->result_dirty = false;
    pthread_mutex_unlock(&algo->mutex);
    return 0;
}

extern "C" int plate_algo_process_frame(plate_algo_t *algo, const camera_frame_t *frame,
                                      algo_result_cb_t cb, void *user_data)
{
    if (!algo || !frame || !frame->data) {
        return -1;
    }
    if (frame->fmt != CAMERA_FMT_RGB888) {
        return -1;
    }

    int sret = plate_algo_submit_rgb888(algo, (const uint8_t *)frame->data, frame->width,
                                        frame->height);
    if (sret < 0) {
        return -1;
    }

    if (cb) {
        plate_result_set_t result;
        if (plate_algo_get_result(algo, &result) == 0) {
            algo_result_t out;
            memset(&out, 0, sizeof(out));
            out.id = ALGO_ID_PLATE;
            out.state = algo->state;
            out.hit_count = result.count;
            size_t off = 0;
            for (int i = 0; i < result.count && off < sizeof(out.text) - 2; i++) {
                const plate_item_t *p = &result.plates[i];
                int rem = (int)(sizeof(out.text) - 1 - off);
                const char *label = (p->text[0] != '\0') ? p->text : "plate";
                int n = snprintf(out.text + off, (size_t)rem, "%s %.0f%%\n", label,
                                 p->det_score * 100.0f);
                if (n <= 0) {
                    break;
                }
                off += (size_t)n;
            }
            if (result.count == 0 && off == 0) {
                snprintf(out.text, sizeof(out.text), "no plate");
            }
            cb(&out, user_data);
        }
    }
    return 0;
}

extern "C" void plate_algo_destroy(plate_algo_t *algo)
{
    if (!algo) {
        return;
    }
    plate_algo_stop(algo);
    pthread_cond_destroy(&algo->cond);
    pthread_mutex_destroy(&algo->mutex);
    delete algo;
}
