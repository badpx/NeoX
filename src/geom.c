/* ===========================================================================
 * geom.c — Soft / GL / Metal 共用的曲线细分与覆盖率光栅化。
 * 路径和裁剪都经同一 CPU 几何/覆盖率实现；GPU 上传其掩码再执行合成。
 * 规范：docs/semantics-v0.md §3、§4、§9、§10。
 * =========================================================================*/
#include "internal.h"

/* ------------------------------------------------------------------ 路径 flatten（§4）
 * 曲线在设备空间 flatten，目标误差 NEOX_FLAT_TOL 像素、最深 12 层；
 * 达到深度上限仍不能满足弦段距离检查时返回 UNSUPPORTED，不静默降精度。
 * 输出：点列表 + 轮廓表（轮廓隐式闭合，填充语义）。 */

static neox_result flat_put_pt(neox_context *ctx, nx_flat *f, float x, float y)
{
    if (!isfinite(x) || !isfinite(y)) return NEOX_ERROR_INVALID_ARGUMENT;
    if (f->npts > UINT32_MAX - 2) return NEOX_ERROR_OUT_OF_MEMORY;
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
    if (f->npolys == UINT32_MAX) return NEOX_ERROR_OUT_OF_MEMORY;
    nx_poly *np = (nx_poly *)nx_array_grow(ctx, f->polys, sizeof(nx_poly),
                                           &f->cap_polys, f->npolys + 1);
    if (!np) return NEOX_ERROR_OUT_OF_MEMORY;
    f->polys = np;
    f->polys[f->npolys].start = f->npts / 2;
    f->polys[f->npolys].count = 0;
    f->npolys++;
    return NEOX_OK;
}

/* A Bezier lies in its control hull. If every control (including the exact
 * endpoints) is within tolerance of the emitted float chord, the whole curve
 * is within that convex capsule. Checking each control avoids cancellation at
 * inflections; clamping the projection also preserves collinear overshoots.
 * Subdivision uses double so finite float controls cannot overflow midpoints. */
static int flat_near_chord(double x, double y, float ax, float ay, float bx, float by)
{
    double dx = (double)bx - ax, dy = (double)by - ay;
    double length2 = dx*dx + dy*dy;
    double t = length2 ? ((x-ax)*dx + (y-ay)*dy) / length2 : 0.0;
    t = fmax(0.0, fmin(1.0, t));
    double ex = x - ((double)ax + t*dx), ey = y - ((double)ay + t*dy);
    return ex*ex + ey*ey <= (double)NEOX_FLAT_TOL * NEOX_FLAT_TOL;
}

static neox_result flat_curve(neox_context *ctx, nx_flat *f,
                              const double *points, unsigned degree, unsigned depth)
{
    float ax = f->pts[f->npts-2], ay = f->pts[f->npts-1];
    float bx = (float)points[degree*2], by = (float)points[degree*2+1];
    int flat = 1;
    for (unsigned i = 0; i <= degree; i++)
        if (!flat_near_chord(points[i*2], points[i*2+1], ax, ay, bx, by)) flat = 0;
    if (flat) return flat_put_pt(ctx, f, bx, by);
    if (depth >= NEOX_FLAT_MAX_DEPTH) {
        ctx->last_error = "curve flattening exceeds precision or subdivision limit";
        return NEOX_ERROR_UNSUPPORTED;
    }
    /* de Casteljau at t=1/2. Arrays are bounded for quadratic/cubic curves. */
    double tmp[8], left[8], right[8];
    memcpy(tmp, points, (degree+1)*2*sizeof(double));
    left[0] = tmp[0]; left[1] = tmp[1];
    right[degree*2] = tmp[degree*2]; right[degree*2+1] = tmp[degree*2+1];
    for (unsigned level = 1; level <= degree; level++) {
        for (unsigned i = 0; i <= degree-level; i++) {
            tmp[i*2] = (tmp[i*2] + tmp[i*2+2]) * 0.5;
            tmp[i*2+1] = (tmp[i*2+1] + tmp[i*2+3]) * 0.5;
        }
        left[level*2] = tmp[0]; left[level*2+1] = tmp[1];
        right[(degree-level)*2] = tmp[(degree-level)*2];
        right[(degree-level)*2+1] = tmp[(degree-level)*2+1];
    }
    neox_result r = flat_curve(ctx, f, left, degree, depth+1);
    if (r != NEOX_OK) return r;
    return flat_curve(ctx, f, right, degree, depth+1);
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
    uint32_t i;
    size_t pi = 0; /* scalar offset can exceed UINT32_MAX for a valid point count */
    float cx = ctm.tx, cy = ctm.ty; /* transformed implicit MOVE(0,0) */
    float sx = cx, sy = cy;
    int in_poly = 0;
    neox_result r;

    memset(out, 0, sizeof(*out));
    if (!nx_affine_is_finite(ctm)) return NEOX_ERROR_INVALID_ARGUMENT;
    /* Singular transforms have no filled area, including curved paths. */
    if ((double)ctm.a*ctm.d - (double)ctm.b*ctm.c == 0.0) return NEOX_OK;
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
            if (!in_poly) { /* implicit origin, or previous CLOSE's MOVE point */
                r = flat_begin_poly(ctx, out);
                if (r != NEOX_OK) goto fail;
                r = flat_put_pt(ctx, out, cx, cy);
                if (r != NEOX_OK) goto fail;
                sx = cx; sy = cy;
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
                r = flat_put_pt(ctx, out, cx, cy);
                if (r != NEOX_OK) goto fail;
                sx = cx; sy = cy;
                in_poly = 1;
            }
            if (!isfinite(c1x) || !isfinite(c1y) || !isfinite(x1) || !isfinite(y1)) {
                r = NEOX_ERROR_INVALID_ARGUMENT; goto fail;
            }
            double curve[] = {cx,cy,c1x,c1y,x1,y1};
            r = flat_curve(ctx, out, curve, 2, 0);
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
                r = flat_put_pt(ctx, out, cx, cy);
                if (r != NEOX_OK) goto fail;
                sx = cx; sy = cy;
                in_poly = 1;
            }
            if (!isfinite(c1x) || !isfinite(c1y) || !isfinite(c2x) || !isfinite(c2y) ||
                !isfinite(x1) || !isfinite(y1)) {
                r = NEOX_ERROR_INVALID_ARGUMENT; goto fail;
            }
            double curve[] = {cx,cy,c1x,c1y,c2x,c2y,x1,y1};
            r = flat_curve(ctx, out, curve, 3, 0);
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
            r = NEOX_ERROR_INVALID_ARGUMENT;
            goto fail;
        }
    }
    return NEOX_OK;
fail:
    nx_flat_free(ctx, out);
    return r;
}

/* ------------------------------------------------------------------ 扫描线光栅化（§4.2、§4.4）
 * 每个像素行取 NEOX_SS 条子扫描线；子扫描线上求解析水平 coverage。
 * 半开规则 [ymin, ymax)：顶点不会被重复计数；水平边跳过。 */
typedef struct nx_cross { double x; int32_t w; uint32_t path; } nx_cross;

static int nx_cross_cmp(const void *pa, const void *pb)
{
    const nx_cross *a = (const nx_cross *)pa;
    const nx_cross *b = (const nx_cross *)pb;
    if (a->x < b->x) return -1;
    if (a->x > b->x) return 1;
    return (a->w > b->w) ? -1 : (a->w < b->w);
}

/* 将覆盖区间 [xa,xb) 的水平 coverage 累加进一行掩码 */
static void nx_span_add(float *row, uint32_t w, double xa, double xb, float scale)
{
    xa = fmax(xa, 0.0); xb = fmin(xb, (double)w);
    if (xb <= xa) return;
    /* Both endpoints are clamped before conversion. The exclusive end may be
     * INT32_MAX; no signed loop increment or float-rounded viewport overflow. */
    uint32_t begin = (uint32_t)floor(xa), end = (uint32_t)ceil(xb);
    for (uint32_t i = begin; i < end; i++) {
        double cov = fmin(xb, (double)i+1.0) - fmax(xa, (double)i);
        if (cov > 0.0) row[i] += (float)cov * scale;
    }
}

neox_result nx_rasterize_path(neox_context *ctx, const nx_path_slot *path,
                              neox_affine ctm, uint32_t w, uint32_t h,
                              float *mask)
{
    nx_clip_path input = {path, ctm};
    return nx_rasterize_intersection(ctx, &input, 1, w, h, mask);
}

neox_result nx_rasterize_intersection(neox_context *ctx, const nx_clip_path *paths,
    uint32_t count, uint32_t w, uint32_t h, float *mask)
{
    nx_flat *flats = NULL;
    nx_cross *xs = NULL;
    int32_t *winding = NULL;
    size_t total = 0, xi, flat_bytes, winding_bytes, mask_bytes, pixels;
    neox_result result = NEOX_ERROR_OUT_OF_MEMORY;
    const float scale = 1.0f / (float)NEOX_SS;
    if (!count || !paths || !mask || !w || !h || w > INT32_MAX || h > INT32_MAX ||
        !nx_size_mul(count, sizeof(*flats), &flat_bytes) ||
        !nx_size_mul(count, sizeof(*winding), &winding_bytes) ||
        !nx_size_mul(w, h, &pixels) || !nx_size_mul(pixels, sizeof(float), &mask_bytes))
        return NEOX_ERROR_INVALID_ARGUMENT;
    flats = nx_alloc(ctx, flat_bytes, 16);
    if (!flats) return result;
    memset(flats, 0, flat_bytes);
    for (uint32_t q = 0; q < count; q++) {
        result = nx_flatten_path(ctx, paths[q].path, paths[q].ctm, &flats[q]);
        if (result != NEOX_OK) goto done;
        if (flats[q].npts / 2 > SIZE_MAX / sizeof(*xs) - total) {
            result = NEOX_ERROR_OUT_OF_MEMORY; goto done;
        }
        total += flats[q].npts / 2;
    }
    xs = nx_alloc(ctx, total * sizeof(*xs), 16);
    winding = nx_alloc(ctx, winding_bytes, 16);
    if (!xs || !winding) { result = NEOX_ERROR_OUT_OF_MEMORY; goto done; }
    memset(mask, 0, mask_bytes);
    for (uint32_t j = 0; j < h; j++) {
        float *row = mask + (size_t)j * w;
        for (uint32_t k = 0; k < NEOX_SS; k++) {
            double y = (double)j + ((double)k + 0.5) * scale;
            uint32_t inside_count = 0;
            xi = 0;
            memset(winding, 0, winding_bytes);
            for (uint32_t q = 0; q < count; q++) {
                const nx_flat *flat = &flats[q];
                for (uint32_t p = 0; p < flat->npolys; p++) {
                    const nx_poly *pl = &flat->polys[p];
                    uint32_t n = pl->count;
                    const float *pp = &flat->pts[(size_t)pl->start * 2];
                    for (uint32_t e = 0; e < n; e++) {
                        uint32_t e2 = (e + 1 == n) ? 0 : e + 1;
                        float x0 = pp[e * 2], y0 = pp[e * 2 + 1];
                        float x1 = pp[e2 * 2], y1 = pp[e2 * 2 + 1];
                        if (y0 == y1 || y < fminf(y0,y1) || y >= fmaxf(y0,y1)) continue;
                        /* Double intermediates keep finite float endpoints from overflowing. */
                        xs[xi].x = (double)x0 + (y-y0) * ((double)x1-x0) / ((double)y1-y0);
                        xs[xi].w = y1 > y0 ? 1 : -1;
                        xs[xi++].path = q;
                    }
                }
            }
            qsort(xs, xi, sizeof(*xs), nx_cross_cmp);
            for (size_t p = 0; p + 1 < xi; p++) {
                uint32_t q = xs[p].path;
                int was = winding[q] != 0;
                if (paths[q].path->fill_rule == NEOX_FILL_NONZERO)
                    winding[q] += xs[p].w;
                else winding[q] ^= 1;
                int now = winding[q] != 0;
                if (now && !was) inside_count++;
                if (was && !now) inside_count--;
                if (inside_count == count)
                    nx_span_add(row,w,xs[p].x,xs[p+1].x,scale);
            }
        }
    }
    result = NEOX_OK;
done:
    if (xs) nx_free(ctx,xs,total*sizeof(*xs),16);
    if (winding) nx_free(ctx,winding,winding_bytes,16);
    for (uint32_t q = 0; q < count; q++) nx_flat_free(ctx,&flats[q]);
    nx_free(ctx,flats,flat_bytes,16);
    return result;
}
