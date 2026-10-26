# NeoX Linux 平台适配方案

> 状态：待实施（跟踪：GitHub issue #1）
> 读者：在 Linux 环境执行适配的开发者。本文假设你已通读
> [design-v0.2.md](design-v0.2.md) 与 [adr/0004-gl-validation-backend.md](adr/0004-gl-validation-backend.md)。

## 1. 目标与范围

**目标**：NeoX 在 Linux（x86_64 / aarch64）上完成构建、测试全绿，且 GL 后端
与 Soft 后端通过跨后端一致性测试（§9 容差内，预期逐位一致——见 §5 风险）。

**非目标（本期不做，平台胶水层职责，后续单独立项）**：

- 窗口系统呈现（X11/Wayland surface、交换链、vsync）；
- DMA-BUF / GBM 零拷贝互操作；
- fontconfig 字体发现（NeoX-Text v0 只接受显式字体路径/内存缓冲）。

## 2. 现状盘点

| 组件 | 可移植性 | 需要做什么 |
|---|---|---|
| 核心（core.c / geom.c / soft.c） | 纯 C11 + libc，已可移植 | ubuntu CI（Soft-only）已绿，无需改动 |
| NeoX-Text（stb_truetype） | stb 本身可移植 | 仅测试字体的获取路径（§4-W6） |
| GL 后端 `src/gl.c` | **macOS 专用** | 上下文创建（CGL）与函数入口解析是仅有的平台相关点 |
| Metal 后端 | macOS/iOS 限定 | 不在 Linux 范围内 |

`gl.c` 中平台相关的只有两处：

1. **上下文创建**：文件头部自声明的 CGL 函数族（`gl.c:54-66`）与
   `gl_device_init`（`gl.c:758`）中的 CGLChoosePixelFormat/CGLCreateContext/
   CGLSetCurrentContext（`gl.c:357`）调用；
2. **GL 函数入口**：macOS 上 OpenGL.framework 直接导出全部符号，静态链接
   即可；Linux 上 GL ≥1.2 的函数必须经 `eglGetProcAddress` / `glXGetProcAddress`
   或 `dlsym` 动态获取。

其余全部（shader、FBO、纹理池、ping-pong 混合、CPU resolve）都是标准
GL 4.1 Core，无平台耦合。

## 3. 架构原则（必须遵守）

- **平台差异用文件/链接组合表达，不用预处理器污染主干**：上下文创建拆成
  独立编译单元，CMake 按平台选源文件。禁止在 gl.c 里出现 `#ifdef __linux__`
  的上下文创建分支。
- 契约不变：`neox_gl.h` 的公开 API 不动（`neox_gl_device_desc` 已有
  struct_size + magic 版本协商，Linux 实现复用同一 desc）；sdl/外部 GL
  上下文的接入仍走 `platform` 参数块。
- 全部测试语义不变：Linux 上的验收就是现有 `test_gpu_consistency`
  在非 SKIP 模式下跑通。

## 4. 工作分解

### W1：上下文创建抽象（重构 gl.c）

将 gl.c 中 CGL 相关代码抽到 `src/gl_ctx_cgl.c`（macOS），新建
`src/gl_ctx_egl.c`（Linux/Android 通用），二者实现同一个内部接口
（建议放在 `src/gl_ctx.h`）：

```c
/* 内部接口草案（非公开头文件） */
typedef struct nx_gl_ctx nx_gl_ctx;
nx_gl_ctx *nx_gl_ctx_create(const neox_gl_device_desc *desc); /* NULL=自建离屏 */
int        nx_gl_ctx_make_current(nx_gl_ctx *);
void       *nx_gl_ctx_get_proc(nx_gl_ctx *, const char *name);
void       nx_gl_ctx_destroy(nx_gl_ctx *);
```

`gl.c` 只经此接口触碰上下文，CGL/EGL 字样不再出现在 gl.c。

### W2：GL 入口点微型加载器（Linux）

新建 `src/gl_load.c`（仅 Linux 链接）：对 gl.c 使用的每个 GL 函数（当前约
32 处调用点，逐一列出清单）定义函数指针，经
`nx_gl_ctx_get_proc()` 填充；获取失败即 `NEOX_ERROR_BACKEND`。
macOS 下保持静态链接现状（`gl_load` 不参与编译或为空实现）。
**不引入 GLEW/glLoadGen**（零依赖原则，ADR 0004）。

### W3：EGL surfaceless 上下文（首选路径）

`gl_ctx_egl.c` 实现：

1. `eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, ...)`——无头、
   不需要 X/Wayland，Mesa llvmpipe 支持；
2. 不支持时回退 `EGL_PLATFORM_DEVICE`（显式选 device），再回退 GLX（W4 备选）；
3. `eglChooseConfig`：OpenGL API（`EGL_OPENGL_API` 经 `eglBindAPI`）、
   **桌面 GL 而非 GLES**——现有 shader 是 GLSL 410；
4. PBuffer 或 surfaceless 表面（渲染目标是自有 FBO，不需要真实 surface，
   `EGL_NO_SURFACE` + surfaceless 上下文即可）；
5. 上下文属性：4.1 Core Profile（`EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT`）。

依赖库：`libEGL.so.1` + Mesa 驱动。**运行期 dlopen，不做链接期硬依赖**
（与 NeoX 的"后端可独立链接"策略一致；找不到库 = 后端不可用 =
`NEOX_ERROR_BACKEND`，进程仍可跑 Soft）。

### W4：备选 GLX 路径

仅在 EGL surfaceless/device 不可用时需要（老 Mesa）。Xvfb + GLX 1.4 创建
3.2+ core 上下文（`glXCreateContextAttribsARB`）。若 CI 实测 EGL 路径稳定，
本项可砍掉。

### W5：CMake 接线

```cmake
if(UNIX AND NOT APPLE)
  option(NEOX_BACKEND_GL "OpenGL backend (Linux EGL surfaceless + GL 4.1 Core)" ON)
endif()
```

- 源文件选择：`APPLE → gl_ctx_cgl.c`，`LINUX → gl_ctx_egl.c + gl_load.c`；
- 链接 `${CMAKE_DL_LIBS}`（dlopen）；
- gl.c 保持零改动（W1 完成后）。

### W6：测试与 CI

- `test_gpu_consistency` 在 Linux 首次真正运行（当前 SKIP 路径保留给无
  GL 环境）；**验收标准 = Soft↔GL 25+1 场景逐位一致**（同 macOS）；
- CI 新增 `ubuntu-gl` job：

```yaml
ubuntu-gl:
  runs-on: ubuntu-latest
  steps:
    - uses: actions/checkout@v4
    - run: sudo apt-get update && sudo apt-get install -y libegl1-mesa libgl1-mesa-dri
    - run: cmake -S . -B build -DNEOX_BACKEND_GL=ON && cmake --build build
    - run: ctest --test-dir build --output-on-failure
```

- NeoX-Text 集成测试字体：ubuntu CI 安装 `fonts-dejavu-core`，测试按
  候选路径列表探测（`/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf` 等），
  全部缺失才 SKIP；中文字体在 CI 不强求（CJK 集成测试保持 macOS-only，
  字体路径已有多候选探测）。

## 5. 风险与已知坑

1. **逐位一致性未必立即达成**：macOS 三方逐位一致的前提是"覆盖率来自共享
   CPU 光栅化 + 混合在 RGBA32F 浮点层 + CPU resolve 量化"（ADR 0003）——
   这条链路里 GPU 只做确定性浮点运算。Mesa llvmpipe 的浮点实现与 Apple GL
   可能存在 1-ulp 级差异，经 CPU resolve 的 ties-to-even 量化后**可能**在
   个别像素翻转。**预期仍逐位一致；若失败**，按规范 §9 的容差条款验收
   （内部精确 + 边界 ≤32/255）并记录实测偏差，不要为了追平 macOS 修改
   共享几何代码。
2. **dual-source blending**：llvmpipe 支持 `ARB_blend_func_extended`，但
   实现必须走"探测 + ping-pong 回退"双路径（Metal 后端 ADR 0005 已有同款
   模式与强制回退开关，照抄）。
3. **llvmpipe 性能**：软渲染跑 26 场景一致性测试可能需要数十秒，CI 超时
   设宽一点；不要在 CI 里跑性能断言。
4. **无显示环境**：GitHub runner 无 X；surfaceless EGL 是正因如此成为首选。
   若被迫走 GLX，用 `xvfb-run -a ctest ...`。
5. **确定性边界**：stb_truetype 光栅化同版本同平台确定，跨平台不保证——
   文本一致性测试只在同机三后端间比较，不与 macOS 黄金图比对。

## 6. 执行清单（可勾选）

- [ ] W1 `src/gl_ctx.h` + `gl_ctx_cgl.c` 拆分，macOS 侧测试无回归
- [ ] W2 `src/gl_load.c` 函数清单与加载器
- [ ] W3 `gl_ctx_egl.c`（surfaceless → device 探测链）
- [ ] W5 CMake 接线 + dlopen 链接
- [ ] W6 本机 `ctest` 全绿（Soft↔GL 一致性非 SKIP 通过）
- [ ] W6 CI `ubuntu-gl` job 绿灯
- [ ] 关闭 issue #1，ADR 0004 补充 Linux 落地记录
