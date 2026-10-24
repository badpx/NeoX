# ADR 0004：首个 GPU 后端选 OpenGL/CGL（链路验证定位）

- 状态：已接受（2026-10-24）
- 背景：第一阶段需要第二个实现来验证语义规范。候选：Metal（原生但需
  ObjC/C++ 桥接）、WebGPU/wgpu-native（引入大型第三方依赖）、OpenGL
  （已 deprecated 但纯 C、零依赖、CGL 离屏上下文无需窗口系统）。
- 决定：以 OpenGL 4.1 Core + CGL 离屏上下文实现最小 GPU 后端，定位为链路
  验证；完整的 GLES3/WebGL2 后端属第二阶段，Metal/Compute 属第三阶段。
- 后果：以最小工作量获得"SD → GPU 执行 → 读回验证"全链路；三角形批渲染
  管线经验可直接服务 GLES3/WebGL2。已知限制：macOS 专用（CGL）；§6.1 语义
  依赖 dual-source blending（`ARB_blend_func_extended`），GLES3 无此 core
  特性、WebGL2 完全没有，未来后端对 SRC/SRC_IN/SRC_OUT/DST_ATOP 四模式须
  回退 ping-pong 手动混合（ER 自由，SD 不受影响）。
