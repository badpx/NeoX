/* ===========================================================================
 * ex01_scene.c — NeoX 固定演示场景（Phase 1 最小子集全覆盖）
 *
 * 内容：贝塞尔填充路径、自相交星形（EVENODD）、半透明组（两个重叠矩形）、
 *       八边形裁剪、程序生成棋盘纹理（LINEAR）、A8 伪字形条。
 * 输出：内存帧缓冲 → 24bit 无压缩 BMP（自写 writer）→ build/ex01_scene.bmp
 * =========================================================================*/
#include "neox.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 512
#define H 512

/* ------------------------------------------------------------------ BMP writer（24bit 无压缩，自下而上 BGR 行，4 字节对齐） */
static int write_bmp(const char *path, const uint8_t *rgba, uint32_t w,
                     uint32_t h)
{
    FILE *f = fopen(path, "wb");
    uint32_t row_bytes = w * 3;
    uint32_t pad = (4 - (row_bytes % 4)) % 4;
    uint32_t img_size = (row_bytes + pad) * h;
    uint8_t header[54];
    int x, y;
    if (!f) return -1;
    memset(header, 0, sizeof(header));
    header[0] = 'B'; header[1] = 'M';
    {
        uint32_t file_size = 54 + img_size;
        uint32_t off = 54;
        int32_t sw = (int32_t)w, sh = (int32_t)h;
        memcpy(header + 2, &file_size, 4);
        memcpy(header + 10, &off, 4);
        header[14] = 40; /* BITMAPINFOHEADER */
        memcpy(header + 18, &sw, 4);
        memcpy(header + 22, &sh, 4);
        header[26] = 1;  /* planes */
        header[28] = 24; /* bpp */
        memcpy(header + 34, &img_size, 4);
    }
    if (fwrite(header, 1, 54, f) != 54) { fclose(f); return -1; }
    for (y = (int)h - 1; y >= 0; y--) {
        for (x = 0; x < (int)w; x++) {
            const uint8_t *p = &rgba[((size_t)y * w + (size_t)x) * 4];
            uint8_t bgr[3] = { p[2], p[1], p[0] };
            if (fwrite(bgr, 1, 3, f) != 3) { fclose(f); return -1; }
        }
        if (pad) {
            static const uint8_t zeros[3] = { 0, 0, 0 };
            if (fwrite(zeros, 1, pad, f) != pad) { fclose(f); return -1; }
        }
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ 演示场景构建 */
static neox_path make_blob(neox_device *dev)
{
    /* 三次贝塞尔闭合形（NONZERO） */
    static const uint8_t verbs[] = {
        NEOX_VERB_MOVE, NEOX_VERB_CUBIC, NEOX_VERB_CUBIC,
        NEOX_VERB_CUBIC, NEOX_VERB_CLOSE
    };
    static const float pts[] = {
        30, 160,   90, 20,   200, 60,   250, 30,
        300, 120,   260, 220,   180, 240,
        60, 200,   30, 160,   30, 160
    };
    neox_path_desc d;
    neox_path p = { 0 };
    memset(&d, 0, sizeof(d));
    d.verbs = verbs; d.points = pts;
    d.verb_count = 5; d.point_count = 10;
    d.fill_rule = NEOX_FILL_NONZERO;
    neox_path_create(dev, &d, &p);
    return p;
}

static neox_path make_star(neox_device *dev, neox_fill_rule rule)
{
    /* 10 顶点自相交五角星 */
    uint8_t verbs[11];
    float pts[20];
    uint32_t i;
    neox_path_desc d;
    neox_path p = { 0 };
    for (i = 0; i < 5; i++) {
        float a1 = (float)i * 4.0f * 3.14159265f / 5.0f - 1.5707963f;
        float a2 = a1 + 0.62831853f;
        pts[i * 4 + 0] = 70.0f * cosf(a1);
        pts[i * 4 + 1] = 70.0f * sinf(a1);
        pts[i * 4 + 2] = 30.0f * cosf(a2);
        pts[i * 4 + 3] = 30.0f * sinf(a2);
        verbs[i * 2] = NEOX_VERB_LINE;
        verbs[i * 2 + 1] = NEOX_VERB_LINE;
    }
    verbs[0] = NEOX_VERB_MOVE;
    verbs[10] = NEOX_VERB_CLOSE;
    memset(&d, 0, sizeof(d));
    d.verbs = verbs; d.points = pts;
    d.verb_count = 11; d.point_count = 10;
    d.fill_rule = rule;
    neox_path_create(dev, &d, &p);
    return p;
}

static neox_path make_rect(neox_device *dev, float x, float y, float w, float h)
{
    static const uint8_t verbs[] = {
        NEOX_VERB_MOVE, NEOX_VERB_LINE, NEOX_VERB_LINE, NEOX_VERB_LINE, NEOX_VERB_CLOSE
    };
    float pts[8] = { x, y, x + w, y, x + w, y + h, x, y + h };
    neox_path_desc d;
    neox_path p = { 0 };
    memset(&d, 0, sizeof(d));
    d.verbs = verbs; d.points = pts;
    d.verb_count = 5; d.point_count = 4;
    d.fill_rule = NEOX_FILL_NONZERO;
    neox_path_create(dev, &d, &p);
    return p;
}

static neox_path make_octagon(neox_device *dev, float cx, float cy, float r)
{
    uint8_t verbs[9];
    float pts[16];
    uint32_t i;
    neox_path_desc d;
    neox_path p = { 0 };
    for (i = 0; i < 8; i++) {
        float a = (float)i * 3.14159265f / 4.0f;
        pts[i * 2] = cx + r * cosf(a);
        pts[i * 2 + 1] = cy + r * sinf(a);
        verbs[i] = NEOX_VERB_LINE;
    }
    verbs[0] = NEOX_VERB_MOVE;
    verbs[8] = NEOX_VERB_CLOSE;
    memset(&d, 0, sizeof(d));
    d.verbs = verbs; d.points = pts;
    d.verb_count = 9; d.point_count = 8;
    d.fill_rule = NEOX_FILL_NONZERO;
    neox_path_create(dev, &d, &p);
    return p;
}

static neox_texture make_checker(neox_device *dev, uint32_t n, uint32_t cell)
{
    /* 程序生成棋盘纹理（sRGB RGBA8，n x n 格） */
    uint32_t size = n * cell;
    uint8_t *px = (uint8_t *)malloc((size_t)size * size * 4);
    uint32_t x, y;
    neox_texture_desc d;
    neox_texture t = { 0 };
    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            int c = ((x / cell) + (y / cell)) & 1;
            uint8_t *p = &px[((size_t)y * size + x) * 4];
            p[0] = c ? 235 : 40;
            p[1] = c ? 190 : 60;
            p[2] = c ? 60 : 190;
            p[3] = 255;
        }
    }
    memset(&d, 0, sizeof(d));
    d.width = size; d.height = size;
    d.format = NEOX_FORMAT_RGBA8_SRGB;
    d.pixels = px;
    d.row_stride = (size_t)size * 4;
    neox_texture_create(dev, &d, &t);
    free(px);
    return t;
}

static neox_texture make_glyph_atlas(neox_device *dev)
{
    /* A8 伪字形图集：5 个 12x16 块字（伪随机笔触，确定性生成） */
    uint32_t W8 = 64, H8 = 16, x, y, g;
    uint8_t *px = (uint8_t *)calloc((size_t)W8 * H8, 1);
    neox_texture_desc d;
    neox_texture t = { 0 };
    for (g = 0; g < 5; g++) {
        uint32_t ox = 2 + g * 12;
        for (y = 0; y < 16; y++) {
            for (x = 0; x < 8; x++) {
                int on;
                if (y == 0 || y == 15 || x == 0 || x == 7)
                    on = 1; /* 外框 */
                else
                    on = ((x * 7 + y * 13 + g * 29) % 11) < 4; /* 确定性伪笔触 */
                if (on)
                    px[(size_t)y * W8 + ox + x] = 255;
            }
        }
    }
    memset(&d, 0, sizeof(d));
    d.width = W8; d.height = H8;
    d.format = NEOX_FORMAT_A8;
    d.pixels = px;
    d.row_stride = W8;
    neox_texture_create(dev, &d, &t);
    free(px);
    return t;
}

/* 计时与内存计数 */
typedef struct {
    uint64_t live, peak, outstanding;
} mem_stat;

static void *m_alloc(void *user, size_t size, size_t align)
{
    mem_stat *s = (mem_stat *)user;
    void *p;
    (void)align;
    p = malloc(size);
    if (!p) return NULL;
    s->live += size;
    if (s->live > s->peak) s->peak = s->live;
    s->outstanding++;
    return p;
}
static void m_free(void *user, void *ptr, size_t size, size_t align)
{
    mem_stat *s = (mem_stat *)user;
    (void)align;
    if (!ptr) return;
    s->live -= size;
    s->outstanding--;
    free(ptr);
}

int main(void)
{
    neox_context_desc cd;
    neox_device_desc dd;
    neox_context *ctx;
    neox_device *dev;
    static uint8_t pixels[W * H * 4];
    neox_frame *fr;
    neox_submission sub;
    neox_exec_report report;
    neox_path blob, star, clip, r1, r2;
    neox_texture checker, atlas;
    mem_stat stat;
    neox_allocator al;
    clock_t t0, t1;
    double ms;

    memset(&stat, 0, sizeof(stat));
    memset(&cd, 0, sizeof(cd));
    al.user = &stat;
    al.alloc = m_alloc;
    al.free = m_free;
    cd.allocator = &al;
    if (neox_context_create(&cd, &ctx) != NEOX_OK) {
        fprintf(stderr, "context_create failed\n");
        return 1;
    }
    memset(&dd, 0, sizeof(dd));
    dd.backend = NEOX_BACKEND_SOFT;
    if (neox_device_create(ctx, &dd, &dev) != NEOX_OK) {
        fprintf(stderr, "device_create failed\n");
        return 1;
    }

    blob = make_blob(dev);
    star = make_star(dev, NEOX_FILL_EVENODD);
    clip = make_octagon(dev, 340.0f, 300.0f, 110.0f);
    r1 = make_rect(dev, 0, 0, 130, 90);
    r2 = make_rect(dev, 55, 40, 130, 90);
    checker = make_checker(dev, 8, 8);
    atlas = make_glyph_atlas(dev);

    {
        neox_target_desc td;
        neox_state st;
        neox_paint pt;
        neox_glyph glyphs[5];
        uint32_t i;
        neox_color clear = { 0.09f, 0.10f, 0.14f, 1.0f };

        memset(&td, 0, sizeof(td));
        td.kind = NEOX_BACKEND_SOFT;
        td.soft.pixels = pixels;
        td.soft.stride = (size_t)W * 4;
        td.soft.width = W;
        td.soft.height = H;
        td.clear = &clear;
        if (neox_frame_begin(dev, &td, &fr) != NEOX_OK) {
            fprintf(stderr, "frame_begin failed\n");
            return 1;
        }

        /* 1) 贝塞尔填充路径（青色半透明） */
        pt.color = (neox_color){ 0.1f, 0.75f, 0.8f, 0.85f };
        memset(&st, 0, sizeof(st));
        st.transform = (neox_affine){ 1.3f, 0, 0, 1.3f, 20, 30 };
        st.opacity = 1.0f;
        neox_push_state(fr, &st);
        neox_draw_path(fr, blob, &pt);
        neox_pop_state(fr);

        /* 2) 自相交星形（EVENODD，黄色） */
        pt.color = (neox_color){ 0.95f, 0.85f, 0.15f, 1.0f };
        memset(&st, 0, sizeof(st));
        st.transform = (neox_affine){ 1, 0, 0, 1, 140, 380 };
        st.opacity = 1.0f;
        neox_push_state(fr, &st);
        neox_draw_path(fr, star, &pt);
        neox_pop_state(fr);

        /* 3) 半透明组：两个重叠矩形（组 opacity=0.65） */
        memset(&st, 0, sizeof(st));
        st.transform = (neox_affine){ 1, 0, 0, 1, 60, 200 };
        st.opacity = 0.65f;
        neox_push_state(fr, &st);
        pt.color = (neox_color){ 0.9f, 0.25f, 0.3f, 1.0f };
        neox_draw_path(fr, r1, &pt);
        pt.color = (neox_color){ 0.2f, 0.45f, 0.95f, 1.0f };
        neox_draw_path(fr, r2, &pt);
        neox_pop_state(fr);

        /* 4) 裁剪（八边形）内的棋盘纹理（LINEAR） */
        memset(&st, 0, sizeof(st));
        st.transform = (neox_affine){ 1, 0, 0, 1, 0, 0 };
        st.opacity = 1.0f;
        st.clip = &clip;
        neox_push_state(fr, &st);
        neox_draw_texture(fr, checker, (neox_rect){ 0, 0, 64, 64 },
                          (neox_rect){ 230, 230, 220, 140 },
                          NEOX_FILTER_LINEAR, 1.0f);
        neox_pop_state(fr);

        /* 5) A8 伪字形条（绿色） */
        for (i = 0; i < 5; i++) {
            glyphs[i].x = 40.0f + (float)i * 40.0f;
            glyphs[i].y = 60.0f;
            glyphs[i].sx = (uint16_t)(2 + i * 12);
            glyphs[i].sy = 0;
            glyphs[i].sw = 8;
            glyphs[i].sh = 16;
        }
        pt.color = (neox_color){ 0.35f, 0.9f, 0.45f, 1.0f };
        memset(&st, 0, sizeof(st));
        st.transform = (neox_affine){ 2.2f, 0, 0, 2.2f, 0, 0 };
        st.opacity = 1.0f;
        neox_push_state(fr, &st);
        neox_draw_glyphs(fr, atlas, glyphs, 5, &pt);
        neox_pop_state(fr);

        if (neox_frame_end(fr, &sub) != NEOX_OK) {
            fprintf(stderr, "frame_end failed\n");
            return 1;
        }
    }

    /* 计时：pump 驱动执行（含光栅化与写回） */
    t0 = clock();
    neox_device_pump(dev);
    t1 = clock();
    ms = (double)(t1 - t0) * 1000.0 / (double)CLOCKS_PER_SEC;

    if (neox_submission_get_report(dev, sub, &report) == NEOX_OK) {
        printf("draw_calls=%u algorithm=%d peak_bytes=%llu\n",
               report.draw_calls, (int)report.algorithm,
               (unsigned long long)report.peak_bytes);
    }
    printf("render %dx%d in %.2f ms (alloc peak %llu bytes, live %llu)\n",
           W, H, ms, (unsigned long long)stat.peak,
           (unsigned long long)stat.live);

    if (write_bmp("ex01_scene.bmp", pixels, W, H) != 0) {
        fprintf(stderr, "bmp write failed\n");
        return 1;
    }
    printf("wrote ex01_scene.bmp\n");

    neox_texture_release(dev, checker);
    neox_texture_release(dev, atlas);
    neox_path_release(dev, blob);
    neox_path_release(dev, star);
    neox_path_release(dev, clip);
    neox_path_release(dev, r1);
    neox_path_release(dev, r2);
    neox_device_pump(dev);
    neox_device_destroy(dev);
    neox_context_destroy(ctx);
    if (stat.outstanding != 0) {
        fprintf(stderr, "leak: %llu blocks\n",
                (unsigned long long)stat.outstanding);
        return 1;
    }
    return 0;
}
