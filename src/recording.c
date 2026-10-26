/* Command recording, state validation, resource pins and atomic publication. */
#include "internal.h"

/* ------------------------------------------------------------------ 命令发射 */


/* Pins belong to recorded commands, then transfer with them to submission.
 * Recording a resource and releasing its public handle before frame_end is safe. */
neox_result nx_cmd_ref(neox_device *dev, const nx_cmd *c, int acquire)
{
    uint32_t type, idx;
    if (c->op == NXC_PUSH && c->clip_idx >= 0) {
        type = NX_TYPE_PATH; idx = (uint32_t)c->clip_idx;
    } else if (c->op == NXC_PATH) {
        type = NX_TYPE_PATH; idx = (uint32_t)c->res_idx;
    } else if (c->op == NXC_TEX || c->op == NXC_GLYPHS) {
        type = NX_TYPE_TEXTURE; idx = (uint32_t)c->res_idx;
    } else return NEOX_OK;
    uint32_t *refs = type == NX_TYPE_PATH
        ? &((nx_path_slot *)dev->paths.slots)[idx].refcount
        : &((nx_tex_slot *)dev->textures.slots)[idx].refcount;
    /* Check both pins before changing either. A texture version can also be
     * shared by captures, so its reference count has a different lifetime. */
    if (acquire && (*refs == UINT32_MAX ||
        (type == NX_TYPE_TEXTURE && c->texture && c->texture->refs == UINT32_MAX)))
        NX_ERR(dev->ctx, "resource reference count exhausted", NEOX_ERROR_OUT_OF_MEMORY);
    if (type==NX_TYPE_TEXTURE && c->texture) {
        if(acquire)c->texture->refs++;
        else nx_tex_data_release(dev->ctx,c->texture);
    }
    if (!acquire) nx_unref_index(dev, type, idx);
    else (*refs)++;
    return NEOX_OK;
}

/* 追加命令到帧列表；CAP 激活时同步录制 */
static neox_result frame_append_cmd(neox_frame *fr, const nx_cmd *c)
{
    neox_device *dev = fr->dev;
    neox_context *ctx = dev->ctx;
    if (fr->cmd_count == UINT32_MAX)
        NX_ERR(ctx, "command count exhausted", NEOX_ERROR_OUT_OF_MEMORY);
    nx_cmd *nc = (nx_cmd *)nx_array_grow(ctx, fr->cmds, sizeof(nx_cmd),
                                         &fr->cmd_cap, fr->cmd_count + 1);
    if (!nc) NX_ERR(ctx, "command list allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    fr->cmds = nc;
    fr->cmds[fr->cmd_count] = *c;
    if(c->op==NXC_TEX || c->op==NXC_GLYPHS)
        fr->cmds[fr->cmd_count].texture=((nx_tex_slot *)dev->textures.slots)[c->res_idx].data;
    neox_result pinned = nx_cmd_ref(dev, &fr->cmds[fr->cmd_count], 1);
    if (pinned != NEOX_OK) return pinned;
#if NEOX_ENABLE_CAPTURE
    if (dev->cap) {
        /* CAP 录制必须在计数自增前取已写入的命令槽 */
        neox_result cr = nx_capture_record(dev->cap, &fr->cmds[fr->cmd_count]);
        if (cr != NEOX_OK) {
            nx_cmd_ref(dev, &fr->cmds[fr->cmd_count], 0);
            ctx->last_error = "capture recording failed";
            return cr;
        }
    }
#endif
    fr->cmd_count++;
    if (c->op == NXC_PATH || c->op == NXC_TEX || c->op == NXC_GLYPHS)
        fr->draw_calls++;
    return NEOX_OK;
}

/* 释放命令列表内部拥有的 glyphs 拷贝 */
void nx_commands_free(neox_context *ctx, nx_cmd *cmds, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (cmds[i].op == NXC_GLYPHS && cmds[i].glyphs)
            nx_free(ctx, cmds[i].glyphs,
                    (size_t)cmds[i].glyph_count * sizeof(neox_glyph), 16);
    }
}

/* PUSH：owns_layer 的判定与帧期/执行期必须一致（组不透明度或裁剪 → 隔离层，§7） */
static int push_needs_layer(float opacity, int32_t clip_idx)
{
    return opacity < 1.0f || clip_idx >= 0;
}

neox_result nx_emit_push(neox_frame *fr, neox_affine ctm, float opacity,
                             neox_blend blend, uint32_t flags,
                             int32_t clip_idx, uint64_t clip_id)
{
    neox_device *dev = fr->dev;
    neox_context *ctx = dev->ctx;
    nx_cmd c;
    /* All frontends reach this boundary, including precomposed scene matrices.
     * Reject before allocating masks, publishing commands or touching CAP. */
    if (!nx_affine_is_finite(ctm))
        NX_ERR(ctx, "composed transform exceeds numeric range", NEOX_ERROR_INVALID_ARGUMENT);
    /* The mean is bounded by twice the largest coefficient. Keep ordinary
     * recording free of the distance-field square root. */
    if ((fabsf(ctm.a) > FLT_MAX*0.5f || fabsf(ctm.b) > FLT_MAX*0.5f ||
         fabsf(ctm.c) > FLT_MAX*0.5f || fabsf(ctm.d) > FLT_MAX*0.5f) &&
        !isfinite(nx_glyph_scale_factor(ctm)))
        NX_ERR(ctx, "transform scale exceeds numeric range", NEOX_ERROR_INVALID_ARGUMENT);
    memset(&c, 0, sizeof(c));
    c.op = NXC_PUSH;
    c.ctm = ctm;
    c.opacity = opacity;
    c.blend = blend;
    c.flags = flags;
    c.clip_idx = clip_idx;
    c.clip_id = clip_id;
    c.owns_layer = push_needs_layer(opacity, clip_idx);
    neox_result r = frame_append_cmd(fr, &c);
    if (r != NEOX_OK) nx_commands_free(ctx,&c,1);
    return r;
}

neox_result nx_emit_pop(neox_frame *fr)
{
    nx_cmd c;
    memset(&c, 0, sizeof(c));
    c.op = NXC_POP;
    return frame_append_cmd(fr, &c);
}

neox_result nx_emit_path(neox_frame *fr, int32_t path_idx, uint64_t path_id,
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
                               neox_color color, const neox_glyph_style *style)
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
    if (style) {   /* NULL = COVERAGE 默认 */
        c.atlas_mode = style->atlas_mode;
        /* dist_range 0 = 契约缺省 4；命令流存生效值，三后端与 CAP 一致 */
        c.dist_range = style->dist_range_px > 0.0f ? style->dist_range_px : 4.0f;
        c.softness = style->softness;
    }
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

void nx_frame_destroy(neox_device *dev, neox_frame *fr)
{
    neox_context *ctx = dev->ctx;
    if (!fr) return;
    nx_capture_frame_rollback(fr);
    for (uint32_t i = 0; i < fr->cmd_count; i++) nx_cmd_ref(dev, &fr->cmds[i], 0);
    nx_commands_free(ctx, fr->cmds, fr->cmd_count);
    if (fr->cmds) nx_free(ctx, fr->cmds, (size_t)fr->cmd_cap * sizeof(nx_cmd), 16);
    if (fr->ctm_stack) nx_free(ctx, fr->ctm_stack, (size_t)fr->ctm_cap * sizeof(neox_affine), 16);
    if (fr->base)
        nx_free(ctx, fr->base, (size_t)fr->width * fr->height * 4 * sizeof(float), 16);
    if (fr->backend_base.release) fr->backend_base.release(fr->backend_base.data);
    nx_free(ctx, fr, sizeof(neox_frame), 16);
}

void neox_frame_cancel(neox_frame *frame)
{
    if (!frame) return;
    neox_device *dev=frame->dev;
    if (dev->active_frame==frame) dev->active_frame=NULL;
    nx_frame_destroy(dev,frame);
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
    if (nx_device_check_live(dev) != NEOX_OK) return NEOX_ERROR_DEVICE_LOST;
    if (target->kind != dev->kind)
        NX_ERR(ctx, "target backend kind mismatch", NEOX_ERROR_INVALID_ARGUMENT);
    if (target->clear && nx_color_is_bad(*target->clear))
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
        if (fr->backend_base.release) fr->backend_base.release(fr->backend_base.data);
        nx_free(ctx, fr, sizeof(neox_frame), 16);
        return dev->loss_reason ? NEOX_ERROR_DEVICE_LOST : r;
    }
    /* 基底 CTM（恒等） */
    {
        neox_affine *st = (neox_affine *)nx_alloc(ctx, sizeof(neox_affine) * 8, 16);
        if (!st) {
            nx_frame_destroy(dev, fr);
            NX_ERR(ctx, "frame stack allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
        }
        fr->ctm_stack = st;
        fr->ctm_cap = 8;
        fr->ctm_stack[0] = nx_affine_identity();
        fr->ctm_count = 1;
    }
    nx_capture_frame_begin(fr);
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
    if (frame->ctm_count == UINT32_MAX)
        NX_ERR(ctx, "state stack count exhausted", NEOX_ERROR_OUT_OF_MEMORY);
    ctm = nx_affine_mul(frame->ctm_stack[frame->ctm_count - 1], state->transform);
    {
        neox_affine *nst = (neox_affine *)nx_array_grow(ctx, frame->ctm_stack,
                            sizeof(neox_affine), &frame->ctm_cap,
                            frame->ctm_count + 1);
        if (!nst) NX_ERR(ctx, "state stack allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
        frame->ctm_stack = nst;
        frame->ctm_stack[frame->ctm_count++] = ctm;
    }
    r = nx_emit_push(frame, ctm, state->opacity, state->blend, state->flags,
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
    neox_result r = nx_emit_pop(frame);
    if (r == NEOX_OK) frame->ctm_count--;
    return r;
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
    if (nx_color_is_bad(paint->color))
        NX_ERR(ctx, "paint color contains NaN/Inf", NEOX_ERROR_INVALID_ARGUMENT);
    r = nx_resolve(frame->dev, path.id, NX_TYPE_PATH, &idx);
    if (r != NEOX_OK) return r;
    return nx_emit_path(frame, (int32_t)idx, path.id, paint->color);
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
    if (!isfinite(src.x+src.w) || !isfinite(src.y+src.h) ||
        !isfinite(dst.x+dst.w) || !isfinite(dst.y+dst.h))
        NX_ERR(ctx, "rectangle endpoint exceeds numeric range", NEOX_ERROR_INVALID_ARGUMENT);
    r = nx_resolve(frame->dev, tex.id, NX_TYPE_TEXTURE, &idx);
    if (r != NEOX_OK) return r;
    if (((nx_tex_slot *)frame->dev->textures.slots)[idx].format==NEOX_FORMAT_RGBA8_DATA)
        NX_ERR(ctx, "data texture requires a data draw operation", NEOX_ERROR_INVALID_ARGUMENT);
    return emit_tex(frame, (int32_t)idx, tex.id, src, dst, filter, opacity);
}

neox_result nx_validate_glyphs(neox_device *dev, uint32_t idx,
        const neox_glyph *glyphs, uint32_t count, neox_color color,
        const neox_glyph_style *style)
{
    neox_context *ctx = dev->ctx;
    const nx_tex_slot *at = &((const nx_tex_slot *)dev->textures.slots)[idx];
    size_t glyph_bytes;
    if (!nx_size_mul(count, sizeof(neox_glyph), &glyph_bytes))
        NX_ERR(ctx, "glyph byte size overflow", NEOX_ERROR_INVALID_ARGUMENT);
    if ((!glyphs && count) || nx_color_is_bad(color))
        NX_ERR(ctx, "invalid glyphs or paint", NEOX_ERROR_INVALID_ARGUMENT);
    const neox_glyph_style zero = {0};
    if (!style) style = &zero;
    {
        const nx_tex_slot *ts;
        if ((unsigned)style->atlas_mode > NEOX_ATLAS_MSDF)
            NX_ERR(ctx, "invalid atlas mode", NEOX_ERROR_INVALID_ARGUMENT);
        if (nx_float_bad(style->dist_range_px) || style->dist_range_px < 0.0f)
            NX_ERR(ctx, "dist_range out of range or NaN",
                   NEOX_ERROR_INVALID_ARGUMENT);
        if (nx_float_bad(style->softness) || style->softness < 0.0f)
            NX_ERR(ctx, "softness out of range or NaN",
                   NEOX_ERROR_INVALID_ARGUMENT);
        /* 格式匹配契约（§8.2）：COVERAGE/SDF → A8；MSDF → RGBA8_DATA */
        ts = &((const nx_tex_slot *)dev->textures.slots)[idx];
        if (style->atlas_mode == NEOX_ATLAS_MSDF) {
            if (ts->format != NEOX_FORMAT_RGBA8_DATA)
                NX_ERR(ctx, "MSDF atlas must be RGBA8_DATA",
                       NEOX_ERROR_INVALID_ARGUMENT);
        } else if (ts->format != NEOX_FORMAT_A8) {
            NX_ERR(ctx, "COVERAGE/SDF atlas must be A8",
                   NEOX_ERROR_INVALID_ARGUMENT);
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        const neox_glyph *g = &glyphs[i];
        if (nx_float_bad(g->x) || nx_float_bad(g->y) ||
            (uint32_t)g->sx + g->sw > at->width ||
            (uint32_t)g->sy + g->sh > at->height)
            NX_ERR(ctx, "glyph outside atlas or nonfinite", NEOX_ERROR_INVALID_ARGUMENT);
    }
    return NEOX_OK;
}

neox_result neox_draw_glyphs(neox_frame *frame, neox_texture atlas,
                             const neox_glyph *glyphs, uint32_t count,
                             const neox_paint *paint)
{
    /* COVERAGE 默认转发（行为与 v0 前完全一致） */
    return neox_draw_glyphs_ex(frame, atlas, glyphs, count, paint, NULL);
}

neox_result neox_draw_glyphs_ex(neox_frame *frame, neox_texture atlas,
                                const neox_glyph *glyphs, uint32_t count,
                                const neox_paint *paint,
                                const neox_glyph_style *style)
{
    neox_context *ctx;
    uint32_t idx;
    neox_result r;
    if (!frame) return NEOX_ERROR_INVALID_ARGUMENT;
    ctx = frame->dev->ctx;
    if (!paint || (!glyphs && count))
        NX_ERR(ctx, "paint/glyphs is NULL", NEOX_ERROR_INVALID_ARGUMENT);
    if (nx_color_is_bad(paint->color))
        NX_ERR(ctx, "paint color contains NaN/Inf", NEOX_ERROR_INVALID_ARGUMENT);
    r = nx_resolve(frame->dev, atlas.id, NX_TYPE_TEXTURE, &idx);
    if (r != NEOX_OK) return r;
    r = nx_validate_glyphs(frame->dev, idx, glyphs, count, paint->color, style);
    if (r != NEOX_OK) return r;
    if (count == 0) return NEOX_OK;
    return emit_glyphs(frame, (int32_t)idx, atlas.id, glyphs, count,
                       paint->color, style);
}

/* ------------------------------------------------------------------ submission 组装与执行 */

void nx_sub_release_payload(neox_device *dev, nx_sub_slot *s)
{
    neox_context *ctx = dev->ctx;
    uint32_t i;
    for (i = 0; i < s->cmd_count; i++) nx_cmd_ref(dev, &s->cmds[i], 0);
    nx_commands_free(ctx, s->cmds, s->cmd_count);
    if (s->cmds) nx_free(ctx, s->cmds, (size_t)s->cmd_cap * sizeof(nx_cmd), 16);
    s->cmds = NULL;
    s->cmd_count = s->cmd_cap = 0;
    if (s->accum)
        nx_free(ctx, s->accum, (size_t)s->width * s->height * 4 * sizeof(float), 16);
    s->accum = NULL;
    if (s->backend_base.release) s->backend_base.release(s->backend_base.data);
    memset(&s->backend_base,0,sizeof(s->backend_base));
}

neox_result neox_frame_end(neox_frame *frame, neox_submission *out_sub)
{
    neox_device *dev;
    neox_context *ctx;
    nx_sub_slot *sub;
    uint32_t sidx, *np;
    neox_result r;
    if (!frame) return NEOX_ERROR_INVALID_ARGUMENT;
    dev = frame->dev;
    ctx = dev->ctx;
    if (!out_sub) {
        ctx->last_error = "out_sub is NULL";
        return NEOX_ERROR_INVALID_ARGUMENT;
    }
    *out_sub = (neox_submission){ 0 };
    if (nx_device_check_live(dev) != NEOX_OK) return NEOX_ERROR_DEVICE_LOST;
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
    sub->status = NEOX_SUBMISSION_PENDING;
    /* 先做所有可能失败的装配（引用计数、pending 入队），成功后才移交载荷：
     * 失败路径 frame 保持完整、可重试（§10.1 原子性） */
    sub->target = frame->target;
    sub->surface = frame->surface;
    sub->target_stride = frame->target_stride;
    sub->width = frame->width;
    sub->height = frame->height;
    sub->report.flags = 0; /* backend reports explicitly selected approximations */
    sub->report.draw_calls = frame->draw_calls;
    /* report.algorithm 由各后端 execute 设置（Soft=SCANLINE，GL=TRIANGLE） */
    np = (uint32_t *)nx_array_grow(ctx, dev->pending, 4, &dev->pending_cap,
                                   dev->pending_count + 1);
    if (!np) {
        ctx->last_error = "pending queue allocation failed";
        r = NEOX_ERROR_OUT_OF_MEMORY;
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
    sub->backend_base = frame->backend_base;
    memset(&frame->backend_base,0,sizeof(frame->backend_base));
    frame->capture_owner=NULL; /* frame_end publishes captured commands too */
    /* frame 随提交销毁（净增持久节点恒为零） */
    dev->active_frame = NULL;
    nx_frame_destroy(dev, frame);
    *out_sub = (neox_submission){
        nx_make_handle(dev->id, NX_TYPE_SUB, sidx, sub->gen) };
    return NEOX_OK;
fail:
    /* 原子性：提交失败，目标与本帧资源状态保持调用前一致（§10.1）。
     * frame 保持存活可重试；sub 槽位清空归还。 */
    nx_sub_release_payload(dev, sub);
    nx_table_release(ctx, &dev->subs, sidx);
    return r;
}


neox_result nx_frame_finish_staged(neox_frame *frame, neox_frame *staged,
                                      neox_capture *cap, neox_result result)
{
    neox_device *dev=frame->dev; neox_context *ctx=dev->ctx;
    if (result==NEOX_OK && staged->cmd_count) {
        if (staged->cmd_count > UINT32_MAX-frame->cmd_count) result=NEOX_ERROR_OUT_OF_MEMORY;
        else {
            nx_cmd *cmds=(nx_cmd *)nx_array_grow(ctx,frame->cmds,sizeof(nx_cmd),
                &frame->cmd_cap,frame->cmd_count+staged->cmd_count);
            if (!cmds) result=NEOX_ERROR_OUT_OF_MEMORY;
            else frame->cmds=cmds;
        }
#if NEOX_ENABLE_CAPTURE
        if (result==NEOX_OK && cap) {
            result = nx_capture_record_batch(cap,staged->cmds,staged->cmd_count);
        }
#else
        (void)cap;
#endif
        if (result==NEOX_OK) {
            memcpy(frame->cmds+frame->cmd_count,staged->cmds,(size_t)staged->cmd_count*sizeof(nx_cmd));
            frame->cmd_count+=staged->cmd_count; frame->draw_calls+=staged->draw_calls;
            staged->cmd_count=0; /* transfer payloads and resource pins */
        }
    }
    for (uint32_t i=0;i<staged->cmd_count;i++) nx_cmd_ref(dev,&staged->cmds[i],0);
    nx_commands_free(ctx,staged->cmds,staged->cmd_count);
    if (staged->cmds) nx_free(ctx,staged->cmds,(size_t)staged->cmd_cap*sizeof(nx_cmd),16);
    return result;
}
