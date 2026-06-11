#ifndef HAIR_ALGO_H
#define HAIR_ALGO_H

#include "../algo_types.h"
#include "../../camera/camera_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HAIR_MAX_LINES 32

/* 与 camera_preview_box_t 字段一致；调用方可直接打包传入。 */
typedef struct {
    int x1, y1;
    int x2, y2;
    int x3, y3;
    int x4, y4;
} hair_box_t;

typedef struct {
    hair_box_t box;
    float length_px; /* 沿主轴的长度（像素） */
    float width_px;  /* 沿次轴的宽度（像素） */
    float score;     /* 0..1，候选可信度 */
} hair_line_t;

typedef struct {
    hair_line_t lines[HAIR_MAX_LINES];
    int count;
    int src_w;
    int src_h;
    uint32_t seq;
    int infer_ms;
} hair_result_set_t;

/* 可调参数；为空时使用默认值。算法本身是纯 CV，不需要模型文件，
 * 但保留参数结构方便后续扩展（例如训练好的 RKNN 二分类校验模型）。
 *
 * 头发丝的判定逻辑：「连续的黑色细长线」。
 *   - 「黑色」：要求像素 max(R,G,B) < dark_v_threshold，
 *     这样可天然排除橙红色火锅底料（高 R）、油亮高光（高 V）等。
 *   - 「细长」：minAreaRect 主轴 >= min_length_px、次轴 <= max_width_px、长宽比 >= min_aspect。
 *   - 「连续」：沿候选主轴采样 N 点，最长连续黑色段 / N >= min_continuous_ratio。
 *
 * 褶皱通常是断续短弧 + 颜色偏暖，被以上三条联合过滤掉。
 */
typedef struct {
    int blur_ksize;        /* 背景估计高斯核大小，默认 51（奇数） */
    int blackhat_length;   /* 黑帽结构元长度（像素），默认 15 */
    int min_length_px;     /* 候选最小长度，默认 40（褶皱通常更短） */
    int max_width_px;      /* 候选最大宽度，默认 6 */
    float min_aspect;      /* 候选最小长宽比，默认 6.0 */
    float min_score;       /* 综合得分阈值（0..1），默认 0.30 */

    /* 颜色与连续性约束 */
    int dark_v_threshold;        /* "黑色"判定：max(R,G,B) < 此值，默认 90 */
    float min_dark_ratio;        /* 候选 bbox 内黑色像素占比下限，默认 0.45 */
    float min_continuous_ratio;  /* 主轴方向上最长连续黑色 / 总采样点，默认 0.55 */
} hair_algo_params_t;

typedef struct hair_algo hair_algo_t;

/* 创建头发检测算法实例；params 为 NULL 时使用默认参数。 */
hair_algo_t *hair_algo_create(const hair_algo_params_t *params);

/* 启动 worker 线程。 */
int hair_algo_start(hair_algo_t *algo);

/* 停止线程。 */
int hair_algo_stop(hair_algo_t *algo);

bool hair_algo_is_running(const hair_algo_t *algo);

/* 非阻塞提交 RGB888 帧；返回 0=已接受、1=丢弃、-1=错误。 */
int hair_algo_submit_rgb888(hair_algo_t *algo, const uint8_t *rgb, int w, int h);

/* 取一次最新结果；0=有新结果、1=无新结果、-1=错误。 */
int hair_algo_get_result(hair_algo_t *algo, hair_result_set_t *out);

/* 兼容旧 algo_runner 接口。 */
int hair_algo_process_frame(hair_algo_t *algo, const camera_frame_t *frame,
                            algo_result_cb_t cb, void *user_data);

void hair_algo_destroy(hair_algo_t *algo);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
