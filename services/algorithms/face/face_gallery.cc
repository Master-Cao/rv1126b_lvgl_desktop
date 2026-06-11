#include "face_gallery.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FACE_GALLERY_MAGIC 0x4c414746u /* "FGAL" little-endian */

static void trim_line(char *s)
{
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) {
        s[--n] = '\0';
    }
}

static float cosine_similarity(const float *a, const float *b, int dim)
{
    double dot = 0.0;
    double na = 0.0;
    double nb = 0.0;
    for (int i = 0; i < dim; i++) {
        dot += (double)a[i] * (double)b[i];
        na += (double)a[i] * (double)a[i];
        nb += (double)b[i] * (double)b[i];
    }
    if (na < 1e-12 || nb < 1e-12) {
        return 0.0f;
    }
    return (float)(dot / (sqrt(na) * sqrt(nb)));
}

extern "C" int face_gallery_load(const char *gallery_dir, face_gallery_t *gallery)
{
    if (!gallery_dir || !gallery) {
        return -1;
    }
    memset(gallery, 0, sizeof(*gallery));

    char bin_path[512];
    char txt_path[512];
    snprintf(bin_path, sizeof(bin_path), "%s/face_encoding.bin", gallery_dir);
    snprintf(txt_path, sizeof(txt_path), "%s/gallery.txt", gallery_dir);

    FILE *fp = fopen(bin_path, "rb");
    if (!fp) {
        printf("face_gallery: open fail: %s\n", bin_path);
        return -1;
    }

    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t count = 0;
    uint32_t dim = 0;
    if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != FACE_GALLERY_MAGIC ||
        fread(&version, sizeof(version), 1, fp) != 1 || version != 1 ||
        fread(&count, sizeof(count), 1, fp) != 1 || fread(&dim, sizeof(dim), 1, fp) != 1) {
        fclose(fp);
        printf("face_gallery: invalid bin header: %s\n", bin_path);
        return -1;
    }
    if (count <= 0 || dim <= 0 || dim > 4096) {
        fclose(fp);
        return -1;
    }

    size_t embed_bytes = (size_t)count * (size_t)dim * sizeof(float);
    float *embeddings = (float *)malloc(embed_bytes);
    if (!embeddings) {
        fclose(fp);
        return -1;
    }
    if (fread(embeddings, 1, embed_bytes, fp) != embed_bytes) {
        free(embeddings);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    int *person_ids = (int *)calloc((size_t)count, sizeof(int));
    char (*names)[FACE_NAME_MAX_LEN] = (char (*)[FACE_NAME_MAX_LEN])calloc(
        (size_t)count, sizeof(*names));
    if (!person_ids || !names) {
        free(embeddings);
        free(person_ids);
        free(names);
        return -1;
    }

    for (int i = 0; i < (int)count; i++) {
        person_ids[i] = i;
        snprintf(names[i], FACE_NAME_MAX_LEN, "person_%d", i);
    }

    FILE *ft = fopen(txt_path, "r");
    if (ft) {
        char line[256];
        while (fgets(line, sizeof(line), ft)) {
            trim_line(line);
            if (!line[0] || line[0] == '#') {
                continue;
            }
            int id = -1;
            char name[FACE_NAME_MAX_LEN];
            name[0] = '\0';
            if (sscanf(line, "%d,%63[^\n]", &id, name) >= 2 && id >= 0 && id < (int)count) {
                person_ids[id] = id;
                strncpy(names[id], name, FACE_NAME_MAX_LEN - 1);
                names[id][FACE_NAME_MAX_LEN - 1] = '\0';
            }
        }
        fclose(ft);
    } else {
        printf("face_gallery: warn no gallery.txt, use default names\n");
    }

    gallery->count = (int)count;
    gallery->dim = (int)dim;
    gallery->embeddings = embeddings;
    gallery->person_ids = person_ids;
    gallery->names = names;
    gallery->loaded = true;
    printf("face_gallery: loaded %d persons dim=%d from %s\n", gallery->count, gallery->dim,
           gallery_dir);
    return 0;
}

extern "C" void face_gallery_free(face_gallery_t *gallery)
{
    if (!gallery) {
        return;
    }
    free(gallery->embeddings);
    free(gallery->person_ids);
    free(gallery->names);
    memset(gallery, 0, sizeof(*gallery));
}

extern "C" int face_gallery_match(const face_gallery_t *gallery, const float *query_embed,
                                  float threshold, int *out_person_id, char *out_name,
                                  int out_name_size, float *out_score)
{
    if (out_person_id) {
        *out_person_id = -1;
    }
    if (out_name && out_name_size > 0) {
        out_name[0] = '\0';
    }
    if (out_score) {
        *out_score = 0.0f;
    }
    if (!gallery || !gallery->loaded || !query_embed || gallery->count <= 0) {
        return -1;
    }

    int best_idx = -1;
    float best_score = -1.0f;
    for (int i = 0; i < gallery->count; i++) {
        const float *feat = gallery->embeddings + (size_t)i * (size_t)gallery->dim;
        float score = cosine_similarity(query_embed, feat, gallery->dim);
        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }

    if (best_idx < 0) {
        return -1;
    }

    /* 始终返回最佳相似度，便于调试阈值；姓名仅在超过 threshold 时填充。 */
    if (out_score) {
        *out_score = best_score;
    }

    if (best_score < threshold) {
        return 0;
    }

    if (out_person_id) {
        *out_person_id = gallery->person_ids[best_idx];
    }
    if (out_name && out_name_size > 0) {
        strncpy(out_name, gallery->names[best_idx], (size_t)out_name_size - 1);
        out_name[out_name_size - 1] = '\0';
    }
    return 1;
}
