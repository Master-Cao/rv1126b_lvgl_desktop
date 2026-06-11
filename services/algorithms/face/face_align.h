#ifndef FACE_ALIGN_H
#define FACE_ALIGN_H

/*
 * 根据 SCRFD 5 关键点仿射对齐到 112x112（InsightFace 标准模板）。
 */
#include "face_algo.h"

#include <opencv2/core.hpp>

#ifdef __cplusplus
extern "C" {
#endif

/* src_rgb: RGB888 原图；landmarks: 5 点；返回 112x112 RGB 对齐图，失败返回空 Mat。 */
cv::Mat face_align_warp_rgb(const cv::Mat &src_rgb, const face_point_t landmarks[FACE_LANDMARK_NUM]);

#ifdef __cplusplus
}
#endif

#endif /* FACE_ALIGN_H */
