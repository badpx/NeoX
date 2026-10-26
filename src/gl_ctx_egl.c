/* ===========================================================================
 * gl_ctx_egl.c — Linux（及同类 EGL 桌面 GL）离屏上下文
 *
 * 首选 EGL_MESA_platform_surfaceless：无头、不需要 X11/Wayland，Mesa
 * llvmpipe 支持，渲染目标是自有 FBO（EGL_NO_SURFACE）。不支持时回退
 * EGL_EXT_platform_device，再回退 eglGetDisplay(EGL_DEFAULT_DISPLAY)；
 * surfaceless make-current 失败时再试 1×1 PBuffer。
 *
 * 桌面 OpenGL 4.1 Core（现有 shader 是 GLSL 410），不是 GLES。
 * libEGL.so.1 运行期 dlopen，链接期不依赖 libEGL：找不到库 =
 * NEOX_ERROR_BACKEND，进程仍可跑 Soft（docs/linux-port.md W3）。
 * GLX 路径未实现：本机与 CI 的 surfaceless 已足够；老 Mesa 若三条
 * EGL 路径都失败，后端明确不可用。
 * =========================================================================*/
#include "gl_ctx.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int            EGLint;
typedef unsigned int   EGLenum;
typedef unsigned int   EGLBoolean;
typedef intptr_t       EGLAttrib;
typedef void          *EGLDisplay;
typedef void          *EGLConfig;
typedef void          *EGLContext;
typedef void          *EGLSurface;
typedef void          *EGLDeviceEXT;
typedef void         (*nx_egl_fn)(void);

#define EGL_FALSE           0
#define EGL_TRUE            1
#define EGL_NO_DISPLAY      ((EGLDisplay)0)
#define EGL_NO_CONTEXT      ((EGLContext)0)
#define EGL_NO_SURFACE      ((EGLSurface)0)
#define EGL_DEFAULT_DISPLAY ((void *)0)

#define EGL_OPENGL_API                      0x30A2
#define EGL_OPENGL_BIT                      0x0008
#define EGL_PBUFFER_BIT                     0x0001
#define EGL_ALPHA_SIZE                      0x3021
#define EGL_BLUE_SIZE                       0x3022
#define EGL_GREEN_SIZE                      0x3023
#define EGL_RED_SIZE                        0x3024
#define EGL_NONE                            0x3038
#define EGL_EXTENSIONS                      0x3055
#define EGL_HEIGHT                          0x3056
#define EGL_WIDTH                           0x3057
#define EGL_SURFACE_TYPE                    0x3033
#define EGL_RENDERABLE_TYPE                 0x3040
#define EGL_CONTEXT_MAJOR_VERSION           0x3098
#define EGL_CONTEXT_MINOR_VERSION           0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK     0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT 0x00000001
#define EGL_PLATFORM_DEVICE_EXT             0x313F
#define EGL_PLATFORM_SURFACELESS_MESA       0x31DD

typedef EGLBoolean (*PFN_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
typedef EGLBoolean (*PFN_eglTerminate)(EGLDisplay);
typedef EGLBoolean (*PFN_eglBindAPI)(EGLenum);
typedef EGLBoolean (*PFN_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *,
                                          EGLint, EGLint *);
typedef EGLContext (*PFN_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext,
                                           const EGLint *);
typedef EGLBoolean (*PFN_eglDestroyContext)(EGLDisplay, EGLContext);
typedef EGLBoolean (*PFN_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface,
                                         EGLContext);
typedef EGLint     (*PFN_eglGetError)(void);
typedef EGLDisplay (*PFN_eglGetDisplay)(void *);
typedef const char *(*PFN_eglQueryString)(EGLDisplay, EGLint);
typedef EGLSurface (*PFN_eglCreatePbufferSurface)(EGLDisplay, EGLConfig,
                                                  const EGLint *);
typedef EGLBoolean (*PFN_eglDestroySurface)(EGLDisplay, EGLSurface);
typedef nx_egl_fn  (*PFN_eglGetProcAddress)(const char *);
typedef EGLDisplay (*PFN_eglGetPlatformDisplay)(EGLenum, void *, const EGLAttrib *);
typedef EGLBoolean (*PFN_eglQueryDevicesEXT)(EGLint, EGLDeviceEXT *, EGLint *);

struct nx_gl_ctx {
    EGLContext ctx;
    EGLSurface surface; /* EGL_NO_SURFACE：surfaceless */
};

static void *g_lib;
static EGLDisplay g_dpy;
static int g_refs;
static const char *g_kind = "egl-none";
static const char *g_err;
static char g_errbuf[192];

static PFN_eglGetProcAddress       p_getProc;
static PFN_eglGetDisplay           p_getDisplay;
static PFN_eglInitialize           p_initialize;
static PFN_eglTerminate            p_terminate;
static PFN_eglBindAPI              p_bindAPI;
static PFN_eglChooseConfig         p_chooseConfig;
static PFN_eglCreateContext        p_createContext;
static PFN_eglDestroyContext       p_destroyContext;
static PFN_eglMakeCurrent          p_makeCurrent;
static PFN_eglGetError             p_getError;
static PFN_eglQueryString          p_queryString;
static PFN_eglCreatePbufferSurface p_createPbuffer;
static PFN_eglDestroySurface       p_destroySurface;
static PFN_eglGetPlatformDisplay   p_getPlatformDisplay;

const char *nx_gl_ctx_last_error(void) { return g_err; }
const char *nx_gl_ctx_kind(void) { return g_kind; }

static void set_err(const char *msg) { g_err = msg; }

static void set_err_egl(const char *prefix)
{
    EGLint code = p_getError ? p_getError() : 0;
    snprintf(g_errbuf, sizeof g_errbuf, "%s (egl 0x%x)", prefix, (unsigned)code);
    g_err = g_errbuf;
}

static int ext_has(const char *ext, const char *name)
{
    size_t n;
    if (!ext || !name) return 0;
    n = strlen(name);
    while (*ext) {
        if ((strncmp(ext, name, n) == 0) && (ext[n] == '\0' || ext[n] == ' '))
            return 1;
        while (*ext && *ext != ' ') ext++;
        while (*ext == ' ') ext++;
    }
    return 0;
}

static int load_lib(void)
{
    if (g_lib) return 1;
    g_lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g_lib) g_lib = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_lib) {
        const char *de = dlerror();
        snprintf(g_errbuf, sizeof g_errbuf, "EGL: dlopen libEGL.so.1 failed%s%s",
                 de ? ": " : "", de ? de : "");
        g_err = g_errbuf;
        return 0;
    }
    /* dlsym 返回对象指针。经 union 转到函数指针，避免 -Wcast-function-type。 */
#define NX_DLSYM(dst, type, sym) do { \
        union { void *vp; type fp; } u_; \
        u_.vp = dlsym(g_lib, sym); \
        dst = u_.fp; \
    } while (0)
    NX_DLSYM(p_getProc, PFN_eglGetProcAddress, "eglGetProcAddress");
    NX_DLSYM(p_getDisplay, PFN_eglGetDisplay, "eglGetDisplay");
    NX_DLSYM(p_initialize, PFN_eglInitialize, "eglInitialize");
    NX_DLSYM(p_terminate, PFN_eglTerminate, "eglTerminate");
    NX_DLSYM(p_bindAPI, PFN_eglBindAPI, "eglBindAPI");
    NX_DLSYM(p_chooseConfig, PFN_eglChooseConfig, "eglChooseConfig");
    NX_DLSYM(p_createContext, PFN_eglCreateContext, "eglCreateContext");
    NX_DLSYM(p_destroyContext, PFN_eglDestroyContext, "eglDestroyContext");
    NX_DLSYM(p_makeCurrent, PFN_eglMakeCurrent, "eglMakeCurrent");
    NX_DLSYM(p_getError, PFN_eglGetError, "eglGetError");
    NX_DLSYM(p_queryString, PFN_eglQueryString, "eglQueryString");
    NX_DLSYM(p_createPbuffer, PFN_eglCreatePbufferSurface, "eglCreatePbufferSurface");
    NX_DLSYM(p_destroySurface, PFN_eglDestroySurface, "eglDestroySurface");
    if (!p_getProc || !p_getDisplay || !p_initialize || !p_terminate ||
        !p_bindAPI || !p_chooseConfig || !p_createContext || !p_destroyContext ||
        !p_makeCurrent || !p_getError || !p_createPbuffer || !p_destroySurface) {
        set_err("EGL: libEGL is missing a required entry point");
        return 0;
    }
    NX_DLSYM(p_getPlatformDisplay, PFN_eglGetPlatformDisplay, "eglGetPlatformDisplay");
    if (!p_getPlatformDisplay) {
        nx_egl_fn raw = p_getProc("eglGetPlatformDisplay");
        union { nx_egl_fn fn; PFN_eglGetPlatformDisplay plat; } u;
        u.fn = raw;
        p_getPlatformDisplay = u.plat;
    }
#undef NX_DLSYM
    return 1;
}

static int try_init_display(EGLDisplay dpy)
{
    EGLint major = 0, minor = 0;
    if (dpy == EGL_NO_DISPLAY) return 0;
    if (!p_initialize(dpy, &major, &minor)) {
        p_terminate(dpy);
        return 0;
    }
    return 1;
}

static int ensure_display(void)
{
    const char *cext = NULL;
    if (g_dpy != EGL_NO_DISPLAY) return 1;

    if (p_queryString)
        cext = p_queryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

    if (p_getPlatformDisplay &&
        (!cext || ext_has(cext, "EGL_MESA_platform_surfaceless"))) {
        EGLDisplay dpy = p_getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
                                              EGL_DEFAULT_DISPLAY, NULL);
        if (try_init_display(dpy)) {
            g_dpy = dpy;
            g_kind = "egl-surfaceless";
            return 1;
        }
    }
    if (p_getPlatformDisplay &&
        (!cext || ext_has(cext, "EGL_EXT_platform_device"))) {
        nx_egl_fn raw = p_getProc("eglQueryDevicesEXT");
        union { nx_egl_fn fn; PFN_eglQueryDevicesEXT query; } u;
        PFN_eglQueryDevicesEXT query;
        u.fn = raw;
        query = u.query;
        EGLDeviceEXT devs[8];
        EGLint n = 0, i;
        if (query && query(8, devs, &n)) {
            for (i = 0; i < n; i++) {
                EGLDisplay dpy = p_getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT,
                                                      devs[i], NULL);
                if (try_init_display(dpy)) {
                    g_dpy = dpy;
                    g_kind = "egl-device";
                    return 1;
                }
            }
        }
    }
    {
        EGLDisplay dpy = p_getDisplay(EGL_DEFAULT_DISPLAY);
        if (try_init_display(dpy)) {
            g_dpy = dpy;
            g_kind = "egl-default";
            return 1;
        }
    }
    set_err("EGL: no surfaceless, device, or default display");
    return 0;
}

static int choose_config(int want_pbuffer, EGLConfig *out)
{
    /* eglChooseConfig 未写 EGL_SURFACE_TYPE 时默认要求 WINDOW_BIT。
     * Mesa surfaceless 的 config 只有 PBUFFER（且带 EGL_OPENGL_BIT），
     * 默认匹配会得到 0 个。显式要 0：不要求任何 surface 位。渲染目标是
     * 自有 FBO，配合 EGL_KHR_surfaceless_context 用 EGL_NO_SURFACE。 */
    const EGLint plain[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_SURFACE_TYPE, 0,
        EGL_NONE
    };
    const EGLint pbuf[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_NONE
    };
    EGLint n = 0;
    if (!p_chooseConfig(g_dpy, want_pbuffer ? pbuf : plain, out, 1, &n) || n < 1)
        return 0;
    return 1;
}

static const EGLint g_ctx_attribs[] = {
    EGL_CONTEXT_MAJOR_VERSION, 4,
    EGL_CONTEXT_MINOR_VERSION, 1,
    EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
    EGL_NONE
};

static void drop_ctx(EGLContext ctx, EGLSurface surf)
{
    p_makeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surf != EGL_NO_SURFACE) p_destroySurface(g_dpy, surf);
    if (ctx != EGL_NO_CONTEXT) p_destroyContext(g_dpy, ctx);
}

nx_gl_ctx *nx_gl_ctx_create(const neox_gl_device_desc *desc)
{
    EGLConfig cfg;
    EGLContext share;
    EGLContext glctx;
    EGLSurface surf = EGL_NO_SURFACE;
    nx_gl_ctx *out;

    g_err = NULL;
    if (!load_lib() || !ensure_display()) return NULL;
    if (!p_bindAPI(EGL_OPENGL_API)) {
        set_err_egl("EGL: eglBindAPI(OpenGL) failed");
        return NULL;
    }
    share = (desc && desc->shared_context) ? (EGLContext)desc->shared_context
                                            : EGL_NO_CONTEXT;
    if (!choose_config(0, &cfg)) {
        set_err("EGL: no desktop OpenGL config");
        return NULL;
    }
    glctx = p_createContext(g_dpy, cfg, share, g_ctx_attribs);
    if (glctx == EGL_NO_CONTEXT) {
        set_err_egl("EGL: OpenGL 4.1 Core context creation failed");
        return NULL;
    }
    if (!p_makeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, glctx)) {
        /* surfaceless 不被这个 config 接受：换成 PBuffer 重建。 */
        drop_ctx(glctx, EGL_NO_SURFACE);
        glctx = EGL_NO_CONTEXT;
        if (!choose_config(1, &cfg)) {
            set_err("EGL: surfaceless make-current failed and no pbuffer config");
            return NULL;
        }
        {
            const EGLint sattr[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
            surf = p_createPbuffer(g_dpy, cfg, sattr);
        }
        if (surf == EGL_NO_SURFACE) {
            set_err_egl("EGL: pbuffer surface failed");
            return NULL;
        }
        glctx = p_createContext(g_dpy, cfg, share, g_ctx_attribs);
        if (glctx == EGL_NO_CONTEXT ||
            !p_makeCurrent(g_dpy, surf, surf, glctx)) {
            drop_ctx(glctx, surf);
            set_err_egl("EGL: could not make context current");
            return NULL;
        }
    }

    out = (nx_gl_ctx *)calloc(1, sizeof(*out));
    if (!out) {
        drop_ctx(glctx, surf);
        set_err("EGL: out of memory");
        return NULL;
    }
    out->ctx = glctx;
    out->surface = surf;
    g_refs++;
    return out;
}

int nx_gl_ctx_make_current(nx_gl_ctx *ctx)
{
    EGLSurface s;
    if (!ctx || ctx->ctx == EGL_NO_CONTEXT || g_dpy == EGL_NO_DISPLAY) return 0;
    s = ctx->surface;
    return p_makeCurrent(g_dpy, s, s, ctx->ctx) == EGL_TRUE;
}

void *nx_gl_ctx_get_proc(nx_gl_ctx *ctx, const char *name)
{
    nx_egl_fn fn;
    (void)ctx;
    if (!p_getProc || !name) return NULL;
    fn = p_getProc(name);
    {
        union { nx_egl_fn fp; void *vp; } u;
        u.fp = fn;
        return u.vp;
    }
}

void nx_gl_ctx_destroy(nx_gl_ctx *ctx)
{
    if (!ctx) return;
    if (g_dpy != EGL_NO_DISPLAY)
        drop_ctx(ctx->ctx, ctx->surface);
    free(ctx);
    if (g_refs > 0 && --g_refs == 0 && g_dpy != EGL_NO_DISPLAY) {
        p_terminate(g_dpy);
        g_dpy = EGL_NO_DISPLAY;
        g_kind = "egl-none";
    }
}
