/* ===========================================================================
 * neox_gl.h — NeoX GPU（OpenGL）后端公开接口
 *
 * 状态：随 NEOX_API_DRAFT，后端专属扩展。仅当构建启用 NEOX_BACKEND_GL
 * （macOS：CGL 离屏上下文 + OpenGL 4.1 Core Profile）时可用。
 *
 * 用法（platform_size 协商见 neox_device_desc 约定）：
 *   neox_gl_device_desc gld;
 *   memset(&gld, 0, sizeof(gld));
 *   gld.struct_size = sizeof(gld);                     // 首字段：结构体版本
 *   gld.magic = NEOX_GL_DESC_MAGIC;                    // 后端标识（GL）
 *   gld.shared_context = NULL;                         // NULL = 自建离屏上下文
 *   neox_device_desc dd;
 *   memset(&dd, 0, sizeof(dd));
 *   dd.backend = NEOX_BACKEND_GPU;
 *   dd.platform = &gld;
 *   dd.platform_size = sizeof(gld);
 *   neox_device_create(ctx, &dd, &dev);
 *
 *   neox_gl_surface *surf;
 *   neox_gl_surface_create(dev, w, h, &surf);
 *
 *   neox_target_desc td = { 0 };
 *   td.kind = NEOX_BACKEND_GPU;
 *   td.surface = surf;                                  // neox_frame_begin 目标
 *   ...
 *   neox_device_pump(dev);
 *   neox_gl_surface_read_pixels(surf, pixels);          // RGBA8 sRGB 非预乘
 *
 * 语义与 Soft 后端完全一致（docs/semantics-v0.md §9 容差内）：
 *   - 帧命令仍只录制，neox_device_pump 时经 GL 执行；执行期间表面内容
 *     不被写入（§10.1 原子性），失败时 submission 标记 FAILED。
 *   - 所有 GL 调用发生在 pump / 表面 API 内；录制阶段不触碰 GL 状态。
 * =========================================================================*/
#ifndef NEOX_GL_H
#define NEOX_GL_H

#include "neox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 设备参数
 * 经 neox_device_desc.platform 传入（[borrow]，创建时读取）。 */

#define NEOX_GL_DESC_MAGIC 0x4e58474cu  /* 'NXGL'：NEOX_BACKEND_GPU 参数块后端标识 */

typedef struct neox_gl_device_desc {
    uint32_t struct_size;   /* 必须 = sizeof(neox_gl_device_desc)（版本协商） */
    uint32_t magic;         /* 必须 = NEOX_GL_DESC_MAGIC（后端区分） */
    void *shared_context;   /* 外部 CGLContextObj：新上下文加入其 share group，
                             * 纹理对象可跨上下文共享。NULL = 自建离屏上下文。 */
} neox_gl_device_desc;
/* 约定：platform_size >= sizeof(neox_gl_device_desc) 且首字段 struct_size
 * 与之一致；platform_size 更大时按前向兼容处理（只用本版前缀字段）。 */

/* ------------------------------------------------------------------ 离屏表面
 * 不透明句柄；neox_frame_begin 的 target.surface 接受本指针。
 * 表面生命周期由调用方管理（[own]），须先于 device_destroy 释放。 */

typedef struct neox_gl_surface neox_gl_surface;

/* [own] 创建 RGBA8 sRGB 非预乘离屏表面（FBO） */
neox_result neox_gl_surface_create(neox_device *dev,
                                   uint32_t width, uint32_t height,
                                   neox_gl_surface **out_surface);
/* [own] 销毁表面（GL 对象立即释放；不得在提交尚未 pump 时销毁被引用的表面） */
void        neox_gl_surface_destroy(neox_device *dev,
                                    neox_gl_surface *surface);

/* 读回整幅表面：RGBA8 sRGB 非预乘，行距 width*4 字节，左上原点。
 * 正式读回通道（测试与无窗口环境）；内部先同步再拷贝。 */
neox_result neox_gl_surface_read_pixels(neox_gl_surface *surface,
                                        uint8_t *out_pixels);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NEOX_GL_H */
