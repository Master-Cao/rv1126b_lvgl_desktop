/*
 * YOLOv8-Seg RKNN（移植自 atk_yolov8_seg_cam，OpenCV letterbox，无 RGA）。
 */
#include "yolov8_seg.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opencv2/opencv.hpp>

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

static int get_image_size(image_buffer_t *image)
{
    if (!image) {
        return 0;
    }
    switch (image->format) {
    case IMAGE_FORMAT_GRAY8:
        return image->width * image->height;
    case IMAGE_FORMAT_RGB888:
        return image->width * image->height * 3;
    case IMAGE_FORMAT_RGBA8888:
        return image->width * image->height * 4;
    default:
        return 0;
    }
}

static int convert_image_with_letterbox_cpu(image_buffer_t *src_image, image_buffer_t *dst_image,
                                            letterbox_t *letterbox, unsigned char fill_color)
{
    if (!src_image || !dst_image || !letterbox) {
        return -1;
    }
    if (src_image->format != IMAGE_FORMAT_RGB888 || dst_image->format != IMAGE_FORMAT_RGB888) {
        return -1;
    }
    if (!src_image->virt_addr || !dst_image->virt_addr) {
        return -1;
    }

    int src_w = src_image->width;
    int src_h = src_image->height;
    int dst_w = dst_image->width;
    int dst_h = dst_image->height;
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return -1;
    }

    float scale_w = (float)dst_w / (float)src_w;
    float scale_h = (float)dst_h / (float)src_h;
    float scale = scale_w < scale_h ? scale_w : scale_h;
    int resize_w = (int)((float)src_w * scale);
    int resize_h = (int)((float)src_h * scale);
    if (resize_w < 1) resize_w = 1;
    if (resize_h < 1) resize_h = 1;
    int pad_x = (dst_w - resize_w) / 2;
    int pad_y = (dst_h - resize_h) / 2;

    cv::Mat dst_mat(dst_h, dst_w, CV_8UC3, dst_image->virt_addr);
    dst_mat.setTo(cv::Scalar(fill_color, fill_color, fill_color));

    int src_stride = src_image->width_stride > 0 ? src_image->width_stride : src_w * 3;
    cv::Mat src_mat(src_h, src_w, CV_8UC3, src_image->virt_addr, (size_t)src_stride);
    cv::Mat roi = dst_mat(cv::Rect(pad_x, pad_y, resize_w, resize_h));
    cv::resize(src_mat, roi, cv::Size(resize_w, resize_h), 0, 0, cv::INTER_LINEAR);

    letterbox->scale = scale;
    letterbox->x_pad = pad_x;
    letterbox->y_pad = pad_y;
    return 0;
}

extern "C" int init_yolov8_seg_model(const char *model_path, rknn_app_context_t *app_ctx)
{
    if (!app_ctx || !model_path) {
        return -1;
    }
    memset(app_ctx, 0, sizeof(*app_ctx));

    char *model = NULL;
    int model_len = read_data_from_file(model_path, &model);
    if (!model || model_len <= 0) {
        fprintf(stderr, "yolov8_seg: load model fail: %s\n", model_path);
        return -1;
    }

    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, model, model_len, 0, NULL);
    free(model);
    if (ret < 0) {
        fprintf(stderr, "yolov8_seg: rknn_init fail ret=%d\n", ret);
        return -1;
    }

    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) {
        rknn_destroy(ctx);
        return -1;
    }
    printf("yolov8_seg: input=%d output=%d\n", io_num.n_input, io_num.n_output);

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
    app_ctx->is_quant = (app_ctx->output_attrs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC &&
                         app_ctx->output_attrs[0].type != RKNN_TENSOR_FLOAT16);

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
    printf("yolov8_seg: model %dx%d ch=%d quant=%d\n", app_ctx->model_width, app_ctx->model_height,
           app_ctx->model_channel, app_ctx->is_quant ? 1 : 0);
    return 0;
}

extern "C" int release_yolov8_seg_model(rknn_app_context_t *app_ctx)
{
    if (!app_ctx) {
        return 0;
    }
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

extern "C" int inference_yolov8_seg_model(rknn_app_context_t *app_ctx, image_buffer_t *img,
                                          object_detect_result_list *od_results)
{
    if (!app_ctx || !img || !od_results) {
        return -1;
    }

    memset(od_results, 0, sizeof(*od_results));

    letterbox_t letter_box;
    memset(&letter_box, 0, sizeof(letter_box));

    image_buffer_t dst_img;
    memset(&dst_img, 0, sizeof(dst_img));
    dst_img.width = app_ctx->model_width;
    dst_img.height = app_ctx->model_height;
    dst_img.format = IMAGE_FORMAT_RGB888;
    dst_img.size = get_image_size(&dst_img);
    dst_img.virt_addr = (unsigned char *)malloc((size_t)dst_img.size);
    if (!dst_img.virt_addr) {
        return -1;
    }

    app_ctx->input_image_width = img->width;
    app_ctx->input_image_height = img->height;

    int ret = convert_image_with_letterbox_cpu(img, &dst_img, &letter_box, 114);
    if (ret < 0) {
        free(dst_img.virt_addr);
        return -1;
    }

    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = (uint32_t)dst_img.size;
    inputs[0].buf = dst_img.virt_addr;

    ret = rknn_inputs_set(app_ctx->rknn_ctx, app_ctx->io_num.n_input, inputs);
    if (ret < 0) {
        free(dst_img.virt_addr);
        return -1;
    }

    ret = rknn_run(app_ctx->rknn_ctx, NULL);
    if (ret < 0) {
        free(dst_img.virt_addr);
        return -1;
    }

    rknn_output *outputs =
        (rknn_output *)calloc(app_ctx->io_num.n_output, sizeof(rknn_output));
    if (!outputs) {
        free(dst_img.virt_addr);
        return -1;
    }
    for (uint32_t i = 0; i < app_ctx->io_num.n_output; i++) {
        outputs[i].index = i;
        outputs[i].want_float = app_ctx->is_quant ? 0 : 1;
    }

    ret = rknn_outputs_get(app_ctx->rknn_ctx, app_ctx->io_num.n_output, outputs, NULL);
    if (ret < 0) {
        free(outputs);
        free(dst_img.virt_addr);
        return -1;
    }

    ret = seg_post_process(app_ctx, outputs, &letter_box, BOX_THRESH, NMS_THRESH, od_results);
    rknn_outputs_release(app_ctx->rknn_ctx, app_ctx->io_num.n_output, outputs);
    free(outputs);
    free(dst_img.virt_addr);
    return ret;
}
