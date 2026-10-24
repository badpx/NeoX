# ADR 0002：覆盖率对所有混合模式取插值语义

- 状态：已接受（2026-10-24）
- 背景：Soft 参照实现最初对 CLEAR/DST 等"Porter-Duff 因子不含源"的模式
  在 cov>0 时整像素生效（如 CLEAR 整像素清零）。GL 后端复刻该行为时发现
  其数学上不一致：覆盖率在这些模式下没有起到"参与比例"的作用。
- 决定：规范 §6.1 定义 `out = cov·blend(src,dst) + (1−cov)·dst`，对全部
  13 种模式统一。CLEAR 成为抗锯齿擦除 `dst·(1−cov)`；DST 恒无视觉变化。
  PLUS 的 clamp 作用于 blend 内部（先 clamp 后插值）。§6.1 只管绘制原语；
  组不透明度/裁剪走源调制 + 全覆盖 Porter-Duff，不受其管辖。
- 后果：与 Cairo/PDF 等成熟 2D 模型的语义一致；GL 端四个模式需要
  dual-source blending 表达（见 ADR 0004 的移植性注记）。
