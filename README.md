# NeoX

<p align="center"><img src="logo.png" width="128" alt="NeoX logo"></p>

NeoX 是一个以**图形语义契约**为中心的跨平台 2D 图形库：矢量路径、位图、文字与图层合成。它是 NeoSDK（2003–2007，DOS 时代的 C 语言图形库）的精神续作——不继承其接口，但继承其"头文件级简洁、源码全开放"的美学。

**English**: NeoX is a cross-platform 2D graphics library built around a verifiable graphics-semantics contract: vector paths, bitmaps, text and layer compositing, with multiple execution backends (software rasterizer + GPU) proven consistent against a single specification.

## 设计要点

- **三个独立契约**：图形语义（SD）、内部执行表示（ER，后端私有）、捕获/传输格式（CAP）。正常本地绘制不经过序列化。
- **立即模式与保留模式同为前端**：立即 API 与保留场景树汇入同一执行系统；录制、保留、增量执行是三个分开的能力。
- **明确的失败语义**：后端无法忠实执行时明确失败，近似执行须显式授权且可在正式构建中查询实际执行路径。
- **资源受限环境友好**：注入分配器、可固定内存预算、核心不创建线程也不假设线程存在，目标覆盖桌面、移动、浏览器（WASM）与嵌入式。
- **可验证性**：同一内容经立即/保留两条路径、经不同后端执行，结果须满足规范容差（特定参考场景逐位一致，距离场按容差验证）。

详见 [docs/design-v0.2.md](docs/design-v0.2.md)（方案文档）、[docs/semantics-v0.md](docs/semantics-v0.md)（语义规范）与 [docs/adr/](docs/adr/)（架构决策记录）。

## 当前状态

| 组件 | 状态 |
|---|---|
| 语义规范 v0（最小子集） | 三后端验证通过，草案冻结 |
| Soft 后端（软件光栅） | 完成，参照实现 |
| GL 后端（OpenGL 4.1 Core；macOS CGL / Linux EGL surfaceless） | 完成，链路验证定位 |
| Linux 窗口呈现（Wayland 优先，X11 回退；Soft 与 GL） | 完成，平台胶水层 |
| Metal 后端（macOS，真异步） | 完成，首个验证"提交后存活期"契约的后端 |
| 保留模式场景树 | 完成（脏区追踪为粗粒度，增量执行在第二阶段） |
| CAP 捕获/回放（最小版） | 完成，含跨后端回放 |
| NeoX-Text（A8 覆盖率 + SDF/MSDF 距离场图集，简化排版） | 可用；内置 MSDF 生成器为实验实现（ADR 0006/0007） |
| GLES3 / WebGL2 / WASM | 第二阶段 |

测试包含语义、三后端一致性、生命周期、立即/保留/CAP 组合、裁剪性质与文字模块。
特定参考场景三后端逐位一致，距离场场景按规范 §8.2 容差验收；这不代表任意内容的质量或性能已被证明。
独立距离场质量与离线资产约定见 [docs/distance-field-quality.md](docs/distance-field-quality.md)。
架构审阅后的实施进度和验证范围见 [docs/implementation-review.md](docs/implementation-review.md)。第三方依赖：仅模块级 vendored
stb_truetype（见 [docs/dependencies.md](docs/dependencies.md)），核心零依赖。

## 构建与测试

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

- 依赖：C11 编译器 + CMake；无第三方库。Linux GL 后端运行期 `dlopen("libEGL.so.1")`，另需 Mesa 的软件光栅驱动（无显示器也可跑，不需要 X11/Wayland）。Ubuntu 24.04：

```sh
sudo apt-get install -y build-essential cmake libgl1-mesa-dri fonts-dejavu-core
sudo apt-get install -y libegl1 libegl-mesa0 || sudo apt-get install -y libegl1-mesa
```

- macOS 上默认启用 GL 与 Metal（`NEOX_BACKEND_GL` / `NEOX_BACKEND_METAL`）。Linux 默认启用 GL（EGL surfaceless + GL 4.1 Core）和窗口呈现（`NEOX_PRESENT`），Metal 关闭。`-DNEOX_BACKEND_GL=OFF` 仍可只编 Soft；`-DNEOX_PRESENT=OFF` 去掉窗口层。没有显示器时窗口测试与 `ex05_window` 自动跳过，离屏测试照常。
- 示例：`build/ex01_scene`（Soft）、`build/ex02_gpuscene`（GL）、`build/ex03_mtlscene`（Metal，仅 macOS）、`build/ex04_text`（中英混排；Linux 需上述字体或文泉驿）输出 BMP 到 build 目录。`build/ex05_window` 打开真实窗口（`NEOX_PRESENT_GPU=1` 走 GL，`NEOX_PRESENT_FRAMES=2` 画两帧后退出并写出 resize 前后的 BMP）。`NEOX_GL_LOG=1` 打印 EGL 路径与渲染器；`NEOX_GL_FORCE_NO_DUAL=1` 强制关掉 dual-source，走 ping-pong 回退。
- 离屏方案见 [docs/linux-port.md](docs/linux-port.md)，上屏方案见 [docs/linux-present.md](docs/linux-present.md)。Mesa llvmpipe 25.2.8 上 Soft 与 GL 的一致性场景（含关掉 dual-source 的回退路径）实测逐位一致。

## 模块裁剪与安装消费

Scene、CAP、Text 可分别通过 `NEOX_WITH_SCENE`、`NEOX_WITH_CAPTURE`、
`NEOX_WITH_TEXT` 关闭。Scene/CAP 的实现从核心目标中排除，关闭 Scene 也不再
分配场景/节点槽位表；Text 和 Present 是独立静态库。测试和示例分别由
`NEOX_BUILD_TESTS`、`NEOX_BUILD_EXAMPLES` 控制：顶层构建默认开启，作为
`add_subdirectory` 子项目默认关闭，可显式覆盖。

只构建软件立即模式库：

```sh
cmake -S . -B build-min -DNEOX_BACKEND_GL=OFF -DNEOX_BACKEND_METAL=OFF \
  -DNEOX_PRESENT=OFF -DNEOX_WITH_TEXT=OFF -DNEOX_WITH_SCENE=OFF \
  -DNEOX_WITH_CAPTURE=OFF -DNEOX_SINGLE_THREADED=ON \
  -DNEOX_BUILD_TESTS=OFF -DNEOX_BUILD_EXAMPLES=OFF
cmake --build build-min
cmake --install build-min --prefix /path/to/neox
```

下游只需安装目录，配置时设置 `CMAKE_PREFIX_PATH=/path/to/neox`：

```cmake
find_package(NeoX 0.1 CONFIG REQUIRED COMPONENTS Core)
target_link_libraries(my_app PRIVATE NeoX::core)
# 启用对应模块的安装包还提供 NeoX::text、NeoX::present。
# find_package 的可选组件名：Scene、Capture、Text、Present、GL、Metal。
```

安装的 `neox_config.h` 与库来自同一构建，`neox.h` 自动包含它；使用
`#if NEOX_ENABLE_SCENE` / `#if NEOX_ENABLE_CAPTURE` / `#if NEOX_ENABLE_TEXT`
查询前端/Text 能力，关闭的 Scene/CAP API 不声明也不链接。后端和 Present
沿用 `#ifdef NEOX_ENABLE_GL` / `NEOX_ENABLE_METAL` / `NEOX_ENABLE_PRESENT`。
安装包会传递静态库需要的数学库、系统框架与 sanitizer 链接选项。
直接手工编译源码时，也必须使用同一 CMake 配置生成的头文件。

`installed_consumer` 测试会安装到临时前缀、移动整个安装目录，再从独立目录
编译运行消费程序。`python3 tests/check_modules.py --workdir /tmp/neox-modules`
验证 Scene/CAP/Text 的八种组合、缺失组件拒绝、测试/示例开关及子目录消费。
Python 只用于这项开发验收，不是库的构建或运行依赖。
Linux 完整平台和配置验收可运行 `bash tests/check_linux.sh /tmp/neox-linux-acceptance`
（需预先具备 Mesa/EGL、Xvfb、Weston 与测试字体）。

## 许可证

MIT，见 [LICENSE](LICENSE)。

开发验收建议启用 `-DNEOX_SANITIZE=ON -DNEOX_WARNINGS_AS_ERRORS=ON`，CTest 将使
UBSan 遇错终止；GPU 验收设置 `NEOX_REQUIRE_GL=1 NEOX_REQUIRE_METAL=1`。
使用窗口 API 的消费方链接独立目标 `neox_present`；纯核心消费方链接 `neox`。
GL 窗口截图需在 present 前调用 `neox_window_request_readback`，普通 present 不回读。
每个返回的 submission 用完报告后调用 `neox_submission_release`，避免长期保留记录。

GL 的 `neox_gl_device_desc.output_mode = NEOX_GL_OUTPUT_GPU` 显式选择 GPU
目标快照与输出转换，避免帧开始/结束的 CPU 像素往返；默认保持 CPU 参考量化。
该模式在执行报告标记近似输出，当前验收误差为每通道 ≤1/255，几何与混合规则不变。
`neox_gl_device_get_stats` 可查询像素传输、读回、等待及已知纹理存储量。
GL 仍在每次提交末等待完成；路径覆盖率仍在 CPU 生成。详见 ADR 0008。


GPU 一致性测试支持 GL-only、Metal-only 或两者同时启用；无可用 GPU 时 CTest
明确显示 SKIP。硬件验收应设置 `NEOX_REQUIRE_GL=1` / `NEOX_REQUIRE_METAL=1`，
使要求的后端未编入或不可用时失败。Metal 混合矩阵另覆盖
`NEOX_MTL_REQUIRE_DUAL=1`（要求真实双源）和 `NEOX_MTL_FORCE_NO_DUAL=1`
（强制手动回退）；这两个测试开关不可同时用于正常验收。
托管 CI 的单后端作业允许显式 SKIP，不代替有硬件的必执行验收。
