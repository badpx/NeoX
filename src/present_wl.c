#define _GNU_SOURCE
/* ===========================================================================
 * present_wl.c — Wayland 呈现。dlopen libwayland-client / libwayland-egl。
 *
 * 不包含 wayland 头文件，这样没装 -dev 的 CI 也能编。核心 wl_*_interface
 * 从 libwayland-client 取；xdg-shell 的接口表按 stable 协议手写（与
 * wayland-scanner 1.22 的 private-code 一致），因为 .so 里没有它们。
 * 绑定版本压到本层实际处理的事件：xdg ≤3、seat ≤5、output ≤2。
 * =========================================================================*/
#include "present_internal.h"

#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define NX_MARSHAL_DESTROY 1u

struct wl_message {
    const char *name;
    const char *signature;
    const struct wl_interface **types;
};

struct wl_interface {
    const char *name;
    int version;
    int method_count;
    const struct wl_message *methods;
    int event_count;
    const struct wl_message *events;
};

typedef void *(*PFN_connect)(const char *);
typedef void (*PFN_disconnect)(void *);
typedef int (*PFN_disp)(void *);
typedef int (*PFN_round)(void *);
typedef int (*PFN_flush)(void *);
typedef int (*PFN_fd)(void *);
typedef int (*PFN_prep)(void *);
typedef int (*PFN_read)(void *);
typedef void (*PFN_cancel)(void *);
typedef void *(*PFN_marshal)(void *, uint32_t, const struct wl_interface *,
                             uint32_t, uint32_t, ...);
typedef int (*PFN_listen)(void *, void *, void *);
typedef uint32_t (*PFN_ver)(void *);
typedef void (*PFN_pdestroy)(void *);
typedef void *(*PFN_ewin)(void *, int, int);
typedef void (*PFN_eresize)(void *, int, int, int, int);
typedef void (*PFN_edestroy)(void *);

static struct {
    void *lib;
    PFN_connect connect;
    PFN_disconnect disconnect;
    PFN_disp dispatch;
    PFN_disp pending;
    PFN_round roundtrip;
    PFN_flush flush;
    PFN_fd get_fd;
    PFN_prep prepare;
    PFN_read read_events;
    PFN_cancel cancel;
    PFN_marshal marshal;
    PFN_listen listen;
    PFN_ver version;
    PFN_pdestroy destroy;
    const struct wl_interface *registry;
    const struct wl_interface *compositor;
    const struct wl_interface *shm;
    const struct wl_interface *pool;
    const struct wl_interface *buffer;
    const struct wl_interface *surface;
    const struct wl_interface *seat;
    const struct wl_interface *pointer;
    const struct wl_interface *keyboard;
    const struct wl_interface *output;
    const struct wl_interface *callback;
} g_wl;

static void *g_egl_lib;
static PFN_ewin g_ewin_create;
static PFN_eresize g_ewin_resize;
static PFN_edestroy g_ewin_destroy;

static const struct wl_interface *xdg_types[26];

static const struct wl_message xdg_wm_base_requests[] = {
    { "destroy", "", xdg_types + 0 },
    { "create_positioner", "n", xdg_types + 4 },
    { "get_xdg_surface", "no", xdg_types + 5 },
    { "pong", "u", xdg_types + 0 },
};
static const struct wl_message xdg_wm_base_events[] = {
    { "ping", "u", xdg_types + 0 },
};
static const struct wl_interface xdg_wm_base_interface = {
    "xdg_wm_base", 7, 4, xdg_wm_base_requests, 1, xdg_wm_base_events
};

static const struct wl_message xdg_positioner_requests[] = {
    { "destroy", "", xdg_types + 0 },
    { "set_size", "ii", xdg_types + 0 },
    { "set_anchor_rect", "iiii", xdg_types + 0 },
    { "set_anchor", "u", xdg_types + 0 },
    { "set_gravity", "u", xdg_types + 0 },
    { "set_constraint_adjustment", "u", xdg_types + 0 },
    { "set_offset", "ii", xdg_types + 0 },
    { "set_reactive", "3", xdg_types + 0 },
    { "set_parent_size", "3ii", xdg_types + 0 },
    { "set_parent_configure", "3u", xdg_types + 0 },
};
static const struct wl_interface xdg_positioner_interface = {
    "xdg_positioner", 7, 10, xdg_positioner_requests, 0, NULL
};

static const struct wl_message xdg_surface_requests[] = {
    { "destroy", "", xdg_types + 0 },
    { "get_toplevel", "n", xdg_types + 7 },
    { "get_popup", "n?oo", xdg_types + 8 },
    { "set_window_geometry", "iiii", xdg_types + 0 },
    { "ack_configure", "u", xdg_types + 0 },
};
static const struct wl_message xdg_surface_events[] = {
    { "configure", "u", xdg_types + 0 },
};
static const struct wl_interface xdg_surface_interface = {
    "xdg_surface", 7, 5, xdg_surface_requests, 1, xdg_surface_events
};

static const struct wl_message xdg_toplevel_requests[] = {
    { "destroy", "", xdg_types + 0 },
    { "set_parent", "?o", xdg_types + 11 },
    { "set_title", "s", xdg_types + 0 },
    { "set_app_id", "s", xdg_types + 0 },
    { "show_window_menu", "ouii", xdg_types + 12 },
    { "move", "ou", xdg_types + 16 },
    { "resize", "ouu", xdg_types + 18 },
    { "set_max_size", "ii", xdg_types + 0 },
    { "set_min_size", "ii", xdg_types + 0 },
    { "set_maximized", "", xdg_types + 0 },
    { "unset_maximized", "", xdg_types + 0 },
    { "set_fullscreen", "?o", xdg_types + 21 },
    { "unset_fullscreen", "", xdg_types + 0 },
    { "set_minimized", "", xdg_types + 0 },
};
static const struct wl_message xdg_toplevel_events[] = {
    { "configure", "iia", xdg_types + 0 },
    { "close", "", xdg_types + 0 },
    { "configure_bounds", "4ii", xdg_types + 0 },
    { "wm_capabilities", "5a", xdg_types + 0 },
};
static const struct wl_interface xdg_toplevel_interface = {
    "xdg_toplevel", 7, 14, xdg_toplevel_requests, 4, xdg_toplevel_events
};

static const struct wl_message xdg_popup_requests[] = {
    { "destroy", "", xdg_types + 0 },
    { "grab", "ou", xdg_types + 22 },
    { "reposition", "3ou", xdg_types + 24 },
};
static const struct wl_message xdg_popup_events[] = {
    { "configure", "iiii", xdg_types + 0 },
    { "popup_done", "", xdg_types + 0 },
    { "repositioned", "3u", xdg_types + 0 },
};
static const struct wl_interface xdg_popup_interface = {
    "xdg_popup", 7, 3, xdg_popup_requests, 3, xdg_popup_events
};

static int xdg_filled;

static const struct wl_interface *iface_sym(const char *name)
{
    return (const struct wl_interface *)dlsym(g_wl.lib, name);
}

static int fill_xdg(void)
{
    if (xdg_filled) return 1;
    g_wl.surface = iface_sym("wl_surface_interface");
    g_wl.seat = iface_sym("wl_seat_interface");
    g_wl.output = iface_sym("wl_output_interface");
    if (!g_wl.surface || !g_wl.seat || !g_wl.output) return 0;
    xdg_types[4] = &xdg_positioner_interface;
    xdg_types[5] = &xdg_surface_interface;
    xdg_types[6] = g_wl.surface;
    xdg_types[7] = &xdg_toplevel_interface;
    xdg_types[8] = &xdg_popup_interface;
    xdg_types[9] = &xdg_surface_interface;
    xdg_types[10] = &xdg_positioner_interface;
    xdg_types[11] = &xdg_toplevel_interface;
    xdg_types[12] = g_wl.seat;
    xdg_types[16] = g_wl.seat;
    xdg_types[18] = g_wl.seat;
    xdg_types[21] = g_wl.output;
    xdg_types[22] = g_wl.seat;
    xdg_types[24] = &xdg_positioner_interface;
    xdg_filled = 1;
    return 1;
}

static int load_wl(void)
{
    if (g_wl.lib) return 1;
    g_wl.lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!g_wl.lib) g_wl.lib = dlopen("libwayland-client.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_wl.lib) {
        const char *de = dlerror();
        char buf[256];
        snprintf(buf, sizeof buf, "Wayland: dlopen libwayland-client.so.0 failed%s%s",
                 de ? ": " : "", de ? de : "");
        nx_win_set_error(buf);
        return 0;
    }
#define NX_DLSYM(field, type, sym) do { \
        union { void *vp; type fp; } u_; \
        u_.vp = dlsym(g_wl.lib, sym); \
        g_wl.field = u_.fp; \
    } while (0)
    NX_DLSYM(connect, PFN_connect, "wl_display_connect");
    NX_DLSYM(disconnect, PFN_disconnect, "wl_display_disconnect");
    NX_DLSYM(dispatch, PFN_disp, "wl_display_dispatch");
    NX_DLSYM(pending, PFN_disp, "wl_display_dispatch_pending");
    NX_DLSYM(roundtrip, PFN_round, "wl_display_roundtrip");
    NX_DLSYM(flush, PFN_flush, "wl_display_flush");
    NX_DLSYM(get_fd, PFN_fd, "wl_display_get_fd");
    NX_DLSYM(prepare, PFN_prep, "wl_display_prepare_read");
    NX_DLSYM(read_events, PFN_read, "wl_display_read_events");
    NX_DLSYM(cancel, PFN_cancel, "wl_display_cancel_read");
    NX_DLSYM(marshal, PFN_marshal, "wl_proxy_marshal_flags");
    NX_DLSYM(listen, PFN_listen, "wl_proxy_add_listener");
    NX_DLSYM(version, PFN_ver, "wl_proxy_get_version");
    NX_DLSYM(destroy, PFN_pdestroy, "wl_proxy_destroy");
#undef NX_DLSYM
    g_wl.registry = iface_sym("wl_registry_interface");
    g_wl.compositor = iface_sym("wl_compositor_interface");
    g_wl.shm = iface_sym("wl_shm_interface");
    g_wl.pool = iface_sym("wl_shm_pool_interface");
    g_wl.buffer = iface_sym("wl_buffer_interface");
    g_wl.pointer = iface_sym("wl_pointer_interface");
    g_wl.keyboard = iface_sym("wl_keyboard_interface");
    g_wl.callback = iface_sym("wl_callback_interface");
    if (!g_wl.connect || !g_wl.disconnect || !g_wl.roundtrip || !g_wl.pending ||
        !g_wl.flush || !g_wl.get_fd || !g_wl.prepare || !g_wl.read_events ||
        !g_wl.cancel || !g_wl.marshal || !g_wl.listen || !g_wl.version ||
        !g_wl.destroy || !g_wl.registry || !g_wl.compositor || !g_wl.shm ||
        !g_wl.pool || !g_wl.buffer || !g_wl.pointer || !g_wl.keyboard ||
        !g_wl.callback || !fill_xdg()) {
        nx_win_set_error("Wayland: libwayland-client is missing a required entry");
        return 0;
    }
    return 1;
}

static int load_egl(void)
{
    if (g_egl_lib) return 1;
    g_egl_lib = dlopen("libwayland-egl.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g_egl_lib) g_egl_lib = dlopen("libwayland-egl.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_egl_lib) {
        nx_win_set_error("Wayland: dlopen libwayland-egl.so.1 failed");
        return 0;
    }
#define NX_DLSYM(dst, type, sym) do { \
        union { void *vp; type fp; } u_; \
        u_.vp = dlsym(g_egl_lib, sym); \
        dst = u_.fp; \
    } while (0)
    NX_DLSYM(g_ewin_create, PFN_ewin, "wl_egl_window_create");
    NX_DLSYM(g_ewin_resize, PFN_eresize, "wl_egl_window_resize");
    NX_DLSYM(g_ewin_destroy, PFN_edestroy, "wl_egl_window_destroy");
#undef NX_DLSYM
    if (!g_ewin_create || !g_ewin_resize || !g_ewin_destroy) {
        nx_win_set_error("Wayland: libwayland-egl is missing an entry point");
        return 0;
    }
    return 1;
}

typedef struct nx_wl {
    neox_window *win;
    void *dpy;
    void *registry;
    void *compositor;
    void *shm;
    void *wm;
    void *seat;
    void *pointer;
    void *keyboard;
    void *output;
    void *surface;
    void *xdg_surface;
    void *toplevel;
    void *egl_window;
    uint32_t comp_ver, wm_ver, seat_ver, surf_ver;
    int output_scale;
    int configured;
    int opened;
    int dead;
    int frame_done;
    uint32_t buttons;
    int shm_fd;
    void *map;
    size_t map_bytes;
    void *pool;
    void *bufs[2];
    int busy[2];
    uint32_t buf_w, buf_h;
    uint32_t egl_w, egl_h;
} nx_wl;

static uint32_t clamp_ver(uint32_t server, uint32_t cap)
{
    if (server < 1) return 1;
    return server < cap ? server : cap;
}

static int memfd_new(size_t size)
{
#ifdef SYS_memfd_create
    int fd = (int)syscall(SYS_memfd_create, "neox", 1 /* MFD_CLOEXEC */);
#else
    int fd = -1;
#endif
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int wl_pump(nx_wl *p, int timeout_ms)
{
    struct pollfd fds;
    int pr;
    if (!p || !p->dpy || p->dead) return -1;
    if (g_wl.pending(p->dpy) < 0) {
        p->dead = 1;
        return -1;
    }
    while (g_wl.prepare(p->dpy) != 0) {
        if (g_wl.pending(p->dpy) < 0) {
            p->dead = 1;
            return -1;
        }
    }
    if (g_wl.flush(p->dpy) < 0 && errno != EAGAIN) {
        g_wl.cancel(p->dpy);
        p->dead = 1;
        return -1;
    }
    fds.fd = g_wl.get_fd(p->dpy);
    fds.events = POLLIN;
    fds.revents = 0;
    pr = poll(&fds, 1, timeout_ms);
    if (pr > 0) {
        if (g_wl.read_events(p->dpy) < 0 || g_wl.pending(p->dpy) < 0) {
            p->dead = 1;
            return -1;
        }
    } else {
        g_wl.cancel(p->dpy);
        if (pr < 0 && errno != EINTR) {
            p->dead = 1;
            return -1;
        }
    }
    return p->dead ? -1 : 0;
}

static void drop_buffers(nx_wl *p)
{
    int i;
    for (i = 0; i < 2; i++) {
        if (!p->bufs[i]) continue;
        g_wl.marshal(p->bufs[i], 0, NULL, 1, NX_MARSHAL_DESTROY);
        p->bufs[i] = NULL;
        p->busy[i] = 0;
    }
    if (p->pool) {
        g_wl.marshal(p->pool, 1, NULL, 1, NX_MARSHAL_DESTROY);
        p->pool = NULL;
    }
    if (p->map && p->map != MAP_FAILED) munmap(p->map, p->map_bytes);
    p->map = NULL;
    p->map_bytes = 0;
    if (p->shm_fd >= 0) close(p->shm_fd);
    p->shm_fd = -1;
    p->buf_w = 0;
    p->buf_h = 0;
}

static void wl_close(neox_window *w)
{
    nx_wl *p = (nx_wl *)w->plat;
    if (!p) return;
    if (p->egl_window && g_ewin_destroy) g_ewin_destroy(p->egl_window);
    p->egl_window = NULL;
    drop_buffers(p);
    if (p->dpy && g_wl.marshal) {
        if (p->toplevel)
            g_wl.marshal(p->toplevel, 0, NULL, p->wm_ver ? p->wm_ver : 1, NX_MARSHAL_DESTROY);
        if (p->xdg_surface)
            g_wl.marshal(p->xdg_surface, 0, NULL, p->wm_ver ? p->wm_ver : 1, NX_MARSHAL_DESTROY);
        if (p->surface)
            g_wl.marshal(p->surface, 0, NULL, p->surf_ver ? p->surf_ver : 1, NX_MARSHAL_DESTROY);
        if (p->wm)
            g_wl.marshal(p->wm, 0, NULL, p->wm_ver ? p->wm_ver : 1, NX_MARSHAL_DESTROY);
    }
    if (g_wl.destroy) {
        if (p->pointer) g_wl.destroy(p->pointer);
        if (p->keyboard) g_wl.destroy(p->keyboard);
        if (p->seat) g_wl.destroy(p->seat);
        if (p->output) g_wl.destroy(p->output);
        if (p->shm) g_wl.destroy(p->shm);
        if (p->compositor) g_wl.destroy(p->compositor);
        if (p->registry) g_wl.destroy(p->registry);
    }
    if (p->dpy && g_wl.disconnect) g_wl.disconnect(p->dpy);
    free(p);
    w->plat = NULL;
}

static int shell_scale(const neox_window *w)
{
    double s = floor((double)w->scale + 0.5);
    if (!isfinite(s) || s > INT32_MAX) return 0;
    return s < 1 ? 1 : (int)s;
}

static void apply_geom(nx_wl *p)
{
    neox_window *w = p->win;
    int scale = shell_scale(w);
    uint32_t ver;
    if (!p->xdg_surface || !p->surface) return;
    ver = g_wl.version(p->xdg_surface);
    g_wl.marshal(p->xdg_surface, 3, NULL, ver, 0, 0, 0,
                 (int32_t)w->pt_w, (int32_t)w->pt_h);
    if (p->toplevel) {
        uint32_t tv = g_wl.version(p->toplevel);
        g_wl.marshal(p->toplevel, 8, NULL, tv, 0, (int32_t)w->pt_w, (int32_t)w->pt_h);
        g_wl.marshal(p->toplevel, 7, NULL, tv, 0, (int32_t)w->pt_w, (int32_t)w->pt_h);
    }
    if (p->surf_ver >= 3)
        g_wl.marshal(p->surface, 8, NULL, p->surf_ver, 0, scale);
    if (p->egl_window && g_ewin_resize &&
        (p->egl_w != w->px_w || p->egl_h != w->px_h)) {
        g_ewin_resize(p->egl_window, (int)w->px_w, (int)w->px_h, 0, 0);
        p->egl_w = w->px_w;
        p->egl_h = w->px_h;
    }
}

static void on_ping(void *data, void *wm, uint32_t serial)
{
    nx_wl *p = (nx_wl *)data;
    uint32_t ver;
    (void)p;
    ver = g_wl.version(wm);
    g_wl.marshal(wm, 3, NULL, ver, 0, serial);
}

static void on_xdg_configure(void *data, void *xdg_surface, uint32_t serial)
{
    nx_wl *p = (nx_wl *)data;
    uint32_t ver = g_wl.version(xdg_surface);
    g_wl.marshal(xdg_surface, 4, NULL, ver, 0, serial);
    p->configured = 1;
}

static void on_tl_configure(void *data, void *tl, int32_t width, int32_t height,
                            void *states)
{
    nx_wl *p = (nx_wl *)data;
    neox_window_event ev;
    (void)tl;
    (void)states;
    if (!p->opened || width <= 0 || height <= 0) return;
    if ((uint32_t)width == p->win->pt_w && (uint32_t)height == p->win->pt_h)
        return;
    if (nx_win_set_geometry(p->win, (uint32_t)width, (uint32_t)height,
                            p->win->scale) != NEOX_OK) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = NEOX_WINDOW_RESIZE;
    ev.width = p->win->pt_w;
    ev.height = p->win->pt_h;
    ev.scale = p->win->scale;
    nx_win_push_event(p->win, &ev);
    apply_geom(p);
}

static void on_tl_close(void *data, void *tl)
{
    nx_wl *p = (nx_wl *)data;
    neox_window_event ev;
    (void)tl;
    memset(&ev, 0, sizeof ev);
    ev.kind = NEOX_WINDOW_CLOSE;
    nx_win_push_event(p->win, &ev);
}

static void on_tl_ignore(void *data, void *tl, int32_t a, int32_t b)
{
    (void)data;
    (void)tl;
    (void)a;
    (void)b;
}

static void on_tl_caps(void *data, void *tl, void *caps)
{
    (void)data;
    (void)tl;
    (void)caps;
}

static void on_out_geom(void *data, void *out, int32_t x, int32_t y,
                        int32_t pw, int32_t ph, int32_t sub,
                        const char *make, const char *model, int32_t transform)
{
    (void)data; (void)out; (void)x; (void)y; (void)pw; (void)ph;
    (void)sub; (void)make; (void)model; (void)transform;
}

static void on_out_mode(void *data, void *out, uint32_t flags, int32_t w,
                        int32_t h, int32_t refresh)
{
    (void)data; (void)out; (void)flags; (void)w; (void)h; (void)refresh;
}

static void on_out_done(void *data, void *out)
{
    (void)data;
    (void)out;
}

static void on_out_scale(void *data, void *out, int32_t factor)
{
    nx_wl *p = (nx_wl *)data;
    neox_window_event ev;
    (void)out;
    if (factor < 1) factor = 1;
    p->output_scale = factor;
    if (!p->win || p->win->scale_locked || !p->opened) return;
    if (p->win->scale == (float)factor) return;
    if (nx_win_set_geometry(p->win, p->win->pt_w, p->win->pt_h,
                            (float)factor) != NEOX_OK) return;
    memset(&ev, 0, sizeof ev);
    ev.kind = NEOX_WINDOW_SCALE;
    ev.scale = p->win->scale;
    ev.width = p->win->pt_w;
    ev.height = p->win->pt_h;
    nx_win_push_event(p->win, &ev);
    apply_geom(p);
}

static uint32_t evdev_keysym(uint32_t key)
{
    static const char row1[] = "1234567890";
    static const char rowq[] = "qwertyuiop";
    static const char rowa[] = "asdfghjkl";
    static const char rowz[] = "zxcvbnm";
    if (key == 1) return 0xff1bu;
    if (key == 14) return 0xff08u;
    if (key == 15) return 0xff09u;
    if (key == 28) return 0xff0du;
    if (key == 57) return (uint32_t)' ';
    if (key >= 2 && key <= 11) return (uint32_t)row1[key - 2];
    if (key >= 16 && key <= 25) return (uint32_t)rowq[key - 16];
    if (key >= 30 && key <= 38) return (uint32_t)rowa[key - 30];
    if (key >= 44 && key <= 50) return (uint32_t)rowz[key - 44];
    return key;
}

static void on_keymap(void *data, void *kbd, uint32_t format, int32_t fd, uint32_t size)
{
    (void)data; (void)kbd; (void)format; (void)size;
    if (fd >= 0) close(fd);
}

static void on_kb_enter(void *data, void *kbd, uint32_t serial, void *surface, void *keys)
{
    (void)data; (void)kbd; (void)serial; (void)surface; (void)keys;
}

static void on_kb_leave(void *data, void *kbd, uint32_t serial, void *surface)
{
    (void)data; (void)kbd; (void)serial; (void)surface;
}

static void on_key(void *data, void *kbd, uint32_t serial, uint32_t time,
                   uint32_t key, uint32_t state)
{
    nx_wl *p = (nx_wl *)data;
    neox_window_event ev;
    (void)kbd; (void)serial; (void)time;
    memset(&ev, 0, sizeof ev);
    ev.kind = NEOX_WINDOW_KEY;
    ev.pressed = state == 1;
    ev.keycode = key;
    ev.keysym = evdev_keysym(key);
    nx_win_push_event(p->win, &ev);
}

static void on_mods(void *data, void *kbd, uint32_t serial, uint32_t d,
                    uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)data; (void)kbd; (void)serial; (void)d; (void)latched; (void)locked; (void)group;
}

static void on_repeat(void *data, void *kbd, int32_t rate, int32_t delay)
{
    (void)data; (void)kbd; (void)rate; (void)delay;
}

static void on_ptr_enter(void *data, void *ptr, uint32_t serial, void *surface,
                         int32_t x, int32_t y)
{
    (void)data; (void)ptr; (void)serial; (void)surface; (void)x; (void)y;
}

static void on_ptr_leave(void *data, void *ptr, uint32_t serial, void *surface)
{
    (void)data; (void)ptr; (void)serial; (void)surface;
}

static void on_motion(void *data, void *ptr, uint32_t time, int32_t x, int32_t y)
{
    nx_wl *p = (nx_wl *)data;
    neox_window_event ev;
    (void)ptr; (void)time;
    memset(&ev, 0, sizeof ev);
    ev.kind = NEOX_WINDOW_POINTER;
    ev.buttons = p->buttons;
    ev.x = (float)x / 256.0f;
    ev.y = (float)y / 256.0f;
    nx_win_push_event(p->win, &ev);
}

static void on_button(void *data, void *ptr, uint32_t serial, uint32_t time,
                      uint32_t button, uint32_t state)
{
    nx_wl *p = (nx_wl *)data;
    neox_window_event ev;
    int b = 0;
    (void)ptr; (void)serial; (void)time;
    if (button == 0x110) b = 1;
    else if (button == 0x111) b = 2;
    else if (button == 0x112) b = 3;
    if (b >= 1 && b <= 3) {
        if (state == 1) p->buttons |= 1u << (unsigned)(b - 1);
        else p->buttons &= ~(1u << (unsigned)(b - 1));
    }
    memset(&ev, 0, sizeof ev);
    ev.kind = NEOX_WINDOW_POINTER;
    ev.button = b;
    ev.pressed = state == 1;
    ev.buttons = p->buttons;
    nx_win_push_event(p->win, &ev);
}

static void on_axis(void *data, void *ptr, uint32_t time, uint32_t axis, int32_t value)
{
    (void)data; (void)ptr; (void)time; (void)axis; (void)value;
}

static void on_frame_ptr(void *data, void *ptr)
{
    (void)data; (void)ptr;
}

static void on_axis_source(void *data, void *ptr, uint32_t source)
{
    (void)data; (void)ptr; (void)source;
}

static void on_axis_stop(void *data, void *ptr, uint32_t time, uint32_t axis)
{
    (void)data; (void)ptr; (void)time; (void)axis;
}

static void on_axis_discrete(void *data, void *ptr, uint32_t axis, int32_t discrete)
{
    (void)data; (void)ptr; (void)axis; (void)discrete;
}

static void on_seat_caps(void *data, void *seat, uint32_t caps)
{
    nx_wl *p = (nx_wl *)data;
    uint32_t ver = p->seat_ver ? p->seat_ver : 1;
    (void)seat;
    if ((caps & 1u) && !p->pointer) {
        static const struct {
            void (*enter)(void *, void *, uint32_t, void *, int32_t, int32_t);
            void (*leave)(void *, void *, uint32_t, void *);
            void (*motion)(void *, void *, uint32_t, int32_t, int32_t);
            void (*button)(void *, void *, uint32_t, uint32_t, uint32_t, uint32_t);
            void (*axis)(void *, void *, uint32_t, uint32_t, int32_t);
            void (*frame)(void *, void *);
            void (*axis_source)(void *, void *, uint32_t);
            void (*axis_stop)(void *, void *, uint32_t, uint32_t);
            void (*axis_discrete)(void *, void *, uint32_t, int32_t);
        } lis = {
            on_ptr_enter, on_ptr_leave, on_motion, on_button, on_axis,
            on_frame_ptr, on_axis_source, on_axis_stop, on_axis_discrete
        };
        p->pointer = g_wl.marshal(p->seat, 0, g_wl.pointer, ver, 0, NULL);
        if (p->pointer) g_wl.listen(p->pointer, (void *)&lis, p);
    }
    if ((caps & 2u) && !p->keyboard) {
        static const struct {
            void (*keymap)(void *, void *, uint32_t, int32_t, uint32_t);
            void (*enter)(void *, void *, uint32_t, void *, void *);
            void (*leave)(void *, void *, uint32_t, void *);
            void (*key)(void *, void *, uint32_t, uint32_t, uint32_t, uint32_t);
            void (*modifiers)(void *, void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
            void (*repeat_info)(void *, void *, int32_t, int32_t);
        } lis = {
            on_keymap, on_kb_enter, on_kb_leave, on_key, on_mods, on_repeat
        };
        p->keyboard = g_wl.marshal(p->seat, 1, g_wl.keyboard, ver, 0, NULL);
        if (p->keyboard) g_wl.listen(p->keyboard, (void *)&lis, p);
    }
}

static void on_seat_name(void *data, void *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

static void on_global(void *data, void *registry, uint32_t name,
                      const char *interface, uint32_t version)
{
    nx_wl *p = (nx_wl *)data;
    if (!p->compositor && strcmp(interface, "wl_compositor") == 0) {
        p->comp_ver = clamp_ver(version, 4);
        p->compositor = g_wl.marshal(registry, 0, g_wl.compositor, p->comp_ver, 0,
                                     name, interface, p->comp_ver, NULL);
    } else if (!p->shm && strcmp(interface, "wl_shm") == 0) {
        p->shm = g_wl.marshal(registry, 0, g_wl.shm, 1, 0, name, interface, 1u, NULL);
    } else if (!p->wm && strcmp(interface, "xdg_wm_base") == 0) {
        static const struct {
            void (*ping)(void *, void *, uint32_t);
        } lis = { on_ping };
        p->wm_ver = clamp_ver(version, 3);
        p->wm = g_wl.marshal(registry, 0, &xdg_wm_base_interface, p->wm_ver, 0,
                             name, interface, p->wm_ver, NULL);
        if (p->wm) g_wl.listen(p->wm, (void *)&lis, p);
    } else if (!p->seat && strcmp(interface, "wl_seat") == 0) {
        static const struct {
            void (*caps)(void *, void *, uint32_t);
            void (*name)(void *, void *, const char *);
        } lis = { on_seat_caps, on_seat_name };
        p->seat_ver = clamp_ver(version, 5);
        p->seat = g_wl.marshal(registry, 0, g_wl.seat, p->seat_ver, 0,
                               name, interface, p->seat_ver, NULL);
        if (p->seat) g_wl.listen(p->seat, (void *)&lis, p);
    } else if (!p->output && strcmp(interface, "wl_output") == 0) {
        static const struct {
            void (*geometry)(void *, void *, int32_t, int32_t, int32_t, int32_t,
                             int32_t, const char *, const char *, int32_t);
            void (*mode)(void *, void *, uint32_t, int32_t, int32_t, int32_t);
            void (*done)(void *, void *);
            void (*scale)(void *, void *, int32_t);
        } lis = { on_out_geom, on_out_mode, on_out_done, on_out_scale };
        uint32_t ver = clamp_ver(version, 2);
        p->output = g_wl.marshal(registry, 0, g_wl.output, ver, 0,
                                 name, interface, ver, NULL);
        if (p->output) g_wl.listen(p->output, (void *)&lis, p);
    }
}

static void on_global_remove(void *data, void *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static void on_buf_release(void *data, void *buf)
{
    nx_wl *p = (nx_wl *)data;
    int i;
    for (i = 0; i < 2; i++) {
        if (p->bufs[i] == buf) p->busy[i] = 0;
    }
}

static void on_frame_done(void *data, void *cb, uint32_t time)
{
    nx_wl *p = (nx_wl *)data;
    (void)time;
    p->frame_done = 1;
    g_wl.destroy(cb);
}

static neox_result ensure_pool(nx_wl *p, uint32_t w, uint32_t h)
{
    size_t bytes;
    /* Both pool size and buffer offsets/stride are signed protocol integers. */
    if (!nx_win_rgba_size(w, h, &bytes) || bytes > INT32_MAX / 2u) {
        nx_win_set_error("Wayland shared-memory buffers exceed protocol size limits");
        return NEOX_ERROR_INVALID_ARGUMENT;
    }
    bytes *= 2u;
    if (p->pool && p->buf_w == w && p->buf_h == h && p->map) return NEOX_OK;
    drop_buffers(p);
    p->shm_fd = memfd_new(bytes);
    if (p->shm_fd < 0) {
        nx_win_set_error("Wayland: memfd_create failed");
        return NEOX_ERROR_BACKEND;
    }
    p->map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, p->shm_fd, 0);
    if (p->map == MAP_FAILED) {
        p->map = NULL;
        nx_win_set_error("Wayland: mmap failed");
        return NEOX_ERROR_BACKEND;
    }
    p->map_bytes = bytes;
    p->pool = g_wl.marshal(p->shm, 0, g_wl.pool, 1, 0, NULL, p->shm_fd, (int32_t)bytes);
    if (!p->pool) {
        nx_win_set_error("Wayland: wl_shm_create_pool failed");
        return NEOX_ERROR_BACKEND;
    }
    p->buf_w = w;
    p->buf_h = h;
    return NEOX_OK;
}

static neox_result wl_present_soft(neox_window *w)
{
    nx_wl *p = (nx_wl *)w->plat;
    neox_result r;
    size_t stride;
    int slot = -1;
    int i;
    uint32_t x, y;
    void *buf;
    static const struct {
        void (*release)(void *, void *);
    } rel = { on_buf_release };
    static const struct {
        void (*done)(void *, void *, uint32_t);
    } frame_lis = { on_frame_done };
    if (!p || !w->soft_px) {
        nx_win_set_error("Wayland: no software frame");
        return NEOX_ERROR_INVALID_ARGUMENT;
    }
    r = ensure_pool(p, w->px_w, w->px_h);
    if (r != NEOX_OK) return r;
    for (i = 0; i < 2; i++) {
        if (!p->busy[i]) { slot = i; break; }
    }
    if (slot < 0) {
        wl_pump(p, 30);
        for (i = 0; i < 2; i++) {
            if (!p->busy[i]) { slot = i; break; }
        }
    }
    if (slot < 0) slot = 0;
    stride = (size_t)w->px_w * 4u;
    for (y = 0; y < w->px_h; y++) {
        uint8_t *row = (uint8_t *)p->map + (size_t)slot * stride * w->px_h + (size_t)y * stride;
        for (x = 0; x < w->px_w; x++) {
            const uint8_t *s = w->soft_px + ((size_t)y * w->px_w + x) * 4u;
            unsigned r8 = s[0], g8 = s[1], b8 = s[2], a = s[3];
            if (a < 255u) {
                r8 = r8 * a / 255u;
                g8 = g8 * a / 255u;
                b8 = b8 * a / 255u;
            }
            row[x * 4u + 0] = (uint8_t)b8;
            row[x * 4u + 1] = (uint8_t)g8;
            row[x * 4u + 2] = (uint8_t)r8;
            row[x * 4u + 3] = 255;
        }
    }
    if (p->bufs[slot]) {
        g_wl.marshal(p->bufs[slot], 0, NULL, 1, NX_MARSHAL_DESTROY);
        p->bufs[slot] = NULL;
    }
    buf = g_wl.marshal(p->pool, 0, g_wl.buffer, 1, 0, NULL,
                       (int32_t)(slot * (int)(stride * w->px_h)),
                       (int32_t)w->px_w, (int32_t)w->px_h, (int32_t)stride, 1u);
    if (!buf) {
        nx_win_set_error("Wayland: wl_shm_pool_create_buffer failed");
        return NEOX_ERROR_BACKEND;
    }
    p->bufs[slot] = buf;
    p->busy[slot] = 1;
    g_wl.listen(buf, (void *)&rel, p);
    g_wl.marshal(p->surface, 1, NULL, p->surf_ver, 0, buf, 0, 0);
    g_wl.marshal(p->surface, 2, NULL, p->surf_ver, 0, 0, 0,
                 (int32_t)w->pt_w, (int32_t)w->pt_h);
    if (w->vsync) {
        void *cb = g_wl.marshal(p->surface, 3, g_wl.callback, p->surf_ver, 0, NULL);
        p->frame_done = 0;
        if (cb) g_wl.listen(cb, (void *)&frame_lis, p);
    }
    g_wl.marshal(p->surface, 6, NULL, p->surf_ver, 0);
    if (w->vsync) {
        for (i = 0; i < 5 && !p->frame_done; i++) {
            if (wl_pump(p, 20) != 0) break;
        }
    } else if (g_wl.flush(p->dpy) < 0 && errno != EAGAIN) {
        p->dead = 1;
        nx_win_set_error("Wayland: flush failed");
        return NEOX_ERROR_BACKEND;
    }
    return NEOX_OK;
}

static int wl_poll(neox_window *w)
{
    nx_wl *p = (nx_wl *)w->plat;
    if (!p) return -1;
    return wl_pump(p, 0);
}

static neox_result wl_resize(neox_window *w, uint32_t pt_w, uint32_t pt_h)
{
    nx_wl *p = (nx_wl *)w->plat;
    (void)pt_w;
    (void)pt_h;
    if (!p) return NEOX_ERROR_BACKEND;
    apply_geom(p);
    if (p->surface)
        g_wl.marshal(p->surface, 6, NULL, p->surf_ver, 0);
    return NEOX_OK;
}

static void wl_gl_display(neox_window *w, void **display, uint32_t *kind)
{
    nx_wl *p = (nx_wl *)w->plat;
    *display = p ? p->dpy : NULL;
    *kind = 0x5741594cu; /* NEOX_GL_NATIVE_WAYLAND */
}

static void *wl_gl_window(neox_window *w)
{
    nx_wl *p = (nx_wl *)w->plat;
    if (!p || !p->surface) return NULL;
    if (!load_egl()) return NULL;
    if (!p->egl_window) {
        p->egl_window = g_ewin_create(p->surface, (int)w->px_w, (int)w->px_h);
        p->egl_w = w->px_w;
        p->egl_h = w->px_h;
    } else if (p->egl_w != w->px_w || p->egl_h != w->px_h) {
        g_ewin_resize(p->egl_window, (int)w->px_w, (int)w->px_h, 0, 0);
        p->egl_w = w->px_w;
        p->egl_h = w->px_h;
    }
    return p->egl_window;
}

static neox_result wl_open(neox_window *w)
{
    nx_wl *p;
    static const struct {
        void (*global)(void *, void *, uint32_t, const char *, uint32_t);
        void (*global_remove)(void *, void *, uint32_t);
    } reg_lis = { on_global, on_global_remove };
    static const struct {
        void (*configure)(void *, void *, uint32_t);
    } xdg_lis = { on_xdg_configure };
    static const struct {
        void (*configure)(void *, void *, int32_t, int32_t, void *);
        void (*close)(void *, void *);
        void (*bounds)(void *, void *, int32_t, int32_t);
        void (*caps)(void *, void *, void *);
    } tl_lis = { on_tl_configure, on_tl_close, on_tl_ignore, on_tl_caps };
    int spins;
    if (!load_wl()) return NEOX_ERROR_BACKEND;
    p = (nx_wl *)calloc(1, sizeof(*p));
    if (!p) {
        nx_win_set_error("out of memory");
        return NEOX_ERROR_OUT_OF_MEMORY;
    }
    p->win = w;
    p->shm_fd = -1;
    w->plat = p;
    p->dpy = g_wl.connect(NULL);
    if (!p->dpy) {
        nx_win_set_error("Wayland: wl_display_connect failed");
        wl_close(w);
        return NEOX_ERROR_BACKEND;
    }
    p->registry = g_wl.marshal(p->dpy, 1, g_wl.registry, 1, 0, NULL);
    if (!p->registry || g_wl.listen(p->registry, (void *)&reg_lis, p) != 0) {
        nx_win_set_error("Wayland: registry failed");
        wl_close(w);
        return NEOX_ERROR_BACKEND;
    }
    if (g_wl.roundtrip(p->dpy) < 0 || g_wl.roundtrip(p->dpy) < 0) {
        nx_win_set_error("Wayland: registry roundtrip failed");
        wl_close(w);
        return NEOX_ERROR_BACKEND;
    }
    if (!p->compositor || !p->shm || !p->wm) {
        nx_win_set_error("Wayland: compositor, shm or xdg_wm_base missing");
        wl_close(w);
        return NEOX_ERROR_BACKEND;
    }
    if (!w->scale_locked && p->output_scale > 0)
        w->scale = (float)p->output_scale;
    if (nx_win_set_geometry(w, w->pt_w, w->pt_h, (float)shell_scale(w)) != NEOX_OK) {
        wl_close(w);
        return NEOX_ERROR_INVALID_ARGUMENT;
    }

    p->surf_ver = p->comp_ver ? p->comp_ver : 1;
    p->surface = g_wl.marshal(p->compositor, 0, g_wl.surface, p->surf_ver, 0, NULL);
    p->xdg_surface = g_wl.marshal(p->wm, 2, &xdg_surface_interface, p->wm_ver, 0,
                                  NULL, p->surface);
    if (!p->surface || !p->xdg_surface) {
        nx_win_set_error("Wayland: xdg surface failed");
        wl_close(w);
        return NEOX_ERROR_BACKEND;
    }
    g_wl.listen(p->xdg_surface, (void *)&xdg_lis, p);
    p->toplevel = g_wl.marshal(p->xdg_surface, 1, &xdg_toplevel_interface, p->wm_ver, 0, NULL);
    if (!p->toplevel) {
        nx_win_set_error("Wayland: xdg toplevel failed");
        wl_close(w);
        return NEOX_ERROR_BACKEND;
    }
    g_wl.listen(p->toplevel, (void *)&tl_lis, p);
    g_wl.marshal(p->toplevel, 2, NULL, p->wm_ver, 0, w->title);
    g_wl.marshal(p->toplevel, 3, NULL, p->wm_ver, 0, "neox");
    g_wl.marshal(p->toplevel, 8, NULL, p->wm_ver, 0, (int32_t)w->pt_w, (int32_t)w->pt_h);
    g_wl.marshal(p->toplevel, 7, NULL, p->wm_ver, 0, (int32_t)w->pt_w, (int32_t)w->pt_h);
    g_wl.marshal(p->surface, 6, NULL, p->surf_ver, 0);
    for (spins = 0; spins < 8 && !p->configured; spins++) {
        if (g_wl.roundtrip(p->dpy) < 0) break;
    }
    if (!p->configured) {
        nx_win_set_error("Wayland: xdg configure was not received");
        wl_close(w);
        return NEOX_ERROR_BACKEND;
    }
    apply_geom(p);
    g_wl.marshal(p->surface, 6, NULL, p->surf_ver, 0);
    if (g_wl.roundtrip(p->dpy) < 0) {
        nx_win_set_error("Wayland: initial commit failed");
        wl_close(w);
        return NEOX_ERROR_BACKEND;
    }
    p->opened = 1;
    return NEOX_OK;
}

const nx_win_vt *nx_present_wl_vt(void)
{
    static const nx_win_vt vt = {
        "wayland",
        wl_open,
        wl_close,
        wl_poll,
        wl_resize,
        wl_present_soft,
        NULL, /* 读回走 soft_px 或 GL presented */
        wl_gl_display,
        wl_gl_window,
        NULL
    };
    return &vt;
}
