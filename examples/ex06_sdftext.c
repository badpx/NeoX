/* ===========================================================================
 * ex05_sdftext.c — SDF/MSDF 距离场文字演示（Soft 后端单幅）
 *
 * 内容：同一字体/文本在三种图集语义（§8.2）下的并排对比——
 *   上区 COVERAGE（A8 覆盖率）：16/24/40px；
 *   中区 SDF（A8 距离场，dr=4）：16/24/40px + 16px 字形 2.5x 放大（距离场
 *        的可缩放性；s 因子随变换缩放，边缘不糊成 texel 块）；
 *   下区 MSDF（RGBA8_LINEAR，dr=4）：16/24/40px。
 * 模式标签用覆盖率图集绘制（图例不自身参与对比）。
 * 输出（构建目录）：ex05_sdftext.bmp。字体缺失时打印提示并以 0 退出。
 * =========================================================================*/
#include "neox.h"
#include "neox_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 760
#define H 640

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
        header[14] = 40;
        memcpy(header + 18, &sw, 4);
        memcpy(header + 22, &sh, 4);
        header[26] = 1;
        header[28] = 24;
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

/* ------------------------------------------------------------------ 文字场景 */

static const char *SAMPLE =
    "The quick brown fox 0123456789 "
    "\xE5\xBF\xAB\xE7\x85\xA7\xE7\xBA\xA2\xE7\x8B\x90\xE8\xB7\xB3\xE8\xBF\x87"
    "\xE6\x87\x92\xE7\x8B\x97 SDF/MSDF";

/* (x,y) 为该行的基线位置（布局约定：首行基线 y=0）；xform 非 NULL 时
 * 先平移到基线原点 (x,y) 再经其缩放（s 因子演示）。
 * 返回占用高度（下一行基线起点） */
static float draw_line(nx_text_context *tctx, neox_frame *fr, nx_atlas *at,
                       nx_font *font, const char *text, float size,
                       float x, float y, neox_color color,
                       const neox_affine *xform, float softness)
{
    nx_layout *lo = NULL;
    neox_paint pt;
    neox_result r;
    neox_affine t;
    r = nx_text_layout(tctx, font, size, text, 0.0f, NX_ALIGN_LEFT, &lo);
    if (r != NEOX_OK) return 0.0f;
    if (xform) {
        /* t = xform ∘ translate(x, y)（布局 y=0 即首行基线） */
        t.a = xform->a; t.b = xform->b;
        t.c = xform->c; t.d = xform->d;
        t.tx = xform->tx + xform->a * x + xform->c * y;
        t.ty = xform->ty + xform->b * x + xform->d * y;
    } else {
        t = (neox_affine){ 1, 0, 0, 1, x, y };
    }
    {
        neox_state st;
        memset(&st, 0, sizeof(st));
        st.transform = t;
        st.opacity = 1.0f;
        r = neox_push_state(fr, &st);
    }
    if (r == NEOX_OK) {
        pt.color = color;
        r = softness > 0.0f ? nx_text_draw_ex(fr, at, lo, &pt, softness)
                            : nx_text_draw(fr, at, lo, &pt);
        neox_pop_state(fr);
    }
    if (r != NEOX_OK)
        fprintf(stderr, "ex05: draw failed (%d): %s\n", (int)r,
                nx_text_last_error(tctx) ? nx_text_last_error(tctx) : "-");
    nx_layout_destroy(lo);
    return r == NEOX_OK ? size * 1.6f : 0.0f;
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    neox_context_desc cd;
    neox_device_desc dd;
    neox_context *ctx = NULL;
    neox_device *soft = NULL;
    nx_text_context *tctx = NULL;
    nx_font *font = NULL;
    nx_atlas *at_cov = NULL, *at_sdf = NULL, *at_msdf = NULL;
    neox_frame *fr = NULL;
    neox_submission sub;
    neox_target_desc td;
    neox_color clear = { 0.93f, 0.93f, 0.95f, 1.0f };
    static uint8_t img[W * H * 4];
    float y;
    int rc = 1;

    static const struct { const char *path; int idx; } candidates[] = {
        { "/System/Library/Fonts/PingFang.ttc", 0 },
        { "/System/Library/Fonts/STHeiti Medium.ttc", 0 },
        { "/System/Library/Fonts/Songti.ttc", 0 },
        { "/System/Library/Fonts/Supplemental/Arial.ttf", 0 },
        { "/System/Library/Fonts/Helvetica.ttc", 0 }
    };
    uint32_t ci;

    memset(&cd, 0, sizeof(cd));
    if (neox_context_create(&cd, &ctx) != NEOX_OK) return 1;
    if (nx_text_context_create(ctx, NULL, &tctx) != NEOX_OK) return 1;

    for (ci = 0; ci < sizeof(candidates) / sizeof(candidates[0]) && !font; ci++) {
        FILE *fp = fopen(candidates[ci].path, "rb");
        if (!fp) continue;
        fclose(fp);
        if (nx_font_load_file(tctx, candidates[ci].path, candidates[ci].idx,
                              &font) != NEOX_OK)
            font = NULL;
    }
    if (!font) {
        printf("no system font found — ex05 skipped\n");
        nx_text_context_destroy(tctx);
        neox_context_destroy(ctx);
        return 0;
    }

    memset(&dd, 0, sizeof(dd));
    dd.backend = NEOX_BACKEND_SOFT;
    if (neox_device_create(ctx, &dd, &soft) != NEOX_OK) goto out;

    /* 三模式图集（模式创建时固定，§8.2 格式契约由模块保证） */
    if (nx_atlas_create(tctx, soft, 1024, 1024, &at_cov) != NEOX_OK) goto out;
    if (nx_atlas_create_ex(tctx, soft, 1024, 1024, NEOX_ATLAS_SDF, 4.0f,
                           &at_sdf) != NEOX_OK) goto out;
    if (nx_atlas_create_ex(tctx, soft, 1024, 1024, NEOX_ATLAS_MSDF, 4.0f,
                           &at_msdf) != NEOX_OK) goto out;

    memset(&td, 0, sizeof(td));
    td.kind = NEOX_BACKEND_SOFT;
    td.soft.pixels = img;
    td.soft.stride = (size_t)W * 4;
    td.soft.width = W;
    td.soft.height = H;
    td.clear = &clear;
    if (neox_frame_begin(soft, &td, &fr) != NEOX_OK) goto out;

    /* 上区：COVERAGE */
    y = 30.0f;
    draw_line(tctx, fr, at_cov, font, "COVERAGE (A8)", 14.0f, 24, y,
              (neox_color){ 0.55f, 0.20f, 0.15f, 1.0f }, NULL, 0.0f);
    y += 28.0f;
    y += draw_line(tctx, fr, at_cov, font, SAMPLE, 16.0f, 24, y,
                   (neox_color){ 0.12f, 0.14f, 0.20f, 1.0f }, NULL, 0.0f);
    y += draw_line(tctx, fr, at_cov, font, SAMPLE, 24.0f, 24, y,
                   (neox_color){ 0.12f, 0.14f, 0.20f, 1.0f }, NULL, 0.0f);
    y += draw_line(tctx, fr, at_cov, font, SAMPLE, 40.0f, 24, y,
                   (neox_color){ 0.12f, 0.14f, 0.20f, 1.0f }, NULL, 0.0f);

    /* 中区：SDF（含 2.5x 缩放演示） */
    y += 30.0f;
    draw_line(tctx, fr, at_cov, font, "SDF (A8, dist_range 4)", 14.0f, 24, y,
              (neox_color){ 0.15f, 0.45f, 0.20f, 1.0f }, NULL, 0.0f);
    y += 28.0f;
    y += draw_line(tctx, fr, at_sdf, font, SAMPLE, 16.0f, 24, y,
                   (neox_color){ 0.12f, 0.14f, 0.20f, 1.0f }, NULL, 0.0f);
    y += draw_line(tctx, fr, at_sdf, font, SAMPLE, 24.0f, 24, y,
                   (neox_color){ 0.12f, 0.14f, 0.20f, 1.0f }, NULL, 0.0f);
    y += draw_line(tctx, fr, at_sdf, font, SAMPLE, 40.0f, 24, y,
                   (neox_color){ 0.12f, 0.14f, 0.20f, 1.0f }, NULL, 0.0f);
    {
        /* 16px 布局字形经 2.5x 变换放大：s 因子随变换缩放，边缘保持 1px 过渡。
         * 纯缩放 + 平移烘焙进 xform（基线落 (24, y)） */
        neox_affine x2 = { 2.5f, 0, 0, 2.5f, 24.0f, y };
        draw_line(tctx, fr, at_sdf, font, SAMPLE, 16.0f, 0, 0,
                  (neox_color){ 0.12f, 0.14f, 0.20f, 1.0f }, &x2, 0.0f);
        y += 16.0f * 1.6f * 2.5f;
    }

    /* 下区：MSDF */
    y += 30.0f;
    draw_line(tctx, fr, at_cov, font, "MSDF (RGBA8_LINEAR, dist_range 4)",
              14.0f, 24, y,
              (neox_color){ 0.20f, 0.30f, 0.60f, 1.0f }, NULL, 0.0f);
    y += 28.0f;
    y += draw_line(tctx, fr, at_msdf, font, SAMPLE, 16.0f, 24, y,
                   (neox_color){ 0.12f, 0.14f, 0.20f, 1.0f }, NULL, 0.0f);
    y += draw_line(tctx, fr, at_msdf, font, SAMPLE, 24.0f, 24, y,
                   (neox_color){ 0.12f, 0.14f, 0.20f, 1.0f }, NULL, 0.0f);
    draw_line(tctx, fr, at_msdf, font, SAMPLE, 40.0f, 24, y,
              (neox_color){ 0.12f, 0.14f, 0.20f, 1.0f }, NULL, 0.0f);

    if (neox_frame_end(fr, &sub) != NEOX_OK) goto out;
    fr = NULL;
    neox_device_pump(soft);

    if (write_bmp("ex05_sdftext.bmp", img, W, H) != 0) goto out;
    printf("wrote ex05_sdftext.bmp\n");
    rc = 0;

out:
    if (fr) (void)neox_frame_end(fr, &sub);
    if (at_msdf) nx_atlas_destroy(at_msdf);
    if (at_sdf) nx_atlas_destroy(at_sdf);
    if (at_cov) nx_atlas_destroy(at_cov);
    if (soft) neox_device_destroy(soft);
    nx_text_context_destroy(tctx);
    neox_context_destroy(ctx);
    return rc;
}
