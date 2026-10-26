/* ===========================================================================
 * test_text.c — NeoX-Text 模块 v0 测试
 *
 * 覆盖：
 *   - UTF-8 解码：合法 1-4 字节、截断、overlong、代理区、超范围、非法延续
 *     （经 layout + 自定义字体恒等映射读回 glyph 序列逐类断言）
 *   - 排版（mock 字体后端注入）：kerning、拉丁按词贪心断行、CJK 逐字断行、
 *     '\n' 强制断行、三对齐、度量包围盒
 *   - 动态图集：缓存命中/未命中统计、size 量化、容量满错误、flush 语义
 *   - 集成（macOS 系统字体存在时；否则 SKIP 段落）：度量合理性 +
 *     中英混排经 nx_text_draw 在 Soft/GL/Metal 三后端逐位一致
 * =========================================================================*/
#include "neox.h"
#include "neox_gl.h"
#include "neox_mtl.h"
#include "neox_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

#define CHECK_RES(expr, code) do { \
    neox_result r_ = (expr); \
    g_checks++; \
    if (r_ != (code)) { \
        fprintf(stderr, "FAIL %s:%d: %s = %d, expect %d\n", \
                __FILE__, __LINE__, #expr, (int)r_, (int)(code)); \
        g_failures++; \
    } \
} while (0)

/* ------------------------------------------------------------------ 夹具 */

typedef struct {
    uint64_t live, peak, outstanding, allocs;
    int fail_next;
    void  *ptrs[2048];
    size_t sizes[2048];
    uint32_t n;
} alloc_stat;

static void *t_alloc(void *user, size_t size, size_t align)
{
    alloc_stat *s = (alloc_stat *)user;
    void *p;
    (void)align;
    if (s->fail_next) { s->fail_next = 0; return NULL; }
    p = malloc(size ? size : 1);
    if (!p) return NULL;
    s->live += size;
    if (s->live > s->peak) s->peak = s->live;
    s->allocs++;
    s->outstanding++;
    if (s->n < 2048) { s->ptrs[s->n] = p; s->sizes[s->n] = size; s->n++; }
    return p;
}

static void t_free(void *user, void *ptr, size_t size, size_t align)
{
    alloc_stat *s = (alloc_stat *)user;
    uint32_t i;
    (void)align;
    if (!ptr) return;
    for (i = 0; i < s->n; i++)
        if (s->ptrs[i] == ptr) {
            s->ptrs[i] = s->ptrs[s->n - 1];
            s->sizes[i] = s->sizes[s->n - 1];
            s->n--;
            goto removed;
        }
    fprintf(stderr, "WARN: free of untracked ptr %p\n", ptr);
removed:
    s->live -= size;
    s->outstanding--;
    free(ptr);
}

static alloc_stat g_stat;
static neox_context *g_ctx;
static nx_text_context *g_tctx;

static const neox_allocator g_al = { &g_stat, t_alloc, t_free };

/* ------------------------------------------------------------------ mock 字体 */

typedef struct {
    float adv_scale;          /* advance = size*adv_scale + (glyph % 3) */
    int kern_pair[2];         /* 生效字距对（码点） */
    float kern_amount;        /* 相对 size 的比例 */
    uint32_t rasterize_calls;
} mock_font;

static float mock_advance(void *user, uint32_t glyph, float size_px)
{
    mock_font *m = (mock_font *)user;
    return size_px * m->adv_scale + (float)(glyph % 3u);
}

static float mock_kerning(void *user, uint32_t g1, uint32_t g2, float size_px)
{
    mock_font *m = (mock_font *)user;
    if ((int)g1 == m->kern_pair[0] && (int)g2 == m->kern_pair[1])
        return size_px * m->kern_amount;
    return 0.0f;
}

static neox_result mock_rasterize(void *user, uint32_t glyph, float size_px,
                                  uint8_t **out_bmp, int32_t *out_w,
                                  int32_t *out_h, float *out_bx, float *out_by)
{
    mock_font *m = (mock_font *)user;
    int32_t w = 2 + (int32_t)(glyph % 5u);
    int32_t h = (int32_t)(size_px * 0.6f + 0.5f);
    int32_t x, y;
    uint8_t *bmp;
    m->rasterize_calls++;
    if (glyph == 0x20) {           /* 空格：空位图（零矩形缓存路径） */
        *out_bmp = NULL;
        *out_w = 0;
        *out_h = 0;
        *out_bx = 0.0f;
        *out_by = 0.0f;
        return NEOX_OK;
    }
    if (h < 1) h = 1;
    bmp = (uint8_t *)malloc((size_t)w * (size_t)h);
    if (!bmp) return NEOX_ERROR_OUT_OF_MEMORY;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            bmp[(size_t)y * w + x] = (uint8_t)((x * 31 + y * 17 + glyph) & 0xFF);
    *out_bmp = bmp;
    *out_w = w;
    *out_h = h;
    *out_bx = 1.0f;
    *out_by = (float)(1 - h);   /* 位图顶部在基线上方 h-1 像素 */
    return NEOX_OK;
}

static void mock_free_bitmap(void *user, uint8_t *bmp)
{
    (void)user;
    free(bmp);
}

/* 排版并返回字形序列（恒等映射：glyph_index == codepoint） */
static void layout_glyphs(nx_font *font, float size, const char *s,
                          float max_w, nx_text_align align,
                          uint32_t *out_glyphs, uint32_t *out_n, float *out_w)
{
    nx_layout *lo = NULL;
    uint32_t i;
    CHECK_RES(nx_text_layout(g_tctx, font, size, s, max_w, align, &lo), NEOX_OK);
    if (!lo) return;
    for (i = 0; i < lo->glyph_count && i < 256; i++)
        out_glyphs[i] = lo->glyphs[i].glyph_index;
    *out_n = lo->glyph_count;
    *out_w = lo->width;
    nx_layout_destroy(lo);
}

/* ------------------------------------------------------------------ UTF-8 解码 */

static void test_utf8_decode(void)
{
    nx_font_backend be;
    mock_font mock;
    nx_font *f = NULL;
    uint32_t g[256];
    uint32_t n;
    float w;

    memset(&mock, 0, sizeof(mock));
    mock.adv_scale = 0.5f;
    memset(&be, 0, sizeof(be));
    be.user = &mock;
    be.glyph_advance = mock_advance;
    be.glyph_kerning = mock_kerning;
    be.glyph_rasterize = mock_rasterize;
    be.glyph_free_bitmap = mock_free_bitmap;
    be.ascent = 0.8f; be.descent = -0.2f; be.line_gap = 0.1f;
    CHECK_RES(nx_font_create_custom(g_tctx, &be, &f), NEOX_OK);
    CHECK(f != NULL);
    if (!f) return;

    /* 合法：1/2/3/4 字节（A / é U+00E9 / 中 U+4E2D / 😀 U+1F600） */
    layout_glyphs(f, 16, "A\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80", 0,
                  NX_ALIGN_LEFT, g, &n, &w);
    CHECK(n == 4);
    CHECK(g[0] == 0x41);
    CHECK(g[1] == 0xE9);
    CHECK(g[2] == 0x4E2D);
    CHECK(g[3] == 0x1F600);

    /* 截断（3 字节序列缺尾） */
    layout_glyphs(f, 16, "\xE4\xB8", 0, NX_ALIGN_LEFT, g, &n, &w);
    CHECK(n == 2);
    CHECK(g[0] == 0xFFFD && g[1] == 0xFFFD);

    /* overlong（C0 AF） */
    layout_glyphs(f, 16, "\xC0\xAF", 0, NX_ALIGN_LEFT, g, &n, &w);
    CHECK(n == 2);
    CHECK(g[0] == 0xFFFD && g[1] == 0xFFFD);

    /* E0  overlong 变体（E0 80 AF） */
    layout_glyphs(f, 16, "\xE0\x80\xAF", 0, NX_ALIGN_LEFT, g, &n, &w);
    CHECK(n == 3);
    CHECK(g[0] == 0xFFFD && g[1] == 0xFFFD && g[2] == 0xFFFD);

    /* 代理区（ED A0 80） */
    layout_glyphs(f, 16, "\xED\xA0\x80", 0, NX_ALIGN_LEFT, g, &n, &w);
    CHECK(n == 3);
    CHECK(g[0] == 0xFFFD && g[1] == 0xFFFD && g[2] == 0xFFFD);

    /* 超范围（F4 90 80 80） */
    layout_glyphs(f, 16, "\xF4\x90\x80\x80", 0, NX_ALIGN_LEFT, g, &n, &w);
    CHECK(n == 4);
    CHECK(g[0] == 0xFFFD && g[3] == 0xFFFD);

    /* 非法延续字节穿插 */
    layout_glyphs(f, 16, "A\x80\xBF""B", 0, NX_ALIGN_LEFT, g, &n, &w);
    CHECK(n == 4);
    CHECK(g[0] == 0x41 && g[1] == 0xFFFD && g[2] == 0xFFFD && g[3] == 0x42);

    /* 空串 */
    layout_glyphs(f, 16, "", 0, NX_ALIGN_LEFT, g, &n, &w);
    CHECK(n == 0);

    nx_font_destroy(f);
}

/* ------------------------------------------------------------------ 排版 */

static void test_layout(void)
{
    nx_font_backend be;
    mock_font mock;
    nx_font *f = NULL;
    nx_layout *lo = NULL;

    memset(&mock, 0, sizeof(mock));
    mock.adv_scale = 0.5f;
    mock.kern_pair[0] = 'A';
    mock.kern_pair[1] = 'V';
    mock.kern_amount = -0.1f;
    memset(&be, 0, sizeof(be));
    be.user = &mock;
    be.glyph_advance = mock_advance;
    be.glyph_kerning = mock_kerning;
    be.glyph_rasterize = mock_rasterize;
    be.glyph_free_bitmap = mock_free_bitmap;
    be.ascent = 0.8f; be.descent = -0.2f; be.line_gap = 0.1f;
    CHECK_RES(nx_font_create_custom(g_tctx, &be, &f), NEOX_OK);
    if (!f) return;

    /* 度量（size 16：ascent=12.8 descent=-3.2 line_gap=1.6） */
    CHECK(nx_font_ascent(f, 16.0f) > 12.7f && nx_font_ascent(f, 16.0f) < 12.9f);
    CHECK(nx_font_descent(f, 16.0f) < -3.1f && nx_font_descent(f, 16.0f) > -3.3f);

    /* kerning 生效：'AV' 词宽 < 'AA' 词宽（A=65, V=86） */
    {
        float w_av, w_aa;
        uint32_t g[8], n;
        layout_glyphs(f, 16, "AV", 0, NX_ALIGN_LEFT, g, &n, &w_av);
        layout_glyphs(f, 16, "AA", 0, NX_ALIGN_LEFT, g, &n, &w_aa);
        CHECK(w_aa - w_av > 1.5f && w_aa - w_av < 1.7f);  /* 16*0.1=1.6 */
    }

    /* 拉丁按词贪心断行：词宽 = 2*(16*0.5+码点%3) */
    {
        /* 'a'(97%3=1) 词宽 2*(8+1)=18；三词 "aa bb cc" */
        float max_w = 50.0f;   /* 两词一行：aa(18)+空格(10)+bb(18)=46<=50 */
        uint32_t g[64], n;
        {
            float wtmp = 0.0f;
            layout_glyphs(f, 16, "aa bb cc", max_w, NX_ALIGN_LEFT, g, &n, &wtmp);
            CHECK(n == 7);          /* 断行处空格被吞掉 */
            CHECK(g[5] == 'c');    /* 第 5 个字形起为第二行 */
        }
    }
    /* 拉丁断行位置精确断言（行结构） */
    {
        nx_layout *l2 = NULL;
        CHECK_RES(nx_text_layout(g_tctx, f, 16.0f, "aa bb cc", 50.0f,
                                 NX_ALIGN_LEFT, &l2), NEOX_OK);
        if (l2) {
            CHECK(l2->line_count == 2);
            /* 第一行 5 字形（aa␣bb），第二行 3（cc） */
            if (l2->line_count == 2) {
                /* line 数组为内部字段，经 glyphs y 值区分行 */
                CHECK(l2->glyphs[0].y == 0.0f);
                CHECK(l2->glyphs[5].y > 0.0f);
            }
            nx_layout_destroy(l2);
        }
    }

    /* CJK 逐字断行：两字各成词，max_width 容一字 */
    {
        nx_layout *l2 = NULL;
        /* CJK 字 '中'(0x4E2D%3=2) advance=10；容一字的行宽 = 10 */
        CHECK_RES(nx_text_layout(g_tctx, f, 16.0f, "\xE4\xB8\xAD\xE6\x96\x87",
                                 12.0f, NX_ALIGN_LEFT, &l2), NEOX_OK);
        if (l2) {
            CHECK(l2->glyph_count == 2);
            CHECK(l2->line_count == 2);
            if (l2->line_count == 2) {
                CHECK(l2->glyphs[0].x == 0.0f);
                CHECK(l2->glyphs[1].x == 0.0f);   /* 第二行行首 */
            }
            nx_layout_destroy(l2);
        }
    }

    /* '\n' 强制断行 */
    {
        nx_layout *l2 = NULL;
        CHECK_RES(nx_text_layout(g_tctx, f, 16.0f, "ab\ncd", 0,
                                 NX_ALIGN_LEFT, &l2), NEOX_OK);
        if (l2) {
            CHECK(l2->line_count == 2);
            nx_layout_destroy(l2);
        }
    }

    /* 右对齐：偏移 = max_width - 行宽 */
    {
        nx_layout *l2 = NULL;
        /* 'a' advance = 8+1 = 9；行宽 18 */
        CHECK_RES(nx_text_layout(g_tctx, f, 16.0f, "aa", 100.0f,
                                 NX_ALIGN_RIGHT, &l2), NEOX_OK);
        if (l2) {
            CHECK(l2->glyphs[0].x > 81.5f && l2->glyphs[0].x < 82.5f); /* 100-18 */
            nx_layout_destroy(l2);
        }
    }

    /* 度量包围盒 */
    {
        nx_layout *l2 = NULL;
        float bbox[4];
        CHECK_RES(nx_text_layout(g_tctx, f, 16.0f, "aa\naa", 0,
                                 NX_ALIGN_LEFT, &l2), NEOX_OK);
        if (l2) {
            CHECK_RES(nx_text_measure(l2, bbox), NEOX_OK);
            CHECK(bbox[0] == 0.0f);
            CHECK(bbox[1] < -12.0f);          /* -ascent */
            CHECK(bbox[2] > 17.0f);           /* 行宽 18 */
            CHECK(bbox[3] > 15.0f);           /* 行高 + -descent */
            nx_layout_destroy(l2);
        }
    }

    /* max_width <= 0：不换行 */
    {
        nx_layout *l2 = NULL;
        CHECK_RES(nx_text_layout(g_tctx, f, 16.0f, "aa bb cc", -1.0f,
                                 NX_ALIGN_LEFT, &l2), NEOX_OK);
        if (l2) {
            CHECK(l2->line_count == 1);
            nx_layout_destroy(l2);
        }
    }

    /* 失败语义：非法字号 */
    CHECK_RES(nx_text_layout(g_tctx, f, 0.0f, "aa", 0, NX_ALIGN_LEFT, &lo),
              NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(lo == NULL);
    /* fail_next：排版分配失败不留半完成 */
    {
        nx_layout *l2 = NULL;
        g_stat.fail_next = 1;
        CHECK_RES(nx_text_layout(g_tctx, f, 16.0f, "aaaaaaaaaaaaaaaa", 0,
                                 NX_ALIGN_LEFT, &l2), NEOX_ERROR_OUT_OF_MEMORY);
        CHECK(l2 == NULL);
    }

    nx_font_destroy(f);
}

/* ------------------------------------------------------------------ 图集 */

static void test_atlas(void)
{
    nx_font_backend be;
    mock_font mock;
    nx_font *f = NULL;
    nx_atlas *at = NULL;
    neox_device_desc dd;
    neox_device *soft = NULL;
    nx_atlas_rect rc1, rc2;
    uint64_t hits, misses;

    memset(&mock, 0, sizeof(mock));
    mock.adv_scale = 0.5f;
    memset(&be, 0, sizeof(be));
    be.user = &mock;
    be.glyph_advance = mock_advance;
    be.glyph_kerning = mock_kerning;
    be.glyph_rasterize = mock_rasterize;
    be.glyph_free_bitmap = mock_free_bitmap;
    be.ascent = 0.8f; be.descent = -0.2f; be.line_gap = 0.1f;
    CHECK_RES(nx_font_create_custom(g_tctx, &be, &f), NEOX_OK);
    if (!f) return;

    memset(&dd, 0, sizeof(dd));
    dd.backend = NEOX_BACKEND_SOFT;
    CHECK_RES(neox_device_create(g_ctx, &dd, &soft), NEOX_OK);
    if (!soft) return;

    /* 缓存命中与量化 */
    CHECK_RES(nx_atlas_create(g_tctx, soft, 128, 128, &at), NEOX_OK);
    CHECK(at != NULL);
    if (at) {
        CHECK_RES(nx_atlas_glyph(at, f, 'A', 16.0f, &rc1), NEOX_OK);
        CHECK(rc1.w > 0 && rc1.h > 0);
        CHECK_RES(nx_atlas_glyph(at, f, 'A', 16.0f, &rc2), NEOX_OK);
        CHECK(rc2.x == rc1.x && rc2.y == rc1.y);   /* 命中同槽 */
        CHECK_RES(nx_atlas_glyph(at, f, 'A', 20.0f, &rc2), NEOX_OK);
        CHECK(rc2.x != rc1.x || rc2.y != rc1.y);   /* 不同字号 = 不同项 */
        /* 0.25px 量化：16.0 与 16.1 同键（16.1*4=64.4→64），16.2 不同（64.8→65 边界
         * 视舍入实现；用 17.0 确保差异） */
        CHECK_RES(nx_atlas_glyph(at, f, 'A', 16.1f, &rc2), NEOX_OK);
        CHECK(rc2.x == rc1.x && rc2.y == rc1.y);
        nx_atlas_stats(at, &hits, &misses);
        CHECK(hits == 2 && misses == 2);

        /* flush 幂等 */
        CHECK_RES(nx_atlas_flush(at), NEOX_OK);
        CHECK_RES(nx_atlas_flush(at), NEOX_OK);

        /* 图集即纹理：句柄有效 */
        CHECK(nx_atlas_texture(at).id != 0);
        nx_atlas_destroy(at);
        at = NULL;
    }

    /* 容量满：极小图集 + 大字号 */
    CHECK_RES(nx_atlas_create(g_tctx, soft, 16, 16, &at), NEOX_OK);
    if (at) {
        uint32_t gi;
        neox_result r = NEOX_OK;
        for (gi = 'A'; gi < 'A' + 8 && r == NEOX_OK; gi++)
            r = nx_atlas_glyph(at, f, gi, 64.0f, &rc1);
        CHECK_RES(r, NEOX_ERROR_OUT_OF_MEMORY);
        CHECK(nx_text_last_error(g_tctx) != NULL);
        nx_atlas_destroy(at);
        at = NULL;
    }

    /* 空字形（空格：零矩形，不占容量） */
    CHECK_RES(nx_atlas_create(g_tctx, soft, 64, 64, &at), NEOX_OK);
    if (at) {
        CHECK_RES(nx_atlas_glyph(at, f, 0x20, 16.0f, &rc1), NEOX_OK);
        CHECK(rc1.w == 0 && rc1.h == 0);
        nx_atlas_destroy(at);
    }

    nx_font_destroy(f);
    neox_device_destroy(soft);
}

/* ------------------------------------------------------------------ 集成（系统字体） */

typedef struct {
    neox_device *dev;
    int use_gl, use_mtl;
} render_dev;

static int render_text(const render_dev *rd, nx_font *font, const char *text,
                       float size, uint32_t w, uint32_t h, uint8_t *out)
{
    nx_atlas *at = NULL;
    nx_layout *lo = NULL;
    neox_frame *fr = NULL;
    neox_submission sub;
    neox_result r = NEOX_OK;
    neox_paint pt;
    neox_state st;
    neox_color clear = { 0.85f, 0.86f, 0.88f, 1.0f };

    r = nx_atlas_create(g_tctx, rd->dev, 512, 512, &at);
    if (r != NEOX_OK) return -1;
    r = nx_text_layout(g_tctx, font, size, text, (float)w - 8.0f,
                       NX_ALIGN_LEFT, &lo);
    if (r == NEOX_OK) {
        neox_target_desc td;
        memset(&td, 0, sizeof(td));
        td.kind = rd->use_gl || rd->use_mtl ? NEOX_BACKEND_GPU
                                            : NEOX_BACKEND_SOFT;
        td.clear = &clear;
        if (rd->use_gl) {
            neox_gl_surface *s = NULL;
            if (neox_gl_surface_create(rd->dev, w, h, &s) != NEOX_OK)
                r = NEOX_ERROR_BACKEND;
            else td.surface = s;
            if (r == NEOX_OK && neox_frame_begin(rd->dev, &td, &fr) == NEOX_OK) {
                memset(&st, 0, sizeof(st));
                st.transform = (neox_affine){ 1, 0, 0, 1, 4, 26 };
                st.opacity = 1.0f;
                neox_push_state(fr, &st);
                pt.color = (neox_color){ 0.15f, 0.25f, 0.65f, 1.0f };
                r = nx_text_draw(fr, at, lo, &pt);
                neox_pop_state(fr);
                if (r == NEOX_OK) r = neox_frame_end(fr, &sub);
                if (r == NEOX_OK) {
                    neox_device_pump(rd->dev);
                    if (rd->use_mtl)
                        (void)neox_submission_wait(rd->dev, sub);
                    if (neox_gl_surface_read_pixels(s, out) != NEOX_OK)
                        r = NEOX_ERROR_BACKEND;
                }
            }
            if (s) neox_gl_surface_destroy(rd->dev, s);
        } else if (rd->use_mtl) {
            neox_mtl_surface *s = NULL;
            if (neox_mtl_surface_create(rd->dev, w, h, &s) != NEOX_OK)
                r = NEOX_ERROR_BACKEND;
            else td.surface = s;
            if (r == NEOX_OK && neox_frame_begin(rd->dev, &td, &fr) == NEOX_OK) {
                memset(&st, 0, sizeof(st));
                st.transform = (neox_affine){ 1, 0, 0, 1, 4, 26 };
                st.opacity = 1.0f;
                neox_push_state(fr, &st);
                pt.color = (neox_color){ 0.15f, 0.25f, 0.65f, 1.0f };
                r = nx_text_draw(fr, at, lo, &pt);
                neox_pop_state(fr);
                if (r == NEOX_OK) r = neox_frame_end(fr, &sub);
                if (r == NEOX_OK) {
                    neox_device_pump(rd->dev);
                    (void)neox_submission_wait(rd->dev, sub);
                    if (neox_mtl_surface_read_pixels(s, out) != NEOX_OK)
                        r = NEOX_ERROR_BACKEND;
                }
            }
            if (s) neox_mtl_surface_destroy(rd->dev, s);
        } else {
            td.soft.pixels = out;
            td.soft.stride = (size_t)w * 4;
            td.soft.width = w;
            td.soft.height = h;
            if (neox_frame_begin(rd->dev, &td, &fr) == NEOX_OK) {
                memset(&st, 0, sizeof(st));
                st.transform = (neox_affine){ 1, 0, 0, 1, 4, 26 };
                st.opacity = 1.0f;
                neox_push_state(fr, &st);
                pt.color = (neox_color){ 0.15f, 0.25f, 0.65f, 1.0f };
                r = nx_text_draw(fr, at, lo, &pt);
                neox_pop_state(fr);
                if (r == NEOX_OK) r = neox_frame_end(fr, &sub);
                if (r == NEOX_OK) neox_device_pump(rd->dev);
            } else {
                r = NEOX_ERROR_BACKEND;
            }
        }
    }
    if (lo) nx_layout_destroy(lo);
    if (at) nx_atlas_destroy(at);
    return r == NEOX_OK ? 0 : -1;
}

static int create_gpu_device(nx_text_context *tctx, int use_mtl, neox_device **out)
{
    neox_device_desc dd;
    (void)tctx;
    memset(&dd, 0, sizeof(dd));
    dd.backend = NEOX_BACKEND_GPU;
    if (use_mtl) {
        neox_mtl_device_desc mtd;
        memset(&mtd, 0, sizeof(mtd));
        mtd.struct_size = (uint32_t)sizeof(mtd);
        mtd.magic = NEOX_MTL_DESC_MAGIC;
        dd.platform = &mtd;
        dd.platform_size = sizeof(mtd);
        /* desc 为 [borrow]，init 在返回前读取——必须在作用域内调用 */
        return neox_device_create(g_ctx, &dd, out) == NEOX_OK ? 0 : -1;
    } else {
        neox_gl_device_desc gld;
        memset(&gld, 0, sizeof(gld));
        gld.struct_size = (uint32_t)sizeof(gld);
        gld.magic = NEOX_GL_DESC_MAGIC;
        dd.platform = &gld;
        dd.platform_size = sizeof(gld);
        return neox_device_create(g_ctx, &dd, out) == NEOX_OK ? 0 : -1;
    }
}

static void test_integration_real_fonts(void)
{
    static const struct {
        const char *path;
        int ttc_index;
    } fonts[] = {
        { "/System/Library/Fonts/PingFang.ttc", 0 },
        { "/System/Library/Fonts/STHeiti Medium.ttc", 0 },
        { "/System/Library/Fonts/Songti.ttc", 0 },
        { "/System/Library/Fonts/Supplemental/Arial.ttf", 0 }
    };
    static const char *sample = "Hello, NeoX! \xE4\xB8\xAD\xE6\x96\x87\xE6\xB8\xAC\xE8\xAF\x95 123\nSecond line.";
    nx_font *f = NULL;
    uint32_t fi;
    int loaded = 0;

    for (fi = 0; fi < sizeof(fonts) / sizeof(fonts[0]) && !loaded; fi++) {
        FILE *fp = fopen(fonts[fi].path, "rb");
        if (!fp) continue;
        fclose(fp);
        if (nx_font_load_file(g_tctx, fonts[fi].path, fonts[fi].ttc_index,
                              &f) == NEOX_OK)
            loaded = 1;
    }
    if (!loaded) {
        printf("SKIP: 系统字体不可用（集成测试跳过）\n");
        return;
    }
    CHECK(f != NULL);

    /* 度量合理性 */
    {
        float a = nx_font_ascent(f, 16.0f);
        float d = nx_font_descent(f, 16.0f);
        CHECK(a > 0.0f);
        CHECK(d <= 0.0f);
        CHECK(a + (-d) > 8.0f && a + (-d) < 64.0f);  /* 合理 em 范围 */
    }

    /* 三后端渲染同一段文字：逐位一致（图集内容确定性） */
    {
        enum { W = 220, H = 64 };
        static uint8_t img_soft[W * H * 4], img_gl[W * H * 4], img_mtl[W * H * 4];
        neox_device *soft = NULL, *gl = NULL, *mtl = NULL;
        neox_device_desc dd;
        render_dev rd;
        int have_gl, have_mtl;

        memset(&dd, 0, sizeof(dd));
        dd.backend = NEOX_BACKEND_SOFT;
        CHECK_RES(neox_device_create(g_ctx, &dd, &soft), NEOX_OK);
        have_gl = create_gpu_device(g_tctx, 0, &gl) == 0;
        have_mtl = create_gpu_device(g_tctx, 1, &mtl) == 0;

        memset(&rd, 0, sizeof(rd));
        rd.dev = soft;
        CHECK(render_text(&rd, f, sample, 18.0f, W, H, img_soft) == 0);

        if (have_gl) {
            memset(&rd, 0, sizeof(rd));
            rd.dev = gl;
            rd.use_gl = 1;
            CHECK(render_text(&rd, f, sample, 18.0f, W, H, img_gl) == 0);
            CHECK(memcmp(img_soft, img_gl, sizeof(img_soft)) == 0);
        }
        if (have_mtl) {
            memset(&rd, 0, sizeof(rd));
            rd.dev = mtl;
            rd.use_mtl = 1;
            CHECK(render_text(&rd, f, sample, 18.0f, W, H, img_mtl) == 0);
            CHECK(memcmp(img_soft, img_mtl, sizeof(img_mtl)) == 0);
        }
        /* 内容非平凡（有文字痕迹） */
        {
            uint32_t i, diff = 0;
            for (i = 0; i < W * H; i++)
                if (img_soft[i * 4] != 217) diff++;
            CHECK(diff > 100);
        }
        if (mtl) neox_device_destroy(mtl);
        if (gl) neox_device_destroy(gl);
        if (soft) neox_device_destroy(soft);
    }

    nx_font_destroy(f);
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    neox_context_desc cd;

    memset(&g_stat, 0, sizeof(g_stat));
    memset(&cd, 0, sizeof(cd));
    cd.allocator = &g_al;
    if (neox_context_create(&cd, &g_ctx) != NEOX_OK) {
        fprintf(stderr, "context_create failed\n");
        return 2;
    }
    CHECK(neox_context_get_allocator(g_ctx) == &g_al);  /* 注入路径同指针 */
    CHECK_RES(nx_text_context_create(g_ctx, &g_al, &g_tctx), NEOX_OK);
    if (!g_tctx) return 2;

    test_utf8_decode();
    test_layout();
    test_atlas();
    test_integration_real_fonts();

    nx_text_context_destroy(g_tctx);
    neox_context_destroy(g_ctx);

    if (g_stat.outstanding != 0) {
        fprintf(stderr, "leak: %llu blocks\n",
                (unsigned long long)g_stat.outstanding);
        g_failures++;
    }
    if (g_failures == 0) {
        printf("ALL PASS (%d checks)\n", g_checks);
        return 0;
    }
    printf("%d/%d checks failed\n", g_failures, g_checks);
    return 1;
}
