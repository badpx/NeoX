/* ===========================================================================
 * ex04_text.c — NeoX-Text v0 中英混排演示（Soft | GL | Metal 三联并排）
 *
 * 内容：左对齐 16px 段落（含标点/数字/强制换行）、20px 居中对齐段落、
 *       30px 大标题片段。图集为各后端独立的确定性 A8 图集（同内容）。
 * 输出（构建目录）：ex04_text.bmp（Soft）+ ex04_compare.bmp（三联并排）。
 * 字体缺失时打印提示并以 0 退出（示例冒烟测试不失败）。
 * NEOX_REQUIRE_GL 非空时，缺字体或 GL 画不出来改为失败。
 * =========================================================================*/
#include "neox.h"
#include "neox_text.h"
#if NEOX_ENABLE_GL
#include "neox_gl.h"
#endif
#if NEOX_ENABLE_METAL
#include "neox_mtl.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 720
#define H 400

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

static const char *PARA1 =
    "NeoX-Text v0: \xE4\xB8\xAD\xE8\x8B\xB1\xE6\xB7\xB7\xE6\x8E\x92\xEF\xBC\x8C"
    "greedy line breaking, A8 coverage atlas.\n"
    "second line: 12345 \xE6\xA0\x87\xE5\xBD\xA2\xE7\xBC\x93\xE5\xAD\x98\xE3\x80\x82";
static const char *PARA2 =
    "\xE5\xB1\x85\xE4\xB8\xAD\xE5\xAF\xB9\xE9\xBD\x90\xE6\xAE\xB5\xE8\x90\xBD"
    "\xEF\xBC\x9A" "Center Aligned, 20px "
    "\xEF\xBC\x8C\xE6\xA0\x87\xE7\x82\xB9\xEF\xBC\x81\xEF\xBC\x9F";

static int draw_paragraph(nx_text_context *tctx, neox_frame *fr, nx_atlas *at,
                          nx_font *font, const char *text, float size,
                          float x, float y, float max_w, nx_text_align align,
                          neox_color color)
{
    nx_layout *lo = NULL;
    neox_paint pt;
    neox_result r;
    float bbox[4];
    float yoff;
    r = nx_text_layout(tctx, font, size, text, max_w, align, &lo);
    if (r != NEOX_OK) return -1;
    nx_text_measure(lo, bbox);
    /* 布局 y=0 为首行基线；上移到 (x, y - ascent) 使 (x,y) 成为段落顶 */
    yoff = y - lo->ascent;
    {
        neox_state st;
        memset(&st, 0, sizeof(st));
        st.transform = (neox_affine){ 1, 0, 0, 1, x, yoff };
        st.opacity = 1.0f;
        r = neox_push_state(fr, &st);
    }
    if (r == NEOX_OK) {
        pt.color = color;
        r = nx_text_draw(fr, at, lo, &pt);
        neox_pop_state(fr);
    }
    nx_layout_destroy(lo);
    return r == NEOX_OK ? 0 : -1;
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    neox_context_desc cd;
    neox_device_desc dd;
    neox_context *ctx = NULL;
    neox_device *soft = NULL, *gl = NULL, *mtl = NULL;
    nx_text_context *tctx = NULL;
    nx_font *font = NULL;
    static uint8_t img_soft[W * H * 4], img_gl[W * H * 4], img_mtl[W * H * 4];
    static uint8_t side[W * 3 * H * 4];
    uint32_t y;
    int have_gl = 0, have_mtl = 0;

    static const struct { const char *path; int idx; } candidates[] = {
        { "/System/Library/Fonts/PingFang.ttc", 0 },
        { "/System/Library/Fonts/STHeiti Medium.ttc", 0 },
        { "/System/Library/Fonts/Songti.ttc", 0 },
        { "/System/Library/Fonts/Supplemental/Arial.ttf", 0 },
        { "/System/Library/Fonts/Helvetica.ttc", 0 },
        { "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", 0 },
        { "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 0 },
        { "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", 0 },
        { "/usr/share/fonts/truetype/freefont/FreeSans.ttf", 0 },
        { "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 0 }
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
        const char *req = getenv("NEOX_REQUIRE_GL");
        printf("no system font found — ex04 skipped\n");
        nx_text_context_destroy(tctx);
        neox_context_destroy(ctx);
        return (req && req[0]) ? 1 : 0;
    }

    memset(&dd, 0, sizeof(dd));
    dd.backend = NEOX_BACKEND_SOFT;
    if (neox_device_create(ctx, &dd, &soft) != NEOX_OK) return 1;
    /* Soft */
    {
        nx_atlas *at = NULL;
        neox_frame *fr = NULL;
        neox_submission sub;
        neox_target_desc td;
        neox_color clear = { 0.93f, 0.93f, 0.95f, 1.0f };
        if (nx_atlas_create(tctx, soft, 512, 512, &at) != NEOX_OK) return 1;
        memset(&td, 0, sizeof(td));
        td.kind = NEOX_BACKEND_SOFT;
        td.soft.pixels = img_soft;
        td.soft.stride = (size_t)W * 4;
        td.soft.width = W;
        td.soft.height = H;
        td.clear = &clear;
        if (neox_frame_begin(soft, &td, &fr) != NEOX_OK) return 1;
        if (draw_paragraph(tctx, fr, at, font, "NeoX-Text v0", 30.0f, 40, 44,
                           0, NX_ALIGN_LEFT,
                           (neox_color){ 0.65f, 0.18f, 0.15f, 1.0f }) != 0 ||
            draw_paragraph(tctx, fr, at, font, PARA1, 16.0f, 40, 80,
                           (float)W - 80, NX_ALIGN_LEFT,
                           (neox_color){ 0.12f, 0.14f, 0.20f, 1.0f }) != 0 ||
            draw_paragraph(tctx, fr, at, font, PARA2, 20.0f, 0, 220, (float)W,
                           NX_ALIGN_CENTER,
                           (neox_color){ 0.15f, 0.25f, 0.65f, 1.0f }) != 0)
            return 1;
        if (neox_frame_end(fr, &sub) != NEOX_OK) return 1;
        neox_device_pump(soft);
        nx_atlas_destroy(at);
    }

    /* GL */
#if NEOX_ENABLE_GL
    {
        neox_gl_device_desc gld;
        memset(&gld, 0, sizeof(gld));
        gld.struct_size = (uint32_t)sizeof(gld);
        gld.magic = NEOX_GL_DESC_MAGIC;
        memset(&dd, 0, sizeof(dd));
        dd.backend = NEOX_BACKEND_GPU;
        dd.platform = &gld;
        dd.platform_size = sizeof(gld);
        if (neox_device_create(ctx, &dd, &gl) == NEOX_OK) {
            neox_gl_surface *s = NULL;
            have_gl = 1;
            if (neox_gl_surface_create(gl, W, H, &s) == NEOX_OK) {
                nx_atlas *at = NULL;
                neox_frame *fr = NULL;
                neox_submission sub;
                neox_target_desc td;
                neox_color clear = { 0.93f, 0.93f, 0.95f, 1.0f };
                if (nx_atlas_create(tctx, gl, 512, 512, &at) == NEOX_OK) {
                    memset(&td, 0, sizeof(td));
                    td.kind = NEOX_BACKEND_GPU;
                    td.surface = s;
                    td.clear = &clear;
                    if (neox_frame_begin(gl, &td, &fr) == NEOX_OK) {
                        if (draw_paragraph(tctx, fr, at, font, "NeoX-Text v0",
                                           30.0f, 40, 44, 0, NX_ALIGN_LEFT,
                                           (neox_color){ 0.65f, 0.18f, 0.15f, 1.0f }) == 0 &&
                            draw_paragraph(tctx, fr, at, font, PARA1, 16.0f, 40, 80,
                                           (float)W - 80, NX_ALIGN_LEFT,
                                           (neox_color){ 0.12f, 0.14f, 0.20f, 1.0f }) == 0 &&
                            draw_paragraph(tctx, fr, at, font, PARA2, 20.0f, 0, 220,
                                           (float)W, NX_ALIGN_CENTER,
                                           (neox_color){ 0.15f, 0.25f, 0.65f, 1.0f }) == 0 &&
                            neox_frame_end(fr, &sub) == NEOX_OK) {
                            neox_device_pump(gl);
                            if (neox_gl_surface_read_pixels(s, img_gl) != 0)
                                memset(img_gl, 0x33, sizeof(img_gl));
                        }
                    }
                    nx_atlas_destroy(at);
                }
                neox_gl_surface_destroy(gl, s);
            }
        }
    }

#endif
    /* Metal */
#if NEOX_ENABLE_METAL
    {
        neox_mtl_device_desc mtd;
        memset(&mtd, 0, sizeof(mtd));
        mtd.struct_size = (uint32_t)sizeof(mtd);
        mtd.magic = NEOX_MTL_DESC_MAGIC;
        memset(&dd, 0, sizeof(dd));
        dd.backend = NEOX_BACKEND_GPU;
        dd.platform = &mtd;
        dd.platform_size = sizeof(mtd);
        if (neox_device_create(ctx, &dd, &mtl) == NEOX_OK) {
            neox_mtl_surface *s = NULL;
            have_mtl = 1;
            if (neox_mtl_surface_create(mtl, W, H, &s) == NEOX_OK) {
                nx_atlas *at = NULL;
                neox_frame *fr = NULL;
                neox_submission sub;
                neox_target_desc td;
                neox_color clear = { 0.93f, 0.93f, 0.95f, 1.0f };
                if (nx_atlas_create(tctx, mtl, 512, 512, &at) == NEOX_OK) {
                    memset(&td, 0, sizeof(td));
                    td.kind = NEOX_BACKEND_GPU;
                    td.surface = s;
                    td.clear = &clear;
                    if (neox_frame_begin(mtl, &td, &fr) == NEOX_OK) {
                        if (draw_paragraph(tctx, fr, at, font, "NeoX-Text v0",
                                           30.0f, 40, 44, 0, NX_ALIGN_LEFT,
                                           (neox_color){ 0.65f, 0.18f, 0.15f, 1.0f }) == 0 &&
                            draw_paragraph(tctx, fr, at, font, PARA1, 16.0f, 40, 80,
                                           (float)W - 80, NX_ALIGN_LEFT,
                                           (neox_color){ 0.12f, 0.14f, 0.20f, 1.0f }) == 0 &&
                            draw_paragraph(tctx, fr, at, font, PARA2, 20.0f, 0, 220,
                                           (float)W, NX_ALIGN_CENTER,
                                           (neox_color){ 0.15f, 0.25f, 0.65f, 1.0f }) == 0 &&
                            neox_frame_end(fr, &sub) == NEOX_OK) {
                            neox_device_pump(mtl);
                            (void)neox_submission_wait(mtl, sub);
                            if (neox_mtl_surface_read_pixels(s, img_mtl) != 0)
                                memset(img_mtl, 0x33, sizeof(img_mtl));
                        }
                    }
                    nx_atlas_destroy(at);
                }
                neox_mtl_surface_destroy(mtl, s);
            }
        }
    }
#endif

    {
        const char *req = getenv("NEOX_REQUIRE_GL");
        if (req && req[0] && !have_gl) {
            fprintf(stderr, "NEOX_REQUIRE_GL: GL text render unavailable\n");
            if (mtl) neox_device_destroy(mtl);
            if (gl) neox_device_destroy(gl);
            if (soft) neox_device_destroy(soft);
            nx_font_destroy(font);
            nx_text_context_destroy(tctx);
            neox_context_destroy(ctx);
            return 1;
        }
    }
    if (write_bmp("ex04_text.bmp", img_soft, W, H) != 0) return 1;
    printf("wrote ex04_text.bmp\n");
    for (y = 0; y < H; y++) {
        memcpy(&side[(size_t)y * W * 3 * 4], &img_soft[(size_t)y * W * 4],
               (size_t)W * 4);
        if (have_gl)
            memcpy(&side[((size_t)y * W * 3 + W) * 4], &img_gl[(size_t)y * W * 4],
                   (size_t)W * 4);
        else
            memset(&side[((size_t)y * W * 3 + W) * 4], 0x33, (size_t)W * 4);
        if (have_mtl)
            memcpy(&side[((size_t)y * W * 3 + W * 2) * 4],
                   &img_mtl[(size_t)y * W * 4], (size_t)W * 4);
        else
            memset(&side[((size_t)y * W * 3 + W * 2) * 4], 0x33, (size_t)W * 4);
    }
    for (y = 0; y < H; y++) {
        memset(&side[((size_t)y * W * 3 + W - 1) * 4], 0, 8);
        memset(&side[((size_t)y * W * 3 + W * 2 - 1) * 4], 0, 8);
    }
    if (write_bmp("ex04_compare.bmp", side, W * 3, H) != 0) return 1;
    printf("wrote ex04_compare.bmp (soft | gl | mtl)\n");

    if (mtl) neox_device_destroy(mtl);
    if (gl) neox_device_destroy(gl);
    if (soft) neox_device_destroy(soft);
    nx_text_context_destroy(tctx);
    neox_context_destroy(ctx);
    return 0;
}
