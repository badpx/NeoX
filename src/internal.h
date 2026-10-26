/* ===========================================================================
 * internal.h — NeoX 内部结构（不公开）
 *
 * 设计要点：
 *   - 资源状态挂在 context/device/frame；唯一进程级元数据为不复用的设备身份序列；
 *   - 句柄布局（64 位）：dev_id[63:40] | type[39:36] | generation[35:20] | index[19:0]；
 *   - 所有分配经 neox_context 内的注入分配器，受 memory_budget 约束（规范 §10.1）；
 *   - Soft 后端无真异步：submission 在 neox_device_pump 时执行，但接口语义按异步
 *     模型实现（frame_end 打包、目标缓冲在执行成功前不被触碰，保证原子性）。
 * =========================================================================*/
#ifndef NEOX_INTERNAL_H
#define NEOX_INTERNAL_H

#include "neox.h"

#include <math.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 常量 */

#define NEOX_VER 0x000100u /* 0.1.0，与 neox_version() 对应 */

/* 垂直超采样数（规范 §4.4：覆盖率用采样逼近，受 §9 容差约束）。
 * 水平方向为解析精确 coverage，垂直方向 8 子扫描线逼近。 */
#define NEOX_SS 8

/* Flatten's device-pixel chord tolerance and bounded recursion. A curve that
 * cannot satisfy the tolerance at this depth returns UNSUPPORTED (§10.1). */
#define NEOX_FLAT_TOL 0.125f
#define NEOX_FLAT_MAX_DEPTH 12

/* 句柄类型码 */
enum {
    NX_TYPE_TEXTURE = 1,
    NX_TYPE_PATH    = 2,
    NX_TYPE_SCENE   = 3,
    NX_TYPE_NODE    = 4,
    NX_TYPE_SUB     = 5
};

/* ------------------------------------------------------------------ 句柄打包 */

static inline uint64_t nx_make_handle(uint32_t dev, uint32_t type,
                                      uint32_t index, uint32_t gen)
{
    return ((uint64_t)dev << 40) | ((uint64_t)type << 36) |
           ((uint64_t)gen << 20) | (uint64_t)index;
}
static inline uint32_t nx_handle_dev(uint64_t h)    { return (uint32_t)(h >> 40); }
static inline uint32_t nx_handle_type(uint64_t h)   { return (uint32_t)((h >> 36) & 0xfu); }
static inline uint32_t nx_handle_gen(uint64_t h)    { return (uint32_t)((h >> 20) & 0xffffu); }
static inline uint32_t nx_handle_index(uint64_t h)  { return (uint32_t)(h & 0xfffffu); }

#include "module.h"

/* 可增长字节缓冲（所有扩容经注入分配器；失败返回 NEOX_ERROR_OUT_OF_MEMORY） */
typedef struct nx_buf {
    uint8_t *p;
    size_t len, cap;
} nx_buf;

neox_result nx_buf_reserve(neox_context *ctx, nx_buf *b, size_t add);
neox_result nx_buf_put(neox_context *ctx, nx_buf *b, const void *data, size_t n);

/* ------------------------------------------------------------------ 结构前向 */

typedef struct neox_device nx_device; /* 内部别名，与公开 opaque 类型同一结构 */
typedef struct nx_cmd     nx_cmd;

/* ------------------------------------------------------------------ context */

typedef struct nx_memory_probe {
    uint64_t baseline, peak;
    struct nx_memory_probe *parent;
} nx_memory_probe;
void nx_memory_probe_begin(neox_context *ctx, nx_memory_probe *probe);
void nx_memory_probe_end(neox_context *ctx, nx_memory_probe *probe, neox_exec_report *report);

struct neox_context {
    neox_allocator alloc;        /* 归一化后的分配器（NULL 输入替换为系统分配器） */
    const neox_allocator *injected; /* 创建时注入的分配器原指针（NULL = 系统默认；
                                     供 neox_context_get_allocator 返回） */
    uint64_t budget;             /* 0 = 不限 */
    uint64_t used;               /* 当前已分配字节 */
    uint64_t used_peak;          /* monotonic context CPU high-water mark */
    nx_memory_probe *memory_probe; /* synchronous scope; never retained across GPU work */
    const char *last_error;      /* 最近一次错误的静态描述串 */
    nx_device *devices;          /* 设备链表（context_destroy 级联销毁） */
};

/* ------------------------------------------------------------------ 槽位表 */

/* 通用槽位表：index 复用 + 独立 generation（规范：释放后 generation 递增） */
typedef struct nx_table {
    void   *slots;      /* 元素数组 */
    uint32_t count;     /* 槽位数（含空闲槽） */
    uint32_t capacity;  /* 数组容量 */
    uint32_t elem_size;
    uint32_t free_head; /* intrusive free chain in unused slots; UINT32_MAX = empty */
} nx_table;

neox_result nx_table_init(neox_context *ctx, nx_table *t, uint32_t elem_size,
                          uint32_t initial_cap);
void        nx_table_destroy(neox_context *ctx, nx_table *t);
/* 分配一个槽位（尽量复用空闲槽），返回槽位索引；失败返回 UINT32_MAX。
 * 新槽位的 generation 从 1 开始；复用保留代际，槽位其余内容清零。 */
uint32_t    nx_table_alloc(neox_context *ctx, nx_table *t);
void nx_table_release(neox_context *ctx, nx_table *t, uint32_t index);

/* ------------------------------------------------------------------ 纹理槽 */

/* A slot owns its current version; commands and capture snapshots retain the
 * version they recorded. BYTE storage preserves upload encoding; FLOAT storage
 * is only for legacy CAP resources whose original bytes cannot be recovered. */
typedef struct nx_tex_data {
    uint64_t version;
    size_t bytes;
    uint32_t refs, linear;
    void *pixels;
} nx_tex_data;
void nx_tex_data_release(neox_context *ctx, nx_tex_data *data);

typedef struct nx_tex_slot {
    uint32_t gen;
    uint32_t in_use;
    uint32_t refcount;      /* 挂起的 submission 引用数（延迟回收保护） */
    uint32_t defer_next;
    uint32_t deferred;      /* 已进延迟回收队列 */
    uint32_t width, height;
    neox_pixel_format format;
    nx_tex_data *data;      /* current immutable-while-referenced version */
} nx_tex_slot;

/* ------------------------------------------------------------------ 路径槽 */

typedef struct nx_path_slot {
    uint32_t gen;
    uint32_t in_use;
    uint32_t refcount;
    uint32_t defer_next;
    uint32_t deferred;
    uint8_t  *verbs;        /* neox_path_verb 拷贝 */
    float    *points;       /* xy 交替拷贝 */
    uint32_t verb_count, point_count;
    neox_fill_rule fill_rule;
} nx_path_slot;

/* ------------------------------------------------------------------ 节点槽 / 场景槽 */

typedef struct nx_node_slot {
    uint32_t gen;
    uint32_t in_use;
    neox_node_kind kind;
    uint64_t scene_id; /* stable ownership, independent of attachment */
    neox_affine transform;
    neox_blend  blend;
    float       opacity;
    int         visible;
    uint32_t    parent;         /* 节点槽索引，根为自身 */
    uint32_t   *children;       /* 子节点槽索引数组（文档顺序） */
    uint32_t    child_count, child_cap;
    uint32_t    dirty;          /* 定点修改的脏位（当前粗粒度重绘，见 scene.c 注释） */
    /* 内容（*_id 为创建时句柄的副本：渲染期重校验，资源释放后再渲染为契约错误） */
    union {
        struct { uint32_t path_idx; uint64_t path_id; neox_paint paint; } path;
        struct { uint32_t tex_idx; uint64_t tex_id; neox_rect src, dst;
                 neox_filter filter; } image;
        struct { uint32_t atlas_idx; uint64_t atlas_id;
                 neox_glyph *glyphs; uint32_t count; neox_paint paint;
                 neox_glyph_style style; } glyphs;
        struct { uint32_t clip_idx; int has_clip; uint64_t clip_id; } group;
    } u;
} nx_node_slot;

typedef struct nx_scene_slot {
    uint32_t gen;
    uint32_t in_use;
    uint32_t root;          /* 根组节点槽索引 */
    uint32_t any_dirty;     /* 场景级脏标记（增量执行预留） */
} nx_scene_slot;

/* ------------------------------------------------------------------ 帧命令 */

typedef enum nx_op {
    NXC_PUSH = 0,       /* 压入绘制状态（组/裁剪/变换边界） */
    NXC_POP,
    NXC_PATH,           /* 填充路径 */
    NXC_TEX,            /* draw_texture */
    NXC_GLYPHS          /* draw_glyphs */
} nx_op;

typedef struct nx_cmd {
    nx_op op;
    /* PUSH 的状态快照 */
    neox_affine ctm;        /* 已复合到该层的世界变换 */
    float       opacity;    /* 组不透明度 */
    neox_blend  blend;
    uint32_t    flags;
    int32_t     clip_idx;   /* 路径槽索引；-1 = 无裁剪 */
    uint64_t    clip_id;    /* 裁剪路径原始句柄（CAP 录制与渲染期重校验） */
    int32_t     owns_layer; /* push 时是否创建隔离层（组/裁剪） */
    /* 绘制参数 */
    int32_t     res_idx;    /* 路径/纹理槽索引 */
    uint64_t    res_id;     /* 原始句柄（CAP 录制与调试） */
    nx_tex_data *texture;   /* retained pixel version for TEX/GLYPHS */
    neox_color  color;      /* paint（sRGB 非预乘，规范 §5） */
    neox_rect   src, dst;
    neox_filter filter;
    float       tex_opacity;
    neox_glyph *glyphs;     /* 创建时深拷贝 */
    uint32_t    glyph_count;
    /* 字形图集语义（neox_draw_glyphs_ex；0/0/0 = COVERAGE 默认，规范 §8.2） */
    neox_atlas_mode atlas_mode;
    float           dist_range;   /* 距离场编码范围（图集像素） */
    float           softness;     /* 边缘过渡宽度（输出像素，0=默认 1） */
} nx_cmd;

/* Backend-owned target snapshot, moved from recording to submission. This
 * leaves ER storage private instead of requiring every backend to use floats. */
typedef struct nx_backend_base {
    void *data;
    void (*release)(void *data);
} nx_backend_base;

/* ------------------------------------------------------------------ 帧（立即模式） */

struct neox_frame {
    nx_device  *dev;
    /* 目标（借用，执行成功前不被写入；规范 §10.1 原子性） */
    uint8_t    *target;
    void       *surface;    /* GPU：neox_gl_surface*（借用，见 neox_gl.h） */
    size_t      target_stride;
    uint32_t    width, height;
    /* 命令列表（frame_end 移交 submission，pump 时执行） */
    nx_cmd     *cmds;
    uint32_t    cmd_count, cmd_cap;
    /* 帧期状态：CTM 栈（push/pop 同步）+ 基底缓冲（清屏色或既有目标内容） */
    neox_affine *ctm_stack;
    uint32_t    ctm_count, ctm_cap;
    float      *base;       /* reference CPU base, moved at frame_end */
    nx_backend_base backend_base;
    neox_capture *capture_owner; /* per-frame capture transaction */
    uint32_t capture_cmd_start,capture_res_start;
    uint32_t    draw_calls;
};

/* ------------------------------------------------------------------ submission 槽 */

typedef struct nx_sub_slot {
    uint32_t gen;
    uint32_t in_use;
    neox_submission_status status;
    neox_result result;
    uint32_t started; /* queued -> submitted exactly once */
    uint32_t released; /* caller discarded handle; reclaim at completion */
    /* 执行载荷（frame_end 从 frame 移入，pump 执行后释放） */
    nx_cmd     *cmds;
    uint32_t    cmd_count, cmd_cap;
    float      *accum;
    nx_backend_base backend_base;
    uint8_t    *target;
    void       *surface;    /* GPU：neox_gl_surface* */
    size_t      target_stride;
    uint32_t    width, height;
    /* 引用资源（执行期保活；record 时 refcount++，执行后 --） */
    /* 执行报告 */
    neox_exec_report report;
} nx_sub_slot;

/* ------------------------------------------------------------------ device */

struct neox_device {
    neox_context *ctx;
    uint32_t      id;           /* 编入句柄的 device 标识 */
    neox_backend_kind kind;
    const struct nx_backend *vt; /* 后端 vtable（init/begin_target/execute/...） */
    void         *bstate;       /* 后端私有状态（GL：上下文、程序、层池） */
    nx_device    *next;         /* context 设备链表 */
    void (*owner_detach)(void *owner); /* notify optional adapter before destruction */
    void *owner;
    const char *loss_reason; /* terminal device state; static backend detail */

    nx_table textures;
    nx_table paths;
    nx_table scenes;
    nx_table nodes;
    nx_table subs;

    uint32_t defer_head; /* intrusive packed type/index list; 0 = empty */

    uint32_t   *pending;        /* 待执行 submission 槽索引 */
    uint32_t    pending_count, pending_cap;

    neox_frame *active_frame;   /* 进行中的帧（同时只允许一帧） */

    struct neox_capture *cap;   /* 进行中的捕获（CAP 最小版） */
};

/* ------------------------------------------------------------------ 颜色转换（规范 §5） */

/* sRGB 编码值（[0,1] float）→ 线性光 */
static inline float nx_srgb_to_linear(float c)
{
    if (c <= 0.04045f) return c / 12.92f;
    return powf((c + 0.055f) / 1.055f, 2.4f);
}
/* 线性光 → sRGB 编码值 */
static inline float nx_linear_to_srgb(float c)
{
    if (c <= 0.0031308f) return c * 12.92f;
    return 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

/* sRGB 非预乘 paint → 预乘线性（规范 §5：混合在线性光下进行） */
static inline void nx_paint_to_premul_linear(neox_color c, float out[4])
{
    float a = c.a;
    out[0] = nx_srgb_to_linear(c.r) * a;
    out[1] = nx_srgb_to_linear(c.g) * a;
    out[2] = nx_srgb_to_linear(c.b) * a;
    out[3] = a;
}

/* 预乘线性 → sRGB8 非预乘量化（§9：round-to-nearest, ties-to-even）。
 * a==0 时输出全零（非预乘除法无定义，取最不惊讶的零）。 */
static inline void nx_premul_linear_to_srgb8(const float p[4], uint8_t out[4])
{
    if (p[3] <= 0.0f) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    float ia = 1.0f / p[3];
    float r = p[0] * ia, g = p[1] * ia, b = p[2] * ia;
    if (r > 1.0f) r = 1.0f;
    if (g > 1.0f) g = 1.0f;
    if (b > 1.0f) b = 1.0f;
    out[0] = (uint8_t)lrintf(nx_linear_to_srgb(r) * 255.0f);
    out[1] = (uint8_t)lrintf(nx_linear_to_srgb(g) * 255.0f);
    out[2] = (uint8_t)lrintf(nx_linear_to_srgb(b) * 255.0f);
    out[3] = (uint8_t)lrintf(p[3] * 255.0f);
}

/* ------------------------------------------------------------------ 仿射（规范 §3） */

static inline neox_affine nx_affine_identity(void)
{
    neox_affine m = { 1, 0, 0, 1, 0, 0 };
    return m;
}
static inline int nx_affine_is_finite(neox_affine m)
{
    return isfinite(m.a) && isfinite(m.b) && isfinite(m.c) &&
           isfinite(m.d) && isfinite(m.tx) && isfinite(m.ty);
}
/* 复合：先应用 n，再应用 m（规范 §3：自叶向根左乘；状态栈新变换先作用于局部坐标） */
static inline neox_affine nx_affine_mul(neox_affine m, neox_affine n)
{
    neox_affine r;
    r.a  = m.a * n.a + m.c * n.b;
    r.b  = m.b * n.a + m.d * n.b;
    r.c  = m.a * n.c + m.c * n.d;
    r.d  = m.b * n.c + m.d * n.d;
    r.tx = m.a * n.tx + m.c * n.ty + m.tx;
    r.ty = m.b * n.tx + m.d * n.ty + m.ty;
    return r;
}
static inline void nx_affine_apply(neox_affine m, float x, float y, float *ox, float *oy)
{
    *ox = m.a * x + m.c * y + m.tx;
    *oy = m.b * x + m.d * y + m.ty;
    if (!isfinite(*ox)) {
        double v = (double)m.a*x + (double)m.c*y + m.tx;
        *ox = fabs(v) > FLT_MAX ? (v < 0 ? -INFINITY : INFINITY) : (float)v;
    }
    if (!isfinite(*oy)) {
        double v = (double)m.b*x + (double)m.d*y + m.ty;
        *oy = fabs(v) > FLT_MAX ? (v < 0 ? -INFINITY : INFINITY) : (float)v;
    }
}
/* Clockwise transformed corners; the rectangle and output must be finite. */
static inline int nx_affine_quad(neox_affine m, neox_rect r, float x[4], float y[4])
{
    float right = r.x+r.w, bottom = r.y+r.h;
    if (!isfinite(right) || !isfinite(bottom)) return 0;
    nx_affine_apply(m,r.x,r.y,&x[0],&y[0]);
    nx_affine_apply(m,right,r.y,&x[1],&y[1]);
    nx_affine_apply(m,right,bottom,&x[2],&y[2]);
    nx_affine_apply(m,r.x,bottom,&x[3],&y[3]);
    for (unsigned i=0;i<4;i++) if (!isfinite(x[i]) || !isfinite(y[i])) return 0;
    return 1;
}
/* Keep raster setup coordinates in a viewport-sized guard band. For distant
 * corners use a conservative visible AABB; shader containment still decides
 * the actual pixels. Ordinary quads retain their tight geometry. */
static inline int nx_bound_quad(float x[4], float y[4], uint32_t w, uint32_t h)
{
    float x0=x[0], x1=x[0], y0=y[0], y1=y[0];
    for (unsigned i=1;i<4;i++) {
        x0=fminf(x0,x[i]);x1=fmaxf(x1,x[i]);
        y0=fminf(y0,y[i]);y1=fmaxf(y1,y[i]);
    }
    if (x1<=0 || y1<=0 || x0>=(float)w || y0>=(float)h) return 0;
    if (x0>=-(float)w && x1<=2.0f*w && y0>=-(float)h && y1<=2.0f*h) return 1;
    x0=fmaxf(0,x0);x1=fminf((float)w,x1);
    y0=fmaxf(0,y0);y1=fminf((float)h,y1);
    x[0]=x[3]=x0;x[1]=x[2]=x1;y[0]=y[1]=y0;y[2]=y[3]=y1;
    return 1;
}
/* 1 = inverse produced, 0 = singular (legal empty geometry), -1 = range error.
 * Products of float coefficients fit in double, so finite large/small matrices
 * cannot become spuriously singular or invert to a matrix containing NaN/Inf. */
static inline int nx_affine_invert(neox_affine m, neox_affine *out)
{
    if (!nx_affine_is_finite(m)) return -1;
    double det = (double)m.a*m.d - (double)m.b*m.c;
    if (det == 0.0) return 0;
    double v[6] = {m.d/det, -m.b/det, -m.c/det, m.a/det, 0, 0};
    v[4] = -(v[0]*m.tx + v[2]*m.ty);
    v[5] = -(v[1]*m.tx + v[3]*m.ty);
    for (unsigned i = 0; i < 6; i++)
        if (!isfinite(v[i]) || fabs(v[i]) > FLT_MAX) return -1;
    *out = (neox_affine){(float)v[0],(float)v[1],(float)v[2],
                         (float)v[3],(float)v[4],(float)v[5]};
    return 1;
}
/* Keep sampling arithmetic finite even when large inverse products cancel.
 * Scale each inverse row and the corresponding local rectangle by the same
 * power of two. Normal inputs are unchanged; textures divide by the scaled
 * width, glyphs restore texel offsets using original_width/scaled_width.
 * The bound includes the whole viewport and subtraction of the local origin.
 * Reject a rescaling that would lose a nonzero input to the subnormal range. */
static inline int nx_sample_transform(neox_affine inv, neox_rect rect,
                                      uint32_t w, uint32_t h,
                                      neox_affine *out, neox_rect *local)
{
    float rows[2][5]={{inv.a,inv.c,inv.tx,rect.x,rect.w},
                      {inv.b,inv.d,inv.ty,rect.y,rect.h}};
    for (unsigned r=0;r<2;r++) {
        double bound=fabs((double)rows[r][0])*w + fabs((double)rows[r][1])*h +
                     fabs((double)rows[r][2]) + fabs((double)rows[r][3]) +
                     fabs((double)rows[r][4]);
        if (!isfinite(bound)) return 0;
        float scale=1;
        while (bound > (double)FLT_MAX/8) { bound*=0.5;scale*=0.5f; }
        if (scale==1) continue;
        for (unsigned i=0;i<5;i++) {
            float v=rows[r][i]*scale;
            if (rows[r][i]!=0 && fabsf(v)<FLT_MIN) return 0;
            rows[r][i]=v;
        }
    }
    *out=(neox_affine){rows[0][0],rows[1][0],rows[0][1],rows[1][1],rows[0][2],rows[1][2]};
    *local=(neox_rect){rows[0][3],rows[1][3],rows[0][4],rows[1][4]};
    return 1;
}
/* Convert only after bounding to the integer raster domain. limit <= INT32_MAX. */
static inline int32_t nx_pixel_floor(float x, uint32_t limit)
{
    if (x <= 0) return 0;
    if ((double)x >= limit) return (int32_t)limit;
    return (int32_t)floorf(x);
}
static inline int32_t nx_pixel_ceil(float x, uint32_t limit)
{
    if (x <= 0) return 0;
    if ((double)x >= limit) return (int32_t)limit;
    return (int32_t)ceilf(x);
}

/* ------------------------------------------------------------------ 后端 vtable
 *
 * Soft 与 GL 共享：句柄表、命令流录制（emit_*）、scene→命令流展开、
 * CAP 捕获/回放（CAP 工作在命令流层级，对 GPU 后端天然有效）、
 * submission 装配、延迟回收队列。后端差异收口到以下四个钩子：
 *   init         建槽位表 + 后端状态（失败须自行清理已分配部分）
 *   shutdown     释放后端状态（GL 对象、上下文）
 *   begin_target 校验 target 种类、确定帧尺寸、装载基底（fr->base）
 *   execute      pump 时执行 submission 命令流
 * 渲染语义的几何输入（路径 flatten、裁剪掩码光栅化）为跨后端共享代码
 * （geom.c），这是 §9 跨后端一致性的前提。 */

typedef struct nx_backend {
    const char *name;
    /* platform/plat_size：后端专属参数块及其字节数（neox_device_desc 约定：
     * 参数块首字段为 uint32_t struct_size；platform 为 NULL 时 plat_size 须为 0） */
    neox_result (*init)(neox_device *dev, const void *platform, size_t plat_size);
    void        (*shutdown)(neox_device *dev);
    neox_result (*begin_target)(neox_frame *fr, const neox_target_desc *target);
    neox_result (*execute)(neox_device *dev, nx_sub_slot *sub);
    void        (*get_caps)(neox_device *dev, neox_caps *out_caps);
    /* 异步进度（真异步后端，如 Metal）：登记在 completedHandler 的完成事件
     * 在本调用线程被收割——推进 submission 状态、释放载荷、返回新完成数。
     * 同步后端（Soft/GL）为 NULL（其 execute 在返回前已完成）。 */
    uint32_t    (*poll)(neox_device *dev);
    /* submission_wait 的进度等待：阻塞至至少一个在途提交完成（避免空转）。
     * 仅异步后端实现（内部可用 waitUntilCompleted 一类原语）；NULL = 无需等待。 */
    void        (*block)(neox_device *dev);
    void        (*memory_stats)(neox_device *dev, neox_gpu_memory_stats *out);
} nx_backend;

const nx_backend *nx_backend_soft(void);
#ifdef NEOX_ENABLE_GL
const nx_backend *nx_backend_gl(void);
#endif
#ifdef NEOX_ENABLE_METAL
const nx_backend *nx_backend_mtl(void);
#endif

/* NEOX_BACKEND_GPU 参数块魔数（第二字段，区分 GL/Metal 实现；定义值与
 * include/neox_gl.h、include/neox_mtl.h 的公开宏保持一致） */
#define NX_GPU_DESC_MAGIC_GL  0x4e58474cu  /* 'NXGL' */
#define NX_GPU_DESC_MAGIC_MTL 0x4e584d54u  /* 'NXMT' */

/* ------------------------------------------------------------------ 小工具（各后端共用） */

static inline int nx_float_bad(float v) { return isnan(v) || isinf(v); }

static inline int nx_clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* 可增长数组（元素定长；失败返回 NULL，原数组保持有效） */
static inline void *nx_array_grow(neox_context *ctx, void *arr, size_t elem,
                                  uint32_t *cap, uint64_t need)
{
    if (need > UINT32_MAX) return NULL;
    if (need <= *cap) return arr;
    uint32_t ncap = *cap ? *cap : 8;
    void *np;
    while (ncap < need) {
        if (ncap > UINT32_MAX / 2) { ncap = (uint32_t)need; break; }
        ncap *= 2;
    }
    if (elem && ncap > SIZE_MAX / elem) return NULL;
    np = nx_alloc(ctx, elem * (size_t)ncap, 16);
    if (!np) return NULL;
    if (arr) {
        memcpy(np, arr, elem * (size_t)(*cap));
        nx_free(ctx, arr, elem * (size_t)(*cap), 16);
    }
    *cap = ncap;
    return np;
}

/* RGBA8 sRGB 非预乘像素行 → 预乘线性 float（§5；frame 基底装载用） */
static inline void nx_pixels_to_premul_linear(const uint8_t *src, size_t stride,
                                              uint32_t w, uint32_t h, float *out)
{
    uint32_t x, y;
    for (y = 0; y < h; y++) {
        const uint8_t *row = src + (size_t)y * stride;
        for (x = 0; x < w; x++) {
            const uint8_t *sp = row + (size_t)x * 4;
            float *dp = &out[((size_t)y * w + x) * 4];
            float a = sp[3] / 255.0f;
            dp[0] = nx_srgb_to_linear(sp[0] / 255.0f) * a;
            dp[1] = nx_srgb_to_linear(sp[1] / 255.0f) * a;
            dp[2] = nx_srgb_to_linear(sp[2] / 255.0f) * a;
            dp[3] = a;
        }
    }
}

/* ------------------------------------------------------------------ 几何（geom.c，跨后端共享）
 *
 * flatten 在设备空间进行，内部 double 细分、输出 float 顶点；容差
 * NEOX_FLAT_TOL、最深 NEOX_FLAT_MAX_DEPTH，无法满足时明确失败。
 * rasterize 输出覆盖率掩码（w*h float）；三个后端共同消费该覆盖率。
 * 裁剪在 push 期急切光栅化（失败在调用点传播），绘制路径在执行期计算。 */

typedef struct nx_poly { uint32_t start, count; } nx_poly;

typedef struct nx_flat {
    float    *pts;      uint32_t npts, cap_pts;
    nx_poly  *polys;    uint32_t npolys, cap_polys;
} nx_flat;

void nx_flat_free(neox_context *ctx, nx_flat *f);
/* 将路径按 CTM 变换后 flatten 为轮廓集合（§3、§4） */
neox_result nx_flatten_path(neox_context *ctx, const nx_path_slot *path,
                            neox_affine ctm, nx_flat *out);
/* 输出 mask（w*h）由调用方分配；本函数清零后填充覆盖率（§4.4） */
neox_result nx_rasterize_path(neox_context *ctx, const nx_path_slot *path,
                              neox_affine ctm, uint32_t w, uint32_t h,
                              float *mask);

/* ------------------------------------------------------------------ 距离场重建（规范 §8.2，三后端共用）
 *
 * 编码：d = (0.5 − v) · dist_range（v ∈ [0,1] 为归一化采样值，正 = 字形内）。
 * 重建：t = clamp(d·s/w + 0.5, 0, 1); cov = t*t*(3-2*t)。
 * 正距离为内部，覆盖率随距离单调增加；w=0 使用 1 输出像素。 */
static inline float nx_sdf_reconstruct(float v_norm, float dist_range,
                                       float softness, float s)
{
    float w = softness > 0.0f ? softness : 1.0f;
    float d = (0.5f - v_norm) * dist_range * s;
    float t = d / w + 0.5f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}
static inline float nx_median3(float a, float b, float c)
{
    return fmaxf(fminf(a, b), fminf(fmaxf(a, b), c));
}

/* Mean singular value: (s0+s1)^2 = ||M||_F^2 + 2*|det(M)|.
 * Double intermediates cover every finite float input without squaring overflow
 * or subtracting nearly equal eigenvalues. No cancellation-prone sqrt(tr-disc).
 * A result outside float representation is rejected by command recording. */
static inline float nx_glyph_scale_factor(neox_affine m)
{
    double a = m.a, b = m.b, c = m.c, d = m.d;
    double det = a*d - b*c;
    double s = 0.5 * sqrt(a*a + b*b + c*c + d*d + 2.0*fabs(det));
    return s > FLT_MAX ? INFINITY : (float)s;
}

/* ------------------------------------------------------------------ 句柄解析（core.c 实现） */

/* 从 device 侧表解析并校验句柄。
 * 返回 NEOX_OK 并输出槽索引；错误时设置 ctx->last_error：
 *   - 句柄 dev 与本 device 不符        → NEOX_ERROR_OWNERSHIP_VIOLATION
 *   - 句柄类型与期望不符                → NEOX_ERROR_OWNERSHIP_VIOLATION
 *   - generation 不匹配 / 槽未占用      → NEOX_ERROR_STALE_HANDLE */
neox_result nx_resolve(neox_device *dev, uint64_t handle, uint32_t expect_type,
                       uint32_t *out_index);

/* submission 引用计数（延迟回收保护：执行完成前资源内存不复用） */
void        nx_unref_index(neox_device *dev, uint32_t type, uint32_t idx);
/* 回收槽位像素/数据并归还空闲链（refcount 必须为 0；pump 驱动） */
void        nx_slot_recycle(neox_device *dev, uint32_t type, uint32_t idx);

/* 设备槽位表初始化/释放（core.c 实现，后端 init 使用） */
neox_result nx_device_init_tables(neox_device *dev);
void        nx_device_release_tables(neox_device *dev);

/* submission 载荷释放（recording.c 实现）：状态推进后由 pump / 异步 poll 调用 */
void        nx_sub_release_payload(neox_device *dev, nx_sub_slot *sub);

void nx_texel_load(const nx_tex_slot *slot, const nx_tex_data *data, size_t pixel, float out[4]);
void nx_texture_expand_row(const nx_tex_slot *slot, const nx_tex_data *data, uint32_t y, float *out);

/* Backend-neutral recording/resource module boundaries (private API). */
#define NX_MAX_TEXTURE_SIZE 16384u
static inline int nx_color_is_bad(neox_color c)
{
    return nx_float_bad(c.r) || nx_float_bad(c.g) ||
           nx_float_bad(c.b) || nx_float_bad(c.a);
}
void nx_reap_resources(neox_device *dev);
neox_result nx_texture_from_linear(neox_device *dev, uint32_t w, uint32_t h,
                                        neox_pixel_format fmt, const float *px,
                                        neox_texture *out);
neox_result nx_validate_glyphs(neox_device *dev, uint32_t idx,
        const neox_glyph *glyphs, uint32_t count, neox_color color,
        const neox_glyph_style *style);
neox_result nx_emit_push(neox_frame *fr, neox_affine ctm, float opacity,
                             neox_blend blend, uint32_t flags,
                             int32_t clip_idx, uint64_t clip_id);
void nx_commands_free(neox_context *ctx, nx_cmd *cmds, uint32_t n);
neox_result nx_cmd_ref(neox_device *dev, const nx_cmd *c, int acquire);
void nx_frame_destroy(neox_device *dev, neox_frame *fr);
neox_result nx_emit_pop(neox_frame *fr);
neox_result nx_frame_finish_staged(neox_frame *frame, neox_frame *staged,
                                      neox_capture *cap, neox_result result);
neox_result nx_emit_path(neox_frame *fr, int32_t path_idx, uint64_t path_id,
                             neox_color color);
#if NEOX_ENABLE_CAPTURE
void nx_capture_frame_begin(neox_frame *frame);
void nx_capture_frame_rollback(neox_frame *frame);
neox_result nx_capture_record(neox_capture *cap, const nx_cmd *c);
neox_result nx_capture_record_batch(neox_capture *cap,const nx_cmd *cmds,uint32_t count);
#else
static inline void nx_capture_frame_begin(neox_frame *frame) { (void)frame; }
static inline void nx_capture_frame_rollback(neox_frame *frame) { (void)frame; }
#endif
/* Called only on the owning thread, never directly from GPU callbacks. */
neox_result nx_device_mark_lost(neox_device *dev, const char *reason);
neox_result nx_device_check_live(neox_device *dev);
/* Backend calls this only after work no longer accesses its payload. */
void nx_submission_complete(neox_device *dev, nx_sub_slot *sub, neox_result result);

/* ------------------------------------------------------------------ 错误辅助 */

#define NX_ERR(ctx, msg, code) do { (ctx)->last_error = (msg); return (code); } while (0)

/* Intersect filled regions on each subscanline before integrating coverage. */
typedef struct nx_clip_path {
    const nx_path_slot *path;
    neox_affine ctm;
} nx_clip_path;
neox_result nx_rasterize_intersection(neox_context *ctx, const nx_clip_path *paths,
    uint32_t count, uint32_t w, uint32_t h, float *mask);

#ifdef __cplusplus
}
#endif

#endif /* NEOX_INTERNAL_H */
