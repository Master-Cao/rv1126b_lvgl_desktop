# models

板端运行所需的 RKNN 模型集中放在这里。`scripts/install_board.sh` 默认从本目录
拷到 `/opt/rv1126b_desktop/models/`。

| 文件 | 用途 | 默认路径 |
|------|------|---------|
| `ppocrv4_det.rknn` | PP-OCR 文字检测 | `/opt/rv1126b_desktop/models/ppocrv4_det.rknn` |
| `ppocrv4_rec.rknn` | PP-OCR 文字识别 | `/opt/rv1126b_desktop/models/ppocrv4_rec.rknn` |
| `yolov8.rknn` | YOLOv8 目标检测（COCO 80 类） | `/opt/rv1126b_desktop/models/yolov8.rknn` |
| `face_det.rknn` | 人脸检测（SCRFD，bbox + 5 关键点） | `/opt/rv1126b_desktop/models/face_det.rknn` |
| `face_rec.rknn` | 人脸识别特征提取（待接入） | `/opt/rv1126b_desktop/models/face_rec.rknn` |
| `plate_det.rknn` | 车牌检测 | `/opt/rv1126b_desktop/models/plate_det.rknn` |
| `plate.rknn` | 车牌检测（备用文件名） | `/opt/rv1126b_desktop/models/plate.rknn` |
| `plate_rec.rknn` | 车牌字符识别（LPRNet） | `/opt/rv1126b_desktop/models/plate_rec.rknn` |
| `yolov8_seg.rknn` | 目标实例分割 | `/opt/rv1126b_desktop/models/yolov8_seg.rknn` |

## 环境变量覆盖

如果不想用默认路径，可在板端启动前导出环境变量：

```sh
export OCR_DET_MODEL=/data/models/ppocrv4_det.rknn
export OCR_REC_MODEL=/data/models/ppocrv4_rec.rknn
export DETECT_MODEL=/data/models/yolov8.rknn
export FACE_DET_MODEL=/data/models/face_det.rknn
export FACE_REC_MODEL=/data/models/face_rec.rknn
export FACE_GALLERY=/data/models/face_gallery
export PLATE_DET_MODEL=/data/models/plate_det.rknn
export PLATE_REC_MODEL=/data/models/plate_rec.rknn
export SEG_MODEL=/data/models/yolov8_seg.rknn
```

`*_algo_create()` 内部按 “入参 → 环境变量 → 默认路径” 的顺序回退。

## COCO 标签

YOLOv8 的 80 类标签已经被静态嵌入到 `services/algorithms/detect/postprocess.cc`
（`kCocoLabels[]`），板端不再需要 `coco_80_labels_list.txt` 文件。
