# hair

火锅底料/油脂表面"发丝级细暗线缺陷"检测，纯 OpenCV 实现。

## 判定逻辑

> 头发丝 = **连续的、黑色的、细长的**线。
>
> 火锅底料一般是橙红色块，褶皱/油泡/辣椒籽缝隙通常是**短的弧/点 + 偏暖色**。
> 三条联合判定足以滤掉绝大部分非头发干扰。

## 处理管线

1. **颜色先验 → "黑色"蒙版**：`dark_mask = (max(R,G,B) < dark_v_threshold)`。
   - 橙红 (例 R=220,G=80,B=40)：max=220 → 排除
   - 高光 (例 R=240,G=240,B=235)：max=240 → 排除
   - 黑发 (例 R=20,G=18,B=22)：max=22 → 命中
2. **灰度 + 大尺度高斯背景估计**，`src = min(gray, bg)` 把高光截到 bg 上限，
   防止油亮高光给后续 Otsu 拉偏。
3. **多角度 Black-hat**：以 6 个方向（0/30/60/90/120/150°）的 1 像素粗线
   作为结构元做形态学 Black-hat，逐像素取最大值，强化任意倾斜的"暗细线"响应。
4. **Otsu 二值化** Black-hat 响应 → `bh_bin`。
5. **`bin = bh_bin ∧ dark_mask`** —— 求交。这一步把"在橙红色区域产生的伪响应"
   彻底排除（褶皱/光斑），只保留"真正黑且窄"的像素。
6. 小尺度闭运算 (3×3) 连接因塑料封膜导致断裂的发丝小段。
7. `findContours` 取候选轮廓，对每个候选做三重校验：
   - **几何**：`minAreaRect` 主轴 ≥ `min_length_px`，次轴 ≤ `max_width_px`，长宽比 ≥ `min_aspect`。
   - **整体黑色**：候选区域 (bbox ∩ contour) 内黑色像素占比 ≥ `min_dark_ratio`。
   - **主轴连续性**：沿候选主轴中线均匀采样 N 点，**最长连续黑色段 / N ≥ `min_continuous_ratio`**。
8. 输出 `minAreaRect` 的 4 个角点（与 `camera_preview` 多边形渲染对齐）。

## 关键参数（默认值，可通过 `hair_algo_params_t` 调整）

| 参数 | 默认 | 说明 |
|---|---|---|
| `blur_ksize` | 51 | 背景估计高斯核（奇数） |
| `blackhat_length` | 15 | 线形结构元长度（像素） |
| `min_length_px` | 40 | 候选最小长度（褶皱通常更短） |
| `max_width_px` | 6 | 候选最大宽度 |
| `min_aspect` | 6.0 | 主/次轴最小长宽比 |
| `min_score` | 0.30 | 综合得分阈值（0..1） |
| `dark_v_threshold` | 90 | 黑色判定：`max(R,G,B) < ` 此值 |
| `min_dark_ratio` | 0.45 | bbox 内黑色占比下限 |
| `min_continuous_ratio` | 0.55 | 主轴方向最长连续黑色段 / 总采样点 |

综合得分 `score = 0.4·resp + 0.3·dark_ratio + 0.3·continuity`。

## 接口

见 [`hair_algo.h`](./hair_algo.h)。worker 线程异步推理，
`submit_rgb888` + `get_result` 与 `ocr_algo` 完全一致。

## 调试参数指南

- **误检多** → 调大 `dark_v_threshold` 反而会更宽松，正确做法是：调大 `min_aspect`(→8.0)，
  调大 `min_continuous_ratio`(→0.7)，或调大 `min_length_px`(→60)。
- **漏检** → 调大 `dark_v_threshold`(→110)，调小 `min_dark_ratio`(→0.30)，
  调小 `blackhat_length`(→11)，调小 `min_length_px`(→25)。
- **塑料封膜反光仍干扰** → 优先在硬件加暗场低角度光 + 正交偏振，
  软件侧可调大 `blur_ksize`(→81)。

## 后续演进建议

- 第二版：在候选 ROI 上叠加轻量 CNN 二分类（RKNN int8），抑制塑料褶皱误检。
- 第三版：累计标注数据后训练 U-Net 直接做发丝分割（量化后部署到 NPU）。
- 光机推荐：暗场低角度环形光 + 正交偏振滤片。
