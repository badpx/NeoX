/* ===========================================================================
 * core.c — NeoX 核心：context、分配器包装、错误细节、句柄、槽位表
 *
 * 语义引用：docs/semantics-v0.md §5（颜色）、§9（确定性）、§10.1（失败语义）。
 * 本文件不依赖任何后端；Soft 后端在 soft.c。
 * =========================================================================*/
#include "internal.h"
#ifndef NEOX_SINGLE_THREADED
#include <stdatomic.h>
#endif

/* A process-wide identity sequence, never a resource registry. No identity is
 * reused, including after context destruction. Embedded builds may opt into
 * serialized device creation without requiring atomics or a thread runtime. */
static uint32_t nx_new_device_id(void)
{
#ifdef NEOX_SINGLE_THREADED
    static uint32_t next = 1;
    return next <= 0xffffffu ? next++ : 0;
#else
    static _Atomic uint32_t next = 1;
    uint32_t id = atomic_load_explicit(&next, memory_order_relaxed);
    while (id <= 0xffffffu) {
        if (atomic_compare_exchange_weak_explicit(&next, &id, id + 1,
                memory_order_relaxed, memory_order_relaxed)) return id;
    }
    return 0;
#endif
}

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

void *nx_alloc_using(neox_context *ctx, const neox_allocator *al, size_t size, size_t align)
{
    if (size == 0) size = 1;
    if (size > UINT64_MAX - ctx->used ||
        (ctx->budget && (ctx->used > ctx->budget || size > ctx->budget - ctx->used))) return NULL;
    void *p = al->alloc(al->user, size, align);
    if (!p) return NULL;
    ctx->used += size;
    if (ctx->used > ctx->used_peak) ctx->used_peak = ctx->used;
    for (nx_memory_probe *p = ctx->memory_probe; p; p = p->parent)
        if (ctx->used > p->peak) p->peak = ctx->used;
    return p;
}

void nx_free_using(neox_context *ctx, const neox_allocator *al, void *ptr, size_t size, size_t align)
{
    if (!ptr) return;
    if (size == 0) size = 1; /* 与 nx_alloc 的 0→1 映射对称，保证预算记账一致 */
    ctx->used -= size;
    al->free(al->user, ptr, size, align);
}

void *nx_alloc(neox_context *ctx, size_t size, size_t align)
{
    return nx_alloc_using(ctx,&ctx->alloc,size,align);
}
void nx_free(neox_context *ctx, void *ptr, size_t size, size_t align)
{
    nx_free_using(ctx,&ctx->alloc,ptr,size,align);
}

neox_result nx_buf_reserve(neox_context *ctx, nx_buf *b, size_t add)
{
    if (add > SIZE_MAX - b->len) return NEOX_ERROR_OUT_OF_MEMORY;
    if (b->len + add <= b->cap) return NEOX_OK;
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < b->len + add) {
        if (ncap > SIZE_MAX / 2) { ncap = b->len + add; break; }
        ncap *= 2;
    }
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
    if (!n) return NEOX_OK;
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
    t->free_head = UINT32_MAX;
    size_t bytes;
    if (!nx_size_mul(elem_size, initial_cap, &bytes)) return NEOX_ERROR_OUT_OF_MEMORY;
    t->slots = nx_alloc(ctx, bytes, 16);
    if (!t->slots) return NEOX_ERROR_OUT_OF_MEMORY;
    t->capacity = initial_cap;
    return NEOX_OK;
}

void nx_table_destroy(neox_context *ctx, nx_table *t)
{
    if (t->slots) nx_free(ctx, t->slots, (size_t)t->elem_size * t->capacity, 16);
    memset(t, 0, sizeof(*t));
}

uint32_t nx_table_alloc(neox_context *ctx, nx_table *t)
{
    uint32_t idx, gen = 1, *head;
    if (t->free_head != UINT32_MAX) {
        idx = t->free_head;
        head = (uint32_t *)((uint8_t *)t->slots + (size_t)idx * t->elem_size);
        gen = head[0];
        t->free_head = head[2];
    } else {
        if (t->count >= 0x100000u) return UINT32_MAX;
        if (t->count == t->capacity) {
            uint32_t ncap = t->capacity ? t->capacity * 2 : 16;
            size_t bytes;
            if (!nx_size_mul(t->elem_size, ncap, &bytes)) return UINT32_MAX;
            uint8_t *np = (uint8_t *)nx_alloc(ctx, bytes, 16);
            if (!np) return UINT32_MAX;
            if (t->slots) memcpy(np, t->slots, (size_t)t->elem_size * t->count);
            nx_free(ctx, t->slots, (size_t)t->elem_size * t->capacity, 16);
            t->slots = np;
            t->capacity = ncap;
        }
        idx = t->count++;
        head = (uint32_t *)((uint8_t *)t->slots + (size_t)idx * t->elem_size);
    }
    memset(head, 0, t->elem_size);
    head[0] = gen;
    head[1] = 1;
    return idx;
}

/* Release cannot allocate or fail. Retire a slot at generation exhaustion:
 * wrapping would make an ancient handle valid again. */
void nx_table_release(neox_context *ctx, nx_table *t, uint32_t index)
{
    uint32_t *head = (uint32_t *)((uint8_t *)t->slots + (size_t)index * t->elem_size);
    uint32_t gen = head[0];
    (void)ctx;
    memset(head, 0, t->elem_size);
    head[0] = gen < 0xffffu ? gen + 1 : gen;
    if (gen >= 0xffffu) return;
    head[2] = t->free_head;
    t->free_head = index;
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
#if NEOX_ENABLE_SCENE
    if (nx_table_init(ctx, &dev->scenes, sizeof(nx_scene_slot), 8) != NEOX_OK)
        return NEOX_ERROR_OUT_OF_MEMORY;
    if (nx_table_init(ctx, &dev->nodes, sizeof(nx_node_slot), 32) != NEOX_OK)
        return NEOX_ERROR_OUT_OF_MEMORY;
#endif
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
        /* 参数块第二字段为后端魔数（struct_size 之后），区分 GL/Metal 实现；
         * platform 为 NULL（或魔数为 0 的旧式参数块）时按历史行为默认 GL */
        const uint8_t *p = desc ? (const uint8_t *)desc->platform : NULL;
        size_t psz = desc ? desc->platform_size : 0;
        uint32_t magic = 0, header_size = 0;
        /* Validate the shared header before backend availability. Malformed
         * input remains INVALID_ARGUMENT in GL-only/Metal-only/Soft builds. */
        if ((!p && psz) || (p && psz < 2*sizeof(uint32_t)))
            NX_ERR(ctx, "invalid GPU platform header", NEOX_ERROR_INVALID_ARGUMENT);
        if (p) {
            memcpy(&header_size,p,sizeof(header_size));
            memcpy(&magic,p+sizeof(uint32_t),sizeof(magic));
            if (header_size < 2*sizeof(uint32_t) || header_size > psz)
                NX_ERR(ctx, "invalid GPU platform descriptor size", NEOX_ERROR_INVALID_ARGUMENT);
        }
        if (magic == 0 || magic == NX_GPU_DESC_MAGIC_GL) {
#ifdef NEOX_ENABLE_GL
            vt = nx_backend_gl();
#else
            NX_ERR(ctx, "GL backend not available in this build",
                   NEOX_ERROR_UNSUPPORTED);
#endif
        } else if (magic == NX_GPU_DESC_MAGIC_MTL) {
#ifdef NEOX_ENABLE_METAL
            vt = nx_backend_mtl();
#else
            NX_ERR(ctx, "Metal backend not available in this build",
                   NEOX_ERROR_UNSUPPORTED);
#endif
        } else
            NX_ERR(ctx, "unknown GPU platform desc magic",
                   NEOX_ERROR_INVALID_ARGUMENT);
    } else
        NX_ERR(ctx, "unknown backend kind", NEOX_ERROR_INVALID_ARGUMENT);
    dev = (neox_device *)nx_alloc(ctx, sizeof(neox_device), 16);
    if (!dev) NX_ERR(ctx, "device allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    memset(dev, 0, sizeof(*dev));
    dev->ctx = ctx;
    dev->kind = kind;
    dev->vt = vt;
    dev->id = nx_new_device_id();
    if (!dev->id) {
        nx_free(ctx, dev, sizeof(neox_device), 16);
        NX_ERR(ctx, "device identity space exhausted", NEOX_ERROR_BACKEND);
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
    ctx->injected = (desc && desc->allocator) ? desc->allocator : NULL;
    if (desc) ctx->budget = desc->memory_budget;
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

const neox_allocator *neox_context_get_allocator(neox_context *ctx)
{
    if (!ctx) return NULL;
    /* 创建时注入的原指针；未注入（系统默认）返回 NULL，
     * 由调用方决定转发 malloc/free 的语义 */
    return ctx->injected;
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

/* ------------------------------------------------------------------ 引用与延迟回收（core 侧工具，runtime/recording 使用） */

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
        nx_tex_data_release(ctx,s->data);
        nx_table_release(ctx, &dev->textures, idx);
        break;
    }
    case NX_TYPE_PATH: {
        nx_path_slot *s = &((nx_path_slot *)dev->paths.slots)[idx];
        if (s->verbs)  nx_free(ctx, s->verbs, s->verb_count, 4);
        if (s->points) nx_free(ctx, s->points, (size_t)s->point_count * 8, 16);
        nx_table_release(ctx, &dev->paths, idx);
        break;
    }
    default: break;
    }
}

/* Optional modules see opaque ownership and allocation services only. */
const neox_allocator *nx_context_allocator(neox_context *ctx) { return &ctx->alloc; }
neox_context *nx_device_context(neox_device *dev) { return dev ? dev->ctx : NULL; }
neox_device *nx_frame_device(neox_frame *frame) { return frame ? frame->dev : NULL; }

void nx_memory_probe_begin(neox_context *ctx, nx_memory_probe *p)
{
    p->baseline = p->peak = ctx->used;
    p->parent = ctx->memory_probe;
    ctx->memory_probe = p;
}
void nx_memory_probe_end(neox_context *ctx, nx_memory_probe *p, neox_exec_report *report)
{
    uint64_t extra = p->peak - p->baseline;
    if (extra > report->peak_bytes) report->peak_bytes = extra;
    ctx->memory_probe = p->parent;
}
neox_result neox_context_get_memory_stats(neox_context *ctx, neox_cpu_memory_stats *out)
{
    if (!ctx || !out) return NEOX_ERROR_INVALID_ARGUMENT;
    out->live_bytes = ctx->used;
    out->peak_bytes = ctx->used_peak;
    out->budget_bytes = ctx->budget;
    return NEOX_OK;
}
neox_result neox_device_get_memory_stats(neox_device *dev, neox_gpu_memory_stats *out)
{
    if (!dev || !out) return NEOX_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (dev->vt->memory_stats) dev->vt->memory_stats(dev, out);
    return NEOX_OK;
}
