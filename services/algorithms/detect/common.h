#ifndef _RKNN_MODEL_ZOO_COMMON_H_
#define _RKNN_MODEL_ZOO_COMMON_H_

/* yolov8 demo 公共类型；与 ocr/common.h 保持完全一致，使用相同的 include guard
 * 让单个翻译单元里两份头同时存在时也只解析一次。 */

typedef enum {
    IMAGE_FORMAT_GRAY8,
    IMAGE_FORMAT_RGB888,
    IMAGE_FORMAT_RGBA8888,
    IMAGE_FORMAT_YUV420SP_NV21,
    IMAGE_FORMAT_YUV420SP_NV12,
} image_format_t;

typedef struct {
    int width;
    int height;
    int width_stride;
    int height_stride;
    image_format_t format;
    unsigned char *virt_addr;
    int size;
    int fd;
} image_buffer_t;

typedef struct {
    int left;
    int top;
    int right;
    int bottom;
} image_rect_t;

#endif /* _RKNN_MODEL_ZOO_COMMON_H_ */
