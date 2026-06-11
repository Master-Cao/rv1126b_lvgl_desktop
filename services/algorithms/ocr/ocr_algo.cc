/*
 * OCR 算法封装：在独立 worker 线程上跑 PP-OCR det+rec 推理。
 * 接口对外为 C，实现使用 C++ + OpenCV，桥接 ppocr_system.cc。
 * 参考 ppocr/cpp/main.cc 中的 ocr_worker_thread 实现。
 */
#include "ocr_algo.h"

#include "common.h"
#include "ppocr_system.h"

#include <opencv2/opencv.hpp>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <stdio.h>

#define DEFAULT_DET_MODEL "/opt/rv1126b_desktop/models/ppocrv4_det.rknn"
#define DEFAULT_REC_MODEL "/opt/rv1126b_desktop/models/ppocrv4_rec.rknn"

/* PP-OCR 后处理参数（与 ppocr/cpp/main.cc 默认一致） */
#define DEFAULT_THRESHOLD       0.3f
#define DEFAULT_BOX_THRESHOLD   0.6f
#define DEFAULT_USE_DILATION    false
#define DEFAULT_DB_SCORE_MODE   "slow"
#define DEFAULT_DB_BOX_TYPE     "poly"
#define DEFAULT_DB_UNCLIP_RATIO 1.5f

struct ocr_algo {
    /* 配置 */
    char det_path[256];
    char rec_path[256];

    /* RKNN 模型上下文 */
    ppocr_system_app_context app_ctx;
    ppocr_det_postprocess_params params;
    bool models_loaded;

    /* worker 线程 */
    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    volatile bool running;
    volatile bool infer_busy;
    volatile bool frame_pending;

    /* pending 帧（采集线程 -> worker） */
    cv::Mat pending_frame;

    /* 最新结果（worker -> UI） */
    ocr_result_set_t latest;
    bool result_dirty;
    uint32_t result_seq;

    /* 旧 algo_result_t 兼容字段 */
    algo_state_t state;
};

static double get_time_ms()
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

static void copy_results(const ppocr_text_recog_array_result_t &src, ocr_result_set_t *dst,
                         int src_w, int src_h, int infer_ms, uint32_t seq)
{
    int count = src.count > OCR_MAX_LINES ? OCR_MAX_LINES : src.count;
    dst->count = count;
    dst->src_w = src_w;
    dst->src_h = src_h;
    dst->infer_ms = infer_ms;
    dst->seq = seq;

    for (int i = 0; i < count; i++) {
        const ppocr_text_recog_result_t &r = src.text_result[i];
        ocr_text_line_t &line = dst->lines[i];

        line.box.x1 = r.box.left_top.x;
        line.box.y1 = r.box.left_top.y;
        line.box.x2 = r.box.right_top.x;
        line.box.y2 = r.box.right_top.y;
        line.box.x3 = r.box.right_bottom.x;
        line.box.y3 = r.box.right_bottom.y;
        line.box.x4 = r.box.left_bottom.x;
        line.box.y4 = r.box.left_bottom.y;
        line.score = r.text.score;

        size_t n = strnlen(r.text.str, sizeof(r.text.str));
        if (n >= sizeof(line.text)) {
            n = sizeof(line.text) - 1;
        }
        memcpy(line.text, r.text.str, n);
        line.text[n] = '\0';
    }
}

static void *worker_thread_fn(void *arg)
{
    ocr_algo *algo = static_cast<ocr_algo *>(arg);

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

        ppocr_text_recog_array_result_t local;
        memset(&local, 0, sizeof(local));

        double t0 = get_time_ms();
        int ret = inference_ppocr_system_model(&algo->app_ctx, &src_image, &algo->params, &local);
        int elapsed = (int)(get_time_ms() - t0);

        pthread_mutex_lock(&algo->mutex);
        if (ret == 0) {
            uint32_t seq = ++algo->result_seq;
            copy_results(local, &algo->latest, frame.cols, frame.rows, elapsed, seq);
            algo->result_dirty = true;
        }
        algo->infer_busy = false;
        pthread_mutex_unlock(&algo->mutex);

        if (ret == 0 && local.count > 0) {
            printf("ocr_algo: %d texts, %d ms\n", local.count, elapsed);
        }
    }

    return NULL;
}

extern "C" ocr_algo_t *ocr_algo_create(const char *det_model, const char *rec_model)
{
    ocr_algo_t *algo = new (std::nothrow) ocr_algo;
    if (!algo) {
        return NULL;
    }
    memset(&algo->app_ctx, 0, sizeof(algo->app_ctx));
    memset(&algo->params, 0, sizeof(algo->params));
    memset(&algo->latest, 0, sizeof(algo->latest));
    algo->models_loaded = false;
    algo->running = false;
    algo->infer_busy = false;
    algo->frame_pending = false;
    algo->result_dirty = false;
    algo->result_seq = 0;
    algo->state = ALGO_STATE_IDLE;

    pthread_mutex_init(&algo->mutex, NULL);
    pthread_cond_init(&algo->cond, NULL);

    snprintf(algo->det_path, sizeof(algo->det_path), "%s",
             resolve_path(det_model, "OCR_DET_MODEL", DEFAULT_DET_MODEL));
    snprintf(algo->rec_path, sizeof(algo->rec_path), "%s",
             resolve_path(rec_model, "OCR_REC_MODEL", DEFAULT_REC_MODEL));

    /* 与 ppocr main.cc 一致的默认后处理参数 */
    algo->params.threshold = DEFAULT_THRESHOLD;
    algo->params.box_threshold = DEFAULT_BOX_THRESHOLD;
    algo->params.use_dilate = DEFAULT_USE_DILATION;
    algo->params.db_score_mode = (char *)DEFAULT_DB_SCORE_MODE;
    algo->params.db_box_type = (char *)DEFAULT_DB_BOX_TYPE;
    algo->params.db_unclip_ratio = DEFAULT_DB_UNCLIP_RATIO;

    return algo;
}

extern "C" int ocr_algo_start(ocr_algo_t *algo)
{
    if (!algo) {
        return -1;
    }
    if (algo->running) {
        return 0;
    }

    if (!algo->models_loaded) {
        if (init_ppocr_model(algo->det_path, &algo->app_ctx.det_context) != 0) {
            fprintf(stderr, "ocr_algo: init det model fail: %s\n", algo->det_path);
            algo->state = ALGO_STATE_ERROR;
            return -1;
        }
        if (init_ppocr_model(algo->rec_path, &algo->app_ctx.rec_context) != 0) {
            fprintf(stderr, "ocr_algo: init rec model fail: %s\n", algo->rec_path);
            release_ppocr_model(&algo->app_ctx.det_context);
            algo->state = ALGO_STATE_ERROR;
            return -1;
        }
        algo->models_loaded = true;
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

extern "C" int ocr_algo_stop(ocr_algo_t *algo)
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

    if (algo->models_loaded) {
        release_ppocr_model(&algo->app_ctx.det_context);
        release_ppocr_model(&algo->app_ctx.rec_context);
        algo->models_loaded = false;
    }

    algo->state = ALGO_STATE_IDLE;
    return 0;
}

extern "C" bool ocr_algo_is_running(const ocr_algo_t *algo)
{
    return algo && algo->running;
}

extern "C" int ocr_algo_submit_rgb888(ocr_algo_t *algo, const uint8_t *rgb, int w, int h)
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

extern "C" int ocr_algo_get_result(ocr_algo_t *algo, ocr_result_set_t *out)
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

extern "C" int ocr_algo_process_frame(ocr_algo_t *algo, const camera_frame_t *frame,
                                      algo_result_cb_t cb, void *user_data)
{
    if (!algo || !frame || !frame->data) {
        return -1;
    }
    if (frame->fmt != CAMERA_FMT_RGB888) {
        return -1;
    }

    int sret = ocr_algo_submit_rgb888(algo, (const uint8_t *)frame->data, frame->width,
                                      frame->height);
    if (sret < 0) {
        return -1;
    }

    if (cb) {
        ocr_result_set_t result;
        if (ocr_algo_get_result(algo, &result) == 0) {
            algo_result_t out;
            memset(&out, 0, sizeof(out));
            out.id = ALGO_ID_OCR;
            out.state = algo->state;
            out.hit_count = result.count;
            size_t off = 0;
            for (int i = 0; i < result.count && off < sizeof(out.text) - 2; i++) {
                int rem = (int)(sizeof(out.text) - 1 - off);
                int n = snprintf(out.text + off, rem, "%s\n", result.lines[i].text);
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

extern "C" void ocr_algo_destroy(ocr_algo_t *algo)
{
    if (!algo) {
        return;
    }
    ocr_algo_stop(algo);
    pthread_cond_destroy(&algo->cond);
    pthread_mutex_destroy(&algo->mutex);
    delete algo;
}
