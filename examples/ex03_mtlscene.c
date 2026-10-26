/* ===========================================================================
 * ex03_mtlscene.c — ex01 相同语义场景在 Metal 后端上的渲染 + 三方对比
 *
 * 输出（构建目录）：
 *   ex03_mtlscene.bmp   Metal 后端渲染结果（离屏表面 → read_pixels → BMP）
 *   ex03_compare.bmp    Soft | GL | Metal 三联并排供目检
 *
 * Metal 为真异步后端：pump 提交后经 neox_submission_wait 等待完成。
 * 无 GL/Metal 环境时打印提示并以 0 退出（示例冒烟测试不失败）。
 * =========================================================================*/
#include "neox.h"
#if NEOX_ENABLE_GL
#include "neox_gl.h"
#endif
#include "neox_mtl.h"

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

/* ------------------------------------------------------------------ 场景构建（与 ex01 相同的几何与语义） */
static neox_path make_blob(neox_device *dev)
{
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

/* 与 ex01_scene.c 完全相同的绘制命令序列（语义层一致性的目检对象） */
static void draw_demo_scene(neox_frame *fr,
                            neox_path blob, neox_path star, neox_path clip,
                            neox_path r1, neox_path r2,
                            neox_texture checker, neox_texture atlas)
{
    neox_state st;
    neox_paint pt;
    neox_glyph glyphs[5];
    uint32_t i;
    neox_color clear = { 0.09f, 0.10f, 0.14f, 1.0f };
    (void)clear;

    pt.color = (neox_color){ 0.1f, 0.75f, 0.8f, 0.85f };
    memset(&st, 0, sizeof(st));
    st.transform = (neox_affine){ 1.3f, 0, 0, 1.3f, 20, 30 };
    st.opacity = 1.0f;
    neox_push_state(fr, &st);
    neox_draw_path(fr, blob, &pt);
    neox_pop_state(fr);

    pt.color = (neox_color){ 0.95f, 0.85f, 0.15f, 1.0f };
    memset(&st, 0, sizeof(st));
    st.transform = (neox_affine){ 1, 0, 0, 1, 140, 380 };
    st.opacity = 1.0f;
    neox_push_state(fr, &st);
    neox_draw_path(fr, star, &pt);
    neox_pop_state(fr);

    memset(&st, 0, sizeof(st));
    st.transform = (neox_affine){ 1, 0, 0, 1, 60, 200 };
    st.opacity = 0.65f;
    neox_push_state(fr, &st);
    pt.color = (neox_color){ 0.9f, 0.25f, 0.3f, 1.0f };
    neox_draw_path(fr, r1, &pt);
    pt.color = (neox_color){ 0.2f, 0.45f, 0.95f, 1.0f };
    neox_draw_path(fr, r2, &pt);
    neox_pop_state(fr);

    memset(&st, 0, sizeof(st));
    st.transform = (neox_affine){ 1, 0, 0, 1, 0, 0 };
    st.opacity = 1.0f;
    st.clip = &clip;
    neox_push_state(fr, &st);
    neox_draw_texture(fr, checker, (neox_rect){ 0, 0, 64, 64 },
                      (neox_rect){ 230, 230, 220, 140 },
                      NEOX_FILTER_LINEAR, 1.0f);
    neox_pop_state(fr);

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
}

static int render_on_gpu(neox_device *dev, int use_mtl, uint8_t *out,
                         neox_color clear)
{
    neox_path blob, star, clip, r1, r2;
    neox_texture checker, atlas;
    neox_frame *fr;
    neox_submission sub;
    neox_exec_report rep;
    clock_t t0, t1;
    int rc = 0;
    blob = make_blob(dev);
    star = make_star(dev, NEOX_FILL_EVENODD);
    clip = make_octagon(dev, 340.0f, 300.0f, 110.0f);
    r1 = make_rect(dev, 0, 0, 130, 90);
    r2 = make_rect(dev, 55, 40, 130, 90);
    checker = make_checker(dev, 8, 8);
    atlas = make_glyph_atlas(dev);
    {
        neox_target_desc td;
        memset(&td, 0, sizeof(td));
        td.kind = NEOX_BACKEND_GPU;
        td.clear = &clear;
        if (use_mtl) {
            neox_mtl_surface *surf = NULL;
            if (neox_mtl_surface_create(dev, W, H, &surf) != NEOX_OK) rc = 1;
            else td.surface = surf;
            if (!rc && neox_frame_begin(dev, &td, &fr) != NEOX_OK) rc = 1;
            if (!rc) draw_demo_scene(fr, blob, star, clip, r1, r2, checker, atlas);
            if (!rc && neox_frame_end(fr, &sub) != NEOX_OK) rc = 1;
            t0 = clock();
            if (!rc) neox_device_pump(dev);
            if (!rc && neox_submission_wait(dev, sub) != NEOX_OK) rc = 1;
            t1 = clock();
            if (!rc && neox_submission_get_report(dev, sub, &rep) == NEOX_OK)
                printf("mtl:  %.2f ms (pump+wait, draw_calls=%u alg=%d)\n",
                       (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC,
                       rep.draw_calls, (int)rep.algorithm);
            if (!rc && neox_mtl_surface_read_pixels(surf, out) != NEOX_OK) rc = 1;
            if (surf) neox_mtl_surface_destroy(dev, surf);
        } else {
#if NEOX_ENABLE_GL
            neox_gl_surface *surf = NULL;
            if (neox_gl_surface_create(dev, W, H, &surf) != NEOX_OK) rc = 1;
            else td.surface = surf;
            if (!rc && neox_frame_begin(dev, &td, &fr) != NEOX_OK) rc = 1;
            if (!rc) draw_demo_scene(fr, blob, star, clip, r1, r2, checker, atlas);
            if (!rc && neox_frame_end(fr, &sub) != NEOX_OK) rc = 1;
            t0 = clock();
            if (!rc) neox_device_pump(dev);
            if (!rc) neox_device_pump(dev);
            t1 = clock();
            if (!rc && neox_submission_get_report(dev, sub, &rep) == NEOX_OK)
                printf("gl:   %.2f ms (pump, draw_calls=%u alg=%d)\n",
                       (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC,
                       rep.draw_calls, (int)rep.algorithm);
            if (!rc && neox_gl_surface_read_pixels(surf, out) != NEOX_OK) rc = 1;
            if (surf) neox_gl_surface_destroy(dev, surf);
#else
            rc = 1;   /* GL 后端未编译 */
#endif
        }
    }
    neox_texture_release(dev, checker);
    neox_texture_release(dev, atlas);
    neox_path_release(dev, blob);
    neox_path_release(dev, star);
    neox_path_release(dev, clip);
    neox_path_release(dev, r1);
    neox_path_release(dev, r2);
    neox_device_pump(dev);
    return rc;
}

int main(void)
{
    neox_context_desc cd;
    neox_device_desc dd;
    neox_context *ctx;
    neox_device *soft = NULL, *gpu = NULL, *mtl = NULL;
    static uint8_t soft_px[W * H * 4];
    static uint8_t gl_px[W * H * 4];
    static uint8_t mtl_px[W * H * 4];
    static uint8_t side[W * 3 * H * 4];
    neox_color clear = { 0.09f, 0.10f, 0.14f, 1.0f };

    memset(&cd, 0, sizeof(cd));
    if (neox_context_create(&cd, &ctx) != NEOX_OK) {
        fprintf(stderr, "context_create failed\n");
        return 1;
    }
    memset(&dd, 0, sizeof(dd));
    dd.backend = NEOX_BACKEND_SOFT;
    if (neox_device_create(ctx, &dd, &soft) != NEOX_OK) {
        fprintf(stderr, "soft device_create failed\n");
        return 1;
    }

    /* ---------------- Soft 黄金图 ---------------- */
    {
        neox_path blob, star, clip, r1, r2;
        neox_texture checker, atlas;
        neox_target_desc td;
        neox_frame *fr;
        neox_submission sub;
        clock_t t0, t1;

        blob = make_blob(soft);
        star = make_star(soft, NEOX_FILL_EVENODD);
        clip = make_octagon(soft, 340.0f, 300.0f, 110.0f);
        r1 = make_rect(soft, 0, 0, 130, 90);
        r2 = make_rect(soft, 55, 40, 130, 90);
        checker = make_checker(soft, 8, 8);
        atlas = make_glyph_atlas(soft);

        memset(&td, 0, sizeof(td));
        td.kind = NEOX_BACKEND_SOFT;
        td.soft.pixels = soft_px;
        td.soft.stride = (size_t)W * 4;
        td.soft.width = W;
        td.soft.height = H;
        td.clear = &clear;
        if (neox_frame_begin(soft, &td, &fr) != NEOX_OK) return 1;
        draw_demo_scene(fr, blob, star, clip, r1, r2, checker, atlas);
        if (neox_frame_end(fr, &sub) != NEOX_OK) return 1;
        t0 = clock();
        neox_device_pump(soft);
        t1 = clock();
        printf("soft: %.2f ms\n", (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC);

        neox_texture_release(soft, checker);
        neox_texture_release(soft, atlas);
        neox_path_release(soft, blob);
        neox_path_release(soft, star);
        neox_path_release(soft, clip);
        neox_path_release(soft, r1);
        neox_path_release(soft, r2);
        neox_device_pump(soft);
    }

    /* ---------------- GL ---------------- */
#if NEOX_ENABLE_GL
    {
        neox_gl_device_desc gld;
        memset(&gld, 0, sizeof(gld));
        gld.struct_size = (uint32_t)sizeof(gld);
        gld.magic = NEOX_GL_DESC_MAGIC;
        gld.shared_context = NULL;
        memset(&dd, 0, sizeof(dd));
        dd.backend = NEOX_BACKEND_GPU;
        dd.platform = &gld;
        dd.platform_size = sizeof(gld);
        if (neox_device_create(ctx, &dd, &gpu) != NEOX_OK) {
            printf("GL backend unavailable (%s)\n", neox_get_error_detail(ctx));
            gpu = NULL;
        } else if (render_on_gpu(gpu, 0, gl_px, clear) != 0) {
            fprintf(stderr, "gl render failed\n");
            return 1;
        }
    }

#endif
    /* ---------------- Metal（真异步） ---------------- */
    {
        neox_mtl_device_desc mtd;
        memset(&mtd, 0, sizeof(mtd));
        mtd.struct_size = (uint32_t)sizeof(mtd);
        mtd.magic = NEOX_MTL_DESC_MAGIC;
        mtd.device = NULL;
        mtd.command_queue = NULL;
        memset(&dd, 0, sizeof(dd));
        dd.backend = NEOX_BACKEND_GPU;
        dd.platform = &mtd;
        dd.platform_size = sizeof(mtd);
        if (neox_device_create(ctx, &dd, &mtl) != NEOX_OK) {
            printf("Metal backend unavailable (%s)\n",
                   neox_get_error_detail(ctx));
            mtl = NULL;
        } else if (render_on_gpu(mtl, 1, mtl_px, clear) != 0) {
            fprintf(stderr, "mtl render failed\n");
            return 1;
        }
    }

    if (mtl) {
        if (write_bmp("ex03_mtlscene.bmp", mtl_px, W, H) != 0) return 1;
        printf("wrote ex03_mtlscene.bmp\n");
    }
    /* 三联并排：Soft | GL | Metal */
    {
        uint32_t y;
        for (y = 0; y < H; y++) {
            memcpy(&side[(size_t)y * W * 3 * 4], &soft_px[(size_t)y * W * 4],
                   (size_t)W * 4);
            if (gpu)
                memcpy(&side[((size_t)y * W * 3 + W) * 4],
                       &gl_px[(size_t)y * W * 4], (size_t)W * 4);
            else
                memset(&side[((size_t)y * W * 3 + W) * 4], 0x33, (size_t)W * 4);
            if (mtl)
                memcpy(&side[((size_t)y * W * 3 + W * 2) * 4],
                       &mtl_px[(size_t)y * W * 4], (size_t)W * 4);
            else
                memset(&side[((size_t)y * W * 3 + W * 2) * 4], 0x33,
                       (size_t)W * 4);
        }
        for (y = 0; y < H; y++) { /* 分隔黑线 */
            memset(&side[((size_t)y * W * 3 + W - 1) * 4], 0, 8);
            memset(&side[((size_t)y * W * 3 + W * 2 - 1) * 4], 0, 8);
        }
    }
    if (write_bmp("ex03_compare.bmp", side, W * 3, H) != 0) return 1;
    printf("wrote ex03_compare.bmp (soft | gl | mtl)\n");

    if (mtl) neox_device_destroy(mtl);
    if (gpu) neox_device_destroy(gpu);
    neox_device_destroy(soft);
    neox_context_destroy(ctx);
    return 0;
}
