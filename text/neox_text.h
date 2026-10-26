/* ===========================================================================
 * neox_text.h — NeoX-Text 模块 v0：字体加载、简化排版、动态 A8 图集
 *
 * 职责划分（design-v0.2.md §8）：核心只接受已定位字形（neox_glyph + 图集
 * 纹理，三模式见 neox_atlas_mode）；本模块负责字体解析、简化排版（LTR、
 * 贪心断行、无复杂整形）、动态图集缓存与绘制桥。SDF/MSDF 图集经
 * nx_atlas_create_ex 创建（stb 后端：SDF 用 stbtt_GetGlyphSDF，MSDF 为
 * 简化版自实现——真距离、拐角 >60° 边着色，尖角内侧可略圆，ADR 0006）。
 *
 * 范围边界（v0 明确不做）：
 *   - 复杂文字整形（阿拉伯语形变、印地语等）；拉丁连字仅预留开关位
 *     （nx_text_context 的 flags），默认关闭；
 *   - 双向文本（Bidi）；富文本样式；字体回退链（fallback）。
 *
 * 内存：持久对象和 stb 光栅化临时分配均计入 core_ctx 的 memory_budget。
 * alloc 可覆盖实际分配器，但不会绕过共享预算；为 NULL 时使用核心分配器。
 * tctx 及其对象必须先于 core_ctx 销毁；共享同一 core_ctx 的内存操作需串行化。
 * 自定义字体后端自身的分配由调用方管理，不在本模块的自动预算范围。
 *
 * 失败语义：与核心一致——失败返回非 OK，状态不留半完成；last_error 为
 * 静态描述串。
 * =========================================================================*/
#ifndef NEOX_TEXT_H
#define NEOX_TEXT_H

#include "neox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 上下文 */

typedef struct nx_text_context nx_text_context;

/* [own] 创建文本上下文。alloc 为 NULL 时使用核心分配器；均共享核心预算。 */
neox_result nx_text_context_create(neox_context *core_ctx,
                                   const neox_allocator *alloc, /* [borrow] */
                                   nx_text_context **out_ctx);
/* [own] 级联销毁全部字体/图集 */
void        nx_text_context_destroy(nx_text_context *tctx);
const char *nx_text_last_error(nx_text_context *tctx); /* 静态串，无错误返回 NULL */

/* 模块级开关位（v0 仅定义，拉丁连字默认关闭） */
#define NX_TEXT_ENABLE_LIGA 0x1u
void nx_text_context_set_flags(nx_text_context *tctx, uint32_t flags);

/* ------------------------------------------------------------------ 字体 */

typedef struct nx_font nx_font;

/* [own] 从文件加载（整个文件读入模块缓冲，随字体存活） */
neox_result nx_font_load_file(nx_text_context *tctx, const char *path,
                              int ttc_index, nx_font **out_font);
/* [own] 从内存加载（data 在 [borrow]；模块内部拷贝，调用方可立即释放） */
neox_result nx_font_load_memory(nx_text_context *tctx, const void *data,
                                size_t size, int ttc_index, nx_font **out_font);
/* [own] 显式销毁（亦可随文本上下文级联销毁）。先销毁借用此字体的 layout。
 * 图集不借用字体对象：已缓存像素仍可存活，新字体不会因地址复用误命中。 */
void        nx_font_destroy(nx_font *font);

/* 度量（size_px 像素字号；descent <= 0；line_gap 可为 0 或负）。
 * size_px 须有限且非负；结果超出有限 float 时返回 0，并设置 text last_error。
 * NULL font 返回 0。 */
float nx_font_ascent(nx_font *font, float size_px);
float nx_font_descent(nx_font *font, float size_px);
float nx_font_line_gap(nx_font *font, float size_px);

/* 测试/专用注入：自定义字体后端（排版与图集测试不依赖字体文件）。
 * codepoint → glyph 恒等映射；度量/字距/光栅化由回调提供。
 * bear_x/bear_y 语义同 stbtt_GetGlyphBitmapBox：位图左上角相对
 * 基线原点的偏移（bear_y 通常 <= 0，即位于基线上方）。 */
typedef struct nx_font_backend {
    void *user;
    float (*glyph_advance)(void *user, uint32_t glyph_index, float size_px);
    /* Optional; NULL means zero kerning. Advances/kerning/metrics must be finite. */
    float (*glyph_kerning)(void *user, uint32_t g1, uint32_t g2, float size_px);
    neox_result (*glyph_rasterize)(void *user, uint32_t glyph_index,
                                   float size_px, uint8_t **out_bmp,
                                   int32_t *out_w, int32_t *out_h,
                                   float *out_bear_x, float *out_bear_y);
    /* 可选：SDF/MSDF 距离场光栅化。NULL → 该字体的 SDF/MSDF 图集返回
     * NEOX_ERROR_UNSUPPORTED（COVERAGE 不受影响）。编码契约（§8.2）：
     * 距离 d（图集像素，形内为正）→ v = 128 − d·255/dist_range，截断
     * [0,255]（±0.5 LSB 常量偏移，容差内）；SDF 输出单通道 w×h，
     * MSDF 输出 RGBA w×h×4（alpha=255）。bear_x/bear_y 同
     * glyph_rasterize（位图左上角相对基线原点；位图已含 dist_range 边距） */
    neox_result (*glyph_rasterize_sdf)(void *user, uint32_t glyph_index,
                                       float size_px, neox_atlas_mode mode,
                                       float dist_range, uint8_t **out_bmp,
                                       int32_t *out_w, int32_t *out_h,
                                       float *out_bear_x, float *out_bear_y);
    /* Any non-NULL bitmap returned by rasterize (including on failure) is
     * released exactly once. Dimensions must be nonnegative; bearings finite. */
    void (*glyph_free_bitmap)(void *user, uint8_t *bmp);
    void (*destroy)(void *user);
    float ascent, descent, line_gap; /* size_px = 1 时的归一化度量 */
} nx_font_backend;

neox_result nx_font_create_custom(nx_text_context *tctx,
                                  const nx_font_backend *backend, /* [borrow] */
                                  nx_font **out_font);

/* ------------------------------------------------------------------ 排版 */

typedef enum nx_text_align {
    NX_ALIGN_LEFT = 0, NX_ALIGN_CENTER, NX_ALIGN_RIGHT
} nx_text_align;

/* 已定位字形：x/y 为基线原点（首行基线 y=0，向下逐行 +line_height） */
typedef struct nx_laid_glyph {
    uint32_t glyph_index;
    float x, y;
} nx_laid_glyph;

/* 排版结果（glyphs 由布局拥有；font/size_px 供绘制桥内部使用） */
typedef struct nx_layout {
    nx_laid_glyph *glyphs;
    uint32_t glyph_count;
    float width, height;             /* 内容包围盒（已含对齐偏移） */
    float ascent, descent, line_gap; /* 该字号行度量 */
    float line_height;
    uint32_t line_count;
    float size_px;
    nx_font *font;                   /* [borrow] 字体必须比此布局存活更久 */
    /* —— 内部字段（实现使用，调用方勿依赖，v0 后为不透明化预留） —— */
    uint32_t glyph_cap;
    struct nx_line_info *line;       /* 行表：范围 + 自然宽度（对齐前） */
    uint32_t line_cap;
    struct nx_text_context *tctx;
} nx_layout;

/* [own] 简化排版：UTF-8 解码（非法序列替换为 U+FFFD）→ cmap → 字距 →
 * LTR 贪心断行（CJK 逐字可断、拉丁按词；'\n' 强制断行）→ 对齐。
 * size_px/max_width 须有限，size_px > 0；max_width <= 0 表示不换行。
 * 度量/字距或累计坐标无法表示为有限 float 时返回 INVALID_ARGUMENT，
 * 分配失败返回 OUT_OF_MEMORY；失败不返回部分 layout。
 * font 必须属于 tctx；布局须先于字体和 tctx 销毁。 */
neox_result nx_text_layout(nx_text_context *tctx, nx_font *font, float size_px,
                           const char *utf8, float max_width,
                           nx_text_align align, nx_layout **out_layout);
void nx_layout_destroy(nx_layout *layout);

/* 排版结果包围盒 [x0, y0, x1, y1]（布局坐标系，y 向下） */
neox_result nx_text_measure(const nx_layout *layout, float out_bbox[4]);

/* ------------------------------------------------------------------ 动态图集
 * 一张图集 neox_texture（创建时建立，可经 nx_atlas_texture 取得用于其他
 * 核心绘制）。shelf/row 装箱；缓存键 (不复用的字体 ID, glyph, size 量化 0.25px)。
 * 未命中即光栅化写入并记脏矩形，nx_atlas_flush 经 neox_texture_update
 * 增量上传。容量满返回 NEOX_ERROR_OUT_OF_MEMORY（v0 不做 LRU 驱逐，
 * 结构保留）。销毁字体不会回收图集空间；空间随图集销毁释放。
 * 字体和图集须属于同一文本上下文；图集设备须属于该上下文的 core_ctx，
 * 绘制帧须来自图集的设备。图集须先于设备销毁。
 *
 * 图集模式创建时固定（COVERAGE/SDF → A8 纹理，MSDF → RGBA8_DATA）；
 * 缓存天然按图集隔离，即同名字形在不同模式图集中各自光栅化。SDF/MSDF
 * 位图已含 dist_range 边距（编码见 nx_font_backend.glyph_rasterize_sdf）。 */

typedef struct nx_atlas nx_atlas;
typedef struct nx_atlas_rect { uint16_t x, y, w, h; } nx_atlas_rect;

neox_result nx_atlas_create(nx_text_context *tctx, neox_device *dev,
                            uint32_t width, uint32_t height, nx_atlas **out);
/* Built-in MSDF generation is experimental (simple edge coloring, fixed curve
 * subdivision, bitmap-derived sign; no pseudo-distance/error correction).
 * It is not guaranteed to improve on SDF. Offline assets can use the core glyph API.
 * dist_range_px == 0 取默认 4；负数、非有限值或超出 16384 被拒绝。MSDF 需要字体后端支持
 * glyph_rasterize_sdf，否则 nx_atlas_glyph 返回 NEOX_ERROR_UNSUPPORTED */
neox_result nx_atlas_create_ex(nx_text_context *tctx, neox_device *dev,
                               uint32_t width, uint32_t height,
                               neox_atlas_mode mode, float dist_range_px,
                               nx_atlas **out);
void        nx_atlas_destroy(nx_atlas *atlas);
neox_texture nx_atlas_texture(const nx_atlas *atlas);
neox_atlas_mode nx_atlas_get_mode(const nx_atlas *atlas);
float       nx_atlas_get_dist_range(const nx_atlas *atlas);

/* 取字形图集矩形（未命中即光栅化装箱；容量满返回 OUT_OF_MEMORY）。
 * 内置字体后端在生成位图前检查 glyph_index、缩放后尺寸及距离场边距；
 * 装不下的字形不会先分配巨型位图。stb SDF 编码系数无法表示为有限 float
 * 时返回 UNSUPPORTED；MSDF 使用 double 编码系数。失败不插入该字形缓存。 */
neox_result nx_atlas_glyph(nx_atlas *atlas, nx_font *font, uint32_t glyph_index,
                           float size_px, nx_atlas_rect *out_rect);
/* 脏矩形经 neox_texture_update 增量上传（nx_text_draw 内部已调用；
 * 手动管理图集时在用其纹理绘制前调用一次） */
neox_result nx_atlas_flush(nx_atlas *atlas);

/* 缓存统计（测试/诊断） */
void nx_atlas_stats(const nx_atlas *atlas, uint64_t *out_hits, uint64_t *out_misses);

/* ------------------------------------------------------------------ 绘制桥 */

/* 排版结果 → neox_glyph 数组 → neox_draw_glyphs(_ex)。
 * 图集自动 flush；缺失字形（未映射 codepoint → glyph 0）由字体后端渲染
 * .notdef（stb 字形 0 依字体而定，通常为空或方框），不跳过。
 * softness 为 §8.2 边缘过渡宽度（输出像素，0 = 默认 1）；图集模式与
 * dist_range 由图集自身携带（nx_atlas_create_ex），两处恒成对。 */
neox_result nx_text_draw(neox_frame *frame, nx_atlas *atlas,
                         const nx_layout *layout, const neox_paint *paint);
neox_result nx_text_draw_ex(neox_frame *frame, nx_atlas *atlas,
                            const nx_layout *layout, const neox_paint *paint,
                            float softness);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NEOX_TEXT_H */
