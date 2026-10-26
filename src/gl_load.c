/* ===========================================================================
 * gl_load.c — Linux GL 入口微型加载器（仅非 Apple 参与链接）
 *
 * 不引入 GLEW / glLoadGen（零依赖，ADR 0004 / docs/linux-port.md W2）。
 * 任一入口解析失败即失败：调用方把它映射为 NEOX_ERROR_BACKEND，
 * Soft 后端仍可单独运行。
 * =========================================================================*/
#include "gl_api.h"

#if !defined(__APPLE__)

static const char *g_load_err;

#define NX_GL_DEF(ret, name, args) PFN_##name name;
NX_GL_PROCS(NX_GL_DEF)
#undef NX_GL_DEF

/* void(*)(void) 与任意函数指针互转不触发 -Wcast-function-type；
 * 对象指针与函数指针经 union 转换（POSIX dlsym / eglGetProcAddress）。 */
typedef void (*nx_gl_fn)(void);

static nx_gl_fn nx_gl_sym(void *p)
{
    union { void *vp; nx_gl_fn fp; } u;
    u.vp = p;
    return u.fp;
}

int nx_gl_load(nx_gl_ctx *ctx)
{
    g_load_err = NULL;
#define NX_GL_LOAD(ret, name, args) \
    name = (PFN_##name)nx_gl_sym(nx_gl_ctx_get_proc(ctx, #name)); \
    if (!name) { g_load_err = "missing GL entry: " #name; return 0; }
    NX_GL_PROCS(NX_GL_LOAD)
#undef NX_GL_LOAD
    return 1;
}

const char *nx_gl_load_last_error(void) { return g_load_err; }

#endif /* !__APPLE__ */
