/* ===========================================================================
 * neox_present.h — 桌面窗口呈现（平台胶水，不是图形语义的一部分）
 *
 * 坐标仍是用户单位（point）。本层把 point × scale 映射成帧缓冲像素，
 * 负责窗口、关闭/缩放/指针/键盘事件、vsync 与把已完成的帧送上屏幕。
 * 核心与 Soft/GL 执行器不包含 X11/Wayland 分支。
 *
 * Linux：运行期 dlopen libX11 / libwayland-client / libwayland-egl。
 * 首选 Wayland（WAYLAND_DISPLAY 可用时），否则 X11。
 * NEOX_PRESENT_BACKEND=wayland|x11 可强制。找不到显示时创建失败，
 * 离屏 API 不受影响。
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
    float scale;            /* 0 = 跟随输出（Xft.dpi / wl_output.scale）；
                             * >0 强制。Wayland 会取最近的整数 buffer scale。 */
    int vsync;              /* 非 0：请求交换间隔 1 / 帧回调。失败仍呈现。 */
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
                             * Wayland 把 KEY_ESC 映射成同一个值，ASCII 键为字符。 */
    uint32_t keycode;       /* 平台原始：X keycode 或 evdev keycode */
    int pressed;            /* KEY / 按键：1 按下，0 松开 */
    int button;             /* POINTER 按键：1 左 2 中 3 右；移动为 0 */
    uint32_t buttons;       /* 当前按下的键位，bit0 左 bit1 中 bit2 右 */
    float x, y;             /* POINTER：逻辑 point */
} neox_window_event;

neox_result neox_window_create(const neox_present_desc *desc, neox_window **out);
/* 释放窗口，以及 neox_window_create_device 创建的设备。
 * 设备必须留到这里再释放：Wayland 上要先 eglTerminate，再断开 wl_display。
 * 调用方不要再 neox_device_destroy 那台设备。 */
void        neox_window_destroy(neox_window *win);
const char *neox_window_last_error(void);

/* 后端名："x11" 或 "wayland"。 */
const char *neox_window_backend_name(const neox_window *win);

void neox_window_metrics(const neox_window *win,
                         uint32_t *points_w, uint32_t *points_h,
                         float *scale,
                         uint32_t *pixels_w, uint32_t *pixels_h);

/* 非阻塞。有事件返回 1。 */
int neox_window_poll(neox_window *win, neox_window_event *out);

/* 改逻辑尺寸。像素尺寸随之变化；下一帧 acquire_target 会重建表面。 */
neox_result neox_window_resize(neox_window *win, uint32_t points_w, uint32_t points_h);

/* 请求关闭。之后 poll 得到 NEOX_WINDOW_CLOSE。 */
void neox_window_request_close(neox_window *win);

/* SOFT：CPU 帧缓冲。GPU：在该窗口的 EGL display 上建 GL 设备
 * （不是 surfaceless），才能创建 EGL window surface。 */
neox_result neox_window_create_device(neox_window *win, neox_context *ctx,
                                      neox_backend_kind backend,
                                      neox_device **out_dev);

/* 填 neox_frame_begin 用的 target。尺寸是像素。SOFT 指向窗口内部缓冲；
 * GL 指向窗口表面。尺寸变化后必须再次调用。 */
neox_result neox_window_acquire_target(neox_window *win, neox_device *dev,
                                       neox_target_desc *out_target);

/* neox_device_pump 完成之后调用。GL：eglSwapBuffers。SOFT：XPutImage 或 wl_shm。 */
neox_result neox_window_present(neox_window *win);

/* 最近一次 present 的像素，RGBA8 左上原点，行距 pixels_w*4。
 * X11 尽量读窗口本身；Wayland GL 读交换前的窗口帧缓冲。 */
neox_result neox_window_read_pixels(neox_window *win, uint8_t *out_pixels);

#ifdef __cplusplus
}
#endif
#endif /* NEOX_PRESENT_H */
