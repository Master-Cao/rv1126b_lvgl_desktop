/*
 * MobileFaceNet RKNN 特征提取（w600k_mbf / face_rec.rknn）。
 */
#include "face_rec.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static void l2_normalize_inplace(float *vec, int dim)
{
    double sum = 0.0;
    for (int i = 0; i < dim; i++) {
        sum += (double)vec[i] * (double)vec[i];
    }
    if (sum < 1e-12) {
        return;
    }
    float inv = (float)(1.0 / sqrt(sum));
    for (int i = 0; i < dim; i++) {
        vec[i] *= inv;
    }
}

extern "C" int init_face_rec_model(const char *model_path, face_rec_context_t *app_ctx)
{
    if (!app_ctx || !model_path) {
        return -1;
    }
    memset(app_ctx, 0, sizeof(*app_ctx));

    printf("face_rec: loading model: %s\n", model_path);
    if (access(model_path, R_OK) != 0) {
        printf("face_rec: file missing or unreadable: %s\n", model_path);
        return -1;
    }
    struct stat st;
    if (stat(model_path, &st) == 0) {
        printf("face_rec: file ok size=%lld bytes\n", (long long)st.st_size);
    }

    char *model = NULL;
    int model_len = read_data_from_file(model_path, &model);
    if (!model || model_len <= 0) {
        printf("face_rec: load_model fail: %s\n", model_path);
        return -1;
    }

    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, model, model_len, 0, NULL);
    free(model);
    if (ret < 0) {
        printf("face_rec: rknn_init fail ret=%d\n", ret);
        return -1;
    }

    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC || io_num.n_input < 1 || io_num.n_output < 1) {
        rknn_destroy(ctx);
        return -1;
    }

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
    }
    for (uint32_t i = 0; i < io_num.n_output; i++) {
        app_ctx->output_attrs[i].index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &app_ctx->output_attrs[i], sizeof(rknn_tensor_attr));
    }

    app_ctx->rknn_ctx = ctx;
    app_ctx->io_num = io_num;

    rknn_tensor_attr *in0 = &app_ctx->input_attrs[0];
    app_ctx->input_is_nchw = (in0->fmt == RKNN_TENSOR_NCHW);
    if (app_ctx->input_is_nchw) {
        app_ctx->model_channel = in0->dims[1];
        app_ctx->model_height = in0->dims[2];
        app_ctx->model_width = in0->dims[3];
    } else {
        app_ctx->model_height = in0->dims[1];
        app_ctx->model_width = in0->dims[2];
        app_ctx->model_channel = in0->dims[3];
    }
    app_ctx->input_bytes = app_ctx->model_width * app_ctx->model_height * app_ctx->model_channel;
    app_ctx->input_buf = (unsigned char *)malloc((size_t)app_ctx->input_bytes);
    if (!app_ctx->input_buf) {
        release_face_rec_model(app_ctx);
        return -1;
    }

    printf("face_rec model: %dx%d ch=%d fmt=%s\n", app_ctx->model_width, app_ctx->model_height,
           app_ctx->model_channel, app_ctx->input_is_nchw ? "NCHW" : "NHWC");
    printf("face_rec: init ok\n");
    return 0;
}

extern "C" int release_face_rec_model(face_rec_context_t *app_ctx)
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

extern "C" int inference_face_rec_model(face_rec_context_t *app_ctx, const unsigned char *aligned_rgb,
                                        int width_stride, float *out_embed)
{
    if (!app_ctx || !aligned_rgb || !out_embed || app_ctx->rknn_ctx == 0 || !app_ctx->input_buf) {
        return -1;
    }

    int stride = width_stride > 0 ? width_stride : (app_ctx->model_width * app_ctx->model_channel);
    if (app_ctx->input_is_nchw) {
        /* HWC -> CHW */
        int h = app_ctx->model_height;
        int w = app_ctx->model_width;
        int c = app_ctx->model_channel;
        unsigned char *dst = app_ctx->input_buf;
        for (int ch = 0; ch < c; ch++) {
            for (int y = 0; y < h; y++) {
                const unsigned char *row = aligned_rgb + (size_t)y * (size_t)stride;
                for (int x = 0; x < w; x++) {
                    dst[ch * h * w + y * w + x] = row[x * c + ch];
                }
            }
        }
    } else {
        int need = app_ctx->model_width * app_ctx->model_channel;
        if (stride == need) {
            memcpy(app_ctx->input_buf, aligned_rgb, (size_t)app_ctx->input_bytes);
        } else {
            for (int y = 0; y < app_ctx->model_height; y++) {
                memcpy(app_ctx->input_buf + (size_t)y * (size_t)need,
                       aligned_rgb + (size_t)y * (size_t)stride, (size_t)need);
            }
        }
    }

    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = app_ctx->input_is_nchw ? RKNN_TENSOR_NCHW : RKNN_TENSOR_NHWC;
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

    int n_elems = app_ctx->output_attrs[0].n_elems;
    int copy_n = n_elems < FACE_REC_EMBED_DIM ? n_elems : FACE_REC_EMBED_DIM;
    memcpy(out_embed, outputs[0].buf, (size_t)copy_n * sizeof(float));
    if (copy_n < FACE_REC_EMBED_DIM) {
        memset(out_embed + copy_n, 0, (size_t)(FACE_REC_EMBED_DIM - copy_n) * sizeof(float));
    }
    rknn_outputs_release(app_ctx->rknn_ctx, 1, outputs);

    l2_normalize_inplace(out_embed, FACE_REC_EMBED_DIM);
    return 0;
}
