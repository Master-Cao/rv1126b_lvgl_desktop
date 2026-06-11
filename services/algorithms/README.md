# algorithms

RKNN 等推理算法的统一封装层。

| 文件 | 职责 |
|------|------|
| `algo_types.h` | 算法 ID、通用结果结构 |
| `algo_runner.c/.h` | 按类型创建/启停算法（可选调度入口） |
| `ocr/` | PP-OCR 检测 + 识别 |
| `detect/` | YOLOv8 目标检测（COCO 80 类） |
| `hair/` | 头发检测（经典 CV，OpenCV 实现） |
| `face/` | 人脸检测 + 识别（SCRFD + MobileFaceNet RKNN） |
| `plate/` | 车牌检测（YOLOv8 `plate_det.rknn`，识别待接入） |
| `seg/` | YOLOv8 实例分割（COCO 80 类，`yolov8_seg.rknn`） |

每个算法目录提供一致的接口：

```c
*_algo_t *xxx_algo_create(...);
int xxx_algo_start(*_algo_t *);
int xxx_algo_stop(*_algo_t *);
int xxx_algo_submit_rgb888(*_algo_t *, const uint8_t *rgb, int w, int h);
int xxx_algo_get_result(*_algo_t *, *_result_set_t *out);
int xxx_algo_process_frame(*_algo_t *, const camera_frame_t *, algo_result_cb_t, void *);
void xxx_algo_destroy(*_algo_t *);
```

由对应 `lvgl_desktop/apps/*_app.c` 在按钮回调里调用。worker 线程内部完成模型推理，
UI 线程通过 `submit + get` 异步交互，避免阻塞 LVGL 主循环。
