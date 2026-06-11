/*
 * LPRNet 车牌字符识别实现（移植自 rknn_model_zoo/LPRNet）。
 * - 预处理：resize 到模型尺寸 + RGB→BGR（对齐原 demo image_preprocess）
 * - 推理：rknn_inputs_set(UINT8,NHWC) + rknn_run + rknn_outputs_get(want_float)
 * - 后处理：逐位 argmax + 去重去 blank 的 CTC 贪心解码
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <opencv2/opencv.hpp>

#include "lprnet.h"

/* 车牌字典，68 项，索引与模型输出类别一致；最后一项为 blank("-")。 */
static const char *const PLATE_CODE[LPRNET_OUT_ROWS] = {
    "京", "沪", "津", "渝", "冀", "晋", "蒙", "辽", "吉", "黑",
    "苏", "浙", "皖", "闽", "赣", "鲁", "豫", "鄂", "湘", "粤",
    "桂", "琼", "川", "贵", "云", "藏", "陕", "甘", "青", "宁",
    "新",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "A", "B", "C", "D", "E", "F", "G", "H", "J", "K",
    "L", "M", "N", "P", "Q", "R", "S", "T", "U", "V",
    "W", "X", "Y", "Z", "I", "O", "-"};

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

static void dump_tensor_attr(rknn_tensor_attr *attr)
{
    printf("  index=%d, name=%s, n_dims=%d, dims=[%d, %d, %d, %d], n_elems=%d, size=%d, fmt=%s, "
           "type=%s, qnt_type=%s, zp=%d, scale=%f\n",
           attr->index, attr->name, attr->n_dims, attr->dims[0], attr->dims[1], attr->dims[2],
           attr->dims[3], attr->n_elems, attr->size, get_format_string(attr->fmt),
           get_type_string(attr->type), get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
}

extern "C" int init_lprnet_model(const char *model_path, lprnet_context_t *app_ctx)
{
    if (!app_ctx) {
        return -1;
    }
    memset(app_ctx, 0, sizeof(*app_ctx));

    char *model = NULL;
    int model_len = read_data_from_file(model_path, &model);
    if (!model || model_len <= 0) {
        printf("lprnet: load_model fail: %s\n", model_path);
        return -1;
    }

    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, model, model_len, 0, NULL);
    free(model);
    if (ret < 0) {
        printf("lprnet: rknn_init fail ret=%d\n", ret);
        return -1;
    }

    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC || io_num.n_input < 1 || io_num.n_output < 1) {
        rknn_destroy(ctx);
        return -1;
    }
    printf("lprnet model: input num=%d, output num=%d\n", io_num.n_input, io_num.n_output);

    app_ctx->input_attrs = (rknn_tensor_attr *)calloc(io_num.n_input, sizeof(rknn_tensor_attr));
    app_ctx->output_attrs = (rknn_tensor_attr *)calloc(io_num.n_output, sizeof(rknn_tensor_attr));
    if (!app_ctx->input_attrs || !app_ctx->output_attrs) {
        free(app_ctx->input_attrs);
        free(app_ctx->output_attrs);
        rknn_destroy(ctx);
        return -1;
    }

    for (uint32_t i = 0; i < io_num.n_input; i++) {
        app_ctx->input_attrs[i].index = i;
        rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &app_ctx->input_attrs[i], sizeof(rknn_tensor_attr));
        dump_tensor_attr(&app_ctx->input_attrs[i]);
    }
    for (uint32_t i = 0; i < io_num.n_output; i++) {
        app_ctx->output_attrs[i].index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &app_ctx->output_attrs[i], sizeof(rknn_tensor_attr));
        dump_tensor_attr(&app_ctx->output_attrs[i]);
    }

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
    printf("lprnet model: %dx%d ch=%d\n", app_ctx->model_width, app_ctx->model_height,
           app_ctx->model_channel);

    app_ctx->input_bytes = app_ctx->model_width * app_ctx->model_height * app_ctx->model_channel;
    app_ctx->input_buf = (unsigned char *)malloc((size_t)app_ctx->input_bytes);
    if (!app_ctx->input_buf) {
        release_lprnet_model(app_ctx);
        return -1;
    }
    return 0;
}

extern "C" int release_lprnet_model(lprnet_context_t *app_ctx)
{
    if (!app_ctx) {
        return 0;
    }
    free(app_ctx->input_buf);
    app_ctx->input_buf = NULL;
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

extern "C" int inference_lprnet_model(lprnet_context_t *app_ctx, image_buffer_t *src_img,
                                      char *out_text, int out_text_size, float *out_conf)
{
    if (out_conf) {
        *out_conf = 0.0f;
    }
    if (!app_ctx || !src_img || !out_text || out_text_size <= 0 || app_ctx->rknn_ctx == 0 ||
        !app_ctx->input_buf) {
        return -1;
    }
    if (!src_img->virt_addr || src_img->width <= 0 || src_img->height <= 0) {
        return -1;
    }
    out_text[0] = '\0';

    /* 预处理：缩放到模型尺寸 + RGB→BGR（对齐原 demo image_preprocess）。 */
    cv::Mat src_rgb(src_img->height, src_img->width, CV_8UC3, src_img->virt_addr,
                    src_img->width_stride > 0 ? (size_t)src_img->width_stride : cv::Mat::AUTO_STEP);
    cv::Mat dst(app_ctx->model_height, app_ctx->model_width, CV_8UC3, app_ctx->input_buf);
    cv::resize(src_rgb, dst, cv::Size(app_ctx->model_width, app_ctx->model_height), 0, 0,
               cv::INTER_LINEAR);
    cv::cvtColor(dst, dst, cv::COLOR_RGB2BGR);

    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = app_ctx->input_bytes;
    inputs[0].buf = app_ctx->input_buf;

    if (rknn_inputs_set(app_ctx->rknn_ctx, 1, inputs) < 0) {
        return -1;
    }
    if (rknn_run(app_ctx->rknn_ctx, NULL) < 0) {
        return -1;
    }

    rknn_output outputs[1];
    memset(outputs, 0, sizeof(outputs));
    outputs[0].index = 0;
    outputs[0].want_float = 1;
    if (rknn_outputs_get(app_ctx->rknn_ctx, 1, outputs, NULL) < 0) {
        return -1;
    }

    const float *buf = (const float *)outputs[0].buf;
    int rows = LPRNET_OUT_ROWS;
    int cols = LPRNET_OUT_COLS;
    /* 若模型输出维度不同，按实际 n_elems 兜底，避免越界。 */
    int n_elems = (int)app_ctx->output_attrs[0].n_elems;
    if (rows * cols > n_elems) {
        rknn_outputs_release(app_ctx->rknn_ctx, 1, outputs);
        return -1;
    }

    /* 输出按 [rows(类别) x cols(位置)] 行主序存放：元素(y,x) 在 y*cols + x。 */
    int argmax[LPRNET_OUT_COLS];
    float conf_col[LPRNET_OUT_COLS];
    for (int x = 0; x < cols; x++) {
        int best = 0;
        float best_v = buf[x];
        for (int y = 1; y < rows; y++) {
            float v = buf[y * cols + x];
            if (v > best_v) {
                best_v = v;
                best = y;
            }
        }
        /* 对该位置做 softmax 取最大概率作为置信度。 */
        float sum = 0.0f;
        for (int y = 0; y < rows; y++) {
            sum += expf(buf[y * cols + x] - best_v);
        }
        conf_col[x] = (sum > 0.0f) ? (1.0f / sum) : 0.0f;
        argmax[x] = best;
    }

    rknn_outputs_release(app_ctx->rknn_ctx, 1, outputs);

    /* CTC 贪心解码：去重 + 去 blank（blank 索引为 rows-1）。 */
    int blank = rows - 1;
    int prev = argmax[0];
    size_t off = 0;
    float conf_sum = 0.0f;
    int conf_n = 0;

    const int sel0 = argmax[0];
    if (sel0 != blank) {
        const char *s = PLATE_CODE[sel0];
        size_t len = strlen(s);
        if (off + len < (size_t)out_text_size) {
            memcpy(out_text + off, s, len);
            off += len;
        }
        conf_sum += conf_col[0];
        conf_n++;
    }
    for (int x = 1; x < cols; x++) {
        int cur = argmax[x];
        if (cur == blank || cur == prev) {
            prev = cur;
            continue;
        }
        prev = cur;
        const char *s = PLATE_CODE[cur];
        size_t len = strlen(s);
        if (off + len < (size_t)out_text_size) {
            memcpy(out_text + off, s, len);
            off += len;
        }
        conf_sum += conf_col[x];
        conf_n++;
    }
    out_text[off] = '\0';

    if (out_conf && conf_n > 0) {
        *out_conf = conf_sum / (float)conf_n;
    }
    return 0;
}
