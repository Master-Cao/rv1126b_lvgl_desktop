#ifndef DETECT_ALGO_H
#define DETECT_ALGO_H

#include "../algo_types.h"
#include "../../camera/camera_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DETECT_MAX_RESULTS  32
#define DETECT_NAME_MAX_LEN 32

/* 与 camera_preview_box_t / ocr_box_t 字段同布局，可直接打包为 4 点多边形。
 * YOLOv8 输出的是轴对齐矩形，这里把 (left,top,right,bottom) 展开成
 * (x1,y1)=左上, (x2,y2)=右上, (x3,y3)=右下, (x4,y4)=左下。 */
typedef struct {
    int x1, y1;
    int x2, y2;
    int x3, y3;
    int x4, y4;
} detect_box_t;

typedef struct {
    detect_box_t box;
    int cls_id;
    float score;
    char name[DETECT_NAME_MAX_LEN]; /* coco_cls_to_name 拷贝过来，UI 不再依赖 detect 内部 */
} detect_object_t;

typedef struct {
    detect_object_t objects[DETECT_MAX_RESULTS];
    int count;
    int src_w;
    int src_h;
    uint32_t seq;
    int infer_ms;
} detect_result_set_t;

typedef struct detect_algo detect_algo_t;

/* 模型路径可为 NULL，内部按 DETECT_MODEL 环境变量回退到
 * /opt/rv1126b_desktop/models/yolov8.rknn。 */
detect_algo_t *detect_algo_create(const char *model_path);

/* 加载 RKNN 模型并启动 worker 线程（与 ocr_algo_start 行为一致）。 */
int detect_algo_start(detect_algo_t *algo);

/* 停止线程、释放推理上下文。 */
int detect_algo_stop(detect_algo_t *algo);

bool detect_algo_is_running(const detect_algo_t *algo);

/* 非阻塞提交一帧 RGB888；推理线程繁忙或已有 pending 时直接丢弃。
 * 返回 0=已接受，1=丢弃，-1=错误。 */
int detect_algo_submit_rgb888(detect_algo_t *algo, const uint8_t *rgb, int w, int h);

/* 拷贝最新一次推理结果，没有新结果时返回 1，有新结果返回 0。 */
int detect_algo_get_result(detect_algo_t *algo, detect_result_set_t *out);

/* 兼容 algo_runner 旧接口：内部 submit + get + 触发回调。 */
int detect_algo_process_frame(detect_algo_t *algo, const camera_frame_t *frame,
                              algo_result_cb_t cb, void *user_data);

void detect_algo_destroy(detect_algo_t *algo);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DETECT_ALGO_H */
