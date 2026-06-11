/*
 * 目标检测算法封装：在独立 worker 线程上跑 YOLOv8 RKNN 推理。
 * 接口对外为 C，实现使用 C++ + OpenCV，桥接 yolov8.cc / postprocess.cc。
 * 整体结构对齐 ocr_algo.cc：submit_rgb888 + get_result 异步流水线。
 */
#include "detect_algo.h"

#include "yolov8.h"

#include <opencv2/opencv.hpp>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define DEFAULT_MODEL_PATH "/opt/rv1126b_desktop/models/yolov8.rknn"

struct detect_algo {
    char model_path[256];

    rknn_app_context_t app_ctx;
    bool model_loaded;
    bool post_inited;

    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    volatile bool running;
    volatile bool infer_busy;
    volatile bool frame_pending;

    cv::Mat pending_frame;

    detect_result_set_t latest;
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

static void copy_results(const object_detect_result_list &src, detect_result_set_t *dst,
                         int src_w, int src_h, int infer_ms, uint32_t seq)
{
    int count = src.count > DETECT_MAX_RESULTS ? DETECT_MAX_RESULTS : src.count;
    dst->count = count;
    dst->src_w = src_w;
    dst->src_h = src_h;
    dst->infer_ms = infer_ms;
    dst->seq = seq;

    for (int i = 0; i < count; i++) {
        const object_detect_result &r = src.results[i];
        detect_object_t &o = dst->objects[i];

        /* YOLOv8 输出的是轴对齐矩形；展开成 4 点多边形以复用 camera_preview 的多边形渲染。 */
        int left = r.box.left;
        int top = r.box.top;
        int right = r.box.right;
        int bottom = r.box.bottom;
        o.box.x1 = left;  o.box.y1 = top;
        o.box.x2 = right; o.box.y2 = top;
        o.box.x3 = right; o.box.y3 = bottom;
        o.box.x4 = left;  o.box.y4 = bottom;
        o.cls_id = r.cls_id;
        o.score = r.prop;

        const char *name = coco_cls_to_name(r.cls_id);
        if (!name) {
            name = "obj";
        }
        size_t n = strnlen(name, sizeof(o.name) - 1);
        memcpy(o.name, name, n);
        o.name[n] = '\0';
    }
}

static void *worker_thread_fn(void *arg)
{
    detect_algo *algo = static_cast<detect_algo *>(arg);

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
        int ret = inference_yolov8_model(&algo->app_ctx, &src_image, &local);
        int elapsed = (int)(now_ms() - t0);

        pthread_mutex_lock(&algo->mutex);
        if (ret == 0) {
            uint32_t seq = ++algo->result_seq;
            copy_results(local, &algo->latest, frame.cols, frame.rows, elapsed, seq);
            algo->result_dirty = true;
        }
        algo->infer_busy = false;
        pthread_mutex_unlock(&algo->mutex);

        if (ret == 0 && local.count > 0) {
            printf("detect_algo: %d objects, %d ms\n", local.count, elapsed);
        }
    }

    return NULL;
}

extern "C" detect_algo_t *detect_algo_create(const char *model_path)
{
    detect_algo_t *algo = new (std::nothrow) detect_algo;
    if (!algo) {
        return NULL;
    }
    memset(&algo->app_ctx, 0, sizeof(algo->app_ctx));
    memset(&algo->latest, 0, sizeof(algo->latest));
    algo->model_loaded = false;
    algo->post_inited = false;
    algo->running = false;
    algo->infer_busy = false;
    algo->frame_pending = false;
    algo->result_dirty = false;
    algo->result_seq = 0;
    algo->state = ALGO_STATE_IDLE;

    pthread_mutex_init(&algo->mutex, NULL);
    pthread_cond_init(&algo->cond, NULL);

    snprintf(algo->model_path, sizeof(algo->model_path), "%s",
             resolve_path(model_path, "DETECT_MODEL", DEFAULT_MODEL_PATH));
    return algo;
}

extern "C" int detect_algo_start(detect_algo_t *algo)
{
    if (!algo) {
        return -1;
    }
    if (algo->running) {
        return 0;
    }

    if (!algo->post_inited) {
        if (init_post_process() != 0) {
            fprintf(stderr, "detect_algo: init_post_process fail\n");
            algo->state = ALGO_STATE_ERROR;
            return -1;
        }
        algo->post_inited = true;
    }

    if (!algo->model_loaded) {
        if (init_yolov8_model(algo->model_path, &algo->app_ctx) != 0) {
            fprintf(stderr, "detect_algo: init yolov8 model fail: %s\n", algo->model_path);
            algo->state = ALGO_STATE_ERROR;
            return -1;
        }
        algo->model_loaded = true;
    }

    algo->running = true;
    algo->infer_busy = false;
    algo->frame_pending = false;
    algo->state = ALGO_STATE_RUNNING;

    if (pthread_create(&algo->worker, NULL, worker_thread_fn, algo) != 0) {
        algo->running = false;
        algo->state = ALGO_STATE_ERROR;
        return -1;
    }
    return 0;
}

extern "C" int detect_algo_stop(detect_algo_t *algo)
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

    if (algo->model_loaded) {
        release_yolov8_model(&algo->app_ctx);
        algo->model_loaded = false;
    }

    algo->state = ALGO_STATE_IDLE;
    return 0;
}

extern "C" bool detect_algo_is_running(const detect_algo_t *algo)
{
    return algo && algo->running;
}

extern "C" int detect_algo_submit_rgb888(detect_algo_t *algo, const uint8_t *rgb, int w, int h)
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
        return 1; /* drop */
    }

    cv::Mat tmp(h, w, CV_8UC3);
    memcpy(tmp.data, rgb, (size_t)w * (size_t)h * 3);
    algo->pending_frame = tmp;
    algo->frame_pending = true;
    pthread_cond_signal(&algo->cond);
    pthread_mutex_unlock(&algo->mutex);
    return 0;
}

extern "C" int detect_algo_get_result(detect_algo_t *algo, detect_result_set_t *out)
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

extern "C" int detect_algo_process_frame(detect_algo_t *algo, const camera_frame_t *frame,
                                         algo_result_cb_t cb, void *user_data)
{
    if (!algo || !frame || !frame->data) {
        return -1;
    }
    if (frame->fmt != CAMERA_FMT_RGB888) {
        return -1;
    }

    int sret = detect_algo_submit_rgb888(algo, (const uint8_t *)frame->data, frame->width,
                                         frame->height);
    if (sret < 0) {
        return -1;
    }

    if (cb) {
        detect_result_set_t result;
        if (detect_algo_get_result(algo, &result) == 0) {
            algo_result_t out;
            memset(&out, 0, sizeof(out));
            out.id = ALGO_ID_DETECT;
            out.state = algo->state;
            out.hit_count = result.count;
            size_t off = 0;
            for (int i = 0; i < result.count && off < sizeof(out.text) - 2; i++) {
                int rem = (int)(sizeof(out.text) - 1 - off);
                int n = snprintf(out.text + off, rem, "%s %.0f%%\n", result.objects[i].name,
                                 result.objects[i].score * 100.0f);
                if (n <= 0) {
                    break;
                }
                off += (size_t)n;
            }
            cb(&out, user_data);
        }
    }
    return 0;
}

extern "C" void detect_algo_destroy(detect_algo_t *algo)
{
    if (!algo) {
        return;
    }
    detect_algo_stop(algo);
    if (algo->post_inited) {
        deinit_post_process();
        algo->post_inited = false;
    }
    pthread_cond_destroy(&algo->cond);
    pthread_mutex_destroy(&algo->mutex);
    delete algo;
}
