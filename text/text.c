/* ===========================================================================
 * text.c — NeoX-Text 模块 v0 实现
 *
 * 组件：
 *   - UTF-8 解码（严格校验：overlong/代理区/超范围/截断 → U+FFFD，逐失效
 *     字节前进 1——简化版 maximal-subpart，语义文档化于头文件）
 *   - 字体：stb_truetype（TTC 经 GetFontOffsetForIndex）或测试注入的自定义
 *     后端（codepoint→glyph 恒等）
 *   - 排版：分词（CJK 单字成词、拉丁成词、空白独立）、贪心断行、三对齐
 *   - 动态图集：shelf/row 装箱 + 开放寻址缓存 + 脏矩形增量上传
 *   - 绘制桥：layout → neox_glyph[] → neox_draw_glyphs
 *
 * 内存：持久内存全部经注入分配器；stb 光栅化瞬态分配（STBTT_malloc 默认
 * 系统 malloc）在 rasterize 内立即 FreeBitmap 释放（文档化，见
 * docs/dependencies.md）。
 * =========================================================================*/
#include "neox_text.h"

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#define STBTT_STATIC               /* 文件局部符号，避免与其他 stb 用户冲突 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ 小工具 */

static void *tx_alloc(nx_text_context *t, size_t size, size_t align);
static void  tx_free(nx_text_context *t, void *p, size_t size, size_t align);

static void *tx_array_grow(nx_text_context *t, void *arr, size_t elem,
                           uint32_t *cap, uint32_t need)
{
    if (need <= *cap) return arr;
    uint32_t ncap = *cap ? *cap * 2 : 16;
    void *np;
    while (ncap < need) ncap *= 2;
    np = tx_alloc(t, elem * (size_t)ncap, 16);
    if (!np) return NULL;
    if (arr) {
        memcpy(np, arr, elem * (size_t)(*cap));
        tx_free(t, arr, elem * (size_t)(*cap), 16);
    }
    *cap = ncap;
    return np;
}

#define TX_ERR(t, msg, code) do { (t)->last_error = (msg); return (code); } while (0)

/* ------------------------------------------------------------------ 上下文 */

struct nx_text_context {
    neox_context *core;
    neox_allocator al;        /* 归一化：调用方注入或转发 core 的分配器 */
    const char *last_error;
    uint32_t flags;
    nx_font *fonts;           /* 级联销毁链表 */
    nx_atlas *atlases;
};

/* 系统默认分配器（core 未注入分配器时模块侧转发 malloc/free，语义不变） */
static void *tx_sys_alloc(void *user, size_t size, size_t align)
{
    (void)user; (void)align;
    return malloc(size ? size : 1);
}
static void tx_sys_free(void *user, void *ptr, size_t size, size_t align)
{
    (void)user; (void)size; (void)align;
    free(ptr);
}
static const neox_allocator TX_SYS_ALLOC = { NULL, tx_sys_alloc, tx_sys_free };

static void *tx_alloc(nx_text_context *t, size_t size, size_t align)
{
    return t->al.alloc(t->al.user, size, align);
}

static void tx_free(nx_text_context *t, void *p, size_t size, size_t align)
{
    if (p) t->al.free(t->al.user, p, size, align);
}

neox_result nx_text_context_create(neox_context *core_ctx,
                                   const neox_allocator *alloc,
                                   nx_text_context **out_ctx)
{
    nx_text_context *t;
    if (!core_ctx || !out_ctx) return NEOX_ERROR_INVALID_ARGUMENT;
    *out_ctx = NULL;
    /* 分配器归一化：调用方注入 > core 注入 > 系统默认（公开 API 查询，无内部耦合） */
    {
        const neox_allocator *al = alloc ? alloc
                                         : neox_context_get_allocator(core_ctx);
        if (!al) al = &TX_SYS_ALLOC;
        t = (nx_text_context *)al->alloc(al->user, sizeof(*t), 16);
        if (!t) return NEOX_ERROR_OUT_OF_MEMORY;
        memset(t, 0, sizeof(*t));
        t->core = core_ctx;
        t->al = *al;
    }
    *out_ctx = t;
    return NEOX_OK;
}

void nx_text_context_destroy(nx_text_context *t)
{
    if (!t) return;
    while (t->fonts) nx_font_destroy(t->fonts);
    while (t->atlases) nx_atlas_destroy(t->atlases);
    {
        neox_allocator al = t->al;
        al.free(al.user, t, sizeof(*t), 16);
    }
}

const char *nx_text_last_error(nx_text_context *t)
{
    return t ? t->last_error : NULL;
}

void nx_text_context_set_flags(nx_text_context *t, uint32_t flags)
{
    if (t) t->flags = flags;
}

/* ------------------------------------------------------------------ UTF-8 解码
 * 严格校验：C0/C1、overlong、代理区（ED A0..BF）、> U+10FFFF（F4 90..）、
 * 截断/非法延续字节 → U+FFFD 并前进 1 字节（简化版 maximal-subpart） */

static uint32_t nx_utf8_cont(const char *p, const char *end, int *ok)
{
    if (p < end && ((uint8_t)*p & 0xC0) == 0x80) {
        *ok = 1;
        return (uint32_t)((uint8_t)*p & 0x3F);
    }
    *ok = 0;
    return 0;
}

/* 取下一个码点；到达结尾返回 0。非法序列输出 0xFFFD 并前进 1 字节 */
static int nx_utf8_next(const char **pp, const char *end, uint32_t *out_cp)
{
    const char *p = *pp;
    uint8_t b;
    if (p >= end) return 0;
    b = (uint8_t)*p;
    if (b < 0x80) {
        *out_cp = b;
        *pp = p + 1;
        return 1;
    }
    if (b >= 0xC2 && b <= 0xDF) {
        int ok;
        uint32_t c = nx_utf8_cont(p + 1, end, &ok);
        if (ok) {
            *out_cp = ((uint32_t)(b & 0x1F) << 6) | c;
            *pp = p + 2;
            return 1;
        }
    } else if (b >= 0xE0 && b <= 0xEF) {
        int ok1, ok2;
        uint32_t c1 = nx_utf8_cont(p + 1, end, &ok1);
        uint32_t c2 = nx_utf8_cont(p + 2, end, &ok2);
        if (ok1 && ok2) {
            uint32_t cp = ((uint32_t)(b & 0x0F) << 12) | (c1 << 6) | c2;
            /* E0 第二字节需 A0..BF（排除 overlong）；ED 需 80..9F（排除代理区） */
            uint8_t s = (uint8_t)p[1];
            int lo_ok = (b != 0xE0 || s >= 0xA0) && (b != 0xED || s <= 0x9F);
            if (lo_ok) {
                *out_cp = cp;
                *pp = p + 3;
                return 1;
            }
        }
    } else if (b >= 0xF0 && b <= 0xF4) {
        int ok1, ok2, ok3;
        uint32_t c1 = nx_utf8_cont(p + 1, end, &ok1);
        uint32_t c2 = nx_utf8_cont(p + 2, end, &ok2);
        uint32_t c3 = nx_utf8_cont(p + 3, end, &ok3);
        if (ok1 && ok2 && ok3) {
            uint32_t cp = ((uint32_t)(b & 0x07) << 18) | (c1 << 12) |
                          (c2 << 6) | c3;
            uint8_t s = (uint8_t)p[1];
            int lo_ok = (b != 0xF0 || s >= 0x90) && (b != 0xF4 || s <= 0x8F);
            if (lo_ok && cp <= 0x10FFFF) {
                *out_cp = cp;
                *pp = p + 4;
                return 1;
            }
        }
    }
    *out_cp = 0xFFFD;
    *pp = p + 1;
    return 1;
}

/* ------------------------------------------------------------------ 字体 */

struct nx_font {
    nx_text_context *tctx;
    nx_font *next;                /* tctx 级联链 */
    /* stb 实例（自定义后端时为 NULL） */
    uint8_t *data;                /* [own] 字体文件拷贝 */
    size_t data_size;
    stbtt_fontinfo stb;
    /* 统一后端入口 */
    nx_font_backend be;
    int is_custom;
};

static void font_unlink(nx_font *f)
{
    nx_text_context *t = f->tctx;
    nx_font **link = &t->fonts;
    while (*link && *link != f) link = &(*link)->next;
    if (*link) *link = f->next;
}

static void font_free(nx_font *f)
{
    nx_text_context *t = f->tctx;
    if (f->is_custom) {
        if (f->be.destroy) f->be.destroy(f->be.user);
    } else {
        if (f->data) tx_free(t, f->data, f->data_size, 16);
    }
    tx_free(t, f, sizeof(*f), 16);
}

void nx_font_destroy(nx_font *f)
{
    if (!f) return;
    font_unlink(f);
    font_free(f);
}

/* stb 后端回调 */
static float stb_advance(void *user, uint32_t glyph, float size_px)
{
    nx_font *f = (nx_font *)user;
    int adv, lsb;
    float scale = stbtt_ScaleForPixelHeight(&f->stb, size_px);
    stbtt_GetGlyphHMetrics(&f->stb, (int)glyph, &adv, &lsb);
    return adv * scale;
}

static float stb_kerning(void *user, uint32_t g1, uint32_t g2, float size_px)
{
    nx_font *f = (nx_font *)user;
    float scale = stbtt_ScaleForPixelHeight(&f->stb, size_px);
    return stbtt_GetGlyphKernAdvance(&f->stb, (int)g1, (int)g2) * scale;
}

static neox_result stb_rasterize(void *user, uint32_t glyph, float size_px,
                                 uint8_t **out_bmp, int32_t *out_w, int32_t *out_h,
                                 float *out_bx, float *out_by)
{
    nx_font *f = (nx_font *)user;
    float scale = stbtt_ScaleForPixelHeight(&f->stb, size_px);
    int w = 0, h = 0, ox = 0, oy = 0;
    unsigned char *bmp = stbtt_GetGlyphBitmap(&f->stb, scale, scale,
                                              (int)glyph, &w, &h, &ox, &oy);
    *out_bmp = bmp;               /* 可能为 NULL（空字形） */
    *out_w = w; *out_h = h;
    *out_bx = (float)ox;
    *out_by = (float)oy;
    return NEOX_OK;
}

static void stb_free_bitmap(void *user, uint8_t *bmp)
{
    (void)user;
    if (bmp) stbtt_FreeBitmap(bmp, NULL);
}

/* ------------------------------------------------------------------ 距离场
 * 编码（§8.2，三后端共用同一重建公式）：距离 d（图集像素，形内为正）→
 * v = 128 − d·255/dist_range，截断 [0,255]（±0.5 LSB 常量偏移，§9 容差内）。
 * SDF：stbtt_GetGlyphSDF（真距离）。MSDF：简化版自实现（ADR 0006 范围的
 * 允许简化）：真距离 + 拐角方向差 > 60° 边着色循环 R/G/B + 二次/三次
 * 曲线细分 16 段；尖角内侧可略圆。alpha 恒 255（防核心预乘破坏数值）。
 * 位图已含 dist_range 边距（pad = ceil(dr)+1），bear 相应外扩。 */

#define NX_MSDF_SUBDIV 16        /* 曲线细分数（简化版，§9 容差内） */
#define NX_MSDF_CORNER_COS 0.5f  /* cos(60°)：方向差阈值（余弦比较，免 acos） */

typedef struct nx_msdf_seg {
    float x0, y0, x1, y1;   /* 像素空间（y 向下） */
    int color;              /* 0=R 1=G 2=B */
} nx_msdf_seg;

static float nx_msdf_seg_dist2(float px, float py,
                               float x0, float y0, float x1, float y1)
{
    float dx = x1 - x0, dy = y1 - y0;
    float l2 = dx * dx + dy * dy;
    float t = l2 > 0.0f ? ((px - x0) * dx + (py - y0) * dy) / l2 : 0.0f;
    if (t < 0.0f) t = 0.0f;
    else if (t > 1.0f) t = 1.0f;
    dx = px - (x0 + t * dx);
    dy = py - (y0 + t * dy);
    return dx * dx + dy * dy;
}

static int nx_msdf_push_seg(nx_msdf_seg **segs, int *n, int *cap,
                            float x0, float y0, float x1, float y1, int color)
{
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 64;
        nx_msdf_seg *ns = (nx_msdf_seg *)STBTT_malloc(
            (size_t)nc * sizeof(nx_msdf_seg), NULL);
        if (!ns) return 0;
        memcpy(ns, *segs, (size_t)*n * sizeof(nx_msdf_seg));
        if (*segs) STBTT_free(*segs, NULL);
        *segs = ns;
        *cap = nc;
    }
    (*segs)[*n].x0 = x0; (*segs)[*n].y0 = y0;
    (*segs)[*n].x1 = x1; (*segs)[*n].y1 = y1;
    (*segs)[*n].color = color;
    (*n)++;
    return 1;
}

/* stb 轮廓 → 着色线段表（像素空间，y 向下）。拐角方向差 > 60° 换色，
 * 逐轮廓从 R 起循环 R→G→B；轮廓闭合边补上。失败返回 NULL。 */
static nx_msdf_seg *nx_msdf_build_segs(int nv,
                                       stbtt_vertex *verts, float scale,
                                       int *out_n)
{
    nx_msdf_seg *segs = NULL;
    int n = 0, cap = 0, i;
    int color = 0, open = 0, have_prev = 0;
    float sx = 0, sy = 0, cx = 0, cy = 0;   /* 轮廓起点 / 当前点 */
    float pdx = 0, pdy = 0;                 /* 上一条边方向（单位化前） */
    #define NX_PX(vx) ((float)(vx) * scale)
    #define NX_PY(vy) (-(float)(vy) * scale)
    for (i = 0; i < nv; i++) {
        stbtt_vertex *v = &verts[i];
        if (v->type == STBTT_vmove) {
            if (open)   /* 闭合上一轮廓 */
                if (!nx_msdf_push_seg(&segs, &n, &cap, cx, cy, sx, sy, color))
                    goto oom;
            sx = cx = NX_PX(v->x);
            sy = cy = NX_PY(v->y);
            color = 0;
            open = 1;
            have_prev = 0;
        } else if (v->type == STBTT_vline || v->type == STBTT_vcurve ||
                   v->type == STBTT_vcubic) {
            float ex, ey, dirx, diry, l;
            int c;
            if (!open) continue;
            ex = NX_PX(v->x);
            ey = NX_PY(v->y);
            dirx = ex - cx;
            diry = ey - cy;
            l = sqrtf(dirx * dirx + diry * diry);
            c = color;
            if (have_prev && l > 0.0f) {
                float pl = sqrtf(pdx * pdx + pdy * pdy);
                if (pl > 0.0f) {
                    float cosv = (pdx * dirx + pdy * diry) / (pl * l);
                    /* 方向差（取锐角侧）：cos < 0.5 即 > 60° */
                    if (cosv < NX_MSDF_CORNER_COS)
                        c = color = (color + 1) % 3;
                }
            }
            if (v->type == STBTT_vline) {
                if (!nx_msdf_push_seg(&segs, &n, &cap, cx, cy, ex, ey, c))
                    goto oom;
            } else if (v->type == STBTT_vcurve) {
                float mx = NX_PX(v->cx), my = NX_PY(v->cy);
                float px2 = cx, py2 = cy;
                int k;
                for (k = 1; k <= NX_MSDF_SUBDIV; k++) {
                    float t = (float)k / (float)NX_MSDF_SUBDIV;
                    float it = 1.0f - t;
                    float qx = it * it * cx + 2.0f * it * t * mx + t * t * ex;
                    float qy = it * it * cy + 2.0f * it * t * my + t * t * ey;
                    if (!nx_msdf_push_seg(&segs, &n, &cap, px2, py2, qx, qy, c))
                        goto oom;
                    px2 = qx; py2 = qy;
                }
            } else {  /* vcubic（CFF）：de Casteljau 细分 */
                float m1x = NX_PX(v->cx), m1y = NX_PY(v->cy);
                float m2x = NX_PX(v->cx1), m2y = NX_PY(v->cy1);
                float px2 = cx, py2 = cy;
                int k;
                for (k = 1; k <= NX_MSDF_SUBDIV; k++) {
                    float t = (float)k / (float)NX_MSDF_SUBDIV;
                    float it = 1.0f - t;
                    float q01x = it * cx + t * m1x, q01y = it * cy + t * m1y;
                    float q12x = it * m1x + t * m2x, q12y = it * m1y + t * m2y;
                    float q23x = it * m2x + t * ex, q23y = it * m2y + t * ey;
                    float q012x = it * q01x + t * q12x, q012y = it * q01y + t * q12y;
                    float q123x = it * q12x + t * q23x, q123y = it * q12y + t * q23y;
                    float qx = it * q012x + t * q123x;
                    float qy = it * q012y + t * q123y;
                    if (!nx_msdf_push_seg(&segs, &n, &cap, px2, py2, qx, qy, c))
                        goto oom;
                    px2 = qx; py2 = qy;
                }
            }
            if (l > 0.0f) { pdx = dirx; pdy = diry; have_prev = 1; }
            cx = ex; cy = ey;
        }
    }
    if (open)
        if (!nx_msdf_push_seg(&segs, &n, &cap, cx, cy, sx, sy, color))
            goto oom;
    #undef NX_PX
    #undef NX_PY
    *out_n = n;
    return segs;
oom:
    if (segs) STBTT_free(segs, NULL);
    return NULL;
}

/* 单字形 MSDF（RGBA，alpha=255）。空字形返回 NULL（同 GetGlyphBitmap 约定） */
static uint8_t *nx_msdf_glyph(stbtt_fontinfo *stb, int glyph, float scale,
                              float dr, int *out_w, int *out_h,
                              int *out_xoff, int *out_yoff)
{
    int ix0, iy0, ix1, iy1, pad, W, H, x, y, i;
    int nv, nseg = 0, cw, ch;
    stbtt_vertex *verts = NULL;
    nx_msdf_seg *segs = NULL;
    uint8_t *cov = NULL, *out = NULL;
    float pds = -255.0f / dr;

    stbtt_GetGlyphBitmapBox(stb, glyph, scale, scale, &ix0, &iy0, &ix1, &iy1);
    if (ix0 == ix1 || iy0 == iy1) return NULL;   /* 空字形 */
    nv = stbtt_GetGlyphShape(stb, glyph, &verts);
    if (nv <= 0) {
        if (verts) STBTT_free(verts, stb->userdata);
        return NULL;
    }
    pad = (int)ceilf(dr) + 1;
    W = (ix1 - ix0) + pad * 2;
    H = (iy1 - iy0) + pad * 2;
    cw = ix1 - ix0;
    ch = iy1 - iy0;
    segs = nx_msdf_build_segs(nv, verts, scale, &nseg);
    STBTT_free(verts, stb->userdata);
    if (!segs || nseg == 0) goto done;
    cov = (uint8_t *)STBTT_malloc((size_t)cw * ch, stb->userdata);
    if (!cov) goto done;
    stbtt_MakeGlyphBitmap(stb, cov, cw, ch, cw, scale, scale, glyph);
    out = (uint8_t *)STBTT_malloc((size_t)W * H * 4, stb->userdata);
    if (!out) goto done;
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            /* 像素中心（stbtt_GetGlyphSDF 同款坐标约定） */
            float px = (float)(ix0 - pad + x) + 0.5f;
            float py = (float)(iy0 - pad + y) + 0.5f;
            float best[3] = { 1e30f, 1e30f, 1e30f }, best_all = 1e30f;
            int inside = 0;
            for (i = 0; i < nseg; i++) {
                float d2 = nx_msdf_seg_dist2(px, py, segs[i].x0, segs[i].y0,
                                             segs[i].x1, segs[i].y1);
                if (d2 < best_all) best_all = d2;
                if (d2 < best[segs[i].color]) best[segs[i].color] = d2;
            }
            if ((unsigned)(x - pad) < (unsigned)cw &&
                (unsigned)(y - pad) < (unsigned)ch)
                inside = cov[(size_t)(y - pad) * cw + (x - pad)] >= 128;
            {
                uint8_t *q = out + ((size_t)y * W + x) * 4;
                int ch2;
                for (ch2 = 0; ch2 < 3; ch2++) {
                    float d, v;
                    if (best[ch2] > 1e29f) best[ch2] = best_all; /* 该色无边 */
                    d = inside ? sqrtf(best[ch2]) : -sqrtf(best[ch2]);
                    v = 128.0f + pds * d;
                    if (v < 0.0f) v = 0.0f;
                    else if (v > 255.0f) v = 255.0f;
                    q[ch2] = (uint8_t)v;
                }
                q[3] = 255;
            }
        }
    }
done:
    if (segs) STBTT_free(segs, NULL);
    if (cov) STBTT_free(cov, stb->userdata);
    if (out) {
        *out_w = W;
        *out_h = H;
        *out_xoff = ix0 - pad;
        *out_yoff = iy0 - pad;
    }
    return out;
}

static neox_result stb_rasterize_sdf(void *user, uint32_t glyph, float size_px,
                                     neox_atlas_mode mode, float dist_range,
                                     uint8_t **out_bmp, int32_t *out_w,
                                     int32_t *out_h, float *out_bx,
                                     float *out_by)
{
    nx_font *f = (nx_font *)user;
    float scale = stbtt_ScaleForPixelHeight(&f->stb, size_px);
    int pad = (int)ceilf(dist_range) + 1;
    int w = 0, h = 0, ox = 0, oy = 0;
    unsigned char *bmp;
    if (mode == NEOX_ATLAS_SDF) {
        bmp = stbtt_GetGlyphSDF(&f->stb, scale, (int)glyph, pad, 128,
                                -255.0f / dist_range, &w, &h, &ox, &oy);
    } else {
        bmp = nx_msdf_glyph(&f->stb, (int)glyph, scale, dist_range,
                            &w, &h, &ox, &oy);
    }
    *out_bmp = bmp;               /* 可能为 NULL（空字形） */
    *out_w = w; *out_h = h;
    *out_bx = (float)ox;
    *out_by = (float)oy;
    return NEOX_OK;
}

static uint32_t font_glyph_index(nx_font *f, uint32_t codepoint)
{
    if (f->is_custom) return codepoint;  /* 恒等映射（文档化） */
    return (uint32_t)stbtt_FindGlyphIndex(&f->stb, (int)codepoint);
}

static neox_result font_load(nx_text_context *t, const void *data, size_t size,
                             int ttc_index, nx_font **out)
{
    nx_font *f;
    int offset;
    if (!data || size == 0)
        TX_ERR(t, "empty font data", NEOX_ERROR_INVALID_ARGUMENT);
    f = (nx_font *)tx_alloc(t, sizeof(*f), 16);
    if (!f) TX_ERR(t, "font allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    memset(f, 0, sizeof(*f));
    f->tctx = t;
    f->data = (uint8_t *)tx_alloc(t, size, 16);
    if (!f->data) {
        tx_free(t, f, sizeof(*f), 16);
        TX_ERR(t, "font buffer allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    }
    memcpy(f->data, data, size);
    f->data_size = size;
    offset = stbtt_GetFontOffsetForIndex(f->data, ttc_index);
    if (offset < 0 || !stbtt_InitFont(&f->stb, f->data, offset)) {
        tx_free(t, f->data, size, 16);
        tx_free(t, f, sizeof(*f), 16);
        TX_ERR(t, "not a valid TrueType font (or bad TTC index)",
               NEOX_ERROR_INVALID_ARGUMENT);
    }
    f->be.user = f;
    f->be.glyph_advance = stb_advance;
    f->be.glyph_kerning = stb_kerning;
    f->be.glyph_rasterize = stb_rasterize;
    f->be.glyph_rasterize_sdf = stb_rasterize_sdf;
    f->be.glyph_free_bitmap = stb_free_bitmap;
    {
        int a, d, g;
        stbtt_GetFontVMetrics(&f->stb, &a, &d, &g);
        /* 归一化到 size_px = 1（ScaleForPixelHeight(1) ≈ 1/units_per_em） */
        {
            float s1 = stbtt_ScaleForPixelHeight(&f->stb, 1.0f);
            f->be.ascent = a * s1;
            f->be.descent = d * s1;
            f->be.line_gap = g * s1;
        }
    }
    f->next = t->fonts;
    t->fonts = f;
    *out = f;
    return NEOX_OK;
}

neox_result nx_font_load_memory(nx_text_context *t, const void *data,
                                size_t size, int ttc_index, nx_font **out)
{
    if (!t || !out) return NEOX_ERROR_INVALID_ARGUMENT;
    *out = NULL;
    return font_load(t, data, size, ttc_index, out);
}

neox_result nx_font_load_file(nx_text_context *t, const char *path,
                              int ttc_index, nx_font **out)
{
    FILE *fp;
    long len;
    uint8_t *buf;
    neox_result r;
    if (!t || !path || !out) return NEOX_ERROR_INVALID_ARGUMENT;
    *out = NULL;
    fp = fopen(path, "rb");
    if (!fp) TX_ERR(t, "font file not found", NEOX_ERROR_INVALID_ARGUMENT);
    if (fseek(fp, 0, SEEK_END) != 0 || (len = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        TX_ERR(t, "font file seek failed", NEOX_ERROR_INVALID_ARGUMENT);
    }
    buf = (uint8_t *)tx_alloc(t, (size_t)len, 16);
    if (!buf) {
        fclose(fp);
        TX_ERR(t, "font buffer allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    }
    if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        fclose(fp);
        tx_free(t, buf, (size_t)len, 16);
        TX_ERR(t, "font file read failed", NEOX_ERROR_INVALID_ARGUMENT);
    }
    fclose(fp);
    r = font_load(t, buf, (size_t)len, ttc_index, out);
    tx_free(t, buf, (size_t)len, 16);  /* font_load 内部拷贝 */
    return r;
}

neox_result nx_font_create_custom(nx_text_context *t,
                                  const nx_font_backend *backend, nx_font **out)
{
    nx_font *f;
    if (!t || !backend || !backend->glyph_advance || !backend->glyph_rasterize ||
        !backend->glyph_free_bitmap || !out)
        return NEOX_ERROR_INVALID_ARGUMENT;
    *out = NULL;
    f = (nx_font *)tx_alloc(t, sizeof(*f), 16);
    if (!f) TX_ERR(t, "font allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    memset(f, 0, sizeof(*f));
    f->tctx = t;
    f->is_custom = 1;
    f->be = *backend;
    f->next = t->fonts;
    t->fonts = f;
    *out = f;
    return NEOX_OK;
}

float nx_font_ascent(nx_font *f, float size_px)  { return f ? f->be.ascent * size_px : 0.0f; }
float nx_font_descent(nx_font *f, float size_px) { return f ? f->be.descent * size_px : 0.0f; }
float nx_font_line_gap(nx_font *f, float size_px){ return f ? f->be.line_gap * size_px : 0.0f; }

/* ------------------------------------------------------------------ 排版 */

/* CJK 可断行区间（逐字可断；含全角形式与 CJK 标点） */
static int nx_is_cjk(uint32_t cp)
{
    return (cp >= 0x2E80 && cp <= 0x9FFF) ||
           (cp >= 0x3000 && cp <= 0x30FF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) ||
           (cp >= 0x20000 && cp <= 0x2FA1F);
}

/* 行表项（结构体定义在头文件前向声明为不完整类型，此处补全） */
struct nx_line_info {
    uint32_t start, count;
    float width;
};
typedef struct nx_line_info nx_line_info;

void nx_layout_destroy(nx_layout *lo)
{
    if (!lo) return;
    tx_free(lo->tctx, lo->glyphs, (size_t)lo->glyph_cap * sizeof(nx_laid_glyph), 16);
    tx_free(lo->tctx, lo->line, (size_t)lo->line_cap * sizeof(nx_line_info), 16);
    tx_free(lo->tctx, lo, sizeof(*lo), 16);
}

neox_result nx_text_measure(const nx_layout *lo, float out_bbox[4])
{
    if (!lo || !out_bbox) return NEOX_ERROR_INVALID_ARGUMENT;
    out_bbox[0] = 0.0f;
    out_bbox[1] = -lo->ascent;
    out_bbox[2] = lo->width;
    out_bbox[3] = (float)(lo->line_count ? lo->line_count - 1 : 0) *
                  lo->line_height - lo->descent;
    return NEOX_OK;
}

typedef struct nx_token {
    uint32_t glyph0;   /* 输入字形数组起点 */
    uint32_t count;
    float width;       /* 含词内字距 */
    int is_space;
    int hard_break;    /* '\n'：强制断行占位（count=0） */
} nx_token;

static neox_result layout_add_line(nx_layout *lo)
{
    nx_line_info *nl;
    if (lo->line_count < lo->line_cap) return NEOX_OK;
    nl = (nx_line_info *)tx_array_grow(lo->tctx, lo->line, sizeof(nx_line_info),
                                       &lo->line_cap, lo->line_count + 1);
    if (!nl) return NEOX_ERROR_OUT_OF_MEMORY;
    lo->line = nl;
    return NEOX_OK;
}

static void layout_finish_line(nx_layout *lo, uint32_t g0, float w)
{
    lo->line[lo->line_count].start = g0;
    lo->line[lo->line_count].count = lo->glyph_count - g0;
    lo->line[lo->line_count].width = w;
    lo->line_count++;
}

neox_result nx_text_layout(nx_text_context *t, nx_font *font, float size_px,
                           const char *utf8, float max_width,
                           nx_text_align align, nx_layout **out_layout)
{
    nx_layout *lo;
    uint32_t *gidx = NULL;      /* 字形索引（输入序） */
    float *gadv = NULL;         /* 每字形前进宽 */
    uint32_t n = 0, cap_idx = 0, cap_adv = 0, i;
    neox_result r = NEOX_ERROR_OUT_OF_MEMORY;
    const char *p, *end;
    nx_token *tok = NULL;
    uint32_t ntok = 0, tokcap = 0;

    if (!t || !font || !utf8 || !out_layout) return NEOX_ERROR_INVALID_ARGUMENT;
    *out_layout = NULL;
    if (!(size_px > 0.0f)) TX_ERR(t, "size_px must be positive",
                                  NEOX_ERROR_INVALID_ARGUMENT);

    lo = (nx_layout *)tx_alloc(t, sizeof(*lo), 16);
    if (!lo) TX_ERR(t, "layout allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    memset(lo, 0, sizeof(*lo));
    lo->tctx = t;
    lo->font = font;
    lo->size_px = size_px;
    lo->ascent = font->be.ascent * size_px;
    lo->descent = font->be.descent * size_px;
    lo->line_gap = font->be.line_gap * size_px;
    lo->line_height = lo->ascent - lo->descent + lo->line_gap;

    /* 1) UTF-8 解码 + cmap + 逐字形前进宽 */
    p = utf8;
    end = utf8 + strlen(utf8);
    while (nx_utf8_next(&p, end, &i /* 复用为码点临时 */)) {
        uint32_t cp = i, gi, *ng;
        float *na;
        if (n == cap_idx) {
            ng = (uint32_t *)tx_array_grow(t, gidx, 4, &cap_idx, n + 1);
            if (!ng) goto fail;
            gidx = ng;
        }
        if (n == cap_adv) {
            na = (float *)tx_array_grow(t, gadv, 4, &cap_adv, n + 1);
            if (!na) goto fail;
            gadv = na;
        }
        gi = font_glyph_index(font, cp);
        gidx[n] = gi;
        gadv[n] = font->be.glyph_advance(font->be.user, gi, size_px);
        n++;
    }

    /* 2) 分词：CJK 单字成词；空白独立成词；其余连续成词（类别数组法） */
    {
        /* 码点类别数组（1=空白 2=CJK 3=词字符 4=换行） */
        uint8_t *cls = (uint8_t *)tx_alloc(t, n ? n : 1, 16);
        float pen;
        uint32_t line_g0 = 0;
        float line_w = 0.0f;
        uint32_t tok_i;

        if (!cls) goto fail;
        p = utf8;
        {
            uint32_t k = 0;
            while (k < n && nx_utf8_next(&p, end, &i)) {
                if (i == '\n') cls[k] = 4;
                else if (i == ' ' || i == '\t') cls[k] = 1;
                else if (nx_is_cjk(i)) cls[k] = 2;
                else cls[k] = 3;
                k++;
            }
        }

        /* 分词 */
        i = 0;
        while (i < n) {
            nx_token tk;
            memset(&tk, 0, sizeof(tk));
            tk.glyph0 = i;
            if (cls[i] == 4) {           /* '\n'：强制断行标记 */
                tk.hard_break = 1;
                tk.count = 1;
                i += 1;
            } else if (cls[i] == 1) {    /* 空白 */
                tk.is_space = 1;
                tk.count = 1;
                tk.width = gadv[i];
                i += 1;
            } else if (cls[i] == 2) {    /* CJK：单字成词 */
                tk.count = 1;
                tk.width = gadv[i];
                i += 1;
            } else {                     /* 拉丁词：连续词字符 + 词内字距 */
                float w = 0.0f;
                tk.glyph0 = i;
                while (i < n && cls[i] == 3) {
                    w += gadv[i];
                    if (i + 1 < n && cls[i + 1] == 3) {
                        w += font->be.glyph_kerning(font->be.user, gidx[i],
                                                    gidx[i + 1], size_px);
                    }
                    i++;
                }
                tk.count = i - tk.glyph0;
                tk.width = w;
            }
            {
                nx_token *nt = (nx_token *)tx_array_grow(t, tok, sizeof(nx_token),
                                                         &tokcap, ntok + 1);
                if (!nt) {
                    tx_free(t, cls, n ? n : 1, 16);
                    goto fail;
                }
                tok = nt;
                tok[ntok++] = tk;
            }
        }
        tx_free(t, cls, n ? n : 1, 16);

        /* 3) 贪心断行 + 放置 */
        pen = 0.0f;
        line_g0 = 0;
        line_w = 0.0f;
        for (tok_i = 0; tok_i < ntok; tok_i++) {
            nx_token *tk = &tok[tok_i];
            if (tk->hard_break) {
                r = layout_add_line(lo);
                if (r != NEOX_OK) goto fail;
                layout_finish_line(lo, line_g0, line_w);
                pen = 0.0f;
                line_w = 0.0f;
                line_g0 = lo->glyph_count;
                continue;
            }
            /* 行首空白丢弃 */
            if (tk->is_space && lo->glyph_count == line_g0)
                continue;
            /* 贪心换行：本行已有内容且放不下 */
            if (max_width > 0.0f && lo->glyph_count > line_g0 &&
                pen + tk->width > max_width) {
                r = layout_add_line(lo);
                if (r != NEOX_OK) goto fail;
                layout_finish_line(lo, line_g0, line_w);
                pen = 0.0f;
                line_w = 0.0f;
                line_g0 = lo->glyph_count;
                if (tk->is_space)
                    continue;   /* 换行后行首空白同样丢弃 */
            }
            /* 放置词内字形 */
            {
                uint32_t k;
                float adv = pen;
                for (k = 0; k < tk->count; k++) {
                    uint32_t gi = tk->glyph0 + k;
                    nx_laid_glyph lg;
                    if (lo->glyph_count == lo->glyph_cap) {
                        nx_laid_glyph *ngl = (nx_laid_glyph *)tx_array_grow(
                            t, lo->glyphs, sizeof(nx_laid_glyph),
                            &lo->glyph_cap, lo->glyph_count + 1);
                        if (!ngl) goto fail;
                        lo->glyphs = ngl;
                    }
                    lg.glyph_index = gidx[gi];
                    lg.x = adv;
                    lg.y = 0.0f;   /* y 在行定稿后统一设置 */
                    lo->glyphs[lo->glyph_count++] = lg;
                    adv += gadv[gi];
                    if (k + 1 < tk->count) {
                        adv += font->be.glyph_kerning(font->be.user, gidx[gi],
                                                      gidx[gi + 1], size_px);
                    }
                }
                pen = adv;
                line_w += tk->width;
            }
        }
        /* 收尾行 */
        r = layout_add_line(lo);
        if (r != NEOX_OK) goto fail;
        layout_finish_line(lo, line_g0, line_w);

        /* 4) 对齐 + 基线 y + 包围盒 */
        {
            float box_w = 0.0f;
            for (i = 0; i < lo->line_count; i++) {
                uint32_t g0 = lo->line[i].start;
                uint32_t cnt = lo->line[i].count;
                float off = 0.0f;
                float ybase = (float)i * lo->line_height;
                uint32_t k;
                if (align == NX_ALIGN_CENTER && max_width > 0.0f)
                    off = (max_width - lo->line[i].width) * 0.5f;
                else if (align == NX_ALIGN_RIGHT && max_width > 0.0f)
                    off = max_width - lo->line[i].width;
                for (k = 0; k < cnt; k++) {
                    lo->glyphs[g0 + k].x += off;
                    lo->glyphs[g0 + k].y = ybase;
                }
                if (lo->line[i].width > box_w) box_w = lo->line[i].width;
            }
            lo->width = box_w;
            lo->height = (float)(lo->line_count ? lo->line_count - 1 : 0) *
                         lo->line_height + lo->ascent - lo->descent;
        }
    }

    r = NEOX_OK;
    *out_layout = lo;
    lo = NULL;

fail:
    if (tok) tx_free(t, tok, (size_t)tokcap * sizeof(nx_token), 16);
    if (gidx) tx_free(t, gidx, (size_t)cap_idx * 4, 16);
    if (gadv) tx_free(t, gadv, (size_t)cap_adv * 4, 16);
    if (lo) nx_layout_destroy(lo);
    if (r != NEOX_OK && !t->last_error)
        t->last_error = "layout failed (out of memory)";
    return r;
}

/* ------------------------------------------------------------------ 动态图集 */

typedef struct nx_atlas_entry {
    int in_use;
    nx_font *font;
    uint32_t glyph;
    uint32_t size_q;        /* size_px × 4（0.25px 量化） */
    nx_atlas_rect rect;
    float bear_x, bear_y;
    uint64_t last_use;      /* LRU 字段（v0 不驱逐，结构保留） */
} nx_atlas_entry;

typedef struct nx_shelf {
    uint32_t y, h, cursor_x;
} nx_shelf;

#define NX_ATLAS_CACHE_CAP 4096u   /* 开放寻址表容量（2 的幂） */
#define NX_ATLAS_PAD 1u            /* 字形间 1px 间隔，防双线性串扰 */

struct nx_atlas {
    nx_text_context *tctx;
    nx_atlas *next;             /* tctx 级联链 */
    neox_device *dev;
    neox_texture tex;           /* 图集纹理（COVERAGE/SDF → A8；MSDF → RGBA8_LINEAR） */
    neox_atlas_mode mode;       /* 创建时固定；缓存天然按图集（模式）隔离 */
    float dist_range;           /* 距离场编码范围（图集像素；COVERAGE 下无意义） */
    uint32_t bpp;               /* 纹理/镜像每像素字节（1 或 4） */
    uint32_t w, h;
    uint8_t *shadow;            /* [own] CPU 侧镜像（A8 或 RGBA8） */
    nx_shelf *shelves;
    uint32_t nshelves, capshelves;
    uint32_t shelf_y;           /* 下一条新 shelf 的 y */
    nx_atlas_entry *cache;      /* [own] 开放寻址 */
    uint64_t tick;
    uint64_t hits, misses;
    /* 脏矩形（包围盒；含自上次 flush 的全部写入） */
    int dirty;
    uint32_t dx0, dy0, dx1, dy1;
};

static uint32_t atlas_hash(nx_font *font, uint32_t glyph, uint32_t size_q)
{
    uint32_t h = (uint32_t)(uintptr_t)font;
    h = h * 1664525u + glyph;
    h = h * 1664525u + size_q;
    h ^= h >> 16;
    return h & (NX_ATLAS_CACHE_CAP - 1u);
}

static void atlas_unlink(nx_atlas *a)
{
    nx_text_context *t = a->tctx;
    nx_atlas **link = &t->atlases;
    while (*link && *link != a) link = &(*link)->next;
    if (*link) *link = a->next;
}

neox_result nx_atlas_create(nx_text_context *t, neox_device *dev,
                            uint32_t width, uint32_t height, nx_atlas **out)
{
    /* 现行行为 = COVERAGE 覆盖率图集（dist_range 缺省 4，模式下行无关） */
    return nx_atlas_create_ex(t, dev, width, height, NEOX_ATLAS_COVERAGE, 0.0f,
                              out);
}

neox_result nx_atlas_create_ex(nx_text_context *t, neox_device *dev,
                               uint32_t width, uint32_t height,
                               neox_atlas_mode mode, float dist_range_px,
                               nx_atlas **out)
{
    nx_atlas *a;
    neox_texture_desc td;
    uint8_t *zero;
    if (!t || !dev || !out || width == 0 || height == 0 ||
        mode > NEOX_ATLAS_MSDF)
        return NEOX_ERROR_INVALID_ARGUMENT;
    if (dist_range_px < 0.0f)
        TX_ERR(t, "dist_range_px must be >= 0", NEOX_ERROR_INVALID_ARGUMENT);
    if (dist_range_px == 0.0f) dist_range_px = 4.0f;   /* 契约缺省 */
    *out = NULL;
    a = (nx_atlas *)tx_alloc(t, sizeof(*a), 16);
    if (!a) TX_ERR(t, "atlas allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    memset(a, 0, sizeof(*a));
    a->tctx = t;
    a->dev = dev;
    a->w = width;
    a->h = height;
    a->mode = mode;
    a->dist_range = dist_range_px;
    a->bpp = (mode == NEOX_ATLAS_MSDF) ? 4u : 1u;
    a->shadow = (uint8_t *)tx_alloc(t, (size_t)width * height * a->bpp, 16);
    if (!a->shadow) {
        tx_free(t, a, sizeof(*a), 16);
        TX_ERR(t, "atlas shadow buffer allocation failed",
               NEOX_ERROR_OUT_OF_MEMORY);
    }
    memset(a->shadow, 0, (size_t)width * height * a->bpp);
    a->cache = (nx_atlas_entry *)tx_alloc(t, NX_ATLAS_CACHE_CAP * sizeof(nx_atlas_entry), 16);
    if (!a->cache) {
        tx_free(t, a->shadow, (size_t)width * height * a->bpp, 16);
        tx_free(t, a, sizeof(*a), 16);
        TX_ERR(t, "atlas cache allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    }
    memset(a->cache, 0, NX_ATLAS_CACHE_CAP * sizeof(nx_atlas_entry));
    /* 图集纹理：初始全零（MSDF 含 alpha=0；绘制端忽略 alpha 仅用于核心预乘） */
    zero = (uint8_t *)tx_alloc(t, (size_t)width * height * a->bpp, 16);
    if (!zero) {
        tx_free(t, a->cache, NX_ATLAS_CACHE_CAP * sizeof(nx_atlas_entry), 16);
        tx_free(t, a->shadow, (size_t)width * height * a->bpp, 16);
        tx_free(t, a, sizeof(*a), 16);
        TX_ERR(t, "atlas texture staging allocation failed",
               NEOX_ERROR_OUT_OF_MEMORY);
    }
    memset(zero, 0, (size_t)width * height * a->bpp);
    memset(&td, 0, sizeof(td));
    td.width = width;
    td.height = height;
    td.format = (mode == NEOX_ATLAS_MSDF) ? NEOX_FORMAT_RGBA8_LINEAR
                                          : NEOX_FORMAT_A8;
    td.pixels = zero;
    td.row_stride = (size_t)width * a->bpp;
    if (neox_texture_create(dev, &td, &a->tex) != NEOX_OK) {
        tx_free(t, zero, (size_t)width * height * a->bpp, 16);
        tx_free(t, a->cache, NX_ATLAS_CACHE_CAP * sizeof(nx_atlas_entry), 16);
        tx_free(t, a->shadow, (size_t)width * height * a->bpp, 16);
        tx_free(t, a, sizeof(*a), 16);
        TX_ERR(t, "atlas texture creation failed", NEOX_ERROR_BACKEND);
    }
    tx_free(t, zero, (size_t)width * height * a->bpp, 16);
    a->next = t->atlases;
    t->atlases = a;
    *out = a;
    return NEOX_OK;
}

void nx_atlas_destroy(nx_atlas *a)
{
    nx_text_context *t;
    if (!a) return;
    t = a->tctx;
    atlas_unlink(a);
    if (a->tex.id) neox_texture_release(a->dev, a->tex);
    if (a->shadow) tx_free(t, a->shadow, (size_t)a->w * a->h * a->bpp, 16);
    if (a->shelves) tx_free(t, a->shelves, (size_t)a->capshelves * sizeof(nx_shelf), 16);
    if (a->cache) tx_free(t, a->cache, NX_ATLAS_CACHE_CAP * sizeof(nx_atlas_entry), 16);
    tx_free(t, a, sizeof(*a), 16);
}

neox_texture nx_atlas_texture(const nx_atlas *a)
{
    neox_texture z = { 0 };
    return a ? a->tex : z;
}

neox_atlas_mode nx_atlas_get_mode(const nx_atlas *a)
{
    return a ? a->mode : NEOX_ATLAS_COVERAGE;
}

float nx_atlas_get_dist_range(const nx_atlas *a)
{
    return a ? a->dist_range : 0.0f;
}

void nx_atlas_stats(const nx_atlas *a, uint64_t *out_hits, uint64_t *out_misses)
{
    if (!a) return;
    if (out_hits) *out_hits = a->hits;
    if (out_misses) *out_misses = a->misses;
}

/* shelf/row 装箱：优先现有 shelf 同行追加，否则开新 shelf；容量满返回 0 */
static int atlas_pack(nx_atlas *a, uint32_t w, uint32_t h, nx_atlas_rect *out)
{
    uint32_t i, need = w + NX_ATLAS_PAD;
    for (i = 0; i < a->nshelves; i++) {
        nx_shelf *s = &a->shelves[i];
        if (s->h >= h && s->cursor_x + need <= a->w) {
            out->x = (uint16_t)s->cursor_x;
            out->y = (uint16_t)s->y;
            out->w = (uint16_t)w;
            out->h = (uint16_t)h;
            s->cursor_x += need;
            return 1;
        }
    }
    if (a->shelf_y + h > a->h)
        return 0;
    {
        nx_shelf *ns = (nx_shelf *)tx_array_grow(a->tctx, a->shelves,
                          sizeof(nx_shelf), &a->capshelves, a->nshelves + 1);
        if (!ns) return 0;
        a->shelves = ns;
        a->shelves[a->nshelves].y = a->shelf_y;
        a->shelves[a->nshelves].h = h;
        a->shelves[a->nshelves].cursor_x = need;
        a->nshelves++;
        out->x = 0;
        out->y = (uint16_t)a->shelf_y;
        out->w = (uint16_t)w;
        out->h = (uint16_t)h;
        a->shelf_y += h + NX_ATLAS_PAD;
    }
    return 1;
}

static void atlas_dirty(nx_atlas *a, uint32_t x0, uint32_t y0,
                        uint32_t x1, uint32_t y1)
{
    if (!a->dirty) {
        a->dirty = 1;
        a->dx0 = x0; a->dy0 = y0;
        a->dx1 = x1; a->dy1 = y1;
        return;
    }
    if (x0 < a->dx0) a->dx0 = x0;
    if (y0 < a->dy0) a->dy0 = y0;
    if (x1 > a->dx1) a->dx1 = x1;
    if (y1 > a->dy1) a->dy1 = y1;
}

neox_result nx_atlas_flush(nx_atlas *a)
{
    neox_rect region;
    neox_result r;
    const uint8_t *src;
    if (!a) return NEOX_ERROR_INVALID_ARGUMENT;
    if (!a->dirty) return NEOX_OK;
    region.x = (float)a->dx0;
    region.y = (float)a->dy0;
    region.w = (float)(a->dx1 - a->dx0);
    region.h = (float)(a->dy1 - a->dy0);
    src = a->shadow + ((size_t)a->dy0 * a->w + a->dx0) * a->bpp;
    r = neox_texture_update(a->dev, a->tex, region, src, a->w * a->bpp);
    if (r == NEOX_OK) a->dirty = 0;
    return r;
}

/* 内部取字形（含 bearing），供绘制桥使用 */
static neox_result atlas_lookup(nx_atlas *a, nx_font *font, uint32_t glyph,
                                float size_px, nx_atlas_rect *out_rect,
                                float *out_bx, float *out_by)
{
    uint32_t size_q = (uint32_t)(size_px * 4.0f + 0.5f);
    uint32_t slot = atlas_hash(font, glyph, size_q);
    uint32_t probe;
    nx_atlas_entry *e;

    for (probe = 0; probe < NX_ATLAS_CACHE_CAP; probe++) {
        e = &a->cache[(slot + probe) & (NX_ATLAS_CACHE_CAP - 1u)];
        if (!e->in_use) break;
        if (e->font == font && e->glyph == glyph && e->size_q == size_q) {
            a->hits++;
            e->last_use = ++a->tick;
            *out_rect = e->rect;
            *out_bx = e->bear_x;
            *out_by = e->bear_y;
            return NEOX_OK;
        }
    }
    /* 未命中：光栅化（按图集模式）+ 装箱（量化后的字号，保证缓存键与位图一致） */
    {
        float size_qf = (float)size_q * 0.25f;
        uint8_t *bmp = NULL;
        int32_t bw = 0, bh = 0;
        float bx = 0.0f, by = 0.0f;
        nx_atlas_rect rc;
        neox_result r;
        if (a->mode == NEOX_ATLAS_COVERAGE) {
            r = font->be.glyph_rasterize(font->be.user, glyph, size_qf,
                                         &bmp, &bw, &bh, &bx, &by);
        } else {
            if (!font->be.glyph_rasterize_sdf) {
                a->tctx->last_error =
                    "font backend does not support SDF/MSDF rasterization";
                return NEOX_ERROR_UNSUPPORTED;
            }
            r = font->be.glyph_rasterize_sdf(font->be.user, glyph, size_qf,
                                             a->mode, a->dist_range,
                                             &bmp, &bw, &bh, &bx, &by);
        }
        if (r != NEOX_OK) {
            a->tctx->last_error = "glyph rasterization failed";
            return r;
        }
        if (bw <= 0 || bh <= 0) {           /* 空字形（如空格）：零矩形缓存 */
            memset(&rc, 0, sizeof(rc));
        } else {
            uint32_t y;
            if (!atlas_pack(a, (uint32_t)bw, (uint32_t)bh, &rc)) {
                if (bmp) font->be.glyph_free_bitmap(font->be.user, bmp);
                a->tctx->last_error =
                    "atlas capacity exhausted (v0 不做 LRU 驱逐)";
                return NEOX_ERROR_OUT_OF_MEMORY;
            }
            /* SDF/MSDF 位图已含 dist_range 边距，整块拷贝（行字节 bw×bpp） */
            for (y = 0; y < (uint32_t)bh; y++)
                memcpy(a->shadow + ((size_t)(rc.y + y) * a->w + rc.x) * a->bpp,
                       bmp + (size_t)y * bw * a->bpp, (size_t)bw * a->bpp);
            atlas_dirty(a, rc.x, rc.y, rc.x + (uint32_t)bw, rc.y + (uint32_t)bh);
        }
        if (bmp) font->be.glyph_free_bitmap(font->be.user, bmp);
        /* 插入缓存（从查找时的首个空槽开始探测） */
        for (probe = 0; probe < NX_ATLAS_CACHE_CAP; probe++) {
            e = &a->cache[(slot + probe) & (NX_ATLAS_CACHE_CAP - 1u)];
            if (!e->in_use) {
                e->in_use = 1;
                e->font = font;
                e->glyph = glyph;
                e->size_q = size_q;
                e->rect = rc;
                e->bear_x = bx;
                e->bear_y = by;
                e->last_use = ++a->tick;
                a->misses++;
                *out_rect = rc;
                *out_bx = bx;
                *out_by = by;
                return NEOX_OK;
            }
            if (e->font == font && e->glyph == glyph && e->size_q == size_q) {
                /* 并发同键（单线程下不发生）；视为命中 */
                a->hits++;
                *out_rect = e->rect;
                *out_bx = e->bear_x;
                *out_by = e->bear_y;
                return NEOX_OK;
            }
        }
        a->tctx->last_error = "atlas cache table exhausted";
        return NEOX_ERROR_OUT_OF_MEMORY;
    }
}

neox_result nx_atlas_glyph(nx_atlas *a, nx_font *font, uint32_t glyph_index,
                           float size_px, nx_atlas_rect *out_rect)
{
    float bx, by;
    if (!a || !font || !out_rect || !(size_px > 0.0f))
        return NEOX_ERROR_INVALID_ARGUMENT;
    return atlas_lookup(a, font, glyph_index, size_px, out_rect, &bx, &by);
}

/* ------------------------------------------------------------------ 绘制桥 */

static neox_result text_draw_impl(neox_frame *frame, nx_atlas *a,
                                  const nx_layout *layout,
                                  const neox_paint *paint,
                                  const neox_glyph_style *style)
{
    neox_result r;
    neox_glyph *arr;
    uint32_t i;
    float bx, by;
    if (!frame || !a || !layout || !paint || !style)
        return NEOX_ERROR_INVALID_ARGUMENT;
    if (layout->glyph_count == 0) return NEOX_OK;
    arr = (neox_glyph *)tx_alloc(a->tctx,
                                 (size_t)layout->glyph_count * sizeof(neox_glyph),
                                 16);
    if (!arr) {
        a->tctx->last_error = "glyph array allocation failed";
        return NEOX_ERROR_OUT_OF_MEMORY;
    }
    /* 先收集全部字形（未命中即光栅化并记脏矩形），再统一 flush 上传，
     * 最后绘制——顺序不可颠倒（flush 必须先于本次产生的脏矩形） */
    for (i = 0; i < layout->glyph_count; i++) {
        const nx_laid_glyph *lg = &layout->glyphs[i];
        nx_atlas_rect rc;
        r = atlas_lookup(a, layout->font, lg->glyph_index, layout->size_px,
                         &rc, &bx, &by);
        if (r != NEOX_OK) {
            tx_free(a->tctx, arr,
                    (size_t)layout->glyph_count * sizeof(neox_glyph), 16);
            return r;
        }
        /* 位图左上角 = 基线原点 + bearing（bear_y <= 0 在基线上方）；
         * SDF/MSDF 位图含边距，bear 已外扩，同一放置公式成立 */
        arr[i].x = lg->x + bx;
        arr[i].y = lg->y + by;
        arr[i].sx = rc.x;
        arr[i].sy = rc.y;
        arr[i].sw = rc.w;
        arr[i].sh = rc.h;
    }
    r = nx_atlas_flush(a);
    if (r == NEOX_OK)
        r = neox_draw_glyphs_ex(frame, a->tex, arr, layout->glyph_count,
                                paint, style);
    tx_free(a->tctx, arr, (size_t)layout->glyph_count * sizeof(neox_glyph), 16);
    return r;
}

neox_result nx_text_draw(neox_frame *frame, nx_atlas *a,
                         const nx_layout *layout, const neox_paint *paint)
{
    return nx_text_draw_ex(frame, a, layout, paint, 0.0f);
}

neox_result nx_text_draw_ex(neox_frame *frame, nx_atlas *a,
                            const nx_layout *layout, const neox_paint *paint,
                            float softness)
{
    neox_glyph_style style;
    if (!a) return NEOX_ERROR_INVALID_ARGUMENT;
    /* 图集模式与 dist_range 成对携带（§8.2：两处一致由模块保证） */
    style.atlas_mode = a->mode;
    style.dist_range_px = a->dist_range;
    style.softness = softness;
    return text_draw_impl(frame, a, layout, paint, &style);
}
