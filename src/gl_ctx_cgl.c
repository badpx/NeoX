/* ===========================================================================
 * gl_ctx_cgl.c — macOS CGL 离屏上下文（从 gl.c 拆出，行为与拆分前一致）
 *
 * CGLChoosePixelFormat 一族在 <OpenGL/OpenGL.h> 中声明，但该头会连带引入
 * gl.h，与 Core Profile 的 gl3.h 冲突。这几个入口是 10.0 起稳定的纯 C ABI，
 * 按 CGLTypes.h 的类型自行声明。
 * =========================================================================*/
#include "gl_ctx.h"

#include <OpenGL/CGLCurrent.h>
#include <OpenGL/CGLTypes.h>
/* GLint 等标量类型；gltypes.h 仅 typedef，不引入 gl.h 的函数声明，
 * 与"避开 gl.h"的既定策略兼容（gl3.h 亦经此头取 GLint）。 */
#include <OpenGL/gltypes.h>

#include <dlfcn.h>
#include <stdlib.h>

extern CGLError CGLChoosePixelFormat(const CGLPixelFormatAttribute *attribs,
                                     CGLPixelFormatObj *pix, GLint *npix);
extern void CGLReleasePixelFormat(CGLPixelFormatObj pix);
extern CGLError CGLCreateContext(CGLPixelFormatObj pix, CGLContextObj share,
                                 CGLContextObj *ctx);
extern void CGLReleaseContext(CGLContextObj ctx);

struct nx_gl_ctx {
    CGLContextObj cgl;
};

static const char *g_err;

const char *nx_gl_ctx_last_error(void) { return g_err; }
const char *nx_gl_ctx_kind(const nx_gl_ctx *ctx)
{
    (void)ctx;
    return "cgl";
}

int nx_gl_ctx_can_window(const nx_gl_ctx *ctx)
{
    (void)ctx;
    return 0;
}

void *nx_gl_ctx_window_surface(nx_gl_ctx *ctx, void *native_window, int vsync)
{
    (void)ctx;
    (void)native_window;
    (void)vsync;
    g_err = "CGL: window surfaces are not supported";
    return NULL;
}

void nx_gl_ctx_window_destroy(nx_gl_ctx *ctx, void *egl_surface)
{
    (void)ctx;
    (void)egl_surface;
}

int nx_gl_ctx_window_make_current(nx_gl_ctx *ctx, void *egl_surface)
{
    (void)ctx;
    (void)egl_surface;
    return 0;
}

void nx_gl_ctx_window_swap(nx_gl_ctx *ctx, void *egl_surface)
{
    (void)ctx;
    (void)egl_surface;
}

int nx_gl_ctx_native_visual(const nx_gl_ctx *ctx, unsigned long *out_id)
{
    (void)ctx;
    (void)out_id;
    return 0;
}

nx_gl_ctx *nx_gl_ctx_create(const neox_gl_device_desc *desc)
{
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAAccelerated,
        kCGLPFANoRecovery,
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
        (CGLPixelFormatAttribute)0
    };
    CGLPixelFormatObj pf = NULL;
    CGLContextObj share = NULL;
    CGLContextObj cgl = NULL;
    GLint npix = 0;
    CGLError cerr;
    nx_gl_ctx *out;

    g_err = NULL;
    share = desc ? (CGLContextObj)desc->shared_context : NULL;
    cerr = CGLChoosePixelFormat(attrs, &pf, &npix);
    if (cerr != kCGLNoError || pf == NULL || npix == 0) {
        g_err = "CGL: no OpenGL 4.1 Core pixel format";
        return NULL;
    }
    cerr = CGLCreateContext(pf, share, &cgl);
    CGLReleasePixelFormat(pf);
    if (cerr != kCGLNoError || cgl == NULL) {
        g_err = "CGL: context creation failed";
        return NULL;
    }
    out = (nx_gl_ctx *)calloc(1, sizeof(*out));
    if (!out) {
        CGLReleaseContext(cgl);
        g_err = "CGL: out of memory";
        return NULL;
    }
    out->cgl = cgl;
    return out;
}

int nx_gl_ctx_make_current(nx_gl_ctx *ctx)
{
    if (!ctx || !ctx->cgl) return 0;
    return CGLSetCurrentContext(ctx->cgl) == kCGLNoError;
}

void *nx_gl_ctx_get_proc(nx_gl_ctx *ctx, const char *name)
{
    (void)ctx;
    return dlsym(RTLD_DEFAULT, name);
}

void nx_gl_ctx_destroy(nx_gl_ctx *ctx)
{
    if (!ctx) return;
    if (ctx->cgl) {
        CGLSetCurrentContext(NULL);
        CGLReleaseContext(ctx->cgl);
    }
    free(ctx);
}
