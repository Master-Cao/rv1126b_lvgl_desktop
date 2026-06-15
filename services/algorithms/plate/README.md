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
| `PLATE_DET_MODEL` | `/opt/rv1126b_desktop/models/plate_det.rknn`（亦支持 `plate.rknn`） |
| `PLATE_REC_MODEL` | `/opt/rv1126b_desktop/models/plate_rec.rknn`（识别暂未使用） |

## 模型格式

板端后处理支持两种 RKNN 输出形态：

| `output num` | 来源 | 后处理 |
|--------------|------|--------|
| **1** | Ultralytics 导出 ONNX 再转 RKNN，如 `(1,5,8400)` | 融合头 cxcywh + score |
| **9** | rknn_model_zoo 三分支 DFL 模型 | 与原 detect/yolov8 相同 |

若 PC 上 PT/ONNX 能检出但板端 `plate: no box`，请查看启动日志里 `output num=` 是否为 1；旧代码只实现了 9 路 DFL 解码。

## 板端离线测试

交叉编译生成 `build/Release/plate_test`（不依赖 LVGL/相机）：

```bash
./build.sh
scp build/Release/plate_test root@<board>:/opt/rv1126b_desktop/
scp scripts/plate_board_test.sh root@<board>:/opt/rv1126b_desktop/
scp models/plate_det.rknn root@<board>:/opt/rv1126b_desktop/models/

# 板端
chmod +x /opt/rv1126b_desktop/plate_board_test.sh
/opt/rv1126b_desktop/plate_board_test.sh -i /tmp/plate.jpg
/opt/rv1126b_desktop/plate_board_test.sh --all ./images
```

正常日志应含 `class_num=1`；检出成功会保存 `*_plate_test.jpg`。

| 文件 | 职责 |
|------|------|
| `plate_algo.cc/.h` | 对外 C 接口 + worker 线程 |
| `yolov8_plate.cc/.h` | RKNN 加载、letterbox 推理 |
| `postprocess.cc/.h` | NMS 后处理：9 路 DFL（rknn_model_zoo）或 1 路融合头（Ultralytics ONNX） |
| `lprnet.cc/.h` | LPRNet 识别（保留，当前未调用） |
| `plate_test.cc` | 板端离线测试 main |
| `common.h` | `image_buffer_t` 等 |
