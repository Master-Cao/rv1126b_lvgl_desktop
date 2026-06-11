#ifndef OCR_ALGO_H
#define OCR_ALGO_H

#include "../algo_types.h"
#include "../../camera/camera_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OCR_MAX_LINES 64
#define OCR_TEXT_LEN  512

typedef struct {
    int x1, y1;
    int x2, y2;
    int x3, y3;
    int x4, y4;
} ocr_box_t;

typedef struct {
    ocr_box_t box;
    char text[OCR_TEXT_LEN];
    float score;
} ocr_text_line_t;

typedef struct {
    ocr_text_line_t lines[OCR_MAX_LINES];
    int count;
    int src_w;
    int src_h;
    uint32_t seq;
    int infer_ms;
} ocr_result_set_t;

typedef struct ocr_algo ocr_algo_t;

/* 模型路径可为 NULL，内部回退到环境变量 OCR_DET_MODEL/OCR_REC_MODEL，
 * 再回退到 /opt/rv1126b_desktop/models/ppocrv4_det.rknn 与 ppocrv4_rec.rknn */
ocr_algo_t *ocr_algo_create(const char *det_model, const char *rec_model);

/* 加载 RKNN 模型并启动 worker 线程 */
int ocr_algo_start(ocr_algo_t *algo);

/* 停止线程、释放推理上下文 */
int ocr_algo_stop(ocr_algo_t *algo);

bool ocr_algo_is_running(const ocr_algo_t *algo);

/* 非阻塞提交一帧 RGB888；推理线程繁忙或已有 pending 时直接丢弃，返回 0 表示已接受、1 表示丢弃 */
int ocr_algo_submit_rgb888(ocr_algo_t *algo, const uint8_t *rgb, int w, int h);

/* 拷贝最新一次推理结果，没有新结果时返回 1，有新结果返回 0 */
int ocr_algo_get_result(ocr_algo_t *algo, ocr_result_set_t *out);

/* 兼容旧接口：内部 submit + get + 触发回调 */
int ocr_algo_process_frame(ocr_algo_t *algo, const camera_frame_t *frame,
                           algo_result_cb_t cb, void *user_data);

void ocr_algo_destroy(ocr_algo_t *algo);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
