/* ===========================================================================
 * gl_ctx.h — GL 上下文内部接口（平台实现：gl_ctx_cgl.c / gl_ctx_egl.c）
 *
 * gl.c 只经本接口创建、绑定与销毁上下文。平台差异由 CMake 选择编译单元
 * 表达，不在 gl.c 里写 CGL/EGL 分支（docs/linux-port.md §3）。
 * =========================================================================*/
#ifndef NEOX_GL_CTX_H
#define NEOX_GL_CTX_H

#include "neox_gl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nx_gl_ctx nx_gl_ctx;

/* desc == NULL：自建离屏上下文，不加入外部 share group。
 * desc != NULL：只读取 shared_context（struct_size / magic 由调用方协商）。
 * 成功返回非 NULL；上下文已创建，调用方须再 nx_gl_ctx_make_current。
 * 失败返回 NULL，细节见 nx_gl_ctx_last_error（静态串，下次调用前有效）。 */
nx_gl_ctx  *nx_gl_ctx_create(const neox_gl_device_desc *desc);
int         nx_gl_ctx_make_current(nx_gl_ctx *ctx);
int         nx_gl_ctx_is_lost(const nx_gl_ctx *ctx);
/* 解析当前上下文的 GL 入口。须在 make_current 成功之后调用。 */
void       *nx_gl_ctx_get_proc(nx_gl_ctx *ctx, const char *name);
void        nx_gl_ctx_destroy(nx_gl_ctx *ctx);
const char *nx_gl_ctx_last_error(void);
/* 诊断用："cgl" / "egl-surfaceless" / "egl-x11" / "egl-wayland" 等。 */
const char *nx_gl_ctx_kind(const nx_gl_ctx *ctx);

/* 窗口表面。native 由平台编译单元解释（Linux：X11 Window 或 wl_egl_window*）。
 * 上下文必须建在对应的本地 display 上（neox_gl_device_desc.native_display）。
 * macOS 恒不支持。失败返回 NULL / 0。 */
int         nx_gl_ctx_can_window(const nx_gl_ctx *ctx);
void       *nx_gl_ctx_window_surface(nx_gl_ctx *ctx, void *native_window, int vsync);
void        nx_gl_ctx_window_destroy(nx_gl_ctx *ctx, void *egl_surface);
int         nx_gl_ctx_window_make_current(nx_gl_ctx *ctx, void *egl_surface);
int         nx_gl_ctx_window_swap(nx_gl_ctx *ctx, void *egl_surface);
/* X11：EGL_NATIVE_VISUAL_ID。没有（Wayland / 离屏 / CGL）返回 0。 */
int         nx_gl_ctx_native_visual(const nx_gl_ctx *ctx, unsigned long *out_id);

#ifdef __cplusplus
}
#endif
#endif /* NEOX_GL_CTX_H */
