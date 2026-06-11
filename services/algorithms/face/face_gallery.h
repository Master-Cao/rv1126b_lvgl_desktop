#ifndef FACE_GALLERY_H
#define FACE_GALLERY_H

/*
 * 人脸底库：加载 PC 端 enroll.py 导出的 face_encoding.bin + gallery.txt。
 *
 * face_encoding.bin 格式:
 *   magic[4] = "FGAL"
 *   version  uint32 = 1
 *   count    uint32
 *   dim      uint32 (512)
 *   float32[count * dim]
 *
 * gallery.txt 每行: id,name
 */
#include "face_algo.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int count;
    int dim;
    float *embeddings;
    int *person_ids;
    char (*names)[FACE_NAME_MAX_LEN];
    bool loaded;
} face_gallery_t;

int face_gallery_load(const char *gallery_dir, face_gallery_t *gallery);

void face_gallery_free(face_gallery_t *gallery);

/* 1:N 余弦相似度检索；out_score 始终写入最佳相似度（无论是否超过 threshold）。 */
int face_gallery_match(const face_gallery_t *gallery, const float *query_embed, float threshold,
                       int *out_person_id, char *out_name, int out_name_size, float *out_score);

#ifdef __cplusplus
}
#endif

#endif /* FACE_GALLERY_H */
