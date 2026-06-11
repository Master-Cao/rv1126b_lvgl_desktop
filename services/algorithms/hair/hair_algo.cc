/*
 * 头发丝检测算法（纯 OpenCV，CPU 实现）
 *
 * 处理管线（针对火锅底料/油脂/塑料封膜等强反光、低对比场景）：
 *   1. RGB -> 灰度
 *   2. 大尺度高斯模糊作为背景估计，flat = gray - bg + 128 压平光照不均；
 *   3. 多角度（0/30/60/90/120/150°）线形结构元做 Black-hat 形态学运算，
 *      取各角度响应的逐像素最大值，强化"细暗线"；
 *   4. Otsu 阈值 + 形态学闭运算把断裂的线段连接起来；
 *   5. findContours 得到候选连通域，对每个候选做几何过滤：
 *        - minAreaRect 提取主/次轴长度；
 *        - 主轴长度 > min_length_px；
 *        - 次轴宽度 < max_width_px；
 *        - 主/次轴比 > min_aspect；
 *        - 候选区域在 Black-hat 响应图上的均值高于 min_score 阈值；
 *   6. 输出 minAreaRect 的 4 个角点（与 camera_preview 的 4 点多边形对齐）。
 *
 * 与 ocr_algo 一致地使用独立 worker 线程，UI 通过 submit/get_result 异步交互。
 */
#include "hair_algo.h"

#include <opencv2/opencv.hpp>

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define DEFAULT_BLUR_KSIZE           51
#define DEFAULT_BLACKHAT_LENGTH      15
#define DEFAULT_MIN_LENGTH_PX        40   /* 褶皱通常更短，提高门槛过滤掉它们 */
#define DEFAULT_MAX_WIDTH_PX         6
#define DEFAULT_MIN_ASPECT           6.0f
#define DEFAULT_MIN_SCORE            0.30f

#define DEFAULT_DARK_V_THRESHOLD     90    /* max(R,G,B) < 90 视为"黑色" */
#define DEFAULT_MIN_DARK_RATIO       0.45f /* bbox 内黑色占比 */
#define DEFAULT_MIN_CONTINUOUS_RATIO 0.55f /* 主轴最长连续黑色段比例 */

#define HAIR_ANGLE_COUNT             6

struct hair_algo {
    hair_algo_params_t params;

    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    volatile bool running;
    volatile bool infer_busy;
    volatile bool frame_pending;

    cv::Mat pending_frame;

    hair_result_set_t latest;
    bool result_dirty;
    uint32_t result_seq;

    algo_state_t state;

    /* 预先计算好的多角度结构元，每次推理直接用，避免反复构造 */
    cv::Mat kernels[HAIR_ANGLE_COUNT];
};

static double now_ms()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/* 构造一个 len x len 的方形 8U 内核，沿指定角度画一条 1 像素粗的线段。
 * 用作 Black-hat 的"细长线结构元"，覆盖任意方向的发丝。 */
static cv::Mat make_line_kernel(int len, double angle_deg)
{
    int s = len | 1; /* 强制奇数，保证中心对称 */
    cv::Mat k = cv::Mat::zeros(s, s, CV_8U);
    double rad = angle_deg * CV_PI / 180.0;
    double dx = std::cos(rad);
    double dy = std::sin(rad);
    int half = s / 2;
    cv::Point p1((int)std::round(half - dx * half), (int)std::round(half - dy * half));
    cv::Point p2((int)std::round(half + dx * half), (int)std::round(half + dy * half));
    cv::line(k, p1, p2, cv::Scalar(1), 1, cv::LINE_8);
    return k;
}

static void build_kernels(hair_algo *algo)
{
    int len = algo->params.blackhat_length;
    if (len < 5) {
        len = 5;
    }
    const double angles[HAIR_ANGLE_COUNT] = {0.0, 30.0, 60.0, 90.0, 120.0, 150.0};
    for (int i = 0; i < HAIR_ANGLE_COUNT; i++) {
        algo->kernels[i] = make_line_kernel(len, angles[i]);
    }
}

/* 多角度 Black-hat：response = max_i ( blackhat(flat, kernel_i) ). */
static void multi_angle_blackhat(const cv::Mat &flat, const cv::Mat kernels[HAIR_ANGLE_COUNT],
                                 cv::Mat &response)
{
    cv::Mat tmp;
    response = cv::Mat::zeros(flat.size(), CV_8U);
    for (int i = 0; i < HAIR_ANGLE_COUNT; i++) {
        cv::morphologyEx(flat, tmp, cv::MORPH_BLACKHAT, kernels[i]);
        cv::max(response, tmp, response);
    }
}

/* 给定 minAreaRect 的 4 角点，识别主轴端点：找到长边对边的中点连线 [mid_a, mid_b]，
 * 即"沿发丝长度方向的中线"。 */
static void rect_major_axis(const cv::Point2f corners[4], cv::Point2f &mid_a,
                            cv::Point2f &mid_b)
{
    float d01 = (float)cv::norm(corners[0] - corners[1]);
    float d12 = (float)cv::norm(corners[1] - corners[2]);
    if (d01 >= d12) {
        /* 0-1 和 2-3 是长边 → 短边为 1-2 与 3-0，其中点连线即主轴 */
        mid_a = (corners[1] + corners[2]) * 0.5f;
        mid_b = (corners[3] + corners[0]) * 0.5f;
    } else {
        mid_a = (corners[0] + corners[1]) * 0.5f;
        mid_b = (corners[2] + corners[3]) * 0.5f;
    }
}

/* 沿主轴中线均匀采样 N 个点，统计：
 *   - dark_ratio    : 黑色采样点 / N
 *   - continuity    : 最长连续黑色段 / N
 * dark_mask 应是 0/255 的二值图。 */
static void axis_continuity_stats(const cv::Mat &dark_mask, const cv::Point2f &a,
                                  const cv::Point2f &b, int samples, float &dark_ratio,
                                  float &continuity)
{
    int dark = 0;
    int longest = 0;
    int cur = 0;
    if (samples < 2) {
        samples = 2;
    }
    for (int i = 0; i < samples; i++) {
        float t = (float)i / (float)(samples - 1);
        cv::Point2f p = a * (1.f - t) + b * t;
        int x = (int)std::round(p.x);
        int y = (int)std::round(p.y);
        bool inside = (x >= 0 && y >= 0 && x < dark_mask.cols && y < dark_mask.rows);
        bool is_dark = inside && dark_mask.at<uchar>(y, x) > 0;
        if (is_dark) {
            dark++;
            cur++;
            if (cur > longest) {
                longest = cur;
            }
        } else {
            cur = 0;
        }
    }
    dark_ratio = (float)dark / (float)samples;
    continuity = (float)longest / (float)samples;
}

/* 把一个 contour 的 minAreaRect 转成 4 点角点，做几何 + 黑色 + 连续性三重校验。
 * 失败返回 false；成功填充 out 并返回 true。 */
static bool extract_line_from_contour(const std::vector<cv::Point> &contour,
                                      const cv::Mat &response, const cv::Mat &dark_mask,
                                      const hair_algo_params_t &params, hair_line_t &out)
{
    if (contour.size() < 5) {
        return false;
    }
    cv::RotatedRect rr = cv::minAreaRect(contour);
    float w = rr.size.width;
    float h = rr.size.height;
    if (w <= 0.f || h <= 0.f) {
        return false;
    }
    float major = std::max(w, h);
    float minor = std::min(w, h);
    /* 几何：长 + 细 + 高长宽比 */
    if (major < (float)params.min_length_px) {
        return false;
    }
    if (minor > (float)params.max_width_px) {
        return false;
    }
    float aspect = major / std::max(minor, 1.f);
    if (aspect < params.min_aspect) {
        return false;
    }

    /* 在 response / dark_mask 上仅在 contour 内部统计 */
    cv::Rect br = cv::boundingRect(contour) & cv::Rect(0, 0, response.cols, response.rows);
    if (br.width <= 0 || br.height <= 0) {
        return false;
    }
    cv::Mat poly_mask = cv::Mat::zeros(br.size(), CV_8U);
    std::vector<cv::Point> shifted;
    shifted.reserve(contour.size());
    for (const cv::Point &p : contour) {
        shifted.emplace_back(p.x - br.x, p.y - br.y);
    }
    const cv::Point *pts = shifted.data();
    int npts = (int)shifted.size();
    cv::fillPoly(poly_mask, &pts, &npts, 1, cv::Scalar(255));

    /* (1) Black-hat 响应均值 */
    cv::Scalar resp_mean = cv::mean(response(br), poly_mask);
    float resp_score = (float)(resp_mean[0] / 255.0);

    /* (2) bbox 内黑色像素比例（"黑"即 dark_mask=255） */
    cv::Mat dark_in_box;
    cv::bitwise_and(dark_mask(br), poly_mask, dark_in_box);
    int dark_cnt = cv::countNonZero(dark_in_box);
    int poly_cnt = cv::countNonZero(poly_mask);
    float dark_ratio_box = poly_cnt > 0 ? (float)dark_cnt / (float)poly_cnt : 0.f;
    if (dark_ratio_box < params.min_dark_ratio) {
        return false;
    }

    /* (3) 主轴沿线最长连续黑色段比例（最关键的一条：褶皱常常不连续） */
    cv::Point2f corners[4];
    rr.points(corners);
    cv::Point2f mid_a, mid_b;
    rect_major_axis(corners, mid_a, mid_b);
    int samples = std::max(20, (int)major);
    float axis_dark_ratio = 0.f;
    float continuity = 0.f;
    axis_continuity_stats(dark_mask, mid_a, mid_b, samples, axis_dark_ratio, continuity);
    if (continuity < params.min_continuous_ratio) {
        return false;
    }

    /* 综合得分：三项加权（响应强度 + 整体黑色比例 + 主轴连续性） */
    float score = 0.4f * resp_score + 0.3f * dark_ratio_box + 0.3f * continuity;
    if (score < params.min_score) {
        return false;
    }

    out.box.x1 = (int)std::round(corners[0].x);
    out.box.y1 = (int)std::round(corners[0].y);
    out.box.x2 = (int)std::round(corners[1].x);
    out.box.y2 = (int)std::round(corners[1].y);
    out.box.x3 = (int)std::round(corners[2].x);
    out.box.y3 = (int)std::round(corners[2].y);
    out.box.x4 = (int)std::round(corners[3].x);
    out.box.y4 = (int)std::round(corners[3].y);
    out.length_px = major;
    out.width_px = minor;
    out.score = score;
    return true;
}

/* 颜色"黑色"蒙版：max(R,G,B) < dark_v_threshold。
 * 这一步利用了"火锅底料偏橙红 / 高光偏亮，而头发偏黑"的先验。
 * 例如：(R=220,G=80,B=40) 的辣椒红 → max=220，被排除；
 *       (R=20,G=18,B=22) 的发丝 → max=22，命中；
 *       (R=240,G=240,B=235) 的塑料高光 → max=240，被排除。 */
static void build_dark_mask(const cv::Mat &rgb, int dark_v_threshold, cv::Mat &dark_mask)
{
    std::vector<cv::Mat> ch;
    cv::split(rgb, ch);
    cv::Mat maxch;
    cv::max(ch[0], ch[1], maxch);
    cv::max(maxch, ch[2], maxch);
    /* maxch < dark_v_threshold → 255 */
    cv::threshold(maxch, dark_mask, (double)dark_v_threshold, 255, cv::THRESH_BINARY_INV);
}

static void detect_hair_lines(hair_algo *algo, const cv::Mat &rgb,
                              hair_result_set_t *result, int infer_ms, uint32_t seq)
{
    /* (1) 颜色先验：构造"黑色"蒙版，作为后续二值化的强约束。 */
    cv::Mat dark_mask;
    build_dark_mask(rgb, algo->params.dark_v_threshold, dark_mask);

    cv::Mat gray;
    cv::cvtColor(rgb, gray, cv::COLOR_RGB2GRAY);

    /* (2) 大尺度光照预压平：高光截到 bg 上限，稳定后续 Otsu。 */
    cv::Mat src = gray;
    cv::Mat flattened;
    int blur_k = algo->params.blur_ksize | 1;
    if (blur_k >= 3) {
        cv::Mat bg;
        cv::GaussianBlur(gray, bg, cv::Size(blur_k, blur_k), 0);
        cv::min(gray, bg, flattened);
        src = flattened;
    }

    /* (3) 多角度 Black-hat：闭运算 - src，强响应=暗于局部背景的窄结构。 */
    cv::Mat resp;
    multi_angle_blackhat(src, algo->kernels, resp);

    /* (4) Otsu 二值化 */
    cv::Mat bh_bin;
    cv::threshold(resp, bh_bin, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);

    /* (5) 与"黑色"蒙版求交 —— 这是把橙红色干扰彻底排除的关键。
     *     褶皱产生的 Black-hat 响应若位于橙色区域，到这里就被滤掉。 */
    cv::Mat bin;
    cv::bitwise_and(bh_bin, dark_mask, bin);

    /* (6) 闭运算连接发丝因压缩/塑料褶皱被打断的小段，连成"连续黑色细线" */
    cv::Mat se = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::morphologyEx(bin, bin, cv::MORPH_CLOSE, se);

    /* (7) 候选轮廓 */
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(bin, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);

    int kept = 0;
    for (size_t i = 0; i < contours.size() && kept < HAIR_MAX_LINES; i++) {
        hair_line_t line;
        memset(&line, 0, sizeof(line));
        if (extract_line_from_contour(contours[i], resp, dark_mask, algo->params, line)) {
            result->lines[kept++] = line;
        }
    }
    result->count = kept;
    result->src_w = rgb.cols;
    result->src_h = rgb.rows;
    result->infer_ms = infer_ms;
    result->seq = seq;
}

static void *worker_thread_fn(void *arg)
{
    hair_algo *algo = static_cast<hair_algo *>(arg);

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

        hair_result_set_t local;
        memset(&local, 0, sizeof(local));

        double t0 = now_ms();
        detect_hair_lines(algo, frame, &local, 0, 0);
        int elapsed = (int)(now_ms() - t0);

        pthread_mutex_lock(&algo->mutex);
        uint32_t seq = ++algo->result_seq;
        local.seq = seq;
        local.infer_ms = elapsed;
        algo->latest = local;
        algo->result_dirty = true;
        algo->infer_busy = false;
        pthread_mutex_unlock(&algo->mutex);

        if (local.count > 0) {
            printf("hair_algo: %d candidates, %d ms\n", local.count, elapsed);
        }
    }
    return NULL;
}

extern "C" hair_algo_t *hair_algo_create(const hair_algo_params_t *params)
{
    hair_algo_t *algo = new (std::nothrow) hair_algo;
    if (!algo) {
        return NULL;
    }
    memset(&algo->latest, 0, sizeof(algo->latest));
    algo->running = false;
    algo->infer_busy = false;
    algo->frame_pending = false;
    algo->result_dirty = false;
    algo->result_seq = 0;
    algo->state = ALGO_STATE_IDLE;

    pthread_mutex_init(&algo->mutex, NULL);
    pthread_cond_init(&algo->cond, NULL);

    if (params) {
        algo->params = *params;
    } else {
        memset(&algo->params, 0, sizeof(algo->params));
    }
    if (algo->params.blur_ksize <= 0) {
        algo->params.blur_ksize = DEFAULT_BLUR_KSIZE;
    }
    if (algo->params.blackhat_length <= 0) {
        algo->params.blackhat_length = DEFAULT_BLACKHAT_LENGTH;
    }
    if (algo->params.min_length_px <= 0) {
        algo->params.min_length_px = DEFAULT_MIN_LENGTH_PX;
    }
    if (algo->params.max_width_px <= 0) {
        algo->params.max_width_px = DEFAULT_MAX_WIDTH_PX;
    }
    if (algo->params.min_aspect <= 0.f) {
        algo->params.min_aspect = DEFAULT_MIN_ASPECT;
    }
    if (algo->params.min_score <= 0.f) {
        algo->params.min_score = DEFAULT_MIN_SCORE;
    }
    if (algo->params.dark_v_threshold <= 0) {
        algo->params.dark_v_threshold = DEFAULT_DARK_V_THRESHOLD;
    }
    if (algo->params.min_dark_ratio <= 0.f) {
        algo->params.min_dark_ratio = DEFAULT_MIN_DARK_RATIO;
    }
    if (algo->params.min_continuous_ratio <= 0.f) {
        algo->params.min_continuous_ratio = DEFAULT_MIN_CONTINUOUS_RATIO;
    }

    build_kernels(algo);
    return algo;
}

extern "C" int hair_algo_start(hair_algo_t *algo)
{
    if (!algo) {
        return -1;
    }
    if (algo->running) {
        return 0;
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

extern "C" int hair_algo_stop(hair_algo_t *algo)
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

    algo->state = ALGO_STATE_IDLE;
    return 0;
}

extern "C" bool hair_algo_is_running(const hair_algo_t *algo)
{
    return algo && algo->running;
}

extern "C" int hair_algo_submit_rgb888(hair_algo_t *algo, const uint8_t *rgb, int w, int h)
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

extern "C" int hair_algo_get_result(hair_algo_t *algo, hair_result_set_t *out)
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

extern "C" int hair_algo_process_frame(hair_algo_t *algo, const camera_frame_t *frame,
                                       algo_result_cb_t cb, void *user_data)
{
    if (!algo || !frame || !frame->data) {
        return -1;
    }
    if (frame->fmt != CAMERA_FMT_RGB888) {
        return -1;
    }
    int sret = hair_algo_submit_rgb888(algo, (const uint8_t *)frame->data, frame->width,
                                       frame->height);
    if (sret < 0) {
        return -1;
    }
    if (cb) {
        hair_result_set_t result;
        if (hair_algo_get_result(algo, &result) == 0) {
            algo_result_t out;
            memset(&out, 0, sizeof(out));
            out.id = ALGO_ID_DETECT; /* 暂复用检测大类 */
            out.state = algo->state;
            out.hit_count = result.count;
            snprintf(out.text, sizeof(out.text), "hair lines: %d", result.count);
            cb(&out, user_data);
        }
    }
    return 0;
}

extern "C" void hair_algo_destroy(hair_algo_t *algo)
{
    if (!algo) {
        return;
    }
    hair_algo_stop(algo);
    pthread_cond_destroy(&algo->cond);
    pthread_mutex_destroy(&algo->mutex);
    delete algo;
}
