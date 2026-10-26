# NeoX 依赖清单

按 design-v0.2.md §9 三级分类：**核心必需 / 模块必需（可裁剪）/ 仅工具链**。
核心级依赖目标数量为零——当前核心（`src/core.c`、`geom.c`、`soft.c`、
`gl.c`、`metal.mm`）除 C/POSIX 标准库与平台系统库外无第三方依赖。
macOS 链接 OpenGL.framework / Metal.framework / Foundation.framework /
QuartzCore.framework。Linux GL 后端不在链接期依赖 libEGL：运行期
`dlopen("libEGL.so.1")`，并需要一份桌面 GL 4.1 驱动（CI 与无头环境用
Mesa llvmpipe，包名见下）。找不到库时 GL 设备创建返回
`NEOX_ERROR_BACKEND`，Soft 仍可运行。

## 模块必需（可裁剪）

| 依赖 | 版本 | 许可证 | 用途 | 裁剪方式 | 获取 |
|---|---|---|---|---|---|
| stb_truetype.h（notions/stb） | v1.26（2021-08-28） | public domain（仓库以 MIT/Unlicense 双授权发布） | NeoX-Text 字体解析与字形 A8 光栅化 | 原文未改，单头文件全量 vendor | `third_party/stb/`（详见 `third_party/README.md`） |

- 构建开关：`NEOX_WITH_TEXT=OFF` 时 `neox_text` 库、测试与示例不参与构建，
  核心与其余测试不受影响（ubuntu CI 无字体环境走此路径 + 集成测试 SKIP）。
- stb 的瞬态光栅化中间分配走系统分配器（`STBTT_malloc` 默认实现，光栅化
  后立即释放）；NeoX-Text 的持久内存（字体缓冲、排版结果、图集、缓存表）
  全部经调用方注入的分配器。

## 平台系统框架（Apple 后端，链接期）

| 框架 | 后端 | 说明 |
|---|---|---|
| OpenGL.framework | GL 后端（已 deprecated，链路验证定位） | 仅 macOS，`-DNEOX_BACKEND_GL=OFF` 可裁 |
| Metal / Foundation / QuartzCore.framework | Metal 后端 | 仅 macOS，`-DNEOX_BACKEND_METAL=OFF` 可裁 |
| libEGL.so.1（运行期 dlopen，非链接依赖） | Linux GL 后端 | Ubuntu 24.04：`libegl1` + `libegl-mesa0`（旧版本包名 `libegl1-mesa`）。驱动：`libgl1-mesa-dri`（`swrast_dri.so` = llvmpipe） |
| libX11.so.6、libwayland-client.so.0、libwayland-egl.so.1（运行期 dlopen，非链接依赖） | Linux 窗口呈现 | 包：`libx11-6`、`libwayland-client0`、`libwayland-egl1`。缺库时只有窗口创建失败，离屏 Soft/GL 仍可运行。不需要 `-dev` 头。构建开关：`-DNEOX_PRESENT=OFF` |
| fonts-dejavu-core（仅测试/示例） | NeoX-Text 集成 | `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf`；CJK 可选文泉驿 / Noto |

## 仅工具链

CMake ≥ 3.16、C11 编译器、macOS 上 ObjC++ 编译器（Metal 后端）。
