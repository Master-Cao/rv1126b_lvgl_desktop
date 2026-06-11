# face

人脸检测 + 识别模块（SCRFD + MobileFaceNet + 底库比对）。

## 管线

1. **检测**（SCRFD）：`face_det.rknn` → bbox + 5 关键点（`scrfd.cc`）
2. **对齐**：5 点仿射 → `112×112`（`face_align.cc`，InsightFace 标准模板）
3. **识别**：`face_rec.rknn`（w600k_mbf）→ 512 维 L2 归一化特征（`face_rec.cc`）
4. **比对**：与底库 `face_gallery` 余弦 1:N，超过 `match_threshold`（默认 0.5 / 50%）输出姓名

## 接口

与 `detect` / `hair` 一致：

```c
face_algo_t *face_algo_create(const face_algo_config_t *config);
int face_algo_start(face_algo_t *algo);
int face_algo_stop(face_algo_t *algo);
int face_algo_submit_rgb888(face_algo_t *algo, const uint8_t *rgb, int w, int h);
int face_algo_get_result(face_algo_t *algo, face_result_set_t *out);
int face_algo_process_frame(face_algo_t *algo, const camera_frame_t *, algo_result_cb_t, void *);
void face_algo_destroy(face_algo_t *algo);
```

`face_algo_config_t` 可传 `NULL`，使用默认路径与环境变量。

## 模型与底库

| 环境变量 | 默认路径 |
|----------|----------|
| `FACE_DET_MODEL` | `/opt/rv1126b_desktop/models/face_det.rknn` |
| `FACE_REC_MODEL` | `/opt/rv1126b_desktop/models/face_rec.rknn` |
| `FACE_GALLERY` | `/opt/rv1126b_desktop/models/face_gallery` |

底库目录需包含：

- `face_encoding.bin` — PC 端 `enroll.py` 导出（magic `FGAL`）
- `gallery.txt` — 每行 `id,name`

## PC 端录入与部署

```bash
# 1. 录入底库（每人一个子文件夹）
cd faceNet/python
python3 enroll.py --input-dir ../enroll_data

# 2. 拷贝到板端
scp ../model/gallery/face_encoding.bin board:/opt/rv1126b_desktop/models/face_gallery/
scp ../model/gallery/gallery.txt       board:/opt/rv1126b_desktop/models/face_gallery/
scp ../model/face_rec.rknn             board:/opt/rv1126b_desktop/models/
scp ../model/face_det.rknn               board:/opt/rv1126b_desktop/models/
```

## 文件

| 文件 | 职责 |
|------|------|
| `face_algo.cc/.h` | 对外 C 接口 + worker 线程，串联检测/对齐/识别/比对 |
| `scrfd.cc/.h` | SCRFD RKNN 推理 + 后处理 |
| `face_align.cc/.h` | 5 点对齐 112×112 |
| `face_rec.cc/.h` | MobileFaceNet RKNN 特征提取 |
| `face_gallery.cc/.h` | 底库加载 + 1:N 余弦检索 |
