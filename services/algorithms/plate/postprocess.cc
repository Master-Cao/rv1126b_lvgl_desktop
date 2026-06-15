/*
 * 车牌 YOLOv8 检测后处理（移植自 detect/postprocess.cc，单类别 plate）。
 */
#include "yolov8_plate.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <set>
#include <vector>

static const char *kPlateLabels[OBJ_CLASS_NUM] = {
    "plate",
};

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

inline static int clamp_int(float val, int min, int max)
{
    return val > min ? (val < max ? (int)val : max) : min;
}

static float CalculateOverlap(float xmin0, float ymin0, float xmax0, float ymax0, float xmin1,
                              float ymin1, float xmax1, float ymax1)
{
    float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1.0f);
    float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1.0f);
    float i = w * h;
    float u = (xmax0 - xmin0 + 1.0f) * (ymax0 - ymin0 + 1.0f) +
              (xmax1 - xmin1 + 1.0f) * (ymax1 - ymin1 + 1.0f) - i;
    return u <= 0.f ? 0.f : (i / u);
}

static int nms(int validCount, std::vector<float> &outputLocations, std::vector<int> classIds,
               std::vector<int> &order, int filterId, float threshold)
{
    for (int i = 0; i < validCount; ++i) {
        if (order[i] == -1 || classIds[i] != filterId) {
            continue;
        }
        int n = order[i];
        for (int j = i + 1; j < validCount; ++j) {
            int m = order[j];
            if (m == -1 || classIds[i] != filterId) {
                continue;
            }
            float xmin0 = outputLocations[n * 4 + 0];
            float ymin0 = outputLocations[n * 4 + 1];
            float xmax0 = outputLocations[n * 4 + 0] + outputLocations[n * 4 + 2];
            float ymax0 = outputLocations[n * 4 + 1] + outputLocations[n * 4 + 3];

            float xmin1 = outputLocations[m * 4 + 0];
            float ymin1 = outputLocations[m * 4 + 1];
            float xmax1 = outputLocations[m * 4 + 0] + outputLocations[m * 4 + 2];
            float ymax1 = outputLocations[m * 4 + 1] + outputLocations[m * 4 + 3];

            if (CalculateOverlap(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1) >
                threshold) {
                order[j] = -1;
            }
        }
    }
    return 0;
}

static int quick_sort_indice_inverse(std::vector<float> &input, int left, int right,
                                     std::vector<int> &indices)
{
    float key;
    int key_index;
    int low = left;
    int high = right;
    if (left < right) {
        key_index = indices[left];
        key = input[left];
        while (low < high) {
            while (low < high && input[high] <= key) {
                high--;
            }
            input[low] = input[high];
            indices[low] = indices[high];
            while (low < high && input[low] >= key) {
                low++;
            }
            input[high] = input[low];
            indices[high] = indices[low];
        }
        input[low] = key;
        indices[low] = key_index;
        quick_sort_indice_inverse(input, left, low - 1, indices);
        quick_sort_indice_inverse(input, low + 1, right, indices);
    }
    return low;
}

inline static int32_t __clip(float val, float min, float max)
{
    float f = val <= min ? min : (val >= max ? max : val);
    return (int32_t)f;
}

static int8_t qnt_f32_to_affine(float f32, int32_t zp, float scale)
{
    float dst_val = (f32 / scale) + zp;
    return (int8_t)__clip(dst_val, -128, 127);
}

static float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale)
{
    return ((float)qnt - (float)zp) * scale;
}

static void compute_dfl(float *tensor, int dfl_len, float *box)
{
    for (int b = 0; b < 4; b++) {
        float exp_t[dfl_len];
        float exp_sum = 0;
        float acc_sum = 0;
        for (int i = 0; i < dfl_len; i++) {
            exp_t[i] = expf(tensor[i + b * dfl_len]);
            exp_sum += exp_t[i];
        }
        for (int i = 0; i < dfl_len; i++) {
            acc_sum += exp_t[i] / exp_sum * i;
        }
        box[b] = acc_sum;
    }
}

static int process_i8(int8_t *box_tensor, int32_t box_zp, float box_scale, int8_t *score_tensor,
                      int32_t score_zp, float score_scale, int8_t *score_sum_tensor,
                      int32_t score_sum_zp, float score_sum_scale, int grid_h, int grid_w,
                      int stride, int dfl_len, int class_num, std::vector<float> &boxes,
                      std::vector<float> &objProbs, std::vector<int> &classId, float threshold)
{
    int validCount = 0;
    int grid_len = grid_h * grid_w;
    int8_t score_thres_i8 = qnt_f32_to_affine(threshold, score_zp, score_scale);
    int8_t score_sum_thres_i8 = qnt_f32_to_affine(threshold, score_sum_zp, score_sum_scale);

    for (int i = 0; i < grid_h; i++) {
        for (int j = 0; j < grid_w; j++) {
            int offset = i * grid_w + j;
            int max_class_id = -1;

            if (score_sum_tensor != nullptr) {
                if (score_sum_tensor[offset] < score_sum_thres_i8) {
                    continue;
                }
            }

            int8_t max_score = -score_zp;
            for (int c = 0; c < class_num; c++) {
                if ((score_tensor[offset] > score_thres_i8) &&
                    (score_tensor[offset] > max_score)) {
                    max_score = score_tensor[offset];
                    max_class_id = c;
                }
                offset += grid_len;
            }

            if (max_score > score_thres_i8) {
                offset = i * grid_w + j;
                float box[4];
                float before_dfl[dfl_len * 4];
                for (int k = 0; k < dfl_len * 4; k++) {
                    before_dfl[k] = deqnt_affine_to_f32(box_tensor[offset], box_zp, box_scale);
                    offset += grid_len;
                }
                compute_dfl(before_dfl, dfl_len, box);

                float x1 = (-box[0] + j + 0.5f) * stride;
                float y1 = (-box[1] + i + 0.5f) * stride;
                float x2 = (box[2] + j + 0.5f) * stride;
                float y2 = (box[3] + i + 0.5f) * stride;
                boxes.push_back(x1);
                boxes.push_back(y1);
                boxes.push_back(x2 - x1);
                boxes.push_back(y2 - y1);

                objProbs.push_back(deqnt_affine_to_f32(max_score, score_zp, score_scale));
                classId.push_back(max_class_id);
                validCount++;
            }
        }
    }
    return validCount;
}

static int process_fp32(float *box_tensor, float *score_tensor, float *score_sum_tensor,
                        int grid_h, int grid_w, int stride, int dfl_len, int class_num,
                        std::vector<float> &boxes, std::vector<float> &objProbs,
                        std::vector<int> &classId, float threshold)
{
    int validCount = 0;
    int grid_len = grid_h * grid_w;
    for (int i = 0; i < grid_h; i++) {
        for (int j = 0; j < grid_w; j++) {
            int offset = i * grid_w + j;
            int max_class_id = -1;

            if (score_sum_tensor != nullptr) {
                if (score_sum_tensor[offset] < threshold) {
                    continue;
                }
            }

            float max_score = 0;
            for (int c = 0; c < class_num; c++) {
                if ((score_tensor[offset] > threshold) && (score_tensor[offset] > max_score)) {
                    max_score = score_tensor[offset];
                    max_class_id = c;
                }
                offset += grid_len;
            }

            if (max_score > threshold) {
                offset = i * grid_w + j;
                float box[4];
                float before_dfl[dfl_len * 4];
                for (int k = 0; k < dfl_len * 4; k++) {
                    before_dfl[k] = box_tensor[offset];
                    offset += grid_len;
                }
                compute_dfl(before_dfl, dfl_len, box);

                float x1 = (-box[0] + j + 0.5f) * stride;
                float y1 = (-box[1] + i + 0.5f) * stride;
                float x2 = (box[2] + j + 0.5f) * stride;
                float y2 = (box[3] + i + 0.5f) * stride;
                boxes.push_back(x1);
                boxes.push_back(y1);
                boxes.push_back(x2 - x1);
                boxes.push_back(y2 - y1);

                objProbs.push_back(max_score);
                classId.push_back(max_class_id);
                validCount++;
            }
        }
    }
    return validCount;
}

/* Ultralytics YOLOv8 ONNX/RKNN 单路输出 (1, 4+C, N)，已含 cxcywh，非 DFL 三分支。 */
static bool parse_single_output_layout(const rknn_tensor_attr *attr, int *channels, int *num_anchors,
                                       bool *channel_major)
{
    if (!attr || !channels || !num_anchors || !channel_major || attr->n_dims < 2) {
        return false;
    }

    int d1 = attr->dims[1];
    int d2 = attr->n_dims >= 3 ? attr->dims[2] : 1;

    if (attr->n_dims == 3) {
        if (d1 <= d2) {
            *channels = d1;
            *num_anchors = d2;
            *channel_major = true;
        } else {
            *channels = d2;
            *num_anchors = d1;
            *channel_major = false;
        }
        return *channels >= 5 && *num_anchors > 0;
    }

    if (attr->n_dims >= 4) {
        if (attr->fmt == RKNN_TENSOR_NCHW) {
            *channels = d1;
            *num_anchors = attr->dims[2] * attr->dims[3];
            *channel_major = true;
        } else {
            *channels = attr->dims[attr->n_dims - 1];
            *num_anchors = d1 * d2;
            *channel_major = false;
        }
        return *channels >= 5 && *num_anchors > 0;
    }

    return false;
}

static int post_process_single_output(rknn_app_context_t *app_ctx, rknn_output *outputs,
                                      letterbox_t *letter_box, float conf_threshold,
                                      float nms_threshold, object_detect_result_list *od_results)
{
    const rknn_tensor_attr *attr = &app_ctx->output_attrs[0];
    float *data = (float *)outputs[0].buf;
    if (!data) {
        return 0;
    }

    int channels = 0;
    int num_anchors = 0;
    bool channel_major = true;
    if (!parse_single_output_layout(attr, &channels, &num_anchors, &channel_major)) {
        printf("plate_post: single-output layout parse fail, n_dims=%d dims=[%d,%d,%d,%d]\n",
               attr->n_dims, attr->dims[0], attr->dims[1], attr->dims[2], attr->dims[3]);
        return 0;
    }

    int class_num = channels - 4;
    if (class_num <= 0) {
        class_num = 1;
    }

    std::vector<float> filterBoxes;
    std::vector<float> objProbs;
    std::vector<int> classId;
    int validCount = 0;
    int model_in_w = app_ctx->model_width;
    int model_in_h = app_ctx->model_height;

    /* 诊断：记录全局最高分及对应框，便于判断是 score 偏低还是坐标异常 */
    float dbg_best_score = -1e9f;
    float dbg_best_cx = 0, dbg_best_cy = 0, dbg_best_w = 0, dbg_best_h = 0;
    float dbg_coord_min = 1e9f, dbg_coord_max = -1e9f;

    for (int i = 0; i < num_anchors; i++) {
        float cx, cy, w, h;
        if (channel_major) {
            cx = data[0 * num_anchors + i];
            cy = data[1 * num_anchors + i];
            w = data[2 * num_anchors + i];
            h = data[3 * num_anchors + i];
        } else {
            int base = i * channels;
            cx = data[base + 0];
            cy = data[base + 1];
            w = data[base + 2];
            h = data[base + 3];
        }

        float max_score = 0.f;
        int max_class_id = 0;
        for (int c = 0; c < class_num; c++) {
            float score;
            if (channel_major) {
                score = data[(4 + c) * num_anchors + i];
            } else {
                score = data[i * channels + 4 + c];
            }
            if (score > max_score) {
                max_score = score;
                max_class_id = c;
            }
        }

        if (max_score > dbg_best_score) {
            dbg_best_score = max_score;
            dbg_best_cx = cx;
            dbg_best_cy = cy;
            dbg_best_w = w;
            dbg_best_h = h;
        }
        if (cx < dbg_coord_min) dbg_coord_min = cx;
        if (cx > dbg_coord_max) dbg_coord_max = cx;

        if (max_score < conf_threshold) {
            continue;
        }

        float x1 = cx - w * 0.5f;
        float y1 = cy - h * 0.5f;
        filterBoxes.push_back(x1);
        filterBoxes.push_back(y1);
        filterBoxes.push_back(w);
        filterBoxes.push_back(h);
        objProbs.push_back(max_score);
        classId.push_back(max_class_id);
        validCount++;
    }

    printf("plate_post(single): anchors=%d ch=%d conf_thr=%.3f valid=%d best_score=%.4f "
           "best_box=cxcywh(%.1f,%.1f,%.1f,%.1f) cx_range[%.1f,%.1f]\n",
           num_anchors, channels, conf_threshold, validCount, dbg_best_score, dbg_best_cx,
           dbg_best_cy, dbg_best_w, dbg_best_h, dbg_coord_min, dbg_coord_max);

    if (validCount <= 0) {
        return 0;
    }

    std::vector<int> indexArray;
    for (int i = 0; i < validCount; ++i) {
        indexArray.push_back(i);
    }
    quick_sort_indice_inverse(objProbs, 0, validCount - 1, indexArray);

    std::set<int> class_set(std::begin(classId), std::end(classId));
    for (auto c : class_set) {
        nms(validCount, filterBoxes, classId, indexArray, c, nms_threshold);
    }

    int last_count = 0;
    od_results->count = 0;
    for (int i = 0; i < validCount; ++i) {
        if (indexArray[i] == -1 || last_count >= OBJ_NUMB_MAX_SIZE) {
            continue;
        }
        int n = indexArray[i];

        float x1 = filterBoxes[n * 4 + 0] - letter_box->x_pad;
        float y1 = filterBoxes[n * 4 + 1] - letter_box->y_pad;
        float x2 = x1 + filterBoxes[n * 4 + 2];
        float y2 = y1 + filterBoxes[n * 4 + 3];
        int id = classId[n];
        float obj_conf = objProbs[n];

        od_results->results[last_count].box.left =
            clamp_int(x1, 0, model_in_w) / letter_box->scale;
        od_results->results[last_count].box.top =
            clamp_int(y1, 0, model_in_h) / letter_box->scale;
        od_results->results[last_count].box.right =
            clamp_int(x2, 0, model_in_w) / letter_box->scale;
        od_results->results[last_count].box.bottom =
            clamp_int(y2, 0, model_in_h) / letter_box->scale;
        od_results->results[last_count].prop = obj_conf;
        od_results->results[last_count].cls_id = id;
        last_count++;
    }
    od_results->count = last_count;
    return 0;
}

extern "C" int plate_post_process(rknn_app_context_t *app_ctx, void *outputs, letterbox_t *letter_box,
                            float conf_threshold, float nms_threshold,
                            object_detect_result_list *od_results)
{
    rknn_output *_outputs = (rknn_output *)outputs;
    std::vector<float> filterBoxes;
    std::vector<float> objProbs;
    std::vector<int> classId;
    int validCount = 0;
    int stride = 0;
    int grid_h = 0;
    int grid_w = 0;
    int model_in_w = app_ctx->model_width;
    int model_in_h = app_ctx->model_height;

    memset(od_results, 0, sizeof(object_detect_result_list));

    if (app_ctx->io_num.n_output == 1) {
        return post_process_single_output(app_ctx, _outputs, letter_box, conf_threshold,
                                          nms_threshold, od_results);
    }

    int class_num = app_ctx->model_class_num > 0 ? app_ctx->model_class_num : 1;
    int output_per_branch = app_ctx->io_num.n_output >= 3 ? (int)app_ctx->io_num.n_output / 3 : 1;
    for (int i = 0; i < 3; i++) {
        void *score_sum = nullptr;
        int32_t score_sum_zp = 0;
        float score_sum_scale = 1.0f;
        if (output_per_branch == 3) {
            score_sum = _outputs[i * output_per_branch + 2].buf;
            score_sum_zp = app_ctx->output_attrs[i * output_per_branch + 2].zp;
            score_sum_scale = app_ctx->output_attrs[i * output_per_branch + 2].scale;
        }
        int box_idx = i * output_per_branch;
        int score_idx = i * output_per_branch + 1;
        if (box_idx >= (int)app_ctx->io_num.n_output || score_idx >= (int)app_ctx->io_num.n_output) {
            break;
        }

        int dfl_len = app_ctx->output_attrs[box_idx].dims[1] / 4;
        if (dfl_len <= 0) {
            dfl_len = 16;
        }
        parse_grid_hw(&app_ctx->output_attrs[box_idx], &grid_h, &grid_w);
        if (grid_h <= 0 || grid_w <= 0) {
            continue;
        }
        stride = model_in_h / grid_h;

        if (app_ctx->is_quant) {
            validCount += process_i8(
                (int8_t *)_outputs[box_idx].buf, app_ctx->output_attrs[box_idx].zp,
                app_ctx->output_attrs[box_idx].scale, (int8_t *)_outputs[score_idx].buf,
                app_ctx->output_attrs[score_idx].zp, app_ctx->output_attrs[score_idx].scale,
                (int8_t *)score_sum, score_sum_zp, score_sum_scale, grid_h, grid_w, stride,
                dfl_len, class_num, filterBoxes, objProbs, classId, conf_threshold);
        } else {
            validCount += process_fp32((float *)_outputs[box_idx].buf,
                                       (float *)_outputs[score_idx].buf, (float *)score_sum,
                                       grid_h, grid_w, stride, dfl_len, class_num, filterBoxes,
                                       objProbs, classId, conf_threshold);
        }
    }

    if (validCount <= 0) {
        return 0;
    }

    std::vector<int> indexArray;
    for (int i = 0; i < validCount; ++i) {
        indexArray.push_back(i);
    }
    quick_sort_indice_inverse(objProbs, 0, validCount - 1, indexArray);

    std::set<int> class_set(std::begin(classId), std::end(classId));
    for (auto c : class_set) {
        nms(validCount, filterBoxes, classId, indexArray, c, nms_threshold);
    }

    int last_count = 0;
    od_results->count = 0;
    for (int i = 0; i < validCount; ++i) {
        if (indexArray[i] == -1 || last_count >= OBJ_NUMB_MAX_SIZE) {
            continue;
        }
        int n = indexArray[i];

        float x1 = filterBoxes[n * 4 + 0] - letter_box->x_pad;
        float y1 = filterBoxes[n * 4 + 1] - letter_box->y_pad;
        float x2 = x1 + filterBoxes[n * 4 + 2];
        float y2 = y1 + filterBoxes[n * 4 + 3];
        int id = classId[n];
        float obj_conf = objProbs[n];

        od_results->results[last_count].box.left =
            clamp_int(x1, 0, model_in_w) / letter_box->scale;
        od_results->results[last_count].box.top =
            clamp_int(y1, 0, model_in_h) / letter_box->scale;
        od_results->results[last_count].box.right =
            clamp_int(x2, 0, model_in_w) / letter_box->scale;
        od_results->results[last_count].box.bottom =
            clamp_int(y2, 0, model_in_h) / letter_box->scale;
        od_results->results[last_count].prop = obj_conf;
        od_results->results[last_count].cls_id = id;
        last_count++;
    }
    od_results->count = last_count;
    return 0;
}

extern "C" int plate_init_post_process(void)
{
    return 0;
}

extern "C" void plate_deinit_post_process(void)
{
}

extern "C" const char *plate_cls_to_name(int cls_id)
{
    if (cls_id < 0) {
        return "null";
    }
    if (cls_id < OBJ_CLASS_NUM) {
        return kPlateLabels[cls_id];
    }
    return "plate";
}
