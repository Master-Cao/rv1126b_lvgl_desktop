/*
 * 人脸检测 + 识别算法封装。
 * 对外 C 接口，内部 C++ + OpenCV worker 线程，结构对齐 detect_algo / hair_algo。
 *
 * 管线：
 *   1. SCRFD (face_det.rknn) → bbox + 5 关键点
 *   2. 仿射对齐 → 112×112
 *   3. MobileFaceNet (face_rec.rknn) → 512 维特征
 *   4. 底库 gallery 余弦 1:N → name / person_id / match_score
 */
#include "face_algo.h"

#include "face_align.h"
#include "face_gallery.h"
#include "face_rec.h"
#include "scrfd.h"

#include <opencv2/opencv.hpp>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_DET_MODEL_PATH    "/opt/rv1126b_desktop/models/face_det.rknn"
#define DEFAULT_REC_MODEL_PATH    "/opt/rv1126b_desktop/models/face_rec.rknn"
#define DEFAULT_GALLERY_PATH      "/opt/rv1126b_desktop/models/face_gallery"
#define DEFAULT_MATCH_THRESHOLD   0.5f

struct face_algo {
    char det_model_path[256];
    char rec_model_path[256];
    char gallery_path[256];
    float match_threshold;

    bool det_loaded;
    bool rec_loaded;
    bool gallery_loaded;

    scrfd_context_t det_ctx;
    face_rec_context_t rec_ctx;
    face_gallery_t gallery;

    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    volatile bool running;
    volatile bool infer_busy;
    volatile bool frame_pending;

    cv::Mat pending_frame;

    face_result_set_t *latest;
    face_result_set_t *scratch;
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

static void log_path_access(const char *tag, const char *path)
{
    if (!path || !path[0]) {
        printf("face_algo: %s path empty\n", tag);
        return;
    }
    if (access(path, R_OK) != 0) {
        printf("face_algo: %s missing or unreadable: %s\n", tag, path);
        return;
    }
    if (strcmp(tag, "gallery_dir") == 0) {
        char bin_path[512];
        char txt_path[512];
        snprintf(bin_path, sizeof(bin_path), "%s/face_encoding.bin", path);
        snprintf(txt_path, sizeof(txt_path), "%s/gallery.txt", path);
        struct stat st_bin;
        struct stat st_txt;
        int bin_ok = (stat(bin_path, &st_bin) == 0);
        int txt_ok = (stat(txt_path, &st_txt) == 0);
        printf("face_algo: gallery_dir ok: %s\n", path);
        printf("face_algo:   face_encoding.bin %s (%s)\n", bin_ok ? "ok" : "MISSING", bin_path);
        printf("face_algo:   gallery.txt       %s (%s)\n", txt_ok ? "ok" : "MISSING", txt_path);
        return;
    }
    struct stat st;
    if (stat(path, &st) == 0) {
        printf("face_algo: %s ok: %s (%lld bytes)\n", tag, path, (long long)st.st_size);
    } else {
        printf("face_algo: %s stat fail: %s\n", tag, path);
    }
}

static int run_face_pipeline(face_algo *algo, const cv::Mat &frame, face_result_set_t *out,
                             int *det_ms, int *rec_ms)
{
    memset(out, 0, sizeof(*out));
    out->src_w = frame.cols;
    out->src_h = frame.rows;
    out->count = 0;
    *det_ms = 0;
    *rec_ms = 0;

    if (!algo->det_loaded || frame.empty() || !frame.isContinuous() || !frame.data) {
        return 0;
    }

    image_buffer_t src_image;
    memset(&src_image, 0, sizeof(src_image));
    src_image.width = frame.cols;
    src_image.height = frame.rows;
    src_image.width_stride = (int)frame.step[0];
    src_image.height_stride = frame.rows;
    src_image.format = IMAGE_FORMAT_RGB888;
    src_image.virt_addr = frame.data;
    src_image.size = frame.cols * frame.rows * 3;

    double t0 = now_ms();
    int ret = inference_scrfd_model(&algo->det_ctx, &src_image, algo->det_ctx.infer_result);
    *det_ms = (int)(now_ms() - t0);
    if (ret < 0) {
        return -1;
    }

    const scrfd_result *det = algo->det_ctx.infer_result;
    if (!det) {
        return -1;
    }

    int count = det->count > FACE_MAX_FACES ? FACE_MAX_FACES : det->count;
    out->count = count;

    double rec_t0 = now_ms();
    for (int i = 0; i < count; i++) {
        const scrfd_object_t &o = det->object[i];
        face_item_t &f = out->faces[i];

        int left = o.left;
        int top = o.top;
        int right = o.right;
        int bottom = o.bottom;
        f.box.x1 = left;  f.box.y1 = top;
        f.box.x2 = right; f.box.y2 = top;
        f.box.x3 = right; f.box.y3 = bottom;
        f.box.x4 = left;  f.box.y4 = bottom;

        for (int j = 0; j < FACE_LANDMARK_NUM; j++) {
            f.landmarks[j].x = (float)o.point[j].x;
            f.landmarks[j].y = (float)o.point[j].y;
        }
        f.det_score = o.score;
        f.match_score = 0.0f;
        f.person_id = -1;
        f.name[0] = '\0';

        if (!algo->rec_loaded) {
            continue;
        }

        cv::Mat aligned = face_align_warp_rgb(frame, f.landmarks);
        if (aligned.empty() || !aligned.isContinuous()) {
            continue;
        }

        float embed[FACE_REC_EMBED_DIM];
        if (inference_face_rec_model(&algo->rec_ctx, aligned.data, (int)aligned.step[0], embed) != 0) {
            continue;
        }

        if (algo->gallery_loaded) {
            int pid = -1;
            char name[FACE_NAME_MAX_LEN];
            float score = 0.0f;
            int matched = face_gallery_match(&algo->gallery, embed, algo->match_threshold, &pid,
                                             name, (int)sizeof(name), &score);
            f.match_score = score;
            if (matched > 0) {
                f.person_id = pid;
                strncpy(f.name, name, FACE_NAME_MAX_LEN - 1);
                f.name[FACE_NAME_MAX_LEN - 1] = '\0';
            }
        }
    }
    *rec_ms = (int)(now_ms() - rec_t0);

    return 0;
}

static bool try_load_models(face_algo *algo)
{
    if (!algo) {
        return false;
    }

    printf("face_algo: ===== load models begin =====\n");
    printf("face_algo: det_model=%s\n", algo->det_model_path);
    printf("face_algo: rec_model=%s\n", algo->rec_model_path);
    printf("face_algo: gallery=%s\n", algo->gallery_path);
    printf("face_algo: match_threshold=%.2f\n", algo->match_threshold);
    log_path_access("det_model", algo->det_model_path);
    log_path_access("rec_model", algo->rec_model_path);
    log_path_access("gallery_dir", algo->gallery_path);
    fflush(stdout);

    if (!algo->det_loaded) {
        printf("face_algo: loading SCRFD ...\n");
        fflush(stdout);
        if (init_scrfd_model(algo->det_model_path, &algo->det_ctx) == 0) {
            algo->det_loaded = true;
            printf("face_algo: SCRFD loaded ok\n");
        } else {
            fprintf(stderr, "face_algo: SCRFD load FAIL: %s\n", algo->det_model_path);
        }
    }

    if (!algo->rec_loaded) {
        printf("face_algo: loading face_rec ...\n");
        fflush(stdout);
        if (init_face_rec_model(algo->rec_model_path, &algo->rec_ctx) == 0) {
            algo->rec_loaded = true;
            printf("face_algo: face_rec loaded ok\n");
        } else {
            fprintf(stderr, "face_algo: face_rec load FAIL: %s\n", algo->rec_model_path);
        }
    }

    if (!algo->gallery_loaded) {
        printf("face_algo: loading gallery ...\n");
        fflush(stdout);
        if (face_gallery_load(algo->gallery_path, &algo->gallery) == 0) {
            algo->gallery_loaded = true;
            printf("face_algo: gallery loaded ok (%d persons)\n", algo->gallery.count);
        } else {
            fprintf(stderr,
                    "face_algo: gallery load FAIL (det/rec can still run, names will be empty): "
                    "%s\n",
                    algo->gallery_path);
        }
    }

    printf("face_algo: load summary det=%s rec=%s gallery=%s\n",
           algo->det_loaded ? "OK" : "FAIL", algo->rec_loaded ? "OK" : "FAIL",
           algo->gallery_loaded ? "OK" : "FAIL");
    printf("face_algo: ===== load models end =====\n");
    fflush(stdout);

    return algo->det_loaded && algo->rec_loaded;
}

static void release_all_models(face_algo *algo)
{
    if (!algo) {
        return;
    }
    if (algo->det_loaded) {
        release_scrfd_model(&algo->det_ctx);
        algo->det_loaded = false;
    }
    if (algo->rec_loaded) {
        release_face_rec_model(&algo->rec_ctx);
        algo->rec_loaded = false;
    }
    if (algo->gallery_loaded) {
        face_gallery_free(&algo->gallery);
        algo->gallery_loaded = false;
    }
}

static void *worker_thread_fn(void *arg)
{
    face_algo *algo = static_cast<face_algo *>(arg);

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

        face_result_set_t *local = algo->scratch;
        int det_ms = 0;
        int rec_ms = 0;
        int ret = 0;

        if (algo->det_loaded && local) {
            ret = run_face_pipeline(algo, frame, local, &det_ms, &rec_ms);
        } else if (local) {
            memset(local, 0, sizeof(*local));
            local->src_w = frame.cols;
            local->src_h = frame.rows;
            local->count = 0;
        }

        pthread_mutex_lock(&algo->mutex);
        if (ret == 0 && local && algo->latest) {
            uint32_t seq = ++algo->result_seq;
            local->seq = seq;
            local->det_ms = det_ms;
            local->rec_ms = rec_ms;
            *algo->latest = *local;
            algo->result_dirty = true;
        }
        algo->infer_busy = false;
        pthread_mutex_unlock(&algo->mutex);

        if (ret == 0 && local && local->count > 0) {
            for (int i = 0; i < local->count; i++) {
                const face_item_t *f = &local->faces[i];
                const char *name = (f->name[0] != '\0') ? f->name : "未知";
                printf("face: det=%dms rec=%dms det_conf=%.2f rec_conf=%.2f %s\n", det_ms,
                       rec_ms, f->det_score, f->match_score, name);
            }
        }
    }

    return NULL;
}

extern "C" face_algo_t *face_algo_create(const face_algo_config_t *config)
{
    face_algo_t *algo = new (std::nothrow) face_algo;
    if (!algo) {
        return NULL;
    }

    algo->det_model_path[0] = '\0';
    algo->rec_model_path[0] = '\0';
    algo->gallery_path[0] = '\0';
    algo->match_threshold = DEFAULT_MATCH_THRESHOLD;
    algo->det_loaded = false;
    algo->rec_loaded = false;
    algo->gallery_loaded = false;
    memset(&algo->det_ctx, 0, sizeof(algo->det_ctx));
    memset(&algo->rec_ctx, 0, sizeof(algo->rec_ctx));
    memset(&algo->gallery, 0, sizeof(algo->gallery));
    algo->running = false;
    algo->infer_busy = false;
    algo->frame_pending = false;
    algo->latest = NULL;
    algo->scratch = NULL;
    algo->result_dirty = false;
    algo->result_seq = 0;
    algo->state = ALGO_STATE_IDLE;

    const char *det = config ? config->det_model_path : NULL;
    const char *rec = config ? config->rec_model_path : NULL;
    const char *gallery = config ? config->gallery_path : NULL;
    if (config && config->match_threshold > 0.0f && config->match_threshold <= 1.0f) {
        algo->match_threshold = config->match_threshold;
    }

    snprintf(algo->det_model_path, sizeof(algo->det_model_path), "%s",
             resolve_path(det, "FACE_DET_MODEL", DEFAULT_DET_MODEL_PATH));
    snprintf(algo->rec_model_path, sizeof(algo->rec_model_path), "%s",
             resolve_path(rec, "FACE_REC_MODEL", DEFAULT_REC_MODEL_PATH));
    snprintf(algo->gallery_path, sizeof(algo->gallery_path), "%s",
             resolve_path(gallery, "FACE_GALLERY", DEFAULT_GALLERY_PATH));

    algo->latest = (face_result_set_t *)calloc(1, sizeof(face_result_set_t));
    algo->scratch = (face_result_set_t *)calloc(1, sizeof(face_result_set_t));
    if (!algo->latest || !algo->scratch) {
        free(algo->latest);
        free(algo->scratch);
        delete algo;
        return NULL;
    }

    pthread_mutex_init(&algo->mutex, NULL);
    pthread_cond_init(&algo->cond, NULL);
    return algo;
}

extern "C" int face_algo_start(face_algo_t *algo)
{
    if (!algo) {
        return -1;
    }
    if (algo->running) {
        return 0;
    }

    if (!try_load_models(algo)) {
        fprintf(stderr, "face_algo: start aborted: required model missing\n");
        fprintf(stderr, "face_algo:   det=%s (%s)\n", algo->det_model_path,
                algo->det_loaded ? "ok" : "fail");
        fprintf(stderr, "face_algo:   rec=%s (%s)\n", algo->rec_model_path,
                algo->rec_loaded ? "ok" : "fail");
        fprintf(stderr, "face_algo:   gallery=%s (%s, optional for startup)\n", algo->gallery_path,
                algo->gallery_loaded ? "ok" : "fail");
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

extern "C" int face_algo_stop(face_algo_t *algo)
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

extern "C" bool face_algo_is_running(const face_algo_t *algo)
{
    return algo && algo->running;
}

extern "C" bool face_algo_models_ready(const face_algo_t *algo)
{
    return algo && algo->det_loaded && algo->rec_loaded;
}

extern "C" int face_algo_submit_rgb888(face_algo_t *algo, const uint8_t *rgb, int w, int h)
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

extern "C" int face_algo_get_result(face_algo_t *algo, face_result_set_t *out)
{
    if (!algo || !out) {
        return -1;
    }

    pthread_mutex_lock(&algo->mutex);
    if (!algo->result_dirty) {
        pthread_mutex_unlock(&algo->mutex);
        return 1;
    }
    *out = *algo->latest;
    algo->result_dirty = false;
    pthread_mutex_unlock(&algo->mutex);
    return 0;
}

extern "C" int face_algo_process_frame(face_algo_t *algo, const camera_frame_t *frame,
                                       algo_result_cb_t cb, void *user_data)
{
    if (!algo || !frame || !frame->data) {
        return -1;
    }
    if (frame->fmt != CAMERA_FMT_RGB888) {
        return -1;
    }

    int sret = face_algo_submit_rgb888(algo, (const uint8_t *)frame->data, frame->width,
                                       frame->height);
    if (sret < 0) {
        return -1;
    }

    if (cb) {
        face_result_set_t result;
        if (face_algo_get_result(algo, &result) == 0) {
            algo_result_t out;
            memset(&out, 0, sizeof(out));
            out.id = ALGO_ID_FACE;
            out.state = algo->state;
            out.hit_count = result.count;
            size_t off = 0;
            for (int i = 0; i < result.count && off < sizeof(out.text) - 2; i++) {
                const face_item_t *f = &result.faces[i];
                int rem = (int)(sizeof(out.text) - 1 - off);
                const char *label = (f->name[0] != '\0') ? f->name : "unknown";
                int n = snprintf(out.text + off, (size_t)rem, "%s %.0f%%\n", label,
                                 f->match_score * 100.0f);
                if (n <= 0) {
                    break;
                }
                off += (size_t)n;
            }
            if (result.count == 0 && off == 0) {
                snprintf(out.text, sizeof(out.text), "no face");
            }
            cb(&out, user_data);
        }
    }
    return 0;
}

extern "C" void face_algo_destroy(face_algo_t *algo)
{
    if (!algo) {
        return;
    }
    face_algo_stop(algo);
    free(algo->latest);
    free(algo->scratch);
    algo->latest = NULL;
    algo->scratch = NULL;
    pthread_cond_destroy(&algo->cond);
    pthread_mutex_destroy(&algo->mutex);
    delete algo;
}
