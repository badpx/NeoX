# NeoX 依赖清单

按 design-v0.2.md §9 三级分类：**核心必需 / 模块必需（可裁剪）/ 仅工具链**。
核心级依赖目标数量为零——当前核心（`src/core.c`、`geom.c`、`soft.c`、
`gl.c`、`metal.mm`）除 C/POSIX 标准库与平台系统库外无第三方依赖。
macOS 链接 OpenGL.framework / Metal.framework / Foundation.framework /
QuartzCore.framework；可选 Cocoa 窗口模块另链接 AppKit.framework。Linux GL 后端不在链接期依赖 libEGL：运行期
`dlopen("libEGL.so.1")`，并需要一份桌面 GL 4.1 驱动（CI 与无头环境用
Mesa llvmpipe，包名见下）。找不到库时 GL 设备创建返回
`NEOX_ERROR_BACKEND`，Soft 仍可运行。

## 模块必需（可裁剪）

| 依赖 | 版本 | 许可证 | 用途 | 裁剪方式 | 获取 |
|---|---|---|---|---|---|
| stb_truetype.h（notions/stb） | v1.26（2021-08-28） | public domain（仓库以 MIT/Unlicense 双授权发布） | NeoX-Text 字体解析与字形 A8 光栅化 | 单头文件全量 vendor，保留少量 OOM 检查补丁 | `third_party/stb/`（详见 `third_party/README.md`） |

- 构建开关：`NEOX_WITH_TEXT=OFF` 时 `neox_text` 库、测试与示例不参与构建，
  核心与其余测试不受影响（ubuntu CI 无字体环境走此路径 + 集成测试 SKIP）。
- stb 的 `STBTT_malloc/free` 经 Text 上下文的注入分配器，临时与持久内存都
  计入核心 `memory_budget`；显式替换 Text 分配器也不绕过预算。
  分配尺寸和 owner 保存在对齐前缀中，释放无需全局状态。
- vendored stb 保留局部 OOM 补丁：扫描线 NULL 检查、活动边失败返回、SDF
  数据/预计算缓冲失败清理、CFF 输出缓冲失败清理。外层捕捉分配失败标志，
  避免 stb 的 void 函数提前返回后把不完整位图当成成功结果。
  补丁范围与升级要求见 `third_party/README.md`。
- 文件 I/O 的系统内部缓冲、调用方自定义字体后端自身分配不在库的自动预算
  内；受限环境使用内存字体或离线资产，核心不强制依赖文件 I/O。

## 平台系统框架（Apple 后端，链接期）

| 框架 | 后端 | 说明 |
|---|---|---|
| OpenGL.framework | GL 后端（已 deprecated，链路验证定位） | 仅 macOS，`-DNEOX_BACKEND_GL=OFF` 可裁 |
| Metal / Foundation / QuartzCore.framework | Metal 后端 | 仅 macOS，`-DNEOX_BACKEND_METAL=OFF` 可裁 |
| AppKit / QuartzCore.framework | macOS 窗口呈现（Soft + Metal） | 仅 `neox_present` 链接；`-DNEOX_PRESENT=OFF` 可裁，核心不引入 AppKit |
| libEGL.so.1（运行期 dlopen，非链接依赖） | Linux GL 后端 | Ubuntu 24.04：`libegl1` + `libegl-mesa0`（旧版本包名 `libegl1-mesa`）。驱动：`libgl1-mesa-dri`（`swrast_dri.so` = llvmpipe） |
| libX11.so.6、libwayland-client.so.0、libwayland-egl.so.1（运行期 dlopen，非链接依赖） | Linux 窗口呈现 | 包：`libx11-6`、`libwayland-client0`、`libwayland-egl1`。缺库时只有窗口创建失败，离屏 Soft/GL 仍可运行。不需要 `-dev` 头。构建开关：`-DNEOX_PRESENT=OFF` |
| fonts-dejavu-core（仅测试/示例） | NeoX-Text 集成 | `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf`；CJK 可选文泉驿 / Noto |

## 仅工具链

CMake ≥ 3.16、C11 编译器、macOS 上 ObjC++ 编译器（Metal 后端或 Cocoa 窗口模块）。


模块边界：Text 只包含私有 `src/module.h` 的不透明分配/归属服务和检查过的
字节算术，不读取核心 context/device/frame 布局。该私有头不安装；公开消费
仅使用 `neox.h`、已启用模块的公开头和自动生成的 `neox_config.h`。
Scene/CAP 开关不引入新依赖，安装与独立消费方式见 README。
