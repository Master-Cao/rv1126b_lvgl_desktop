/*
 * 目标实例分割算法封装（YOLOv8-Seg RKNN）。
 * 移植自 atk_yolov8_seg_cam，结构对齐 detect_algo。
 */
#include "seg_algo.h"

#include "yolov8_seg.h"

#include <opencv2/opencv.hpp>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define DEFAULT_MODEL_PATH "/opt/rv1126b_desktop/models/yolov8_seg.rknn"

struct seg_algo {
    char model_path[256];

    rknn_app_context_t app_ctx;
    bool model_loaded;
    bool post_inited;

    uint8_t *owned_mask;

    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    volatile bool running;
    volatile bool infer_busy;
    volatile bool frame_pending;

    cv::Mat pending_frame;

    seg_result_set_t latest;
    bool result_dirty;
    uint32_t result_seq;

    algo_state_t state;
};

static double now_ms()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static const char *resolve_path(const char *prefer, const char *env_key, const char *fallback)
{
    if (prefer && prefer[0]) {
        return prefer;
    }
    const char *env = getenv(env_key);
    if (env && env[0]) {
        return env;
    }
    return fallback;
}

static void free_owned_mask(seg_algo *algo)
{
    if (!algo) {
        return;
    }
    free(algo->owned_mask);
    algo->owned_mask = NULL;
}

static void copy_results(const object_detect_result_list &src, seg_result_set_t *dst, int src_w,
                         int src_h, int infer_ms, uint32_t seq, uint8_t **out_owned_mask)
{
    int count = src.count > SEG_MAX_INSTANCES ? SEG_MAX_INSTANCES : src.count;
    dst->count = count;
    dst->src_w = src_w;
    dst->src_h = src_h;
    dst->infer_ms = infer_ms;
    dst->seq = seq;
    dst->has_mask = false;
    dst->seg_mask = NULL;

    for (int i = 0; i < count; i++) {
        const object_detect_result &r = src.results[i];
        seg_item_t &o = dst->instances[i];

        int left = r.box.left;
        int top = r.box.top;
        int right = r.box.right;
        int bottom = r.box.bottom;
        o.box.x1 = left;
        o.box.y1 = top;
        o.box.x2 = right;
        o.box.y2 = top;
        o.box.x3 = right;
        o.box.y3 = bottom;
        o.box.x4 = left;
        o.box.y4 = bottom;
        o.class_id = r.cls_id;
        o.score = r.prop;

        const char *name = seg_coco_cls_to_name(r.cls_id);
        if (!name) {
            name = "obj";
        }
        size_t n = strnlen(name, sizeof(o.class_name) - 1);
        memcpy(o.class_name, name, n);
        o.class_name[n] = '\0';
    }

    if (src.results_seg[0].seg_mask && src_w > 0 && src_h > 0) {
        size_t bytes = (size_t)src_w * (size_t)src_h;
        uint8_t *mask = (uint8_t *)malloc(bytes);
        if (mask) {
            memcpy(mask, src.results_seg[0].seg_mask, bytes);
            dst->seg_mask = mask;
            dst->has_mask = true;
            *out_owned_mask = mask;
        }
    }
}

static int run_seg_pipeline(seg_algo *algo, const cv::Mat &frame, seg_result_set_t *out, int *infer_ms)
{
    memset(out, 0, sizeof(*out));
    out->src_w = frame.cols;
    out->src_h = frame.rows;
    out->count = 0;
    *infer_ms = 0;

    if (!algo->model_loaded || frame.empty() || !frame.isContinuous() || !frame.data) {
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
    int ret = inference_yolov8_seg_model(&algo->app_ctx, &src_image, &local);
    *infer_ms = (int)(now_ms() - t0);
    if (ret < 0) {
        if (local.results_seg[0].seg_mask) {
            free(local.results_seg[0].seg_mask);
        }
        return -1;
    }

    uint8_t *new_mask = NULL;
    copy_results(local, out, frame.cols, frame.rows, *infer_ms, 0, &new_mask);
    if (local.results_seg[0].seg_mask) {
        free(local.results_seg[0].seg_mask);
        local.results_seg[0].seg_mask = NULL;
    }
    free_owned_mask(algo);
    algo->owned_mask = new_mask;
    out->seg_mask = algo->owned_mask;
    return 0;
}

static bool try_load_models(seg_algo *algo)
{
    if (!algo) {
        return false;
    }

    if (!algo->post_inited) {
        if (seg_init_post_process() != 0) {
            fprintf(stderr, "seg_algo: seg_init_post_process fail\n");
            return false;
        }
        algo->post_inited = true;
    }

    if (!algo->model_loaded) {
        if (init_yolov8_seg_model(algo->model_path, &algo->app_ctx) != 0) {
            fprintf(stderr, "seg_algo: init model fail: %s\n", algo->model_path);
            return false;
        }
        algo->model_loaded = true;
    }
    return true;
}

static void release_all_models(seg_algo *algo)
{
    if (!algo) {
        return;
    }
    if (algo->model_loaded) {
        release_yolov8_seg_model(&algo->app_ctx);
        algo->model_loaded = false;
    }
    if (algo->post_inited) {
        seg_deinit_post_process();
        algo->post_inited = false;
    }
    free_owned_mask(algo);
    algo->latest.seg_mask = NULL;
    algo->latest.has_mask = false;
}

static void *worker_thread_fn(void *arg)
{
    seg_algo *algo = static_cast<seg_algo *>(arg);

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
        frame = algo->pending_frame.clone();
        algo->pending_frame.release();
        algo->frame_pending = false;
        algo->infer_busy = true;
        pthread_mutex_unlock(&algo->mutex);

        if (frame.empty() || !frame.isContinuous()) {
            pthread_mutex_lock(&algo->mutex);
            algo->infer_busy = false;
            pthread_mutex_unlock(&algo->mutex);
            continue;
        }

        seg_result_set_t local;
        memset(&local, 0, sizeof(local));
        int infer_ms = 0;
        int ret = run_seg_pipeline(algo, frame, &local, &infer_ms);

        pthread_mutex_lock(&algo->mutex);
        if (ret == 0) {
            uint32_t seq = ++algo->result_seq;
            local.seq = seq;
            local.infer_ms = infer_ms;
            local.seg_mask = algo->owned_mask;
            local.has_mask = (algo->owned_mask != NULL);
            algo->latest = local;
            algo->result_dirty = true;
        }
        algo->infer_busy = false;
        pthread_mutex_unlock(&algo->mutex);

        if (ret == 0 && local.count > 0) {
            printf("seg: %d instances, %d ms\n", local.count, infer_ms);
        }
    }

    return NULL;
}

extern "C" seg_algo_t *seg_algo_create(const seg_algo_config_t *config)
{
    seg_algo_t *algo = new (std::nothrow) seg_algo;
    if (!algo) {
        return NULL;
    }

    algo->model_path[0] = '\0';
    memset(&algo->app_ctx, 0, sizeof(algo->app_ctx));
    algo->model_loaded = false;
    algo->post_inited = false;
    algo->owned_mask = NULL;
    algo->running = false;
    algo->infer_busy = false;
    algo->frame_pending = false;
    memset(&algo->latest, 0, sizeof(algo->latest));
    algo->result_dirty = false;
    algo->result_seq = 0;
    algo->state = ALGO_STATE_IDLE;

    const char *model = config ? config->model_path : NULL;
    snprintf(algo->model_path, sizeof(algo->model_path), "%s",
             resolve_path(model, "SEG_MODEL", DEFAULT_MODEL_PATH));

    pthread_mutex_init(&algo->mutex, NULL);
    pthread_cond_init(&algo->cond, NULL);
    printf("seg_algo: model path %s\n", algo->model_path);
    return algo;
}

extern "C" int seg_algo_start(seg_algo_t *algo)
{
    if (!algo) {
        return -1;
    }
    if (algo->running) {
        return 0;
    }

    if (!try_load_models(algo)) {
        fprintf(stderr, "seg_algo: model init fail: %s\n", algo->model_path);
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

extern "C" int seg_algo_stop(seg_algo_t *algo)
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

extern "C" bool seg_algo_is_running(const seg_algo_t *algo)
{
    return algo && algo->running;
}

extern "C" bool seg_algo_models_ready(const seg_algo_t *algo)
{
    return algo && algo->model_loaded;
}

extern "C" const char *seg_algo_get_model_path(const seg_algo_t *algo)
{
    return (algo && algo->model_path[0]) ? algo->model_path : NULL;
}

extern "C" int seg_algo_submit_rgb888(seg_algo_t *algo, const uint8_t *rgb, int w, int h)
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

extern "C" int seg_algo_get_result(seg_algo_t *algo, seg_result_set_t *out)
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

extern "C" int seg_algo_process_frame(seg_algo_t *algo, const camera_frame_t *frame,
                                      algo_result_cb_t cb, void *user_data)
{
    if (!algo || !frame || !frame->data) {
        return -1;
    }
    if (frame->fmt != CAMERA_FMT_RGB888) {
        return -1;
    }

    int sret = seg_algo_submit_rgb888(algo, (const uint8_t *)frame->data, frame->width,
                                      frame->height);
    if (sret < 0) {
        return -1;
    }

    if (cb) {
        seg_result_set_t result;
        if (seg_algo_get_result(algo, &result) == 0) {
            algo_result_t out;
            memset(&out, 0, sizeof(out));
            out.id = ALGO_ID_SEG;
            out.state = algo->state;
            out.hit_count = result.count;
            size_t off = 0;
            for (int i = 0; i < result.count && off < sizeof(out.text) - 2; i++) {
                const seg_item_t *s = &result.instances[i];
                int rem = (int)(sizeof(out.text) - 1 - off);
                const char *label = (s->class_name[0] != '\0') ? s->class_name : "object";
                int n = snprintf(out.text + off, (size_t)rem, "%s %.0f%%\n", label,
                                 s->score * 100.0f);
                if (n <= 0) {
                    break;
                }
                off += (size_t)n;
            }
            if (result.count == 0 && off == 0) {
                snprintf(out.text, sizeof(out.text), "no instance");
            }
            cb(&out, user_data);
        }
    }
    return 0;
}

extern "C" void seg_algo_destroy(seg_algo_t *algo)
{
    if (!algo) {
        return;
    }
    seg_algo_stop(algo);
    pthread_cond_destroy(&algo->cond);
    pthread_mutex_destroy(&algo->mutex);
    delete algo;
}
