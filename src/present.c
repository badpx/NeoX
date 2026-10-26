/* ===========================================================================
 * present.c — 窗口呈现胶水（与 Soft/GL 执行器分开）
 *
 * 选择：WAYLAND_DISPLAY 非空时先试 Wayland，失败再回退 X11。
 * NEOX_PRESENT_BACKEND=wayland|x11 强制，不再回退。
 * 事件队列在这里，平台文件只负责把系统事件推进来。
 * =========================================================================*/
#include "present_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_errbuf[384];
static const char *g_err;

void nx_win_set_error(const char *msg)
{
    if (!msg || !msg[0]) {
        g_errbuf[0] = 0;
        g_err = NULL;
        return;
    }
    snprintf(g_errbuf, sizeof g_errbuf, "%s", msg);
    g_err = g_errbuf;
}

const char *neox_window_last_error(void) { return g_err; }

void nx_win_recompute(neox_window *w)
{
    float s = w->scale;
    double px, py;
    if (s < 0.5f) s = 1.0f;
    w->scale = s;
    px = (double)w->pt_w * (double)s + 0.5;
    py = (double)w->pt_h * (double)s + 0.5;
    w->px_w = px < 1.0 ? 1u : (uint32_t)px;
    w->px_h = py < 1.0 ? 1u : (uint32_t)py;
}

void nx_win_push_event(neox_window *w, const neox_window_event *ev)
{
    if (!w || !ev) return;
    if (w->qn >= NX_WIN_Q) {
        w->qh = (w->qh + 1) % NX_WIN_Q;
        w->qn--;
    }
    w->q[w->qt] = *ev;
    w->qt = (w->qt + 1) % NX_WIN_Q;
    w->qn++;
}

static int pop_event(neox_window *w, neox_window_event *out)
{
    if (w->qn <= 0) return 0;
    *out = w->q[w->qh];
    w->qh = (w->qh + 1) % NX_WIN_Q;
    w->qn--;
    return 1;
}

static int backend_forced(const char **name)
{
    const char *e = getenv("NEOX_PRESENT_BACKEND");
    if (!e || !e[0]) {
        if (name) *name = NULL;
        return 0;
    }
    if (name) *name = e;
    return 1;
}

neox_result neox_window_create(const neox_present_desc *desc, neox_window **out)
{
    neox_window *w;
    const char *forced = NULL;
    const char *wl_env;
    int try_wl = 0;
    neox_result r;

    if (out) *out = NULL;
    nx_win_set_error(NULL);
    if (!desc || !out) return NEOX_ERROR_INVALID_ARGUMENT;
    if (desc->struct_size != (uint32_t)sizeof(*desc) ||
        desc->magic != NEOX_PRESENT_DESC_MAGIC) {
        nx_win_set_error("neox_present_desc version mismatch");
        return NEOX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->width == 0 || desc->height == 0 || desc->scale < 0.0f) {
        nx_win_set_error("window size must be non-zero");
        return NEOX_ERROR_INVALID_ARGUMENT;
    }

    w = (neox_window *)calloc(1, sizeof(*w));
    if (!w) {
        nx_win_set_error("out of memory");
        return NEOX_ERROR_OUT_OF_MEMORY;
    }
    snprintf(w->title, sizeof w->title, "%s", desc->title ? desc->title : "NeoX");
    w->pt_w = desc->width;
    w->pt_h = desc->height;
    w->scale_locked = desc->scale > 0.0f;
    w->scale = w->scale_locked ? desc->scale : 1.0f;
    w->vsync = desc->vsync ? 1 : 0;
    nx_win_recompute(w);

    if (backend_forced(&forced)) {
        if (strcmp(forced, "wayland") == 0) try_wl = 1;
        else if (strcmp(forced, "x11") == 0) try_wl = 0;
        else {
            nx_win_set_error("NEOX_PRESENT_BACKEND must be wayland or x11");
            free(w);
            return NEOX_ERROR_INVALID_ARGUMENT;
        }
        w->vt = try_wl ? nx_present_wl_vt() : nx_present_x11_vt();
        r = w->vt->open(w);
        if (r != NEOX_OK) {
            if (w->vt->close) w->vt->close(w);
            free(w);
            return r;
        }
        *out = w;
        return NEOX_OK;
    }

    wl_env = getenv("WAYLAND_DISPLAY");
    if (wl_env && wl_env[0]) {
        char saved[384];
        w->vt = nx_present_wl_vt();
        r = w->vt->open(w);
        if (r == NEOX_OK) {
            *out = w;
            return NEOX_OK;
        }
        snprintf(saved, sizeof saved, "%s", g_err ? g_err : "wayland failed");
        if (w->vt->close) w->vt->close(w);
        w->plat = NULL;
        w->vt = nx_present_x11_vt();
        r = w->vt->open(w);
        if (r != NEOX_OK) {
            char xerr[384];
            snprintf(xerr, sizeof xerr, "%s", g_err ? g_err : "x11 failed");
            if (w->vt->close) w->vt->close(w);
            snprintf(g_errbuf, sizeof g_errbuf, "wayland: %s; x11: %s", saved, xerr);
            g_err = g_errbuf;
            free(w);
            return r;
        }
        *out = w;
        return NEOX_OK;
    }

    w->vt = nx_present_x11_vt();
    r = w->vt->open(w);
    if (r != NEOX_OK) {
        if (w->vt->close) w->vt->close(w);
        free(w);
        return r;
    }
    *out = w;
    return NEOX_OK;
}

void neox_window_destroy(neox_window *win)
{
    if (!win) return;
#if NEOX_ENABLE_GL
    if (win->gl_surf && win->dev) {
        neox_gl_surface_destroy(win->dev, win->gl_surf);
        win->gl_surf = NULL;
    }
#endif
    if (win->vt && win->vt->close) win->vt->close(win);
    free(win->soft_px);
    free(win);
}

const char *neox_window_backend_name(const neox_window *win)
{
    if (!win || !win->vt) return NULL;
    return win->vt->name;
}

void neox_window_metrics(const neox_window *win,
                         uint32_t *points_w, uint32_t *points_h,
                         float *scale,
                         uint32_t *pixels_w, uint32_t *pixels_h)
{
    if (!win) return;
    if (points_w) *points_w = win->pt_w;
    if (points_h) *points_h = win->pt_h;
    if (scale) *scale = win->scale;
    if (pixels_w) *pixels_w = win->px_w;
    if (pixels_h) *pixels_h = win->px_h;
}

int neox_window_poll(neox_window *win, neox_window_event *out)
{
    if (!win || !out) return 0;
    memset(out, 0, sizeof *out);
    if (win->close_latched) {
        win->close_latched = 0;
        out->kind = NEOX_WINDOW_CLOSE;
        return 1;
    }
    if (win->qn == 0 && win->vt && win->vt->poll) {
        if (win->vt->poll(win) != 0) {
            out->kind = NEOX_WINDOW_CLOSE;
            return 1;
        }
    }
    return pop_event(win, out);
}

neox_result neox_window_resize(neox_window *win, uint32_t points_w, uint32_t points_h)
{
    if (!win || points_w == 0 || points_h == 0) return NEOX_ERROR_INVALID_ARGUMENT;
    win->pt_w = points_w;
    win->pt_h = points_h;
    nx_win_recompute(win);
    if (win->vt && win->vt->resize)
        return win->vt->resize(win, points_w, points_h);
    return NEOX_OK;
}

void neox_window_request_close(neox_window *win)
{
    if (!win) return;
    win->close_latched = 1;
}

neox_result neox_window_create_device(neox_window *win, neox_context *ctx,
                                      neox_backend_kind backend,
                                      neox_device **out_dev)
{
    neox_device_desc dd;
    neox_result r;
    if (out_dev) *out_dev = NULL;
    if (!win || !ctx || !out_dev) return NEOX_ERROR_INVALID_ARGUMENT;
    memset(&dd, 0, sizeof dd);

    if (backend == NEOX_BACKEND_SOFT) {
        dd.backend = NEOX_BACKEND_SOFT;
        r = neox_device_create(ctx, &dd, out_dev);
        if (r != NEOX_OK) return r;
        win->ctx = ctx;
        win->dev = *out_dev;
        win->is_gl = 0;
        return NEOX_OK;
    }

    if (backend != NEOX_BACKEND_GPU) return NEOX_ERROR_INVALID_ARGUMENT;
#if NEOX_ENABLE_GL
    {
        neox_gl_device_desc gld;
        void *nd = NULL;
        uint32_t kind = 0;
        if (!win->vt || !win->vt->gl_display) {
            nx_win_set_error("backend has no GL display");
            return NEOX_ERROR_UNSUPPORTED;
        }
        win->vt->gl_display(win, &nd, &kind);
        if (!nd) {
            nx_win_set_error("window has no native display");
            return NEOX_ERROR_UNSUPPORTED;
        }
        memset(&gld, 0, sizeof gld);
        gld.struct_size = (uint32_t)sizeof gld;
        gld.magic = NEOX_GL_DESC_MAGIC;
        gld.native_display = nd;
        gld.native_kind = kind;
        dd.backend = NEOX_BACKEND_GPU;
        dd.platform = &gld;
        dd.platform_size = sizeof gld;
        r = neox_device_create(ctx, &dd, out_dev);
        if (r != NEOX_OK) {
            const char *e = neox_get_error_detail(ctx);
            if (e) nx_win_set_error(e);
            return r;
        }
        win->ctx = ctx;
        win->dev = *out_dev;
        win->is_gl = 1;
        return NEOX_OK;
    }
#else
    (void)r;
    nx_win_set_error("GL backend not built");
    return NEOX_ERROR_UNSUPPORTED;
#endif
}

neox_result neox_window_acquire_target(neox_window *win, neox_device *dev,
                                       neox_target_desc *out_target)
{
    if (!win || !dev || !out_target) return NEOX_ERROR_INVALID_ARGUMENT;
    if (win->dev && dev != win->dev) {
        nx_win_set_error("target device is not the window device");
        return NEOX_ERROR_INVALID_ARGUMENT;
    }

    if (!win->is_gl) {
        size_t n = (size_t)win->px_w * (size_t)win->px_h * 4u;
        if (win->soft_bytes < n) {
            uint8_t *p = (uint8_t *)realloc(win->soft_px, n);
            if (!p) {
                nx_win_set_error("out of memory");
                return NEOX_ERROR_OUT_OF_MEMORY;
            }
            win->soft_px = p;
            win->soft_bytes = n;
        }
        memset(win->soft_px, 0, n);
        out_target->kind = NEOX_BACKEND_SOFT;
        out_target->soft.pixels = win->soft_px;
        out_target->soft.stride = (size_t)win->px_w * 4u;
        out_target->soft.width = win->px_w;
        out_target->soft.height = win->px_h;
        return NEOX_OK;
    }

#if NEOX_ENABLE_GL
    {
        void *nw;
        uint32_t vid = 0;
        neox_result r;
        if (win->vt->use_visual &&
            neox_gl_device_native_visual(dev, &vid) == NEOX_OK)
            win->vt->use_visual(win, (unsigned long)vid);
        if (!win->vt->gl_window) {
            nx_win_set_error("backend has no GL window");
            return NEOX_ERROR_UNSUPPORTED;
        }
        nw = win->vt->gl_window(win);
        if (!nw) return NEOX_ERROR_BACKEND;
        if (win->gl_surf && nw != win->gl_native) {
            neox_gl_surface_destroy(dev, win->gl_surf);
            win->gl_surf = NULL;
            win->gl_native = NULL;
        }
        if (!win->gl_surf) {
            neox_gl_window_desc wd;
            memset(&wd, 0, sizeof wd);
            wd.struct_size = (uint32_t)sizeof wd;
            wd.magic = NEOX_GL_WINSURF_MAGIC;
            wd.native_window = nw;
            wd.width = win->px_w;
            wd.height = win->px_h;
            wd.vsync = win->vsync;
            r = neox_gl_surface_create_window(dev, &wd, &win->gl_surf);
            if (r != NEOX_OK) {
                const char *e = neox_get_error_detail(win->ctx);
                if (e) nx_win_set_error(e);
                return r;
            }
            win->gl_native = nw;
            win->gl_w = win->px_w;
            win->gl_h = win->px_h;
        } else if (win->gl_w != win->px_w || win->gl_h != win->px_h) {
            r = neox_gl_surface_resize(win->gl_surf, win->px_w, win->px_h);
            if (r != NEOX_OK) {
                const char *e = neox_get_error_detail(win->ctx);
                if (e) nx_win_set_error(e);
                return r;
            }
            win->gl_w = win->px_w;
            win->gl_h = win->px_h;
        }
        out_target->kind = NEOX_BACKEND_GPU;
        out_target->surface = win->gl_surf;
        return NEOX_OK;
    }
#else
    nx_win_set_error("GL backend not built");
    return NEOX_ERROR_UNSUPPORTED;
#endif
}

neox_result neox_window_present(neox_window *win)
{
    if (!win) return NEOX_ERROR_INVALID_ARGUMENT;
    if (win->is_gl) {
#if NEOX_ENABLE_GL
        if (!win->gl_surf) {
            nx_win_set_error("acquire_target before present");
            return NEOX_ERROR_INVALID_ARGUMENT;
        }
        return neox_gl_surface_present(win->gl_surf);
#else
        return NEOX_ERROR_UNSUPPORTED;
#endif
    }
    if (!win->vt || !win->vt->present_soft) return NEOX_ERROR_UNSUPPORTED;
    return win->vt->present_soft(win);
}

neox_result neox_window_read_pixels(neox_window *win, uint8_t *out_pixels)
{
    if (!win || !out_pixels) return NEOX_ERROR_INVALID_ARGUMENT;
    if (win->vt && win->vt->read_screen && win->vt->read_screen(win, out_pixels) == 0)
        return NEOX_OK;
#if NEOX_ENABLE_GL
    if (win->is_gl && win->gl_surf)
        return neox_gl_surface_read_presented(win->gl_surf, out_pixels);
#endif
    if (!win->is_gl && win->soft_px) {
        memcpy(out_pixels, win->soft_px, (size_t)win->px_w * (size_t)win->px_h * 4u);
        return NEOX_OK;
    }
    nx_win_set_error("no presented frame");
    return NEOX_ERROR_UNSUPPORTED;
}
