/* 窗口呈现：Soft 与 GL 上屏后读回，检查朝向、缩放、resize 和关闭。
 * 没有 DISPLAY / WAYLAND_DISPLAY 时跳过（返回 0），除非 NEOX_REQUIRE_PRESENT。 */
#include "neox.h"
#include "neox_present.h"

#if NEOX_ENABLE_GL
#include "neox_gl.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fails;

static void expect(int cond, const char *msg)
{
    if (cond) return;
    fprintf(stderr, "FAIL: %s\n", msg);
    if (neox_window_last_error())
        fprintf(stderr, "  window: %s\n", neox_window_last_error());
    g_fails++;
}

static int have_display(void)
{
    const char *force = getenv("NEOX_PRESENT_BACKEND");
    const char *xd = getenv("DISPLAY");
    const char *wd = getenv("WAYLAND_DISPLAY");
    if (force && strcmp(force, "wayland") == 0) return wd && wd[0];
    if (force && strcmp(force, "x11") == 0) return xd && xd[0];
    return (xd && xd[0]) || (wd && wd[0]);
}

static neox_path make_rect(neox_device *dev, float x, float y, float w, float h)
{
    const uint8_t verbs[] = {
        NEOX_VERB_MOVE, NEOX_VERB_LINE, NEOX_VERB_LINE, NEOX_VERB_LINE, NEOX_VERB_CLOSE
    };
    float pts[8] = { x, y, x + w, y, x + w, y + h, x, y + h };
    neox_path_desc d;
    neox_path p = { 0 };
    memset(&d, 0, sizeof d);
    d.verbs = verbs;
    d.points = pts;
    d.verb_count = 5;
    d.point_count = 4;
    d.fill_rule = NEOX_FILL_NONZERO;
    neox_path_create(dev, &d, &p);
    return p;
}

static int pix_near(const uint8_t *p, int r, int g, int b)
{
    int dr = (int)p[0] - r;
    int dg = (int)p[1] - g;
    int db = (int)p[2] - b;
    if (dr < 0) dr = -dr;
    if (dg < 0) dg = -dg;
    if (db < 0) db = -db;
    return dr <= 3 && dg <= 3 && db <= 3 && p[3] >= 250;
}

static int draw_bars(neox_window *win, neox_device *dev)
{
    neox_target_desc td;
    neox_frame *fr = NULL;
    neox_submission sub = { 0 };
    neox_state st;
    neox_paint paint;
    neox_path top, bot;
    neox_color blue = { 0.0f, 0.0f, 1.0f, 1.0f };
    neox_color red = { 1.0f, 0.0f, 0.0f, 1.0f };
    neox_color green = { 0.0f, 1.0f, 0.0f, 1.0f };
    uint32_t pt_w = 0, pt_h = 0;
    float scale = 1.0f;
    neox_result r;
    neox_window_metrics(win, &pt_w, &pt_h, &scale, NULL, NULL);
    memset(&td, 0, sizeof td);
    r = neox_window_acquire_target(win, dev, &td);
    if (r != NEOX_OK) {
        fprintf(stderr, "acquire: %s\n", neox_window_last_error());
        return 0;
    }
    td.clear = &blue;
    if (neox_frame_begin(dev, &td, &fr) != NEOX_OK) {
        fprintf(stderr, "begin %s\n", neox_window_last_error());
        return 0;
    }
    memset(&st, 0, sizeof st);
    st.transform.a = scale;
    st.transform.d = scale;
    st.opacity = 1.0f;
    if (neox_push_state(fr, &st) != NEOX_OK) {
        fprintf(stderr, "push failed\n");
        return 0;
    }
    top = make_rect(dev, 0.0f, 0.0f, (float)pt_w, 8.0f);
    bot = make_rect(dev, 0.0f, (float)pt_h - 8.0f, (float)pt_w, 8.0f);
    memset(&paint, 0, sizeof paint);
    paint.color = red;
    if (neox_draw_path(fr, top, &paint) != NEOX_OK) {
        fprintf(stderr, "draw top failed\n");
        return 0;
    }
    paint.color = green;
    if (neox_draw_path(fr, bot, &paint) != NEOX_OK) {
        fprintf(stderr, "draw bot failed\n");
        return 0;
    }
    if (neox_pop_state(fr) != NEOX_OK) {
        fprintf(stderr, "pop failed\n");
        return 0;
    }
    if (neox_frame_end(fr, &sub) != NEOX_OK) {
        fprintf(stderr, "end failed\n");
        return 0;
    }
    if (neox_submission_wait(dev, sub) != NEOX_OK) {
        fprintf(stderr, "wait failed status=%d\n",
                (int)neox_submission_get_status(dev, sub));
        return 0;
    }
    if (neox_window_present(win) != NEOX_OK) {
        fprintf(stderr, "present: %s\n", neox_window_last_error());
        return 0;
    }
    neox_path_release(dev, top);
    neox_path_release(dev, bot);
    return 1;
}

static int check_orientation(neox_window *win, const char *tag)
{
    uint32_t pw = 0, ph = 0;
    uint8_t *px;
    const uint8_t *top, *mid, *bot;
    char buf[128];
    neox_window_metrics(win, NULL, NULL, NULL, &pw, &ph);
    px = (uint8_t *)malloc((size_t)pw * ph * 4u);
    if (!px) return 0;
    if (neox_window_read_pixels(win, px) != NEOX_OK) {
        fprintf(stderr, "%s read: %s\n", tag, neox_window_last_error());
        free(px);
        return 0;
    }
    top = px + ((size_t)pw * 2u + pw / 2u) * 4u;
    mid = px + ((size_t)pw * (ph / 2u) + pw / 2u) * 4u;
    bot = px + ((size_t)pw * (ph - 3u) + pw / 2u) * 4u;
    snprintf(buf, sizeof buf, "%s top row is red (not flipped)", tag);
    expect(pix_near(top, 255, 0, 0), buf);
    snprintf(buf, sizeof buf, "%s middle is blue", tag);
    expect(pix_near(mid, 0, 0, 255), buf);
    snprintf(buf, sizeof buf, "%s bottom row is green", tag);
    expect(pix_near(bot, 0, 255, 0), buf);
    free(px);
    return 1;
}

static void run_backend(neox_context *ctx, neox_backend_kind kind, const char *tag,
                        uint32_t w, uint32_t h, float scale)
{
    neox_present_desc desc;
    neox_window *win = NULL;
    neox_device *dev = NULL;
    neox_window_event ev;
    uint32_t pt_w = 0, pt_h = 0, px_w = 0, px_h = 0;
    float got = 0.0f;
    memset(&desc, 0, sizeof desc);
    desc.struct_size = (uint32_t)sizeof desc;
    desc.magic = NEOX_PRESENT_DESC_MAGIC;
    desc.title = tag;
    desc.width = w;
    desc.height = h;
    desc.scale = scale;
    desc.vsync = 1;
    if (neox_window_create(&desc, &win) != NEOX_OK) {
        fprintf(stderr, "%s create: %s\n", tag, neox_window_last_error());
        g_fails++;
        return;
    }
    printf("present %s via %s\n", tag, neox_window_backend_name(win));
    neox_window_metrics(win, &pt_w, &pt_h, &got, &px_w, &px_h);
    expect(pt_w == w && pt_h == h, "logical size");
    if (scale > 0.0f) {
        uint32_t want_w = (uint32_t)((double)w * (double)got + 0.5);
        uint32_t want_h = (uint32_t)((double)h * (double)got + 0.5);
        expect(px_w == want_w && px_h == want_h, "pixel size follows scale");
    }
    if (neox_window_create_device(win, ctx, kind, &dev) != NEOX_OK) {
        fprintf(stderr, "%s device: %s\n", tag, neox_window_last_error());
        if (kind == NEOX_BACKEND_GPU && !getenv("NEOX_REQUIRE_GL")) {
            printf("%s GPU skipped\n", tag);
            neox_window_destroy(win);
            return;
        }
        g_fails++;
        neox_window_destroy(win);
        return;
    }
    expect(draw_bars(win, dev), "draw/present");
    check_orientation(win, tag);
    expect(neox_window_resize(win, w + 20, h + 10) == NEOX_OK, "resize");
    neox_window_metrics(win, &pt_w, &pt_h, &got, &px_w, &px_h);
    expect(pt_w == w + 20 && pt_h == h + 10, "resized points");
    expect(draw_bars(win, dev), "draw after resize");
    check_orientation(win, tag);
    memset(&ev, 0, sizeof ev);
    neox_window_request_close(win);
    expect(neox_window_poll(win, &ev) == 1 && ev.kind == NEOX_WINDOW_CLOSE,
           "request_close yields CLOSE");
    neox_window_destroy(win);
}

#if NEOX_ENABLE_GL
static int offscreen_still_works(neox_context *ctx)
{
    neox_gl_device_desc gld;
    neox_device_desc dd;
    neox_device *dev = NULL;
    neox_gl_surface *surf = NULL;
    neox_target_desc td;
    neox_frame *fr = NULL;
    neox_submission sub = { 0 };
    neox_color red = { 1.0f, 0.0f, 0.0f, 1.0f };
    uint8_t px[16];
    memset(&gld, 0, sizeof gld);
    gld.struct_size = (uint32_t)sizeof gld;
    gld.magic = NEOX_GL_DESC_MAGIC;
    memset(&dd, 0, sizeof dd);
    dd.backend = NEOX_BACKEND_GPU;
    dd.platform = &gld;
    dd.platform_size = sizeof gld;
    if (neox_device_create(ctx, &dd, &dev) != NEOX_OK) {
        if (getenv("NEOX_REQUIRE_GL")) {
            fprintf(stderr, "surfaceless GL create failed: %s\n",
                    neox_get_error_detail(ctx));
            return 0;
        }
        printf("surfaceless GL skipped\n");
        return 1;
    }
    if (neox_gl_surface_create(dev, 2, 2, &surf) != NEOX_OK) return 0;
    memset(&td, 0, sizeof td);
    td.kind = NEOX_BACKEND_GPU;
    td.surface = surf;
    td.clear = &red;
    if (neox_frame_begin(dev, &td, &fr) != NEOX_OK) return 0;
    if (neox_frame_end(fr, &sub) != NEOX_OK) return 0;
    if (neox_submission_wait(dev, sub) != NEOX_OK) return 0;
    if (neox_gl_surface_read_pixels(surf, px) != NEOX_OK) return 0;
    neox_gl_surface_destroy(dev, surf);
    neox_device_destroy(dev);
    return px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255;
}
#endif

int main(void)
{
    neox_context_desc cd;
    neox_context *ctx = NULL;
    if (!have_display()) {
        if (getenv("NEOX_REQUIRE_PRESENT")) {
            fprintf(stderr, "NEOX_REQUIRE_PRESENT set but no display\n");
            return 1;
        }
        printf("present: skipped (no DISPLAY or WAYLAND_DISPLAY)\n");
        return 0;
    }
    memset(&cd, 0, sizeof cd);
    if (neox_context_create(&cd, &ctx) != NEOX_OK) return 1;
#if NEOX_ENABLE_GL
    expect(offscreen_still_works(ctx), "surfaceless GL before window");
#endif
    run_backend(ctx, NEOX_BACKEND_SOFT, "soft", 64, 48, 1.0f);
    run_backend(ctx, NEOX_BACKEND_SOFT, "soft-hidpi", 30, 24, 2.0f);
#if NEOX_ENABLE_GL
    run_backend(ctx, NEOX_BACKEND_GPU, "gl", 64, 48, 1.0f);
    run_backend(ctx, NEOX_BACKEND_GPU, "gl-hidpi", 30, 24, 2.0f);
    expect(offscreen_still_works(ctx), "surfaceless GL after window");
#endif
    neox_context_destroy(ctx);
    if (g_fails) {
        fprintf(stderr, "%d present check(s) failed\n", g_fails);
        return 1;
    }
    printf("present: ok\n");
    return 0;
}
