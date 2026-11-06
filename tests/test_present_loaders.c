/* P0-1：动态库入口校验失败后不得缓存"已加载"。
 *
 * 让真正的加载器（present_wl.c / present_x11.c / gl_ctx_egl.c 的 load_*）
 * 通过 NX_TEST_HOOK_DLOPEN 走一张"缺符号的假库"：第一次加载必须失败、把
 * 句柄关闭并清空函数表；第二次调用不能把上一次的失败当成成功（否则会在
 * 空函数指针上崩溃，repro_loader/repro_egl_loader 的原始症状）。 */
/* 与 test_present_adapter_bounds.c 相同：先包含被编译单元的依赖，再包含
 * 实现，并保留 present_wl.c 需要的 _GNU_SOURCE / _POSIX_C_SOURCE。 */
#define _GNU_SOURCE
#include "../src/present_internal.h"
#include "../src/gl_ctx.h"
#include "neox.h"

#include <stdio.h>
#include <string.h>

#define NX_TEST_HOOK_DLOPEN 1
#include "../src/present_wl.c"
#include "../src/present_x11.c"

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

/* ---- 假 libwayland-client / libwayland-egl / libX11 ---------------------- */
static int wl_opens, wl_closes;
static int egl_opens, egl_closes;
static int x_opens, x_closes;

static const char *missing_sym;

static int name_is(const char *n, const char *want) { return strcmp(n, want) == 0; }

/* present_wl.c 的 libwayland-client 与 libwayland-egl 都经 nx_test_dlopen。 */
void *nx_test_dlopen(const char *name, int flags)
{
    (void)flags;
    if (strstr(name, "wayland-client")) { wl_opens++; return (void *)0x1000; }
    if (strstr(name, "wayland-egl")) { egl_opens++; return (void *)0x2000; }
    return NULL;
}
int nx_test_dlclose(void *h)
{
    if (h == (void *)0x1000) wl_closes++;
    if (h == (void *)0x2000) egl_closes++;
    return 0;
}
void *nx_test_dlsym(void *h, const char *sym)
{
    (void)h;
    if (missing_sym && name_is(sym, missing_sym)) return NULL;
    /* 接口表对象只是被存进函数表，测试中不会被解引用。 */
    return (void *)0x3000;
}
void *nx_test_egl_dlopen(const char *name, int flags)
{
    (void)name; (void)flags;
    egl_opens++; /* gl_ctx_egl.c 的 libEGL（由 test_egl_faults 覆盖） */
    return (void *)0x4000;
}
void *nx_test_egl_dlsym(void *h, const char *sym)
{
    (void)h;
    if (missing_sym && name_is(sym, missing_sym)) return NULL;
    return (void *)0x5000;
}
int nx_test_egl_dlclose(void *h) { (void)h; egl_closes++; return 0; }

void *nx_test_x11_dlopen(const char *name, int flags)
{
    (void)name; (void)flags;
    x_opens++;
    return (void *)0x6000;
}
void *nx_test_x11_dlsym(void *h, const char *sym)
{
    (void)h;
    if (missing_sym && name_is(sym, missing_sym)) return NULL;
    return (void *)0x7000;
}
int nx_test_x11_dlclose(void *h) { (void)h; x_closes++; return 0; }

/* 覆盖 P0-1 的四个库：libwayland-client、libwayland-egl、libX11（此处）与
 * libEGL（tests/test_egl_faults.c 用注入分发验证同一三态加载状态）。 */
int main(void)
{
    int first, second;

    /* 1. libwayland-client 缺 wl_proxy_marshal_flags：失败 → 关闭 → 不缓存。 */
    missing_sym = "wl_proxy_marshal_flags";
    wl_opens = wl_closes = 0;
    first = load_wl();
    second = load_wl();
    CHECK(first == 0 && second == 0);          /* 不能把失败当成成功 */
    CHECK(g_wl.lib == NULL && g_wl.marshal == NULL); /* 函数表已清空 */
    CHECK(wl_opens == 2 && wl_closes == 2);    /* 打开两次、失败两次都关闭 */
    CHECK(g_wl.connect == NULL && g_wl.compositor == NULL);
    if (g_wl.marshal) abort();                 /* 二次调用绝不能得到非空入口 */

    /* 2. 符号齐全后重新加载成功，且失败状态被正确重置。 */
    missing_sym = NULL;
    second = load_wl();
    CHECK(second == 1 && g_wl.lib != NULL && g_wl.marshal != NULL);

    /* 3. libwayland-egl 缺 wl_egl_window_create（review 中的 egl loader）。 */
    missing_sym = "wl_egl_window_create";
    egl_opens = egl_closes = 0;
    first = load_egl();
    second = load_egl();
    CHECK(first == 0 && second == 0);
    CHECK(g_egl_lib == NULL && g_ewin_create == NULL &&
          g_ewin_destroy == NULL && g_ewin_resize == NULL);
    /* 每次查询缺一个入口：第一次 dlopen 成功但校验失败（关闭一次），第二次
     * dlopen 仍拿到"库"，再次失败并关闭。 */
    CHECK(egl_opens == 2 && egl_closes == 2);
    if (g_ewin_create) abort();
    missing_sym = NULL;
    CHECK(load_egl() == 1 && g_ewin_create != NULL);

    /* 4. libX11 缺 XInternAtom：同一三态处理。load_x11 成功时还会把
     * XSetErrorHandler/XSetIOErrorHandler 换成本模块的处理器，这是进程级
     * 副作用；本测试只驱动失败路径，因此不在这里让假 dlsym 能真正装上函数
     * 指针（真实库由 tests 中的 X11 集成测试覆盖）。 */
    memset(&g_x, 0, sizeof g_x);
    g_x_state = NX_LOAD_UNTRIED;
    missing_sym = "XInternAtom";
    x_opens = x_closes = 0;
    first = load_x11();
    second = load_x11();
    CHECK(first == 0 && second == 0);
    CHECK(g_x.lib == NULL && g_x.cwin == NULL && g_x.hints == NULL &&
          g_x.atom == NULL);
    CHECK(x_opens == 2 && x_closes == 2);
    if (g_x.atom) abort();

    puts("ALL PASS (loader failure never cached; handles closed; tables cleared)");
    return 0;
}
