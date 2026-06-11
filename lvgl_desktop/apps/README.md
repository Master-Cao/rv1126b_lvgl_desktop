# apps

这里放每个算法应用页面。

| 文件 | 说明 |
|------|------|
| `ocr_app.c/.h` | PP-OCR 文字识别 |
| `detect_app.c/.h` | YOLOv8 目标检测（COCO 80 类） |
| `face_app.c/.h` | 人脸检测 + 识别 |
| `plate_app.c/.h` | 车牌检测 + 号牌识别 |
| `seg_app.c/.h` | 目标实例分割 |
| `hair_app.c/.h` | 头发丝检测（OpenCV） |

应用通过 `lvgl_app_ctx.h` 接收字体与返回回调，由 `lvgl_desktop.c` 统一调度显示。
三个 app 的整体框架完全一致：左侧相机预览 + 右上结果面板 + 右下工具栏，
仅算法实例和结果格式化逻辑不同。

算法与相机逻辑放在工程根目录 `services/`，本目录只负责 UI 与按钮回调。
