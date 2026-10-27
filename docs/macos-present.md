# macOS 窗口呈现

`neox_present` 在 macOS 使用 Cocoa/AppKit 建窗，沿用 Linux 的公开窗口 API。
默认启用；`NEOX_PRESENT=OFF` 可移除整个窗口模块，离屏 Soft/GL/Metal 不受影响。
核心不链接 AppKit，也不处理窗口、输入和应用事件循环。

## 两条呈现路径

| 设备 | 路径 | 截图 |
|---|---|---|
| Soft | CPU RGBA8 → 独立 CGImage → NSView / Core Animation | 读取最近提交的图像；尚未呈现时可读当前 CPU 缓冲 |
| GPU | Metal surface → GPU 全屏三角形 → CAMetalLayer drawable → presentDrawable | 显式请求时，从本次实际 drawable 复制到共享缓冲并转换 BGRA → RGBA |

macOS 的 `NEOX_BACKEND_GPU` 窗口设备固定使用 Metal；没有编入 Metal 时返回
`NEOX_ERROR_UNSUPPORTED`。macOS GL 仍用于离屏渲染，不建立另一套 CGL 窗口路径。
两条窗口路径均显示不透明 RGB；离屏表面的 alpha 语义保持不变。
Soft 读回保留原 CPU 图像 alpha，Metal drawable 的 alpha 为 255。

Soft 提交时复制图像，避免下一次 acquire 清空/重分配缓冲影响系统仍在显示的内容。
Metal 普通 present 不分配读回缓冲、不执行 CPU 像素读回；显式请求截图才增加
GPU blit 和 CPU 格式转换。截图代表提交的 drawable，不包括窗口装饰、遮挡和
系统色彩管理后的桌面合成结果。

Metal present 当前同步等待 GPU 复制完成，保护下一帧 CPU resolve 可能改写的源纹理。
这不等于渲染全程没有 CPU/GPU 往返：现有 Metal 执行器的覆盖率生成及 CPU resolve
仍然保留。异步呈现和 GPU resolve 是独立的后续性能工作，不属于本次窗口接入。

## 调用与生命周期

所有窗口 API、附属设备/上下文的操作及销毁都在 macOS 主线程执行。
库不创建应用事件线程，不调用 `NSApplication.run`，也不覆盖宿主应用的 delegate。
`neox_window_poll` 非阻塞派发 AppKit 应用事件，并把 NeoX 窗口事件交给调用方；
宿主已有 AppKit 循环时也可继续使用。每次原生轮询限制派发事件数，避免输入流饿死渲染。

每帧顺序：

1. 处理窗口事件，读取 point、scale 和 pixel metrics。
2. `neox_window_acquire_target`，把 target 交给 `neox_frame_begin`。
3. 录制并结束帧，等待 submission 完成，释放 submission 句柄。
4. 需要截图时调用 `neox_window_request_readback`。
5. `neox_window_present`；需要截图时再 `neox_window_read_pixels`。

同一个窗口只能挂接一个设备。设备或上下文先销毁时自动脱离窗口；窗口先销毁时先
排空设备提交再释放原生层。脱离后可创建新设备，包括 Metal 与 Soft 之间切换。
活动帧或未完成提交存在时，拒绝重新 acquire 或 present；resize 不释放旧表面，
下一次合法 acquire 才重建，避免在途帧访问已释放的存储。

窗口尺寸或 scale 改变后必须重新 acquire，即使又恢复到原尺寸也不复活旧 target。
成功 acquire、resize 或普通 GPU present 都使旧显式截图失效。
非法 resize 保留旧 metrics 和 target。Cocoa 显式创建/resize 的逻辑/像素尺寸上限为
16384；实际可分配尺寸还受系统、渲染后端与内存限制。

## Retina 与输入

`desc.scale=0` 跟随窗口 `backingScaleFactor`，包括屏幕迁移带来的变化；正值锁定
渲染倍率。窗口大小、鼠标坐标使用 point，target 使用 pixel。调用方用 scale 设置
绘制变换；库不会暗中替换图形状态。resize 和 backing scale 通知更新 metrics 并投递事件。

支持关闭请求、resize、scale、键盘按下/松开、鼠标移动/拖动和左/中/右键。
Cocoa `keyCode` 原样提供；Escape 统一为 `0xff1b`，普通 keysym 使用
`charactersIgnoringModifiers` 的首个 UTF-16 code unit。这是当前通用键盘事件契约，
不提供 IME、文本组合、触控/滚轮等尚未定义的公开事件。
点击系统关闭按钮只投递 CLOSE，由调用方决定销毁时机。

`vsync` 在 Metal 路径请求 `displaySyncEnabled`；Soft 由窗口合成器调度，不能保证
调用 present 与显示刷新逐帧对应。当前窗口为 SDR sRGB，不支持 HDR 或透明窗口。

## 构建和验收

需 macOS SDK 的 AppKit/QuartzCore 与 ObjC++ 编译器，无新增第三方依赖。
Metal 框架只在启用 Metal 后端时链接。图形验收必须在已登录的 macOS 桌面会话运行：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DNEOX_PRESENT=ON -DNEOX_WARNINGS_AS_ERRORS=ON -DNEOX_SANITIZE=ON
cmake --build build --parallel 6
NEOX_REQUIRE_PRESENT=1 NEOX_REQUIRE_METAL=1 NEOX_REQUIRE_GL=1 \
  ctest --test-dir build --output-on-failure

# 交互示例：默认 Soft；GPU 在 macOS 选择 Metal。Escape 或关闭按钮退出。
build/ex05_window
NEOX_PRESENT_GPU=1 build/ex05_window
# 自动 resize、保存两张 BMP 后退出：
NEOX_PRESENT_GPU=1 NEOX_PRESENT_FRAMES=2 build/ex05_window
```

`present` 检查 Soft/Metal 上下朝向、1x/2x 缩放、resize 后像素和截图时机；
`cocoa` / `cocoa_metal` 在真实 NSWindow 上检查自动 Retina metrics、键鼠事件转换、
原生和 API resize、失败回滚、活动/在途帧保护、关闭、主线程约束、销毁及重新挂接；
`ex05_smoke` / `ex05_metal_smoke` 运行完整示例。
Metal 不可用时 GPU 测试可报告 SKIP；设置上述 REQUIRE 标志后不可跳过。
没有 WindowServer/桌面会话的执行器应禁用 Present，不能用离屏通过替代窗口验收。

### 本次本机记录（2026-10-27）

- Debug + ASan/UBSan + warnings-as-errors，GL/Metal/Present/Text 全开：56/56 通过，
  设置 REQUIRE_PRESENT / REQUIRE_METAL / REQUIRE_GL，窗口和 GPU 测试无跳过。
- GL/Metal/Text 全关，Present 保留：22/22 通过，包括真实 Soft 窗口和 GPU 不支持分支。
- Present/GL/Text 关闭，Metal 保留：28/28 通过；消费程序链接行不含 AppKit。
- 三个配置均通过移动安装目录后的独立消费测试。

自动测试验证了真实 NSWindow 和 Metal drawable 的像素；本次屏幕锁定，未完成
桌面截图的人工视觉复核。跨物理显示器迁移、不同 Intel/AMD GPU 的行为尚未实机覆盖。
