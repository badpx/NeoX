/* ===========================================================================
 * geom.c — 路径 flatten 与覆盖率光栅化（Soft / GL 后端共享）
 *
 * 规范引用（docs/semantics-v0.md）：§3 变换、§4 路径几何、§4.4 覆盖率。
 *
 * 共享原因：跨后端一致性（§9）要求两后端的几何输入完全一致，因此
 * 曲线细分（flatten）与裁剪掩码光栅化只此一份实现：
 *   - Soft：flatten → 扫描线填充；裁剪掩码直接作为覆盖率参与合成。
 *   - GL  ：flatten → 三角扇 stencil-then-cover；裁剪掩码在 push 期由
 *           本文件急切光栅化（与 Soft 同一覆盖率语义），执行期上传纹理。
 * =========================================================================*/
#include "internal.h"

/* ------------------------------------------------------------------ 路径 flatten（§4）
 * 曲线在设备空间（已应用 CTM）flatten，容差 NEOX_FLAT_TOL 像素、最深 12 层。
 * 输出：点列表 + 轮廓表（轮廓隐式闭合，填充语义）。 */

static neox_result flat_put_pt(neox_context *ctx, nx_flat *f, float x, float y)
{
    float *np = (float *)nx_array_grow(ctx, f->pts, sizeof(float),
                                       &f->cap_pts, f->npts + 2);
    if (!np) return NEOX_ERROR_OUT_OF_MEMORY;
    f->pts = np;
    f->pts[f->npts++] = x;
    f->pts[f->npts++] = y;
    if (f->npolys > 0) f->polys[f->npolys - 1].count++;
    return NEOX_OK;
}

static neox_result flat_begin_poly(neox_context *ctx, nx_flat *f)
{
    nx_poly *np = (nx_poly *)nx_array_grow(ctx, f->polys, sizeof(nx_poly),
                                           &f->cap_polys, f->npolys + 1);
    if (!np) return NEOX_ERROR_OUT_OF_MEMORY;
    f->polys = np;
    f->polys[f->npolys].start = f->npts / 2;
    f->polys[f->npolys].count = 0;
    f->npolys++;
    return NEOX_OK;
}

/* 二次贝塞尔细分判平：控制点对弦中点的偏离 ≤ 容差 */
static int quad_is_flat(float x0, float y0, float cx, float cy, float x1, float y1)
{
    float dx = cx * 2.0f - x0 - x1;
    float dy = cy * 2.0f - y0 - y1;
    return fabsf(dx) <= NEOX_FLAT_TOL * 2.0f && fabsf(dy) <= NEOX_FLAT_TOL * 2.0f;
}

static neox_result flat_quad(neox_context *ctx, nx_flat *f,
                             float x0, float y0, float cx, float cy,
                             float x1, float y1, int depth)
{
    if ((quad_is_flat(x0, y0, cx, cy, x1, y1) && depth > 0) ||
        depth >= NEOX_FLAT_MAX_DEPTH) {
        return flat_put_pt(ctx, f, x1, y1);
    }
    {
        float mx01 = (x0 + cx) * 0.5f, my01 = (y0 + cy) * 0.5f;
        float mx12 = (cx + x1) * 0.5f, my12 = (cy + y1) * 0.5f;
        float mxx  = (mx01 + mx12) * 0.5f, myy = (my01 + my12) * 0.5f;
        neox_result r = flat_quad(ctx, f, x0, y0, mx01, my01, mxx, myy, depth + 1);
        if (r != NEOX_OK) return r;
        return flat_quad(ctx, f, mxx, myy, mx12, my12, x1, y1, depth + 1);
    }
}

/* 三次贝塞尔细分判平：两控制点均值对端点均值的偏离 ≤ 容差 */
static int cubic_is_flat(float x0, float y0, float c1x, float c1y,
                         float c2x, float c2y, float x1, float y1)
{
    float dx = c1x + c2x - x0 - x1;
    float dy = c1y + c2y - y0 - y1;
    return fabsf(dx) <= NEOX_FLAT_TOL * 2.0f && fabsf(dy) <= NEOX_FLAT_TOL * 2.0f;
}

static neox_result flat_cubic(neox_context *ctx, nx_flat *f,
                              float x0, float y0, float c1x, float c1y,
                              float c2x, float c2y, float x1, float y1, int depth)
{
    if ((cubic_is_flat(x0, y0, c1x, c1y, c2x, c2y, x1, y1) && depth > 0) ||
        depth >= NEOX_FLAT_MAX_DEPTH) {
        return flat_put_pt(ctx, f, x1, y1);
    }
    {
        float a01x = (x0 + c1x) * 0.5f,  a01y = (y0 + c1y) * 0.5f;
        float a12x = (c1x + c2x) * 0.5f, a12y = (c1y + c2y) * 0.5f;
        float a23x = (c2x + x1) * 0.5f,  a23y = (c2y + y1) * 0.5f;
        float b012x = (a01x + a12x) * 0.5f, b012y = (a01y + a12y) * 0.5f;
        float b123x = (a12x + a23x) * 0.5f, b123y = (a12y + a23y) * 0.5f;
        float cxx   = (b012x + b123x) * 0.5f, cyy = (b012y + b123y) * 0.5f;
        neox_result r = flat_cubic(ctx, f, x0, y0, a01x, a01y,
                                   b012x, b012y, cxx, cyy, depth + 1);
        if (r != NEOX_OK) return r;
        return flat_cubic(ctx, f, cxx, cyy, b123x, b123y,
                          a23x, a23y, x1, y1, depth + 1);
    }
}

void nx_flat_free(neox_context *ctx, nx_flat *f)
{
    if (f->pts)   nx_free(ctx, f->pts,   (size_t)f->cap_pts * 4, 16);
    if (f->polys) nx_free(ctx, f->polys, (size_t)f->cap_polys * sizeof(nx_poly), 16);
    memset(f, 0, sizeof(*f));
}

neox_result nx_flatten_path(neox_context *ctx, const nx_path_slot *path,
                            neox_affine ctm, nx_flat *out)
{
    uint32_t i, pi = 0;
    float cx = 0.0f, cy = 0.0f;   /* 当前点（§4.1 隐式 MOVE(0,0) 前的起点） */
    float sx = 0.0f, sy = 0.0f;   /* 当前子路径 MOVE 点 */
    int in_poly = 0;
    neox_result r;

    memset(out, 0, sizeof(*out));
    for (i = 0; i < path->verb_count; i++) {
        switch ((neox_path_verb)path->verbs[i]) {
        case NEOX_VERB_MOVE: {
            float x, y;
            nx_affine_apply(ctm, path->points[pi], path->points[pi + 1], &x, &y);
            pi += 2;
            r = flat_begin_poly(ctx, out);
            if (r != NEOX_OK) goto fail;
            r = flat_put_pt(ctx, out, x, y);
            if (r != NEOX_OK) goto fail;
            cx = sx = x; cy = sy = y;
            in_poly = 1;
            break;
        }
        case NEOX_VERB_LINE: {
            float x, y;
            nx_affine_apply(ctm, path->points[pi], path->points[pi + 1], &x, &y);
            pi += 2;
            if (!in_poly) { /* 隐式 MOVE(0,0) */
                r = flat_begin_poly(ctx, out);
                if (r != NEOX_OK) goto fail;
                r = flat_put_pt(ctx, out, 0.0f, 0.0f);
                if (r != NEOX_OK) goto fail;
                cx = 0.0f; cy = 0.0f;
                in_poly = 1;
            }
            r = flat_put_pt(ctx, out, x, y);
            if (r != NEOX_OK) goto fail;
            cx = x; cy = y;
            break;
        }
        case NEOX_VERB_QUAD: {
            float c1x, c1y, x1, y1;
            nx_affine_apply(ctm, path->points[pi], path->points[pi + 1], &c1x, &c1y);
            nx_affine_apply(ctm, path->points[pi + 2], path->points[pi + 3], &x1, &y1);
            pi += 4;
            if (!in_poly) {
                r = flat_begin_poly(ctx, out);
                if (r != NEOX_OK) goto fail;
                r = flat_put_pt(ctx, out, 0.0f, 0.0f);
                if (r != NEOX_OK) goto fail;
                cx = 0.0f; cy = 0.0f;
                in_poly = 1;
            }
            r = flat_quad(ctx, out, cx, cy, c1x, c1y, x1, y1, 0);
            if (r != NEOX_OK) goto fail;
            cx = x1; cy = y1;
            break;
        }
        case NEOX_VERB_CUBIC: {
            float c1x, c1y, c2x, c2y, x1, y1;
            nx_affine_apply(ctm, path->points[pi], path->points[pi + 1], &c1x, &c1y);
            nx_affine_apply(ctm, path->points[pi + 2], path->points[pi + 3], &c2x, &c2y);
            nx_affine_apply(ctm, path->points[pi + 4], path->points[pi + 5], &x1, &y1);
            pi += 6;
            if (!in_poly) {
                r = flat_begin_poly(ctx, out);
                if (r != NEOX_OK) goto fail;
                r = flat_put_pt(ctx, out, 0.0f, 0.0f);
                if (r != NEOX_OK) goto fail;
                cx = 0.0f; cy = 0.0f;
                in_poly = 1;
            }
            r = flat_cubic(ctx, out, cx, cy, c1x, c1y, c2x, c2y, x1, y1, 0);
            if (r != NEOX_OK) goto fail;
            cx = x1; cy = y1;
            break;
        }
        case NEOX_VERB_CLOSE:
            /* 填充语义下轮廓隐式闭合；后续动词从闭合点继续（§4.1） */
            cx = sx; cy = sy;
            in_poly = 0;
            break;
        default:
            goto fail;
        }
    }
    return NEOX_OK;
fail:
    nx_flat_free(ctx, out);
    return NEOX_ERROR_OUT_OF_MEMORY;
}

/* ------------------------------------------------------------------ 扫描线光栅化（§4.2、§4.4）
 * 每个像素行取 NEOX_SS 条子扫描线；子扫描线上求解析水平 coverage。
 * 半开规则 [ymin, ymax)：顶点不会被重复计数；水平边跳过。 */
typedef struct nx_cross { float x; int32_t w; } nx_cross;

static int nx_cross_cmp(const void *pa, const void *pb)
{
    const nx_cross *a = (const nx_cross *)pa;
    const nx_cross *b = (const nx_cross *)pb;
    if (a->x < b->x) return -1;
    if (a->x > b->x) return 1;
    return (a->w > b->w) ? -1 : (a->w < b->w);
}

/* 将覆盖区间 [xa,xb) 的水平 coverage 累加进一行掩码 */
static void nx_span_add(float *row, uint32_t w, float xa, float xb, float scale)
{
    int32_t i0, i1, i;
    if (xb <= xa) return;
    i0 = (int32_t)floorf(xa);
    i1 = (int32_t)floorf(xb);
    for (i = i0; i <= i1; i++) {
        float hi, lo, cov;
        if (i < 0 || i >= (int32_t)w) continue;
        hi = xb < (float)(i + 1) ? xb : (float)(i + 1);
        lo = xa > (float)i ? xa : (float)i;
        cov = hi - lo;
        if (cov > 0.0f) row[i] += cov * scale;
    }
}

neox_result nx_rasterize_path(neox_context *ctx, const nx_path_slot *path,
                              neox_affine ctm, uint32_t w, uint32_t h,
                              float *mask)
{
    nx_flat flat;
    nx_cross *xs = NULL;
    uint32_t total_pts, xi, j, k, p;
    float scale = 1.0f / (float)NEOX_SS;
    neox_result r;

    r = nx_flatten_path(ctx, path, ctm, &flat);
    if (r != NEOX_OK) return r;
    total_pts = flat.npts / 2;
    if (total_pts < 3 || flat.npolys == 0) {
        memset(mask, 0, (size_t)w * h * sizeof(float));
        nx_flat_free(ctx, &flat);
        return NEOX_OK;
    }
    xs = (nx_cross *)nx_alloc(ctx, ((size_t)total_pts + 2) * sizeof(nx_cross), 16);
    if (!xs) { nx_flat_free(ctx, &flat); return NEOX_ERROR_OUT_OF_MEMORY; }

    memset(mask, 0, (size_t)w * h * sizeof(float));
    for (j = 0; j < h; j++) {
        float *row = mask + (size_t)j * w;
        for (k = 0; k < NEOX_SS; k++) {
            float y = (float)j + ((float)k + 0.5f) * scale;
            int wn = 0;
            xi = 0;
            for (p = 0; p < flat.npolys; p++) {
                const nx_poly *pl = &flat.polys[p];
                uint32_t e, n = pl->count;
                const float *pp = &flat.pts[(size_t)pl->start * 2];
                for (e = 0; e < n; e++) {
                    float x0 = pp[e * 2], y0 = pp[e * 2 + 1];
                    uint32_t e2 = (e + 1 == n) ? 0 : e + 1;
                    float x1 = pp[e2 * 2], y1 = pp[e2 * 2 + 1];
                    float ymin, ymax, x;
                    if (y0 == y1) continue;
                    ymin = y0 < y1 ? y0 : y1;
                    ymax = y0 > y1 ? y0 : y1;
                    if (y < ymin || y >= ymax) continue;
                    x = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
                    xs[xi].x = x;
                    xs[xi].w = (y1 > y0) ? 1 : -1; /* +y 向下为正方向 */
                    xi++;
                }
            }
            if (xi < 2) continue;
            qsort(xs, xi, sizeof(nx_cross), nx_cross_cmp);
            for (p = 0; p + 1 < xi; p++) {
                int inside;
                if (path->fill_rule == NEOX_FILL_NONZERO) {
                    wn += xs[p].w;
                    inside = (wn != 0);
                } else {
                    inside = ((p & 1u) == 0);
                }
                if (inside)
                    nx_span_add(row, w, xs[p].x, xs[p + 1].x, scale);
            }
        }
    }
    nx_free(ctx, xs, ((size_t)total_pts + 2) * sizeof(nx_cross), 16);
    nx_flat_free(ctx, &flat);
    return NEOX_OK;
}
