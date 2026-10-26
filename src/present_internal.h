/* 呈现层内部。只被 present.c / present_x11.c / present_wl.c 包含。 */
#ifndef NEOX_PRESENT_INTERNAL_H
#define NEOX_PRESENT_INTERNAL_H

#include "neox_present.h"

#if NEOX_ENABLE_GL
#include "neox_gl.h"
#endif

#include <stddef.h>
#include <stdint.h>

#define NX_WIN_Q 32

typedef struct nx_win_vt nx_win_vt;

struct neox_window {
    const nx_win_vt *vt;
    void *plat;
    char title[160];
    uint32_t pt_w, pt_h;
    float scale;
    int scale_locked;       /* desc.scale > 0 */
    uint32_t px_w, px_h;
    int vsync;
    int close_latched;
    neox_context *ctx;
    neox_device *dev;
    int is_gl;
    uint8_t *soft_px;
    size_t soft_bytes;
    uint32_t soft_w, soft_h; /* dimensions of the acquired CPU target */
#if NEOX_ENABLE_GL
    neox_gl_surface *gl_surf;
    void *gl_native;
    uint32_t gl_w, gl_h;
#endif
    neox_window_event q[NX_WIN_Q];
    int qh, qt, qn;
};

struct nx_win_vt {
    const char *name;
    neox_result (*open)(neox_window *w);
    void (*close)(neox_window *w);
    /* 把新事件推进窗口队列。返回 0 表示连接还在。 */
    int (*poll)(neox_window *w);
    neox_result (*resize)(neox_window *w, uint32_t pt_w, uint32_t pt_h);
    neox_result (*present_soft)(neox_window *w);
    /* 读屏幕。失败返回非 0，调用方改用 GL presented 缓冲。 */
    int (*read_screen)(neox_window *w, uint8_t *out);
    void (*gl_display)(neox_window *w, void **display, uint32_t *kind);
    /* 当前像素尺寸的本地窗口（X Window 或 wl_egl_window*）。 */
    void *(*gl_window)(neox_window *w);
    /* X11：若 visual_id 与当前窗口不同，按 EGL config 重建窗口。可为 NULL。 */
    void (*use_visual)(neox_window *w, unsigned long visual_id);
};

/* Checked geometry publication. Failure leaves all metrics unchanged. */
neox_result nx_win_set_geometry(neox_window *w, uint32_t width, uint32_t height, float scale);
int nx_win_rgba_size(uint32_t width, uint32_t height, size_t *bytes);
void nx_win_push_event(neox_window *w, const neox_window_event *ev);
void nx_win_set_error(const char *msg);

const nx_win_vt *nx_present_x11_vt(void);
const nx_win_vt *nx_present_wl_vt(void);

#endif
