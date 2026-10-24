/* ===========================================================================
 * test_gpu_consistency.c — NeoX 跨后端一致性测试（Soft vs GL，§9 容差）
 *
 * 固定场景集分别在 Soft 与 GL（OpenGL 4.1 Core）后端渲染，按规范 §9 比较：
 *   - 实心内部（距任何边界 ≥ 2px）：逐位一致；
 *   - 抗锯齿边界像素：覆盖率差 ≤ 32/255，且覆盖率沿边界单调
 *     （单调性判定为代理检查：两后端在边界相邻像素间的强度变化方向一致，
 *     即无振铃/反单调台阶）。
 *
 * 覆盖：非自交/自交路径 × NONZERO/EVENODD、13 种 Porter-Duff 模式逐一、
 *       组不透明度 vs 逐元素不透明度、嵌套裁剪交集、2×2 纹理
 *       NEAREST/LINEAR 放大与负宽镜像、A8 字形着色、scene_render 与
 *       立即模式等价性（两后端各自逐位 + 跨后端容差）、CAP 跨后端回放。
 *
 * 无 GL 环境（CI 无显示 / 未启用 NEOX_BACKEND_GL）时打印 SKIP 并返回 0。
 * =========================================================================*/
#include "neox.h"
#include "neox_gl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ 最小断言框架 */
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

/* ------------------------------------------------------------------ 后端夹具 */
static neox_context *g_ctx;
static neox_device *g_soft;
static neox_device *g_gpu;
static int g_gpu_ok;

/* ------------------------------------------------------------------ 资源（每设备一份） */
typedef struct t_res {
    neox_path rect;        /* 非自交矩形 */
    neox_path blob;        /* 三次贝塞尔闭合形（NONZERO） */
    neox_path star_ee;     /* 自相交星形（EVENODD） */
    neox_path star_nz;     /* 自相交星形（NONZERO） */
    neox_path full;        /* 全幅矩形 */
    neox_path clip_a;      /* 裁剪 1 */
    neox_path clip_b;      /* 裁剪 2（嵌套交集） */
    neox_path clip_oct;    /* 八边形（旋转/场景测试用） */
    neox_texture tex2x2;   /* sRGB 2×2 */
    neox_texture atlas;    /* A8 8×8 伪字形 */
} t_res;

static neox_path mk_path(neox_device *dev, const uint8_t *verbs, uint32_t nv,
                         const float *pts, uint32_t np, neox_fill_rule rule)
{
    neox_path_desc d;
    neox_path p = { 0 };
    memset(&d, 0, sizeof(d));
    d.verbs = verbs; d.points = pts;
    d.verb_count = nv; d.point_count = np;
    d.fill_rule = rule;
    if (neox_path_create(dev, &d, &p) != NEOX_OK) {
        fprintf(stderr, "mk_path failed\n");
        exit(2);
    }
    return p;
}

static neox_path mk_rect(neox_device *dev, float x, float y, float w, float h,
                         neox_fill_rule rule)
{
    static const uint8_t verbs[] = {
        NEOX_VERB_MOVE, NEOX_VERB_LINE, NEOX_VERB_LINE, NEOX_VERB_LINE, NEOX_VERB_CLOSE
    };
    float pts[8] = { x, y, x + w, y, x + w, y + h, x, y + h };
    return mk_path(dev, verbs, 5, pts, 4, rule);
}

static void res_create(neox_device *dev, t_res *r)
{
    static const uint8_t bverbs[] = {
        NEOX_VERB_MOVE, NEOX_VERB_CUBIC, NEOX_VERB_CUBIC,
        NEOX_VERB_CUBIC, NEOX_VERB_CLOSE
    };
    static const float bpts[20] = {
        5, 30,   20, 5,   35, 25,   45, 10,
        55, 25,   55, 40,   45, 50,
        35, 55,   15, 55,   5, 30
    };
    uint8_t sverbs[11];
    float spts[20];
    uint8_t overbs[9];
    float opts[16];
    uint32_t i;
    const uint8_t t2[2 * 2 * 4] = {
        255, 0, 0, 255,    0, 255, 0, 255,
        0, 0, 255, 255,    255, 255, 255, 255
    };
    uint8_t apx[8 * 8];
    neox_texture_desc td;

    /* 星形（10 顶点自相交） */
    for (i = 0; i < 5; i++) {
        float a1 = (float)i * 4.0f * 3.14159265f / 5.0f - 1.5707963f;
        float a2 = a1 + 0.62831853f;
        spts[i * 4 + 0] = 18.0f * cosf(a1);
        spts[i * 4 + 1] = 18.0f * sinf(a1);
        spts[i * 4 + 2] = 7.5f * cosf(a2);
        spts[i * 4 + 3] = 7.5f * sinf(a2);
        sverbs[i * 2] = NEOX_VERB_LINE;
        sverbs[i * 2 + 1] = NEOX_VERB_LINE;
    }
    sverbs[0] = NEOX_VERB_MOVE;
    sverbs[10] = NEOX_VERB_CLOSE;

    /* 八边形 */
    for (i = 0; i < 8; i++) {
        float a = (float)i * 3.14159265f / 4.0f;
        opts[i * 2] = 26.0f * cosf(a);
        opts[i * 2 + 1] = 26.0f * sinf(a);
        overbs[i] = NEOX_VERB_LINE;
    }
    overbs[0] = NEOX_VERB_MOVE;
    overbs[8] = NEOX_VERB_CLOSE;

    /* A8 图集：边框 + 确定性伪笔触 + 一条渐变 */
    for (i = 0; i < 64; i++) {
        uint32_t x = i % 8, y = i / 8;
        int on = (x == 0 || x == 7 || y == 0 || y == 7) ||
                 (((x * 7 + y * 13) % 11) < 4);
        apx[i] = on ? 255 : (uint8_t)(x * 36);
    }

    r->rect = mk_rect(dev, 0, 0, 24, 18, NEOX_FILL_NONZERO);
    r->blob = mk_path(dev, bverbs, 5, bpts, 10, NEOX_FILL_NONZERO);
    r->star_ee = mk_path(dev, sverbs, 11, spts, 10, NEOX_FILL_EVENODD);
    r->star_nz = mk_path(dev, sverbs, 11, spts, 10, NEOX_FILL_NONZERO);
    r->full = mk_rect(dev, 0, 0, 96, 64, NEOX_FILL_NONZERO);
    r->clip_a = mk_rect(dev, 10, 8, 40, 30, NEOX_FILL_NONZERO);
    r->clip_b = mk_rect(dev, 34, 22, 48, 32, NEOX_FILL_NONZERO);
    r->clip_oct = mk_path(dev, overbs, 9, opts, 8, NEOX_FILL_NONZERO);

    memset(&td, 0, sizeof(td));
    td.width = 2; td.height = 2;
    td.format = NEOX_FORMAT_RGBA8_SRGB;
    td.pixels = t2;
    td.row_stride = 8;
    if (neox_texture_create(dev, &td, &r->tex2x2) != NEOX_OK) exit(2);
    memset(&td, 0, sizeof(td));
    td.width = 8; td.height = 8;
    td.format = NEOX_FORMAT_A8;
    td.pixels = apx;
    td.row_stride = 8;
    if (neox_texture_create(dev, &td, &r->atlas) != NEOX_OK) exit(2);
}

static void res_destroy(neox_device *dev, t_res *r)
{
    neox_path_release(dev, r->rect);
    neox_path_release(dev, r->blob);
    neox_path_release(dev, r->star_ee);
    neox_path_release(dev, r->star_nz);
    neox_path_release(dev, r->full);
    neox_path_release(dev, r->clip_a);
    neox_path_release(dev, r->clip_b);
    neox_path_release(dev, r->clip_oct);
    neox_texture_release(dev, r->tex2x2);
    neox_texture_release(dev, r->atlas);
    neox_device_pump(dev);
}

/* ------------------------------------------------------------------ 渲染通道 */
typedef void (*draw_fn)(neox_device *dev, neox_frame *fr, const t_res *r);

static int render_soft(const t_res *r, draw_fn fn, uint32_t w, uint32_t h,
                       const neox_color *clear, uint8_t *out)
{
    neox_target_desc td;
    neox_frame *fr;
    neox_submission sub;
    memset(&td, 0, sizeof(td));
    td.kind = NEOX_BACKEND_SOFT;
    td.soft.pixels = out;
    td.soft.stride = (size_t)w * 4;
    td.soft.width = w;
    td.soft.height = h;
    td.clear = clear;
    if (neox_frame_begin(g_soft, &td, &fr) != NEOX_OK) return -1;
    fn(g_soft, fr, r);
    if (neox_frame_end(fr, &sub) != NEOX_OK) return -1;
    neox_device_pump(g_soft);
    return neox_submission_get_status(g_soft, sub) == NEOX_SUBMISSION_COMPLETE
           ? 0 : -1;
}

static int render_gpu(const t_res *r, draw_fn fn, uint32_t w, uint32_t h,
                      const neox_color *clear, uint8_t *out)
{
    neox_gl_surface *surf = NULL;
    neox_target_desc td;
    neox_frame *fr;
    neox_submission sub;
    int rc = -1;
    if (neox_gl_surface_create(g_gpu, w, h, &surf) != NEOX_OK) return -1;
    memset(&td, 0, sizeof(td));
    td.kind = NEOX_BACKEND_GPU;
    td.surface = surf;
    td.clear = clear;
    if (neox_frame_begin(g_gpu, &td, &fr) != NEOX_OK) goto out;
    fn(g_gpu, fr, r);
    if (neox_frame_end(fr, &sub) != NEOX_OK) goto out;
    neox_device_pump(g_gpu);
    if (neox_submission_get_status(g_gpu, sub) != NEOX_SUBMISSION_COMPLETE)
        goto out;
    if (neox_gl_surface_read_pixels(surf, out) != NEOX_OK) goto out;
    rc = 0;
out:
    neox_gl_surface_destroy(g_gpu, surf);
    return rc;
}

/* ------------------------------------------------------------------ §9 比较 */
#define CMP_TOL 32   /* §9：边界覆盖率差 ≤ 32/255 */

typedef struct cmp_stats {
    uint32_t interior_mismatch; /* 实心内部非逐位一致的像素数 */
    uint32_t boundary_over;     /* 边界像素通道差 > 32 的像素数 */
    uint32_t mono_viol;         /* 单调性代理违例（像素×方向 计数） */
    int max_diff;               /* 边界像素最大通道差（实测报告用） */
} cmp_stats;

/* 以 soft 结果为参照计算边界：任何通道存在 4-邻域差 → 边界，再 2px 膨胀 */
static uint8_t *find_boundary(const uint8_t *a, uint32_t w, uint32_t h)
{
    uint8_t *b = (uint8_t *)calloc((size_t)w * h, 1);
    uint32_t x, y, k;
    int d;
    static const int dx[4] = { 1, -1, 0, 0 };
    static const int dy[4] = { 0, 0, 1, -1 };
    if (!b) exit(2);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const uint8_t *p = &a[((size_t)y * w + x) * 4];
            for (d = 0; d < 4; d++) {
                uint32_t nx = (uint32_t)((int)x + dx[d]);
                uint32_t ny = (uint32_t)((int)y + dy[d]);
                const uint8_t *q;
                if (nx >= w || ny >= h) continue;
                q = &a[((size_t)ny * w + nx) * 4];
                if (memcmp(p, q, 4) != 0) { b[y * w + x] = 1; break; }
            }
        }
    }
    /* Chebyshev 膨胀 2px（"距边界 ≥ 2px 的内部" 的取反） */
    for (k = 0; k < 2; k++) {
        uint8_t *nb = (uint8_t *)malloc((size_t)w * h);
        int ox, oy;
        memset(nb, 0, (size_t)w * h);
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                int hit = 0;
                for (oy = -1; oy <= 1 && !hit; oy++)
                    for (ox = -1; ox <= 1 && !hit; ox++) {
                        int xx = (int)x + ox, yy = (int)y + oy;
                        if (xx >= 0 && yy >= 0 && xx < (int)w && yy < (int)h &&
                            b[(size_t)yy * w + (size_t)xx]) hit = 1;
                    }
                nb[y * w + x] = (uint8_t)hit;
            }
        }
        free(b);
        b = nb;
    }
    return b;
}

/* 单调性代理：边界带内，两后端在相邻像素间强度变化方向不得相反 */
static uint32_t mono_check(const uint8_t *a, const uint8_t *b,
                           const uint8_t *boundary, uint32_t w, uint32_t h)
{
    uint32_t x, y, viol = 0;
    int d;
    static const int dx[4] = { 1, 0 };
    static const int dy[4] = { 0, 1 };
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const uint8_t *pa, *pb;
            int sa, sb;
            if (!boundary[y * w + x]) continue;
            pa = &a[((size_t)y * w + x) * 4];
            pb = &b[((size_t)y * w + x) * 4];
            for (d = 0; d < 2; d++) {
                uint32_t nx = x + (uint32_t)dx[d], ny = y + (uint32_t)dy[d];
                const uint8_t *qa, *qb;
                int ia, ib, i;
                if (nx >= w || ny >= h) continue;
                qa = &a[((size_t)ny * w + nx) * 4];
                qb = &b[((size_t)ny * w + nx) * 4];
                ia = (int)pa[0] + (int)pa[1] + (int)pa[2];
                ib = (int)qa[0] + (int)qa[1] + (int)qa[2];
                sa = (ib > ia) - (ib < ia);
                ia = (int)pb[0] + (int)pb[1] + (int)pb[2];
                ib = (int)qb[0] + (int)qb[1] + (int)qb[2];
                sb = (ib > ia) - (ib < ia);
                if (sa != 0 && sb != 0 && sa != sb) {
                    (void)i;
                    viol++;
                }
            }
        }
    }
    return viol;
}

static void write_bmp(const char *path, const uint8_t *rgba, uint32_t w,
                      uint32_t h)
{
    FILE *f = fopen(path, "wb");
    uint32_t row_bytes = w * 3, pad = (4 - (row_bytes % 4)) % 4;
    uint32_t img_size = (row_bytes + pad) * h;
    uint8_t header[54];
    int x, y;
    if (!f) return;
    memset(header, 0, sizeof(header));
    header[0] = 'B'; header[1] = 'M';
    {
        uint32_t file_size = 54 + img_size, off = 54;
        int32_t sw = (int32_t)w, sh = (int32_t)h;
        memcpy(header + 2, &file_size, 4);
        memcpy(header + 10, &off, 4);
        header[14] = 40;
        memcpy(header + 18, &sw, 4);
        memcpy(header + 22, &sh, 4);
        header[26] = 1; header[28] = 24;
        memcpy(header + 34, &img_size, 4);
    }
    fwrite(header, 1, 54, f);
    for (y = (int)h - 1; y >= 0; y--) {
        for (x = 0; x < (int)w; x++) {
            const uint8_t *p = &rgba[((size_t)y * w + (size_t)x) * 4];
            uint8_t bgr[3] = { p[2], p[1], p[0] };
            fwrite(bgr, 1, 3, f);
        }
        if (pad) { static const uint8_t z[3] = { 0, 0, 0 }; fwrite(z, 1, pad, f); }
    }
    fclose(f);
}

static void compare_images(const uint8_t *soft, const uint8_t *gl,
                           uint32_t w, uint32_t h, const char *name,
                           cmp_stats *st)
{
    uint8_t *boundary = find_boundary(soft, w, h);
    uint32_t x, y;
    memset(st, 0, sizeof(*st));
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const uint8_t *pa = &soft[((size_t)y * w + x) * 4];
            const uint8_t *pb = &gl[((size_t)y * w + x) * 4];
            if (boundary[y * w + x]) {
                int c, md = 0;
                for (c = 0; c < 4; c++) {
                    int dd = (int)pa[c] - (int)pb[c];
                    if (dd < 0) dd = -dd;
                    if (dd > md) md = dd;
                }
                if (md > st->max_diff) st->max_diff = md;
                if (md > CMP_TOL) {
                    st->boundary_over++;
                    if (st->boundary_over <= 4)
                        fprintf(stderr, "  bo px(%u,%u): soft=(%u,%u,%u,%u)"
                                " gl=(%u,%u,%u,%u)\n",
                                x, y, pa[0], pa[1], pa[2], pa[3],
                                pb[0], pb[1], pb[2], pb[3]);
                }
            } else if (memcmp(pa, pb, 4) != 0) {
                st->interior_mismatch++;
                if (st->interior_mismatch <= 5)
                    fprintf(stderr, "  interior px(%u,%u): soft=(%u,%u,%u,%u)"
                            " gl=(%u,%u,%u,%u)\n",
                            x, y, pa[0], pa[1], pa[2], pa[3],
                            pb[0], pb[1], pb[2], pb[3]);
            }
        }
    }
    st->mono_viol = mono_check(soft, gl, boundary, w, h);
    free(boundary);
    if (getenv("NX_DUMP")) {
        char path[128];
        snprintf(path, sizeof(path), "/tmp/nx_%s_soft.bmp", name);
        write_bmp(path, soft, w, h);
        snprintf(path, sizeof(path), "/tmp/nx_%s_gl.bmp", name);
        write_bmp(path, gl, w, h);
    }
    printf("  [%-18s] interior_mismatch=%u boundary_over_32=%u mono_viol=%u"
           " boundary_maxdiff=%d\n",
           name, st->interior_mismatch, st->boundary_over,
           st->mono_viol, st->max_diff);
    CHECK(st->interior_mismatch == 0);
    CHECK(st->boundary_over == 0);
    CHECK(st->mono_viol == 0);
}

/* ------------------------------------------------------------------ 资源实例（每后端一份） */
static t_res g_res_soft, g_res_gpu;

static void check_pair(const char *name, draw_fn fn, uint32_t w, uint32_t h,
                       const neox_color *clear)
{
    uint8_t *a = (uint8_t *)malloc((size_t)w * h * 4);
    uint8_t *b = (uint8_t *)malloc((size_t)w * h * 4);
    cmp_stats st;
    CHECK(render_soft(&g_res_soft, fn, w, h, clear, a) == 0);
    CHECK(render_gpu(&g_res_gpu, fn, w, h, clear, b) == 0);
    compare_images(a, b, w, h, name, &st);
    free(a);
    free(b);
}

/* ------------------------------------------------------------------ 场景 */
static const neox_color CLEAR_GRAY = { 0.85f, 0.86f, 0.88f, 1.0f };

static void push(neox_frame *fr, neox_affine t, float opacity, neox_blend blend,
                 const neox_path *clip)
{
    neox_state st;
    memset(&st, 0, sizeof(st));
    st.transform = t;
    st.opacity = opacity;
    st.blend = blend;
    st.clip = clip;
    if (neox_push_state(fr, &st) != NEOX_OK) {
        fprintf(stderr, "push_state failed\n");
        exit(2);
    }
}

static void pop(neox_frame *fr)
{
    if (neox_pop_state(fr) != NEOX_OK) { fprintf(stderr, "pop failed\n"); exit(2); }
}

/* 非自交 + 自交路径 × 两种填充规则 */
static void sc_paths(neox_device *dev, neox_frame *fr, const t_res *r)
{
    neox_paint pt;
    (void)dev;
    pt.color = (neox_color){ 0.85f, 0.25f, 0.22f, 1.0f };
    push(fr, (neox_affine){ 1, 0, 0, 1, 12, 10 }, 1.0f, NEOX_BLEND_SRC_OVER, NULL);
    neox_draw_path(fr, r->rect, &pt);
    pop(fr);
    pt.color = (neox_color){ 0.2f, 0.4f, 0.85f, 0.8f };
    push(fr, (neox_affine){ 1, 0, 0, 1, 34, 6 }, 1.0f, NEOX_BLEND_SRC_OVER, NULL);
    neox_draw_path(fr, r->blob, &pt);
    pop(fr);
    pt.color = (neox_color){ 0.95f, 0.8f, 0.15f, 1.0f };
    push(fr, (neox_affine){ 1, 0, 0, 1, 68, 22 }, 1.0f, NEOX_BLEND_SRC_OVER, NULL);
    neox_draw_path(fr, r->star_ee, &pt);
    pop(fr);
    pt.color = (neox_color){ 0.25f, 0.7f, 0.35f, 0.9f };
    push(fr, (neox_affine){ 1.2f, 0, 0, 1.2f, 68, 46 }, 1.0f, NEOX_BLEND_SRC_OVER, NULL);
    neox_draw_path(fr, r->star_nz, &pt);
    pop(fr);
}

static neox_blend g_blend_mode = NEOX_BLEND_SRC_OVER;
static void sc_blend(neox_device *dev, neox_frame *fr, const t_res *r)
{
    neox_paint pt;
    (void)dev;
    pt.color = (neox_color){ 0.8f, 0.15f, 0.12f, 1.0f };
    neox_draw_path(fr, r->full, &pt);
    pt.color = (neox_color){ 0.15f, 0.3f, 0.9f, 0.5f };
    push(fr, (neox_affine){ 1, 0, 0, 1, 8, 6 }, 1.0f, g_blend_mode, NULL);
    neox_draw_path(fr, r->blob, &pt);
    pop(fr);
    pt.color = (neox_color){ 0.2f, 0.75f, 0.4f, 0.7f };
    push(fr, (neox_affine){ 0.8f, 0, 0, 0.8f, 40, 20 }, 1.0f, g_blend_mode, NULL);
    neox_draw_path(fr, r->star_nz, &pt);
    pop(fr);
}

/* 组不透明度：隔离合成（§7） */
static void sc_group(neox_device *dev, neox_frame *fr, const t_res *r)
{
    neox_paint pt;
    (void)dev;
    push(fr, (neox_affine){ 1, 0, 0, 1, 14, 12 }, 0.5f, NEOX_BLEND_SRC_OVER, NULL);
    pt.color = (neox_color){ 0.85f, 0.25f, 0.22f, 1.0f };
    neox_draw_path(fr, r->rect, &pt);
    pt.color = (neox_color){ 0.2f, 0.4f, 0.85f, 1.0f };
    push(fr, (neox_affine){ 1, 0, 0, 1, 18, 14 }, 1.0f, NEOX_BLEND_SRC_OVER, NULL);
    neox_draw_path(fr, r->rect, &pt);
    pop(fr);
    pop(fr);
}

/* 逐元素不透明度（与组不透明度语义不同，同样必须跨后端一致） */
static void sc_individual(neox_device *dev, neox_frame *fr, const t_res *r)
{
    neox_paint pt;
    (void)dev;
    pt.color = (neox_color){ 0.85f, 0.25f, 0.22f, 0.5f };
    push(fr, (neox_affine){ 1, 0, 0, 1, 14, 12 }, 1.0f, NEOX_BLEND_SRC_OVER, NULL);
    neox_draw_path(fr, r->rect, &pt);
    pop(fr);
    pt.color = (neox_color){ 0.2f, 0.4f, 0.85f, 0.5f };
    push(fr, (neox_affine){ 1, 0, 0, 1, 32, 26 }, 1.0f, NEOX_BLEND_SRC_OVER, NULL);
    neox_draw_path(fr, r->rect, &pt);
    pop(fr);
}

/* 嵌套裁剪 = 祖先交集（§7） */
static void sc_nested_clip(neox_device *dev, neox_frame *fr, const t_res *r)
{
    neox_paint pt;
    (void)dev;
    pt.color = (neox_color){ 0.15f, 0.2f, 0.25f, 1.0f };
    push(fr, (neox_affine){ 1, 0, 0, 1, 0, 0 }, 1.0f, NEOX_BLEND_SRC_OVER, &r->clip_a);
    push(fr, (neox_affine){ 1, 0, 0, 1, 0, 0 }, 1.0f, NEOX_BLEND_SRC_OVER, &r->clip_b);
    neox_draw_path(fr, r->full, &pt);
    pop(fr);
    pop(fr);
}

static void sc_tex_nearest(neox_device *dev, neox_frame *fr, const t_res *r)
{
    (void)dev;
    push(fr, (neox_affine){ 1, 0, 0, 1, 10, 8 }, 1.0f, NEOX_BLEND_SRC_OVER, NULL);
    neox_draw_texture(fr, r->tex2x2, (neox_rect){ 0, 0, 2, 2 },
                      (neox_rect){ 0, 0, 24, 20 }, NEOX_FILTER_NEAREST, 1.0f);
    pop(fr);
}

static void sc_tex_linear(neox_device *dev, neox_frame *fr, const t_res *r)
{
    (void)dev;
    push(fr, (neox_affine){ 1, 0, 0, 1, 10, 8 }, 1.0f, NEOX_BLEND_SRC_OVER, NULL);
    neox_draw_texture(fr, r->tex2x2, (neox_rect){ 0, 0, 2, 2 },
                      (neox_rect){ 0, 0, 27, 19 }, NEOX_FILTER_LINEAR, 1.0f);
    pop(fr);
}

/* 负宽目标矩形 → 水平镜像（§8） */
static void sc_tex_mirror(neox_device *dev, neox_frame *fr, const t_res *r)
{
    (void)dev;
    push(fr, (neox_affine){ 1, 0, 0, 1, 50, 8 }, 1.0f, NEOX_BLEND_SRC_OVER, NULL);
    neox_draw_texture(fr, r->tex2x2, (neox_rect){ 0, 0, 2, 2 },
                      (neox_rect){ 24, 0, -24, 20 }, NEOX_FILTER_NEAREST, 0.85f);
    pop(fr);
}

/* A8 字形着色（§5：遮罩 × paint）+ 旋转/缩放变换 */
static void sc_glyphs(neox_device *dev, neox_frame *fr, const t_res *r)
{
    neox_glyph gl_[3];
    neox_paint pt;
    (void)dev;
    gl_[0].x = 6;  gl_[0].y = 4;  gl_[0].sx = 0; gl_[0].sy = 0; gl_[0].sw = 8; gl_[0].sh = 8;
    gl_[1].x = 16; gl_[1].y = 8;  gl_[1].sx = 0; gl_[1].sy = 0; gl_[1].sw = 8; gl_[1].sh = 8;
    gl_[2].x = 27; gl_[2].y = 3;  gl_[2].sx = 0; gl_[2].sy = 0; gl_[2].sw = 8; gl_[2].sh = 8;
    pt.color = (neox_color){ 0.2f, 0.55f, 0.75f, 0.9f };
    push(fr, (neox_affine){ 1, 0, 0, 1, 10, 12 }, 1.0f, NEOX_BLEND_SRC_OVER, NULL);
    neox_draw_glyphs(fr, r->atlas, gl_, 3, &pt);
    pop(fr);
    pt.color = (neox_color){ 0.75f, 0.35f, 0.6f, 0.8f };
    push(fr, (neox_affine){ 1.4f, 0.3f, -0.2f, 1.2f, 48, 20 }, 1.0f,
         NEOX_BLEND_SRC_OVER, NULL);
    neox_draw_glyphs(fr, r->atlas, gl_, 2, &pt);
    pop(fr);
}

/* 综合场景：组 + 八边形裁剪 + 旋转 + 混合 + 纹理 + 字形 */
static void sc_kitchen(neox_device *dev, neox_frame *fr, const t_res *r)
{
    neox_paint pt;
    (void)dev;
    push(fr, (neox_affine){ 0.996f, -0.087f, 0.087f, 0.996f, 48, 32 },
         0.85f, NEOX_BLEND_SRC_OVER, &r->clip_oct);
    pt.color = (neox_color){ 0.85f, 0.25f, 0.22f, 0.9f };
    push(fr, (neox_affine){ 1, 0, 0, 1, 20, 12 }, 1.0f, NEOX_BLEND_SRC_OVER, NULL);
    neox_draw_path(fr, r->blob, &pt);
    pop(fr);
    push(fr, (neox_affine){ 1, 0, 0, 1, 26, 18 }, 1.0f, NEOX_BLEND_PLUS, NULL);
    neox_draw_path(fr, r->star_ee, &pt);
    pop(fr);
    push(fr, (neox_affine){ 1, 0, 0, 1, 30, 24 }, 1.0f, NEOX_BLEND_SRC_OVER, NULL);
    neox_draw_texture(fr, r->tex2x2, (neox_rect){ 0, 0, 2, 2 },
                      (neox_rect){ 0, 0, 18, 14 }, NEOX_FILTER_LINEAR, 0.9f);
    pop(fr);
    {
        neox_glyph gg;
        gg.x = 8; gg.y = 30; gg.sx = 0; gg.sy = 0; gg.sw = 8; gg.sh = 8;
        pt.color = (neox_color){ 0.9f, 0.85f, 0.2f, 0.85f };
        push(fr, (neox_affine){ 1.5f, 0, 0, 1.5f, 0, 0 }, 1.0f,
             NEOX_BLEND_SRC_OVER, NULL);
        neox_draw_glyphs(fr, r->atlas, &gg, 1, &pt);
        pop(fr);
    }
    pop(fr);
}


/* §6.1：CLEAR 抗锯齿擦除与 DST 无变化在半覆盖像素上的跨后端一致性。
 * 非整数矩形制造 0.5 覆盖率边缘，星形制造曲线覆盖率。 */
static void sc_clear_partial(neox_device *dev, neox_frame *fr, const t_res *r)
{
    neox_paint pt;
    (void)dev;
    pt.color = (neox_color){ 0.15f, 0.3f, 0.9f, 0.5f };
    push(fr, (neox_affine){ 1, 0, 0, 1, 20.5f, 10.25f }, 1.0f, NEOX_BLEND_CLEAR, NULL);
    neox_draw_path(fr, r->rect, &pt);
    pop(fr);
    pt.color = (neox_color){ 0.9f, 0.5f, 0.2f, 0.8f };
    push(fr, (neox_affine){ 1, 0, 0, 1, 58, 30 }, 1.0f, NEOX_BLEND_CLEAR, NULL);
    neox_draw_path(fr, r->star_nz, &pt);
    pop(fr);
}

static void sc_dst_partial(neox_device *dev, neox_frame *fr, const t_res *r)
{
    neox_paint pt;
    neox_paint pb;
    (void)dev;
    /* 先铺半透明底（DST 下应完全不可见），再 DST 绘制 */
    pb.color = (neox_color){ 0.85f, 0.25f, 0.22f, 1.0f };
    push(fr, (neox_affine){ 1, 0, 0, 1, 30, 18 }, 1.0f, NEOX_BLEND_SRC_OVER, NULL);
    neox_draw_path(fr, r->rect, &pb);
    pop(fr);
    pt.color = (neox_color){ 0.15f, 0.3f, 0.9f, 0.5f };
    push(fr, (neox_affine){ 1, 0, 0, 1, 20.5f, 10.25f }, 1.0f, NEOX_BLEND_DST, NULL);
    neox_draw_path(fr, r->blob, &pt);
    pop(fr);
    pt.color = (neox_color){ 0.9f, 0.5f, 0.2f, 0.8f };
    push(fr, (neox_affine){ 1, 0, 0, 1, 58, 30 }, 1.0f, NEOX_BLEND_DST, NULL);
    neox_draw_path(fr, r->star_ee, &pt);
    pop(fr);
}

/* ------------------------------------------------------------------ scene_render 等价性（§10.2-1） */

static void draw_immediate_kitchen(neox_frame *fr, const t_res *r)
{
    sc_kitchen(NULL, fr, r);
}

static neox_scene build_kitchen_scene(neox_device *dev, const t_res *r,
                                      neox_node *out_group)
{
    neox_scene sc = neox_scene_create(dev);
    neox_node group, n;
    neox_node_desc nd;
    neox_glyph gg;
    CHECK(sc.id != 0);

    memset(&nd, 0, sizeof(nd));
    nd.kind = NEOX_NODE_GROUP;
    nd.transform = (neox_affine){ 0.996f, -0.087f, 0.087f, 0.996f, 48, 32 };
    nd.opacity = 0.85f;
    nd.visible = 1;
    nd.group.clip = r->clip_oct;
    CHECK_RES(neox_node_create(dev, sc, &nd, &group), NEOX_OK);

    memset(&nd, 0, sizeof(nd));
    nd.kind = NEOX_NODE_PATH;
    nd.transform = (neox_affine){ 1, 0, 0, 1, 20, 12 };
    nd.opacity = 1.0f;
    nd.visible = 1;
    nd.path.path = r->blob;
    nd.path.paint.color = (neox_color){ 0.85f, 0.25f, 0.22f, 0.9f };
    CHECK_RES(neox_node_create(dev, sc, &nd, &n), NEOX_OK);
    CHECK_RES(neox_node_append_child(dev, group, n), NEOX_OK);

    memset(&nd, 0, sizeof(nd));
    nd.kind = NEOX_NODE_PATH;
    nd.transform = (neox_affine){ 1, 0, 0, 1, 26, 18 };
    nd.blend = NEOX_BLEND_PLUS;
    nd.opacity = 1.0f;
    nd.visible = 1;
    nd.path.path = r->star_ee;
    nd.path.paint.color = (neox_color){ 0.85f, 0.25f, 0.22f, 0.9f };
    CHECK_RES(neox_node_create(dev, sc, &nd, &n), NEOX_OK);
    CHECK_RES(neox_node_append_child(dev, group, n), NEOX_OK);

    memset(&nd, 0, sizeof(nd));
    nd.kind = NEOX_NODE_IMAGE;
    nd.transform = (neox_affine){ 1, 0, 0, 1, 30, 24 };
    nd.opacity = 1.0f;
    nd.visible = 1;
    nd.image.tex = r->tex2x2;
    nd.image.src = (neox_rect){ 0, 0, 2, 2 };
    nd.image.dst = (neox_rect){ 0, 0, 18, 14 };
    nd.image.filter = NEOX_FILTER_LINEAR;
    CHECK_RES(neox_node_create(dev, sc, &nd, &n), NEOX_OK);
    CHECK_RES(neox_node_append_child(dev, group, n), NEOX_OK);

    memset(&gg, 0, sizeof(gg));
    gg.x = 8; gg.y = 30; gg.sx = 0; gg.sy = 0; gg.sw = 8; gg.sh = 8;
    memset(&nd, 0, sizeof(nd));
    nd.kind = NEOX_NODE_GLYPHS;
    nd.transform = (neox_affine){ 1.5f, 0, 0, 1.5f, 0, 0 };
    nd.opacity = 1.0f;
    nd.visible = 1;
    nd.glyphs.atlas = r->atlas;
    nd.glyphs.glyphs = &gg;
    nd.glyphs.count = 1;
    nd.glyphs.paint.color = (neox_color){ 0.9f, 0.85f, 0.2f, 0.85f };
    CHECK_RES(neox_node_create(dev, sc, &nd, &n), NEOX_OK);
    CHECK_RES(neox_node_append_child(dev, group, n), NEOX_OK);

    *out_group = group;
    return sc;
}

/* ------------------------------------------------------------------ 测试主体 */
static void test_device_semantics(void)
{
    neox_caps caps;
    neox_exec_report rep;
    uint8_t tmp[8 * 8 * 4];
    neox_gl_surface *surf;
    neox_target_desc td;
    neox_frame *fr;
    neox_submission sub;
    neox_paint pt;

    CHECK(neox_device_is_lost(g_gpu) == 0);
    neox_device_get_caps(g_gpu, &caps);
    CHECK(caps.features & NEOX_CAP_FLOAT_BLEND);
    CHECK(caps.max_texture_size >= 16384);

    /* platform_size 协商（neox_device_desc 约定）：platform 为 NULL 时
     * platform_size 须为 0；plat_size 过小 / struct_size 超过 plat_size /
     * 版本不符均拒绝 */
    {
        neox_device_desc dd2;
        neox_gl_device_desc gld2;
        neox_device *dev2 = NULL;
        memset(&dd2, 0, sizeof(dd2));
        dd2.backend = NEOX_BACKEND_GPU;
        dd2.platform_size = 8; /* platform == NULL */
        CHECK_RES(neox_device_create(g_ctx, &dd2, &dev2),
                  NEOX_ERROR_INVALID_ARGUMENT);
        memset(&gld2, 0, sizeof(gld2));
        gld2.struct_size = (uint32_t)sizeof(gld2);
        memset(&dd2, 0, sizeof(dd2));
        dd2.backend = NEOX_BACKEND_GPU;
        dd2.platform = &gld2;
        dd2.platform_size = 4; /* 不足以覆盖结构体 */
        CHECK_RES(neox_device_create(g_ctx, &dd2, &dev2),
                  NEOX_ERROR_INVALID_ARGUMENT);
        dd2.platform_size = sizeof(gld2);
        gld2.struct_size = (uint32_t)sizeof(gld2) + 16; /* 声明超过 plat_size */
        CHECK_RES(neox_device_create(g_ctx, &dd2, &dev2),
                  NEOX_ERROR_INVALID_ARGUMENT);
        gld2.struct_size = (uint32_t)sizeof(gld2) - 4; /* 版本不符 */
        CHECK_RES(neox_device_create(g_ctx, &dd2, &dev2),
                  NEOX_ERROR_INVALID_ARGUMENT);
    }
    /* exec report：algorithm = TRIANGLE */
    CHECK_RES(neox_gl_surface_create(g_gpu, 8, 8, &surf), NEOX_OK);
    memset(&td, 0, sizeof(td));
    td.kind = NEOX_BACKEND_GPU;
    td.surface = surf;
    td.clear = &CLEAR_GRAY;
    CHECK_RES(neox_frame_begin(g_gpu, &td, &fr), NEOX_OK);
    pt.color = (neox_color){ 0.5f, 0.2f, 0.6f, 1.0f };
    CHECK_RES(neox_draw_path(fr, g_res_gpu.rect, &pt), NEOX_OK);
    CHECK_RES(neox_frame_end(fr, &sub), NEOX_OK);
    neox_device_pump(g_gpu);
    CHECK(neox_submission_get_status(g_gpu, sub) == NEOX_SUBMISSION_COMPLETE);
    CHECK_RES(neox_submission_get_report(g_gpu, sub, &rep), NEOX_OK);
    CHECK(rep.algorithm == NEOX_ALG_TRIANGLE);
    CHECK(rep.draw_calls == 1);
    CHECK(neox_gl_surface_read_pixels(surf, tmp) == NEOX_OK);
    /* 表面借用校验：非本设备表面 → OWNERSHIP_VIOLATION */
    {
        neox_device_desc dd;
        neox_device *dev2 = NULL;
        neox_frame *fr2;
        memset(&dd, 0, sizeof(dd));
        dd.backend = NEOX_BACKEND_SOFT;
        if (neox_device_create(g_ctx, &dd, &dev2) == NEOX_OK) {
            neox_target_desc t2;
            memset(&t2, 0, sizeof(t2));
            t2.kind = NEOX_BACKEND_GPU;
            t2.surface = surf;
            CHECK_RES(neox_frame_begin(dev2, &t2, &fr2),
                      NEOX_ERROR_INVALID_ARGUMENT); /* kind 不匹配先行 */
            neox_device_destroy(dev2);
        }
    }
    neox_gl_surface_destroy(g_gpu, surf);
}

static void test_blend_matrix(void)
{
    static const char *names[13] = {
        "src_over", "clear", "src", "dst", "dst_over", "src_in", "dst_in",
        "src_out", "dst_out", "src_atop", "dst_atop", "xor", "plus"
    };
    char label[32];
    int m;
    for (m = 0; m < 13; m++) {
        g_blend_mode = (neox_blend)m;
        snprintf(label, sizeof(label), "blend-%s", names[m]);
        check_pair(label, sc_blend, 96, 64, &CLEAR_GRAY);
    }
}

static void test_scene_equivalence(void)
{
    uint8_t a[96 * 64 * 4], b[96 * 64 * 4], c[96 * 64 * 4], d[96 * 64 * 4];
    neox_scene sc_s, sc_g;
    neox_node grp_s, grp_g;
    cmp_stats st;

    sc_s = build_kitchen_scene(g_soft, &g_res_soft, &grp_s);
    sc_g = build_kitchen_scene(g_gpu, &g_res_gpu, &grp_g);

    /* soft：scene → a；立即 → b（§10.2-1 要求同后端逐位一致） */
    {
        neox_target_desc td;
        neox_frame *fr;
        neox_submission sub;
        memset(&td, 0, sizeof(td));
        td.kind = NEOX_BACKEND_SOFT;
        td.soft.stride = 96 * 4;
        td.soft.width = 96;
        td.soft.height = 64;
        td.clear = &CLEAR_GRAY;
        td.soft.pixels = a;
        CHECK_RES(neox_frame_begin(g_soft, &td, &fr), NEOX_OK);
        CHECK_RES(neox_scene_render(fr, sc_s, (neox_node){ 0 }), NEOX_OK);
        CHECK_RES(neox_frame_end(fr, &sub), NEOX_OK);
        neox_device_pump(g_soft);
        td.soft.pixels = b;
        CHECK_RES(neox_frame_begin(g_soft, &td, &fr), NEOX_OK);
        draw_immediate_kitchen(fr, &g_res_soft);
        CHECK_RES(neox_frame_end(fr, &sub), NEOX_OK);
        neox_device_pump(g_soft);
    }
    /* gpu：scene vs 立即 */
    {
        neox_gl_surface *surf = NULL;
        neox_target_desc td;
        neox_frame *fr;
        neox_submission sub;
        CHECK_RES(neox_gl_surface_create(g_gpu, 96, 64, &surf), NEOX_OK);
        memset(&td, 0, sizeof(td));
        td.kind = NEOX_BACKEND_GPU;
        td.surface = surf;
        td.clear = &CLEAR_GRAY;
        CHECK_RES(neox_frame_begin(g_gpu, &td, &fr), NEOX_OK);
        CHECK_RES(neox_scene_render(fr, sc_g, (neox_node){ 0 }), NEOX_OK);
        CHECK_RES(neox_frame_end(fr, &sub), NEOX_OK);
        neox_device_pump(g_gpu);
        CHECK_RES(neox_gl_surface_read_pixels(surf, c), NEOX_OK);
        CHECK_RES(neox_frame_begin(g_gpu, &td, &fr), NEOX_OK);
        draw_immediate_kitchen(fr, &g_res_gpu);
        CHECK_RES(neox_frame_end(fr, &sub), NEOX_OK);
        neox_device_pump(g_gpu);
        CHECK_RES(neox_gl_surface_read_pixels(surf, d), NEOX_OK);
        neox_gl_surface_destroy(g_gpu, surf);
    }
    /* scene_render 与立即模式共享命令流展开：各后端内逐位一致 */
    CHECK(memcmp(a, b, sizeof(a)) == 0);
    CHECK(memcmp(c, d, sizeof(c)) == 0);
    /* 跨后端 §9 容差 */
    compare_images(a, c, 96, 64, "scene-render", &st);

    neox_scene_destroy(g_soft, sc_s);
    neox_scene_destroy(g_gpu, sc_g);
    neox_device_pump(g_soft);
    neox_device_pump(g_gpu);
}

/* CAP 跨后端回放（CAP 工作于命令流层级，对 GPU 后端天然有效——此处验证） */
static void test_cap_across_backends(void)
{
    uint8_t a[64 * 64 * 4], b[64 * 64 * 4];
    neox_capture *cap;
    uint8_t *data = NULL;
    size_t size = 0;
    neox_paint pt;
    cmp_stats st;

    /* Soft 设备录制 */
    CHECK_RES(neox_capture_begin(g_soft, &cap), NEOX_OK);
    {
        neox_target_desc td;
        neox_frame *fr;
        neox_submission sub;
        memset(&td, 0, sizeof(td));
        td.kind = NEOX_BACKEND_SOFT;
        td.soft.pixels = a;
        td.soft.stride = 64 * 4;
        td.soft.width = 64;
        td.soft.height = 64;
        td.clear = &CLEAR_GRAY;
        CHECK_RES(neox_frame_begin(g_soft, &td, &fr), NEOX_OK);
        push(fr, (neox_affine){ 1, 0, 0, 1, 0, 0 }, 0.7f, NEOX_BLEND_SRC_OVER,
             &g_res_soft.clip_oct);
        pt.color = (neox_color){ 0.3f, 0.6f, 0.85f, 0.9f };
        neox_draw_path(fr, g_res_soft.blob, &pt);
        pt.color = (neox_color){ 0.9f, 0.5f, 0.2f, 0.8f };
        push(fr, (neox_affine){ 1, 0, 0, 1, 24, 10 }, 1.0f, NEOX_BLEND_PLUS, NULL);
        neox_draw_path(fr, g_res_soft.star_nz, &pt);
        pop(fr);
        pop(fr);
        CHECK_RES(neox_frame_end(fr, &sub), NEOX_OK);
        CHECK_RES(neox_capture_end(cap, &data, &size), NEOX_OK);
        neox_device_pump(g_soft);
    }
    /* GL 设备回放 → 与 Soft 原帧 §9 容差内一致 */
    {
        neox_gl_surface *surf = NULL;
        neox_target_desc td;
        neox_frame *fr;
        neox_submission sub;
        CHECK_RES(neox_gl_surface_create(g_gpu, 64, 64, &surf), NEOX_OK);
        memset(&td, 0, sizeof(td));
        td.kind = NEOX_BACKEND_GPU;
        td.surface = surf;
        td.clear = &CLEAR_GRAY;
        CHECK_RES(neox_frame_begin(g_gpu, &td, &fr), NEOX_OK);
        CHECK_RES(neox_capture_replay(fr, data, size), NEOX_OK);
        CHECK_RES(neox_frame_end(fr, &sub), NEOX_OK);
        neox_device_pump(g_gpu);
        CHECK(neox_submission_get_status(g_gpu, sub) == NEOX_SUBMISSION_COMPLETE);
        CHECK_RES(neox_gl_surface_read_pixels(surf, b), NEOX_OK);
        neox_gl_surface_destroy(g_gpu, surf);
    }
    compare_images(a, b, 64, 64, "cap-soft->gl", &st);
    neox_capture_free_buffer(g_ctx, data, size);
}

/* ------------------------------------------------------------------ main */
int main(void)
{
    neox_context_desc cd;
    neox_device_desc dd;

    memset(&cd, 0, sizeof(cd));
    if (neox_context_create(&cd, &g_ctx) != NEOX_OK) {
        fprintf(stderr, "context_create failed\n");
        return 2;
    }
    memset(&dd, 0, sizeof(dd));
    dd.backend = NEOX_BACKEND_SOFT;
    if (neox_device_create(g_ctx, &dd, &g_soft) != NEOX_OK) {
        fprintf(stderr, "soft device_create failed\n");
        return 2;
    }
    {
        neox_gl_device_desc gld;
        memset(&gld, 0, sizeof(gld));
        gld.struct_size = (uint32_t)sizeof(gld);
        gld.shared_context = NULL;
        memset(&dd, 0, sizeof(dd));
        dd.backend = NEOX_BACKEND_GPU;
        dd.platform = &gld;
        dd.platform_size = sizeof(gld);
        if (neox_device_create(g_ctx, &dd, &g_gpu) != NEOX_OK) {
            printf("SKIP: GL backend unavailable (%s)\n",
                   neox_get_error_detail(g_ctx));
            neox_device_destroy(g_soft);
            neox_context_destroy(g_ctx);
            return 0; /* 无 GL 环境：SKIP 而非 FAIL */
        }
    }
    g_gpu_ok = 1;
    (void)g_gpu_ok;

    res_create(g_soft, &g_res_soft);
    res_create(g_gpu, &g_res_gpu);

    test_device_semantics();

    check_pair("paths", sc_paths, 96, 64, &CLEAR_GRAY);
    test_blend_matrix();
    check_pair("group-opacity", sc_group, 96, 64, &CLEAR_GRAY);
    check_pair("individual-alpha", sc_individual, 96, 64, &CLEAR_GRAY);
    check_pair("nested-clip", sc_nested_clip, 96, 64, &CLEAR_GRAY);
    check_pair("tex-nearest", sc_tex_nearest, 96, 64, &CLEAR_GRAY);
    check_pair("tex-linear", sc_tex_linear, 96, 64, &CLEAR_GRAY);
    check_pair("tex-mirror", sc_tex_mirror, 96, 64, &CLEAR_GRAY);
    check_pair("glyphs", sc_glyphs, 96, 64, &CLEAR_GRAY);
    check_pair("blend-clear-partial", sc_clear_partial, 96, 64, &CLEAR_GRAY);
    check_pair("blend-dst-partial", sc_dst_partial, 96, 64, &CLEAR_GRAY);
    check_pair("kitchen-sink", sc_kitchen, 96, 64, &CLEAR_GRAY);

    test_scene_equivalence();
    test_cap_across_backends();

    res_destroy(g_soft, &g_res_soft);
    res_destroy(g_gpu, &g_res_gpu);
    neox_device_destroy(g_gpu);
    neox_device_destroy(g_soft);
    neox_context_destroy(g_ctx);

    if (g_failures == 0) {
        printf("ALL PASS (%d checks)\n", g_checks);
        return 0;
    }
    printf("%d/%d checks failed\n", g_failures, g_checks);
    return 1;
}
