/* ===========================================================================
 * core.c — NeoX 核心：context、分配器包装、错误细节、句柄、槽位表
 *
 * 语义引用：docs/semantics-v0.md §5（颜色）、§9（确定性）、§10.1（失败语义）。
 * 本文件不依赖任何后端；Soft 后端在 soft.c。
 * =========================================================================*/
#include "internal.h"

/* ------------------------------------------------------------------ 系统分配器（desc.allocator 为 NULL 时使用，规范：freestanding 必须注入） */

static void *sys_alloc(void *user, size_t size, size_t align)
{
    (void)user; (void)align;
    return malloc(size);
}
static void sys_free(void *user, void *ptr, size_t size, size_t align)
{
    (void)user; (void)size; (void)align;
    free(ptr);
}

/* ------------------------------------------------------------------ 分配包装（memory_budget 约束，规范 neox_context_desc） */

void *nx_alloc(neox_context *ctx, size_t size, size_t align)
{
    if (size == 0) size = 1;
    if (ctx->budget != 0 && ctx->used + size > ctx->budget) return NULL;
    void *p = ctx->alloc.alloc(ctx->alloc.user, size, align);
    if (!p) return NULL;
    ctx->used += size;
    if (ctx->used > ctx->used_peak) ctx->used_peak = ctx->used;
    return p;
}

void nx_free(neox_context *ctx, void *ptr, size_t size, size_t align)
{
    if (!ptr) return;
    if (size == 0) size = 1; /* 与 nx_alloc 的 0→1 映射对称，保证预算记账一致 */
    ctx->used -= size;
    ctx->alloc.free(ctx->alloc.user, ptr, size, align);
}

neox_result nx_buf_reserve(neox_context *ctx, nx_buf *b, size_t add)
{
    if (b->len + add <= b->cap) return NEOX_OK;
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < b->len + add) ncap *= 2;
    uint8_t *np = (uint8_t *)nx_alloc(ctx, ncap, 16);
    if (!np) return NEOX_ERROR_OUT_OF_MEMORY;
    if (b->p) {
        memcpy(np, b->p, b->len);
        nx_free(ctx, b->p, b->cap, 16);
    }
    b->p = np;
    b->cap = ncap;
    return NEOX_OK;
}

neox_result nx_buf_put(neox_context *ctx, nx_buf *b, const void *data, size_t n)
{
    neox_result r = nx_buf_reserve(ctx, b, n);
    if (r != NEOX_OK) return r;
    memcpy(b->p + b->len, data, n);
    b->len += n;
    return NEOX_OK;
}

/* ------------------------------------------------------------------ 槽位表 */

neox_result nx_table_init(neox_context *ctx, nx_table *t, uint32_t elem_size,
                          uint32_t initial_cap)
{
    memset(t, 0, sizeof(*t));
    t->elem_size = elem_size;
    t->slots = nx_alloc(ctx, (size_t)elem_size * initial_cap, 16);
    if (!t->slots) return NEOX_ERROR_OUT_OF_MEMORY;
    t->capacity = initial_cap;
    return NEOX_OK;
}

void nx_table_destroy(neox_context *ctx, nx_table *t)
{
    if (t->slots) nx_free(ctx, t->slots, (size_t)t->elem_size * t->capacity, 16);
    if (t->free_list) nx_free(ctx, t->free_list, (size_t)t->free_cap * 4, 16);
    memset(t, 0, sizeof(*t));
}

uint32_t nx_table_alloc(neox_context *ctx, nx_table *t)
{
    if (t->free_count > 0) {
        return t->free_list[--t->free_count];
    }
    if (t->count == t->capacity) {
        uint32_t ncap = t->capacity * 2;
        uint8_t *np = (uint8_t *)nx_alloc(ctx, (size_t)t->elem_size * ncap, 16);
        if (!np) return UINT32_MAX;
        memcpy(np, t->slots, (size_t)t->elem_size * t->capacity);
        nx_free(ctx, t->slots, (size_t)t->elem_size * t->capacity, 16);
        t->slots = np;
        t->capacity = ncap;
    }
    return t->count++;
}

/* 归还槽位（gen 由释放路径在调用本函数前递增） */
static void nx_table_release(neox_context *ctx, nx_table *t, uint32_t index)
{
    if (t->free_count == t->free_cap) {
        uint32_t ncap = t->free_cap ? t->free_cap * 2 : 16;
        uint32_t *np = (uint32_t *)nx_alloc(ctx, (size_t)ncap * 4, 16);
        if (!np) return; /* 空闲链表溢出不致致命：槽位永不被复用即可 */
        memcpy(np, t->free_list, (size_t)t->free_cap * 4);
        nx_free(ctx, t->free_list, (size_t)t->free_cap * 4, 16);
        t->free_list = np;
        t->free_cap = ncap;
    }
    t->free_list[t->free_count++] = index;
    (void)ctx;
}

/* ------------------------------------------------------------------ 设备（后端分发；后端实现见 soft.c / gl.c） */

/* 槽位表初始化：所有后端使用同一组表（句柄布局与回收语义是核心契约） */
neox_result nx_device_init_tables(neox_device *dev)
{
    neox_context *ctx = dev->ctx;
    if (nx_table_init(ctx, &dev->textures, sizeof(nx_tex_slot), 16) != NEOX_OK)
        return NEOX_ERROR_OUT_OF_MEMORY;
    if (nx_table_init(ctx, &dev->paths, sizeof(nx_path_slot), 16) != NEOX_OK)
        return NEOX_ERROR_OUT_OF_MEMORY;
    if (nx_table_init(ctx, &dev->scenes, sizeof(nx_scene_slot), 8) != NEOX_OK)
        return NEOX_ERROR_OUT_OF_MEMORY;
    if (nx_table_init(ctx, &dev->nodes, sizeof(nx_node_slot), 32) != NEOX_OK)
        return NEOX_ERROR_OUT_OF_MEMORY;
    if (nx_table_init(ctx, &dev->subs, sizeof(nx_sub_slot), 16) != NEOX_OK)
        return NEOX_ERROR_OUT_OF_MEMORY;
    return NEOX_OK;
}

void nx_device_release_tables(neox_device *dev)
{
    neox_context *ctx = dev->ctx;
    nx_table_destroy(ctx, &dev->textures);
    nx_table_destroy(ctx, &dev->paths);
    nx_table_destroy(ctx, &dev->scenes);
    nx_table_destroy(ctx, &dev->nodes);
    nx_table_destroy(ctx, &dev->subs);
}

neox_result neox_device_create(neox_context *ctx, const neox_device_desc *desc,
                               neox_device **out_dev)
{
    neox_device *dev;
    neox_result r;
    neox_backend_kind kind;
    const nx_backend *vt;
    if (!ctx || !out_dev)
        return NEOX_ERROR_INVALID_ARGUMENT;
    *out_dev = NULL;
    kind = desc ? desc->backend : NEOX_BACKEND_SOFT;
    if (kind == NEOX_BACKEND_SOFT)
        vt = nx_backend_soft();
    else if (kind == NEOX_BACKEND_GPU) {
#ifdef NEOX_ENABLE_GL
        vt = nx_backend_gl();
#else
        NX_ERR(ctx, "GPU backend not available in this build",
               NEOX_ERROR_UNSUPPORTED);
#endif
    } else
        NX_ERR(ctx, "unknown backend kind", NEOX_ERROR_INVALID_ARGUMENT);
    dev = (neox_device *)nx_alloc(ctx, sizeof(neox_device), 16);
    if (!dev) NX_ERR(ctx, "device allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    memset(dev, 0, sizeof(*dev));
    dev->ctx = ctx;
    dev->kind = kind;
    dev->vt = vt;
    dev->id = ctx->next_dev_id++;
    if (dev->id > 0xffffu) { /* 16 位 dev_id 用尽（实践中不会触发） */
        nx_free(ctx, dev, sizeof(neox_device), 16);
        NX_ERR(ctx, "device id space exhausted", NEOX_ERROR_BACKEND);
    }
    r = vt->init(dev, desc ? desc->platform : NULL,
                 desc ? desc->platform_size : 0); /* 失败须自行清理已分配部分 */
    if (r != NEOX_OK) {
        nx_device_release_tables(dev);
        nx_free(ctx, dev, sizeof(neox_device), 16);
        return r;
    }
    /* 挂到 context 设备链表 */
    dev->next = ctx->devices;
    ctx->devices = dev;
    *out_dev = dev;
    return NEOX_OK;
}

/* ------------------------------------------------------------------ context */

neox_result neox_context_create(const neox_context_desc *desc, neox_context **out_ctx)
{
    static const neox_allocator sys = { NULL, sys_alloc, sys_free };
    if (!out_ctx) return NEOX_ERROR_INVALID_ARGUMENT;
    *out_ctx = NULL;
    /* 结构本身也经注入分配器分配（desc 在栈上，可用其分配器） */
    const neox_allocator *al = (desc && desc->allocator) ? desc->allocator : &sys;
    neox_context *ctx = (neox_context *)al->alloc(al->user, sizeof(neox_context), 16);
    if (!ctx) return NEOX_ERROR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc = *al;
    if (desc) ctx->budget = desc->memory_budget;
    ctx->next_dev_id = 1;
    *out_ctx = ctx;
    return NEOX_OK;
}

void neox_context_destroy(neox_context *ctx)
{
    if (!ctx) return;
    /* 级联销毁全部子对象（设备链表） */
    while (ctx->devices) {
        neox_device *d = ctx->devices;
        neox_device_destroy(d); /* 从链表摘除 */
    }
    neox_allocator al = ctx->alloc;
    al.free(al.user, ctx, sizeof(neox_context), 16);
}

uint32_t neox_version(void) { return NEOX_VER; }

const char *neox_get_error_detail(neox_context *ctx)
{
    if (!ctx) return NULL;
    return ctx->last_error;
}

/* ------------------------------------------------------------------ 句柄解析 */

neox_result nx_resolve(neox_device *dev, uint64_t handle, uint32_t expect_type,
                       uint32_t *out_index)
{
    neox_context *ctx = dev->ctx;
    if (handle == 0)
        NX_ERR(ctx, "zero handle", NEOX_ERROR_STALE_HANDLE);
    if (nx_handle_dev(handle) != dev->id)
        NX_ERR(ctx, "handle belongs to another device/instance",
               NEOX_ERROR_OWNERSHIP_VIOLATION);
    if (nx_handle_type(handle) != expect_type)
        NX_ERR(ctx, "handle used with wrong resource type",
               NEOX_ERROR_OWNERSHIP_VIOLATION);
    uint32_t idx = nx_handle_index(handle);
    nx_table *t = NULL;
    uint32_t gen = 0, in_use = 0;
    switch (expect_type) {
    case NX_TYPE_TEXTURE: t = &dev->textures; break;
    case NX_TYPE_PATH:    t = &dev->paths;    break;
    case NX_TYPE_SCENE:   t = &dev->scenes;   break;
    case NX_TYPE_NODE:    t = &dev->nodes;    break;
    case NX_TYPE_SUB:     t = &dev->subs;     break;
    default: NX_ERR(ctx, "unknown handle type", NEOX_ERROR_OWNERSHIP_VIOLATION);
    }
    if (idx >= t->count)
        NX_ERR(ctx, "handle index out of range", NEOX_ERROR_STALE_HANDLE);
    /* 每个槽位的 gen/in_use 固定在前 8 字节（见各槽结构） */
    {
        const uint32_t *head = (const uint32_t *)((const uint8_t *)t->slots +
                                                  (size_t)idx * t->elem_size);
        gen = head[0];
        in_use = head[1];
    }
    if (!in_use || gen != nx_handle_gen(handle))
        NX_ERR(ctx, "stale handle (generation mismatch)", NEOX_ERROR_STALE_HANDLE);
    *out_index = idx;
    return NEOX_OK;
}

/* ------------------------------------------------------------------ 引用与延迟回收（core 侧工具，soft.c 使用） */

/* 解析句柄并把槽位 refcount++（frame_end 记录 submission 引用时调用） */
neox_result nx_ref_handle(neox_device *dev, uint64_t handle, uint32_t expect_type)
{
    uint32_t idx;
    neox_result r = nx_resolve(dev, handle, expect_type, &idx);
    if (r != NEOX_OK) return r;
    switch (expect_type) {
    case NX_TYPE_TEXTURE:
        ((nx_tex_slot *)dev->textures.slots)[idx].refcount++;
        break;
    case NX_TYPE_PATH:
        ((nx_path_slot *)dev->paths.slots)[idx].refcount++;
        break;
    default: break;
    }
    return NEOX_OK;
}

void nx_unref_index(neox_device *dev, uint32_t type, uint32_t idx)
{
    switch (type) {
    case NX_TYPE_TEXTURE:
        if (((nx_tex_slot *)dev->textures.slots)[idx].refcount > 0)
            ((nx_tex_slot *)dev->textures.slots)[idx].refcount--;
        break;
    case NX_TYPE_PATH:
        if (((nx_path_slot *)dev->paths.slots)[idx].refcount > 0)
            ((nx_path_slot *)dev->paths.slots)[idx].refcount--;
        break;
    default: break;
    }
}

/* 释放槽位像素/数据并归还表（refcount 必须为 0；由 pump 驱动） */
void nx_slot_recycle(neox_device *dev, uint32_t type, uint32_t idx)
{
    neox_context *ctx = dev->ctx;
    switch (type) {
    case NX_TYPE_TEXTURE: {
        nx_tex_slot *s = &((nx_tex_slot *)dev->textures.slots)[idx];
        if (s->px) {
            size_t n = (size_t)s->width * s->height * 4 * sizeof(float);
            nx_free(ctx, s->px, n, 16);
        }
        memset(s, 0, sizeof(*s));
        nx_table_release(ctx, &dev->textures, idx);
        break;
    }
    case NX_TYPE_PATH: {
        nx_path_slot *s = &((nx_path_slot *)dev->paths.slots)[idx];
        if (s->verbs)  nx_free(ctx, s->verbs, s->verb_count, 4);
        if (s->points) nx_free(ctx, s->points, (size_t)s->point_count * 8, 16);
        memset(s, 0, sizeof(*s));
        nx_table_release(ctx, &dev->paths, idx);
        break;
    }
    default: break;
    }
}
