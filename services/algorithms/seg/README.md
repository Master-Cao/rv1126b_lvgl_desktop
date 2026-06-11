# seg

YOLOv8 实例分割模块（移植自 `atk_yolov8_seg_cam`）。

## 管线

1. **预处理**：RGB888 letterbox 到模型输入（默认 640×640，填充 114）
2. **推理**：`yolov8_seg.rknn`（13 路输出：3 检测头 + proto）
3. **后处理**：DFL 解码 + NMS + mask 矩阵乘 + 还原到原图
4. **输出**：每实例 bbox + class + score；全图合并 `seg_mask`（可选用于 UI 叠加）

## 接口

```c
seg_algo_t *seg_algo_create(const seg_algo_config_t *config);
int seg_algo_start(seg_algo_t *algo);
int seg_algo_stop(seg_algo_t *algo);
int seg_algo_submit_rgb888(seg_algo_t *algo, const uint8_t *rgb, int w, int h);
int seg_algo_get_result(seg_algo_t *algo, seg_result_set_t *out);
void seg_algo_destroy(seg_algo_t *algo);
```

## 模型

| 环境变量 | 默认路径 |
|----------|----------|
| `SEG_MODEL` | `/opt/rv1126b_desktop/models/yolov8_seg.rknn` |

COCO 80 类标签已内嵌到 `postprocess.cc`，板端无需 `coco_80_labels_list.txt`。

## 文件

| 文件 | 职责 |
|------|------|
| `seg_algo.cc/.h` | 对外 C 接口 + worker 线程 |
| `yolov8_seg.cc/.h` | RKNN 加载与单帧推理 |
| `postprocess.cc/.h` | DFL + NMS + mask 生成 |
| `common.h` | `image_buffer_t` 等公共类型 |
