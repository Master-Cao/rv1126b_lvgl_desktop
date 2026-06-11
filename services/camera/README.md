# camera

相机采集与帧解析模块。

| 文件 | 职责 |
|------|------|
| `camera_types.h` | 帧结构、像素格式枚举 |
| `camera_service.c/.h` | 打开设备、启停、取帧 |
| `camera_parser.c/.h` | NV12/RGB 转换、缩放，供预览与推理使用 |

默认 `camera_service` 为占位实现，后续可在此文件内接入 V4L2 / MPI。
