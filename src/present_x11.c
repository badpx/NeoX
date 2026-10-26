/* ===========================================================================
 * present_x11.c — X11 呈现。运行期 dlopen("libX11.so.6")，不包含 Xlib.h。
 *
 * 结构偏移按 LP64（本机已核对）：XEvent 192，XImage 136，XVisualInfo 64。
 * 其它 ABI 在 open 时直接拒绝，避免读错事件。
 * 像素经 XPutImage / XGetImage，GL 走同一窗口上的 EGL window surface。
 * =========================================================================*/
#include "present_internal.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NX_XEVENT 192
#define NX_XIMAGE_DATA 16
#define NX_XIMAGE_BPL 44
#define NX_XIMAGE_RED 56
#define NX_XIMAGE_GREEN 64
#define NX_XIMAGE_BLUE 72
#define NX_VINFO 64

typedef int (*nx_xerr_fn)(void *dpy, void *ev);
typedef int (*nx_xio_fn)(void *dpy);

typedef void *(*PFN_open)(const char *);
typedef int (*PFN_close)(void *);
typedef int (*PFN_scr)(void *);
typedef unsigned long (*PFN_root)(void *);
typedef int (*PFN_depth)(void *, int);
typedef void *(*PFN_visual)(void *, int);
typedef unsigned long (*PFN_black)(void *, int);
typedef unsigned long (*PFN_simple)(void *, unsigned long, int, int,
                                    unsigned int, unsigned int, unsigned int,
                                    unsigned long, unsigned long);
typedef int (*PFN_destroy)(void *, unsigned long);
typedef int (*PFN_map)(void *, unsigned long);
typedef int (*PFN_name)(void *, unsigned long, const char *);
typedef int (*PFN_select)(void *, unsigned long, long);
typedef int (*PFN_sync)(void *, int);
typedef int (*PFN_pending)(void *);
typedef int (*PFN_next)(void *, void *);
typedef unsigned long (*PFN_atom)(void *, const char *, int);
typedef int (*PFN_proto)(void *, unsigned long, unsigned long *, int);
typedef int (*PFN_resize)(void *, unsigned long, unsigned int, unsigned int);
typedef void *(*PFN_gc)(void *, unsigned long, unsigned long, void *);
typedef int (*PFN_freegc)(void *, void *);
typedef void *(*PFN_image)(void *, void *, unsigned int, int, int, char *,
                           unsigned int, unsigned int, int, int);
typedef int (*PFN_put)(void *, unsigned long, void *, void *, int, int, int, int,
                       unsigned int, unsigned int);
typedef void *(*PFN_get)(void *, unsigned long, int, int, unsigned int,
                         unsigned int, unsigned long, int);
typedef int (*PFN_putpx)(void *, int, int, unsigned long);
typedef unsigned long (*PFN_getpx)(void *, int, int);
typedef int (*PFN_freeimg)(void *);
typedef const char *(*PFN_res)(void *);
typedef nx_xerr_fn (*PFN_seterr)(nx_xerr_fn);
typedef nx_xio_fn (*PFN_setio)(nx_xio_fn);
typedef unsigned long (*PFN_keysym)(void *, int);
typedef int (*PFN_xfree)(void *);
typedef unsigned long (*PFN_cmap)(void *, unsigned long, void *, int);
typedef int (*PFN_freecmap)(void *, unsigned long);
typedef void *(*PFN_vinfo)(void *, long, void *, int *);
typedef unsigned long (*PFN_cwin)(void *, unsigned long, int, int, unsigned int,
                                  unsigned int, unsigned int, int, unsigned int,
                                  void *, unsigned long, void *);

static struct {
    void *lib;
    PFN_open open;
    PFN_close close;
    PFN_scr screen;
    PFN_root root;
    PFN_depth depth;
    PFN_visual visual;
    PFN_black black;
    PFN_simple simple;
    PFN_destroy destroy;
    PFN_map map;
    PFN_name name;
    PFN_select select;
    PFN_sync sync;
    PFN_pending pending;
    PFN_next next;
    PFN_atom atom;
    PFN_proto proto;
    PFN_resize resize;
    PFN_gc gc;
    PFN_freegc freegc;
    PFN_image image;
    PFN_put put;
    PFN_get get;
    PFN_putpx putpx;
    PFN_getpx getpx;
    PFN_freeimg freeimg;
    PFN_res res;
    PFN_seterr seterr;
    PFN_setio setio;
    PFN_keysym keysym;
    PFN_xfree xfree;
    PFN_cmap cmap;
    PFN_freecmap freecmap;
    PFN_vinfo vinfo;
    PFN_cwin cwin;
} g_x;

static int g_handlers;

static int on_xerr(void *dpy, void *ev)
{
    const unsigned char *e = (const unsigned char *)ev;
    unsigned char code = e ? e[32] : 0;
    unsigned char req = e ? e[33] : 0;
    (void)dpy;
    fprintf(stderr, "X error code=%u request=%u\n", code, req);
    return 0;
}

static int on_xio(void *dpy)
{
    (void)dpy;
    return 0;
}

static int load_x11(void)
{
    if (g_x.lib) return 1;
    g_x.lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!g_x.lib) g_x.lib = dlopen("libX11.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_x.lib) {
        const char *de = dlerror();
        char buf[256];
        snprintf(buf, sizeof buf, "X11: dlopen libX11.so.6 failed%s%s",
                 de ? ": " : "", de ? de : "");
        nx_win_set_error(buf);
        return 0;
    }
#define NX_DLSYM(field, type, sym) do { \
        union { void *vp; type fp; } u_; \
        u_.vp = dlsym(g_x.lib, sym); \
        g_x.field = u_.fp; \
    } while (0)
    NX_DLSYM(open, PFN_open, "XOpenDisplay");
    NX_DLSYM(close, PFN_close, "XCloseDisplay");
    NX_DLSYM(screen, PFN_scr, "XDefaultScreen");
    NX_DLSYM(root, PFN_root, "XDefaultRootWindow");
    NX_DLSYM(depth, PFN_depth, "XDefaultDepth");
    NX_DLSYM(visual, PFN_visual, "XDefaultVisual");
    NX_DLSYM(black, PFN_black, "XBlackPixel");
    NX_DLSYM(simple, PFN_simple, "XCreateSimpleWindow");
    NX_DLSYM(destroy, PFN_destroy, "XDestroyWindow");
    NX_DLSYM(map, PFN_map, "XMapWindow");
    NX_DLSYM(name, PFN_name, "XStoreName");
    NX_DLSYM(select, PFN_select, "XSelectInput");
    NX_DLSYM(sync, PFN_sync, "XSync");
    NX_DLSYM(pending, PFN_pending, "XPending");
    NX_DLSYM(next, PFN_next, "XNextEvent");
    NX_DLSYM(atom, PFN_atom, "XInternAtom");
    NX_DLSYM(proto, PFN_proto, "XSetWMProtocols");
    NX_DLSYM(resize, PFN_resize, "XResizeWindow");
    NX_DLSYM(gc, PFN_gc, "XCreateGC");
    NX_DLSYM(freegc, PFN_freegc, "XFreeGC");
    NX_DLSYM(image, PFN_image, "XCreateImage");
    NX_DLSYM(put, PFN_put, "XPutImage");
    NX_DLSYM(get, PFN_get, "XGetImage");
    NX_DLSYM(putpx, PFN_putpx, "XPutPixel");
    NX_DLSYM(getpx, PFN_getpx, "XGetPixel");
    NX_DLSYM(freeimg, PFN_freeimg, "XDestroyImage");
    NX_DLSYM(res, PFN_res, "XResourceManagerString");
    NX_DLSYM(seterr, PFN_seterr, "XSetErrorHandler");
    NX_DLSYM(setio, PFN_setio, "XSetIOErrorHandler");
    NX_DLSYM(keysym, PFN_keysym, "XLookupKeysym");
    NX_DLSYM(xfree, PFN_xfree, "XFree");
    NX_DLSYM(cmap, PFN_cmap, "XCreateColormap");
    NX_DLSYM(freecmap, PFN_freecmap, "XFreeColormap");
    NX_DLSYM(vinfo, PFN_vinfo, "XGetVisualInfo");
    NX_DLSYM(cwin, PFN_cwin, "XCreateWindow");
#undef NX_DLSYM
    if (!g_x.open || !g_x.close || !g_x.screen || !g_x.root || !g_x.simple ||
        !g_x.map || !g_x.select || !g_x.sync || !g_x.pending || !g_x.next ||
        !g_x.atom || !g_x.proto || !g_x.resize || !g_x.gc || !g_x.image ||
        !g_x.put || !g_x.get || !g_x.putpx || !g_x.getpx || !g_x.freeimg ||
        !g_x.keysym || !g_x.visual || !g_x.depth || !g_x.black || !g_x.destroy ||
        !g_x.cwin || !g_x.cmap || !g_x.vinfo || !g_x.xfree) {
        nx_win_set_error("X11: libX11 is missing a required entry point");
        return 0;
    }
    if (!g_handlers && g_x.seterr && g_x.setio) {
        g_x.seterr(on_xerr);
        g_x.setio(on_xio);
        g_handlers = 1;
    }
    return 1;
}

typedef struct nx_x11 {
    void *dpy;
    unsigned long root;
    unsigned long win;
    unsigned long cmap;
    unsigned long visual_id;
    unsigned long wm_delete;
    void *visual;
    void *gc;
    void *image;
    int screen;
    int depth;
    int iw, ih;
    uint32_t buttons;
} nx_x11;

static int ev_i(const unsigned char *e, int off)
{
    int v;
    memcpy(&v, e + off, sizeof v);
    return v;
}

static unsigned int ev_u(const unsigned char *e, int off)
{
    unsigned int v;
    memcpy(&v, e + off, sizeof v);
    return v;
}

static unsigned long ev_ul(const unsigned char *e, int off)
{
    unsigned long v;
    memcpy(&v, e + off, sizeof v);
    return v;
}

static unsigned long img_ul(void *img, int off)
{
    unsigned long v;
    memcpy(&v, (unsigned char *)img + off, sizeof v);
    return v;
}

static int img_i(void *img, int off)
{
    int v;
    memcpy(&v, (unsigned char *)img + off, sizeof v);
    return v;
}

static unsigned long pack_chan(unsigned long mask, unsigned v)
{
    unsigned shift = 0;
    unsigned bits = 0;
    unsigned long m;
    if (!mask) return 0;
    m = mask;
    while ((m & 1ul) == 0ul) {
        m >>= 1;
        shift++;
    }
    while (m & 1ul) {
        bits++;
        m >>= 1;
    }
    if (bits == 0) return 0;
    if (bits < 8) v >>= (8u - bits);
    else if (bits > 8) v <<= (bits - 8u);
    return ((unsigned long)v << shift) & mask;
}

static unsigned unpack_chan(unsigned long pix, unsigned long mask)
{
    unsigned shift = 0;
    unsigned bits = 0;
    unsigned long m, v;
    if (!mask) return 0;
    m = mask;
    while ((m & 1ul) == 0ul) {
        m >>= 1;
        shift++;
    }
    while (m & 1ul) {
        bits++;
        m >>= 1;
    }
    v = (pix & mask) >> shift;
    if (bits >= 8) return (unsigned)(v >> (bits - 8u));
    if (bits == 0) return 0;
    return (unsigned)(v << (8u - bits));
}

static float parse_dpi(const char *s)
{
    const char *p;
    if (!s) return 0.0f;
    p = strstr(s, "Xft.dpi");
    if (!p) return 0.0f;
    p += 7;
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    return strtof(p, NULL);
}

static unsigned long visual_id_of(nx_x11 *p, void *visual)
{
    unsigned char tmpl[NX_VINFO];
    void *list;
    int n = 0;
    int i;
    unsigned long found = 0;
    if (!visual || !g_x.vinfo) return 0;
    memset(tmpl, 0, sizeof tmpl);
    memcpy(tmpl + 16, &p->screen, sizeof p->screen);
    list = g_x.vinfo(p->dpy, 2L /* VisualScreenMask */, tmpl, &n);
    if (!list || n <= 0) return 0;
    for (i = 0; i < n; i++) {
        void *vis = NULL;
        unsigned char *row = (unsigned char *)list + (size_t)i * NX_VINFO;
        memcpy(&vis, row, sizeof vis);
        if (vis == visual) {
            memcpy(&found, row + 8, sizeof found);
            break;
        }
    }
    g_x.xfree(list);
    return found;
}

static void decorate(nx_x11 *p, neox_window *w)
{
    long mask = 1L | 2L | 4L | 8L | 64L | 131072L;
    p->wm_delete = g_x.atom(p->dpy, "WM_DELETE_WINDOW", 0);
    g_x.proto(p->dpy, p->win, &p->wm_delete, 1);
    g_x.name(p->dpy, p->win, w->title);
    g_x.select(p->dpy, p->win, mask);
    g_x.map(p->dpy, p->win);
    g_x.sync(p->dpy, 0);
}

static void drop_image(nx_x11 *p)
{
    if (p->image && g_x.freeimg) g_x.freeimg(p->image);
    p->image = NULL;
    p->iw = 0;
    p->ih = 0;
}

static void x11_close(neox_window *w)
{
    nx_x11 *p = (nx_x11 *)w->plat;
    if (!p) return;
    drop_image(p);
    if (p->gc && g_x.freegc) g_x.freegc(p->dpy, p->gc);
    if (p->win && g_x.destroy) g_x.destroy(p->dpy, p->win);
    if (p->cmap && g_x.freecmap) g_x.freecmap(p->dpy, p->cmap);
    if (p->dpy && g_x.close) g_x.close(p->dpy);
    free(p);
    w->plat = NULL;
}

static neox_result x11_open(neox_window *w)
{
    nx_x11 *p;
    unsigned long bg;
    const char *res;
    float dpi;
    if (sizeof(long) != 8 || sizeof(void *) != 8) {
        nx_win_set_error("X11 backend expects the LP64 ABI");
        return NEOX_ERROR_UNSUPPORTED;
    }
    if (!load_x11()) return NEOX_ERROR_BACKEND;
    p = (nx_x11 *)calloc(1, sizeof(*p));
    if (!p) {
        nx_win_set_error("out of memory");
        return NEOX_ERROR_OUT_OF_MEMORY;
    }
    p->dpy = g_x.open(NULL);
    if (!p->dpy) {
        const char *ds = getenv("DISPLAY");
        char buf[256];
        snprintf(buf, sizeof buf, "X11: XOpenDisplay failed (DISPLAY=%s)",
                 ds ? ds : "(null)");
        nx_win_set_error(buf);
        free(p);
        return NEOX_ERROR_BACKEND;
    }
    w->plat = p;
    p->screen = g_x.screen(p->dpy);
    p->root = g_x.root(p->dpy);
    p->depth = g_x.depth(p->dpy, p->screen);
    p->visual = g_x.visual(p->dpy, p->screen);
    p->visual_id = visual_id_of(p, p->visual);
    if (!w->scale_locked && g_x.res) {
        res = g_x.res(p->dpy);
        dpi = parse_dpi(res);
        if (dpi >= 48.0f && dpi <= 480.0f) {
            w->scale = dpi / 96.0f;
            nx_win_recompute(w);
        }
    }
    bg = g_x.black(p->dpy, p->screen);
    p->win = g_x.simple(p->dpy, p->root, 0, 0, w->px_w, w->px_h, 0, bg, bg);
    if (!p->win) {
        nx_win_set_error("X11: XCreateSimpleWindow failed");
        x11_close(w);
        return NEOX_ERROR_BACKEND;
    }
    p->gc = g_x.gc(p->dpy, p->win, 0, NULL);
    if (!p->gc) {
        nx_win_set_error("X11: XCreateGC failed");
        x11_close(w);
        return NEOX_ERROR_BACKEND;
    }
    decorate(p, w);
    return NEOX_OK;
}

static void x11_use_visual(neox_window *w, unsigned long visual_id)
{
    nx_x11 *p = (nx_x11 *)w->plat;
    unsigned char tmpl[NX_VINFO];
    unsigned char attr[128];
    void *list;
    void *visual = NULL;
    int n = 0;
    int depth = 0;
    unsigned long cmap, win, bg;
    unsigned long mask;
    long evmask;
    if (!p || visual_id == 0 || visual_id == p->visual_id) return;
    memset(tmpl, 0, sizeof tmpl);
    memcpy(tmpl + 8, &visual_id, sizeof visual_id);
    memcpy(tmpl + 16, &p->screen, sizeof p->screen);
    list = g_x.vinfo(p->dpy, 1L | 2L, tmpl, &n);
    if (!list || n < 1) {
        if (list) g_x.xfree(list);
        nx_win_set_error("X11: EGL visual is not on this screen");
        return;
    }
    memcpy(&visual, list, sizeof visual);
    memcpy(&depth, (unsigned char *)list + 20, sizeof depth);
    g_x.xfree(list);
    cmap = g_x.cmap(p->dpy, p->root, visual, 0 /* AllocNone */);
    if (!cmap) {
        nx_win_set_error("X11: XCreateColormap failed");
        return;
    }
    bg = g_x.black(p->dpy, p->screen);
    evmask = 1L | 2L | 4L | 8L | 64L | 131072L;
    memset(attr, 0, sizeof attr);
    memcpy(attr + 8, &bg, sizeof bg);
    memcpy(attr + 24, &bg, sizeof bg);
    memcpy(attr + 72, &evmask, sizeof evmask);
    memcpy(attr + 96, &cmap, sizeof cmap);
    /* CWBackPixel|CWBorderPixel|CWEventMask|CWColormap */
    mask = 2ul | 8ul | 2048ul | 8192ul;
    win = g_x.cwin(p->dpy, p->root, 0, 0, w->px_w, w->px_h, 0, depth,
                   1u /* InputOutput */, visual, mask, attr);
    if (!win) {
        g_x.freecmap(p->dpy, cmap);
        nx_win_set_error("X11: XCreateWindow for EGL visual failed");
        return;
    }
    if (p->gc) g_x.freegc(p->dpy, p->gc);
    drop_image(p);
    if (p->win) g_x.destroy(p->dpy, p->win);
    if (p->cmap) g_x.freecmap(p->dpy, p->cmap);
    p->win = win;
    p->cmap = cmap;
    p->visual = visual;
    p->visual_id = visual_id;
    p->depth = depth;
    p->gc = g_x.gc(p->dpy, p->win, 0, NULL);
    decorate(p, w);
}

static int x11_poll(neox_window *w)
{
    nx_x11 *p = (nx_x11 *)w->plat;
    if (!p || !p->dpy) return -1;
    while (g_x.pending(p->dpy) > 0) {
        unsigned char ev[256];
        int type;
        neox_window_event out;
        memset(ev, 0, sizeof ev);
        memset(&out, 0, sizeof out);
        g_x.next(p->dpy, ev);
        type = ev_i(ev, 0);
        if (type == 33) { /* ClientMessage */
            unsigned long msg = ev_ul(ev, 40);
            unsigned long atom = ev_ul(ev, 56);
            unsigned long protocols;
            (void)msg;
            protocols = g_x.atom(p->dpy, "WM_PROTOCOLS", 1);
            if (atom == p->wm_delete || (protocols && msg == protocols &&
                                         atom == p->wm_delete)) {
                out.kind = NEOX_WINDOW_CLOSE;
                nx_win_push_event(w, &out);
            }
            continue;
        }
        if (type == 22) { /* ConfigureNotify */
            unsigned long window = ev_ul(ev, 40);
            int width, height;
            uint32_t npt_w, npt_h;
            if (window != p->win) continue;
            width = ev_i(ev, 56);
            height = ev_i(ev, 60);
            if (width < 1 || height < 1) continue;
            npt_w = (uint32_t)((double)width / (double)w->scale + 0.5);
            npt_h = (uint32_t)((double)height / (double)w->scale + 0.5);
            if (npt_w < 1) npt_w = 1;
            if (npt_h < 1) npt_h = 1;
            if (npt_w == w->pt_w && npt_h == w->pt_h) continue;
            w->pt_w = npt_w;
            w->pt_h = npt_h;
            nx_win_recompute(w);
            out.kind = NEOX_WINDOW_RESIZE;
            out.width = w->pt_w;
            out.height = w->pt_h;
            out.scale = w->scale;
            nx_win_push_event(w, &out);
            continue;
        }
        if (type == 2 || type == 3) { /* KeyPress / KeyRelease */
            out.kind = NEOX_WINDOW_KEY;
            out.pressed = type == 2;
            out.keycode = ev_u(ev, 84);
            out.keysym = (uint32_t)g_x.keysym(ev, 0);
            out.x = (float)ev_i(ev, 64) / w->scale;
            out.y = (float)ev_i(ev, 68) / w->scale;
            nx_win_push_event(w, &out);
            continue;
        }
        if (type == 4 || type == 5 || type == 6) {
            unsigned int button = (type == 6) ? 0u : ev_u(ev, 84);
            float x = (float)ev_i(ev, 64) / w->scale;
            float y = (float)ev_i(ev, 68) / w->scale;
            if (type == 4 && button >= 1 && button <= 3)
                p->buttons |= 1u << (button - 1u);
            if (type == 5 && button >= 1 && button <= 3)
                p->buttons &= ~(1u << (button - 1u));
            out.kind = NEOX_WINDOW_POINTER;
            out.button = (type == 6) ? 0 : (int)button;
            out.buttons = p->buttons;
            out.pressed = type == 4;
            out.x = x;
            out.y = y;
            nx_win_push_event(w, &out);
        }
    }
    return 0;
}

static neox_result x11_resize(neox_window *w, uint32_t pt_w, uint32_t pt_h)
{
    nx_x11 *p = (nx_x11 *)w->plat;
    (void)pt_w;
    (void)pt_h;
    if (!p) return NEOX_ERROR_BACKEND;
    g_x.resize(p->dpy, p->win, w->px_w, w->px_h);
    g_x.sync(p->dpy, 0);
    return NEOX_OK;
}

static int ensure_image(nx_x11 *p, uint32_t w, uint32_t h)
{
    void *img;
    int bpl;
    void *data;
    if (p->image && p->iw == (int)w && p->ih == (int)h) return 1;
    drop_image(p);
    img = g_x.image(p->dpy, p->visual, (unsigned int)p->depth, 2 /* ZPixmap */,
                    0, NULL, w, h, 32, 0);
    if (!img) return 0;
    bpl = img_i(img, NX_XIMAGE_BPL);
    if (bpl <= 0) {
        g_x.freeimg(img);
        return 0;
    }
    data = calloc((size_t)bpl, h);
    if (!data) {
        g_x.freeimg(img);
        return 0;
    }
    memcpy((unsigned char *)img + NX_XIMAGE_DATA, &data, sizeof data);
    p->image = img;
    p->iw = (int)w;
    p->ih = (int)h;
    return 1;
}

static neox_result x11_present_soft(neox_window *w)
{
    nx_x11 *p = (nx_x11 *)w->plat;
    unsigned long red, green, blue;
    uint32_t x, y;
    if (!p || !w->soft_px) {
        nx_win_set_error("X11: no software frame");
        return NEOX_ERROR_INVALID_ARGUMENT;
    }
    if (!ensure_image(p, w->px_w, w->px_h)) {
        nx_win_set_error("X11: XCreateImage failed");
        return NEOX_ERROR_BACKEND;
    }
    red = img_ul(p->image, NX_XIMAGE_RED);
    green = img_ul(p->image, NX_XIMAGE_GREEN);
    blue = img_ul(p->image, NX_XIMAGE_BLUE);
    for (y = 0; y < w->px_h; y++) {
        for (x = 0; x < w->px_w; x++) {
            const uint8_t *s = w->soft_px + ((size_t)y * w->px_w + x) * 4u;
            unsigned r = s[0], g = s[1], b = s[2], a = s[3];
            unsigned long pix;
            if (a < 255u) {
                r = r * a / 255u;
                g = g * a / 255u;
                b = b * a / 255u;
            }
            pix = pack_chan(red, r) | pack_chan(green, g) | pack_chan(blue, b);
            g_x.putpx(p->image, (int)x, (int)y, pix);
        }
    }
    g_x.put(p->dpy, p->win, p->gc, p->image, 0, 0, 0, 0, w->px_w, w->px_h);
    g_x.sync(p->dpy, 0);
    return NEOX_OK;
}

static int x11_read_screen(neox_window *w, uint8_t *out)
{
    nx_x11 *p = (nx_x11 *)w->plat;
    void *img;
    unsigned long red, green, blue;
    uint32_t x, y;
    if (!p) return -1;
    g_x.sync(p->dpy, 0);
    img = g_x.get(p->dpy, p->win, 0, 0, w->px_w, w->px_h, ~(unsigned long)0, 2);
    if (!img) return -1;
    red = img_ul(img, NX_XIMAGE_RED);
    green = img_ul(img, NX_XIMAGE_GREEN);
    blue = img_ul(img, NX_XIMAGE_BLUE);
    for (y = 0; y < w->px_h; y++) {
        for (x = 0; x < w->px_w; x++) {
            unsigned long pix = g_x.getpx(img, (int)x, (int)y);
            uint8_t *d = out + ((size_t)y * w->px_w + x) * 4u;
            d[0] = (uint8_t)unpack_chan(pix, red);
            d[1] = (uint8_t)unpack_chan(pix, green);
            d[2] = (uint8_t)unpack_chan(pix, blue);
            d[3] = 255;
        }
    }
    g_x.freeimg(img);
    return 0;
}

static void x11_gl_display(neox_window *w, void **display, uint32_t *kind)
{
    nx_x11 *p = (nx_x11 *)w->plat;
    *display = p ? p->dpy : NULL;
    *kind = 0x58313120u; /* NEOX_GL_NATIVE_X11 */
}

static void *x11_gl_window(neox_window *w)
{
    nx_x11 *p = (nx_x11 *)w->plat;
    if (!p || !p->win) return NULL;
    return (void *)(uintptr_t)p->win;
}

const nx_win_vt *nx_present_x11_vt(void)
{
    static const nx_win_vt vt = {
        "x11",
        x11_open,
        x11_close,
        x11_poll,
        x11_resize,
        x11_present_soft,
        x11_read_screen,
        x11_gl_display,
        x11_gl_window,
        x11_use_visual
    };
    return &vt;
}
