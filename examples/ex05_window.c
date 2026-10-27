#define _POSIX_C_SOURCE 200809L
/* ex05_window — 上屏示例。
 * NEOX_PRESENT_FRAMES=N 画 N 帧后退出（第 1 帧存 before，然后放大，第 2 帧存 after）。
 * 不设置时一直画到关窗或 Escape。NEOX_PRESENT_GPU=1 用原生 GPU（Linux GL / macOS Metal），否则 Soft。
 * 没有显示时打印 skip 并返回 0；NEOX_REQUIRE_PRESENT 则返回 1。 */
#include "neox.h"
#include "neox_present.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int write_bmp(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    FILE *f = fopen(path, "wb");
    uint32_t row_bytes = w * 3u;
    uint32_t pad = (4u - (row_bytes % 4u)) % 4u;
    uint32_t img_size = (row_bytes + pad) * h;
    uint8_t header[54];
    uint32_t x, y;
    if (!f) return -1;
    memset(header, 0, sizeof header);
    header[0] = 'B';
    header[1] = 'M';
    {
        uint32_t file_size = 54u + img_size;
        uint32_t off = 54u;
        int32_t sw = (int32_t)w, sh = (int32_t)h;
        memcpy(header + 2, &file_size, 4);
        memcpy(header + 10, &off, 4);
        header[14] = 40;
        memcpy(header + 18, &sw, 4);
        memcpy(header + 22, &sh, 4);
        header[26] = 1;
        header[28] = 24;
        memcpy(header + 34, &img_size, 4);
    }
    if (fwrite(header, 1, 54, f) != 54) { fclose(f); return -1; }
    for (y = h; y-- > 0; ) {
        for (x = 0; x < w; x++) {
            const uint8_t *p = rgba + ((size_t)y * w + x) * 4u;
            uint8_t bgr[3] = { p[2], p[1], p[0] };
            if (fwrite(bgr, 1, 3, f) != 3) { fclose(f); return -1; }
        }
        if (pad) {
            static const uint8_t zeros[3] = { 0, 0, 0 };
            if (fwrite(zeros, 1, pad, f) != pad) { fclose(f); return -1; }
        }
    }
    fclose(f);
    return 0;
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

static int have_display(void)
{
#ifdef __APPLE__
    return 1; /* Cocoa open reports an unavailable window server. */
#else
    const char *force = getenv("NEOX_PRESENT_BACKEND");
    const char *xd = getenv("DISPLAY");
    const char *wd = getenv("WAYLAND_DISPLAY");
    if (force && strcmp(force, "wayland") == 0) return wd && wd[0];
    if (force && strcmp(force, "x11") == 0) return xd && xd[0];
    return (xd && xd[0]) || (wd && wd[0]);
#endif
}

static int frame(neox_window *win, neox_device *dev, const char *bmp)
{
    neox_target_desc td;
    neox_frame *fr = NULL;
    neox_submission sub = { 0 };
    neox_state st;
    neox_paint paint;
    neox_color bg = { 0.12f, 0.18f, 0.38f, 1.0f };
    neox_color red = { 0.90f, 0.22f, 0.18f, 1.0f };
    neox_color green = { 0.20f, 0.75f, 0.35f, 1.0f };
    neox_color yellow = { 0.95f, 0.78f, 0.20f, 1.0f };
    uint32_t pt_w = 0, pt_h = 0, px_w = 0, px_h = 0;
    float scale = 1.0f;
    neox_path a, b, c;
    uint8_t *px;
    memset(&td, 0, sizeof td);
    if (neox_window_acquire_target(win, dev, &td) != NEOX_OK) return 0;
    td.clear = &bg;
    if (neox_frame_begin(dev, &td, &fr) != NEOX_OK) return 0;
    neox_window_metrics(win, &pt_w, &pt_h, &scale, &px_w, &px_h);
    memset(&st, 0, sizeof st);
    st.transform.a = scale;
    st.transform.d = scale;
    st.opacity = 1.0f;
    if (neox_push_state(fr, &st) != NEOX_OK) return 0;
    a = make_rect(dev, 0.0f, 0.0f, (float)pt_w, 28.0f);
    b = make_rect(dev, 0.0f, (float)pt_h - 28.0f, (float)pt_w, 28.0f);
    c = make_rect(dev, (float)pt_w * 0.3f, (float)pt_h * 0.35f,
                  (float)pt_w * 0.4f, (float)pt_h * 0.3f);
    memset(&paint, 0, sizeof paint);
    paint.color = red;
    neox_draw_path(fr, a, &paint);
    paint.color = green;
    neox_draw_path(fr, b, &paint);
    paint.color = yellow;
    neox_draw_path(fr, c, &paint);
    if (neox_pop_state(fr) != NEOX_OK) return 0;
    if (neox_frame_end(fr, &sub) != NEOX_OK) return 0;
    neox_result waited = neox_submission_wait(dev, sub);
    neox_submission_release(dev, sub);
    if (waited != NEOX_OK) return 0;
    if (bmp && neox_window_request_readback(win) != NEOX_OK) return 0;
    if (neox_window_present(win) != NEOX_OK) return 0;
    neox_path_release(dev, a);
    neox_path_release(dev, b);
    neox_path_release(dev, c);
    if (!bmp) return 1;
    px = (uint8_t *)malloc((size_t)px_w * px_h * 4u);
    if (!px) return 0;
    if (neox_window_read_pixels(win, px) != NEOX_OK) {
        free(px);
        return 0;
    }
    if (write_bmp(bmp, px, px_w, px_h) != 0) {
        free(px);
        return 0;
    }
    printf("wrote %s (%ux%u)\n", bmp, px_w, px_h);
    free(px);
    return 1;
}

int main(void)
{
    neox_context_desc cd;
    neox_context *ctx = NULL;
    neox_present_desc desc;
    neox_window *win = NULL;
    neox_device *dev = NULL;
    neox_backend_kind kind = NEOX_BACKEND_SOFT;
    const char *frames_env = getenv("NEOX_PRESENT_FRAMES");
    const char *be;
    int limit = 0;
    int n = 0;
    int gpu = getenv("NEOX_PRESENT_GPU") && getenv("NEOX_PRESENT_GPU")[0] == '1';
    char before[160], after[160];
#ifdef __APPLE__
    const char *gpu_name = "metal";
#else
    const char *gpu_name = "gl";
#endif
    if (!have_display()) {
        if (getenv("NEOX_REQUIRE_PRESENT")) {
            fprintf(stderr, "ex05: display required\n");
            return 1;
        }
        printf("ex05: skipped (no DISPLAY or WAYLAND_DISPLAY)\n");
        return 77;
    }
    if (frames_env && frames_env[0]) limit = atoi(frames_env);
    if (gpu) kind = NEOX_BACKEND_GPU;
    memset(&cd, 0, sizeof cd);
    if (neox_context_create(&cd, &ctx) != NEOX_OK) return 1;
    memset(&desc, 0, sizeof desc);
    desc.struct_size = (uint32_t)sizeof desc;
    desc.magic = NEOX_PRESENT_DESC_MAGIC;
    desc.title = "NeoX window";
    desc.width = 320;
    desc.height = 240;
    desc.scale = 1.0f;
    desc.vsync = 1;
    if (neox_window_create(&desc, &win) != NEOX_OK) {
        fprintf(stderr, "ex05: %s\n", neox_window_last_error());
        neox_context_destroy(ctx);
        return 1;
    }
    be = neox_window_backend_name(win);
    snprintf(before, sizeof before, "ex05_%s_%s_before.bmp", be, gpu ? gpu_name : "soft");
    snprintf(after, sizeof after, "ex05_%s_%s_after.bmp", be, gpu ? gpu_name : "soft");
    if (neox_window_create_device(win, ctx, kind, &dev) != NEOX_OK) {
        fprintf(stderr, "ex05 device: %s\n", neox_window_last_error());
        neox_window_destroy(win);
        neox_context_destroy(ctx);
#ifdef __APPLE__
        if(gpu && !getenv("NEOX_REQUIRE_METAL") && !getenv("NEOX_REQUIRE_PRESENT"))return 77;
#endif
        return 1;
    }
    printf("ex05 %s on %s\n", gpu ? gpu_name : "soft", be);
    for (;;) {
        const char *shot = NULL;
        neox_window_event ev;
        if (limit > 0 && n == 0) shot = before;
        if (limit > 0 && n == 1) shot = after;
        if (!frame(win, dev, shot)) {
            fprintf(stderr, "ex05 frame failed: %s\n", neox_window_last_error());
            neox_window_destroy(win);
            neox_context_destroy(ctx);
            return 1;
        }
        n++;
        if (limit > 0 && n >= limit) break;
        if (limit > 0 && n == 1) {
            if (neox_window_resize(win, 480, 320) != NEOX_OK) {
                fprintf(stderr, "ex05 resize: %s\n", neox_window_last_error());
                neox_window_destroy(win);
                neox_context_destroy(ctx);
                return 1;
            }
        }
        while (neox_window_poll(win, &ev)) {
            if (ev.kind == NEOX_WINDOW_CLOSE ||
                (ev.kind == NEOX_WINDOW_KEY && ev.pressed && ev.keysym == 0xff1bu)) {
                limit = n;
                break;
            }
        }
        if (limit > 0 && n >= limit) break;
        if (limit == 0) {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 16000000L;
            nanosleep(&ts, NULL);
        }
    }
    neox_window_destroy(win);
    neox_context_destroy(ctx);
    return 0;
}
