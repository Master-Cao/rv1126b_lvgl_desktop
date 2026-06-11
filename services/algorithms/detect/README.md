# detect

YOLOv8 目标检测算法封装，对接 RKNN 模型 `yolov8.rknn`（COCO 80 类）。

## 文件

| 文件 | 职责 |
|------|------|
| `detect_algo.h/.cc` | 对外 C 接口 + worker 线程，结构与 `ocr_algo` 完全对齐 |
| `yolov8.h/.cc` | RKNN 上下文管理 + 单帧推理；预处理走 OpenCV CPU letterbox（不依赖 librga） |
| `postprocess.h/.cc` | 三分支 DFL 解码 + NMS；COCO 80 类别名嵌入二进制，运行时不再读 txt |
| `common.h` | `image_buffer_t / image_format_t / image_rect_t`（与 `ocr/common.h` 字段一致） |

## 调用流水线（与 OCR 一致）

```
ocr_algo_create -> ocr_algo_start -> submit_rgb888 -> get_result
detect_algo_create -> detect_algo_start -> submit_rgb888 -> get_result
```

worker 线程：

1. 等待 `frame_pending`；
2. 把 cv::Mat 包装成 `image_buffer_t`；
3. 调用 `inference_yolov8_model`：
   - `convert_image_with_letterbox_cpu` 做保比例缩放 + 灰边填充；
   - `rknn_inputs_set / rknn_run / rknn_outputs_get`；
   - `post_process` 解 DFL + NMS，写回 `object_detect_result_list`。
4. 拷到 `latest`，置 `result_dirty`。

## 模型路径回退

`detect_algo_create(model_path)`：

1. 入参 `model_path` 非空 → 直接用；
2. 否则取环境变量 `DETECT_MODEL`；
3. 否则回落到 `/opt/rv1126b_desktop/models/yolov8.rknn`。

## 不依赖项

为了与桌面工程现有 toolchain 兼容，去掉了 rknn_model_zoo demo 中的：

- `librga` / `libturbojpeg` / `stb_image`（预处理改成 OpenCV CPU 版）；
- `file_utils.c`（`read_data_from_file` 内联）；
- `coco_80_labels_list.txt`（80 类标签嵌入到 `postprocess.cc`）。
