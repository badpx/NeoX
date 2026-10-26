# Linux 窗口呈现

呈现是平台胶水，不是图形语义的一部分（design-v0.2 §5，semantics-v0 §2）。
核心和 Soft/GL 执行器都不包含 X11 或 Wayland 分支。调用方仍然用 point
描述窗口；本层把 `point × scale` 换成帧缓冲像素，负责窗口、事件、vsync，
以及把已经画完的一帧送上屏幕。

## 窗口系统

首选 Wayland，X11 是回退。`WAYLAND_DISPLAY` 非空时先连 Wayland，连不上再
试 X11。`NEOX_PRESENT_BACKEND=wayland` 或 `x11` 可以强制，强制之后不再回退。

选 Wayland 做首选，是因为新的 Linux 桌面会话以它为原生协议，HiDPI 用整数
buffer scale 表达，和「point 与像素分开」的分层一致。X11 仍要覆盖：CI、
Xvfb、以及只导出 `DISPLAY` 的环境都还在用它。不走 GLX。GL 上屏一律是 EGL
window surface（`EGL_PLATFORM_WAYLAND_KHR` / `EGL_PLATFORM_X11_KHR`）。

依赖和离屏 EGL 一样，运行期 `dlopen`，不进链接行：

| 库 | 用途 |
|---|---|
| `libwayland-client.so.0` | 连接、xdg-shell、wl_shm |
| `libwayland-egl.so.1` | `wl_egl_window`，只有 GL 上屏才打开 |
| `libX11.so.6` | 窗口、`XPutImage` / `XGetImage` |
| `libEGL.so.1` | 窗口 GL 上下文（离屏路径原有） |

找不到库或没有显示时，`neox_window_create` 失败，离屏 API 不受影响。
源文件不包含 Xlib / Wayland 头，因此没装 `-dev` 包也能编译。xdg-shell
的接口表写在 `src/present_wl.c` 里（stable 协议，与 wayland-scanner 1.22
的描述一致），因为 `libwayland-client` 不导出这些符号。X11 事件布局按
LP64 写死，其它 ABI 在打开窗口时直接拒绝。

## 和 GL 执行器的边界

`neox_gl_surface` 仍然是离屏 FBO。`read_pixels` 的朝向不变：设备顶行在
GL 帧缓冲的底边，读回即左上原点，不必再翻。

上屏多一个 EGL window surface。`present` 用一张翻转 shader 把 FBO 画进
窗口（窗口的底边才是视觉底边），交换缓冲，再把 current 换回离屏表面。
交换前会 `glReadPixels` 再翻回左上，留给 `neox_gl_surface_read_presented`。

Surfaceless 的 `EGLDisplay` 不能创建 window surface，所以窗口上的 GL 设备
必须建在该窗口自己的 display 上。`neox_gl_device_desc` 末尾追加了两个字段：

- `native_display`：`Display*` 或 `wl_display*`，`NULL` 表示原来的离屏路径
- `native_kind`：`NEOX_GL_NATIVE_X11` 或 `NEOX_GL_NATIVE_WAYLAND`

调用方继续 `memset` 后填写 `struct_size = sizeof(...)`。新字段为零时行为
与只做离屏的旧调用相同。macOS 的 CGL 实现忽略它们，窗口表面一律不支持。
公开的 `neox_target_desc` 没有改：Soft 仍是调用方像素，GPU 仍是
`target.surface`。

Soft 上屏不经过 GL：X11 用 `XPutImage`，Wayland 用 `wl_shm` 的
`XRGB8888`。半透明像素会按 alpha 预乘后写入不透明窗口；测试用的不透明色
不受影响。

## 缩放、vsync、事件

`neox_present_desc.scale == 0` 时跟随输出：X11 读 `Xft.dpi / 96`（可以是
分数），Wayland 用 `wl_output.scale`。大于 0 则锁定。Wayland 的 buffer
scale 只能是整数，锁定值会取最近的整数，`neox_window_metrics` 报告的就是
这个整数，因此 `像素 = round(point × scale)` 在 Wayland 上是精确的。

`vsync != 0` 时请求 `eglSwapInterval(1)`，Wayland 的软件帧再等一次 frame
callback（最多约 100ms）。Xvfb 上交换间隔失败不算错误，帧照样送出。

库不建线程，也不自己跑循环。`neox_window_poll` 非阻塞，取出关闭、逻辑尺寸
变化、输出缩放、键盘和指针。Escape 在两条后端上都是 keysym `0xff1b`。
`neox_window_request_close` 让下一次 poll 得到 `NEOX_WINDOW_CLOSE`。
窗口的逻辑尺寸由 `neox_window_resize` 立即改掉；Wayland 上同时把
xdg min/max 设成这个尺寸，避免合成器把它拉成输出大小。

`neox_window_acquire_target` 给出的目标是像素尺寸。应用若要按 point 绘制，
自己压入 `{scale, 0, 0, scale, 0, 0}`，并在 `neox_frame_end` 前弹出。
`neox_window_destroy` 会释放 `neox_window_create_device` 得到的设备
（先拆掉 GL 表面并 `eglTerminate`，再断开 Wayland/X11）。调用方不要再
销毁这台设备。context 仍由调用方销毁。

X11 读回优先 `XGetImage`（窗口里的像素）。Wayland 没有通用的屏幕读取：
Soft 回退到刚提交的缓冲，GL 用交换前的窗口帧缓冲。

## 构建与运行

运行时包（Ubuntu 24.04）：

```sh
sudo apt-get install -y libx11-6 libwayland-client0 libwayland-egl1 \
    libgl1-mesa-dri xvfb weston
sudo apt-get install -y libegl1 libegl-mesa0 || sudo apt-get install -y libegl1-mesa
```

无头 X11：

```sh
xvfb-run -a -s "-screen 0 1280x1024x24" ./build/ex05_window
```

无头 Wayland：

```sh
rt=$(mktemp -d)
chmod 700 "$rt"
export XDG_RUNTIME_DIR="$rt"
weston --backend=headless --socket=neox-wl --width=1280 --height=1024 &
WAYLAND_DISPLAY=neox-wl NEOX_PRESENT_BACKEND=wayland ./build/ex05_window
```

`NEOX_PRESENT_GPU=1` 走 GL，否则 Soft。`NEOX_PRESENT_FRAMES=2` 画两帧：
第一帧写成 `ex05_<backend>_<soft|gl>_before.bmp`，窗口从 320×240 放大到
480×320，第二帧写成 `*_after.bmp`，然后退出。不设置帧数时一直画到关窗或
Escape。

`ctest` 里的 `present` 与 `ex05_smoke` 在没有显示时跳过。
`NEOX_REQUIRE_PRESENT=1` 把跳过改成失败，给 CI 的 `ubuntu-present` 用。
默认的 `ubuntu-gl` 不设显示，这两项保持跳过，离屏一致性测试不依赖窗口。

## 本机验收

Clang 18，`-Wall -Wextra -Werror`。Mesa 25.2.8，llvmpipe（LLVM 20.1.2）。

- 无显示：`ctest` 9/9。`present` 与 `ex05_smoke` 跳过，离屏 Soft/GL（含 `NEOX_GL_FORCE_NO_DUAL`）通过。
- Xvfb（`-screen 0 1280x1024x24`，`NEOX_PRESENT_BACKEND=x11`）：`test_present` 覆盖 Soft 与 GL、scale 1 与 2、放大后重画、`request_close`。顶行红、中行蓝、底行绿，朝向与 Soft 一致。
- weston 13 `--backend=headless`：同一组测试通过。`ex05_window` 在两条后端、Soft 与 GL 上都写出 320×240 与放大后的 480×320。四组成对的读回颜色相同（顶 `(230,56,46)`、中 `(242,199,51)`、底 `(51,191,89)`）。
- Xvfb 上 EGL 会提示没有 DRI3，随后用 llvmpipe。weston 上 Mesa 会先试一次 ZINK 再落到 llvmpipe。两条都不是失败。

## 已知限制

- 只实现了 Linux，而且 X11 事件布局只接受 LP64。
- 不处理输入法、剪贴板、装饰以外的窗口状态，也不把指针锁定或滚轮轴做成独立事件。
- Wayland 客户端把窗口 min/max 设成请求的逻辑尺寸，合成器拖拽改大小通常不会发生；程序自己调用 `neox_window_resize`。
- Wayland GL 读的是交换前的窗口帧缓冲，不是合成器合成之后的屏幕。
- 不支持 fractional-scale-v1。分数缩放只在 X11（`Xft.dpi`）上保留。
- `eglSwapInterval` 在 Xvfb 上常常失败，垂直同步是尽力而为。
- 没有 DMA-BUF / GBM。软件帧是一次拷贝，GL 帧是一次纹理绘制再交换。

## 显式截图与所有权（2026-10 更新）

窗口接口现在由独立的 `neox_present` 目标提供，它公开链接 `neox`；只使用核心的
消费方不会链接 X11/Wayland 适配对象。截图调用顺序为 acquire → render/wait →
`neox_window_request_readback` → present → `neox_window_read_pixels`。
GL 正常 present 不分配截图缓冲、不回读、不调用 glFinish。显式截图使用一块可复用
RGBA8 缓冲并原地翻转；下一次未请求截图的 present 或 resize 使旧截图失效。
这仅消除了呈现步骤的额外成本，当前参考 GL 执行器内部仍有回读与同步，不能据此
宣称整帧已是 GPU 原生路径。

context 是 device 的存储所有者，window 持有可自动失效的借用引用。window 销毁
会请求 device 销毁，并在设备排空/取消提交、销毁 EGL 后关闭原生 display。
提前销毁 context 或 device 会将 window 脱离设备，随后销毁 window 安全；重复
create_device 返回错误，设备销毁后允许重新关联。无显示环境的 present_contracts
测试覆盖这些顺序；真实 X11/Wayland 截图方向与 resize 继续由 Linux CI 验证。


### Resize 的失败边界

GL 颜色附件先在独立对象中分配，成功后才替换旧附件。普通分配/FBO 错误保持
旧尺寸、旧像素和已有截图；成功变更尺寸后旧像素未保留，截图失效。有活动录制
或未完成提交引用该表面时，尺寸变更返回 INVALID_ARGUMENT。

窗口层先更新 X11 / wl_egl_window 的原生尺寸；GL 表面保留原 EGL window surface
关联。依据 [EGL 1.5 §3.5.1 与 §3.10.1](https://registry.khronos.org/EGL/specs/eglspec.1.5.pdf)，
窗口表面随原生窗口调整，最迟在交换时更新尺寸，不需要每次销毁重建关联。
GPU 分配失败不回滚外部窗口尺寸；目标获取失败后可在下一帧重试。设备失效
仍按终态契约处理，不保证 GPU 内容保留。

`test_gl_surface_faults` 对实际 GL 执行代码注入纹理分配/FBO 完整性错误，验证
旧内容、截图、资源释放和重试；另注入 GL_CONTEXT_LOST 验证错误映射。
这些测试使用真实离屏 GL，不等价于 X11/Wayland resize 的驱动运行验收。


### 尺寸与缓冲使用边界

逻辑和像素维度均须能用有符号 32 位整数表示，RGBA 字节数不得溢出 size_t。
X11 的窗口宽高另受 [X11 协议 CARD16](https://xorg.freedesktop.org/releases/current/doc/xproto/x11protocol.html)
限制，创建和请求 resize 在发送前拒绝超范围尺寸。Wayland 的双缓冲共享内存池
总字节数须能通过 [wl_shm.create_pool 的有符号 size](https://wayland.freedesktop.org/docs/html/apa.html)
表达；这个约束只属于共享内存呈现，不强加给 GL 目标。

resize 返回失败保留原 metrics；成功或 poll 收到实际尺寸变化后，调用方须重新
acquire_target，再呈现或读回。Soft 的 acquire 会清空缓冲，活动帧/排队提交尚
借用它时明确拒绝，调用方先完成或取消工作。已返回的 target 不得跨下一次
成功 acquire 使用。GL 旧尺寸的截图也不能写入按新尺寸分配的输出缓冲。

present_adapter_bounds 在无显示服务器时执行实际适配代码的尺寸和事件分支，
使用模拟 native dispatch；它不证明真实协议连接、窗口生命周期或呈现结果。


## 本轮完整运行验收

在 Ubuntu 24.04 ARM64、Mesa 25.2.8 llvmpipe、Weston 13.0.0 下，开启
ASan/UBSan 与 warnings-as-errors，离屏 41/41，X11/Wayland 的 Soft/GL
窗口及示例四组各 2/2 通过；GL/Text/Present 八组合与模块消费矩阵全部通过。
软件 GL 驱动验证 EGL 和渲染路径，不代替物理 GPU 驱动覆盖。

环境依赖就绪后可执行 `bash tests/check_linux.sh /tmp/neox-linux-acceptance`。
脚本不安装依赖，只在指定输出目录构建；Xvfb 和 Weston 由脚本启动，退出时
清理 compositor。必需的 GL/窗口不可用即失败。详细范围见 implementation-review.md。
