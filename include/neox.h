/* ===========================================================================
 * neox.h — NeoX 公共 API 草案 v0.1
 *
 * 状态：草案（NEOX_API_DRAFT）。允许破坏性变更，直到第一阶段验收完成。
 * 语义以 docs/semantics-v0.md 为准；本头文件只定义接口与契约标注。
 *
 * 契约总览（详见方案文档 v0.2 §4）：
 *   - 会话对象（context/device/frame）为不透明指针，不可序列化；
 *   - 资源（texture/path/scene/node/submission）为句柄（index+generation），
 *     句柄绑定 {实例, 设备, 类型}，跨域使用为契约错误（调试构建断言）；
 *   - 每个参数的内存所有权以 [own]/[borrow] 标注；
 *   - 资源释放经延迟回收：提交后至设备完成使用前，其内存不被复用；
 *   - 所有分配经注入分配器；任何分配失败沿返回值传播，不留半完成状态。
 *
 * 线程模型：核心不创建线程，也不假设线程存在。除特别注明外，同一 device
 * 的 API 调用必须外部串行化。异步性来自设备本身（GPU/DMA），以
 * submission + neox_device_pump 表达。
 * =========================================================================*/
#ifndef NEOX_H
#define NEOX_H

#include <stddef.h>
#include <stdint.h>

#define NEOX_API_DRAFT 1

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 结果 */

typedef enum neox_result {
    NEOX_OK = 0,
    NEOX_ERROR_INVALID_ARGUMENT,   /* 参数违反文档约定 */
    NEOX_ERROR_OUT_OF_MEMORY,      /* 分配器失败；状态保持调用前一致 */
    NEOX_ERROR_UNSUPPORTED,        /* 无法满足语义规范；默认失败而非近似 */
    NEOX_ERROR_DEVICE_LOST,        /* 设备丢失；资源句柄全部失效 */
    NEOX_ERROR_STALE_HANDLE,       /* generation 不匹配：句柄已过期 */
    NEOX_ERROR_OWNERSHIP_VIOLATION,/* 句柄跨实例/跨设备/跨类型使用 */
    NEOX_ERROR_BACKEND             /* 后端内部错误，细节见 error_detail */
} neox_result;

/* ------------------------------------------------------------------ 分配器 */

typedef struct neox_allocator {
    void *user;
    void *(*alloc)(void *user, size_t size, size_t align); /* 失败须返回 NULL */
    void  (*free) (void *user, void *ptr, size_t size, size_t align);
} neox_allocator; /* 传 NULL 表示使用系统 malloc/free；freestanding 构建必须注入 */

/* ------------------------------------------------------------------ 句柄 */

/* 资源句柄：index + generation 打包于 64 位。0 恒为无效句柄。 */
#define NEOX_HANDLE_TYPE(name) typedef struct { uint64_t id; } name
NEOX_HANDLE_TYPE(neox_texture);
NEOX_HANDLE_TYPE(neox_path);
NEOX_HANDLE_TYPE(neox_scene);
NEOX_HANDLE_TYPE(neox_node);
NEOX_HANDLE_TYPE(neox_submission);
#define NEOX_INVALID_HANDLE { 0 }

/* 会话对象（不透明指针，进程内有效，不进入 CAP） */
typedef struct neox_context neox_context;
typedef struct neox_device   neox_device;
typedef struct neox_frame    neox_frame;

/* ------------------------------------------------------------------ 基本类型 */

typedef struct neox_color {                         /* sRGB 编码、非预乘 */
    float r, g, b, a;
} neox_color;

typedef struct neox_affine {                        /* x'=a·x+c·y+tx, y'=b·x+d·y+ty */
    float a, b, c, d, tx, ty;
} neox_affine;

typedef struct neox_rect { float x, y, w, h; } neox_rect;

typedef enum neox_blend {                           /* 公式见语义规范 §6 */
    NEOX_BLEND_SRC_OVER = 0,                        /* 默认 */
    NEOX_BLEND_CLEAR, NEOX_BLEND_SRC, NEOX_BLEND_DST,
    NEOX_BLEND_DST_OVER, NEOX_BLEND_SRC_IN,  NEOX_BLEND_DST_IN,
    NEOX_BLEND_SRC_OUT,  NEOX_BLEND_DST_OUT, NEOX_BLEND_SRC_ATOP,
    NEOX_BLEND_DST_ATOP, NEOX_BLEND_XOR,     NEOX_BLEND_PLUS
} neox_blend;

typedef enum neox_fill_rule {
    NEOX_FILL_NONZERO = 0, NEOX_FILL_EVENODD
} neox_fill_rule;

typedef enum neox_pixel_format {
    NEOX_FORMAT_RGBA8_SRGB = 0,  /* 上传输入为非预乘；内部转为预乘 */
    NEOX_FORMAT_RGBA8_LINEAR,
    NEOX_FORMAT_A8               /* 遮罩（字形图集）；采样值为 alpha */
} neox_pixel_format;

typedef enum neox_filter {
    NEOX_FILTER_NEAREST = 0, NEOX_FILTER_LINEAR
} neox_filter;

/* 绘制调用级标志 */
#define NEOX_ALLOW_APPROXIMATE 0x1u  /* 显式授权近似执行；默认保持语义或失败 */

/* ------------------------------------------------------------------ 上下文 */

typedef struct neox_context_desc {
    const neox_allocator *allocator;  /* [borrow] NULL = 系统分配器 */
    uint64_t memory_budget;           /* 0 = 不限制；>0 时超预算分配即失败 */
    uint32_t flags;                   /* 保留，须为 0 */
} neox_context_desc;

neox_result neox_context_create(const neox_context_desc *desc, /* [borrow] */
                                neox_context **out_ctx);
void        neox_context_destroy(neox_context *ctx);           /* [own] 销毁全部子对象 */
uint32_t    neox_version(void);                                /* 0xMMmmpp */
/* 最近一次错误的静态描述串（英文 ASCII，进程内常量）；无错误返回 NULL */
const char *neox_get_error_detail(neox_context *ctx);

/* ------------------------------------------------------------------ 设备 */

/* 能力位：表达"能否以全语义执行"，不是"能否给出某种输出" */
#define NEOX_CAP_COMPUTE      0x1u
#define NEOX_CAP_FLOAT_BLEND  0x2u
#define NEOX_CAP_WIDE_COLOR   0x4u

typedef struct neox_caps {
    uint32_t features;
    uint32_t max_texture_size;
    uint64_t backend_memory_limit; /* 后端为内部缓冲声明的上限，0 = 未声明 */
} neox_caps;

typedef enum neox_backend_kind {
    NEOX_BACKEND_SOFT = 0,       /* 软件光栅，无 GPU 依赖 */
    NEOX_BACKEND_GPU             /* 最小 GPU 后端；原生句柄经 platform 字段传入 */
} neox_backend_kind;

typedef struct neox_device_desc {
    neox_backend_kind backend;
    /* 后端专属参数块。约定：首字段须为 uint32_t struct_size，
     * 第二字段须为 uint32_t magic 后端标识（GPU kind 下区分实现，
     * 如 'NXGL'/'NXMT'）；platform_size 为调用方声明的结构体字节数，
     * 后端据此做版本协商。 */
    const void *platform;        /* [borrow] ；SOFT 可为 NULL */
    size_t platform_size;        /* platform 指向结构体的字节数，无 platform 时为 0 */
} neox_device_desc;

neox_result neox_device_create(neox_context *ctx,
                               const neox_device_desc *desc,   /* [borrow] */
                               neox_device **out_dev);
void        neox_device_destroy(neox_device *dev);             /* [own] */
void        neox_device_get_caps(neox_device *dev, neox_caps *out_caps);
int         neox_device_is_lost(neox_device *dev);
/* 驱动异步完成与延迟回收队列。无线程环境下这是唯一的进度来源；
 * 应用每帧至少调用一次；返回已完成的 submission 数。 */
uint32_t    neox_device_pump(neox_device *dev);

/* ------------------------------------------------------------------ 纹理
 * load（磁盘→像素）不属于核心；核心只接受已解码像素并显式 upload。 */

typedef struct neox_texture_desc {
    uint32_t width, height;
    neox_pixel_format format;
    const void *pixels;          /* [borrow] 上传时拷贝；上传返回后即可释放 */
    size_t row_stride;           /* 字节 */
} neox_texture_desc;

neox_result neox_texture_create(neox_device *dev,
                                const neox_texture_desc *desc, /* [borrow] */
                                neox_texture *out_tex);
neox_result neox_texture_update(neox_device *dev,
                                neox_texture tex,
                                neox_rect region,
                                const void *pixels,            /* [borrow] */
                                size_t row_stride);
/* 释放进入延迟回收：若设备尚未完成使用，实际回收由 neox_device_pump 驱动。
 * 调用后句柄立即失效（generation 递增），不得再用于绘制。 */
void        neox_texture_release(neox_device *dev, neox_texture tex); /* [own] */

/* ------------------------------------------------------------------ 路径 */

typedef enum neox_path_verb {
    NEOX_VERB_MOVE = 0, NEOX_VERB_LINE, NEOX_VERB_QUAD,
    NEOX_VERB_CUBIC,     NEOX_VERB_CLOSE
} neox_path_verb;

typedef struct neox_path_desc {
    const uint8_t *verbs;        /* [borrow] neox_path_verb 数组，创建时拷贝 */
    const float   *points;       /* [borrow] xy 交替，长度由各动词点数之和决定 */
    uint32_t verb_count;
    uint32_t point_count;
    neox_fill_rule fill_rule;
} neox_path_desc;

neox_result neox_path_create(neox_device *dev,
                             const neox_path_desc *desc,       /* [borrow] */
                             neox_path *out_path);
void        neox_path_release(neox_device *dev, neox_path path); /* [own] */

/* ------------------------------------------------------------------ 帧（立即模式）
 * 帧命令是瞬态的：neox_frame_end 后全部销毁，净增持久节点恒为零。 */

typedef struct neox_target_desc {
    neox_backend_kind kind;      /* 须与 device 后端一致 */
    union {
        struct {                 /* SOFT：调用方提供的帧缓冲 */
            uint8_t *pixels;     /* [borrow] RGBA8_SRGB */
            size_t stride;
            uint32_t width, height;
        } soft;
        void *surface;           /* GPU：平台胶水层提供的表面句柄；
                                    目标宽高从表面对象获取，不在此重复声明 */
    };
    const neox_color *clear;     /* [borrow] NULL = 不清屏 */
} neox_target_desc;

/* 绘制状态栈元素 */
typedef struct neox_state {
    neox_affine transform;       /* 与当前栈顶左乘复合 */
    float opacity;               /* 组不透明度（隔离合成），[0,1] */
    neox_blend blend;
    uint32_t flags;              /* NEOX_ALLOW_APPROXIMATE 等 */
    const neox_path *clip;       /* [borrow] NULL = 不改变裁剪；裁剪为祖先交集 */
} neox_state;

neox_result neox_frame_begin(neox_device *dev,
                             const neox_target_desc *target,   /* [borrow] */
                             neox_frame **out_frame);

/* 状态栈：初始为 {恒等变换, opacity=1, SRC_OVER, flags=0, 无裁剪} */
neox_result neox_push_state(neox_frame *frame, const neox_state *state); /* [borrow] */
neox_result neox_pop_state(neox_frame *frame);

typedef struct neox_paint {
    neox_color color;            /* Phase 1 仅纯色；渐变等由 kind 扩展 */
} neox_paint;

neox_result neox_draw_path(neox_frame *frame, neox_path path,
                           const neox_paint *paint);            /* [borrow] */
neox_result neox_draw_texture(neox_frame *frame, neox_texture tex,
                              neox_rect src, neox_rect dst,
                              neox_filter filter, float opacity);

/* 字形 = 图集纹理中的带位置矩形；shaping/布局不在核心（NeoX-Text 职责） */
typedef struct neox_glyph {
    float x, y;                          /* 目标位置（局部坐标） */
    uint16_t sx, sy, sw, sh;             /* 图集源矩形（A8 遮罩） */
} neox_glyph;
neox_result neox_draw_glyphs(neox_frame *frame, neox_texture atlas,
                             const neox_glyph *glyphs, uint32_t count, /* [borrow] */
                             const neox_paint *paint);

/* 提交。失败是原子的：target 内容与本帧引用的资源状态保持调用前一致。
 * 成功时 out_sub 可用于查询完成状态；frame 随提交销毁。 */
neox_result neox_frame_end(neox_frame *frame,                   /* [own] */
                           neox_submission *out_sub);

/* ------------------------------------------------------------------ 提交与执行报告 */

typedef enum neox_submission_status {
    NEOX_SUBMISSION_PENDING = 0,
    NEOX_SUBMISSION_COMPLETE,
    NEOX_SUBMISSION_FAILED       /* 设备丢失等；画面状态见 frame_end 原子性约定 */
} neox_submission_status;

neox_submission_status neox_submission_get_status(neox_device *dev,
                                                  neox_submission sub);
/* 阻塞至完成；无线程环境内部为 pump 循环。设备丢失时返回非 OK 而非死等。 */
neox_result neox_submission_wait(neox_device *dev, neox_submission sub);

/* 实际执行路径，正式构建可用（能力协商的反馈回路） */
#define NEOX_EXEC_USED_APPROXIMATION 0x1u
typedef enum neox_exec_algorithm {
    NEOX_ALG_SCANLINE = 0, NEOX_ALG_TRIANGLE, NEOX_ALG_COMPUTE, NEOX_ALG_MIXED
} neox_exec_algorithm;
typedef struct neox_exec_report {
    uint32_t flags;
    neox_exec_algorithm algorithm;
    uint32_t draw_calls;
    uint64_t peak_bytes;         /* 本提交经注入分配器的内部缓冲峰值；
                                    不含 GPU 显存（其统计接口待后续版本） */
} neox_exec_report;
neox_result neox_submission_get_report(neox_device *dev,
                                       neox_submission sub,
                                       neox_exec_report *out_report);

/* ------------------------------------------------------------------ 场景（保留模式）
 * 节点拥有稳定身份，可定点修改；内部维护脏区与依赖（增量执行）。
 * 节点引用的 texture/path 为借用关系：场景不延长资源寿命，
 * 资源释放后再渲染引用它的场景为契约错误。 */

typedef enum neox_node_kind {
    NEOX_NODE_GROUP = 0, NEOX_NODE_PATH, NEOX_NODE_IMAGE, NEOX_NODE_GLYPHS
} neox_node_kind;

typedef struct neox_node_desc {
    neox_node_kind kind;
    neox_affine transform;
    neox_blend blend;
    float opacity;
    int visible;
    union {
        struct { neox_path path; neox_paint paint; } path;      /* [borrow] */
        struct { neox_texture tex; neox_rect src, dst;
                 neox_filter filter; } image;                   /* [borrow] */
        struct { neox_texture atlas;                            /* [borrow] */
                 const neox_glyph *glyphs;                      /* [borrow] 创建时拷贝 */
                 uint32_t count; neox_paint paint; } glyphs;
        struct { neox_path clip; } group;                       /* [borrow] 可为空 */
    };
} neox_node_desc;

neox_scene  neox_scene_create(neox_device *dev);               /* 失败返回无效句柄 */
void        neox_scene_destroy(neox_device *dev, neox_scene scene); /* [own] 级联销毁节点 */

neox_result neox_node_create(neox_device *dev, neox_scene scene,
                             const neox_node_desc *desc,        /* [borrow] */
                             neox_node *out_node);
void        neox_node_release(neox_device *dev, neox_node node); /* [own] 从父摘除并销毁 */

neox_result neox_node_append_child(neox_device *dev,
                                   neox_node parent, neox_node child);
neox_result neox_node_remove_from_parent(neox_device *dev, neox_node child);

/* 定点属性修改；每次调用将对应子树标记为脏 */
neox_result neox_node_set_transform(neox_device *dev, neox_node node, neox_affine t);
neox_result neox_node_set_paint(neox_device *dev, neox_node node, const neox_paint *p);
neox_result neox_node_set_opacity(neox_device *dev, neox_node node, float opacity);
neox_result neox_node_set_visible(neox_device *dev, neox_node node, int visible);

/* 将场景渲染进当前帧（叠加在当前状态栈之上）。
 * 语义与"立即模式逐节点绘制同一内容"完全一致（规范 §10.2-1）。 */
neox_result neox_scene_render(neox_frame *frame, neox_scene scene,
                              neox_node root);                  /* root 无效 = 场景根 */

/* ------------------------------------------------------------------ 捕获（CAP 最小版）
 * 第一阶段仅本地录制/回放；格式带版本号，不承诺跨版本兼容。 */

typedef struct neox_capture neox_capture;

neox_result neox_capture_begin(neox_device *dev, neox_capture **out_cap);
/* 结束录制，产出自包含缓冲（含资源快照与版本头）；缓冲所有权移交调用方 */
neox_result neox_capture_end(neox_capture *cap,                 /* [own] */
                             uint8_t **out_data, size_t *out_size);
/* 回放到当前帧。CAP 内资源 ID 经会话内映射表重建为本地句柄。
 * 引用了本地不存在且未随捕获打包的资源 → NEOX_ERROR_UNSUPPORTED。 */
neox_result neox_capture_replay(neox_frame *frame,
                                const uint8_t *data, size_t size); /* [borrow] */
void        neox_capture_free_buffer(neox_context *ctx,
                                     uint8_t *data, size_t size);  /* [own] */

/* 回放确定性所需的注入点（规范 §10 配套）：时间源与随机源由调用方提供 */
typedef struct neox_replay_env {
    double (*now_seconds)(void *user);
    uint32_t (*rand_u32)(void *user);
    void *user;
} neox_replay_env;

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NEOX_H */
