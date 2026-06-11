# plate

车牌检测模块（YOLOv8 RKNN，单类别 `plate`）。

## 管线

1. **检测**（已接入）：`plate_det.rknn` → 车牌 bbox + 置信度
2. **识别**（暂未启用）：`plate_rec.rknn` / LPRNet 代码保留在 `lprnet.cc`，后续可对每个 bbox 裁剪后识别

## 接口

```c
plate_algo_t *plate_algo_create(const plate_algo_config_t *config);
int plate_algo_start(plate_algo_t *algo);
int plate_algo_stop(plate_algo_t *algo);
int plate_algo_submit_rgb888(plate_algo_t *algo, const uint8_t *rgb, int w, int h);
int plate_algo_get_result(plate_algo_t *algo, plate_result_set_t *out);
void plate_algo_destroy(plate_algo_t *algo);
```

## 模型路径

| 环境变量 | 默认路径 |
|----------|----------|
| `PLATE_DET_MODEL` | `/opt/rv1126b_desktop/models/plate_det.rknn` |
| `PLATE_REC_MODEL` | `/opt/rv1126b_desktop/models/plate_rec.rknn`（识别暂未使用） |

## 文件

| 文件 | 职责 |
|------|------|
| `plate_algo.cc/.h` | 对外 C 接口 + worker 线程 |
| `yolov8_plate.cc/.h` | RKNN 加载、letterbox 推理 |
| `postprocess.cc/.h` | DFL + NMS 后处理（1 类） |
| `lprnet.cc/.h` | LPRNet 识别（保留，当前未调用） |
| `common.h` | `image_buffer_t` 等 |
