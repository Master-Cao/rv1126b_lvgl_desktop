# services

桌面 UI（`lvgl_desktop/`）与底层能力之间的业务层。

```text
services/
├── camera/          # 相机采集、帧解析（V4L2 / RGA 等）
└── algorithms/      # 各 RKNN 算法封装
    ├── ocr/
    ├── detect/
    └── hair/
```

约定：

- **UI 组件**（预览、按钮）仍在 `lvgl_desktop/components/`
- **相机与算法逻辑**放在本目录，由 `apps/*_app.c` 调用
- 模型路径、设备节点建议通过环境变量或后续 `config/` 传入，不在 UI 里写死

集成步骤（OCR 示例）：

1. 在 `camera/camera_service.c` 实现真实 V4L2 采集
2. 在 `algorithms/ocr/ocr_algo.c` 接入 rknn_ppocr
3. 在 `ocr_app.c` 的启动/停止回调里调用 `camera_service_*` 与 `ocr_algo_*`
