# NeoX third_party

vendored 第三方代码。按 design-v0.2.md §9 依赖策略：vendor 降低安装难度，
但不消除审计、维护、升级责任；保留下面列出的局部失败处理补丁。

## stb

| 项 | 内容 |
|---|---|
| 项目 | nothings/stb — `stb_truetype.h` |
| 版本 | v1.26（2021-08-28，文件头版本注释） |
| 来源 | https://github.com/nothings/stb （经 jsDelivr CDN 镜像获取：`gh/nothings/stb@master`，与 raw.githubusercontent.com 内容一致） |
| 获取日期 | 2026-10-26 |
| 许可证 | 文件头声明 public domain；stb 仓库整体以 MIT / Unlicense 双授权发布（以宽松条款为准） |
| 用途 | NeoX-Text 模块的 TrueType/TTC 字体解析与字形光栅化（A8 覆盖率位图） |
| 裁剪方式 | 单头文件全量引入，含下述局部 OOM 补丁 |
| 依赖级别 | 模块必需（可裁剪）：`NEOX_WITH_TEXT=OFF` 时核心与其余测试完全不依赖本目录 |

## NeoX 局部补丁（2026-10）

未重写上游风格或算法。为固定预算和失败注入保留以下改动，升级版本时逐项复查：

- 两版扫描线光栅器：动态扫描线分配失败即返回，避免 NULL 指针运算/写入。
- 两版活动边创建：移除分配成功断言，使用已有的 NULL 返回分支。
- SDF：数据、顶点和预计算缓冲不可用时释放其余对象并返回 NULL。
- CFF：输出顶点分配失败不进入解码；第二遍解码失败时释放已分配数组。

`text/text.c` 通过 STBTT_malloc/free 注入上下文分配器，记录失败状态；
光栅化结束检查状态并释放不完整结果。测试 `test_text_memory` 逐分配点失败，
要求 OOM、图集未插入、重试成功、按原尺寸释放且 context 最终零存活字节。
这验证资源失败契约，不代表已完成任意损坏字体输入的安全审计。
