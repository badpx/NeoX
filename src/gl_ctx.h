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
/* 解析当前上下文的 GL 入口。须在 make_current 成功之后调用。 */
void       *nx_gl_ctx_get_proc(nx_gl_ctx *ctx, const char *name);
void        nx_gl_ctx_destroy(nx_gl_ctx *ctx);
const char *nx_gl_ctx_last_error(void);
/* 诊断用："cgl" / "egl-surfaceless" / "egl-device" / "egl-default"。 */
const char *nx_gl_ctx_kind(void);

#ifdef __cplusplus
}
#endif
#endif /* NEOX_GL_CTX_H */
