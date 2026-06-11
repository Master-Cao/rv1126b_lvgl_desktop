#include "face_align.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

static const cv::Point2f kArcFaceDst[5] = {
    cv::Point2f(38.2946f, 51.6963f),
    cv::Point2f(73.5318f, 51.5014f),
    cv::Point2f(56.0252f, 71.7366f),
    cv::Point2f(41.5493f, 92.3655f),
    cv::Point2f(70.7299f, 92.2041f),
};

/*
 * 相似变换（partial affine）：x' = a*x - b*y + tx, y' = b*x + a*y + ty
 * 仅用 opencv_core 的 solve，不依赖 calib3d 的 estimateAffinePartial2D。
 */
static bool estimate_similarity_transform(const cv::Point2f *src, const cv::Point2f *dst, int n,
                                          cv::Mat &out_2x3)
{
    if (!src || !dst || n < 2) {
        return false;
    }

    cv::Mat A(2 * n, 4, CV_64F);
    cv::Mat b(2 * n, 1, CV_64F);
    for (int i = 0; i < n; i++) {
        double x = src[i].x;
        double y = src[i].y;
        double u = dst[i].x;
        double v = dst[i].y;

        A.at<double>(2 * i, 0) = x;
        A.at<double>(2 * i, 1) = -y;
        A.at<double>(2 * i, 2) = 1.0;
        A.at<double>(2 * i, 3) = 0.0;
        b.at<double>(2 * i, 0) = u;

        A.at<double>(2 * i + 1, 0) = y;
        A.at<double>(2 * i + 1, 1) = x;
        A.at<double>(2 * i + 1, 2) = 0.0;
        A.at<double>(2 * i + 1, 3) = 1.0;
        b.at<double>(2 * i + 1, 0) = v;
    }

    cv::Mat params;
    if (!cv::solve(A, b, params, cv::DECOMP_SVD)) {
        return false;
    }
    if (params.rows != 4 || params.cols != 1) {
        return false;
    }

    double a = params.at<double>(0, 0);
    double bb = params.at<double>(1, 0);
    double tx = params.at<double>(2, 0);
    double ty = params.at<double>(3, 0);
    out_2x3 = (cv::Mat_<float>(2, 3) << (float)a, (float)(-bb), (float)tx, (float)bb, (float)a,
               (float)ty);
    return true;
}

cv::Mat face_align_warp_rgb(const cv::Mat &src_rgb, const face_point_t landmarks[FACE_LANDMARK_NUM])
{
    if (src_rgb.empty() || !landmarks) {
        return cv::Mat();
    }

    cv::Point2f src_pts[5];
    for (int i = 0; i < FACE_LANDMARK_NUM; i++) {
        src_pts[i] = cv::Point2f(landmarks[i].x, landmarks[i].y);
    }

    cv::Mat M;
    if (!estimate_similarity_transform(src_pts, kArcFaceDst, FACE_LANDMARK_NUM, M)) {
        return cv::Mat();
    }

    cv::Mat aligned;
    cv::warpAffine(src_rgb, aligned, M, cv::Size(FACE_ALIGN_SIZE, FACE_ALIGN_SIZE),
                   cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
    return aligned;
}
