/* Test the real EGL adapter with injected dispatch; no display or driver needed. */
#include "../src/gl_ctx_egl.c"
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

int main(void)
{
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
