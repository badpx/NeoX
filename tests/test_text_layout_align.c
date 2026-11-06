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

    /* 1. 行尾空白不计宽，词间空白计宽：aa(18) + 空格(10) + bb(18) = 46，
     * 右对齐把词尾贴到 max_width（评审 4489969067）。 */
    OK(nx_text_layout(t, f, 10, "aa bb   ", 50, NX_ALIGN_RIGHT, &l));
    NEAR(l->width, 46.0f);
    NEAR(l->glyphs[0].x, 4.0f);           /* 50 − 46 */
    OK(nx_text_measure(l, box));
    NEAR(box[0], 4.0f);
    NEAR(box[2], 50.0f);
    nx_layout_destroy(l);

    /* 2. 词间空白（后面还有内容）计入行宽：aa(18) + 空格(10) + bb(18) = 46。
     * 行首空白丢弃，行尾空白不计宽、也不进入字形数组。 */
    OK(nx_text_layout(t, f, 10, " aa bb ", 0, NX_ALIGN_LEFT, &l));
    CHECK(l->line_count == 1);
    CHECK(l->glyph_count == 5);           /* 行首/行尾空格不进入字形数组 */
    NEAR(l->width, 46.0f);
    nx_layout_destroy(l);
    /* 2b. 行宽确实覆盖到词尾：右对齐到 100 时最后一个字形不越过 100。 */
    OK(nx_text_layout(t, f, 10, "aa bb", 100, NX_ALIGN_RIGHT, &l));
    NEAR(l->width, 46.0f);
    NEAR(l->glyphs[l->glyph_count - 1].x, 100.0f - 9.0f);
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

    /* P2-9：早期参数失败清空输出（不留下旧布局指针 / 旧包围盒）。 */
    {
        nx_layout *stale = l;
        float bb[4] = { 1, 2, 3, 4 };
        l = (nx_layout *)t;
        CHECK(nx_text_layout(t, f, 0.0f, "aa", 0, NX_ALIGN_LEFT, &l) ==
              NEOX_ERROR_INVALID_ARGUMENT && l == NULL);
        l = (nx_layout *)t;
        CHECK(nx_text_layout(t, f, -1.0f, "aa", 0, NX_ALIGN_LEFT, &l) ==
              NEOX_ERROR_INVALID_ARGUMENT && l == NULL);
        l = (nx_layout *)t;
        CHECK(nx_text_layout(t, f, 10, "aa", 0, (nx_text_align)99, &l) ==
              NEOX_ERROR_INVALID_ARGUMENT && l == NULL);
        /* 评审 4489969121：measure 失败保留调用方输出（既有语义）。 */
        CHECK(nx_text_measure(NULL, bb) == NEOX_ERROR_INVALID_ARGUMENT &&
              bb[0] == 1 && bb[1] == 2 && bb[2] == 3 && bb[3] == 4);
        (void)stale;
    }

    /* 评审 4489969115：layout 不立即光栅化，0.2px 这类此前可用的字号必须
     * 继续可用；真正需要防护的是量化到 0 的极端值，在图集入口钳到 1。 */
    {
        nx_layout *small = NULL;
        OK(nx_text_layout(t, f, 0.2f, "aa", 0, NX_ALIGN_LEFT, &small));
        CHECK(small->width > 0.0f);               /* 0.2px 不再被拒绝 */
        nx_layout_destroy(small);
        small = NULL;
        OK(nx_text_layout(t, f, 0.001f, "aa", 0, NX_ALIGN_LEFT, &small));
        nx_layout_destroy(small);
        {
            /* 图集入口：size_q 最小为 1，绝不把 0 传给光栅回调。 */
            neox_device *dev = NULL;
            nx_atlas *at = NULL;
            nx_atlas_rect rc;
            OK(neox_device_create(ctx, NULL, &dev));
            OK(nx_atlas_create(t, dev, 16, 16, &at));
            OK(nx_atlas_glyph(at, f, 'a', 0.2f, &rc));
            OK(nx_atlas_glyph(at, f, 'a', 0.02f, &rc));
            CHECK(nx_text_last_error(t) != NULL);  /* 量化钳制留下说明 */
            CHECK(nx_atlas_glyph(at, f, 'a', 0.0f, &rc) == NEOX_ERROR_INVALID_ARGUMENT);
            CHECK(nx_atlas_glyph(at, f, 'a', -1.0f, &rc) == NEOX_ERROR_INVALID_ARGUMENT);
            nx_atlas_destroy(at);
            neox_device_destroy(dev);
        }
    }

    /* P2-11：U+3000（表意空格）是空白，不是 CJK 可断行字符：行首被丢弃。 */
    {
        nx_layout *lj = NULL;
        OK(nx_text_layout(t, f, 10, "\xE3\x80\x80" "aa", 0, NX_ALIGN_LEFT, &lj));
        /* 评审 4489969125：只有 ASCII 空格/制表参与行首/行尾丢弃；U+3000
         * 可断行但作为内容字符保留字形并计入行宽（旧实现把它当 CJK 字符）。 */
        CHECK(lj->glyph_count == 3);
        NEAR(lj->glyphs[0].x, 0.0f);
        NEAR(lj->width, 27.0f);
        nx_layout_destroy(lj);
        /* 评审 4489969125：只有 ASCII 空格/制表是行尾可丢弃空白；U+3000 保留
         * 字形并参与行宽，不换行空白（NBSP）同样按内容处理。 */
        OK(nx_text_layout(t, f, 10, "aa" "\xE3\x80\x80", 100, NX_ALIGN_RIGHT, &lj));
        NEAR(lj->width, 27.0f);
        NEAR(lj->glyphs[0].x, 73.0f);             /* 100 − 27 */
        CHECK(lj->glyph_count == 3);
        nx_layout_destroy(lj);
        /* 不换行空格（U+00A0）不能作为断行点：它把 "aa\u00A0bb" 连成一个词
         * （总宽 46），宽度 25 时整词换到下一行而不是在 NBSP 处断开；行首的
         * 前导也一样——这里用 "aa\u00A0bb" 紧接开头。 */
        OK(nx_text_layout(t, f, 10, "aa" "\xC2\xA0" "bb", 25, NX_ALIGN_LEFT, &lj));
        CHECK(lj->line_count == 1);
        CHECK(lj->glyph_count == 5);              /* 没有任何字符被当作行尾空白删除 */
        /* 宽度：aa(18) + NBSP(adv 9，非 ' ' 所以按字形宽) + bb(18) = 45。 */
        NEAR(lj->width, 45.0f);
        nx_layout_destroy(lj);
        /* 对照：同样宽度的普通空格可以在 25 处断开（"aa " 放得下，bb 换行）。 */
        OK(nx_text_layout(t, f, 10, "aa bb", 25, NX_ALIGN_LEFT, &lj));
        CHECK(lj->line_count == 2);
        CHECK(lj->glyph_count == 4);              /* 行尾空格被删除 */
        nx_layout_destroy(lj);
        /* 行内 U+3000 仍然占位（它不是行尾空白）。 */
        OK(nx_text_layout(t, f, 10, "aa" "\xE3\x80\x80" "bb", 0, NX_ALIGN_LEFT, &lj));
        CHECK(lj->glyph_count == 5);
        NEAR(lj->glyphs[2].x, 18.0f);             /* aa 之后是 U+3000 */
        NEAR(lj->glyphs[3].x, 27.0f);             /* bb 从 18 + 9 开始 */
        NEAR(lj->width, 45.0f);                   /* U+3000 在内容区按 9 计宽 */
        nx_layout_destroy(lj);
    }

    nx_font_destroy(f);
    nx_text_context_destroy(t);
    neox_context_destroy(ctx);
    puts("ALL PASS (trailing whitespace, alignment-aware measure, size policy)");
    return 0;
}
