/* P1-15 / P1-16：行尾空白不计入行宽与对齐；测量包围盒包含对齐偏移。
 * 用自定义字体后端（advance 可预测、无真实字形）驱动真实排版实现。 */
#include "neox.h"
#include "neox_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define OK(x) CHECK((x) == NEOX_OK)
#define NEAR(a, b) CHECK(((a) - (b) < 0.001f) && ((b) - (a) < 0.001f))

static float adv(void *u, uint32_t g, float s)
{
    (void)u; (void)s;
    return g == ' ' ? 10.0f : 9.0f;
}
static neox_result ras(void *u, uint32_t g, float s, uint8_t **b, int32_t *w,
                       int32_t *h, float *x, float *y)
{
    (void)u; (void)g; (void)s;
    *b = NULL; *w = *h = 0; *x = *y = 0;
    return NEOX_OK;
}
static void fr(void *u, uint8_t *b) { (void)u; (void)b; }

int main(void)
{
    neox_context *ctx = NULL;
    nx_text_context *t = NULL;
    nx_font *f = NULL;
    nx_layout *l = NULL;
    nx_font_backend be;
    float box[4];
    uint32_t i;

    OK(neox_context_create(NULL, &ctx));
    OK(nx_text_context_create(ctx, NULL, &t));
    memset(&be, 0, sizeof be);
    be.glyph_advance = adv;
    be.glyph_rasterize = ras;
    be.glyph_free_bitmap = fr;
    be.ascent = 0.8f;
    be.descent = -0.2f;
    OK(nx_font_create_custom(t, &be, &f));

    /* 1. 行尾空白：宽度按内容算，右对齐把词尾贴到 max_width。 */
    OK(nx_text_layout(t, f, 10, "aa bb   ", 35, NX_ALIGN_RIGHT, &l));
    NEAR(l->width, 18.0f);
    NEAR(l->glyphs[0].x, 17.0f);          /* 35 − 18 */
    OK(nx_text_measure(l, box));
    NEAR(box[0], 17.0f);
    NEAR(box[2], 35.0f);
    nx_layout_destroy(l);

    /* 2. 词间空白（后面还有内容）仍计入行宽：aa(18) + 空格(10) + bb(18)。
     * 首尾空白不计入行宽（首部空白在行首被丢弃）。 */
    OK(nx_text_layout(t, f, 10, " aa bb ", 0, NX_ALIGN_LEFT, &l));
    CHECK(l->line_count == 1);
    CHECK(l->glyph_count == 5);           /* 行尾空格不再进入字形数组 */
    NEAR(l->width, 36.0f);
    nx_layout_destroy(l);

    /* 3. 换行判定仍然把行尾空白算进去：'aa ' 能放进 21，'aa bbbb' 放不下。 */
    OK(nx_text_layout(t, f, 10, "aa bbbb", 21, NX_ALIGN_LEFT, &l));
    CHECK(l->line_count == 2);
    NEAR(l->glyphs[0].x, 0.0f);
    /* 第二行从下一字形开始：第一行只放了 'aa'（空格被丢弃） */
    NEAR(l->glyphs[l->glyph_count - 4].x, 0.0f);
    nx_layout_destroy(l);

    /* 4. 居中与右对齐的包围盒包含偏移（P1-16）。 */
    OK(nx_text_layout(t, f, 10, "aa", 100, NX_ALIGN_RIGHT, &l));
    OK(nx_text_measure(l, box));
    NEAR(box[0], 82.0f);
    NEAR(box[2], 100.0f);
    NEAR(l->width, 18.0f);
    NEAR(l->glyphs[0].x, 82.0f);
    nx_layout_destroy(l);
    OK(nx_text_layout(t, f, 10, "aa", 100, NX_ALIGN_CENTER, &l));
    OK(nx_text_measure(l, box));
    NEAR(box[0], 41.0f);
    NEAR(box[2], 59.0f);
    nx_layout_destroy(l);

    /* 5. 多行混合宽度：x0 = min(line_off)、x1 = max(line_off + width)。 */
    OK(nx_text_layout(t, f, 10, "aa\naaaa", 100, NX_ALIGN_RIGHT, &l));
    CHECK(l->line_count == 2);
    OK(nx_text_measure(l, box));
    NEAR(box[0], 64.0f);                  /* 36 宽的行：off = 64 */
    NEAR(box[2], 100.0f);                 /* 18 宽的行：off = 82 ... 100 */
    NEAR(l->glyphs[0].x, 82.0f);          /* 第一行（aa） */
    NEAR(l->glyphs[2].x, 64.0f);          /* 第二行（aaaa） */
    for (i = 0; i < l->glyph_count; i++)
        CHECK(l->glyphs[i].x >= box[0] && l->glyphs[i].x <= box[2]);
    nx_layout_destroy(l);

    /* 6. 硬换行后的行不受影响。 */
    OK(nx_text_layout(t, f, 10, "aa \nbb", 100, NX_ALIGN_RIGHT, &l));
    CHECK(l->line_count == 2);
    OK(nx_text_measure(l, box));
    NEAR(box[0], 82.0f);                  /* 两行都是 18 宽 */
    nx_layout_destroy(l);

    nx_font_destroy(f);
    nx_text_context_destroy(t);
    neox_context_destroy(ctx);
    puts("ALL PASS (trailing whitespace and alignment-aware measure bounds)");
    return 0;
}
