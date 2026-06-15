/*
 * 板端车牌检测离线测试（直接调用 yolov8_plate，不依赖 LVGL / 相机）。
 *
 * 交叉编译后产物: build/Release/plate_test
 * 板端示例:
 *   ./plate_test -i /tmp/plate.jpg
 *   ./plate_test -m /opt/rv1126b_desktop/models/plate_det.rknn -i test.jpg -o out.jpg
 *   PLATE_DET_MODEL=/data/plate.rknn ./plate_test -i test.jpg --conf 0.15
 */
#include "yolov8_plate.h"

#include <opencv2/opencv.hpp>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define DEFAULT_MODEL "/opt/rv1126b_desktop/models/plate_det.rknn"
#define ALT_MODEL     "/opt/rv1126b_desktop/models/plate.rknn"

static double now_ms()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static void print_usage(const char *prog)
{
    printf("用法: %s [选项]\n", prog);
    printf("  -m, --model <path>   RKNN 模型 (默认 plate_det.rknn / plate.rknn / PLATE_DET_MODEL)\n");
    printf("  -i, --image <path>   输入图片 (jpg/png/bmp)\n");
    printf("  -o, --output <path>  画框结果图 (默认 plate_test_out.jpg)\n");
    printf("  --conf <float>       置信度阈值 (默认 %.2f)\n", BOX_THRESH);
    printf("  --nms <float>        NMS IoU 阈值 (默认 %.2f)\n", NMS_THRESH);
    printf("  -h, --help           显示帮助\n");
    printf("\n示例:\n");
    printf("  %s -i /tmp/plate.jpg\n", prog);
    printf("  %s -m /opt/rv1126b_desktop/models/plate_det.rknn -i test.jpg -o vis.jpg\n", prog);
}

static const char *resolve_model_path(const char *cli)
{
    if (cli && cli[0]) {
        return cli;
    }
    const char *env = getenv("PLATE_DET_MODEL");
    if (env && env[0]) {
        return env;
    }
    FILE *fp = fopen(DEFAULT_MODEL, "rb");
    if (fp) {
        fclose(fp);
        return DEFAULT_MODEL;
    }
    fp = fopen(ALT_MODEL, "rb");
    if (fp) {
        fclose(fp);
        return ALT_MODEL;
    }
    return DEFAULT_MODEL;
}

static int run_inference(rknn_app_context_t *ctx, const cv::Mat &rgb, float conf, float nms,
                         object_detect_result_list *out, int *infer_ms)
{
    if (!rgb.isContinuous() || rgb.type() != CV_8UC3) {
        fprintf(stderr, "ERROR: 需要连续 RGB888 图像\n");
        return -1;
    }

    image_buffer_t src;
    memset(&src, 0, sizeof(src));
    src.width = rgb.cols;
    src.height = rgb.rows;
    src.width_stride = (int)rgb.step[0];
    src.virt_addr = rgb.data;
    src.format = IMAGE_FORMAT_RGB888;
    src.size = rgb.cols * rgb.rows * 3;

    double t0 = now_ms();
    int ret = plate_inference_yolov8_model_ex(ctx, &src, out, conf, nms);
    *infer_ms = (int)(now_ms() - t0);
    if (ret < 0) {
        return ret;
    }

    (void)conf;
    (void)nms;
    return 0;
}

int main(int argc, char **argv)
{
    const char *model_path = NULL;
    const char *image_path = NULL;
    const char *out_path = "plate_test_out.jpg";
    float conf = BOX_THRESH;
    float nms = NMS_THRESH;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_usage(argv[0]);
            return 0;
        }
        if ((!strcmp(argv[i], "-m") || !strcmp(argv[i], "--model")) && i + 1 < argc) {
            model_path = argv[++i];
        } else if ((!strcmp(argv[i], "-i") || !strcmp(argv[i], "--image")) && i + 1 < argc) {
            image_path = argv[++i];
        } else if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) && i + 1 < argc) {
            out_path = argv[++i];
        } else if (!strcmp(argv[i], "--conf") && i + 1 < argc) {
            conf = (float)atof(argv[++i]);
        } else if (!strcmp(argv[i], "--nms") && i + 1 < argc) {
            nms = (float)atof(argv[++i]);
        } else {
            fprintf(stderr, "未知参数: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!image_path) {
        fprintf(stderr, "ERROR: 请指定 -i <图片>\n\n");
        print_usage(argv[0]);
        return 1;
    }

    model_path = resolve_model_path(model_path);
    printf("======== plate_test ========\n");
    printf("模型: %s\n", model_path);
    printf("图片: %s\n", image_path);
    printf("输出: %s\n", out_path);
    printf("conf=%.3f  nms=%.3f\n", conf, nms);
    printf("============================\n");

    if (plate_init_post_process() != 0) {
        fprintf(stderr, "ERROR: plate_init_post_process fail\n");
        return 1;
    }

    rknn_app_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (plate_init_yolov8_model(model_path, &ctx) != 0) {
        fprintf(stderr, "ERROR: 加载模型失败: %s\n", model_path);
        plate_deinit_post_process();
        return 1;
    }

    printf("\n模型摘要: output_num=%u class_num=%d is_quant=%d input=%dx%dx%d\n\n",
           ctx.io_num.n_output, ctx.model_class_num, ctx.is_quant ? 1 : 0, ctx.model_width,
           ctx.model_height, ctx.model_channel);

    if (ctx.io_num.n_output == 1 && ctx.model_class_num != 1) {
        printf("WARN: 单路输出但 class_num=%d，期望 1（旧版后处理可能误解析为 anchor 数）\n",
               ctx.model_class_num);
    }

    cv::Mat bgr = cv::imread(image_path, cv::IMREAD_COLOR);
    if (bgr.empty()) {
        fprintf(stderr, "ERROR: 无法读取图片: %s\n", image_path);
        plate_release_yolov8_model(&ctx);
        plate_deinit_post_process();
        return 1;
    }

    cv::Mat rgb;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);

    object_detect_result_list results;
    memset(&results, 0, sizeof(results));
    int infer_ms = 0;

    int ret = run_inference(&ctx, rgb, conf, nms, &results, &infer_ms);
    if (ret < 0) {
        fprintf(stderr, "ERROR: 推理失败 ret=%d\n", ret);
        plate_release_yolov8_model(&ctx);
        plate_deinit_post_process();
        return 1;
    }

    printf("推理: %d ms\n", infer_ms);
    printf("检出: %d 个目标\n", results.count);
    for (int i = 0; i < results.count; i++) {
        const object_detect_result &r = results.results[i];
        const char *name = plate_cls_to_name(r.cls_id);
        printf("  [%d] %s  %.1f%%  (%d,%d)-(%d,%d)\n", i, name, r.prop * 100.0f, r.box.left,
               r.box.top, r.box.right, r.box.bottom);

        cv::rectangle(bgr, cv::Point(r.box.left, r.box.top), cv::Point(r.box.right, r.box.bottom),
                      cv::Scalar(0, 255, 0), 2);
        char label[64];
        snprintf(label, sizeof(label), "%s %.0f%%", name, r.prop * 100.0f);
        int y = r.box.top > 20 ? r.box.top - 5 : r.box.top + 15;
        cv::putText(bgr, label, cv::Point(r.box.left, y), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                    cv::Scalar(0, 255, 0), 2);
    }

    if (!cv::imwrite(out_path, bgr)) {
        fprintf(stderr, "WARN: 保存结果图失败: %s\n", out_path);
    } else {
        printf("可视化已保存: %s\n", out_path);
    }

    if (results.count == 0) {
        printf("\n未检出 — 请确认:\n");
        printf("  1) 日志 class_num 应为 1（不是 8400）\n");
        printf("  2) output_num 应为 1（Ultralytics）或 9（rknn_model_zoo）\n");
        printf("  3) 已部署含单路后处理修复的最新 plate_test / rv1126b_desktop\n");
    }

    plate_release_yolov8_model(&ctx);
    plate_deinit_post_process();
    return results.count > 0 ? 0 : 2;
}
