/* ===========================================================================
 * present.c — 窗口呈现胶水（与 Soft/GL 执行器分开）
 *
 * 选择：WAYLAND_DISPLAY 非空时先试 Wayland，失败再回退 X11。
 * NEOX_PRESENT_BACKEND=wayland|x11 强制，不再回退。
 * 事件队列在这里，平台文件只负责把系统事件推进来。
 * =========================================================================*/
#include "present_internal.h"
#include "internal.h"

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

int nx_win_rgba_size(uint32_t width, uint32_t height, size_t *bytes)
{
    size_t row;
    return width && height && width <= INT32_MAX && height <= INT32_MAX &&
           nx_size_mul(width, 4u, &row) && nx_size_mul(row, height, bytes);
}

neox_result nx_win_set_geometry(neox_window *w, uint32_t width, uint32_t height, float scale)
{
    double px, py;
    size_t bytes;
    if (!w || !width || !height || width > INT32_MAX || height > INT32_MAX ||
        !isfinite(scale) || scale <= 0.0f) goto invalid;
    /* Preserve the existing minimum effective scale policy. */
    if (scale < 0.5f) scale = 1.0f;
    px = floor((double)width * scale + 0.5);
    py = floor((double)height * scale + 0.5);
    if (px < 1.0) px = 1.0;
    if (py < 1.0) py = 1.0;
    if (px > INT32_MAX || py > INT32_MAX ||
        !nx_win_rgba_size((uint32_t)px, (uint32_t)py, &bytes)) goto invalid;
    w->pt_w = width; w->pt_h = height; w->scale = scale;
    w->px_w = (uint32_t)px; w->px_h = (uint32_t)py;
    return NEOX_OK;
invalid:
    nx_win_set_error("window geometry is non-finite or exceeds supported dimensions");
    return NEOX_ERROR_INVALID_ARGUMENT;
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
    if (desc->width == 0 || desc->height == 0 || (!isfinite(desc->scale) || desc->scale < 0.0f)) {
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
    r = nx_win_set_geometry(w, w->pt_w, w->pt_h, w->scale);
    if (r != NEOX_OK) { free(w); return r; }

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
        /* Failed Wayland setup may have selected an integer output scale. */
        r = nx_win_set_geometry(w, desc->width, desc->height,
                               desc->scale > 0.0f ? desc->scale : 1.0f);
        if (r != NEOX_OK) { free(w); return r; }
        w->vt = nx_present_x11_vt();
        r = w->vt->open(w);
        if (r != NEOX_OK) {
            char xerr[384];
            snprintf(xerr, sizeof xerr, "%s", g_err ? g_err : "x11 failed");
            if (w->vt->close) w->vt->close(w);
            snprintf(g_errbuf, sizeof g_errbuf, "wayland: %.180s; x11: %.180s", saved, xerr);
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

/* The context owns the device. The window may request destruction, and this
 * callback removes its borrow regardless of which owner initiated teardown. */
static void window_device_detached(void *owner)
{
    neox_window *win = owner;
    win->dev = NULL; win->ctx = NULL; win->is_gl = 0;
    win->soft_w = win->soft_h = 0;
#if NEOX_ENABLE_GL
    win->gl_surf = NULL; win->gl_native = NULL; win->gl_w = win->gl_h = 0;
#endif
}

void neox_window_destroy(neox_window *win)
{
    if (!win) return;
    /* Device teardown drains submissions and destroys EGL before display close. */
    if (win->dev) neox_device_destroy(win->dev);
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
    neox_result r;
    uint32_t old_w, old_h, old_px_w, old_px_h;
    if (!win) return NEOX_ERROR_INVALID_ARGUMENT;
    old_w = win->pt_w; old_h = win->pt_h;
    old_px_w = win->px_w; old_px_h = win->px_h;
    r = nx_win_set_geometry(win, points_w, points_h, win->scale);
    if (r != NEOX_OK) return r;
    r = win->vt && win->vt->resize ? win->vt->resize(win, points_w, points_h) : NEOX_OK;
    if (r != NEOX_OK) {
        win->pt_w = old_w; win->pt_h = old_h;
        win->px_w = old_px_w; win->px_h = old_px_h;
    }
    return r;
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
    if (win->dev) {
        nx_win_set_error("window already has a device");
        return NEOX_ERROR_INVALID_ARGUMENT;
    }
    memset(&dd, 0, sizeof dd);

    if (backend == NEOX_BACKEND_SOFT) {
        dd.backend = NEOX_BACKEND_SOFT;
        r = neox_device_create(ctx, &dd, out_dev);
        if (r != NEOX_OK) return r;
        win->ctx = ctx;
        win->dev = *out_dev;
        win->dev->owner = win; win->dev->owner_detach = window_device_detached;
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
        win->dev->owner = win; win->dev->owner_detach = window_device_detached;
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
    if (!win || !dev || !win->dev || !out_target) return NEOX_ERROR_INVALID_ARGUMENT;
    if (win->dev && dev != win->dev) {
        nx_win_set_error("target device is not the window device");
        return NEOX_ERROR_INVALID_ARGUMENT;
    }

    if (!win->is_gl) {
        size_t n;
        /* Reacquisition clears/reallocates borrowed target storage. Even an
         * unchanged size must not overwrite an active or queued frame. */
        if (dev->active_frame || dev->pending_count) {
            nx_win_set_error("complete or cancel window frames before acquiring a target");
            return NEOX_ERROR_INVALID_ARGUMENT;
        }
        if (!nx_win_rgba_size(win->px_w, win->px_h, &n))
            return NEOX_ERROR_INVALID_ARGUMENT;
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
        win->soft_w = win->px_w; win->soft_h = win->px_h;
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

static int soft_target_current(const neox_window *win)
{
    size_t bytes;
    return win->soft_px && win->soft_w == win->px_w && win->soft_h == win->px_h &&
           nx_win_rgba_size(win->px_w, win->px_h, &bytes) && bytes <= win->soft_bytes;
}

neox_result neox_window_present(neox_window *win)
{
    if (!win || !win->dev) return NEOX_ERROR_INVALID_ARGUMENT;
    if (win->is_gl) {
#if NEOX_ENABLE_GL
        if (!win->gl_surf || win->gl_w != win->px_w || win->gl_h != win->px_h) {
            nx_win_set_error("acquire_target before present and after size changes");
            return NEOX_ERROR_INVALID_ARGUMENT;
        }
        return neox_gl_surface_present(win->gl_surf);
#else
        return NEOX_ERROR_UNSUPPORTED;
#endif
    }
    if (!soft_target_current(win)) {
        nx_win_set_error("acquire_target after window size changes");
        return NEOX_ERROR_INVALID_ARGUMENT;
    }
    if (!win->vt || !win->vt->present_soft) return NEOX_ERROR_UNSUPPORTED;
    return win->vt->present_soft(win);
}

neox_result neox_window_request_readback(neox_window *win)
{
    if (!win || !win->dev) return NEOX_ERROR_INVALID_ARGUMENT;
#if NEOX_ENABLE_GL
    if (win->is_gl) {
        if (!win->gl_surf || win->gl_w != win->px_w || win->gl_h != win->px_h)
            return NEOX_ERROR_INVALID_ARGUMENT;
        return neox_gl_surface_request_readback(win->gl_surf);
    }
#endif
    return NEOX_OK;
}

neox_result neox_window_read_pixels(neox_window *win, uint8_t *out_pixels)
{
    if (!win || !win->dev || !out_pixels) return NEOX_ERROR_INVALID_ARGUMENT;
#if NEOX_ENABLE_GL
    if (win->is_gl) {
        if (!win->gl_surf || win->gl_w != win->px_w || win->gl_h != win->px_h)
            return NEOX_ERROR_INVALID_ARGUMENT;
        return neox_gl_surface_read_presented(win->gl_surf, out_pixels);
    }
#endif
    if (!win->is_gl && !soft_target_current(win)) {
        nx_win_set_error("acquire_target after window size changes");
        return NEOX_ERROR_INVALID_ARGUMENT;
    }
    if (!win->is_gl && win->vt && win->vt->read_screen && win->vt->read_screen(win,out_pixels)==0)
        return NEOX_OK;
    if (!win->is_gl && win->soft_px) {
        memcpy(out_pixels, win->soft_px, (size_t)win->px_w * (size_t)win->px_h * 4u);
        return NEOX_OK;
    }
    nx_win_set_error("no presented frame");
    return NEOX_ERROR_UNSUPPORTED;
}
