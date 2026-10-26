# NeoX third_party

vendored 第三方代码。按 design-v0.2.md §9 依赖策略：vendor 降低安装难度，
但不消除审计、维护、升级责任；原文未改（裁剪方式 = 原样引入）。

## stb

| 项 | 内容 |
|---|---|
| 项目 | nothings/stb — `stb_truetype.h` |
| 版本 | v1.26（2021-08-28，文件头版本注释） |
| 来源 | https://github.com/nothings/stb （经 jsDelivr CDN 镜像获取：`gh/nothings/stb@master`，与 raw.githubusercontent.com 内容一致） |
| 获取日期 | 2026-10-26 |
| 许可证 | 文件头声明 public domain；stb 仓库整体以 MIT / Unlicense 双授权发布（以宽松条款为准） |
| 用途 | NeoX-Text 模块的 TrueType/TTC 字体解析与字形光栅化（A8 覆盖率位图） |
| 裁剪方式 | 原文未改，单头文件 `stb_truetype.h` 全量引入 |
| 依赖级别 | 模块必需（可裁剪）：`NEOX_WITH_TEXT=OFF` 时核心与其余测试完全不依赖本目录 |
