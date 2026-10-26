/* Retained frontend: stable scene ownership, tree mutation and command expansion. */
#include "internal.h"

/* ------------------------------------------------------------------ 场景（保留模式，规范 §10.2-1）
 *
 * 脏区追踪说明（增量执行预留）：node_set_* 会设置节点自身的属性脏位，并把
 * "子树脏" 位沿父链传播到根。当前执行路径仍是粗粒度的整体重绘（scene_render
 * 每次全量遍历）。真正增量执行还需要旧/新包围盒、资源版本依赖与合成影响范围；
 * 当前脏位不构成增量执行实现或验收证据。 */
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
        nx_table_release(ctx, &dev->scenes, sidx);
        ctx->last_error = "scene root allocation failed";
        return (neox_scene){ 0 };
    }
    sc = &((nx_scene_slot *)dev->scenes.slots)[sidx];
    root = &((nx_node_slot *)dev->nodes.slots)[nidx];
    root->scene_id = nx_make_handle(dev->id, NX_TYPE_SCENE, sidx, sc->gen);
    root->kind = NEOX_NODE_GROUP;
    root->transform = nx_affine_identity();
    root->blend = NEOX_BLEND_SRC_OVER;
    root->opacity = 1.0f;
    root->visible = 1;
    root->parent = nidx;
    sc->root = nidx;
    return (neox_scene){ nx_make_handle(dev->id, NX_TYPE_SCENE, sidx, sc->gen) };
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
    nx_table_release(ctx, &dev->nodes, idx);
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
    for (uint32_t i = 0; i < dev->nodes.count; i++) {
        nx_node_slot *n = &((nx_node_slot *)dev->nodes.slots)[i];
        if (n->in_use && n->scene_id == scene.id && n->parent == i)
            node_free_subtree(dev, i);
    }
    nx_table_release(ctx, &dev->scenes, sidx);
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
        if (nx_color_is_bad(desc->path.paint.color))
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
        r = nx_validate_glyphs(dev, t, desc->glyphs.glyphs, desc->glyphs.count,
                            desc->glyphs.paint.color, &desc->glyphs.style);
        if (r != NEOX_OK) return r;
        if (!desc->glyphs.glyphs || desc->glyphs.count == 0)
            NX_ERR(ctx, "glyphs empty", NEOX_ERROR_INVALID_ARGUMENT);
        if (nx_color_is_bad(desc->glyphs.paint.color))
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
    n->scene_id = scene.id;
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
            nx_table_release(ctx, &dev->nodes, idx);
            NX_ERR(ctx, "glyph array allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
        }
        memcpy(copy, desc->glyphs.glyphs,
               (size_t)desc->glyphs.count * sizeof(neox_glyph));
        n->u.glyphs.atlas_idx = t;
        n->u.glyphs.atlas_id = desc->glyphs.atlas.id;
        n->u.glyphs.glyphs = copy;
        n->u.glyphs.count = desc->glyphs.count;
        n->u.glyphs.paint = desc->glyphs.paint;
        n->u.glyphs.style = desc->glyphs.style;
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
        nx_table_release(ctx, &dev->nodes, idx);
        NX_ERR(ctx, "child list allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    }
    root->children = nc;
    root->children[root->child_count++] = idx;
    node_mark_dirty(dev, root_idx, NX_DIRTY_CHILDREN);
    *out_node = (neox_node){ nx_make_handle(dev->id, NX_TYPE_NODE, idx, n->gen) };
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
    p = &((nx_node_slot *)dev->nodes.slots)[pi];
    c = &((nx_node_slot *)dev->nodes.slots)[ci];
    if (p->scene_id != c->scene_id)
        NX_ERR(ctx, "nodes belong to different scenes", NEOX_ERROR_OWNERSHIP_VIOLATION);
    if (p->kind != NEOX_NODE_GROUP)
        NX_ERR(ctx, "parent must be a group", NEOX_ERROR_INVALID_ARGUMENT);
    for (uint32_t walk = pi;;) {
        nx_node_slot *wn = &((nx_node_slot *)dev->nodes.slots)[walk];
        if (walk == ci) NX_ERR(ctx, "append_child would create a cycle", NEOX_ERROR_INVALID_ARGUMENT);
        if (wn->parent == walk) break;
        walk = wn->parent;
    }
    /* Reserve before detaching so allocation failure preserves the tree. */
    uint32_t *nc = (uint32_t *)nx_array_grow(ctx, p->children, 4,
                                            &p->child_cap, p->child_count + 1);
    if (!nc) NX_ERR(ctx, "child list allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    p->children = nc;
    if (c->parent != ci) {
        nx_node_slot *op = &((nx_node_slot *)dev->nodes.slots)[c->parent];
        uint32_t out = 0;
        for (i = 0; i < op->child_count; i++)
            if (op->children[i] != ci) op->children[out++] = op->children[i];
        op->child_count = out;
        node_mark_dirty(dev, c->parent, NX_DIRTY_CHILDREN);
    }
    p->children[p->child_count++] = ci;
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
    if (nx_color_is_bad(p->color)) {
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
        r = nx_emit_push(fr, ctm, n->opacity, n->blend, 0, clip_idx, clip_id);
        if (r != NEOX_OK) return r;
        for (i = 0; i < n->child_count; i++) {
            r = scene_emit(fr, n->children[i], ctm);
            if (r != NEOX_OK) return r;
        }
        return nx_emit_pop(fr);
    }
    /* 叶子：push（transform/opacity/blend）→ 绘制 → pop */
    r = nx_emit_push(fr, ctm, n->opacity, n->blend, 0, -1, 0);
    if (r != NEOX_OK) return r;
    switch (n->kind) {
    case NEOX_NODE_PATH: {
        uint32_t p;
        r = nx_resolve(dev, n->u.path.path_id, NX_TYPE_PATH, &p);
        if (r != NEOX_OK) return r;
        r = nx_emit_path(fr, (int32_t)p, n->u.path.path_id, n->u.path.paint.color);
        break;
    }
    case NEOX_NODE_IMAGE: {
        uint32_t t;
        r = nx_resolve(dev, n->u.image.tex_id, NX_TYPE_TEXTURE, &t);
        if (r != NEOX_OK) return r;
        r = neox_draw_texture(fr, (neox_texture){n->u.image.tex_id}, n->u.image.src,
                     n->u.image.dst, n->u.image.filter, 1.0f);
        break;
    }
    case NEOX_NODE_GLYPHS: {
        uint32_t t;
        r = nx_resolve(dev, n->u.glyphs.atlas_id, NX_TYPE_TEXTURE, &t);
        if (r != NEOX_OK) return r;
        r = neox_draw_glyphs_ex(fr, (neox_texture){n->u.glyphs.atlas_id},
                n->u.glyphs.glyphs, n->u.glyphs.count, &n->u.glyphs.paint,
                &n->u.glyphs.style);
        break;
    }
    default:
        r = NEOX_ERROR_BACKEND;
    }
    if (r != NEOX_OK) return r;
    return nx_emit_pop(fr);
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
        if (((nx_node_slot *)dev->nodes.slots)[root_idx].scene_id != scene.id) {
            dev->ctx->last_error = "root node belongs to another scene";
            return NEOX_ERROR_OWNERSHIP_VIOLATION;
        }
    }
    neox_frame staged = *frame;
    neox_capture *capture = dev->cap;
    staged.cmds = NULL; staged.cmd_count = staged.cmd_cap = staged.draw_calls = 0;
    dev->cap = NULL;
    r = scene_emit(&staged, root_idx, frame->ctm_stack[frame->ctm_count - 1]);
    dev->cap = capture;
    return nx_frame_finish_staged(frame, &staged, capture, r);
}
