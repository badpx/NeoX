/* Software reference executor: sampling, compositing and target conversion only. */
#include "execution.h"

/* ------------------------------------------------------------------ 混合（规范 §6，预乘线性 float）
 * co = ca·Fa + cb·Fb（ca/cb 为预乘色），αo = αa·fa + αb·fb；表中各模式 Fa=fa、Fb=fb。
 * PLUS 逐通道 clamp 到 [0,1]。 */
static void nx_blend_pixel(float d[4], const float s[4], neox_blend mode)
{
    float sa = s[3], da = d[3];
    float fa, fb;
    switch (mode) {
    case NEOX_BLEND_CLEAR:
        d[0] = d[1] = d[2] = d[3] = 0.0f;
        return;
    case NEOX_BLEND_SRC:
        d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        return;
    case NEOX_BLEND_DST:
        return;
    case NEOX_BLEND_SRC_OVER:  fa = 1.0f;      fb = 1.0f - sa; break;
    case NEOX_BLEND_DST_OVER:  fa = 1.0f - da; fb = 1.0f;      break;
    case NEOX_BLEND_SRC_IN:    fa = da;        fb = 0.0f;      break;
    case NEOX_BLEND_DST_IN:    fa = 0.0f;      fb = sa;        break;
    case NEOX_BLEND_SRC_OUT:   fa = 1.0f - da; fb = 0.0f;      break;
    case NEOX_BLEND_DST_OUT:   fa = 0.0f;      fb = 1.0f - sa; break;
    case NEOX_BLEND_SRC_ATOP:  fa = da;        fb = 1.0f - sa; break;
    case NEOX_BLEND_DST_ATOP:  fa = 1.0f - da; fb = sa;        break;
    case NEOX_BLEND_XOR:       fa = 1.0f - da; fb = 1.0f - sa; break;
    case NEOX_BLEND_PLUS: {
        int i;
        for (i = 0; i < 4; i++) {
            float v = s[i] + d[i];
            d[i] = v > 1.0f ? 1.0f : v;
        }
        return;
    }
    default: return;
    }
    d[0] = s[0] * fa + d[0] * fb;
    d[1] = s[1] * fa + d[1] * fb;
    d[2] = s[2] * fa + d[2] * fb;
    d[3] = sa  * fa + da  * fb;
}

/* 覆盖率插值混合（规范 §6.1：out = cov·blend(src,dst) + (1−cov)·dst）。
 *
 * 实现与 GL 后端固定功能因子表逐项对齐运算序列：fragment 输出
 * o = (src·cov, oa)，混合 co = o·Fa + d·Fb——oa 按模式编码使
 * Fb = 1−oa 展开后恰好给出 cov·Fb₀ + (1−cov)（Fb₀ 为 §6 表中原因子）。
 * 全覆盖（cov=1）时退化为 §6 原 Porter-Duff 公式。PLUS 的 clamp 非线性
 * 无法表达为因子，按插值式直接计算（blend 内 clamp 后再插值）。
 *
 *   mode      | Fa              | Fb                  | oa
 *   SRC_OVER  | ONE             | ONE_MINUS_SRC_ALPHA | sa·cov
 *   CLEAR     | ZERO            | ONE_MINUS_SRC_ALPHA | cov
 *   SRC       | ONE             | ONE_MINUS_SRC_ALPHA | cov
 *   DST       | ZERO            | ONE                 | （无关）
 *   DST_OVER  | ONE_MINUS_DST_A | ONE                 | （无关）
 *   SRC_IN    | DST_ALPHA       | ONE_MINUS_SRC_ALPHA | cov
 *   DST_IN    | ZERO            | ONE_MINUS_SRC_ALPHA | cov·(1−sa)
 *   SRC_OUT   | ONE_MINUS_DST_A | ONE_MINUS_SRC_ALPHA | cov
 *   DST_OUT   | ZERO            | ONE_MINUS_SRC_ALPHA | sa·cov
 *   SRC_ATOP  | DST_ALPHA       | ONE_MINUS_SRC_ALPHA | sa·cov
 *   DST_ATOP  | ONE_MINUS_DST_A | ONE_MINUS_SRC_ALPHA | cov·(1−sa)
 *   XOR       | ONE_MINUS_DST_A | ONE_MINUS_SRC_ALPHA | sa·cov
 */
static void nx_blend_pixel_cov(float d[4], const float s[4], neox_blend mode,
                               float cov)
{
    float sa = s[3];
    float oa, fa, fb;
    int i;
    if (mode == NEOX_BLEND_PLUS) {
        float ic = 1.0f - cov;
        for (i = 0; i < 4; i++) {
            float t = s[i] + d[i];
            if (t > 1.0f) t = 1.0f;
            d[i] = cov * t + ic * d[i];
        }
        return;
    }
    /* oa/o1a 与 gl.c 的 dual-source 因子表逐项对齐：alpha 通道在
     * SRC/SRC_IN/SRC_OUT/DST_ATOP 的 dst 因子位置需要与 rgb 不同的编码，
     * GL 侧用 GL_SRC1_ALPHA 双源混合表达（o1a），此处直接取值。 */
    switch (mode) {
    case NEOX_BLEND_SRC_OVER:  oa = sa * cov;          fa = 1.0f;        fb = 1.0f - oa; break;
    case NEOX_BLEND_CLEAR:     oa = cov;               fa = 0.0f;        fb = 1.0f - oa; break;
    case NEOX_BLEND_SRC:       oa = sa * cov;          fa = 1.0f;        fb = 1.0f - cov; break;
    case NEOX_BLEND_DST:       oa = sa * cov;          fa = 0.0f;        fb = 1.0f;      break;
    case NEOX_BLEND_DST_OVER:  oa = sa * cov;          fa = 1.0f - d[3]; fb = 1.0f;      break;
    case NEOX_BLEND_SRC_IN:    oa = sa * cov;          fa = d[3];        fb = 1.0f - cov; break;
    case NEOX_BLEND_DST_IN:    oa = cov * (1.0f - sa); fa = 0.0f;        fb = 1.0f - oa; break;
    case NEOX_BLEND_SRC_OUT:   oa = sa * cov;          fa = 1.0f - d[3]; fb = 1.0f - cov; break;
    case NEOX_BLEND_DST_OUT:   oa = sa * cov;          fa = 0.0f;        fb = 1.0f - oa; break;
    case NEOX_BLEND_SRC_ATOP:  oa = sa * cov;          fa = d[3];        fb = 1.0f - oa; break;
    case NEOX_BLEND_DST_ATOP:  oa = sa * cov;          fa = 1.0f - d[3];
                               fb = 1.0f - cov * (1.0f - sa);                     break;
    case NEOX_BLEND_XOR:       oa = sa * cov;          fa = 1.0f - d[3]; fb = 1.0f - oa; break;
    default: return;
    }
    d[0] = (s[0] * cov) * fa + d[0] * fb;
    d[1] = (s[1] * cov) * fa + d[1] * fb;
    d[2] = (s[2] * cov) * fa + d[2] * fb;
    d[3] = oa * fa + d[3] * fb;
}

/* ------------------------------------------------------------------ 纹理采样（§8：NEAREST/LINEAR，CLAMP）
 * 采样先把紧凑纹理解码为预乘线性值；A8 采样值为 alpha（§5）。 */
static void sample_nearest(const nx_tex_slot *t,float u,float v,float out[4])
{
    int i=(int)floorf(fminf(fmaxf(u,0),t->width-1));
    int j=(int)floorf(fminf(fmaxf(v,0),t->height-1));
    nx_texel_load(t,t->data,(size_t)j*t->width+i,out);
}
static void sample_linear(const nx_tex_slot *t,float u,float v,float out[4])
{
    float x=fminf(fmaxf(u-.5f,0),t->width-1), y=fminf(fmaxf(v-.5f,0),t->height-1);
    int i0=(int)floorf(x),j0=(int)floorf(y),i1=nx_clampi(i0+1,0,t->width-1),j1=nx_clampi(j0+1,0,t->height-1);
    float fx=x-i0,fy=y-j0,p00[4],p10[4],p01[4],p11[4];
    nx_texel_load(t,t->data,(size_t)j0*t->width+i0,p00);nx_texel_load(t,t->data,(size_t)j0*t->width+i1,p10);
    nx_texel_load(t,t->data,(size_t)j1*t->width+i0,p01);nx_texel_load(t,t->data,(size_t)j1*t->width+i1,p11);
    float w00=(1-fx)*(1-fy),w10=fx*(1-fy),w01=(1-fx)*fy,w11=fx*fy;
    for(int c=0;c<4;c++)out[c]=p00[c]*w00+p10[c]*w10+p01[c]*w01+p11[c]*w11;
}

static void sample_tex(const nx_tex_slot *t, float u, float v,
                       neox_filter filter, float out[4])
{
    if (filter == NEOX_FILTER_NEAREST) sample_nearest(t, u, v, out);
    else sample_linear(t, u, v, out);
}

/* ------------------------------------------------------------------ 设备 */


static neox_result soft_device_init(neox_device *dev, const void *platform,
                                    size_t plat_size)
{
    /* SOFT 无参数块：platform 与 plat_size 必须配对（NULL ⇔ 0） */
    if ((platform == NULL) != (plat_size == 0)) {
        dev->ctx->last_error =
            "platform/plat_size mismatch (platform is NULL iff plat_size is 0)";
        return NEOX_ERROR_INVALID_ARGUMENT;
    }
    (void)platform;
    (void)plat_size;
    return nx_device_init_tables(dev);
}

/* ------------------------------------------------------------------ 执行引擎（pump 驱动）

   执行期维护层栈：基底 accum 为 layers[0]，每个 owns_layer 的 PUSH 在执行期
   分配隔离层；POP 时按组不透明度/混合/裁剪掩码整体合成回父层（§7）。
   任何执行期分配失败 → submission FAILED，目标保持调用前状态。 */
typedef struct ex_state {
    neox_affine ctm;
    float       opacity;
    neox_blend  blend;
    const float *clip;
    int32_t     layer_idx;
} ex_state;

static void ex_composite(float *dst, const float *src, const float *clip,
                         float opacity, neox_blend blend, size_t npix)
{
    size_t i;
    int c;
    for (i = 0; i < npix; i++) {
        float s[4];
        const float *sp = &src[i * 4];
        float *dp = &dst[i * 4];
        if (clip) {
            float m = clip[i];
            for (c = 0; c < 4; c++) s[c] = sp[c] * opacity * m;
        } else {
            for (c = 0; c < 4; c++) s[c] = sp[c] * opacity;
        }
        nx_blend_pixel(dp, s, blend);
    }
}

static neox_result ex_draw_path(neox_device *dev, nx_sub_slot *sub,
                                const nx_cmd *cmd, float *layer,
                                const ex_state *top, float *mask)
{
    neox_context *ctx = dev->ctx;
    const nx_path_slot *ps = &((nx_path_slot *)dev->paths.slots)[cmd->res_idx];
    float base[4];
    size_t npix = (size_t)sub->width * sub->height, i;
    neox_result r = nx_rasterize_path(ctx, ps, top->ctm, sub->width, sub->height,
                                      mask);
    if (r != NEOX_OK) return r;
    nx_paint_to_premul_linear(cmd->color, base);
    for (i = 0; i < npix; i++) {
        float cov = mask[i];
        if (cov == 0.0f) continue;
        nx_blend_pixel_cov(&layer[i * 4], base, top->blend, cov);
    }
    return NEOX_OK;
}

static neox_result ex_draw_tex(neox_device *dev, nx_sub_slot *sub,
                               const nx_cmd *cmd, float *layer,
                               const ex_state *top)
{
    nx_tex_slot snapshot = ((nx_tex_slot *)dev->textures.slots)[cmd->res_idx];
    snapshot.data=cmd->texture;
    const nx_tex_slot *tx=&snapshot;
    neox_affine inv;
    float xc[4], yc[4], minx, miny, maxx, maxy;
    int32_t x0, y0, x1, y1, px, py;
    size_t row;
    if (cmd->dst.w == 0.0f || cmd->dst.h == 0.0f) return NEOX_OK;
    /* 退化变换（§3）：逆不存在 → 不产生任何输出 */
    int inverse_status = nx_affine_invert(top->ctm, &inv);
    if (!inverse_status) return NEOX_OK;
    if (inverse_status < 0)
        NX_ERR(dev->ctx, "inverse transform exceeds numeric range", NEOX_ERROR_INVALID_ARGUMENT);
    neox_rect sample_dst;
    if (!nx_sample_transform(inv,cmd->dst,sub->width,sub->height,&inv,&sample_dst))
        NX_ERR(dev->ctx, "sampling transform exceeds float range", NEOX_ERROR_UNSUPPORTED);
    if (!nx_affine_quad(top->ctm, cmd->dst, xc, yc))
        NX_ERR(dev->ctx, "transformed texture coordinates exceed numeric range", NEOX_ERROR_INVALID_ARGUMENT);
    minx = maxx = xc[0]; miny = maxy = yc[0];
    for (px = 1; px < 4; px++) {
        if (xc[px] < minx) minx = xc[px];
        if (xc[px] > maxx) maxx = xc[px];
        if (yc[px] < miny) miny = yc[px];
        if (yc[px] > maxy) maxy = yc[px];
    }
    x0 = nx_pixel_floor(minx, sub->width);
    x1 = nx_pixel_ceil(maxx, sub->width);
    y0 = nx_pixel_floor(miny, sub->height);
    y1 = nx_pixel_ceil(maxy, sub->height);
    for (py = y0; py < y1; py++) {
        row = (size_t)py * sub->width;
        for (px = x0; px < x1; px++) {
            float lx, ly, tu, tv, u, v, smp[4], s[4];
            int c;
            nx_affine_apply(inv, (float)px + 0.5f, (float)py + 0.5f, &lx, &ly);
            tu = (lx - sample_dst.x) / sample_dst.w;
            tv = (ly - sample_dst.y) / sample_dst.h;
            if (!(tu >= 0.0f && tu <= 1.0f && tv >= 0.0f && tv <= 1.0f)) continue;
            /* 源矩形不随 CTM 变换；负宽高目标矩形自然产生镜像（§8） */
            u = cmd->src.x + tu * cmd->src.w;
            v = cmd->src.y + tv * cmd->src.h;
            sample_tex(tx, u, v, cmd->filter, smp);
            for (c = 0; c < 4; c++) s[c] = smp[c] * cmd->tex_opacity;
            nx_blend_pixel(&layer[(row + (size_t)px) * 4], s, top->blend);
        }
    }
    return NEOX_OK;
}

static neox_result ex_draw_glyphs(neox_device *dev, nx_sub_slot *sub,
                                  const nx_cmd *cmd, float *layer,
                                  const ex_state *top)
{
    nx_tex_slot snapshot = ((nx_tex_slot *)dev->textures.slots)[cmd->res_idx];
    snapshot.data=cmd->texture;
    const nx_tex_slot *at=&snapshot;
    neox_affine inv;
    float base[4];
    uint32_t g;
    float s_fac = 0.0f;
    if (cmd->atlas_mode == NEOX_ATLAS_SDF || cmd->atlas_mode == NEOX_ATLAS_MSDF)
        s_fac = nx_glyph_scale_factor(top->ctm);  /* §8.2：奇异值平均 */
    int inverse_status = nx_affine_invert(top->ctm, &inv);
    if (!inverse_status) return NEOX_OK;
    if (inverse_status < 0)
        NX_ERR(dev->ctx, "inverse transform exceeds numeric range", NEOX_ERROR_INVALID_ARGUMENT);
    if ((cmd->atlas_mode == NEOX_ATLAS_SDF || cmd->atlas_mode == NEOX_ATLAS_MSDF) &&
        s_fac <= 0.0f)
        return NEOX_OK;
    nx_paint_to_premul_linear(cmd->color, base);
    for (g = 0; g < cmd->glyph_count; g++) {
        const neox_glyph *gl = &cmd->glyphs[g];
        float gw = (float)gl->sw, gh = (float)gl->sh;
        float xc[4], yc[4], minx, miny, maxx, maxy;
        int32_t x0, y0, x1, y1, px, py;
        if (gw == 0.0f || gh == 0.0f) continue;
        neox_affine sampling;neox_rect local;
        if (!nx_sample_transform(inv,(neox_rect){gl->x,gl->y,gw,gh},sub->width,sub->height,&sampling,&local))
            NX_ERR(dev->ctx, "sampling transform exceeds float range", NEOX_ERROR_UNSUPPORTED);
        float texel_x=gw/local.w, texel_y=gh/local.h;
        if (!nx_affine_quad(top->ctm, (neox_rect){gl->x,gl->y,gw,gh}, xc, yc))
            NX_ERR(dev->ctx, "transformed glyph coordinates exceed numeric range", NEOX_ERROR_INVALID_ARGUMENT);
        minx = maxx = xc[0]; miny = maxy = yc[0];
        for (px = 1; px < 4; px++) {
            if (xc[px] < minx) minx = xc[px];
            if (xc[px] > maxx) maxx = xc[px];
            if (yc[px] < miny) miny = yc[px];
            if (yc[px] > maxy) maxy = yc[px];
        }
        x0 = nx_pixel_floor(minx, sub->width);
        x1 = nx_pixel_ceil(maxx, sub->width);
        y0 = nx_pixel_floor(miny, sub->height);
        y1 = nx_pixel_ceil(maxy, sub->height);
        for (py = y0; py < y1; py++) {
            for (px = x0; px < x1; px++) {
                float lx, ly, au, av, smp[4];
                float cov;
                nx_affine_apply(sampling, (float)px + 0.5f, (float)py + 0.5f, &lx, &ly);
                if (!(lx >= local.x && lx <= local.x + local.w && ly >= local.y && ly <= local.y + local.h))
                    continue;
                /* A8 图集：双线性采样遮罩，颜色来自 paint（§5、§8） */
                au = (float)gl->sx + (lx - local.x) * texel_x;
                av = (float)gl->sy + (ly - local.y) * texel_y;
                sample_linear(at, au, av, smp);
                if (cmd->atlas_mode == NEOX_ATLAS_SDF) {
                    /* A8 距离场：槽内 .a = v/255，§8.2 重建 */
                    cov = nx_sdf_reconstruct(smp[3], cmd->dist_range,
                                             cmd->softness, s_fac);
                } else if (cmd->atlas_mode == NEOX_ATLAS_MSDF) {
                    /* RGBA8_DATA 三通道：median 重建（§8.2 中位数规则） */
                    cov = nx_sdf_reconstruct(nx_median3(smp[0], smp[1], smp[2]),
                                             cmd->dist_range, cmd->softness,
                                             s_fac);
                } else {
                    cov = smp[3];
                }
                if (cov <= 0.0f) continue;
                nx_blend_pixel_cov(&layer[((size_t)py * sub->width + (size_t)px) * 4],
                                   base, top->blend, cov);
            }
        }
    }
    return NEOX_OK;
}

static neox_result soft_execute(neox_device *dev, nx_sub_slot *sub)
{
    neox_context *ctx = dev->ctx;
    nx_execution_clips execution_clips = {0};
    size_t npix = (size_t)sub->width * sub->height;
    float **lbufs = NULL;
    uint32_t nlayers = 0, caplayers = 0;
    ex_state *stk = NULL;
    uint32_t nstk = 0, capstk = 0;
    float *mask = NULL;
    uint32_t i;
    neox_result r = NEOX_ERROR_OUT_OF_MEMORY;

    sub->report.algorithm = NEOX_ALG_SCANLINE;

#define EX_FAIL(msg) do { ctx->last_error = (msg); r = NEOX_ERROR_OUT_OF_MEMORY; goto cleanup; } while (0)

    {
        float **nb = (float **)nx_alloc(ctx, sizeof(float *) * 8, 16);
        ex_state *ns = (ex_state *)nx_alloc(ctx, sizeof(ex_state) * 8, 16);
        if (!nb || !ns) {
            if (nb) nx_free(ctx, nb, sizeof(float *) * 8, 16);
            if (ns) nx_free(ctx, ns, sizeof(ex_state) * 8, 16);
            ctx->last_error = "execute stack allocation failed";
            return NEOX_ERROR_OUT_OF_MEMORY;
        }
        lbufs = nb; caplayers = 8;
        stk = ns; capstk = 8;
    }
    lbufs[0] = sub->accum;
    nlayers = 1;
    stk[0].ctm = nx_affine_identity();
    stk[0].opacity = 1.0f;
    stk[0].blend = NEOX_BLEND_SRC_OVER;
    stk[0].clip = NULL;
    stk[0].layer_idx = 0;
    nstk = 1;

    for (i = 0; i < sub->cmd_count; i++) {
        const nx_cmd *c = &sub->cmds[i];
        ex_state *top = &stk[nstk - 1];
        switch (c->op) {
        case NXC_PUSH: {
            ex_state *nst;
            float **nb;
            if (c->owns_layer) {
                float *lb = (float *)nx_alloc(ctx, npix * 4 * sizeof(float), 16);
                if (!lb) EX_FAIL("group layer allocation failed");
                memset(lb, 0, npix * 4 * sizeof(float));
                nb = (float **)nx_array_grow(ctx, lbufs, sizeof(float *),
                                             &caplayers, nlayers + 1);
                if (!nb) {
                    nx_free(ctx, lb, npix * 4 * sizeof(float), 16);
                    EX_FAIL("layer stack allocation failed");
                }
                lbufs = nb;
                lbufs[nlayers] = lb;
                nlayers++;
            }
            nst = (ex_state *)nx_array_grow(ctx, stk, sizeof(ex_state),
                                            &capstk, nstk + 1);
            if (!nst) EX_FAIL("execute state stack allocation failed");
            stk = nst;
            stk[nstk].ctm = c->ctm;
            stk[nstk].opacity = c->opacity;
            stk[nstk].blend = c->blend;
            r = nx_execution_clip_push(dev, &execution_clips, c, sub->width, sub->height, &stk[nstk].clip);
            if (r != NEOX_OK) goto cleanup;
            stk[nstk].layer_idx = c->owns_layer ? (int32_t)(nlayers - 1)
                                                : stk[nstk - 1].layer_idx;
            nstk++;
            break;
        }
        case NXC_POP: {
            ex_state *top2 = &stk[nstk - 1];
            ex_state *parent = &stk[nstk - 2];

            if (top2->layer_idx != parent->layer_idx) {
                ex_composite(lbufs[parent->layer_idx],
                             lbufs[top2->layer_idx], top2->clip,
                             top2->opacity, top2->blend, npix);
                nx_free(ctx, lbufs[top2->layer_idx], npix * 4 * sizeof(float), 16);
                nlayers--;
            }
            if (top2->clip) nx_execution_clip_pop(ctx, &execution_clips);
            nstk--;
            break;
        }
        case NXC_PATH: {
            float *layer = lbufs[top->layer_idx];
            if (!mask) {
                mask = (float *)nx_alloc(ctx, npix * sizeof(float), 16);
                if (!mask) EX_FAIL("raster mask allocation failed");
            }
            r = ex_draw_path(dev, sub, c, layer, top, mask);
            if (r != NEOX_OK) goto cleanup;
            break;
        }
        case NXC_TEX:
            r = ex_draw_tex(dev, sub, c, lbufs[top->layer_idx], top);
            if (r != NEOX_OK) goto cleanup;
            break;
        case NXC_GLYPHS:
            r = ex_draw_glyphs(dev, sub, c, lbufs[top->layer_idx], top);
            if (r != NEOX_OK) goto cleanup;
            break;
        default:
            ctx->last_error = "unknown command op";
            r = NEOX_ERROR_BACKEND;
            goto cleanup;
        }
    }

    /* 成功路径的最后一步才触碰目标（§10.1 原子性）；按行写回以支持 stride */
    {
        uint32_t y, x;
        for (y = 0; y < sub->height; y++) {
            uint8_t *rowp = sub->target + (size_t)y * sub->target_stride;
            for (x = 0; x < sub->width; x++) {
                uint8_t out[4];
                nx_premul_linear_to_srgb8(&lbufs[0][((size_t)y * sub->width + x) * 4],
                                          out);
                memcpy(rowp + (size_t)x * 4, out, 4);
            }
        }
    }
    r = NEOX_OK;

cleanup:
    nx_execution_clips_destroy(ctx, &execution_clips);
    if (r != NEOX_OK && nlayers > 0) {
        /* 失败：释放执行期分配的组层（基底 accum 由 sub_free_payload 处理） */
        uint32_t k;
        for (k = 1; k < nlayers; k++)
            nx_free(ctx, lbufs[k], npix * 4 * sizeof(float), 16);
    }
    if (mask) nx_free(ctx, mask, npix * sizeof(float), 16);
    if (stk) nx_free(ctx, stk, (size_t)capstk * sizeof(ex_state), 16);
    if (lbufs) nx_free(ctx, lbufs, (size_t)caplayers * sizeof(float *), 16);
    return r;
#undef EX_FAIL
}

/* ------------------------------------------------------------------ pump 与 submission 查询 */

/* ------------------------------------------------------------------ Soft 后端 vtable */

/* begin_target：校验 SOFT 目标、确定帧尺寸、装载基底（清屏色或既有目标内容） */
static neox_result soft_begin_target(neox_frame *fr,
                                     const neox_target_desc *target)
{
    neox_context *ctx = fr->dev->ctx;
    size_t npix, bytes, span, i;
    if (!target->soft.pixels || !nx_image_span(target->soft.width, target->soft.height,
                                              4, target->soft.stride, &span))
        NX_ERR(ctx, "invalid or overflowing soft target span", NEOX_ERROR_INVALID_ARGUMENT);
    if (target->soft.width > INT32_MAX || target->soft.height > INT32_MAX)
        NX_ERR(ctx, "soft target exceeds raster coordinate range", NEOX_ERROR_UNSUPPORTED);
    if (!nx_size_mul(target->soft.width, target->soft.height, &npix) ||
        !nx_size_mul(npix, 4 * sizeof(float), &bytes))
        NX_ERR(ctx, "soft target storage size overflow", NEOX_ERROR_INVALID_ARGUMENT);
    fr->target = target->soft.pixels;
    fr->target_stride = target->soft.stride;
    fr->width = target->soft.width;
    fr->height = target->soft.height;
    fr->base = (float *)nx_alloc(ctx, bytes, 16);
    if (!fr->base) NX_ERR(ctx, "frame buffer allocation failed",
                          NEOX_ERROR_OUT_OF_MEMORY);
    if (target->clear) {
        float p[4];
        nx_paint_to_premul_linear(*target->clear, p);
        for (i = 0; i < npix; i++)
            memcpy(&fr->base[i * 4], p, sizeof(p));
    } else {
        nx_pixels_to_premul_linear(target->soft.pixels, target->soft.stride,
                                   fr->width, fr->height, fr->base);
    }
    return NEOX_OK;
}

static void soft_get_caps(neox_device *dev, neox_caps *out_caps)
{
    (void)dev;
    /* Soft 为参照实现：全部 Phase 1 语义可全精度执行（NEOX_ALLOW_APPROXIMATE 永不触发） */
    out_caps->features = NEOX_CAP_COMPUTE | NEOX_CAP_FLOAT_BLEND;
    out_caps->max_texture_size = NX_MAX_TEXTURE_SIZE;
    out_caps->backend_memory_limit = 0;
}

const nx_backend *nx_backend_soft(void)
{
    static const nx_backend vt = {
        "soft",
        soft_device_init,
        NULL,               /* shutdown：无后端状态 */
        soft_begin_target,
        soft_execute,
        soft_get_caps,
        NULL,               /* poll：同步后端 */
        NULL,               /* block：同步后端 */
        NULL                /* no native texture storage */
    };
    return &vt;
}
