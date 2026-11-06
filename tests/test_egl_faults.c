/* Test the real EGL adapter with injected dispatch; no display or driver needed. */
#define NX_TEST_HOOK_DLOPEN 1
#include "../src/gl_ctx_egl.c"

/* 评审 4489969142：libEGL 的加载器（gl_ctx_egl.c 的 load_lib）必须真正被
 * 驱动一次——只声明 EGL hook 不等于运行了这段加载路径。这里用"缺一个入口
 * 的假 libEGL"走真实 dlopen/dlsym/dlclose 钩子：第一次失败必须关闭句柄并
 * 清空整张函数表，补齐后第二次必须重新加载成功。 */
static int egl_dlopen_calls, egl_dlclose_calls;
const char *egl_missing_symbol;
void *nx_test_egl_dlopen(const char *name, int flags)
{
    (void)name; (void)flags;
    egl_dlopen_calls++;
    return (void *)(uintptr_t)0xE61;
}
void *nx_test_egl_dlsym(void *handle, const char *symbol)
{
    (void)handle;
    if (egl_missing_symbol && strcmp(symbol, egl_missing_symbol) == 0) return NULL;
    return (void *)(uintptr_t)0xE61;
}
int nx_test_egl_dlclose(void *handle)
{
    (void)handle;
    egl_dlclose_calls++;
    return 0;
}
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static EGLint error_code;
static int current_ok, swap_ok, cleanup_calls;
static EGLint get_error(void) { EGLint e = error_code; error_code = 0; return e; }
static EGLBoolean make_current(EGLDisplay d, EGLSurface r, EGLSurface w, EGLContext c)
{ (void)d; (void)r; (void)w; (void)c; return current_ok ? EGL_TRUE : 0; }
static EGLBoolean swap_buffers(EGLDisplay d, EGLSurface s)
{ (void)d; (void)s; return swap_ok ? EGL_TRUE : 0; }
static EGLSurface create_window(EGLDisplay d, EGLConfig c, void *w, const EGLint *a)
{ (void)d; (void)c; (void)w; (void)a; return (EGLSurface)(uintptr_t)2; }
static EGLBoolean destroy_surface(EGLDisplay d, EGLSurface s)
{ (void)d; (void)s; cleanup_calls++; error_code = 0; return EGL_TRUE; }

static unsigned shell_allocs, shell_frees;
static void *shell_alloc(void *user, size_t bytes)
{ CHECK(user == &shell_allocs); ++shell_allocs; return malloc(bytes); }
static void shell_free(void *user, void *p)
{ CHECK(user == &shell_allocs); ++shell_frees; free(p); }

int main(void)
{
    nx_gl_ctx *shell = nx_gl_ctx_alloc_host(NULL, NULL, NULL);
    CHECK(shell && !shell->dealloc);
    nx_gl_ctx_free_host(shell);
    CHECK(!nx_gl_ctx_alloc_host(shell_alloc, NULL, &shell_allocs));
    shell = nx_gl_ctx_alloc_host(shell_alloc, shell_free, &shell_allocs);
    CHECK(shell && shell_allocs == 1);
    nx_gl_ctx_free_host(shell);
    CHECK(shell_frees == 1);

    /* libEGL 加载器：失败 → dlclose + 清表 → 补齐后重新加载成功。 */
    {
        int first, second;
        egl_missing_symbol = "eglDestroyContext";
        egl_dlopen_calls = egl_dlclose_calls = 0;
        first = load_lib();
        second = load_lib();
        CHECK(!first && !second);
        CHECK(g_lib == NULL && p_destroyContext == NULL && p_makeCurrent == NULL);
        CHECK(g_load_state == NX_EGL_LOAD_FAILED);
        CHECK(egl_dlopen_calls == 2 && egl_dlclose_calls == 2);
        egl_missing_symbol = NULL;
        CHECK(load_lib() == 1);
        CHECK(g_lib != NULL && p_destroyContext != NULL && p_getError != NULL);
        CHECK(g_load_state == NX_EGL_LOAD_OK);
        /* 再次进入成功态不重复 dlopen。 */
        egl_dlopen_calls = 0;
        CHECK(load_lib() == 1 && egl_dlopen_calls == 0);
        unload_lib();
    }
    nx_gl_ctx ctx = {0};
    ctx.dpy = (EGLDisplay)(uintptr_t)1; ctx.ctx = (EGLContext)(uintptr_t)1;
    ctx.win_cfg = (EGLConfig)(uintptr_t)1; ctx.can_window = 1;
    p_getError = get_error; p_makeCurrent = make_current; p_swapBuffers = swap_buffers;
    p_createWindow = create_window; p_destroySurface = destroy_surface;
    void *surface = (void *)(uintptr_t)2;
    current_ok = swap_ok = 1;
    CHECK(nx_gl_ctx_make_current(&ctx)); CHECK(nx_gl_ctx_window_swap(&ctx, surface));
    CHECK(!nx_gl_ctx_is_lost(&ctx));
    current_ok = 0; error_code = 0x300D; /* EGL_BAD_SURFACE, recoverable */
    CHECK(!nx_gl_ctx_window_make_current(&ctx, surface)); CHECK(!ctx.lost);
    error_code = 0x300E; /* EGL_CONTEXT_LOST */
    CHECK(!nx_gl_ctx_make_current(&ctx)); CHECK(ctx.lost);
    current_ok = 1; CHECK(nx_gl_ctx_make_current(&ctx)); CHECK(ctx.lost);
    ctx.lost = 0; swap_ok = 0; error_code = 0x300E;
    CHECK(!nx_gl_ctx_window_swap(&ctx, surface)); CHECK(ctx.lost);
    /* Preserve loss before destroying the failed window surface consumes error state. */
    ctx.lost = 0; current_ok = 0; error_code = 0x300E;
    CHECK(!nx_gl_ctx_window_surface(&ctx, surface, 0));
    CHECK(cleanup_calls == 1 && ctx.lost);
    puts("ALL PASS (EGL bind/swap loss, sticky state, cleanup error preservation)");
    return 0;
}
