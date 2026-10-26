# NeoX

<p align="center"><img src="logo.png" width="128" alt="NeoX logo"></p>

NeoX 是一个以**图形语义契约**为中心的跨平台 2D 图形库：矢量路径、位图、文字与图层合成。它是 NeoSDK（2003–2007，DOS 时代的 C 语言图形库）的精神续作——不继承其接口，但继承其"头文件级简洁、源码全开放"的美学。

**English**: NeoX is a cross-platform 2D graphics library built around a verifiable graphics-semantics contract: vector paths, bitmaps, text and layer compositing, with multiple execution backends (software rasterizer + GPU) proven consistent against a single specification.

## 设计要点

- **三个独立契约**：图形语义（SD）、内部执行表示（ER，后端私有）、捕获/传输格式（CAP）。正常本地绘制不经过序列化。
- **立即模式与保留模式同为前端**：立即 API 与保留场景树汇入同一执行系统；录制、保留、增量执行是三个分开的能力。
- **明确的失败语义**：后端无法忠实执行时明确失败，近似执行须显式授权且可在正式构建中查询实际执行路径。
- **资源受限环境友好**：注入分配器、可固定内存预算、核心不创建线程也不假设线程存在，目标覆盖桌面、移动、浏览器（WASM）与嵌入式。
- **可验证性**：同一内容经立即/保留两条路径、经不同后端执行，结果须满足规范容差（当前 Soft 与 GL 后端**逐位一致**）。

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
| NeoX-Text（A8 覆盖率 + SDF/MSDF 距离场图集，简化排版） | 完成，三后端一致（ADR 0006/0007） |
| GLES3 / WebGL2 / WASM | 第二阶段 |

测试规模：835 项断言（255 语义 + 484 跨后端一致性 + 96 文字模块），
**三后端两两逐位一致**（距离场场景按规范 §8.2 容差验收），ASan/UBSan 干净。第三方依赖：仅模块级 vendored
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

## 许可证

MIT，见 [LICENSE](LICENSE)。
