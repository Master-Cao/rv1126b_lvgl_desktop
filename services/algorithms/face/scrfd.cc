/*
 * SCRFD 人脸检测（移植自 scrfd/main.py，改为 C + RKNN + OpenCV）。
 *
 * 处理流程（对齐 main.py 的 SCRFD.detect）：
 *   1. resize_image：keep_ratio 居中 letterbox 到 640x640（填充 0）
 *   2. rknn_inputs_set + rknn_run + rknn_outputs_get（9 个输出）
 *   3. 反量化各输出（按 tensor type，支持 INT8/UINT8/FP16/FP32）
 *   4. 按 stride(8/16/32) 解码 anchor：distance2bbox / distance2kps
 *   5. 还原到原图坐标 + NMS
 *
 * 所有大缓冲区在 init 时分配到堆上，避免 worker 线程栈溢出。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include <opencv2/opencv.hpp>

#include "scrfd.h"

#define SCRFD_CONF_THRESHOLD 0.5f
#define SCRFD_NMS_THRESHOLD  0.5f
#define SCRFD_NUM_ANCHORS    2

static const int SCRFD_STRIDES[SCRFD_FPN_NUM] = {8, 16, 32};

typedef struct {
    float score;
    int idx;
} scrfd_sort_item_t;

static int read_data_from_file(const char *path, char **out_data)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
        fclose(fp);
        return -1;
    }
    char *buf = (char *)malloc((size_t)size);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *out_data = buf;
    return (int)size;
}

static int clamp_int(int x, int lo, int hi)
{
    if (x > hi) return hi;
    if (x < lo) return lo;
    return x;
}

static float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale)
{
    return ((float)qnt - (float)zp) * scale;
}

/* 是否对该输出请求 RKNN 直接转 float32（对齐 yolov8：非 INT8 量化输出都让 RKNN 转好）。 */
static int output_want_float(const rknn_tensor_attr *attr)
{
    if (attr->type == RKNN_TENSOR_INT8 && attr->qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) {
        return 0;
    }
    return 1;
}

/* 按 want_float 取出结果：want_float 时 buf 已是 float32，直接拷贝；否则按 INT8 仿射反量化。
 * 注意：FP16 等非量化输出必须用 want_float=1，否则 RKNN 返回 NPU 原生 tiled 布局，
 * 按 n_elems 读取会越界。 */
static void fetch_output_to_f32(const rknn_output *output, const rknn_tensor_attr *attr,
                                int want_float, float *dst)
{
    int n = (int)attr->n_elems;
    if (!dst || n <= 0 || !output->buf) {
        return;
    }
    if (want_float) {
        memcpy(dst, output->buf, (size_t)n * sizeof(float));
        return;
    }
    const int8_t *p = (const int8_t *)output->buf;
    for (int i = 0; i < n; i++) {
        dst[i] = deqnt_affine_to_f32(p[i], attr->zp, attr->scale);
    }
}

static void quantize_u8_to_i8(const uint8_t *src, int8_t *dst, int n, int32_t zp, float scale)
{
    float inv = (scale > 1e-8f) ? (1.0f / scale) : 1.0f;
    for (int i = 0; i < n; i++) {
        int v = (int)roundf((float)src[i] * inv + (float)zp);
        if (v > 127) v = 127;
        if (v < -128) v = -128;
        dst[i] = (int8_t)v;
    }
}

static void dump_tensor_attr(rknn_tensor_attr *attr)
{
    printf("  index=%d, name=%s, n_dims=%d, dims=[%d, %d, %d, %d], n_elems=%d, size=%d, fmt=%s, "
           "type=%s, qnt_type=%s, zp=%d, scale=%f\n",
           attr->index, attr->name, attr->n_dims, attr->dims[0], attr->dims[1], attr->dims[2],
           attr->dims[3], attr->n_elems, attr->size, get_format_string(attr->fmt),
           get_type_string(attr->type), get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
    fflush(stdout);
}

static void log_model_file_info(const char *tag, const char *path)
{
    if (!path || !path[0]) {
        printf("%s: path is empty\n", tag);
        fflush(stdout);
        return;
    }
    if (access(path, R_OK) != 0) {
        printf("%s: file not readable or missing: %s (errno=%d)\n", tag, path, errno);
        fflush(stdout);
        return;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        printf("%s: stat fail: %s (errno=%d)\n", tag, path, errno);
        fflush(stdout);
        return;
    }
    printf("%s: file ok path=%s size=%lld bytes\n", tag, path, (long long)st.st_size);
    fflush(stdout);
}

static void log_scrfd_output_hint(int n_output)
{
    printf("scrfd: ---- model diagnosis ----\n");
    printf("scrfd: this code expects SCRFD-500M (scrfd_500m_bnkps.onnx) with %d outputs\n",
           SCRFD_OUTPUT_NUM);
    printf("scrfd: layout: score/bbox/kps x 3 strides (8/16/32)\n");
    if (n_output == 3) {
        printf("scrfd: got output=3 -> likely wrong RKNN or merged outputs during convert\n");
        printf("scrfd: re-convert scrfd_500m_bnkps.onnx and keep all 9 separate outputs\n");
    } else if (n_output == 15) {
        printf("scrfd: got output=15 -> scrfd_2.5g / 5-stride model, not compatible\n");
        printf("scrfd: use scrfd_500m_bnkps.onnx instead\n");
    } else {
        printf("scrfd: got output=%d -> unknown SCRFD variant for this post-process\n", n_output);
    }
    printf("scrfd: -------------------------\n");
    fflush(stdout);
}

/* keep_ratio 居中 letterbox（对齐 main.py.resize_image，填充 0），输出 BGR 供模型使用。 */
static int convert_image_letterbox(const cv::Mat &src_rgb, unsigned char *dst, int dst_w, int dst_h,
                                   float *out_scale, int *out_padx, int *out_pady)
{
    int sw = src_rgb.cols;
    int sh = src_rgb.rows;
    if (sw <= 0 || sh <= 0) {
        return -1;
    }
    float scale_w = (float)dst_w / (float)sw;
    float scale_h = (float)dst_h / (float)sh;
    float scale = scale_w < scale_h ? scale_w : scale_h;
    int rw = (int)((float)sw * scale);
    int rh = (int)((float)sh * scale);
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    int padx = (dst_w - rw) / 2;
    int pady = (dst_h - rh) / 2;

    cv::Mat dst_mat(dst_h, dst_w, CV_8UC3, dst);
    dst_mat.setTo(cv::Scalar(0, 0, 0));
    cv::Mat roi = dst_mat(cv::Rect(padx, pady, rw, rh));
    cv::resize(src_rgb, roi, cv::Size(rw, rh), 0, 0, cv::INTER_AREA);
    /* main.py 直接喂 cv2 的 BGR 图，这里把 RGB 转成 BGR 对齐模型期望的通道顺序。 */
    cv::cvtColor(dst_mat, dst_mat, cv::COLOR_RGB2BGR);

    *out_scale = scale;
    *out_padx = padx;
    *out_pady = pady;
    return 0;
}

/* 按 dims 判断输出语义：含 10→关键点，含 4→bbox，否则为 score。 */
static int output_channel(const rknn_tensor_attr *attr)
{
    for (uint32_t i = 0; i < attr->n_dims; i++) {
        if (attr->dims[i] == 10) return 10;
    }
    for (uint32_t i = 0; i < attr->n_dims; i++) {
        if (attr->dims[i] == 4) return 4;
    }
    return 1;
}

static int stride_index_from_anchors(int num_anchors)
{
    for (int s = 0; s < SCRFD_FPN_NUM; s++) {
        int feat = 640 / SCRFD_STRIDES[s];
        if (num_anchors == feat * feat * SCRFD_NUM_ANCHORS) {
            return s;
        }
    }
    return -1;
}

static int classify_output_role(const rknn_tensor_attr *attr)
{
    int ch = output_channel(attr);
    if (ch == 10) {
        return 2; /* kps */
    }
    if (ch == 4) {
        return 1; /* bbox */
    }
    return 0; /* score */
}

static const char *output_role_name(int role)
{
    switch (role) {
    case 1:
        return "bbox";
    case 2:
        return "kps";
    default:
        return "score";
    }
}

static int cmp_score_desc(const void *a, const void *b)
{
    float sa = ((const scrfd_sort_item_t *)a)->score;
    float sb = ((const scrfd_sort_item_t *)b)->score;
    if (sa > sb) return -1;
    if (sa < sb) return 1;
    return 0;
}

static float box_iou(const float *a, const float *b)
{
    /* box: x, y, w, h */
    float ax2 = a[0] + a[2];
    float ay2 = a[1] + a[3];
    float bx2 = b[0] + b[2];
    float by2 = b[1] + b[3];
    float ix1 = a[0] > b[0] ? a[0] : b[0];
    float iy1 = a[1] > b[1] ? a[1] : b[1];
    float ix2 = ax2 < bx2 ? ax2 : bx2;
    float iy2 = ay2 < by2 ? ay2 : by2;
    float iw = ix2 - ix1;
    float ih = iy2 - iy1;
    if (iw <= 0.f || ih <= 0.f) {
        return 0.f;
    }
    float inter = iw * ih;
    float uni = a[2] * a[3] + b[2] * b[3] - inter;
    return uni <= 0.f ? 0.f : inter / uni;
}

static void free_workspace(scrfd_context_t *ctx)
{
    if (!ctx) return;
    free(ctx->input_u8);
    free(ctx->input_i8);
    for (int i = 0; i < SCRFD_OUTPUT_NUM; i++) {
        free(ctx->deq[i]);
        ctx->deq[i] = NULL;
    }
    free(ctx->cand_box);
    free(ctx->cand_score);
    free(ctx->cand_kps);
    free(ctx->cand_order);
    free(ctx->cand_keep);
    free(ctx->infer_result);
    ctx->input_u8 = NULL;
    ctx->input_i8 = NULL;
    ctx->cand_box = NULL;
    ctx->cand_score = NULL;
    ctx->cand_kps = NULL;
    ctx->cand_order = NULL;
    ctx->cand_keep = NULL;
    ctx->infer_result = NULL;
}

static int alloc_workspace(scrfd_context_t *ctx)
{
    ctx->input_bytes = ctx->model_width * ctx->model_height * ctx->model_channel;
    ctx->total_anchors = 0;
    for (int s = 0; s < SCRFD_FPN_NUM; s++) {
        int feat = ctx->model_width / SCRFD_STRIDES[s];
        ctx->total_anchors += feat * feat * SCRFD_NUM_ANCHORS;
    }

    ctx->input_u8 = (unsigned char *)malloc((size_t)ctx->input_bytes);
    ctx->input_i8 = (int8_t *)malloc((size_t)ctx->input_bytes);
    for (uint32_t i = 0; i < ctx->io_num.n_output && i < SCRFD_OUTPUT_NUM; i++) {
        ctx->deq[i] = (float *)malloc((size_t)ctx->output_attrs[i].n_elems * sizeof(float));
        if (!ctx->deq[i]) {
            free_workspace(ctx);
            return -1;
        }
    }
    ctx->cand_box = (float *)malloc((size_t)ctx->total_anchors * 4 * sizeof(float));
    ctx->cand_score = (float *)malloc((size_t)ctx->total_anchors * sizeof(float));
    ctx->cand_kps = (float *)malloc((size_t)ctx->total_anchors * 10 * sizeof(float));
    ctx->cand_order = (int *)malloc((size_t)ctx->total_anchors * sizeof(int));
    ctx->cand_keep = (int *)malloc((size_t)ctx->total_anchors * sizeof(int));
    ctx->infer_result = (scrfd_result *)malloc(sizeof(scrfd_result));

    if (!ctx->input_u8 || !ctx->input_i8 || !ctx->cand_box || !ctx->cand_score || !ctx->cand_kps ||
        !ctx->cand_order || !ctx->cand_keep || !ctx->infer_result) {
        free_workspace(ctx);
        return -1;
    }
    return 0;
}

extern "C" int init_scrfd_model(const char *model_path, scrfd_context_t *app_ctx)
{
    if (!app_ctx) return -1;
    memset(app_ctx, 0, sizeof(*app_ctx));

    printf("scrfd: ===== init begin =====\n");
    printf("scrfd: loading model: %s\n", model_path ? model_path : "(null)");
    log_model_file_info("scrfd", model_path);
    fflush(stdout);

    char *model = NULL;
    int model_len = read_data_from_file(model_path, &model);
    if (!model || model_len <= 0) {
        printf("scrfd: load_model fail: %s (read bytes=%d)\n", model_path, model_len);
        fflush(stdout);
        return -1;
    }
    printf("scrfd: read rknn blob ok, size=%d bytes\n", model_len);
    fflush(stdout);

    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, model, model_len, 0, NULL);
    free(model);
    if (ret < 0) {
        printf("scrfd: rknn_init fail ret=%d path=%s\n", ret, model_path);
        fflush(stdout);
        return -1;
    }
    printf("scrfd: rknn_init ok\n");
    fflush(stdout);

    rknn_sdk_version sdk_ver;
    memset(&sdk_ver, 0, sizeof(sdk_ver));
    if (rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &sdk_ver, sizeof(sdk_ver)) == RKNN_SUCC) {
        printf("scrfd: rknn api=%s driver=%s\n", sdk_ver.api_version, sdk_ver.drv_version);
    }

    rknn_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) {
        printf("scrfd: rknn_query IN_OUT_NUM fail ret=%d\n", ret);
        rknn_destroy(ctx);
        fflush(stdout);
        return -1;
    }
    printf("scrfd: io num input=%d output=%d (expect output=%d)\n", io_num.n_input, io_num.n_output,
           SCRFD_OUTPUT_NUM);
    fflush(stdout);

    rknn_tensor_attr *input_attrs = NULL;
    rknn_tensor_attr *output_attrs = NULL;
    if (io_num.n_input > 0) {
        input_attrs = (rknn_tensor_attr *)calloc(io_num.n_input, sizeof(rknn_tensor_attr));
    }
    if (io_num.n_output > 0) {
        output_attrs = (rknn_tensor_attr *)calloc(io_num.n_output, sizeof(rknn_tensor_attr));
    }
    if ((io_num.n_input > 0 && !input_attrs) || (io_num.n_output > 0 && !output_attrs)) {
        printf("scrfd: alloc tensor attrs fail\n");
        free(input_attrs);
        free(output_attrs);
        rknn_destroy(ctx);
        fflush(stdout);
        return -1;
    }

    printf("scrfd: ---- input tensors ----\n");
    for (uint32_t i = 0; i < io_num.n_input; i++) {
        input_attrs[i].index = i;
        rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &input_attrs[i], sizeof(rknn_tensor_attr));
        dump_tensor_attr(&input_attrs[i]);
    }
    printf("scrfd: ---- output tensors ----\n");
    for (uint32_t i = 0; i < io_num.n_output; i++) {
        output_attrs[i].index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &output_attrs[i], sizeof(rknn_tensor_attr));
        dump_tensor_attr(&output_attrs[i]);
        int role = classify_output_role(&output_attrs[i]);
        int ch = output_channel(&output_attrs[i]);
        int num = output_attrs[i].n_elems > 0 && ch > 0 ? (int)output_attrs[i].n_elems / ch : 0;
        int si = stride_index_from_anchors(num);
        printf("  -> role=%s ch=%d anchors=%d stride_idx=%d\n", output_role_name(role), ch, num,
               si);
    }
    fflush(stdout);

    if (io_num.n_output != SCRFD_OUTPUT_NUM) {
        log_scrfd_output_hint((int)io_num.n_output);
        free(input_attrs);
        free(output_attrs);
        rknn_destroy(ctx);
        printf("scrfd: init fail: output count mismatch\n");
        printf("scrfd: ===== init end (fail) =====\n");
        fflush(stdout);
        return -1;
    }

    app_ctx->input_attrs = input_attrs;
    app_ctx->output_attrs = output_attrs;
    app_ctx->rknn_ctx = ctx;
    app_ctx->io_num = io_num;

    rknn_tensor_attr *in0 = &app_ctx->input_attrs[0];
    if (in0->fmt == RKNN_TENSOR_NCHW) {
        app_ctx->model_channel = in0->dims[1];
        app_ctx->model_height = in0->dims[2];
        app_ctx->model_width = in0->dims[3];
    } else {
        app_ctx->model_height = in0->dims[1];
        app_ctx->model_width = in0->dims[2];
        app_ctx->model_channel = in0->dims[3];
    }
    app_ctx->input_is_int8 = (in0->type == RKNN_TENSOR_INT8);

    if (app_ctx->model_width != 640 || app_ctx->model_height != 640) {
        printf("scrfd: unsupported model size %dx%d (expect 640x640)\n", app_ctx->model_width,
               app_ctx->model_height);
        release_scrfd_model(app_ctx);
        printf("scrfd: ===== init end (fail) =====\n");
        fflush(stdout);
        return -1;
    }

    printf("scrfd model: %dx%d ch=%d input_int8=%d\n", app_ctx->model_width, app_ctx->model_height,
           app_ctx->model_channel, app_ctx->input_is_int8);

    if (alloc_workspace(app_ctx) != 0) {
        printf("scrfd: alloc workspace fail\n");
        release_scrfd_model(app_ctx);
        printf("scrfd: ===== init end (fail) =====\n");
        fflush(stdout);
        return -1;
    }

    printf("scrfd: init ok total_anchors=%d\n", app_ctx->total_anchors);
    printf("scrfd: ===== init end (ok) =====\n");
    fflush(stdout);
    return 0;
}

extern "C" int release_scrfd_model(scrfd_context_t *app_ctx)
{
    if (!app_ctx) return 0;
    free_workspace(app_ctx);
    free(app_ctx->input_attrs);
    free(app_ctx->output_attrs);
    app_ctx->input_attrs = NULL;
    app_ctx->output_attrs = NULL;
    if (app_ctx->rknn_ctx != 0) {
        rknn_destroy(app_ctx->rknn_ctx);
        app_ctx->rknn_ctx = 0;
    }
    return 0;
}

static int post_process_scrfd(scrfd_context_t *ctx, int src_w, int src_h, float scale, int padx,
                              int pady, scrfd_result *result)
{
    /* 按语义把 9 个输出归类到各 stride 的 score / bbox / kps（对齐 main.py 的输出重排）。 */
    float *score_buf[SCRFD_FPN_NUM] = {NULL};
    float *bbox_buf[SCRFD_FPN_NUM] = {NULL};
    float *kps_buf[SCRFD_FPN_NUM] = {NULL};

    for (uint32_t i = 0; i < ctx->io_num.n_output; i++) {
        int ch = output_channel(&ctx->output_attrs[i]);
        int num = (int)ctx->output_attrs[i].n_elems / ch;
        int si = stride_index_from_anchors(num);
        if (si < 0) {
            continue;
        }
        if (ch == 1) {
            score_buf[si] = ctx->deq[i];
        } else if (ch == 4) {
            bbox_buf[si] = ctx->deq[i];
        } else if (ch == 10) {
            kps_buf[si] = ctx->deq[i];
        }
    }

    float inv = (scale > 1e-6f) ? (1.0f / scale) : 1.0f;
    int cand = 0;

    for (int s = 0; s < SCRFD_FPN_NUM; s++) {
        if (!score_buf[s] || !bbox_buf[s] || !kps_buf[s]) {
            printf("scrfd: missing output for stride %d\n", SCRFD_STRIDES[s]);
            continue;
        }
        int stride = SCRFD_STRIDES[s];
        int feat = ctx->model_width / stride;
        const float *scores = score_buf[s];
        const float *bbox = bbox_buf[s];
        const float *kps = kps_buf[s];

        int k = 0;
        for (int y = 0; y < feat; y++) {
            for (int x = 0; x < feat; x++) {
                for (int a = 0; a < SCRFD_NUM_ANCHORS; a++, k++) {
                    float sc = scores[k];
                    if (sc < SCRFD_CONF_THRESHOLD) {
                        continue;
                    }
                    float cx = (float)(x * stride);
                    float cy = (float)(y * stride);

                    float d0 = bbox[k * 4 + 0] * stride;
                    float d1 = bbox[k * 4 + 1] * stride;
                    float d2 = bbox[k * 4 + 2] * stride;
                    float d3 = bbox[k * 4 + 3] * stride;
                    float x1 = cx - d0;
                    float y1 = cy - d1;
                    float x2 = cx + d2;
                    float y2 = cy + d3;
                    /* 还原到原图：先去 pad 再除以缩放比例（对齐 main.py 的 ratiow/ratioh）。 */
                    float bx = (x1 - padx) * inv;
                    float by = (y1 - pady) * inv;
                    float bw = (x2 - x1) * inv;
                    float bh = (y2 - y1) * inv;

                    if (cand >= ctx->total_anchors) {
                        break;
                    }
                    ctx->cand_box[cand * 4 + 0] = bx;
                    ctx->cand_box[cand * 4 + 1] = by;
                    ctx->cand_box[cand * 4 + 2] = bw;
                    ctx->cand_box[cand * 4 + 3] = bh;
                    ctx->cand_score[cand] = sc;
                    for (int j = 0; j < SCRFD_KPS_NUM; j++) {
                        float px = cx + kps[k * 10 + 2 * j] * stride;
                        float py = cy + kps[k * 10 + 2 * j + 1] * stride;
                        ctx->cand_kps[cand * 10 + 2 * j] = (px - padx) * inv;
                        ctx->cand_kps[cand * 10 + 2 * j + 1] = (py - pady) * inv;
                    }
                    cand++;
                }
            }
        }
    }

    /* 按分数排序 + 贪心 NMS。 */
    scrfd_sort_item_t *items =
        (scrfd_sort_item_t *)malloc((size_t)(cand > 0 ? cand : 1) * sizeof(scrfd_sort_item_t));
    if (!items) {
        result->count = 0;
        return -1;
    }
    for (int i = 0; i < cand; i++) {
        items[i].score = ctx->cand_score[i];
        items[i].idx = i;
        ctx->cand_keep[i] = 1;
    }
    qsort(items, (size_t)cand, sizeof(scrfd_sort_item_t), cmp_score_desc);

    int out_n = 0;
    for (int i = 0; i < cand; i++) {
        int ni = items[i].idx;
        if (!ctx->cand_keep[ni]) {
            continue;
        }
        if (out_n < SCRFD_MAX_RESULTS) {
            const float *b = &ctx->cand_box[ni * 4];
            scrfd_object_t *o = &result->object[out_n];
            o->left = clamp_int((int)b[0], 0, src_w);
            o->top = clamp_int((int)b[1], 0, src_h);
            o->right = clamp_int((int)(b[0] + b[2]), 0, src_w);
            o->bottom = clamp_int((int)(b[1] + b[3]), 0, src_h);
            o->score = ctx->cand_score[ni];
            for (int j = 0; j < SCRFD_KPS_NUM; j++) {
                o->point[j].x = clamp_int((int)ctx->cand_kps[ni * 10 + 2 * j], 0, src_w);
                o->point[j].y = clamp_int((int)ctx->cand_kps[ni * 10 + 2 * j + 1], 0, src_h);
            }
            out_n++;
        }
        for (int j = i + 1; j < cand; j++) {
            int mj = items[j].idx;
            if (!ctx->cand_keep[mj]) {
                continue;
            }
            if (box_iou(&ctx->cand_box[ni * 4], &ctx->cand_box[mj * 4]) > SCRFD_NMS_THRESHOLD) {
                ctx->cand_keep[mj] = 0;
            }
        }
    }

    free(items);
    result->count = out_n;
    return 0;
}

extern "C" int inference_scrfd_model(scrfd_context_t *app_ctx, image_buffer_t *src_img,
                                     scrfd_result *out_result)
{
    if (!app_ctx || !src_img || !out_result || app_ctx->rknn_ctx == 0 || !app_ctx->infer_result) {
        return -1;
    }
    if (!src_img->virt_addr || src_img->width <= 0 || src_img->height <= 0) {
        return -1;
    }

    scrfd_result *result = app_ctx->infer_result;
    memset(result, 0, sizeof(*result));

    cv::Mat src_rgb(src_img->height, src_img->width, CV_8UC3, src_img->virt_addr,
                    src_img->width_stride > 0 ? (size_t)src_img->width_stride : cv::Mat::AUTO_STEP);

    float scale = 1.0f;
    int padx = 0;
    int pady = 0;
    if (convert_image_letterbox(src_rgb, app_ctx->input_u8, app_ctx->model_width,
                                app_ctx->model_height, &scale, &padx, &pady) < 0) {
        return -1;
    }

    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = app_ctx->input_bytes;
    if (app_ctx->input_is_int8) {
        quantize_u8_to_i8(app_ctx->input_u8, app_ctx->input_i8, app_ctx->input_bytes,
                          app_ctx->input_attrs[0].zp, app_ctx->input_attrs[0].scale);
        inputs[0].type = RKNN_TENSOR_INT8;
        inputs[0].buf = app_ctx->input_i8;
    } else {
        inputs[0].type = RKNN_TENSOR_UINT8;
        inputs[0].buf = app_ctx->input_u8;
    }

    if (rknn_inputs_set(app_ctx->rknn_ctx, 1, inputs) < 0) {
        fprintf(stderr, "scrfd: rknn_inputs_set fail\n");
        return -1;
    }
    if (rknn_run(app_ctx->rknn_ctx, NULL) < 0) {
        fprintf(stderr, "scrfd: rknn_run fail\n");
        return -1;
    }

    rknn_output outputs[SCRFD_OUTPUT_NUM];
    memset(outputs, 0, sizeof(outputs));
    for (uint32_t i = 0; i < app_ctx->io_num.n_output; i++) {
        outputs[i].index = i;
        outputs[i].want_float = output_want_float(&app_ctx->output_attrs[i]);
    }
    if (rknn_outputs_get(app_ctx->rknn_ctx, app_ctx->io_num.n_output, outputs, NULL) < 0) {
        fprintf(stderr, "scrfd: rknn_outputs_get fail\n");
        return -1;
    }
    for (uint32_t i = 0; i < app_ctx->io_num.n_output; i++) {
        fetch_output_to_f32(&outputs[i], &app_ctx->output_attrs[i],
                            output_want_float(&app_ctx->output_attrs[i]), app_ctx->deq[i]);
    }
    rknn_outputs_release(app_ctx->rknn_ctx, app_ctx->io_num.n_output, outputs);

    if (post_process_scrfd(app_ctx, src_img->width, src_img->height, scale, padx, pady, result) <
        0) {
        return -1;
    }

    memcpy(out_result, result, sizeof(scrfd_result));
    return 0;
}
