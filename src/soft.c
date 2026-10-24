/* ===========================================================================
 * soft.c — NeoX Soft 后端：软件光栅参照实现
 *
 * 本文件同时承载后端中立的公共 API 实现（纹理/路径/帧/场景/CAP/submission），
 * Soft 后端差异部分收口在文件尾的 nx_backend_soft vtable：
 * 设备初始化、基底装载（begin_target）、执行引擎（execute）、能力查询。
 * 跨后端共享的几何代码（flatten、裁剪掩码光栅化）在 geom.c。
 *
 * 规范引用（docs/semantics-v0.md）：
 *   §2 坐标系（左上原点，像素中心 (i+0.5, j+0.5)）
 *   §3 变换（仿射左乘复合；退化变换合法且无输出）
 *   §4 路径（flatten 后按扫描线填充；NONZERO/EVENODD 差异是语义的一部分）
 *   §4.4 覆盖率：水平方向解析精确，垂直方向 NEOX_SS=8 子扫描线逼近（§9 容差内）
 *   §5 颜色：上传转预乘线性；混合在线性光下进行；输出端转回 sRGB8
 *   §6 Porter-Duff 13 模式公式
 *   §7 组不透明度 = 隔离合成（临时层），裁剪为祖先交集且抗锯齿
 *   §8 纹理采样（NEAREST/LINEAR，CLAMP；负宽高目标矩形产生镜像）
 *   §9 确定性：同后端同输入逐位一致（无时间/随机依赖）
 *   §10.1 失败原子性；§10.2-1 立即式与保留式一致
 *
 * 设计注释：
 *   - 帧命令只录制不执行（异步模型）；neox_device_pump 才真正执行。
 *     目标缓冲在执行成功前不被写入，因此 pump 期失败天然保持原子性。
 *   - 组/裁剪层在执行期分配，分配失败 → submission FAILED，目标不变。
 *   - 裁剪掩码在 push_state 期急切光栅化：失败在调用点传播，不留半完成状态。
 * =========================================================================*/
#include "internal.h"

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
 * 纹理内部为预乘线性 float；A8 采样值为 alpha（颜色来自 paint，§5）。 */
static const float *tex_px(const nx_tex_slot *t, int i, int j)
{
    return &t->px[((size_t)j * t->width + (size_t)i) * 4];
}

static void sample_nearest(const nx_tex_slot *t, float u, float v, float out[4])
{
    int i = nx_clampi((int32_t)floorf(u), 0, (int)t->width - 1);
    int j = nx_clampi((int32_t)floorf(v), 0, (int)t->height - 1);
    const float *p = tex_px(t, i, j);
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
}

static void sample_linear(const nx_tex_slot *t, float u, float v, float out[4])
{
    float x = u - 0.5f, y = v - 0.5f;
    int i0 = (int32_t)floorf(x), j0 = (int32_t)floorf(y);
    float fx = x - (float)i0, fy = y - (float)j0;
    int i1 = i0 + 1, j1 = j0 + 1;
    const float *p00, *p10, *p01, *p11;
    float w00, w10, w01, w11;
    int c;
    i0 = nx_clampi(i0, 0, (int)t->width - 1);
    i1 = nx_clampi(i1, 0, (int)t->width - 1);
    j0 = nx_clampi(j0, 0, (int)t->height - 1);
    j1 = nx_clampi(j1, 0, (int)t->height - 1);
    p00 = tex_px(t, i0, j0); p10 = tex_px(t, i1, j0);
    p01 = tex_px(t, i0, j1); p11 = tex_px(t, i1, j1);
    w00 = (1.0f - fx) * (1.0f - fy);
    w10 = fx * (1.0f - fy);
    w01 = (1.0f - fx) * fy;
    w11 = fx * fy;
    for (c = 0; c < 4; c++)
        out[c] = p00[c] * w00 + p10[c] * w10 + p01[c] * w01 + p11[c] * w11;
}

static void sample_tex(const nx_tex_slot *t, float u, float v,
                       neox_filter filter, float out[4])
{
    if (filter == NEOX_FILTER_NEAREST) sample_nearest(t, u, v, out);
    else sample_linear(t, u, v, out);
}

/* ------------------------------------------------------------------ 设备 */

#define NEOX_SOFT_MAX_TEX 16384u

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

static void frame_destroy(neox_device *dev, neox_frame *fr);

void neox_device_destroy(neox_device *dev)
{
    neox_context *ctx;
    neox_device **link;
    uint32_t i;
    if (!dev) return;
    ctx = dev->ctx;

    if (dev->active_frame) {
        frame_destroy(dev, dev->active_frame);
        dev->active_frame = NULL;
    }
    /* 释放全部槽位内容 */
    for (i = 0; i < dev->textures.count; i++) {
        nx_tex_slot *s = &((nx_tex_slot *)dev->textures.slots)[i];
        if (s->in_use && s->px)
            nx_free(ctx, s->px, (size_t)s->width * s->height * 4 * sizeof(float), 16);
    }
    for (i = 0; i < dev->paths.count; i++) {
        nx_path_slot *s = &((nx_path_slot *)dev->paths.slots)[i];
        if (s->in_use) {
            if (s->verbs)  nx_free(ctx, s->verbs, s->verb_count, 4);
            if (s->points) nx_free(ctx, s->points, (size_t)s->point_count * 8, 16);
        }
    }
    for (i = 0; i < dev->nodes.count; i++) {
        nx_node_slot *s = &((nx_node_slot *)dev->nodes.slots)[i];
        if (s->in_use && s->children)
            nx_free(ctx, s->children, (size_t)s->child_cap * 4, 16);
        if (s->in_use && s->kind == NEOX_NODE_GLYPHS && s->u.glyphs.glyphs)
            nx_free(ctx, s->u.glyphs.glyphs, (size_t)s->u.glyphs.count * sizeof(neox_glyph), 16);
    }
    for (i = 0; i < dev->subs.count; i++) {
        nx_sub_slot *s = &((nx_sub_slot *)dev->subs.slots)[i];
        if (s->in_use && s->cmds)
            nx_free(ctx, s->cmds, (size_t)s->cmd_cap * sizeof(nx_cmd), 16);
        if (s->in_use && s->accum)
            nx_free(ctx, s->accum, (size_t)s->width * s->height * 4 * sizeof(float), 16);
        if (s->in_use && s->refs)
            nx_free(ctx, s->refs, (size_t)s->ref_cap * 4, 16);
    }
    nx_table_destroy(ctx, &dev->textures);
    nx_table_destroy(ctx, &dev->paths);
    nx_table_destroy(ctx, &dev->scenes);
    nx_table_destroy(ctx, &dev->nodes);
    nx_table_destroy(ctx, &dev->subs);
    if (dev->vt && dev->vt->shutdown)
        dev->vt->shutdown(dev); /* 后端状态（GL 对象、上下文）随设备销毁 */
    if (dev->defer) nx_free(ctx, dev->defer, (size_t)dev->defer_cap * 4, 16);
    if (dev->pending) nx_free(ctx, dev->pending, (size_t)dev->pending_cap * 4, 16);
    /* 从 context 链表摘除 */
    link = &ctx->devices;
    while (*link && *link != dev) link = &(*link)->next;
    if (*link) *link = dev->next;
    nx_free(ctx, dev, sizeof(neox_device), 16);
}

void neox_device_get_caps(neox_device *dev, neox_caps *out_caps)
{
    if (!out_caps) return;
    memset(out_caps, 0, sizeof(*out_caps));
    if (!dev || !dev->vt) return;
    dev->vt->get_caps(dev, out_caps);
}

int neox_device_is_lost(neox_device *dev)
{
    (void)dev;
    return 0; /* Soft 后端不会丢失（无真异步硬件） */
}

/* ------------------------------------------------------------------ 纹理 */

static neox_result tex_check_desc(const neox_texture_desc *d)
{
    uint32_t bpp;
    if (!d || !d->pixels || d->width == 0 || d->height == 0)
        return NEOX_ERROR_INVALID_ARGUMENT;
    if (d->format != NEOX_FORMAT_RGBA8_SRGB &&
        d->format != NEOX_FORMAT_RGBA8_LINEAR &&
        d->format != NEOX_FORMAT_A8)
        return NEOX_ERROR_INVALID_ARGUMENT;
    if (d->width > NEOX_SOFT_MAX_TEX || d->height > NEOX_SOFT_MAX_TEX)
        return NEOX_ERROR_UNSUPPORTED;
    bpp = (d->format == NEOX_FORMAT_A8) ? 1u : 4u;
    if (d->row_stride != 0 && d->row_stride < (size_t)d->width * bpp)
        return NEOX_ERROR_INVALID_ARGUMENT;
    return NEOX_OK;
}

/* 字节像素 → 内部预乘线性 float（§5） */
static void tex_convert_upload(nx_tex_slot *s, const neox_texture_desc *d)
{
    uint32_t x, y;
    size_t stride = d->row_stride ? d->row_stride : (size_t)d->width * (d->format == NEOX_FORMAT_A8 ? 1 : 4);
    for (y = 0; y < s->height; y++) {
        const uint8_t *row = (const uint8_t *)d->pixels + y * stride;
        for (x = 0; x < s->width; x++) {
            float *dst = &s->px[((size_t)y * s->width + x) * 4];
            if (s->format == NEOX_FORMAT_A8) {
                float a = row[x] / 255.0f; /* A8 无颜色概念，采样值即 alpha（§5） */
                dst[0] = a; dst[1] = a; dst[2] = a; dst[3] = a;
            } else {
                const uint8_t *sp = row + x * 4;
                float r, g, b, a = sp[3] / 255.0f;
                if (s->format == NEOX_FORMAT_RGBA8_SRGB) {
                    r = nx_srgb_to_linear(sp[0] / 255.0f);
                    g = nx_srgb_to_linear(sp[1] / 255.0f);
                    b = nx_srgb_to_linear(sp[2] / 255.0f);
                } else {
                    r = sp[0] / 255.0f;
                    g = sp[1] / 255.0f;
                    b = sp[2] / 255.0f;
                }
                dst[0] = r * a; dst[1] = g * a; dst[2] = b * a; dst[3] = a;
            }
        }
    }
}

/* 内部构造：直接以预乘线性 float 建纹理（CAP 回放用，保证逐位一致） */
static neox_result soft_tex_from_linear(neox_device *dev, uint32_t w, uint32_t h,
                                        neox_pixel_format fmt, const float *px,
                                        neox_texture *out)
{
    neox_context *ctx = dev->ctx;
    nx_tex_slot *s;
    uint32_t idx;
    size_t nbytes = (size_t)w * h * 4 * sizeof(float);
    float *copy = (float *)nx_alloc(ctx, nbytes, 16);
    if (!copy) return NEOX_ERROR_OUT_OF_MEMORY;
    memcpy(copy, px, nbytes);
    idx = nx_table_alloc(ctx, &dev->textures);
    if (idx == UINT32_MAX) {
        nx_free(ctx, copy, nbytes, 16);
        return NEOX_ERROR_OUT_OF_MEMORY;
    }
    s = &((nx_tex_slot *)dev->textures.slots)[idx];
    memset(s, 0, sizeof(*s));
    s->gen = 1;
    s->in_use = 1;
    s->width = w; s->height = h; s->format = fmt;
    s->version = 1;
    s->px = copy;
    *out = (neox_texture){ nx_make_handle(dev->id, NX_TYPE_TEXTURE, idx, s->gen) };
    return NEOX_OK;
}

neox_result neox_texture_create(neox_device *dev, const neox_texture_desc *desc,
                                neox_texture *out_tex)
{
    neox_context *ctx;
    nx_tex_slot *s;
    neox_result r;
    uint32_t idx;
    size_t nbytes;
    if (!dev) return NEOX_ERROR_INVALID_ARGUMENT;
    ctx = dev->ctx;
    if (!out_tex) NX_ERR(ctx, "out_tex is NULL", NEOX_ERROR_INVALID_ARGUMENT);
    *out_tex = (neox_texture){ 0 };
    r = tex_check_desc(desc);
    if (r != NEOX_OK) {
        ctx->last_error = (r == NEOX_ERROR_UNSUPPORTED)
            ? "texture size exceeds backend limit" : "invalid texture desc";
        return r;
    }
    /* 先分配像素（大对象），失败即无任何副作用 */
    nbytes = (size_t)desc->width * desc->height * 4 * sizeof(float);
    {
        float *px = (float *)nx_alloc(ctx, nbytes, 16);
        nx_tex_slot tmp;
        if (!px) NX_ERR(ctx, "texture pixel allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
        memset(&tmp, 0, sizeof(tmp));
        tmp.width = desc->width; tmp.height = desc->height;
        tmp.format = desc->format;
        tmp.px = px;
        tex_convert_upload(&tmp, desc);
        idx = nx_table_alloc(ctx, &dev->textures);
        if (idx == UINT32_MAX) {
            nx_free(ctx, px, nbytes, 16);
            NX_ERR(ctx, "texture slot allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
        }
        s = &((nx_tex_slot *)dev->textures.slots)[idx];
        memset(s, 0, sizeof(*s));
        s->gen = 1;
        s->in_use = 1;
        s->width = desc->width; s->height = desc->height;
        s->format = desc->format;
        s->version = 1;
        s->px = px;
    }
    *out_tex = (neox_texture){ nx_make_handle(dev->id, NX_TYPE_TEXTURE, idx, 1) };
    return NEOX_OK;
}

neox_result neox_texture_update(neox_device *dev, neox_texture tex,
                                neox_rect region, const void *pixels,
                                size_t row_stride)
{
    neox_context *ctx;
    nx_tex_slot *s;
    uint32_t idx, x, y;
    uint32_t bpp;
    neox_result r;
    if (!dev) return NEOX_ERROR_INVALID_ARGUMENT;
    ctx = dev->ctx;
    if (!pixels) NX_ERR(ctx, "pixels is NULL", NEOX_ERROR_INVALID_ARGUMENT);
    r = nx_resolve(dev, tex.id, NX_TYPE_TEXTURE, &idx);
    if (r != NEOX_OK) return r;
    s = &((nx_tex_slot *)dev->textures.slots)[idx];
    if (region.x < 0 || region.y < 0 || region.w < 0 || region.h < 0 ||
        region.x + region.w > (float)s->width ||
        region.y + region.h > (float)s->height)
        NX_ERR(ctx, "update region out of bounds", NEOX_ERROR_INVALID_ARGUMENT);
    bpp = (s->format == NEOX_FORMAT_A8) ? 1u : 4u;
    if (row_stride != 0 && row_stride < (size_t)(uint32_t)region.w * bpp)
        NX_ERR(ctx, "row_stride too small", NEOX_ERROR_INVALID_ARGUMENT);
    if (row_stride == 0) row_stride = (size_t)(uint32_t)region.w * bpp;
    for (y = 0; y < (uint32_t)region.h; y++) {
        const uint8_t *row = (const uint8_t *)pixels + (size_t)y * row_stride;
        for (x = 0; x < (uint32_t)region.w; x++) {
            float *dst = &s->px[(((size_t)(uint32_t)region.y + y) * s->width +
                                 (uint32_t)region.x + x) * 4];
            if (s->format == NEOX_FORMAT_A8) {
                float a = row[x] / 255.0f;
                dst[0] = a; dst[1] = a; dst[2] = a; dst[3] = a;
            } else {
                const uint8_t *sp = row + x * 4;
                float aa = sp[3] / 255.0f, rr, gg, bb;
                if (s->format == NEOX_FORMAT_RGBA8_SRGB) {
                    rr = nx_srgb_to_linear(sp[0] / 255.0f);
                    gg = nx_srgb_to_linear(sp[1] / 255.0f);
                    bb = nx_srgb_to_linear(sp[2] / 255.0f);
                } else {
                    rr = sp[0] / 255.0f; gg = sp[1] / 255.0f; bb = sp[2] / 255.0f;
                }
                dst[0] = rr * aa; dst[1] = gg * aa; dst[2] = bb * aa; dst[3] = aa;
            }
        }
    }
    s->version++; /* GL 后端惰性上传失效标记 */
    return NEOX_OK;
}

void neox_texture_release(neox_device *dev, neox_texture tex)
{
    neox_context *ctx;
    nx_tex_slot *s;
    uint32_t idx;
    neox_result r;
    if (!dev) return;
    ctx = dev->ctx;
    r = nx_resolve(dev, tex.id, NX_TYPE_TEXTURE, &idx);
    if (r != NEOX_OK) return; /* 句柄已失效：静默忽略（void 契约） */
    s = &((nx_tex_slot *)dev->textures.slots)[idx];
    s->gen++;               /* 立即失效旧句柄 */
    if (s->gen == 0) s->gen = 1;
    s->deferred = 1;
    {
        uint32_t *nq = (uint32_t *)nx_array_grow(ctx, dev->defer, 4,
                                                 &dev->defer_cap,
                                                 dev->defer_count + 1);
        if (!nq) {
            /* 队列扩容失败：不丢失语义——槽位保持存活至设备销毁 */
            ctx->last_error = "deferred free queue full";
            return;
        }
        dev->defer = nq;
        dev->defer[dev->defer_count++] = (NX_TYPE_TEXTURE << 24) | idx;
    }
}

/* ------------------------------------------------------------------ 路径 */

static uint32_t verb_points(neox_path_verb v)
{
    switch (v) {
    case NEOX_VERB_MOVE:  return 1;
    case NEOX_VERB_LINE:  return 1;
    case NEOX_VERB_QUAD:  return 2;
    case NEOX_VERB_CUBIC: return 3;
    case NEOX_VERB_CLOSE: return 0;
    }
    return 0;
}

/* 内部构造：直接以路径数据拷贝建路径（CAP 回放用） */
static neox_result soft_path_from_copy(neox_device *dev, const uint8_t *verbs,
                                       uint32_t verb_count, const float *points,
                                       uint32_t point_count,
                                       neox_fill_rule rule, neox_path *out)
{
    neox_context *ctx = dev->ctx;
    nx_path_slot *s;
    uint8_t *vb;
    float *pt;
    uint32_t idx;
    vb = (uint8_t *)nx_alloc(ctx, verb_count ? verb_count : 1, 4);
    if (!vb) return NEOX_ERROR_OUT_OF_MEMORY;
    pt = (float *)nx_alloc(ctx, ((size_t)point_count * 8) ? (size_t)point_count * 8 : 4, 16);
    if (!pt) {
        nx_free(ctx, vb, verb_count ? verb_count : 1, 4);
        return NEOX_ERROR_OUT_OF_MEMORY;
    }
    memcpy(vb, verbs, verb_count);
    memcpy(pt, points, (size_t)point_count * 8);
    idx = nx_table_alloc(ctx, &dev->paths);
    if (idx == UINT32_MAX) {
        nx_free(ctx, vb, verb_count ? verb_count : 1, 4);
        nx_free(ctx, pt, (size_t)point_count * 8, 16);
        return NEOX_ERROR_OUT_OF_MEMORY;
    }
    s = &((nx_path_slot *)dev->paths.slots)[idx];
    memset(s, 0, sizeof(*s));
    s->gen = 1; s->in_use = 1;
    s->verbs = vb; s->points = pt;
    s->verb_count = verb_count; s->point_count = point_count;
    s->fill_rule = rule;
    *out = (neox_path){ nx_make_handle(dev->id, NX_TYPE_PATH, idx, 1) };
    return NEOX_OK;
}

neox_result neox_path_create(neox_device *dev, const neox_path_desc *desc,
                             neox_path *out_path)
{
    neox_context *ctx;
    uint32_t i, need = 0;
    if (!dev) return NEOX_ERROR_INVALID_ARGUMENT;
    ctx = dev->ctx;
    if (!out_path) NX_ERR(ctx, "out_path is NULL", NEOX_ERROR_INVALID_ARGUMENT);
    *out_path = (neox_path){ 0 };
    if (!desc || (!desc->verbs && desc->verb_count) ||
        (!desc->points && desc->point_count))
        NX_ERR(ctx, "invalid path desc", NEOX_ERROR_INVALID_ARGUMENT);
    for (i = 0; i < desc->verb_count; i++) {
        neox_path_verb v = (neox_path_verb)desc->verbs[i];
        if (v < NEOX_VERB_MOVE || v > NEOX_VERB_CLOSE)
            NX_ERR(ctx, "unknown path verb", NEOX_ERROR_INVALID_ARGUMENT);
        need += verb_points(v);
    }
    if (need != desc->point_count)
        NX_ERR(ctx, "point_count does not match verbs", NEOX_ERROR_INVALID_ARGUMENT);
    return soft_path_from_copy(dev, desc->verbs, desc->verb_count,
                               desc->points, desc->point_count,
                               desc->fill_rule, out_path);
}

void neox_path_release(neox_device *dev, neox_path path)
{
    neox_context *ctx;
    nx_path_slot *s;
    uint32_t idx;
    if (!dev) return;
    ctx = dev->ctx;
    if (nx_resolve(dev, path.id, NX_TYPE_PATH, &idx) != NEOX_OK) return;
    s = &((nx_path_slot *)dev->paths.slots)[idx];
    s->gen++;
    if (s->gen == 0) s->gen = 1;
    s->deferred = 1;
    {
        uint32_t *nq = (uint32_t *)nx_array_grow(ctx, dev->defer, 4,
                                                 &dev->defer_cap,
                                                 dev->defer_count + 1);
        if (!nq) {
            ctx->last_error = "deferred free queue full";
            return;
        }
        dev->defer = nq;
        dev->defer[dev->defer_count++] = (NX_TYPE_PATH << 24) | idx;
    }
}

/* ------------------------------------------------------------------ CAP 捕获（opaque 结构，定义见文件尾序列化部分） */
typedef struct neox_capture neox_capture;

/* ------------------------------------------------------------------ 命令发射 */

static neox_result cap_record_cmd(neox_capture *cap, const nx_cmd *c);

/* 追加命令到帧列表；CAP 激活时同步录制 */
static neox_result frame_append_cmd(neox_frame *fr, const nx_cmd *c)
{
    neox_device *dev = fr->dev;
    neox_context *ctx = dev->ctx;
    nx_cmd *nc = (nx_cmd *)nx_array_grow(ctx, fr->cmds, sizeof(nx_cmd),
                                         &fr->cmd_cap, fr->cmd_count + 1);
    if (!nc) NX_ERR(ctx, "command list allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    fr->cmds = nc;
    fr->cmds[fr->cmd_count] = *c;
    if (dev->cap) {
        /* CAP 录制必须在计数自增前取已写入的命令槽 */
        neox_result cr = cap_record_cmd(dev->cap, &fr->cmds[fr->cmd_count]);
        if (cr != NEOX_OK) {
            ctx->last_error = "capture recording failed";
            return cr;
        }
    }
    fr->cmd_count++;
    if (c->op == NXC_PATH || c->op == NXC_TEX || c->op == NXC_GLYPHS)
        fr->draw_calls++;
    return NEOX_OK;
}

/* 释放命令列表内部拥有的内存（glyphs 拷贝、clip 掩码） */
static void frame_free_cmd_payloads(neox_context *ctx, nx_cmd *cmds, uint32_t n,
                                    uint32_t w, uint32_t h)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (cmds[i].op == NXC_GLYPHS && cmds[i].glyphs)
            nx_free(ctx, cmds[i].glyphs,
                    (size_t)cmds[i].glyph_count * sizeof(neox_glyph), 16);
        if (cmds[i].op == NXC_PUSH && cmds[i].clip_mask)
            nx_free(ctx, cmds[i].clip_mask, (size_t)w * h * 4 * sizeof(float), 16);
    }
}

/* PUSH：owns_layer 的判定与帧期/执行期必须一致（组不透明度或裁剪 → 隔离层，§7） */
static int push_needs_layer(float opacity, int32_t clip_idx)
{
    return opacity < 1.0f || clip_idx >= 0;
}

static neox_result emit_push(neox_frame *fr, neox_affine ctm, float opacity,
                             neox_blend blend, uint32_t flags,
                             int32_t clip_idx, uint64_t clip_id)
{
    neox_device *dev = fr->dev;
    neox_context *ctx = dev->ctx;
    nx_cmd c;
    memset(&c, 0, sizeof(c));
    c.op = NXC_PUSH;
    c.ctm = ctm;
    c.opacity = opacity;
    c.blend = blend;
    c.flags = flags;
    c.clip_idx = clip_idx;
    c.clip_id = clip_id;
    c.owns_layer = push_needs_layer(opacity, clip_idx);
    if (clip_idx >= 0) {
        /* 裁剪掩码急切光栅化：失败在调用点传播，不留半完成状态（§10.1）。
         * 掩码按 push 时的复合 CTM 计算，祖先交集在逐层合成时自然实现（§7）。 */
        const nx_path_slot *ps = &((nx_path_slot *)dev->paths.slots)[clip_idx];
        c.clip_mask = (float *)nx_alloc(ctx, (size_t)fr->width * fr->height * 4 *
                                        sizeof(float), 16);
        if (!c.clip_mask)
            NX_ERR(ctx, "clip mask allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
        {
            neox_result r = nx_rasterize_path(ctx, ps, ctm, fr->width, fr->height,
                                           c.clip_mask);
            if (r != NEOX_OK) {
                nx_free(ctx, c.clip_mask,
                        (size_t)fr->width * fr->height * 4 * sizeof(float), 16);
                c.clip_mask = NULL;
                return r;
            }
        }
    }
    return frame_append_cmd(fr, &c);
}

static neox_result emit_pop(neox_frame *fr)
{
    nx_cmd c;
    memset(&c, 0, sizeof(c));
    c.op = NXC_POP;
    return frame_append_cmd(fr, &c);
}

static neox_result emit_path(neox_frame *fr, int32_t path_idx, uint64_t path_id,
                             neox_color color)
{
    nx_cmd c;
    memset(&c, 0, sizeof(c));
    c.op = NXC_PATH;
    c.res_idx = path_idx;
    c.res_id = path_id;
    c.color = color;
    return frame_append_cmd(fr, &c);
}

static neox_result emit_tex(neox_frame *fr, int32_t tex_idx, uint64_t tex_id,
                            neox_rect src, neox_rect dst, neox_filter filter,
                            float opacity)
{
    nx_cmd c;
    memset(&c, 0, sizeof(c));
    c.op = NXC_TEX;
    c.res_idx = tex_idx;
    c.res_id = tex_id;
    c.src = src;
    c.dst = dst;
    c.filter = filter;
    c.tex_opacity = opacity;
    return frame_append_cmd(fr, &c);
}

/* glyphs 深拷贝进命令（借用语义 → 帧拥有快照） */
static neox_result emit_glyphs(neox_frame *fr, int32_t atlas_idx, uint64_t atlas_id,
                               const neox_glyph *glyphs, uint32_t count,
                               neox_color color)
{
    neox_device *dev = fr->dev;
    neox_context *ctx = dev->ctx;
    nx_cmd c;
    neox_glyph *copy;
    memset(&c, 0, sizeof(c));
    copy = (neox_glyph *)nx_alloc(ctx, (size_t)count * sizeof(neox_glyph), 16);
    if (!copy) NX_ERR(ctx, "glyph array allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    memcpy(copy, glyphs, (size_t)count * sizeof(neox_glyph));
    c.op = NXC_GLYPHS;
    c.res_idx = atlas_idx;
    c.res_id = atlas_id;
    c.glyphs = copy;
    c.glyph_count = count;
    c.color = color;
    {
        neox_result r = frame_append_cmd(fr, &c);
        if (r != NEOX_OK) {
            nx_free(ctx, copy, (size_t)count * sizeof(neox_glyph), 16);
            return r;
        }
    }
    return NEOX_OK;
}

/* ------------------------------------------------------------------ 帧（立即模式） */

static void frame_destroy(neox_device *dev, neox_frame *fr)
{
    neox_context *ctx = dev->ctx;
    if (!fr) return;
    frame_free_cmd_payloads(ctx, fr->cmds, fr->cmd_count, fr->width, fr->height);
    if (fr->cmds) nx_free(ctx, fr->cmds, (size_t)fr->cmd_cap * sizeof(nx_cmd), 16);
    if (fr->ctm_stack) nx_free(ctx, fr->ctm_stack, (size_t)fr->ctm_cap * sizeof(neox_affine), 16);
    if (fr->base)
        nx_free(ctx, fr->base, (size_t)fr->width * fr->height * 4 * sizeof(float), 16);
    nx_free(ctx, fr, sizeof(neox_frame), 16);
}

static int color_is_bad(neox_color c)
{
    return nx_float_bad(c.r) || nx_float_bad(c.g) ||
           nx_float_bad(c.b) || nx_float_bad(c.a);
}

neox_result neox_frame_begin(neox_device *dev, const neox_target_desc *target,
                             neox_frame **out_frame)
{
    neox_context *ctx;
    neox_frame *fr;
    neox_result r;
    if (!dev) return NEOX_ERROR_INVALID_ARGUMENT;
    ctx = dev->ctx;
    if (!target || !out_frame)
        NX_ERR(ctx, "target/out_frame is NULL", NEOX_ERROR_INVALID_ARGUMENT);
    *out_frame = NULL;
    if (target->kind != dev->kind)
        NX_ERR(ctx, "target backend kind mismatch", NEOX_ERROR_INVALID_ARGUMENT);
    if (target->clear && color_is_bad(*target->clear))
        NX_ERR(ctx, "clear color contains NaN/Inf", NEOX_ERROR_INVALID_ARGUMENT);
    if (dev->active_frame)
        NX_ERR(ctx, "a frame is already active on this device",
               NEOX_ERROR_INVALID_ARGUMENT);
    fr = (neox_frame *)nx_alloc(ctx, sizeof(neox_frame), 16);
    if (!fr) NX_ERR(ctx, "frame allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    memset(fr, 0, sizeof(*fr));
    fr->dev = dev;
    fr->surface = target->surface;
    /* 目标校验、帧尺寸确定与基底装载由后端完成（§10.1：执行成功前目标不被写入；
     * GL 后端在此快照表面内容，基底统一为预乘线性 float） */
    r = dev->vt->begin_target(fr, target);
    if (r != NEOX_OK) {
        nx_free(ctx, fr, sizeof(neox_frame), 16);
        return r;
    }
    /* 基底 CTM（恒等） */
    {
        neox_affine *st = (neox_affine *)nx_alloc(ctx, sizeof(neox_affine) * 8, 16);
        if (!st) {
            frame_destroy(dev, fr);
            NX_ERR(ctx, "frame stack allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
        }
        fr->ctm_stack = st;
        fr->ctm_cap = 8;
        fr->ctm_stack[0] = nx_affine_identity();
        fr->ctm_count = 1;
    }
    dev->active_frame = fr;
    *out_frame = fr;
    return NEOX_OK;
}

neox_result neox_push_state(neox_frame *frame, const neox_state *state)
{
    neox_context *ctx;
    neox_affine ctm;
    int32_t clip_idx = -1;
    uint64_t clip_id = 0;
    neox_result r;
    if (!frame) return NEOX_ERROR_INVALID_ARGUMENT;
    ctx = frame->dev->ctx;
    if (!state)
        NX_ERR(ctx, "state is NULL", NEOX_ERROR_INVALID_ARGUMENT);
    if (nx_float_bad(state->opacity) || state->opacity < 0.0f || state->opacity > 1.0f)
        NX_ERR(ctx, "opacity out of range or NaN", NEOX_ERROR_INVALID_ARGUMENT);
    if (state->blend > NEOX_BLEND_PLUS)
        NX_ERR(ctx, "invalid blend mode", NEOX_ERROR_INVALID_ARGUMENT);
    {
        const float *m = &state->transform.a;
        uint32_t i;
        for (i = 0; i < 6; i++)
            if (nx_float_bad(m[i]))
                NX_ERR(ctx, "transform contains NaN/Inf", NEOX_ERROR_INVALID_ARGUMENT);
    }
    if (state->clip && state->clip->id != 0) {
        uint32_t idx;
        r = nx_resolve(frame->dev, state->clip->id, NX_TYPE_PATH, &idx);
        if (r != NEOX_OK) return r;
        clip_idx = (int32_t)idx;
        clip_id = state->clip->id;
    }
    ctm = nx_affine_mul(frame->ctm_stack[frame->ctm_count - 1], state->transform);
    {
        neox_affine *nst = (neox_affine *)nx_array_grow(ctx, frame->ctm_stack,
                            sizeof(neox_affine), &frame->ctm_cap,
                            frame->ctm_count + 1);
        if (!nst) NX_ERR(ctx, "state stack allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
        frame->ctm_stack = nst;
        frame->ctm_stack[frame->ctm_count++] = ctm;
    }
    r = emit_push(frame, ctm, state->opacity, state->blend, state->flags,
                  clip_idx, clip_id);
    if (r != NEOX_OK) {
        frame->ctm_count--; /* 不留半完成状态（§10.1） */
        return r;
    }
    return NEOX_OK;
}

neox_result neox_pop_state(neox_frame *frame)
{
    neox_context *ctx;
    if (!frame) return NEOX_ERROR_INVALID_ARGUMENT;
    ctx = frame->dev->ctx;
    if (frame->ctm_count <= 1)
        NX_ERR(ctx, "state stack underflow", NEOX_ERROR_INVALID_ARGUMENT);
    frame->ctm_count--;
    return emit_pop(frame);
}

neox_result neox_draw_path(neox_frame *frame, neox_path path,
                           const neox_paint *paint)
{
    neox_context *ctx;
    uint32_t idx;
    neox_result r;
    if (!frame) return NEOX_ERROR_INVALID_ARGUMENT;
    ctx = frame->dev->ctx;
    if (!paint)
        NX_ERR(ctx, "paint is NULL", NEOX_ERROR_INVALID_ARGUMENT);
    if (color_is_bad(paint->color))
        NX_ERR(ctx, "paint color contains NaN/Inf", NEOX_ERROR_INVALID_ARGUMENT);
    r = nx_resolve(frame->dev, path.id, NX_TYPE_PATH, &idx);
    if (r != NEOX_OK) return r;
    return emit_path(frame, (int32_t)idx, path.id, paint->color);
}

neox_result neox_draw_texture(neox_frame *frame, neox_texture tex,
                              neox_rect src, neox_rect dst,
                              neox_filter filter, float opacity)
{
    neox_context *ctx;
    uint32_t idx;
    neox_result r;
    if (!frame) return NEOX_ERROR_INVALID_ARGUMENT;
    ctx = frame->dev->ctx;
    if (filter != NEOX_FILTER_NEAREST && filter != NEOX_FILTER_LINEAR)
        NX_ERR(ctx, "invalid filter", NEOX_ERROR_INVALID_ARGUMENT);
    if (nx_float_bad(opacity) || opacity < 0.0f || opacity > 1.0f)
        NX_ERR(ctx, "opacity out of range or NaN", NEOX_ERROR_INVALID_ARGUMENT);
    {
        const float *f = &src.x;
        const float *g2 = &dst.x;
        uint32_t i;
        for (i = 0; i < 4; i++) {
            if (nx_float_bad(f[i]) || nx_float_bad(g2[i]))
                NX_ERR(ctx, "rect contains NaN/Inf", NEOX_ERROR_INVALID_ARGUMENT);
        }
    }
    r = nx_resolve(frame->dev, tex.id, NX_TYPE_TEXTURE, &idx);
    if (r != NEOX_OK) return r;
    return emit_tex(frame, (int32_t)idx, tex.id, src, dst, filter, opacity);
}

neox_result neox_draw_glyphs(neox_frame *frame, neox_texture atlas,
                             const neox_glyph *glyphs, uint32_t count,
                             const neox_paint *paint)
{
    neox_context *ctx;
    uint32_t idx;
    neox_result r;
    if (!frame) return NEOX_ERROR_INVALID_ARGUMENT;
    ctx = frame->dev->ctx;
    if (!paint || (!glyphs && count))
        NX_ERR(ctx, "paint/glyphs is NULL", NEOX_ERROR_INVALID_ARGUMENT);
    if (color_is_bad(paint->color))
        NX_ERR(ctx, "paint color contains NaN/Inf", NEOX_ERROR_INVALID_ARGUMENT);
    r = nx_resolve(frame->dev, atlas.id, NX_TYPE_TEXTURE, &idx);
    if (r != NEOX_OK) return r;
    if (count == 0) return NEOX_OK;
    return emit_glyphs(frame, (int32_t)idx, atlas.id, glyphs, count, paint->color);
}

/* ------------------------------------------------------------------ submission 组装与执行 */

static void sub_free_payload(neox_device *dev, nx_sub_slot *s)
{
    neox_context *ctx = dev->ctx;
    uint32_t i;
    frame_free_cmd_payloads(ctx, s->cmds, s->cmd_count, s->width, s->height);
    if (s->cmds) nx_free(ctx, s->cmds, (size_t)s->cmd_cap * sizeof(nx_cmd), 16);
    s->cmds = NULL;
    s->cmd_count = s->cmd_cap = 0;
    if (s->accum)
        nx_free(ctx, s->accum, (size_t)s->width * s->height * 4 * sizeof(float), 16);
    s->accum = NULL;
    for (i = 0; i < s->ref_count; i++) {
        uint32_t type = s->refs[i] >> 24;
        uint32_t idx = s->refs[i] & 0xffffffu;
        nx_unref_index(dev, type, idx);
    }
    if (s->refs) nx_free(ctx, s->refs, (size_t)s->ref_cap * 4, 16);
    s->refs = NULL;
    s->ref_count = s->ref_cap = 0;
}

static neox_result sub_add_ref(neox_context *ctx, nx_sub_slot *s,
                               uint32_t type, uint32_t idx)
{
    uint32_t *nr = (uint32_t *)nx_array_grow(ctx, s->refs, 4, &s->ref_cap,
                                             s->ref_count + 1);
    if (!nr) return NEOX_ERROR_OUT_OF_MEMORY;
    s->refs = nr;
    s->refs[s->ref_count++] = (type << 24) | idx;
    return NEOX_OK;
}

neox_result neox_frame_end(neox_frame *frame, neox_submission *out_sub)
{
    neox_device *dev;
    neox_context *ctx;
    nx_sub_slot *sub;
    uint32_t sidx, i, *np;
    neox_result r;
    if (!frame) return NEOX_ERROR_INVALID_ARGUMENT;
    dev = frame->dev;
    ctx = dev->ctx;
    if (!out_sub) {
        ctx->last_error = "out_sub is NULL";
        return NEOX_ERROR_INVALID_ARGUMENT;
    }
    *out_sub = (neox_submission){ 0 };
    if (frame->ctm_count != 1) {
        ctx->last_error = "unbalanced state stack at frame_end";
        return NEOX_ERROR_INVALID_ARGUMENT;
    }
    sidx = nx_table_alloc(ctx, &dev->subs);
    if (sidx == UINT32_MAX) {
        ctx->last_error = "submission slot allocation failed";
        return NEOX_ERROR_OUT_OF_MEMORY;
    }
    sub = &((nx_sub_slot *)dev->subs.slots)[sidx];
    memset(sub, 0, sizeof(*sub));
    sub->gen = 1;
    sub->in_use = 1;
    sub->status = NEOX_SUBMISSION_PENDING;
    /* 先做所有可能失败的装配（引用计数、pending 入队），成功后才移交载荷：
     * 失败路径 frame 保持完整、可重试（§10.1 原子性） */
    sub->target = frame->target;
    sub->surface = frame->surface;
    sub->target_stride = frame->target_stride;
    sub->width = frame->width;
    sub->height = frame->height;
    sub->report.flags = 0; /* 后端永不近似（NEOX_ALLOW_APPROXIMATE 无需触发） */
    sub->report.draw_calls = frame->draw_calls;
    /* report.algorithm 由各后端 execute 设置（Soft=SCANLINE，GL=TRIANGLE） */
    /* 引用计数：执行期保活（释放只进延迟队列，pump 才真正回收） */
    for (i = 0; i < frame->cmd_count; i++) {
        const nx_cmd *c = &frame->cmds[i];
        if (c->op == NXC_PUSH && c->clip_idx >= 0) {
            r = sub_add_ref(ctx, sub, NX_TYPE_PATH, (uint32_t)c->clip_idx);
            if (r != NEOX_OK) goto fail;
            ((nx_path_slot *)dev->paths.slots)[c->clip_idx].refcount++;
        } else if (c->op == NXC_PATH) {
            r = sub_add_ref(ctx, sub, NX_TYPE_PATH, (uint32_t)c->res_idx);
            if (r != NEOX_OK) goto fail;
            ((nx_path_slot *)dev->paths.slots)[c->res_idx].refcount++;
        } else if (c->op == NXC_TEX || c->op == NXC_GLYPHS) {
            r = sub_add_ref(ctx, sub, NX_TYPE_TEXTURE, (uint32_t)c->res_idx);
            if (r != NEOX_OK) goto fail;
            ((nx_tex_slot *)dev->textures.slots)[c->res_idx].refcount++;
        }
    }
    np = (uint32_t *)nx_array_grow(ctx, dev->pending, 4, &dev->pending_cap,
                                   dev->pending_count + 1);
    if (!np) {
        ctx->last_error = "pending queue allocation failed";
        goto fail;
    }
    dev->pending = np;
    dev->pending[dev->pending_count++] = sidx;
    /* 装配完成，移交载荷（此后无失败路径） */
    sub->cmds = frame->cmds;      frame->cmds = NULL;
    sub->cmd_count = frame->cmd_count;
    sub->cmd_cap = frame->cmd_cap;
    frame->cmd_count = 0;
    frame->cmd_cap = 0;
    sub->accum = frame->base;     frame->base = NULL;
    /* frame 随提交销毁（净增持久节点恒为零） */
    dev->active_frame = NULL;
    frame_destroy(dev, frame);
    *out_sub = (neox_submission){
        nx_make_handle(dev->id, NX_TYPE_SUB, sidx, sub->gen) };
    return NEOX_OK;
fail:
    /* 原子性：提交失败，目标与本帧资源状态保持调用前一致（§10.1）。
     * frame 保持存活可重试；sub 槽位清空归还。 */
    sub_free_payload(dev, sub);
    sub->in_use = 0;
    /* 归还槽位到空闲链 */
    dev->subs.free_list = (uint32_t *)nx_array_grow(ctx, dev->subs.free_list, 4,
                          &dev->subs.free_cap, dev->subs.free_count + 1);
    if (dev->subs.free_list)
        dev->subs.free_list[dev->subs.free_count++] = sidx;
    return r;
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
    const nx_tex_slot *tx = &((nx_tex_slot *)dev->textures.slots)[cmd->res_idx];
    neox_affine inv;
    float xc[4], yc[4], minx, miny, maxx, maxy;
    int32_t x0, y0, x1, y1, px, py;
    size_t row;
    if (cmd->dst.w == 0.0f || cmd->dst.h == 0.0f) return NEOX_OK;
    /* 退化变换（§3）：逆不存在 → 不产生任何输出 */
    if (!nx_affine_invert(top->ctm, &inv)) return NEOX_OK;
    nx_affine_apply(top->ctm, cmd->dst.x, cmd->dst.y, &xc[0], &yc[0]);
    nx_affine_apply(top->ctm, cmd->dst.x + cmd->dst.w, cmd->dst.y, &xc[1], &yc[1]);
    nx_affine_apply(top->ctm, cmd->dst.x, cmd->dst.y + cmd->dst.h, &xc[2], &yc[2]);
    nx_affine_apply(top->ctm, cmd->dst.x + cmd->dst.w, cmd->dst.y + cmd->dst.h,
                    &xc[3], &yc[3]);
    minx = maxx = xc[0]; miny = maxy = yc[0];
    for (px = 1; px < 4; px++) {
        if (xc[px] < minx) minx = xc[px];
        if (xc[px] > maxx) maxx = xc[px];
        if (yc[px] < miny) miny = yc[px];
        if (yc[px] > maxy) maxy = yc[px];
    }
    x0 = nx_clampi((int32_t)floorf(minx), 0, (int32_t)sub->width);
    x1 = nx_clampi((int32_t)ceilf(maxx), 0, (int32_t)sub->width);
    y0 = nx_clampi((int32_t)floorf(miny), 0, (int32_t)sub->height);
    y1 = nx_clampi((int32_t)ceilf(maxy), 0, (int32_t)sub->height);
    for (py = y0; py < y1; py++) {
        row = (size_t)py * sub->width;
        for (px = x0; px < x1; px++) {
            float lx, ly, tu, tv, u, v, smp[4], s[4];
            int c;
            nx_affine_apply(inv, (float)px + 0.5f, (float)py + 0.5f, &lx, &ly);
            tu = (lx - cmd->dst.x) / cmd->dst.w;
            tv = (ly - cmd->dst.y) / cmd->dst.h;
            if (tu < 0.0f || tu > 1.0f || tv < 0.0f || tv > 1.0f) continue;
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
    const nx_tex_slot *at = &((nx_tex_slot *)dev->textures.slots)[cmd->res_idx];
    neox_affine inv;
    float base[4];
    uint32_t g;
    if (!nx_affine_invert(top->ctm, &inv)) return NEOX_OK; /* §3 退化变换 */
    nx_paint_to_premul_linear(cmd->color, base);
    for (g = 0; g < cmd->glyph_count; g++) {
        const neox_glyph *gl = &cmd->glyphs[g];
        float gw = (float)gl->sw, gh = (float)gl->sh;
        float xc[4], yc[4], minx, miny, maxx, maxy;
        int32_t x0, y0, x1, y1, px, py;
        if (gw == 0.0f || gh == 0.0f) continue;
        nx_affine_apply(top->ctm, gl->x, gl->y, &xc[0], &yc[0]);
        nx_affine_apply(top->ctm, gl->x + gw, gl->y, &xc[1], &yc[1]);
        nx_affine_apply(top->ctm, gl->x, gl->y + gh, &xc[2], &yc[2]);
        nx_affine_apply(top->ctm, gl->x + gw, gl->y + gh, &xc[3], &yc[3]);
        minx = maxx = xc[0]; miny = maxy = yc[0];
        for (px = 1; px < 4; px++) {
            if (xc[px] < minx) minx = xc[px];
            if (xc[px] > maxx) maxx = xc[px];
            if (yc[px] < miny) miny = yc[px];
            if (yc[px] > maxy) maxy = yc[px];
        }
        x0 = nx_clampi((int32_t)floorf(minx), 0, (int32_t)sub->width);
        x1 = nx_clampi((int32_t)ceilf(maxx), 0, (int32_t)sub->width);
        y0 = nx_clampi((int32_t)floorf(miny), 0, (int32_t)sub->height);
        y1 = nx_clampi((int32_t)ceilf(maxy), 0, (int32_t)sub->height);
        for (py = y0; py < y1; py++) {
            for (px = x0; px < x1; px++) {
                float lx, ly, au, av, smp[4];
                float cov;
                nx_affine_apply(inv, (float)px + 0.5f, (float)py + 0.5f, &lx, &ly);
                if (lx < gl->x || lx > gl->x + gw || ly < gl->y || ly > gl->y + gh)
                    continue;
                /* A8 图集：双线性采样遮罩，颜色来自 paint（§5、§8） */
                au = (float)gl->sx + (lx - gl->x);
                av = (float)gl->sy + (ly - gl->y);
                sample_linear(at, au, av, smp);
                cov = smp[3];
                if (cov == 0.0f) continue;
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
    size_t npix = (size_t)sub->width * sub->height;
    uint64_t base_used = ctx->used, saved_peak = ctx->used_peak;
    float **lbufs = NULL;
    uint32_t nlayers = 0, caplayers = 0;
    ex_state *stk = NULL;
    uint32_t nstk = 0, capstk = 0;
    float *mask = NULL;
    uint32_t i;
    neox_result r = NEOX_ERROR_OUT_OF_MEMORY;

    ctx->used_peak = base_used; /* 本 submission 的峰值从执行起点计量 */
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
            stk[nstk].clip = c->clip_mask;
            stk[nstk].layer_idx = c->owns_layer ? (int32_t)nlayers - 1
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
    sub->report.peak_bytes = ctx->used_peak - base_used;
    r = NEOX_OK;

cleanup:
    if (r != NEOX_OK && nlayers > 0) {
        /* 失败：释放执行期分配的组层（基底 accum 由 sub_free_payload 处理） */
        uint32_t k;
        for (k = 1; k < nlayers; k++)
            nx_free(ctx, lbufs[k], npix * 4 * sizeof(float), 16);
    }
    if (mask) nx_free(ctx, mask, npix * sizeof(float), 16);
    if (stk) nx_free(ctx, stk, (size_t)capstk * sizeof(ex_state), 16);
    if (lbufs) nx_free(ctx, lbufs, (size_t)caplayers * sizeof(float *), 16);
    {
        uint64_t peak = ctx->used_peak;
        ctx->used_peak = saved_peak > peak ? saved_peak : peak;
    }
    return r;
#undef EX_FAIL
}

/* ------------------------------------------------------------------ pump 与 submission 查询 */

uint32_t neox_device_pump(neox_device *dev)
{
    uint32_t done = 0, i, out;
    if (!dev) return 0;
    /* Soft 后端无真异步：pump 即完成全部 pending submission（接口语义同异步模型） */
    for (i = 0; i < dev->pending_count; i++) {
        nx_sub_slot *sub = &((nx_sub_slot *)dev->subs.slots)[dev->pending[i]];
        neox_result r = dev->vt->execute(dev, sub);
        sub->status = (r == NEOX_OK) ? NEOX_SUBMISSION_COMPLETE
                                     : NEOX_SUBMISSION_FAILED;
        sub_free_payload(dev, sub);
        done++;
    }
    dev->pending_count = 0;
    /* 延迟回收：执行完成后，引用计数归零的槽位才真正回收 */
    out = 0;
    for (i = 0; i < dev->defer_count; i++) {
        uint32_t type = dev->defer[i] >> 24;
        uint32_t idx = dev->defer[i] & 0xffffffu;
        uint32_t rc = 0;
        if (type == NX_TYPE_TEXTURE && idx < dev->textures.count)
            rc = ((nx_tex_slot *)dev->textures.slots)[idx].refcount;
        else if (type == NX_TYPE_PATH && idx < dev->paths.count)
            rc = ((nx_path_slot *)dev->paths.slots)[idx].refcount;
        if (rc == 0)
            nx_slot_recycle(dev, type, idx);
        else
            dev->defer[out++] = dev->defer[i];
    }
    dev->defer_count = out;
    return done;
}

neox_submission_status neox_submission_get_status(neox_device *dev,
                                                  neox_submission sub)
{
    uint32_t idx;
    if (!dev) return NEOX_SUBMISSION_FAILED;
    if (nx_resolve(dev, sub.id, NX_TYPE_SUB, &idx) != NEOX_OK)
        return NEOX_SUBMISSION_FAILED;
    return ((nx_sub_slot *)dev->subs.slots)[idx].status;
}

neox_result neox_submission_wait(neox_device *dev, neox_submission sub)
{
    uint32_t idx;
    neox_result r;
    nx_sub_slot *s;
    if (!dev) return NEOX_ERROR_INVALID_ARGUMENT;
    r = nx_resolve(dev, sub.id, NX_TYPE_SUB, &idx);
    if (r != NEOX_OK) return r;
    s = &((nx_sub_slot *)dev->subs.slots)[idx];
    /* 无线程环境：内部为 pump 循环；Soft 一次 pump 即完成 */
    while (s->status == NEOX_SUBMISSION_PENDING)
        neox_device_pump(dev);
    return NEOX_OK;
}

neox_result neox_submission_get_report(neox_device *dev, neox_submission sub,
                                       neox_exec_report *out_report)
{
    uint32_t idx;
    neox_result r;
    if (!dev) return NEOX_ERROR_INVALID_ARGUMENT;
    if (!out_report) {
        dev->ctx->last_error = "out_report is NULL";
        return NEOX_ERROR_INVALID_ARGUMENT;
    }
    r = nx_resolve(dev, sub.id, NX_TYPE_SUB, &idx);
    if (r != NEOX_OK) return r;
    *out_report = ((nx_sub_slot *)dev->subs.slots)[idx].report;
    return NEOX_OK;
}

/* ------------------------------------------------------------------ Soft 后端 vtable */

/* begin_target：校验 SOFT 目标、确定帧尺寸、装载基底（清屏色或既有目标内容） */
static neox_result soft_begin_target(neox_frame *fr,
                                     const neox_target_desc *target)
{
    neox_context *ctx = fr->dev->ctx;
    size_t npix, i;
    if (!target->soft.pixels || target->soft.width == 0 ||
        target->soft.height == 0 ||
        target->soft.stride < (size_t)target->soft.width * 4)
        NX_ERR(ctx, "invalid soft target", NEOX_ERROR_INVALID_ARGUMENT);
    fr->target = target->soft.pixels;
    fr->target_stride = target->soft.stride;
    fr->width = target->soft.width;
    fr->height = target->soft.height;
    npix = (size_t)fr->width * fr->height;
    fr->base = (float *)nx_alloc(ctx, npix * 4 * sizeof(float), 16);
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
    out_caps->max_texture_size = NEOX_SOFT_MAX_TEX;
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
        soft_get_caps
    };
    return &vt;
}

/* ------------------------------------------------------------------ 场景（保留模式，规范 §10.2-1）
 *
 * 脏区追踪说明（增量执行预留）：node_set_* 会设置节点自身的属性脏位，并把
 * "子树脏" 位沿父链传播到根。当前执行路径仍是粗粒度的整体重绘（scene_render
 * 每次全量遍历），但脏位结构已经就位：后续增量执行器只需在 scene_render 前
 * 检查脏位决定重绘范围，不必改动节点结构（规范 §10.2-2 的一致性由全量重绘
 * 天然保证）。 */
enum {
    NX_DIRTY_TRANSFORM = 0x01,
    NX_DIRTY_PAINT     = 0x02,
    NX_DIRTY_OPACITY   = 0x04,
    NX_DIRTY_VISIBLE   = 0x08,
    NX_DIRTY_CHILDREN  = 0x10, /* 子树结构或子节点属性变化 */
    NX_DIRTY_SUBTREE   = 0x20  /* 祖先视角：本节点子树内有脏 */
};

static void node_mark_dirty(neox_device *dev, uint32_t idx, uint32_t bits)
{
    nx_node_slot *n = &((nx_node_slot *)dev->nodes.slots)[idx];
    n->dirty |= bits;
    while (n->parent != idx) {
        idx = n->parent;
        n = &((nx_node_slot *)dev->nodes.slots)[idx];
        if (n->dirty & NX_DIRTY_SUBTREE) break; /* 已传播过 */
        n->dirty |= NX_DIRTY_SUBTREE;
    }
}

static uint32_t node_root(neox_device *dev, uint32_t idx)
{
    nx_node_slot *n = &((nx_node_slot *)dev->nodes.slots)[idx];
    while (n->parent != idx) {
        idx = n->parent;
        n = &((nx_node_slot *)dev->nodes.slots)[idx];
    }
    return idx;
}

neox_scene neox_scene_create(neox_device *dev)
{
    neox_context *ctx;
    uint32_t sidx, nidx;
    nx_scene_slot *sc;
    nx_node_slot *root;
    if (!dev) return (neox_scene){ 0 };
    ctx = dev->ctx;
    sidx = nx_table_alloc(ctx, &dev->scenes);
    if (sidx == UINT32_MAX) { ctx->last_error = "scene slot allocation failed"; return (neox_scene){ 0 }; }
    nidx = nx_table_alloc(ctx, &dev->nodes);
    if (nidx == UINT32_MAX) {
        /* 归还场景槽位：无其他副作用，满足"失败不留半完成状态" */
        uint32_t *nf = (uint32_t *)nx_array_grow(ctx, dev->scenes.free_list, 4,
                         &dev->scenes.free_cap, dev->scenes.free_count + 1);
        if (nf) dev->scenes.free_list = nf; /* 失败则槽位不再复用，可接受 */
        if (nf) dev->scenes.free_list[dev->scenes.free_count++] = sidx;
        ctx->last_error = "scene root allocation failed";
        return (neox_scene){ 0 };
    }
    sc = &((nx_scene_slot *)dev->scenes.slots)[sidx];
    memset(sc, 0, sizeof(*sc));
    sc->gen = 1; sc->in_use = 1;
    root = &((nx_node_slot *)dev->nodes.slots)[nidx];
    memset(root, 0, sizeof(*root));
    root->gen = 1; root->in_use = 1;
    root->kind = NEOX_NODE_GROUP;
    root->transform = nx_affine_identity();
    root->blend = NEOX_BLEND_SRC_OVER;
    root->opacity = 1.0f;
    root->visible = 1;
    root->parent = nidx;
    sc->root = nidx;
    return (neox_scene){ nx_make_handle(dev->id, NX_TYPE_SCENE, sidx, 1) };
}

static void node_free_subtree(neox_device *dev, uint32_t idx)
{
    neox_context *ctx = dev->ctx;
    nx_node_slot *n = &((nx_node_slot *)dev->nodes.slots)[idx];
    uint32_t i;
    for (i = 0; i < n->child_count; i++)
        node_free_subtree(dev, n->children[i]);
    if (n->children) nx_free(ctx, n->children, (size_t)n->child_cap * 4, 16);
    if (n->kind == NEOX_NODE_GLYPHS && n->u.glyphs.glyphs)
        nx_free(ctx, n->u.glyphs.glyphs,
                (size_t)n->u.glyphs.count * sizeof(neox_glyph), 16);
    memset(n, 0, sizeof(*n));
    dev->nodes.free_list = (uint32_t *)nx_array_grow(ctx, dev->nodes.free_list, 4,
                             &dev->nodes.free_cap, dev->nodes.free_count + 1);
    if (dev->nodes.free_list)
        dev->nodes.free_list[dev->nodes.free_count++] = idx;
}

void neox_scene_destroy(neox_device *dev, neox_scene scene)
{
    uint32_t sidx;
    neox_context *ctx;
    nx_scene_slot *sc;
    if (!dev) return;
    ctx = dev->ctx;
    if (nx_resolve(dev, scene.id, NX_TYPE_SCENE, &sidx) != NEOX_OK) return;
    sc = &((nx_scene_slot *)dev->scenes.slots)[sidx];
    node_free_subtree(dev, sc->root);
    memset(sc, 0, sizeof(*sc));
    dev->scenes.free_list = (uint32_t *)nx_array_grow(ctx, dev->scenes.free_list, 4,
                             &dev->scenes.free_cap, dev->scenes.free_count + 1);
    if (dev->scenes.free_list)
        dev->scenes.free_list[dev->scenes.free_count++] = sidx;
}

/* node_create：先完成全部校验，再分配槽位 —— 失败零副作用（§10.1） */
neox_result neox_node_create(neox_device *dev, neox_scene scene,
                             const neox_node_desc *desc, neox_node *out_node)
{
    neox_context *ctx;
    uint32_t sidx, idx, root_idx, *nc;
    nx_scene_slot *sc;
    nx_node_slot *n, *root;
    neox_result r;
    if (!dev) return NEOX_ERROR_INVALID_ARGUMENT;
    ctx = dev->ctx;
    if (!desc || !out_node)
        NX_ERR(ctx, "desc/out_node is NULL", NEOX_ERROR_INVALID_ARGUMENT);
    *out_node = (neox_node){ 0 };
    r = nx_resolve(dev, scene.id, NX_TYPE_SCENE, &sidx);
    if (r != NEOX_OK) return r;
    if (desc->kind < NEOX_NODE_GROUP || desc->kind > NEOX_NODE_GLYPHS)
        NX_ERR(ctx, "invalid node kind", NEOX_ERROR_INVALID_ARGUMENT);
    if (desc->blend > NEOX_BLEND_PLUS)
        NX_ERR(ctx, "invalid blend mode", NEOX_ERROR_INVALID_ARGUMENT);
    if (nx_float_bad(desc->opacity) || desc->opacity < 0.0f || desc->opacity > 1.0f)
        NX_ERR(ctx, "opacity out of range or NaN", NEOX_ERROR_INVALID_ARGUMENT);
    {
        const float *m = &desc->transform.a;
        uint32_t i;
        for (i = 0; i < 6; i++)
            if (nx_float_bad(m[i]))
                NX_ERR(ctx, "transform contains NaN/Inf", NEOX_ERROR_INVALID_ARGUMENT);
    }
    /* 按类型校验引用（校验阶段不做任何分配） */
    switch (desc->kind) {
    case NEOX_NODE_PATH: {
        uint32_t p;
        r = nx_resolve(dev, desc->path.path.id, NX_TYPE_PATH, &p);
        if (r != NEOX_OK) return r;
        if (color_is_bad(desc->path.paint.color))
            NX_ERR(ctx, "paint color contains NaN/Inf", NEOX_ERROR_INVALID_ARGUMENT);
        break;
    }
    case NEOX_NODE_IMAGE: {
        uint32_t t;
        r = nx_resolve(dev, desc->image.tex.id, NX_TYPE_TEXTURE, &t);
        if (r != NEOX_OK) return r;
        if (desc->image.filter != NEOX_FILTER_NEAREST &&
            desc->image.filter != NEOX_FILTER_LINEAR)
            NX_ERR(ctx, "invalid filter", NEOX_ERROR_INVALID_ARGUMENT);
        break;
    }
    case NEOX_NODE_GLYPHS: {
        uint32_t t;
        r = nx_resolve(dev, desc->glyphs.atlas.id, NX_TYPE_TEXTURE, &t);
        if (r != NEOX_OK) return r;
        if (!desc->glyphs.glyphs || desc->glyphs.count == 0)
            NX_ERR(ctx, "glyphs empty", NEOX_ERROR_INVALID_ARGUMENT);
        if (color_is_bad(desc->glyphs.paint.color))
            NX_ERR(ctx, "paint color contains NaN/Inf", NEOX_ERROR_INVALID_ARGUMENT);
        break;
    }
    case NEOX_NODE_GROUP:
        if (desc->group.clip.id != 0) {
            uint32_t p;
            r = nx_resolve(dev, desc->group.clip.id, NX_TYPE_PATH, &p);
            if (r != NEOX_OK) return r;
        }
        break;
    }
    sc = &((nx_scene_slot *)dev->scenes.slots)[sidx];
    idx = nx_table_alloc(ctx, &dev->nodes);
    if (idx == UINT32_MAX) NX_ERR(ctx, "node slot allocation failed",
                                  NEOX_ERROR_OUT_OF_MEMORY);
    n = &((nx_node_slot *)dev->nodes.slots)[idx];
    memset(n, 0, sizeof(*n));
    n->gen = 1; n->in_use = 1;
    n->kind = desc->kind;
    n->transform = desc->transform;
    n->blend = desc->blend;
    n->opacity = desc->opacity;
    n->visible = desc->visible ? 1 : 0;
    root_idx = sc->root;
    n->parent = root_idx;
    switch (desc->kind) {
    case NEOX_NODE_PATH: {
        uint32_t p;
        nx_resolve(dev, desc->path.path.id, NX_TYPE_PATH, &p);
        n->u.path.path_idx = p;
        n->u.path.path_id = desc->path.path.id;
        n->u.path.paint = desc->path.paint;
        break;
    }
    case NEOX_NODE_IMAGE: {
        uint32_t t;
        nx_resolve(dev, desc->image.tex.id, NX_TYPE_TEXTURE, &t);
        n->u.image.tex_idx = t;
        n->u.image.tex_id = desc->image.tex.id;
        n->u.image.src = desc->image.src;
        n->u.image.dst = desc->image.dst;
        n->u.image.filter = desc->image.filter;
        break;
    }
    case NEOX_NODE_GLYPHS: {
        uint32_t t;
        neox_glyph *copy;
        nx_resolve(dev, desc->glyphs.atlas.id, NX_TYPE_TEXTURE, &t);
        copy = (neox_glyph *)nx_alloc(ctx,
                  (size_t)desc->glyphs.count * sizeof(neox_glyph), 16);
        if (!copy) {
            memset(n, 0, sizeof(*n)); /* 槽位未挂链，直接清空归还 */
            dev->nodes.free_list = (uint32_t *)nx_array_grow(ctx,
                dev->nodes.free_list, 4, &dev->nodes.free_cap,
                dev->nodes.free_count + 1);
            if (dev->nodes.free_list)
                dev->nodes.free_list[dev->nodes.free_count++] = idx;
            NX_ERR(ctx, "glyph array allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
        }
        memcpy(copy, desc->glyphs.glyphs,
               (size_t)desc->glyphs.count * sizeof(neox_glyph));
        n->u.glyphs.atlas_idx = t;
        n->u.glyphs.atlas_id = desc->glyphs.atlas.id;
        n->u.glyphs.glyphs = copy;
        n->u.glyphs.count = desc->glyphs.count;
        n->u.glyphs.paint = desc->glyphs.paint;
        break;
    }
    case NEOX_NODE_GROUP:
        if (desc->group.clip.id != 0) {
            uint32_t p;
            nx_resolve(dev, desc->group.clip.id, NX_TYPE_PATH, &p);
            n->u.group.clip_idx = p;
            n->u.group.has_clip = 1;
            n->u.group.clip_id = desc->group.clip.id;
        }
        break;
    }
    /* 挂到场景根（文档顺序追加） */
    root = &((nx_node_slot *)dev->nodes.slots)[root_idx];
    nc = (uint32_t *)nx_array_grow(ctx, root->children, 4, &root->child_cap,
                                   root->child_count + 1);
    if (!nc) {
        if (n->kind == NEOX_NODE_GLYPHS && n->u.glyphs.glyphs)
            nx_free(ctx, n->u.glyphs.glyphs,
                    (size_t)n->u.glyphs.count * sizeof(neox_glyph), 16);
        memset(n, 0, sizeof(*n));
        dev->nodes.free_list = (uint32_t *)nx_array_grow(ctx,
            dev->nodes.free_list, 4, &dev->nodes.free_cap,
            dev->nodes.free_count + 1);
        if (dev->nodes.free_list)
            dev->nodes.free_list[dev->nodes.free_count++] = idx;
        NX_ERR(ctx, "child list allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    }
    root->children = nc;
    root->children[root->child_count++] = idx;
    node_mark_dirty(dev, root_idx, NX_DIRTY_CHILDREN);
    *out_node = (neox_node){ nx_make_handle(dev->id, NX_TYPE_NODE, idx, 1) };
    return NEOX_OK;
}

void neox_node_release(neox_device *dev, neox_node node)
{
    uint32_t idx;
    nx_node_slot *n;
    if (!dev) return;
    if (nx_resolve(dev, node.id, NX_TYPE_NODE, &idx) != NEOX_OK) return;
    n = &((nx_node_slot *)dev->nodes.slots)[idx];
    if (n->parent != idx) {
        nx_node_slot *p = &((nx_node_slot *)dev->nodes.slots)[n->parent];
        uint32_t i, out = 0;
        for (i = 0; i < p->child_count; i++) {
            if (p->children[i] != idx) p->children[out++] = p->children[i];
        }
        p->child_count = out;
        node_mark_dirty(dev, n->parent, NX_DIRTY_CHILDREN);
    }
    node_free_subtree(dev, idx);
}

neox_result neox_node_append_child(neox_device *dev, neox_node parent,
                                   neox_node child)
{
    neox_context *ctx;
    uint32_t pi, ci, i;
    nx_node_slot *p, *c;
    neox_result r;
    if (!dev) return NEOX_ERROR_INVALID_ARGUMENT;
    ctx = dev->ctx;
    r = nx_resolve(dev, parent.id, NX_TYPE_NODE, &pi);
    if (r != NEOX_OK) return r;
    r = nx_resolve(dev, child.id, NX_TYPE_NODE, &ci);
    if (r != NEOX_OK) return r;
    if (node_root(dev, pi) != node_root(dev, ci))
        NX_ERR(ctx, "nodes belong to different scenes",
               NEOX_ERROR_OWNERSHIP_VIOLATION);
    /* 防环：child 不得是 parent 的祖先 */
    {
        uint32_t walk = pi;
        nx_node_slot *wn = &((nx_node_slot *)dev->nodes.slots)[walk];
        while (wn->parent != walk) {
            if (walk == ci)
                NX_ERR(ctx, "append_child would create a cycle",
                       NEOX_ERROR_INVALID_ARGUMENT);
            walk = wn->parent;
            wn = &((nx_node_slot *)dev->nodes.slots)[walk];
        }
    }
    p = &((nx_node_slot *)dev->nodes.slots)[pi];
    c = &((nx_node_slot *)dev->nodes.slots)[ci];
    if (c->parent != ci) {
        nx_node_slot *op = &((nx_node_slot *)dev->nodes.slots)[c->parent];
        uint32_t out = 0;
        for (i = 0; i < op->child_count; i++) {
            if (op->children[i] != ci) op->children[out++] = op->children[i];
        }
        op->child_count = out;
    }
    {
        uint32_t *nc = (uint32_t *)nx_array_grow(ctx, p->children, 4,
                                                 &p->child_cap,
                                                 p->child_count + 1);
        if (!nc) NX_ERR(ctx, "child list allocation failed",
                        NEOX_ERROR_OUT_OF_MEMORY);
        p->children = nc;
        p->children[p->child_count++] = ci;
    }
    c->parent = pi;
    node_mark_dirty(dev, pi, NX_DIRTY_CHILDREN);
    return NEOX_OK;
}

neox_result neox_node_remove_from_parent(neox_device *dev, neox_node child)
{
    uint32_t ci, i, out = 0;
    nx_node_slot *c, *p;
    neox_result r;
    if (!dev) return NEOX_ERROR_INVALID_ARGUMENT;
    r = nx_resolve(dev, child.id, NX_TYPE_NODE, &ci);
    if (r != NEOX_OK) return r;
    c = &((nx_node_slot *)dev->nodes.slots)[ci];
    if (c->parent == ci) return NEOX_OK; /* 根节点：无父可摘 */
    p = &((nx_node_slot *)dev->nodes.slots)[c->parent];
    for (i = 0; i < p->child_count; i++) {
        if (p->children[i] != ci) p->children[out++] = p->children[i];
    }
    p->child_count = out;
    node_mark_dirty(dev, c->parent, NX_DIRTY_CHILDREN);
    c->parent = ci;
    return NEOX_OK;
}

static neox_result node_check_opacity(neox_context *ctx, float opacity)
{
    if (nx_float_bad(opacity) || opacity < 0.0f || opacity > 1.0f)
        NX_ERR(ctx, "opacity out of range or NaN", NEOX_ERROR_INVALID_ARGUMENT);
    return NEOX_OK;
}

neox_result neox_node_set_transform(neox_device *dev, neox_node node,
                                    neox_affine t)
{
    uint32_t idx;
    nx_node_slot *n;
    neox_result r;
    const float *m = &t.a;
    uint32_t i;
    if (!dev) return NEOX_ERROR_INVALID_ARGUMENT;
    for (i = 0; i < 6; i++)
        if (nx_float_bad(m[i])) {
            dev->ctx->last_error = "transform contains NaN/Inf";
            return NEOX_ERROR_INVALID_ARGUMENT;
        }
    r = nx_resolve(dev, node.id, NX_TYPE_NODE, &idx);
    if (r != NEOX_OK) return r;
    n = &((nx_node_slot *)dev->nodes.slots)[idx];
    n->transform = t;
    node_mark_dirty(dev, idx, NX_DIRTY_TRANSFORM);
    return NEOX_OK;
}

neox_result neox_node_set_paint(neox_device *dev, neox_node node,
                                const neox_paint *p)
{
    uint32_t idx;
    nx_node_slot *n;
    neox_result r;
    if (!dev) return NEOX_ERROR_INVALID_ARGUMENT;
    if (!p) {
        dev->ctx->last_error = "paint is NULL";
        return NEOX_ERROR_INVALID_ARGUMENT;
    }
    if (color_is_bad(p->color)) {
        dev->ctx->last_error = "paint color contains NaN/Inf";
        return NEOX_ERROR_INVALID_ARGUMENT;
    }
    r = nx_resolve(dev, node.id, NX_TYPE_NODE, &idx);
    if (r != NEOX_OK) return r;
    n = &((nx_node_slot *)dev->nodes.slots)[idx];
    if (n->kind == NEOX_NODE_PATH) n->u.path.paint = *p;
    else if (n->kind == NEOX_NODE_GLYPHS) n->u.glyphs.paint = *p;
    else {
        dev->ctx->last_error = "node has no paint";
        return NEOX_ERROR_INVALID_ARGUMENT;
    }
    node_mark_dirty(dev, idx, NX_DIRTY_PAINT);
    return NEOX_OK;
}

neox_result neox_node_set_opacity(neox_device *dev, neox_node node, float opacity)
{
    uint32_t idx;
    nx_node_slot *n;
    neox_result r;
    if (!dev) return NEOX_ERROR_INVALID_ARGUMENT;
    r = node_check_opacity(dev->ctx, opacity);
    if (r != NEOX_OK) return r;
    r = nx_resolve(dev, node.id, NX_TYPE_NODE, &idx);
    if (r != NEOX_OK) return r;
    n = &((nx_node_slot *)dev->nodes.slots)[idx];
    n->opacity = opacity;
    node_mark_dirty(dev, idx, NX_DIRTY_OPACITY);
    return NEOX_OK;
}

neox_result neox_node_set_visible(neox_device *dev, neox_node node, int visible)
{
    uint32_t idx;
    nx_node_slot *n;
    neox_result r;
    if (!dev) return NEOX_ERROR_INVALID_ARGUMENT;
    r = nx_resolve(dev, node.id, NX_TYPE_NODE, &idx);
    if (r != NEOX_OK) return r;
    n = &((nx_node_slot *)dev->nodes.slots)[idx];
    n->visible = visible ? 1 : 0;
    node_mark_dirty(dev, idx, NX_DIRTY_VISIBLE);
    return NEOX_OK;
}

/* scene_render：遍历节点树，发射与立即模式完全相同的命令流（§10.2-1 的构造性证明） */
static neox_result scene_emit(neox_frame *fr, uint32_t idx, neox_affine parent_ctm)
{
    neox_device *dev = fr->dev;
    nx_node_slot *n = &((nx_node_slot *)dev->nodes.slots)[idx];
    neox_affine ctm = nx_affine_mul(parent_ctm, n->transform);
    uint32_t i;
    neox_result r;
    if (!n->visible) return NEOX_OK;
    if (n->kind == NEOX_NODE_GROUP) {
        int32_t clip_idx = -1;
        uint64_t clip_id = 0;
        if (n->u.group.has_clip) {
            uint32_t p;
            r = nx_resolve(dev, n->u.group.clip_id, NX_TYPE_PATH, &p);
            if (r != NEOX_OK) return r;
            clip_idx = (int32_t)p;
            clip_id = n->u.group.clip_id;
        }
        r = emit_push(fr, ctm, n->opacity, n->blend, 0, clip_idx, clip_id);
        if (r != NEOX_OK) return r;
        for (i = 0; i < n->child_count; i++) {
            r = scene_emit(fr, n->children[i], ctm);
            if (r != NEOX_OK) return r;
        }
        return emit_pop(fr);
    }
    /* 叶子：push（transform/opacity/blend）→ 绘制 → pop */
    r = emit_push(fr, ctm, n->opacity, n->blend, 0, -1, 0);
    if (r != NEOX_OK) return r;
    switch (n->kind) {
    case NEOX_NODE_PATH: {
        uint32_t p;
        r = nx_resolve(dev, n->u.path.path_id, NX_TYPE_PATH, &p);
        if (r != NEOX_OK) return r;
        r = emit_path(fr, (int32_t)p, n->u.path.path_id, n->u.path.paint.color);
        break;
    }
    case NEOX_NODE_IMAGE: {
        uint32_t t;
        r = nx_resolve(dev, n->u.image.tex_id, NX_TYPE_TEXTURE, &t);
        if (r != NEOX_OK) return r;
        r = emit_tex(fr, (int32_t)t, n->u.image.tex_id, n->u.image.src,
                     n->u.image.dst, n->u.image.filter, 1.0f);
        break;
    }
    case NEOX_NODE_GLYPHS: {
        uint32_t t;
        r = nx_resolve(dev, n->u.glyphs.atlas_id, NX_TYPE_TEXTURE, &t);
        if (r != NEOX_OK) return r;
        r = emit_glyphs(fr, (int32_t)t, n->u.glyphs.atlas_id, n->u.glyphs.glyphs,
                        n->u.glyphs.count, n->u.glyphs.paint.color);
        break;
    }
    default:
        r = NEOX_ERROR_BACKEND;
    }
    if (r != NEOX_OK) return r;
    return emit_pop(fr);
}

neox_result neox_scene_render(neox_frame *frame, neox_scene scene, neox_node root)
{
    neox_device *dev;
    uint32_t sidx;
    nx_scene_slot *sc;
    uint32_t root_idx;
    neox_result r;
    if (!frame) return NEOX_ERROR_INVALID_ARGUMENT;
    dev = frame->dev;
    r = nx_resolve(dev, scene.id, NX_TYPE_SCENE, &sidx);
    if (r != NEOX_OK) return r;
    sc = &((nx_scene_slot *)dev->scenes.slots)[sidx];
    if (root.id == 0) {
        root_idx = sc->root;
    } else {
        r = nx_resolve(dev, root.id, NX_TYPE_NODE, &root_idx);
        if (r != NEOX_OK) return r;
        if (node_root(dev, root_idx) != sc->root) {
            dev->ctx->last_error = "root node belongs to another scene";
            return NEOX_ERROR_OWNERSHIP_VIOLATION;
        }
    }
    return scene_emit(frame, root_idx, nx_affine_identity());
}

/* ------------------------------------------------------------------ CAP 捕获/回放（最小版，第一阶段仅本地）
 *
 * 格式（自包含，含版本头与资源映射表；不承诺跨版本兼容）：
 *   header  : magic[4]={'N','X','C','A'} | version u32 (=1) | res_count u32 | cmd_count u32
 *   resource: type u32 (1=texture,2=path) | cap_id u32
 *             texture: w u32 | h u32 | format u32 | px_bytes u32 (=w*h*16) | 预乘线性 float 像素
 *             path   : verb_count u32 | point_count u32 | fill_rule u32 | verbs | points
 *   command : op u32
 *             PUSH: ctm 6×f32 | opacity f32 | blend u32 | flags u32 | clip_cap_id u32 (0xFFFFFFFF=无) | owns u32
 *             POP : -
 *             PATH: res cap_id u32 | color 4×f32
 *             TEX : res cap_id u32 | src 4×f32 | dst 4×f32 | filter u32 | opacity f32
 *             GLYPHS: res cap_id u32 | color 4×f32 | count u32 | glyphs (x,y f32; sx,sy,sw,sh u16)
 * 多字节整数为原生小端（第一阶段目标平台约定；CAP 稳定格式属 Phase 2）。
 * CAP 内 ID → 本地句柄的映射在 replay 时经会话内映射表重建（neox.h 契约）。 */
#define NX_CAP_VERSION 1u
#define NX_CAP_NO_RES  0xFFFFFFFFu

typedef struct nx_cap_res {
    uint64_t orig_id;       /* 录制侧原始句柄（映射表键） */
    uint32_t cap_id;        /* CAP 内 ID（序列化值，从 1 起） */
    uint32_t type;          /* NX_TYPE_TEXTURE / NX_TYPE_PATH */
    /* 纹理快照 */
    uint32_t w, h, format;
    float   *px;
    /* 路径快照 */
    uint8_t *verbs;
    float   *points;
    uint32_t verb_count, point_count, fill_rule;
} nx_cap_res;

struct neox_capture {
    nx_device *dev;
    nx_cap_res *res;
    uint32_t res_count, res_cap;
    nx_cmd   *cmds;
    uint32_t  cmd_count, cmd_cap;
};

static void cap_free_res(neox_context *ctx, nx_cap_res *r)
{
    if (r->px)     nx_free(ctx, r->px, (size_t)r->w * r->h * 16, 16);
    if (r->verbs)  nx_free(ctx, r->verbs, r->verb_count, 4);
    if (r->points) nx_free(ctx, r->points, (size_t)r->point_count * 8, 16);
    memset(r, 0, sizeof(*r));
}

/* 资源入表（首次引用时深拷贝）；返回 cap_id；id==0 返回 NX_CAP_NO_RES */
static uint32_t cap_resource(neox_capture *cap, nx_device *dev, uint32_t type,
                             uint64_t id)
{
    neox_context *ctx = dev->ctx;
    nx_cap_res *nr;
    uint32_t i;
    if (id == 0) return NX_CAP_NO_RES;
    for (i = 0; i < cap->res_count; i++) {
        if (cap->res[i].orig_id == id && cap->res[i].type == type)
            return cap->res[i].cap_id;
    }
    nr = (nx_cap_res *)nx_array_grow(ctx, cap->res, sizeof(nx_cap_res),
                                     &cap->res_cap, cap->res_count + 1);
    if (!nr) return NX_CAP_NO_RES; /* 快照失败：命令仍记录，replay 将报 UNSUPPORTED */
    cap->res = nr;
    {
        nx_cap_res *e = &cap->res[cap->res_count];
        memset(e, 0, sizeof(*e));
        e->orig_id = id;
        e->cap_id = cap->res_count + 1;
        e->type = type;
        if (type == NX_TYPE_TEXTURE) {
            uint32_t idx;
            const nx_tex_slot *s;
            size_t nbytes;
            if (nx_resolve(dev, id, NX_TYPE_TEXTURE, &idx) != NEOX_OK)
                return NX_CAP_NO_RES;
            s = &((nx_tex_slot *)dev->textures.slots)[idx];
            nbytes = (size_t)s->width * s->height * 16;
            e->px = (float *)nx_alloc(ctx, nbytes, 16);
            if (!e->px) { e->orig_id = 0; return NX_CAP_NO_RES; }
            memcpy(e->px, s->px, nbytes);
            e->w = s->width; e->h = s->height; e->format = s->format;
        } else {
            uint32_t idx;
            const nx_path_slot *s;
            if (nx_resolve(dev, id, NX_TYPE_PATH, &idx) != NEOX_OK)
                return NX_CAP_NO_RES;
            s = &((nx_path_slot *)dev->paths.slots)[idx];
            e->verbs = (uint8_t *)nx_alloc(ctx, s->verb_count ? s->verb_count : 1, 4);
            e->points = (float *)nx_alloc(ctx,
                          ((size_t)s->point_count * 8) ? (size_t)s->point_count * 8 : 4, 16);
            if (!e->verbs || !e->points) {
                cap_free_res(ctx, e);
                return NX_CAP_NO_RES;
            }
            memcpy(e->verbs, s->verbs, s->verb_count);
            memcpy(e->points, s->points, (size_t)s->point_count * 8);
            e->verb_count = s->verb_count;
            e->point_count = s->point_count;
            e->fill_rule = (uint32_t)s->fill_rule;
        }
        cap->res_count++;
        return e->cap_id;
    }
}

static uint32_t cap_find(const neox_capture *cap, uint64_t id, uint32_t type)
{
    uint32_t i;
    if (id == 0) return NX_CAP_NO_RES;
    for (i = 0; i < cap->res_count; i++)
        if (cap->res[i].orig_id == id && cap->res[i].type == type)
            return cap->res[i].cap_id;
    return NX_CAP_NO_RES;
}

/* 录制一条命令（深拷贝 glyphs；资源经映射表登记） */
static neox_result cap_record_cmd(neox_capture *cap, const nx_cmd *c)
{
    neox_device *dev = cap->dev;
    neox_context *ctx = dev->ctx;
    nx_cmd *nc = (nx_cmd *)nx_array_grow(ctx, cap->cmds, sizeof(nx_cmd),
                                         &cap->cmd_cap, cap->cmd_count + 1);
    if (!nc) return NEOX_ERROR_OUT_OF_MEMORY;
    cap->cmds = nc;
    cap->cmds[cap->cmd_count] = *c;
    if (c->op == NXC_GLYPHS && c->glyph_count) {
        neox_glyph *copy = (neox_glyph *)nx_alloc(ctx,
            (size_t)c->glyph_count * sizeof(neox_glyph), 16);
        if (!copy) return NEOX_ERROR_OUT_OF_MEMORY;
        memcpy(copy, c->glyphs, (size_t)c->glyph_count * sizeof(neox_glyph));
        cap->cmds[cap->cmd_count].glyphs = copy;
    }
    if (c->op == NXC_PUSH && c->clip_idx >= 0) {
        cap->cmds[cap->cmd_count].clip_mask = NULL; /* 录制不携带执行期掩码 */
        cap_resource(cap, dev, NX_TYPE_PATH, c->clip_id);
    }
    if (c->op == NXC_PATH)
        cap_resource(cap, dev, NX_TYPE_PATH, c->res_id);
    else if (c->op == NXC_TEX || c->op == NXC_GLYPHS)
        cap_resource(cap, dev, NX_TYPE_TEXTURE, c->res_id);
    cap->cmd_count++;
    return NEOX_OK;
}

neox_result neox_capture_begin(neox_device *dev, neox_capture **out_cap)
{
    neox_context *ctx;
    neox_capture *cap;
    if (!dev || !out_cap) return NEOX_ERROR_INVALID_ARGUMENT;
    ctx = dev->ctx;
    *out_cap = NULL;
    if (dev->cap)
        NX_ERR(ctx, "a capture is already active", NEOX_ERROR_INVALID_ARGUMENT);
    cap = (neox_capture *)nx_alloc(ctx, sizeof(neox_capture), 16);
    if (!cap) NX_ERR(ctx, "capture allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    memset(cap, 0, sizeof(*cap));
    cap->dev = dev;
    dev->cap = cap;
    *out_cap = cap;
    return NEOX_OK;
}

/* 序列化小工具 */
static neox_result cap_put_u32(neox_context *ctx, nx_buf *b, uint32_t v)
{
    return nx_buf_put(ctx, b, &v, 4);
}
static neox_result cap_put_f32(neox_context *ctx, nx_buf *b, float v)
{
    return nx_buf_put(ctx, b, &v, 4);
}
static neox_result cap_put_u16(neox_context *ctx, nx_buf *b, uint16_t v)
{
    return nx_buf_put(ctx, b, &v, 2);
}

neox_result neox_capture_end(neox_capture *cap, uint8_t **out_data, size_t *out_size)
{
    neox_context *ctx;
    nx_buf b;
    uint32_t i, j;
    uint8_t *exact;
    static const char magic[4] = { 'N', 'X', 'C', 'A' };
    if (!cap || !out_data || !out_size)
        return NEOX_ERROR_INVALID_ARGUMENT;
    ctx = cap->dev->ctx;
    *out_data = NULL;
    *out_size = 0;
    memset(&b, 0, sizeof(b));
    if (nx_buf_put(ctx, &b, magic, 4) != NEOX_OK ||
        cap_put_u32(ctx, &b, NX_CAP_VERSION) != NEOX_OK ||
        cap_put_u32(ctx, &b, cap->res_count) != NEOX_OK ||
        cap_put_u32(ctx, &b, cap->cmd_count) != NEOX_OK)
        goto oom;
    for (i = 0; i < cap->res_count; i++) {
        const nx_cap_res *e = &cap->res[i];
        if (cap_put_u32(ctx, &b, e->type) != NEOX_OK ||
            cap_put_u32(ctx, &b, e->cap_id) != NEOX_OK)
            goto oom;
        if (e->type == NX_TYPE_TEXTURE) {
            uint32_t nbytes = e->w * e->h * 16;
            if (cap_put_u32(ctx, &b, e->w) != NEOX_OK ||
                cap_put_u32(ctx, &b, e->h) != NEOX_OK ||
                cap_put_u32(ctx, &b, e->format) != NEOX_OK ||
                cap_put_u32(ctx, &b, nbytes) != NEOX_OK ||
                nx_buf_put(ctx, &b, e->px, nbytes) != NEOX_OK)
                goto oom;
        } else {
            if (cap_put_u32(ctx, &b, e->verb_count) != NEOX_OK ||
                cap_put_u32(ctx, &b, e->point_count) != NEOX_OK ||
                cap_put_u32(ctx, &b, e->fill_rule) != NEOX_OK ||
                nx_buf_put(ctx, &b, e->verbs, e->verb_count) != NEOX_OK ||
                nx_buf_put(ctx, &b, e->points, (size_t)e->point_count * 8) != NEOX_OK)
                goto oom;
        }
    }
    for (i = 0; i < cap->cmd_count; i++) {
        const nx_cmd *c = &cap->cmds[i];
        const float *m = &c->ctm.a;
        if (cap_put_u32(ctx, &b, (uint32_t)c->op) != NEOX_OK)
            goto oom;
        switch (c->op) {
        case NXC_PUSH: {
            uint32_t clip_cap = cap_find(cap, c->clip_id, NX_TYPE_PATH);
            for (j = 0; j < 6; j++)
                if (cap_put_f32(ctx, &b, m[j]) != NEOX_OK) goto oom;
            if (cap_put_f32(ctx, &b, c->opacity) != NEOX_OK ||
                cap_put_u32(ctx, &b, (uint32_t)c->blend) != NEOX_OK ||
                cap_put_u32(ctx, &b, c->flags) != NEOX_OK ||
                cap_put_u32(ctx, &b, clip_cap) != NEOX_OK ||
                cap_put_u32(ctx, &b, (uint32_t)c->owns_layer) != NEOX_OK)
                goto oom;
            break;
        }
        case NXC_POP:
            break;
        case NXC_PATH: {
            uint32_t rc = cap_find(cap, c->res_id, NX_TYPE_PATH);
            if (cap_put_u32(ctx, &b, rc) != NEOX_OK ||
                cap_put_f32(ctx, &b, c->color.r) != NEOX_OK ||
                cap_put_f32(ctx, &b, c->color.g) != NEOX_OK ||
                cap_put_f32(ctx, &b, c->color.b) != NEOX_OK ||
                cap_put_f32(ctx, &b, c->color.a) != NEOX_OK)
                goto oom;
            break;
        }
        case NXC_TEX: {
            uint32_t rc = cap_find(cap, c->res_id, NX_TYPE_TEXTURE);
            const float *f = &c->src.x;
            if (cap_put_u32(ctx, &b, rc) != NEOX_OK) goto oom;
            for (j = 0; j < 8; j++)
                if (cap_put_f32(ctx, &b, f[j]) != NEOX_OK) goto oom;
            if (cap_put_u32(ctx, &b, (uint32_t)c->filter) != NEOX_OK ||
                cap_put_f32(ctx, &b, c->tex_opacity) != NEOX_OK)
                goto oom;
            break;
        }
        case NXC_GLYPHS: {
            uint32_t rc = cap_find(cap, c->res_id, NX_TYPE_TEXTURE);
            if (cap_put_u32(ctx, &b, rc) != NEOX_OK ||
                cap_put_f32(ctx, &b, c->color.r) != NEOX_OK ||
                cap_put_f32(ctx, &b, c->color.g) != NEOX_OK ||
                cap_put_f32(ctx, &b, c->color.b) != NEOX_OK ||
                cap_put_f32(ctx, &b, c->color.a) != NEOX_OK ||
                cap_put_u32(ctx, &b, c->glyph_count) != NEOX_OK)
                goto oom;
            for (j = 0; j < c->glyph_count; j++) {
                const neox_glyph *g = &c->glyphs[j];
                if (cap_put_f32(ctx, &b, g->x) != NEOX_OK ||
                    cap_put_f32(ctx, &b, g->y) != NEOX_OK ||
                    cap_put_u16(ctx, &b, g->sx) != NEOX_OK ||
                    cap_put_u16(ctx, &b, g->sy) != NEOX_OK ||
                    cap_put_u16(ctx, &b, g->sw) != NEOX_OK ||
                    cap_put_u16(ctx, &b, g->sh) != NEOX_OK)
                    goto oom;
            }
            break;
        }
        default:
            if (b.p) nx_free(ctx, b.p, b.cap, 16);
            for (i = 0; i < cap->res_count; i++)
                cap_free_res(ctx, &cap->res[i]);
            if (cap->res) nx_free(ctx, cap->res, (size_t)cap->res_cap * sizeof(nx_cap_res), 16);
            for (i = 0; i < cap->cmd_count; i++)
                if (cap->cmds[i].glyphs)
                    nx_free(ctx, cap->cmds[i].glyphs,
                            (size_t)cap->cmds[i].glyph_count * sizeof(neox_glyph), 16);
            if (cap->cmds) nx_free(ctx, cap->cmds, (size_t)cap->cmd_cap * sizeof(nx_cmd), 16);
            cap->dev->cap = NULL;
            nx_free(ctx, cap, sizeof(neox_capture), 16);
            ctx->last_error = "unknown command during capture serialize";
            return NEOX_ERROR_BACKEND;
        }
    }
    /* 精确大小缓冲移交（free 时可重现 size） */
    exact = (uint8_t *)nx_alloc(ctx, b.len ? b.len : 1, 16);
    if (!exact) goto oom;
    memcpy(exact, b.p, b.len);
    nx_free(ctx, b.p, b.cap, 16);
    for (i = 0; i < cap->res_count; i++)
        cap_free_res(ctx, &cap->res[i]);
    if (cap->res) nx_free(ctx, cap->res, (size_t)cap->res_cap * sizeof(nx_cap_res), 16);
    for (i = 0; i < cap->cmd_count; i++)
        if (cap->cmds[i].glyphs)
            nx_free(ctx, cap->cmds[i].glyphs,
                    (size_t)cap->cmds[i].glyph_count * sizeof(neox_glyph), 16);
    if (cap->cmds) nx_free(ctx, cap->cmds, (size_t)cap->cmd_cap * sizeof(nx_cmd), 16);
    cap->dev->cap = NULL;
    nx_free(ctx, cap, sizeof(neox_capture), 16);
    *out_data = exact;
    *out_size = b.len;
    return NEOX_OK;
oom:
    if (b.p) nx_free(ctx, b.p, b.cap, 16);
    ctx->last_error = "capture buffer allocation failed";
    return NEOX_ERROR_OUT_OF_MEMORY;
}

void neox_capture_free_buffer(neox_context *ctx, uint8_t *data, size_t size)
{
    if (!ctx || !data) return;
    nx_free(ctx, data, size ? size : 1, 16);
}

/* 边界检查的 CAP 读取器 */
typedef struct nx_reader {
    const uint8_t *p;
    size_t len, pos;
    neox_context *ctx;
} nx_reader;

static int rd_bytes(nx_reader *r, void *out, size_t n)
{
    if (r->pos + n > r->len) {
        r->ctx->last_error = "CAP buffer truncated";
        return 0;
    }
    memcpy(out, r->p + r->pos, n);
    r->pos += n;
    return 1;
}
static int rd_u32(nx_reader *r, uint32_t *v) { return rd_bytes(r, v, 4); }
static int rd_f32(nx_reader *r, float *v) { return rd_bytes(r, v, 4); }
static int rd_u16(nx_reader *r, uint16_t *v) { return rd_bytes(r, v, 2); }

neox_result neox_capture_replay(neox_frame *frame, const uint8_t *data, size_t size)
{
    neox_device *dev;
    neox_context *ctx;
    nx_reader r;
    char magic[4];
    uint32_t version, res_count, cmd_count, i;
    uint32_t *map_id = NULL, *map_idx = NULL; /* cap_id → 本地槽索引 */
    neox_result result = NEOX_OK;
    if (!frame || !data || size == 0) return NEOX_ERROR_INVALID_ARGUMENT;
    dev = frame->dev;
    ctx = dev->ctx;
    memset(&r, 0, sizeof(r));
    r.p = data; r.len = size; r.ctx = ctx;
    if (!rd_bytes(&r, magic, 4) || memcmp(magic, "NXCA", 4) != 0)
        NX_ERR(ctx, "not a CAP buffer", NEOX_ERROR_UNSUPPORTED);
    if (!rd_u32(&r, &version) || version != NX_CAP_VERSION)
        NX_ERR(ctx, "CAP version mismatch", NEOX_ERROR_UNSUPPORTED);
    if (!rd_u32(&r, &res_count) || !rd_u32(&r, &cmd_count))
        NX_ERR(ctx, "CAP header truncated", NEOX_ERROR_UNSUPPORTED);
    map_id = (uint32_t *)nx_alloc(ctx, ((size_t)res_count + 1) * 4, 16);
    map_idx = (uint32_t *)nx_alloc(ctx, ((size_t)res_count + 1) * 4, 16);
    if (!map_id || !map_idx) {
        result = NEOX_ERROR_OUT_OF_MEMORY;
        ctx->last_error = "replay map allocation failed";
        goto done;
    }
    /* 重建资源：CAP 内 ID → 本地槽索引 */
    for (i = 0; i < res_count; i++) {
        uint32_t type, cap_id;
        if (!rd_u32(&r, &type) || !rd_u32(&r, &cap_id)) {
            result = NEOX_ERROR_UNSUPPORTED;
            ctx->last_error = "CAP resource table truncated";
            goto done;
        }
        map_id[i] = cap_id;
        if (type == NX_TYPE_TEXTURE) {
            uint32_t w, h, fmt, nbytes;
            float *px;
            if (!rd_u32(&r, &w) || !rd_u32(&r, &h) || !rd_u32(&r, &fmt) ||
                !rd_u32(&r, &nbytes))
                goto truncated;

            if (nbytes != w * h * 16 || w == 0 || h == 0)
                goto badres;
            px = (float *)nx_alloc(ctx, nbytes, 16);
            if (!px) { result = NEOX_ERROR_OUT_OF_MEMORY; goto done; }
            if (!rd_bytes(&r, px, nbytes)) {
                nx_free(ctx, px, nbytes, 16);
                goto truncated;
            }
            {
                neox_texture t;
                result = soft_tex_from_linear(dev, w, h,
                                              (neox_pixel_format)fmt, px, &t);
                nx_free(ctx, px, nbytes, 16);
                if (result != NEOX_OK) goto done;
                map_idx[i] = nx_handle_index(t.id);
            }
        } else if (type == NX_TYPE_PATH) {
            uint32_t vc, pc, rule, idx;
            uint8_t *vb;
            float *pt;
            neox_path ph;
            if (!rd_u32(&r, &vc) || !rd_u32(&r, &pc) || !rd_u32(&r, &rule))
                goto truncated;

            vb = (uint8_t *)nx_alloc(ctx, vc ? vc : 1, 4);
            pt = (float *)nx_alloc(ctx, ((size_t)pc * 8) ? (size_t)pc * 8 : 4, 16);
            if (!vb || !pt) {
                if (vb) nx_free(ctx, vb, vc ? vc : 1, 4);
                if (pt) nx_free(ctx, pt, (size_t)pc * 8, 16);
                result = NEOX_ERROR_OUT_OF_MEMORY;
                goto done;
            }
            if (!rd_bytes(&r, vb, vc) || !rd_bytes(&r, pt, (size_t)pc * 8)) {
                nx_free(ctx, vb, vc ? vc : 1, 4);
                nx_free(ctx, pt, (size_t)pc * 8, 16);
                goto truncated;
            }
            result = soft_path_from_copy(dev, vb, vc, pt, pc,
                                         (neox_fill_rule)rule, &ph);
            nx_free(ctx, vb, vc ? vc : 1, 4);
            nx_free(ctx, pt, (size_t)pc * 8, 16);
            if (result != NEOX_OK) goto done;
            idx = nx_handle_index(ph.id);
            map_idx[i] = idx;
        } else {
            goto badres;
        }
    }
    /* 执行命令流 */
    for (i = 0; i < cmd_count; i++) {
        uint32_t op;
        if (!rd_u32(&r, &op)) goto truncated;
        switch ((nx_op)op) {
        case NXC_PUSH: {
            neox_affine m;
            float opacity;
            uint32_t blend, flags, clip_cap, owns;
            int32_t clip_idx = -1;
            uint32_t k;
            for (k = 0; k < 6; k++)
                if (!rd_f32(&r, &((float *)&m.a)[k])) goto truncated;
            if (!rd_f32(&r, &opacity) || !rd_u32(&r, &blend) ||
                !rd_u32(&r, &flags) || !rd_u32(&r, &clip_cap) ||
                !rd_u32(&r, &owns))
                goto truncated;
            (void)owns; /* owns_layer 按语义重算，不信任录制值 */
            if (clip_cap != NX_CAP_NO_RES) {
                uint32_t j2;
                clip_idx = -1;
                for (j2 = 0; j2 < res_count; j2++)
                    if (map_id[j2] == clip_cap) { clip_idx = (int32_t)map_idx[j2]; break; }
                if (clip_idx < 0) goto missing;
            }
            result = emit_push(frame, m, opacity, (neox_blend)blend, flags,
                               clip_idx, 0);
            if (result != NEOX_OK) goto done;
            break;
        }
        case NXC_POP:
            result = emit_pop(frame);
            if (result != NEOX_OK) goto done;
            break;
        case NXC_PATH: {
            uint32_t rc, j2;
            neox_color col;
            int32_t idx = -1;
            if (!rd_u32(&r, &rc) || !rd_f32(&r, &col.r) || !rd_f32(&r, &col.g) ||
                !rd_f32(&r, &col.b) || !rd_f32(&r, &col.a))
                goto truncated;
            for (j2 = 0; j2 < res_count; j2++)
                if (map_id[j2] == rc) { idx = (int32_t)map_idx[j2]; break; }
            if (idx < 0) goto missing;
            result = emit_path(frame, idx, 0, col);
            if (result != NEOX_OK) goto done;
            break;
        }
        case NXC_TEX: {
            uint32_t rc, j2, filter;
            neox_rect src, dst;
            float opacity;
            int32_t idx = -1;
            if (!rd_u32(&r, &rc)) goto truncated;
            if (!rd_f32(&r, &src.x) || !rd_f32(&r, &src.y) ||
                !rd_f32(&r, &src.w) || !rd_f32(&r, &src.h) ||
                !rd_f32(&r, &dst.x) || !rd_f32(&r, &dst.y) ||
                !rd_f32(&r, &dst.w) || !rd_f32(&r, &dst.h) ||
                !rd_u32(&r, &filter) || !rd_f32(&r, &opacity))
                goto truncated;
            for (j2 = 0; j2 < res_count; j2++)
                if (map_id[j2] == rc) { idx = (int32_t)map_idx[j2]; break; }
            if (idx < 0) goto missing;
            result = emit_tex(frame, idx, 0, src, dst, (neox_filter)filter, opacity);
            if (result != NEOX_OK) goto done;
            break;
        }
        case NXC_GLYPHS: {
            uint32_t rc, j2, count, k;
            neox_color col;
            neox_glyph *gl;
            int32_t idx = -1;
            if (!rd_u32(&r, &rc) || !rd_f32(&r, &col.r) || !rd_f32(&r, &col.g) ||
                !rd_f32(&r, &col.b) || !rd_f32(&r, &col.a) || !rd_u32(&r, &count))
                goto truncated;
            gl = (neox_glyph *)nx_alloc(ctx,
                     ((size_t)count * sizeof(neox_glyph)) ? (size_t)count * sizeof(neox_glyph) : 16, 16);
            if (!gl) { result = NEOX_ERROR_OUT_OF_MEMORY; goto done; }
            for (k = 0; k < count; k++) {
                if (!rd_f32(&r, &gl[k].x) || !rd_f32(&r, &gl[k].y) ||
                    !rd_u16(&r, &gl[k].sx) || !rd_u16(&r, &gl[k].sy) ||
                    !rd_u16(&r, &gl[k].sw) || !rd_u16(&r, &gl[k].sh)) {
                    nx_free(ctx, gl, (size_t)count * sizeof(neox_glyph), 16);
                    goto truncated;
                }
            }
            for (j2 = 0; j2 < res_count; j2++)
                if (map_id[j2] == rc) { idx = (int32_t)map_idx[j2]; break; }
            if (idx < 0) {
                nx_free(ctx, gl, (size_t)count * sizeof(neox_glyph), 16);
                goto missing;
            }
            result = emit_glyphs(frame, idx, 0, gl, count, col);
            nx_free(ctx, gl, (size_t)count * sizeof(neox_glyph), 16);
            if (result != NEOX_OK) goto done;
            break;
        }
        default:
            goto badres;
        }
    }
    goto done;
truncated:
    result = NEOX_ERROR_UNSUPPORTED;
    ctx->last_error = "CAP buffer truncated";
    goto done;
missing:
    result = NEOX_ERROR_UNSUPPORTED;
    ctx->last_error = "CAP references a resource not packed in the buffer";
    goto done;
badres:
    result = NEOX_ERROR_UNSUPPORTED;
    ctx->last_error = "CAP resource entry invalid";
done:
    if (map_id) nx_free(ctx, map_id, ((size_t)res_count + 1) * 4, 16);
    if (map_idx) nx_free(ctx, map_idx, ((size_t)res_count + 1) * 4, 16);
    return result;
}
