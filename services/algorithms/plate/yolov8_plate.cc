/*
 * 取自 rknn_model_zoo / yolov8/cpp/rknpu2/yolov8.cc，去掉 RV1106 专用分支。
 *
 * 与原 demo 的最大差别：
 *   - read_data_from_file 改为内联实现，去掉 file_utils.c 依赖；
 *   - convert_image_with_letterbox / get_image_size 改为 OpenCV-CPU 实现，
 *     去掉 librga / turbojpeg / stb 依赖（与 ocr/ppocr_system.cc 风格一致）。
 *   - 输入要求：image_buffer_t.format == IMAGE_FORMAT_RGB888 (RGB 三通道连续内存)。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <opencv2/opencv.hpp>

#include "yolov8_plate.h"

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
    case IMAGE_FORMAT_YUV420SP_NV12:
    case IMAGE_FORMAT_YUV420SP_NV21:
        return image->width * image->height * 3 / 2;
    default:
        return 0;
    }
}

/* CPU letterbox：保持长宽比缩放到目标尺寸，剩余区域用 fill_color 填充；
 * 输出 letterbox.scale / x_pad / y_pad，与 RGA 版本字段语义相同，供后处理坐标还原使用。 */
static int convert_image_with_letterbox_cpu(image_buffer_t *src_image, image_buffer_t *dst_image,
                                            letterbox_t *letterbox, unsigned char fill_color)
{
    if (!src_image || !dst_image || !letterbox) {
        return -1;
    }
    if (src_image->format != IMAGE_FORMAT_RGB888 || dst_image->format != IMAGE_FORMAT_RGB888) {
        printf("convert_image_with_letterbox_cpu: only IMAGE_FORMAT_RGB888 supported\n");
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

    /* 整张目标图先填底色；只缩放一次性写到目标的 ROI。 */
    cv::Mat dst_mat(dst_h, dst_w, CV_8UC3, dst_image->virt_addr);
    dst_mat.setTo(cv::Scalar(fill_color, fill_color, fill_color));

    cv::Mat src_mat(src_h, src_w, CV_8UC3, src_image->virt_addr,
                    (size_t)(src_image->width_stride > 0 ? src_image->width_stride : src_w * 3));
    cv::Mat roi = dst_mat(cv::Rect(pad_x, pad_y, resize_w, resize_h));
    cv::resize(src_mat, roi, cv::Size(resize_w, resize_h), 0, 0, cv::INTER_LINEAR);

    letterbox->scale = scale;
    letterbox->x_pad = pad_x;
    letterbox->y_pad = pad_y;
    return 0;
}

static void dump_tensor_attr(rknn_tensor_attr *attr)
{
    printf("  index=%d, name=%s, n_dims=%d, dims=[%d, %d, %d, %d], n_elems=%d, size=%d, fmt=%s, type=%s, "
           "qnt_type=%s, zp=%d, scale=%f\n",
           attr->index, attr->name, attr->n_dims, attr->dims[0], attr->dims[1], attr->dims[2],
           attr->dims[3], attr->n_elems, attr->size, get_format_string(attr->fmt),
           get_type_string(attr->type), get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
}

static int parse_score_class_num(const rknn_tensor_attr *attr)
{
    if (!attr || attr->n_dims < 2) {
        return 1;
    }
    if (attr->fmt == RKNN_TENSOR_NCHW) {
        return attr->dims[1] > 0 ? attr->dims[1] : 1;
    }
    return attr->dims[attr->n_dims - 1] > 0 ? attr->dims[attr->n_dims - 1] : 1;
}

static void parse_grid_hw(const rknn_tensor_attr *attr, int *grid_h, int *grid_w)
{
    if (!attr || !grid_h || !grid_w || attr->n_dims < 4) {
        if (grid_h) {
            *grid_h = 0;
        }
        if (grid_w) {
            *grid_w = 0;
        }
        return;
    }
    if (attr->fmt == RKNN_TENSOR_NCHW) {
        *grid_h = attr->dims[2];
        *grid_w = attr->dims[3];
    } else {
        *grid_h = attr->dims[1];
        *grid_w = attr->dims[2];
    }
}

extern "C" int plate_init_yolov8_model(const char *model_path, rknn_app_context_t *app_ctx)
{
    int ret;
    int model_len = 0;
    char *model = NULL;
    rknn_context ctx = 0;

    model_len = read_data_from_file(model_path, &model);
    if (!model || model_len <= 0) {
        printf("plate_init_yolov8_model: load_model fail: %s\n", model_path);
        return -1;
    }

    ret = rknn_init(&ctx, model, model_len, 0, NULL);
    free(model);
    if (ret < 0) {
        printf("plate_init_yolov8_model: rknn_init fail! ret=%d\n", ret);
        return -1;
    }

    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) {
        printf("plate_init_yolov8_model: rknn_query IO_NUM fail! ret=%d\n", ret);
        rknn_destroy(ctx);
        return -1;
    }
    printf("plate_yolov8: input num=%d, output num=%d\n", io_num.n_input, io_num.n_output);

    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));
    for (uint32_t i = 0; i < io_num.n_input; i++) {
        input_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            printf("plate_init_yolov8_model: rknn_query INPUT_ATTR fail! ret=%d\n", ret);
            rknn_destroy(ctx);
            return -1;
        }
        dump_tensor_attr(&(input_attrs[i]));
    }

    rknn_tensor_attr output_attrs[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));
    for (uint32_t i = 0; i < io_num.n_output; i++) {
        output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            printf("plate_init_yolov8_model: rknn_query OUTPUT_ATTR fail! ret=%d\n", ret);
            rknn_destroy(ctx);
            return -1;
        }
        dump_tensor_attr(&(output_attrs[i]));
    }

    app_ctx->rknn_ctx = ctx;
    app_ctx->is_quant = (output_attrs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC &&
                         output_attrs[0].type != RKNN_TENSOR_FLOAT16);
    app_ctx->io_num = io_num;
    app_ctx->input_attrs = (rknn_tensor_attr *)malloc(io_num.n_input * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->input_attrs, input_attrs, io_num.n_input * sizeof(rknn_tensor_attr));
    app_ctx->output_attrs = (rknn_tensor_attr *)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->output_attrs, output_attrs, io_num.n_output * sizeof(rknn_tensor_attr));

    int output_per_branch = io_num.n_output >= 3 ? (int)io_num.n_output / 3 : 1;
    if (io_num.n_output == 1) {
        const rknn_tensor_attr *a = &output_attrs[0];
        int channels = 1;
        if (a->n_dims == 3) {
            channels = (a->dims[1] < a->dims[2]) ? a->dims[1] : a->dims[2];
        } else if (a->n_dims >= 4) {
            channels = (a->fmt == RKNN_TENSOR_NCHW) ? a->dims[1] : a->dims[a->n_dims - 1];
        }
        app_ctx->model_class_num = channels > 4 ? channels - 4 : 1;
        printf("plate_yolov8: single-output head (Ultralytics), channels=%d class_num=%d\n",
               channels, app_ctx->model_class_num);
    } else {
        /* 分离输出 DFL：每个 stride 顺序为 [reg, cls, (score_sum)]，
         * 类别数取第一个 cls 分支（index 1）的通道数。 */
        int score_attr_idx = 1;
        if (score_attr_idx < (int)io_num.n_output) {
            app_ctx->model_class_num = parse_score_class_num(&output_attrs[score_attr_idx]);
        } else {
            app_ctx->model_class_num = 1;
        }
        if (io_num.n_output % 3 == 0 && output_per_branch >= 2) {
            printf("plate_yolov8: %d-output DFL head (per_branch=%d, rknn_model_zoo)\n",
                   io_num.n_output, output_per_branch);
        } else {
            printf("plate_yolov8: WARN output num=%d (expect 1, 6 or 9)\n", io_num.n_output);
        }
    }

    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW) {
        printf("plate_yolov8: NCHW input fmt\n");
        app_ctx->model_channel = input_attrs[0].dims[1];
        app_ctx->model_height = input_attrs[0].dims[2];
        app_ctx->model_width = input_attrs[0].dims[3];
    } else {
        printf("plate_yolov8: NHWC input fmt\n");
        app_ctx->model_height = input_attrs[0].dims[1];
        app_ctx->model_width = input_attrs[0].dims[2];
        app_ctx->model_channel = input_attrs[0].dims[3];
    }
    printf("plate_yolov8: input height=%d, width=%d, channel=%d, class_num=%d, is_quant=%d\n",
           app_ctx->model_height, app_ctx->model_width, app_ctx->model_channel,
           app_ctx->model_class_num, app_ctx->is_quant ? 1 : 0);
    return 0;
}

extern "C" int plate_release_yolov8_model(rknn_app_context_t *app_ctx)
{
    if (app_ctx->input_attrs) {
        free(app_ctx->input_attrs);
        app_ctx->input_attrs = NULL;
    }
    if (app_ctx->output_attrs) {
        free(app_ctx->output_attrs);
        app_ctx->output_attrs = NULL;
    }
    if (app_ctx->rknn_ctx != 0) {
        rknn_destroy(app_ctx->rknn_ctx);
        app_ctx->rknn_ctx = 0;
    }
    return 0;
}

extern "C" int plate_inference_yolov8_model(rknn_app_context_t *app_ctx, image_buffer_t *img,
                                            object_detect_result_list *od_results)
{
    return plate_inference_yolov8_model_ex(app_ctx, img, od_results, BOX_THRESH, NMS_THRESH);
}

extern "C" int plate_inference_yolov8_model_ex(rknn_app_context_t *app_ctx, image_buffer_t *img,
                                            object_detect_result_list *od_results,
                                            float box_conf_threshold, float nms_threshold)
{
    int ret;
    image_buffer_t dst_img;
    letterbox_t letter_box;
    rknn_input inputs[app_ctx->io_num.n_input];
    rknn_output outputs[app_ctx->io_num.n_output];
    int bg_color = 114;

    if (!app_ctx || !img || !od_results) {
        return -1;
    }

    memset(od_results, 0x00, sizeof(*od_results));
    memset(&letter_box, 0, sizeof(letterbox_t));
    memset(&dst_img, 0, sizeof(image_buffer_t));
    memset(inputs, 0, sizeof(inputs));
    memset(outputs, 0, sizeof(outputs));

    dst_img.width = app_ctx->model_width;
    dst_img.height = app_ctx->model_height;
    dst_img.format = IMAGE_FORMAT_RGB888;
    dst_img.size = get_image_size(&dst_img);
    dst_img.virt_addr = (unsigned char *)malloc(dst_img.size);
    if (!dst_img.virt_addr) {
        printf("plate_inference_yolov8_model: malloc letterbox buffer %d fail\n", dst_img.size);
        return -1;
    }

    ret = convert_image_with_letterbox_cpu(img, &dst_img, &letter_box, (unsigned char)bg_color);
    if (ret < 0) {
        printf("plate_inference_yolov8_model: letterbox fail ret=%d\n", ret);
        free(dst_img.virt_addr);
        return -1;
    }

    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = app_ctx->model_width * app_ctx->model_height * app_ctx->model_channel;
    inputs[0].buf = dst_img.virt_addr;

    ret = rknn_inputs_set(app_ctx->rknn_ctx, app_ctx->io_num.n_input, inputs);
    if (ret < 0) {
        printf("plate_inference_yolov8_model: rknn_inputs_set fail ret=%d\n", ret);
        free(dst_img.virt_addr);
        return -1;
    }

    ret = rknn_run(app_ctx->rknn_ctx, NULL);
    if (ret < 0) {
        printf("plate_inference_yolov8_model: rknn_run fail ret=%d\n", ret);
        free(dst_img.virt_addr);
        return -1;
    }

    for (uint32_t i = 0; i < app_ctx->io_num.n_output; i++) {
        outputs[i].index = i;
        /* Ultralytics 单路输出需 float 解码；9 路 DFL 头保持 int8 原样反量化 */
        if (app_ctx->io_num.n_output == 1) {
            outputs[i].want_float = 1;
        } else {
            outputs[i].want_float = app_ctx->is_quant ? 0 : 1;
        }
    }
    ret = rknn_outputs_get(app_ctx->rknn_ctx, app_ctx->io_num.n_output, outputs, NULL);
    if (ret < 0) {
        printf("plate_inference_yolov8_model: rknn_outputs_get fail ret=%d\n", ret);
        free(dst_img.virt_addr);
        return ret;
    }

    plate_post_process(app_ctx, outputs, &letter_box, box_conf_threshold, nms_threshold, od_results);

    rknn_outputs_release(app_ctx->rknn_ctx, app_ctx->io_num.n_output, outputs);
    free(dst_img.virt_addr);
    return 0;
}
