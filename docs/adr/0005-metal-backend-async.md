# ADR 0005：Metal 后端与异步 submission 模型（poll/block 钩子）

- 状态：已接受（2026-10-25）
- 背景：Metal 是第一个真异步后端。GL 后端用 `glFinish` 假同步，无法验证
  方案文档 §4.3"提交后存活期"契约；内部 vtable 也未预置异步收割概念。
- 决定：
  1. vtable 增加 `poll`/`block` 钩子：`execute` 只编码并提交 command buffer，
     submission 保持 PENDING；`completedHandler`（Metal 线程）只在锁内置标志，
     不触碰 neox 状态（不破坏"核心不创建线程"模型）；`neox_device_pump` 经
     `poll` 非阻塞收割（resolve 读回 → COMPLETE → 解除引用计数）；
     `neox_submission_wait` 经 `block` 等待真实进度。执行期占用的纹理挂
     inflight 条目，完成前不归还池。
  2. GL/Metal 复用 `NEOX_BACKEND_GPU`，经 platform 参数块第二字段魔数
     （`'NXGL'`/`'NXMT'`）分发；该约定正式写入 neox.h 注释。
  3. dual-source blending 以管线创建探测决定，不支持时回退 ping-pong
     手动混合（两条路径均经全部一致性测试真实 exercise）。
- 后果：异步契约得到真实验证；第四个异步后端可直接复用该模式。Metal
  计时 8.32ms（真异步）介于 Soft 6.26ms 与 GL 12.76ms 之间（512×512，
  Debug，含读回）。已修复过程中暴露的 GL 纹理池扩容悬垂指针（固定容量
  1024 项 + 回归测试）。
