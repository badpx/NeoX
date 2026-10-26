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
#define EGL_WINDOW_BIT                      0x0004
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
#define EGL_PLATFORM_X11_KHR                0x31D5
#define EGL_PLATFORM_WAYLAND_KHR            0x31D8
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
typedef EGLSurface (*PFN_eglCreateWindowSurface)(EGLDisplay, EGLConfig, void *,
                                                 const EGLint *);
typedef EGLBoolean (*PFN_eglDestroySurface)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*PFN_eglSwapBuffers)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*PFN_eglSwapInterval)(EGLDisplay, EGLint);
typedef nx_egl_fn  (*PFN_eglGetProcAddress)(const char *);
typedef EGLDisplay (*PFN_eglGetPlatformDisplay)(EGLenum, void *, const EGLAttrib *);
typedef EGLBoolean (*PFN_eglQueryDevicesEXT)(EGLint, EGLDeviceEXT *, EGLint *);
typedef EGLBoolean (*PFN_eglGetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);

struct nx_gl_ctx {
    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface surface; /* 离屏 current 用的 NO_SURFACE 或 1×1 PBuffer */
    EGLConfig win_cfg;
    int native;         /* 1：独立于进程级 surfaceless display */
    int can_window;
    int lost;
    const char *kind;
};

#define NX_NATIVE_MAX 4
struct nx_native_dpy {
    EGLDisplay dpy;
    void *native;
    uint32_t kind;
    int refs;
};
static struct nx_native_dpy g_native[NX_NATIVE_MAX];

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
static PFN_eglCreateWindowSurface  p_createWindow;
static PFN_eglDestroySurface       p_destroySurface;
static PFN_eglSwapBuffers          p_swapBuffers;
static PFN_eglSwapInterval         p_swapInterval;
static PFN_eglGetPlatformDisplay   p_getPlatformDisplay;
static PFN_eglGetConfigAttrib      p_getConfigAttrib;

const char *nx_gl_ctx_last_error(void) { return g_err; }
const char *nx_gl_ctx_kind(const nx_gl_ctx *ctx)
{
    return (ctx && ctx->kind) ? ctx->kind : g_kind;
}

static void set_err(const char *msg) { g_err = msg; }

static void set_err_egl(const char *prefix)
{
    EGLint code = p_getError ? p_getError() : 0;
    snprintf(g_errbuf, sizeof g_errbuf, "%s (egl 0x%x)", prefix, (unsigned)code);
    g_err = g_errbuf;
}

/* Consume EGL's thread-local error before cleanup calls can replace it. */
static int ctx_error(nx_gl_ctx *ctx, const char *message)
{
    EGLint code = p_getError ? p_getError() : 0;
    if (ctx && code == 0x300E) ctx->lost = 1; /* EGL_CONTEXT_LOST */
    snprintf(g_errbuf, sizeof g_errbuf, "%s (egl 0x%x)", message, (unsigned)code);
    g_err = g_errbuf;
    return 0;
}

int nx_gl_ctx_is_lost(const nx_gl_ctx *ctx) { return ctx && ctx->lost; }

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
    NX_DLSYM(p_createWindow, PFN_eglCreateWindowSurface, "eglCreateWindowSurface");
    NX_DLSYM(p_destroySurface, PFN_eglDestroySurface, "eglDestroySurface");
    NX_DLSYM(p_swapBuffers, PFN_eglSwapBuffers, "eglSwapBuffers");
    NX_DLSYM(p_swapInterval, PFN_eglSwapInterval, "eglSwapInterval");
    if (!p_getProc || !p_getDisplay || !p_initialize || !p_terminate ||
        !p_bindAPI || !p_chooseConfig || !p_createContext || !p_destroyContext ||
        !p_makeCurrent || !p_getError || !p_createPbuffer || !p_destroySurface ||
        !p_createWindow || !p_swapBuffers) {
        set_err("EGL: libEGL is missing a required entry point");
        return 0;
    }
    NX_DLSYM(p_getPlatformDisplay, PFN_eglGetPlatformDisplay, "eglGetPlatformDisplay");
    NX_DLSYM(p_getConfigAttrib, PFN_eglGetConfigAttrib, "eglGetConfigAttrib");
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

static int choose_config(EGLDisplay dpy, int mode, EGLConfig *out)
{
    /* mode 0：不要求 surface 位。eglChooseConfig 缺省是 WINDOW_BIT，
     * Mesa surfaceless 的 config 只有 PBUFFER，不显式写会选到 0 个。
     * mode 1：PBuffer。mode 2：窗口。 */
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
    const EGLint win[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };
    /* 窗口 config 上 NO_SURFACE 失败时，上下文必须跟后来的 window surface
     * 用同一份 config，所以 PBuffer 回退仍要求 WINDOW_BIT。 */
    const EGLint winpbuf[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_NONE
    };
    const EGLint *attr = plain;
    if (mode == 1) attr = pbuf;
    else if (mode == 2) attr = win;
    else if (mode == 3) attr = winpbuf;
    EGLint n = 0;
    if (!p_chooseConfig(dpy, attr, out, 1, &n) || n < 1)
        return 0;
    return 1;
}

static const EGLint g_ctx_attribs[] = {
    EGL_CONTEXT_MAJOR_VERSION, 4,
    EGL_CONTEXT_MINOR_VERSION, 1,
    EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
    EGL_NONE
};

static void drop_ctx(EGLDisplay dpy, EGLContext ctx, EGLSurface surf)
{
    p_makeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surf != EGL_NO_SURFACE) p_destroySurface(dpy, surf);
    if (ctx != EGL_NO_CONTEXT) p_destroyContext(dpy, ctx);
}

static void native_release(EGLDisplay dpy)
{
    int i;
    for (i = 0; i < NX_NATIVE_MAX; i++) {
        if (g_native[i].dpy != dpy) continue;
        if (g_native[i].refs > 0 && --g_native[i].refs == 0) {
            p_terminate(dpy);
            g_native[i].dpy = EGL_NO_DISPLAY;
            g_native[i].native = NULL;
            g_native[i].kind = 0;
        }
        return;
    }
}

static EGLDisplay native_acquire(uint32_t kind, void *native)
{
    EGLenum plat;
    EGLDisplay dpy;
    int i, free_slot = -1;
    if (kind == NEOX_GL_NATIVE_X11) plat = EGL_PLATFORM_X11_KHR;
    else if (kind == NEOX_GL_NATIVE_WAYLAND) plat = EGL_PLATFORM_WAYLAND_KHR;
    else {
        set_err("EGL: unknown native display kind");
        return EGL_NO_DISPLAY;
    }
    if (!p_getPlatformDisplay) {
        set_err("EGL: eglGetPlatformDisplay is required for window displays");
        return EGL_NO_DISPLAY;
    }
    for (i = 0; i < NX_NATIVE_MAX; i++) {
        if (g_native[i].refs > 0 && g_native[i].native == native &&
            g_native[i].kind == kind) {
            g_native[i].refs++;
            return g_native[i].dpy;
        }
        if (g_native[i].refs == 0 && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) {
        set_err("EGL: too many native displays");
        return EGL_NO_DISPLAY;
    }
    dpy = p_getPlatformDisplay(plat, native, NULL);
    if (!try_init_display(dpy)) {
        set_err_egl("EGL: native display init failed");
        return EGL_NO_DISPLAY;
    }
    g_native[free_slot].dpy = dpy;
    g_native[free_slot].native = native;
    g_native[free_slot].kind = kind;
    g_native[free_slot].refs = 1;
    return dpy;
}

static int make_current_or_pbuffer(EGLDisplay dpy, EGLContext *glctx,
                                   EGLContext share, EGLSurface *surf,
                                   EGLConfig *cfg, int window_cfg)
{
    *surf = EGL_NO_SURFACE;
    if (!choose_config(dpy, window_cfg ? 2 : 0, cfg)) {
        set_err(window_cfg ? "EGL: no windowed OpenGL config"
                           : "EGL: no desktop OpenGL config");
        return 0;
    }
    *glctx = p_createContext(dpy, *cfg, share, g_ctx_attribs);
    if (*glctx == EGL_NO_CONTEXT) {
        set_err_egl("EGL: OpenGL 4.1 Core context creation failed");
        return 0;
    }
    if (p_makeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, *glctx))
        return 1;
    /* 这个 config 不接受 NO_SURFACE：换成 1×1 PBuffer 重建。 */
    drop_ctx(dpy, *glctx, EGL_NO_SURFACE);
    *glctx = EGL_NO_CONTEXT;
    if (!choose_config(dpy, window_cfg ? 3 : 1, cfg)) {
        set_err("EGL: make-current failed and no pbuffer config");
        return 0;
    }
    {
        const EGLint sattr[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        *surf = p_createPbuffer(dpy, *cfg, sattr);
    }
    if (*surf == EGL_NO_SURFACE) {
        set_err_egl("EGL: pbuffer surface failed");
        return 0;
    }
    *glctx = p_createContext(dpy, *cfg, share, g_ctx_attribs);
    if (*glctx == EGL_NO_CONTEXT ||
        !p_makeCurrent(dpy, *surf, *surf, *glctx)) {
        drop_ctx(dpy, *glctx, *surf);
        *glctx = EGL_NO_CONTEXT;
        *surf = EGL_NO_SURFACE;
        set_err_egl("EGL: could not make context current");
        return 0;
    }
    return 1;
}

nx_gl_ctx *nx_gl_ctx_create(const neox_gl_device_desc *desc)
{
    EGLConfig cfg = NULL;
    EGLContext share;
    EGLContext glctx = EGL_NO_CONTEXT;
    EGLSurface surf = EGL_NO_SURFACE;
    EGLDisplay dpy;
    nx_gl_ctx *out;
    int native = 0;
    const char *kind = "egl-surfaceless";

    g_err = NULL;
    if (!load_lib()) return NULL;
    if (!p_bindAPI(EGL_OPENGL_API)) {
        set_err_egl("EGL: eglBindAPI(OpenGL) failed");
        return NULL;
    }
    share = (desc && desc->shared_context) ? (EGLContext)desc->shared_context
                                            : EGL_NO_CONTEXT;
    if (desc && desc->native_display) {
        dpy = native_acquire(desc->native_kind, desc->native_display);
        if (dpy == EGL_NO_DISPLAY) return NULL;
        native = 1;
        kind = desc->native_kind == NEOX_GL_NATIVE_WAYLAND ? "egl-wayland"
                                                           : "egl-x11";
        if (!make_current_or_pbuffer(dpy, &glctx, share, &surf, &cfg, 1)) {
            native_release(dpy);
            return NULL;
        }
    } else {
        if (!ensure_display()) return NULL;
        dpy = g_dpy;
        kind = g_kind;
        if (!make_current_or_pbuffer(dpy, &glctx, share, &surf, &cfg, 0))
            return NULL;
    }

    out = (nx_gl_ctx *)calloc(1, sizeof(*out));
    if (!out) {
        drop_ctx(dpy, glctx, surf);
        if (native) native_release(dpy);
        set_err("EGL: out of memory");
        return NULL;
    }
    out->dpy = dpy;
    out->ctx = glctx;
    out->surface = surf;
    out->win_cfg = native ? cfg : NULL;
    out->native = native;
    out->can_window = native ? 1 : 0;
    out->kind = kind;
    if (!native) g_refs++;
    return out;
}

int nx_gl_ctx_make_current(nx_gl_ctx *ctx)
{
    EGLSurface s;
    if (!ctx || ctx->ctx == EGL_NO_CONTEXT || ctx->dpy == EGL_NO_DISPLAY) return 0;
    s = ctx->surface;
    if (p_makeCurrent(ctx->dpy, s, s, ctx->ctx) == EGL_TRUE) return 1;
    return ctx_error(ctx, "EGL: could not make context current");
}

int nx_gl_ctx_can_window(const nx_gl_ctx *ctx)
{
    return ctx && ctx->can_window && ctx->win_cfg;
}

void *nx_gl_ctx_window_surface(nx_gl_ctx *ctx, void *native_window, int vsync)
{
    EGLSurface s;
    const EGLint attr[] = { EGL_NONE };
    if (!nx_gl_ctx_can_window(ctx) || !native_window || !p_createWindow) {
        set_err("EGL: this context has no window display");
        return NULL;
    }
    s = p_createWindow(ctx->dpy, ctx->win_cfg, native_window, attr);
    if (s == EGL_NO_SURFACE) {
        ctx_error(ctx, "EGL: window surface failed");
        return NULL;
    }
    if (!p_makeCurrent(ctx->dpy, s, s, ctx->ctx)) {
        ctx_error(ctx, "EGL: could not make window surface current");
        p_destroySurface(ctx->dpy, s);
        return NULL;
    }
    /* 交换间隔失败（Xvfb、部分驱动）不让窗口表面作废。 */
    if (p_swapInterval && !p_swapInterval(ctx->dpy, vsync ? 1 : 0)) {
        ctx_error(ctx, "EGL: swap interval failed");
        if (ctx->lost) { p_destroySurface(ctx->dpy, s); return NULL; }
    }
    return s;
}

void nx_gl_ctx_window_destroy(nx_gl_ctx *ctx, void *egl_surface)
{
    if (!ctx || !egl_surface || !p_destroySurface) return;
    if (ctx->dpy != EGL_NO_DISPLAY)
        p_destroySurface(ctx->dpy, (EGLSurface)egl_surface);
}

int nx_gl_ctx_window_make_current(nx_gl_ctx *ctx, void *egl_surface)
{
    if (!ctx || !egl_surface) return 0;
    if (p_makeCurrent(ctx->dpy, (EGLSurface)egl_surface,
                     (EGLSurface)egl_surface, ctx->ctx) == EGL_TRUE) return 1;
    return ctx_error(ctx, "EGL: could not make window current");
}

int nx_gl_ctx_window_swap(nx_gl_ctx *ctx, void *egl_surface)
{
    if (!ctx || !egl_surface || !p_swapBuffers) return 0;
    if (p_swapBuffers(ctx->dpy, (EGLSurface)egl_surface) == EGL_TRUE) return 1;
    return ctx_error(ctx, "EGL: swap buffers failed");
}

int nx_gl_ctx_native_visual(const nx_gl_ctx *ctx, unsigned long *out_id)
{
    EGLint id = 0;
    if (!ctx || !ctx->win_cfg || !out_id || !p_getConfigAttrib) return 0;
    /* EGL_NATIVE_VISUAL_ID。Wayland 上通常为 0。 */
    if (!p_getConfigAttrib(ctx->dpy, ctx->win_cfg, 0x302E, &id) || id == 0)
        return 0;
    *out_id = (unsigned long)id;
    return 1;
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
    EGLDisplay dpy;
    int native;
    if (!ctx) return;
    dpy = ctx->dpy;
    native = ctx->native;
    if (dpy != EGL_NO_DISPLAY)
        drop_ctx(dpy, ctx->ctx, ctx->surface);
    free(ctx);
    if (native) {
        native_release(dpy);
        return;
    }
    if (g_refs > 0 && --g_refs == 0 && g_dpy != EGL_NO_DISPLAY) {
        p_terminate(g_dpy);
        g_dpy = EGL_NO_DISPLAY;
        g_kind = "egl-none";
    }
}
