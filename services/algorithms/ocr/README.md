# ocr

PP-OCR 算法封装，对接 rknn_ppocr 检测 + 识别模型。

在 `ocr_algo.c` 中实现：

- 模型加载（`.rknn` 路径来自 `ocr_algo_create` 或环境变量）
- `process_frame` 内调用推理并填充 `algo_result_t.text`
