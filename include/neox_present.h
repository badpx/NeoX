/* ===========================================================================
 * neox_present.h — 桌面窗口呈现（平台胶水，不是图形语义的一部分）
 *
 * 坐标仍是用户单位（point）。本层把 point × scale 映射成帧缓冲像素，
 * 负责窗口、关闭/缩放/指针/键盘事件、vsync 与把已完成的帧送上屏幕。
 * 核心与 Soft/GL/Metal 执行器不包含桌面窗口与输入事件逻辑。
 *
 * Linux：运行期 dlopen libX11 / libwayland-client / libwayland-egl。
 * 首选 Wayland（WAYLAND_DISPLAY 可用时），否则 X11。
 * NEOX_PRESENT_BACKEND=wayland|x11 可强制。找不到显示时创建失败，
 * 离屏 API 不受影响。
 *
 * macOS：Cocoa 窗口，Soft 通过 Core Animation 呈现，GPU 使用 Metal。
 * NEOX_PRESENT_BACKEND=cocoa 可强制；GL 保留离屏支持。
 * 所有窗口 API、附属 device/context 的操作与销毁都必须在主线程。
 * 不接管宿主 NSApplication 的 delegate，也不调用 NSApplication.run。
 *
 * 事件由调用方轮询（库不建线程、不自己跑循环）。输入以数据形式交出，
 * 便于以后做确定性回放。
 * =========================================================================*/
#ifndef NEOX_PRESENT_H
#define NEOX_PRESENT_H

#include "neox.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NEOX_PRESENT_DESC_MAGIC 0x4e585052u /* 'NXPR' */

typedef struct neox_present_desc {
    uint32_t struct_size;   /* = sizeof(neox_present_desc) */
    uint32_t magic;         /* = NEOX_PRESENT_DESC_MAGIC */
    const char *title;      /* NULL = "NeoX" */
    uint32_t width;         /* 逻辑宽度（point），必须 > 0 */
    uint32_t height;
    float scale;            /* 0 = 跟随输出（Xft.dpi / wl_output.scale / Cocoa backingScaleFactor）；
                             * 必须有限且非负；>0 强制（小于 0.5 时沿用 1）。
                             * Wayland 会取最近的整数 buffer scale，最小为 1。 */
    int vsync;              /* 非 0：请求交换间隔 1 / 帧回调 / Metal displaySyncEnabled。
                             * Soft 由窗口合成器调度，不保证逐帧 vsync。 */
} neox_present_desc;

typedef struct neox_window neox_window;

typedef enum neox_window_event_kind {
    NEOX_WINDOW_NONE = 0,
    NEOX_WINDOW_CLOSE,
    NEOX_WINDOW_RESIZE,     /* 逻辑尺寸已变 */
    NEOX_WINDOW_SCALE,      /* 输出缩放已变 */
    NEOX_WINDOW_KEY,
    NEOX_WINDOW_POINTER
} neox_window_event_kind;

typedef struct neox_window_event {
    uint32_t kind;
    uint32_t width, height; /* RESIZE：逻辑 point */
    float scale;
    uint32_t keysym;        /* KEY：X11 keysym。Escape 为 0xff1b。
                             * Wayland/Cocoa 把 Escape 映射成同一个值，普通键为字符。
                             * Cocoa 为首个 UTF-16 code unit；不提供 IME/文本组合事件。 */
    uint32_t keycode;       /* 平台原始：X / evdev / Cocoa keycode */
    int pressed;            /* KEY / 按键：1 按下，0 松开 */
    int button;             /* POINTER 按键：1 左 2 中 3 右；移动为 0 */
    uint32_t buttons;       /* 当前按下的键位，bit0 左 bit1 中 bit2 右 */
    float x, y;             /* POINTER：逻辑 point */
} neox_window_event;

neox_result neox_window_create(const neox_present_desc *desc, neox_window **out);
/* Destroy the window and its attached device, draining in-flight work before
 * closing the native display. Context/device destruction may happen first:
 * the window is detached automatically and remains safe to destroy. */
void        neox_window_destroy(neox_window *win);
const char *neox_window_last_error(void);

/* 后端名："x11"、"wayland" 或 "cocoa"。 */
const char *neox_window_backend_name(const neox_window *win);

void neox_window_metrics(const neox_window *win,
                         uint32_t *points_w, uint32_t *points_h,
                         float *scale,
                         uint32_t *pixels_w, uint32_t *pixels_h);

/* 非阻塞。有事件返回 1。 */
int neox_window_poll(neox_window *win, neox_window_event *out);

/* 改逻辑尺寸。像素尺寸随之变化；下一帧 acquire_target 会重建表面。
 * 非法尺寸或适配器返回失败时保留原 metrics。尺寸须可表示为有符号 32 位；
 * X11 另受像素尺寸 CARD16 限制；Cocoa 显式创建/resize 上限为 16384。
 * 原生窗口管理器的异步决定经 poll 报告。 */
neox_result neox_window_resize(neox_window *win, uint32_t points_w, uint32_t points_h);

/* 请求关闭。之后 poll 得到 NEOX_WINDOW_CLOSE。 */
void neox_window_request_close(neox_window *win);

/* SOFT：CPU 帧缓冲。GPU：Linux 在窗口的 EGL display 上建 GL 设备；
 * macOS 创建 Metal 设备。未编入对应 GPU 后端时返回 UNSUPPORTED。
 * A window accepts one device at a time; duplicate creation fails. The returned
 * device is borrowed from the context. After its destruction the window can
 * attach a new device, but acquire/present fail until then. */
neox_result neox_window_create_device(neox_window *win, neox_context *ctx,
                                      neox_backend_kind backend,
                                      neox_device **out_dev);

/* 填 neox_frame_begin 用的 target。尺寸是像素。SOFT 指向窗口内部缓冲；
 * GPU 指向平台呈现所用表面。尺寸变化后必须再次调用。
 * SOFT 会清空缓冲；活动帧或尚未完成的提交存在时拒绝重新获取，
 * 避免破坏其借用的存储。返回的 target 不得跨下一次成功 acquire 使用。 */
neox_result neox_window_acquire_target(neox_window *win, neox_device *dev,
                                       neox_target_desc *out_target);

/* 相关 submission 完成之后调用。Linux：eglSwapBuffers / XPutImage / wl_shm；
 * macOS：Metal drawable / Core Animation。窗口显示为不透明 RGB。
 * Metal present 等待 GPU 复制完成以保护源纹理；普通 present 不回读像素至 CPU。 */
neox_result neox_window_present(neox_window *win);

/* GPU: request a one-shot snapshot of the next present, after acquire_target.
 * Soft: no extra readback is needed. Ordinary present never captures GPU pixels. */
neox_result neox_window_request_readback(neox_window *win);
/* RGBA8 top-left, stride pixels_w*4. GPU requires request_readback before the
 * latest present; Metal snapshots the drawable (opaque alpha), not the offscreen
 * surface. Soft reads the window, last submitted image (Cocoa, original alpha),
 * or its current CPU framebuffer; this is not a screenshot of desktop composition.
 * After a size change, present/readback require a newly acquired target. */
neox_result neox_window_read_pixels(neox_window *win, uint8_t *out_pixels);

#ifdef __cplusplus
}
#endif
#endif /* NEOX_PRESENT_H */
