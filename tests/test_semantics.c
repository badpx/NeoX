/* ===========================================================================
 * test_semantics.c — NeoX 语义断言式测试（无框架依赖，规范 docs/semantics-v0.md）
 *
 * 覆盖（对应交付要求 ①—⑩）：
 *   ① NONZERO vs EVENODD 自相交/双环路径结果差异
 *   ② Porter-Duff 13 模式精确数值断言（§6，线性光下手工推导的期望字节）
 *   ③ 组不透明度 ≠ 逐元素不透明度；组 = 先合成再整体不透明度（§7）
 *   ④ 嵌套裁剪 = 祖先交集，且裁剪抗锯齿参与衰减（§7）
 *   ⑤ draw_texture NEAREST/LINEAR 2x2 放大采样断言；负宽目标矩形镜像（§8）
 *   ⑥ draw_glyphs：A8 图集按 paint 颜色着色（§5）
 *   ⑦ 失败语义：budget 超限/fail_next 下 create/frame_end/pump 失败且目标像素
 *      未被改动（§10.1 原子性）
 *   ⑧ 过期句柄 STALE_HANDLE；跨 device / 跨类型 OWNERSHIP_VIOLATION
 *   ⑨ 确定性：同场景两次渲染逐位一致；scene_render 与立即模式逐位一致（§10.2-1）
 *  ⑩ capture→replay 与原帧逐位一致；CAP 跨 device（两个 Soft device）一致
 * =========================================================================*/
#include "neox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ 最小断言框架 */
static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

#define CHECK_PX(buf, w, x, y, er, eg, eb, ea) do { \
    const uint8_t *pp_ = &((buf))[((size_t)(y) * (size_t)(w) + (size_t)(x)) * 4]; \
    g_checks++; \
    if (pp_[0] != (er) || pp_[1] != (eg) || pp_[2] != (eb) || pp_[3] != (ea)) { \
        fprintf(stderr, "FAIL %s:%d: px(%d,%d) = (%u,%u,%u,%u), expect (%d,%d,%d,%d)\n", \
                __FILE__, __LINE__, (x), (y), pp_[0], pp_[1], pp_[2], pp_[3], \
                (er), (eg), (eb), (ea)); \
        g_failures++; \
    } \
} while (0)

#define CHECK_RES(expr, code) do { \
    neox_result r_ = (expr); \
    g_checks++; \
    if (r_ != (code)) { \
        fprintf(stderr, "FAIL %s:%d: %s = %d, expect %d\n", \
                __FILE__, __LINE__, #expr, (int)r_, (int)(code)); \
        g_failures++; \
    } \
} while (0)

/* ------------------------------------------------------------------ 测试分配器（budget 计数 + fail_next） */
typedef struct {
    uint64_t live;      /* 未释放字节 */
    uint64_t peak;      /* 历史峰值 */
    uint64_t allocs;    /* 累计分配次数 */
    uint64_t outstanding; /* 未释放块数 */
    uint64_t budget;    /* 0 = 不限 */
    int      fail_next; /* 下一次分配失败 */
    /* 存活块登记表（调试泄漏） */
    void  *ptrs[4096];
    size_t sizes[4096];
    uint32_t n;
} alloc_stat;

static void *t_alloc(void *user, size_t size, size_t align)
{
    alloc_stat *s = (alloc_stat *)user;
    void *p;
    (void)align;
    if (s->fail_next) { s->fail_next = 0; return NULL; }
    if (s->budget && s->live + size > s->budget) return NULL;
    p = malloc(size);
    if (!p) return NULL;
    s->live += size;
    if (s->live > s->peak) s->peak = s->live;
    s->allocs++;
    s->outstanding++;
    if (s->n < 4096) { s->ptrs[s->n] = p; s->sizes[s->n] = size; s->n++; }
    return p;
}

static void t_free(void *user, void *ptr, size_t size, size_t align)
{
    alloc_stat *s = (alloc_stat *)user;
    (void)align;
    if (!ptr) return;
    {
        uint32_t i;
        for (i = 0; i < s->n; i++)
            if (s->ptrs[i] == ptr) { s->ptrs[i] = s->ptrs[s->n - 1]; s->sizes[i] = s->sizes[s->n - 1]; s->n--; goto removed; }
        fprintf(stderr, "WARN: free of untracked ptr %p\n", ptr);
removed:;
    }
    s->live -= size;
    s->outstanding--;
    free(ptr);
}

/* ------------------------------------------------------------------ 公共夹具 */
static alloc_stat g_stat;
static neox_context *g_ctx;
static neox_device *g_dev;

static const neox_color BLACK = { 0, 0, 0, 1 };
static const neox_color WHITE = { 1, 1, 1, 1 };
static const neox_color RED   = { 1, 0, 0, 1 };
static const neox_color GREEN = { 0, 1, 0, 1 };
static const neox_color BLUE  = { 0, 0, 1, 1 };

static void fill_bytes(uint8_t *buf, size_t n, uint8_t v) { memset(buf, v, n); }

static neox_result begin_frame(neox_frame **fr, uint8_t *pixels, uint32_t w,
                               uint32_t h, const neox_color *clear)
{
    neox_target_desc t;
    memset(&t, 0, sizeof(t));
    t.kind = NEOX_BACKEND_SOFT;
    t.soft.pixels = pixels;
    t.soft.stride = (size_t)w * 4;
    t.soft.width = w;
    t.soft.height = h;
    t.clear = clear;
    return neox_frame_begin(g_dev, &t, fr);
}

static void end_and_pump(neox_frame *fr)
{
    neox_submission sub;
    if (neox_frame_end(fr, &sub) != NEOX_OK) return;
    neox_device_pump(g_dev);
}

/* 矩形路径（像素坐标） */
static neox_path make_rect(float x, float y, float w, float h, neox_fill_rule rule)
{
    static const uint8_t verbs[] = {
        NEOX_VERB_MOVE, NEOX_VERB_LINE, NEOX_VERB_LINE, NEOX_VERB_LINE, NEOX_VERB_CLOSE
    };
    float pts[8] = { x, y, x + w, y, x + w, y + h, x, y + h };
    neox_path_desc d;
    neox_path p = { 0 };
    memset(&d, 0, sizeof(d));
    d.verbs = verbs;
    d.points = pts;
    d.verb_count = 5;
    d.point_count = 4;
    d.fill_rule = rule;
    if (neox_path_create(g_dev, &d, &p) != NEOX_OK) {
        fprintf(stderr, "make_rect failed\n");
        exit(2);
    }
    return p;
}

/* ------------------------------------------------------------------ ① 填充规则 */
static void test_fill_rules(void)
{
    uint8_t buf[16 * 16 * 4];
    static const uint8_t verbs[] = {
        NEOX_VERB_MOVE, NEOX_VERB_LINE, NEOX_VERB_LINE, NEOX_VERB_LINE, NEOX_VERB_CLOSE,
        NEOX_VERB_MOVE, NEOX_VERB_LINE, NEOX_VERB_LINE, NEOX_VERB_LINE, NEOX_VERB_CLOSE
    };
    float pts[16] = { 1, 1, 6, 1, 6, 6, 1, 6,   3, 3, 8, 3, 8, 8, 3, 8 };
    neox_path_desc d;
    neox_path pz, pe;
    neox_frame *fr;
    neox_paint pt;
    memset(&d, 0, sizeof(d));
    d.verbs = verbs; d.points = pts; d.verb_count = 10; d.point_count = 8;
    d.fill_rule = NEOX_FILL_NONZERO;
    CHECK_RES(neox_path_create(g_dev, &d, &pz), NEOX_OK);
    d.fill_rule = NEOX_FILL_EVENODD;
    CHECK_RES(neox_path_create(g_dev, &d, &pe), NEOX_OK);

    pt.color = BLACK;
    /* NONZERO：重叠区环绕数 2 → 填充 */
    CHECK_RES(begin_frame(&fr, buf, 16, 16, &WHITE), NEOX_OK);
    CHECK_RES(neox_draw_path(fr, pz, &pt), NEOX_OK);
    end_and_pump(fr);
    CHECK_PX(buf, 16, 2, 2, 0, 0, 0, 255);   /* 仅第一环 */
    CHECK_PX(buf, 16, 7, 7, 0, 0, 0, 255);   /* 仅第二环 */
    CHECK_PX(buf, 16, 4, 4, 0, 0, 0, 255);   /* 重叠区（winding=2） */
    CHECK_PX(buf, 16, 0, 0, 255, 255, 255, 255);

    /* EVENODD：重叠区环绕数 2（偶）→ 镂空 */
    CHECK_RES(begin_frame(&fr, buf, 16, 16, &WHITE), NEOX_OK);
    CHECK_RES(neox_draw_path(fr, pe, &pt), NEOX_OK);
    end_and_pump(fr);
    CHECK_PX(buf, 16, 2, 2, 0, 0, 0, 255);
    CHECK_PX(buf, 16, 7, 7, 0, 0, 0, 255);
    CHECK_PX(buf, 16, 4, 4, 255, 255, 255, 255); /* 差异点：规则差异是语义的一部分（§4.2） */

    neox_path_release(g_dev, pz);
    neox_path_release(g_dev, pe);
    neox_device_pump(g_dev);
}

/* ------------------------------------------------------------------ ② Porter-Duff 精确数值（§6）
 * dst = 不透明红（预乘线性 1,0,0,1）；src = sRGB 蓝 a=0.5（预乘线性 0,0,0.5,0.5）。
 * 输出端：非预乘 + 线性→sRGB 量化（§5、§9 ties-to-even）。
 * srgb(0.5)≈0.7354→188；alpha 0.5→127.5 ties-to-even→128。 */
static void test_porter_duff(void)
{
    static const struct {
        neox_blend mode;
        uint8_t r, g, b, a;
    } expect[13] = {
        { NEOX_BLEND_SRC_OVER,  188, 0, 188, 255 },
        { NEOX_BLEND_CLEAR,       0, 0,   0,   0 },
        { NEOX_BLEND_SRC,         0, 0, 255, 128 },
        { NEOX_BLEND_DST,       255, 0,   0, 255 },
        { NEOX_BLEND_DST_OVER,  255, 0,   0, 255 },
        { NEOX_BLEND_SRC_IN,      0, 0, 255, 128 },
        { NEOX_BLEND_DST_IN,    255, 0,   0, 128 },
        { NEOX_BLEND_SRC_OUT,     0, 0,   0,   0 },
        { NEOX_BLEND_DST_OUT,   255, 0,   0, 128 },
        { NEOX_BLEND_SRC_ATOP,  188, 0, 188, 255 },
        { NEOX_BLEND_DST_ATOP,  255, 0,   0, 128 },
        { NEOX_BLEND_XOR,       255, 0,   0, 128 },
        { NEOX_BLEND_PLUS,      255, 0, 188, 255 }
    };
    uint8_t buf[8 * 8 * 4];
    neox_path rect = make_rect(0, 0, 8, 8, NEOX_FILL_NONZERO);
    neox_paint pt;
    uint32_t k;
    pt.color = BLUE;
    pt.color.a = 0.5f;
    for (k = 0; k < 13; k++) {
        neox_frame *fr;
        neox_state st;
        CHECK_RES(begin_frame(&fr, buf, 8, 8, &RED), NEOX_OK);
        memset(&st, 0, sizeof(st));
        st.transform = (neox_affine){ 1, 0, 0, 1, 0, 0 };
        st.opacity = 1.0f;
        st.blend = expect[k].mode;
        CHECK_RES(neox_push_state(fr, &st), NEOX_OK);
        CHECK_RES(neox_draw_path(fr, rect, &pt), NEOX_OK);
        CHECK_RES(neox_pop_state(fr), NEOX_OK);
        end_and_pump(fr);
        CHECK_PX(buf, 8, 3, 3, (int)expect[k].r, (int)expect[k].g,
                 (int)expect[k].b, (int)expect[k].a);
        CHECK_PX(buf, 8, 6, 6, (int)expect[k].r, (int)expect[k].g,
                 (int)expect[k].b, (int)expect[k].a);
    }
    neox_path_release(g_dev, rect);
    neox_device_pump(g_dev);
}

/* ------------------------------------------------------------------ ③ 组不透明度（§7） */
static void test_group_opacity(void)
{
    uint8_t buf[16 * 16 * 4];
    neox_path r1 = make_rect(0, 0, 8, 8, NEOX_FILL_NONZERO);
    neox_path r2 = make_rect(4, 4, 8, 8, NEOX_FILL_NONZERO);
    neox_frame *fr;
    neox_state st;
    neox_paint pr, pg;

    /* A：组 opacity=0.5，子元素不透明。组 = 隔离合成：先合成再整体乘 0.5。
     * 重叠区 (6,6)：层内绿色完全覆盖 → (0,1,0,1)*0.5 = (0,0.5,0,0.5)，
     * SRC_OVER 到不透明黑底 → (0,0.5,0,1) → (0,188,0,255) */
    pr.color = RED;
    pg.color = GREEN;
    CHECK_RES(begin_frame(&fr, buf, 16, 16, &BLACK), NEOX_OK);
    memset(&st, 0, sizeof(st));
    st.transform = (neox_affine){ 1, 0, 0, 1, 0, 0 };
    st.opacity = 0.5f;
    CHECK_RES(neox_push_state(fr, &st), NEOX_OK);
    CHECK_RES(neox_draw_path(fr, r1, &pr), NEOX_OK);
    CHECK_RES(neox_draw_path(fr, r2, &pg), NEOX_OK);
    CHECK_RES(neox_pop_state(fr), NEOX_OK);
    end_and_pump(fr);
    CHECK_PX(buf, 16, 6, 6, 0, 188, 0, 255);
    CHECK_PX(buf, 16, 2, 2, 188, 0, 0, 255); /* 非重叠区 = 红*0.5 合成到不透明底 */

    /* B：各自 opacity=0.5（paint alpha=0.5，无组）。
     * 重叠区：先 (0.5,0,0,0.5)，再绿 (0,0.5,0,0.5) SRC_OVER → (0.25,0.5,0,1)
     * → 非预乘 (0.25,0.5) → srgb(0.25)≈137, srgb(0.5)≈188 */
    pr.color = RED;   pr.color.a = 0.5f;
    pg.color = GREEN; pg.color.a = 0.5f;
    CHECK_RES(begin_frame(&fr, buf, 16, 16, &BLACK), NEOX_OK);
    CHECK_RES(neox_draw_path(fr, r1, &pr), NEOX_OK);
    CHECK_RES(neox_draw_path(fr, r2, &pg), NEOX_OK);
    end_and_pump(fr);
    CHECK_PX(buf, 16, 6, 6, 137, 188, 0, 255);

    /* 二者语义不同（重叠区双重透明 vs 组整体透明），规范明确取组语义（§7）；
     * 个体模式重叠区 g=188、r=137；组模式 g=188、r=0 */
    CHECK(buf[((6 * 16 + 6) * 4) + 0] == 137); /* 个体模式 r=137（B 结果仍在 buf 中） */
    {
        /* 复现组模式：等于"先合成再整体 0.5"的期望值（可重现、确定性） */
        CHECK_RES(begin_frame(&fr, buf, 16, 16, &BLACK), NEOX_OK);
        memset(&st, 0, sizeof(st));
        st.transform = (neox_affine){ 1, 0, 0, 1, 0, 0 };
        st.opacity = 0.5f;
        CHECK_RES(neox_push_state(fr, &st), NEOX_OK);
        pr.color = RED; pg.color = GREEN;
        CHECK_RES(neox_draw_path(fr, r1, &pr), NEOX_OK);
        CHECK_RES(neox_draw_path(fr, r2, &pg), NEOX_OK);
        CHECK_RES(neox_pop_state(fr), NEOX_OK);
        end_and_pump(fr);
        CHECK_PX(buf, 16, 6, 6, 0, 188, 0, 255);
    }
    neox_path_release(g_dev, r1);
    neox_path_release(g_dev, r2);
    neox_device_pump(g_dev);
}

/* ------------------------------------------------------------------ ④ 嵌套裁剪交集（§7） */
static void test_nested_clip(void)
{
    uint8_t buf[16 * 16 * 4];
    neox_path full = make_rect(0, 0, 16, 16, NEOX_FILL_NONZERO);
    neox_path c1 = make_rect(2, 2, 8, 8, NEOX_FILL_NONZERO);   /* 覆盖 2..10 */
    neox_path c2 = make_rect(6, 6, 8, 8, NEOX_FILL_NONZERO);   /* 覆盖 6..14 */
    neox_frame *fr;
    neox_state st;
    neox_paint pt;
    pt.color = BLACK;
    CHECK_RES(begin_frame(&fr, buf, 16, 16, &WHITE), NEOX_OK);
    memset(&st, 0, sizeof(st));
    st.transform = (neox_affine){ 1, 0, 0, 1, 0, 0 };
    st.opacity = 1.0f;
    st.clip = &c1;
    CHECK_RES(neox_push_state(fr, &st), NEOX_OK);
    st.clip = &c2;
    st.opacity = 1.0f;
    CHECK_RES(neox_push_state(fr, &st), NEOX_OK);
    CHECK_RES(neox_draw_path(fr, full, &pt), NEOX_OK);
    CHECK_RES(neox_pop_state(fr), NEOX_OK);
    CHECK_RES(neox_pop_state(fr), NEOX_OK);
    end_and_pump(fr);
    CHECK_PX(buf, 16, 8, 8, 0, 0, 0, 255);       /* 交集 6..10 内 */
    CHECK_PX(buf, 16, 4, 4, 255, 255, 255, 255); /* 仅 clip1：不可见 */
    CHECK_PX(buf, 16, 12, 12, 255, 255, 255, 255); /* 仅 clip2：不可见 */
    CHECK_PX(buf, 16, 0, 0, 255, 255, 255, 255); /* 裁剪外 */
    neox_path_release(g_dev, full);
    neox_path_release(g_dev, c1);
    neox_path_release(g_dev, c2);
    neox_device_pump(g_dev);
}

/* ------------------------------------------------------------------ ⑤ 纹理采样（§8） */
static neox_texture make_2x2(void)
{
    /* sRGB：红 绿 / 蓝 白 */
    const uint8_t px[2 * 2 * 4] = {
        255, 0, 0, 255,    0, 255, 0, 255,
        0, 0, 255, 255,    255, 255, 255, 255
    };
    neox_texture_desc d;
    neox_texture t = { 0 };
    memset(&d, 0, sizeof(d));
    d.width = 2; d.height = 2;
    d.format = NEOX_FORMAT_RGBA8_SRGB;
    d.pixels = px;
    d.row_stride = 8;
    if (neox_texture_create(g_dev, &d, &t) != NEOX_OK) {
        fprintf(stderr, "make_2x2 failed\n");
        exit(2);
    }
    return t;
}

static void test_texture_sampling(void)
{
    uint8_t buf[8 * 8 * 4];
    neox_texture tex = make_2x2();
    neox_frame *fr;
    neox_rect src = { 0, 0, 2, 2 };

    /* NEAREST 放大到 4x4：列 0-1 = 红/蓝列（texel 0），列 2-3 = 绿/白列（texel 1） */
    CHECK_RES(begin_frame(&fr, buf, 8, 8, &BLACK), NEOX_OK);
    CHECK_RES(neox_draw_texture(fr, tex, src, (neox_rect){ 0, 0, 4, 4 },
                                NEOX_FILTER_NEAREST, 1.0f), NEOX_OK);
    end_and_pump(fr);
    CHECK_PX(buf, 8, 0, 0, 255, 0, 0, 255);
    CHECK_PX(buf, 8, 1, 0, 255, 0, 0, 255);
    CHECK_PX(buf, 8, 2, 0, 0, 255, 0, 255);
    CHECK_PX(buf, 8, 0, 2, 0, 0, 255, 255);
    CHECK_PX(buf, 8, 3, 3, 255, 255, 255, 255);

    /* LINEAR 放大到 4x4：像素中心映射 u = 0.25/0.75/1.25/1.75。
     * texel 中心位于 +0.5：u=0.75 → 0.75*红 + 0.25*绿 = (0.75,0.25,0)；
     * u=1.25 → (0.25,0.75,0)；u=0.25/1.75 恰好落在 texel 中心（CLAMP）。 */
    CHECK_RES(begin_frame(&fr, buf, 8, 8, &BLACK), NEOX_OK);
    CHECK_RES(neox_draw_texture(fr, tex, src, (neox_rect){ 0, 0, 4, 4 },
                                NEOX_FILTER_LINEAR, 1.0f), NEOX_OK);
    end_and_pump(fr);
    CHECK_PX(buf, 8, 0, 0, 255, 0, 0, 255);   /* u=0.25 → texel0 中心 */
    CHECK_PX(buf, 8, 1, 0, 225, 137, 0, 255); /* u=0.75 → 0.75红+0.25绿 */
    CHECK_PX(buf, 8, 2, 0, 137, 225, 0, 255); /* u=1.25 → 0.25红+0.75绿 */
    CHECK_PX(buf, 8, 3, 0, 0, 255, 0, 255);   /* u=1.75 → texel1 中心 */
    /* 双轴混合（px=1,py=1：fx=0.25, fy=0.25 → r=0.625, g=0.25, b=0.25） */
    CHECK_PX(buf, 8, 1, 1, 207, 137, 137, 255);

    /* 负宽目标矩形 → 水平镜像（§8） */
    CHECK_RES(begin_frame(&fr, buf, 8, 8, &BLACK), NEOX_OK);
    CHECK_RES(neox_draw_texture(fr, tex, src, (neox_rect){ 4, 0, -4, 4 },
                                NEOX_FILTER_NEAREST, 1.0f), NEOX_OK);
    end_and_pump(fr);
    CHECK_PX(buf, 8, 0, 0, 0, 255, 0, 255);   /* 镜像：左侧采样原右侧 texel */
    CHECK_PX(buf, 8, 3, 0, 255, 0, 0, 255);
    CHECK_PX(buf, 8, 0, 2, 255, 255, 255, 255);
    CHECK_PX(buf, 8, 3, 2, 0, 0, 255, 255);

    neox_texture_release(g_dev, tex);
    neox_device_pump(g_dev);
}

/* ------------------------------------------------------------------ ⑥ A8 字形图集（§5） */
static void test_glyphs(void)
{
    uint8_t buf[4 * 4 * 4];
    const uint8_t atlas_px[2] = { 255, 0 };
    neox_texture_desc d;
    neox_texture atlas = { 0 };
    neox_glyph gl;
    neox_paint pt;
    neox_frame *fr;
    neox_color transparent = { 0, 0, 0, 0 };
    memset(&d, 0, sizeof(d));
    d.width = 2; d.height = 1;
    d.format = NEOX_FORMAT_A8;
    d.pixels = atlas_px;
    d.row_stride = 2;
    CHECK_RES(neox_texture_create(g_dev, &d, &atlas), NEOX_OK);
    memset(&gl, 0, sizeof(gl));
    /* 字形左偏移 0.5：像素 0 覆盖 texel0 中心（alpha=1），
     * 像素 1 落在 texel0/texel1 之间（alpha=0.5），像素 2 落在 texel1 外（alpha=0） */
    gl.x = 0.5f; gl.y = 0; gl.sx = 0; gl.sy = 0; gl.sw = 2; gl.sh = 1;
    pt.color = RED;
    CHECK_RES(begin_frame(&fr, buf, 4, 4, &transparent), NEOX_OK);
    CHECK_RES(neox_draw_glyphs(fr, atlas, &gl, 1, &pt), NEOX_OK);
    end_and_pump(fr);
    CHECK_PX(buf, 4, 0, 0, 255, 0, 0, 255); /* A8 采样值 1.0 → paint 全色 */
    CHECK_PX(buf, 4, 1, 0, 255, 0, 0, 128); /* 0.5 → 127.5 ties-to-even */
    CHECK_PX(buf, 4, 2, 0, 0, 0, 0, 0);     /* 0 → 不变（透明底） */
    neox_texture_release(g_dev, atlas);
    neox_device_pump(g_dev);
}


/* ------------------------------------------------------------------ ⑪ §6.1 覆盖率插值（CLEAR 抗锯齿擦除 / DST 无变化）
 * 矩形 (2.5,2,4,4)：像素 (2,3) 水平覆盖率 0.5、垂直整行 → cov=0.5。
 * CLEAR：out = 0.5·0 + 0.5·红 = 非预乘 (1,0,0)，a=0.5 → (255,0,0,128)（抗锯齿擦除）；
 * DST：out = dst（任何覆盖率下无视觉变化）。全覆盖像素行为与 §6 一致。 */
static void test_blend_coverage_interpolation(void)
{
    uint8_t buf[8 * 8 * 4];
    neox_path rect = make_rect(2.5f, 2.0f, 4.0f, 4.0f, NEOX_FILL_NONZERO);
    neox_frame *fr;
    neox_state st;
    neox_paint pt;

    /* CLEAR：半覆盖像素 = dst·(1−cov)，非整像素清零（§6.1） */
    CHECK_RES(begin_frame(&fr, buf, 8, 8, &RED), NEOX_OK);
    memset(&st, 0, sizeof(st));
    st.transform = (neox_affine){ 1, 0, 0, 1, 0, 0 };
    st.opacity = 1.0f;
    st.blend = NEOX_BLEND_CLEAR;
    CHECK_RES(neox_push_state(fr, &st), NEOX_OK);
    pt.color = BLUE;
    CHECK_RES(neox_draw_path(fr, rect, &pt), NEOX_OK);
    CHECK_RES(neox_pop_state(fr), NEOX_OK);
    end_and_pump(fr);
    CHECK_PX(buf, 8, 2, 3, 255, 0, 0, 128);   /* cov=0.5：dst·0.5 */
    CHECK_PX(buf, 8, 6, 3, 255, 0, 0, 128);   /* 右缘同 */
    CHECK_PX(buf, 8, 4, 3, 0, 0, 0, 0);       /* cov=1：清零 */
    CHECK_PX(buf, 8, 1, 3, 255, 0, 0, 255);   /* cov=0：不变 */

    /* DST：半覆盖像素也无变化（§6.1 推论） */
    CHECK_RES(begin_frame(&fr, buf, 8, 8, &RED), NEOX_OK);
    memset(&st, 0, sizeof(st));
    st.transform = (neox_affine){ 1, 0, 0, 1, 0, 0 };
    st.opacity = 1.0f;
    st.blend = NEOX_BLEND_DST;
    CHECK_RES(neox_push_state(fr, &st), NEOX_OK);
    CHECK_RES(neox_draw_path(fr, rect, &pt), NEOX_OK);
    CHECK_RES(neox_pop_state(fr), NEOX_OK);
    end_and_pump(fr);
    CHECK_PX(buf, 8, 2, 3, 255, 0, 0, 255);   /* cov=0.5：无变化 */
    CHECK_PX(buf, 8, 4, 3, 255, 0, 0, 255);   /* cov=1：无变化 */

    neox_path_release(g_dev, rect);
    neox_device_pump(g_dev);
}

/* ------------------------------------------------------------------ ⑫ platform_size 配对校验（neox_device_desc 约定） */
static void test_platform_size_contract(void)
{
    neox_device_desc dd;
    neox_device *dev = NULL;
    int dummy = 0;

    /* SOFT：platform 为 NULL 时 platform_size 必须为 0 */
    memset(&dd, 0, sizeof(dd));
    dd.backend = NEOX_BACKEND_SOFT;
    dd.platform_size = 8;
    CHECK_RES(neox_device_create(g_ctx, &dd, &dev), NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(dev == NULL);
    /* platform 非 NULL 时 platform_size 必须非 0 */
    memset(&dd, 0, sizeof(dd));
    dd.backend = NEOX_BACKEND_SOFT;
    dd.platform = &dummy;
    dd.platform_size = 0;
    CHECK_RES(neox_device_create(g_ctx, &dd, &dev), NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(dev == NULL);
}

/* ------------------------------------------------------------------ ⑦ 失败语义（§10.1） */
static void test_failure_semantics(void)
{
    alloc_stat st;
    neox_context_desc cd;
    neox_context *ctx2;
    neox_device *dev2;
    neox_frame *fr;
    uint8_t target[64 * 64 * 4];
    neox_submission sub;

    /* (a) budget 极小：context 创建成功（结构较小），device_create 即失败 */
    memset(&st, 0, sizeof(st));
    memset(&cd, 0, sizeof(cd));
    {   /* allocator 需在 context_create 调用期间有效（[borrow]） */
        static neox_allocator a;
        a.user = &st; a.alloc = t_alloc; a.free = t_free;
        cd.allocator = &a;
    }
    cd.memory_budget = 512; /* context 结构 + 少量余量，不够设备表 */
    if (neox_context_create(&cd, &ctx2) == NEOX_OK) {
        neox_device_desc dd;
        memset(&dd, 0, sizeof(dd));
        dd.backend = NEOX_BACKEND_SOFT;
        CHECK_RES(neox_device_create(ctx2, &dd, &dev2), NEOX_ERROR_OUT_OF_MEMORY);
        neox_context_destroy(ctx2);
    } /* 若 context 本身也失败（实现尺寸变化），同样满足"create 失败"语义 */

    /* (b) budget 中等：设备创建成功，frame_begin（64x64 需 16KB+ 基底）超预算失败，
     *     目标像素未被改动 */
    memset(&st, 0, sizeof(st));
    cd.memory_budget = 60000; /* 足够设备表，不够 64x64 基底缓冲(64KB) */
    CHECK_RES(neox_context_create(&cd, &ctx2), NEOX_OK);
    {
        neox_device_desc dd;
        memset(&dd, 0, sizeof(dd));
        dd.backend = NEOX_BACKEND_SOFT;
        CHECK_RES(neox_device_create(ctx2, &dd, &dev2), NEOX_OK);
        fill_bytes(target, sizeof(target), 0xAB);
        {
            neox_target_desc td;
            neox_frame *fr2;
            memset(&td, 0, sizeof(td));
            td.kind = NEOX_BACKEND_SOFT;
            td.soft.pixels = target;
            td.soft.stride = 64 * 4;
            td.soft.width = 64;
            td.soft.height = 64;
            td.clear = &WHITE;
            CHECK_RES(neox_frame_begin(dev2, &td, &fr2), NEOX_ERROR_OUT_OF_MEMORY);
        }
        CHECK(target[0] == 0xAB && target[100] == 0xAB && target[1000] == 0xAB);
        neox_context_destroy(ctx2);
    }

    /* (c) fail_next：frame_end 失败是原子的，帧保持可重试，目标不变 */
    neox_device *saved_dev = g_dev;
    CHECK_RES(neox_device_create(g_ctx, NULL, &g_dev), NEOX_OK);
    fill_bytes(target, sizeof(target), 0xCD);
    CHECK_RES(begin_frame(&fr, target, 64, 64, &WHITE), NEOX_OK);
    {
        neox_paint pt;
        neox_path rect = make_rect(4, 4, 32, 32, NEOX_FILL_NONZERO);
        pt.color = BLUE;
        CHECK_RES(neox_draw_path(fr, rect, &pt), NEOX_OK);
        g_stat.fail_next = 1;
        CHECK_RES(neox_frame_end(fr, &sub), NEOX_ERROR_OUT_OF_MEMORY);
        CHECK(sub.id == 0);
        CHECK(target[0] == 0xCD && target[500] == 0xCD); /* 目标未被触碰 */
        neox_path_release(g_dev, rect);
        /* 重试成功：frame 未被销毁 */
        CHECK_RES(neox_frame_end(fr, &sub), NEOX_OK);
        neox_device_pump(g_dev);
        CHECK_PX(target, 64, 8, 8, 0, 0, 255, 255);   /* 矩形内 = 蓝 */
        CHECK_PX(target, 64, 0, 0, 255, 255, 255, 255); /* 清屏色保留 */
        CHECK(neox_submission_get_status(g_dev, sub) == NEOX_SUBMISSION_COMPLETE);
    }

    neox_device_destroy(g_dev);
    g_dev = saved_dev;

    /* (d) 执行期失败（pump 内分配失败）：submission FAILED，目标不变 */
    fill_bytes(target, sizeof(target), 0x77);
    CHECK_RES(begin_frame(&fr, target, 64, 64, &WHITE), NEOX_OK);
    {
        neox_paint pt;
        neox_path rect = make_rect(4, 4, 32, 32, NEOX_FILL_NONZERO);
        pt.color = BLUE;
        CHECK_RES(neox_draw_path(fr, rect, &pt), NEOX_OK);
        CHECK_RES(neox_frame_end(fr, &sub), NEOX_OK);
        g_stat.fail_next = 1; /* 执行期第一个分配（执行栈）失败 */
        neox_device_pump(g_dev);
        CHECK(neox_submission_get_status(g_dev, sub) == NEOX_SUBMISSION_FAILED);
        CHECK(target[0] == 0x77 && target[500] == 0x77);
        neox_path_release(g_dev, rect);
        neox_device_pump(g_dev);
    }
}

/* ------------------------------------------------------------------ ⑧ 句柄错误（草案契约） */
static void test_handles(void)
{
    neox_texture tex = make_2x2();
    neox_texture stale = tex;
    neox_device_desc dd;
    neox_device *dev2;
    neox_frame *fr;
    uint8_t buf[8 * 8 * 4];
    neox_rect region = { 0, 0, 1, 1 };
    const uint8_t px[4] = { 1, 2, 3, 4 };

    memset(&dd, 0, sizeof(dd));
    dd.backend = NEOX_BACKEND_SOFT;
    CHECK_RES(neox_device_create(g_ctx, &dd, &dev2), NEOX_OK);

    /* 类型误用：texture 句柄当 path 用 → OWNERSHIP_VIOLATION */
    {
        neox_path as_path = { tex.id };
        neox_paint pt;
        pt.color = RED;
        CHECK_RES(begin_frame(&fr, buf, 8, 8, &BLACK), NEOX_OK);
        CHECK_RES(neox_draw_path(fr, as_path, &pt), NEOX_ERROR_OWNERSHIP_VIOLATION);
        neox_frame_end(fr, &(neox_submission){ 0 });
        neox_device_pump(g_dev);
    }
    /* 跨 device 使用 → OWNERSHIP_VIOLATION */
    CHECK_RES(neox_texture_update(dev2, tex, region, px, 4),
              NEOX_ERROR_OWNERSHIP_VIOLATION);
    /* 合法更新后再释放；释放后立即使用 → STALE_HANDLE */
    CHECK_RES(neox_texture_update(g_dev, tex, region, px, 4), NEOX_OK);
    neox_texture_release(g_dev, tex);
    CHECK_RES(neox_texture_update(g_dev, stale, region, px, 4),
              NEOX_ERROR_STALE_HANDLE);
    neox_device_pump(g_dev);
    /* 过期句柄在另一 device 上：dev 标识不匹配优先 → OWNERSHIP_VIOLATION */
    CHECK_RES(neox_texture_update(dev2, stale, region, px, 4),
              NEOX_ERROR_OWNERSHIP_VIOLATION);
    neox_device_destroy(dev2);
}

/* ------------------------------------------------------------------ ⑨ 确定性与立即/保留等价（§9、§10.2-1） */
static neox_path g_star;      /* EVENODD 自相交星形 */
static neox_path g_blob;      /* 贝塞尔填充形 */
static neox_texture g_tex;    /* 2x2 */
static neox_texture g_atlas;  /* A8 */
static neox_path g_clip;      /* 裁剪圆（多边形近似） */

static void build_scene_resources(void)
{
    uint32_t i;
    float pts[2 * 11];
    uint8_t verbs[11];
    neox_path_desc d;
    float cpts[20];
    uint8_t cverbs[5];
    const uint8_t apx[4] = { 200, 255, 128, 64 };
    neox_texture_desc td;

    /* 星形（5 顶点 + 回中心，自相交），EVENODD */
    for (i = 0; i < 5; i++) {
        float ang = (float)i * 4.0f * 3.14159265f / 5.0f;
        pts[i * 4 + 0] = 20.0f + 16.0f * cosf(ang);
        pts[i * 4 + 1] = 20.0f + 16.0f * sinf(ang);
        pts[i * 4 + 2] = 20.0f + 7.0f * cosf(ang + 0.62831853f);
        pts[i * 4 + 3] = 20.0f + 7.0f * sinf(ang + 0.62831853f);
        verbs[i * 2] = NEOX_VERB_LINE;
        verbs[i * 2 + 1] = NEOX_VERB_LINE;
    }
    verbs[0] = NEOX_VERB_MOVE;
    verbs[10] = NEOX_VERB_CLOSE;
    memset(&d, 0, sizeof(d));
    d.verbs = verbs; d.points = pts; d.verb_count = 11; d.point_count = 10;
    d.fill_rule = NEOX_FILL_EVENODD;
    if (neox_path_create(g_dev, &d, &g_star) != NEOX_OK) exit(2);

    /* 三次贝塞尔 blob，NONZERO */
    cverbs[0] = NEOX_VERB_MOVE;
    cverbs[1] = NEOX_VERB_CUBIC;
    cverbs[2] = NEOX_VERB_CUBIC;
    cverbs[3] = NEOX_VERB_CUBIC;
    cverbs[4] = NEOX_VERB_CLOSE;
    {
        static const float k[20] = {
            5, 30,   20, 5,   35, 25,   45, 10,
            55, 25,   55, 40,   45, 50,
            35, 55,   15, 55,   5, 30
        };
        memcpy(cpts, k, sizeof(k));
    }
    memset(&d, 0, sizeof(d));
    d.verbs = cverbs; d.points = cpts; d.verb_count = 5; d.point_count = 10;
    d.fill_rule = NEOX_FILL_NONZERO;
    if (neox_path_create(g_dev, &d, &g_blob) != NEOX_OK) exit(2);

    g_tex = make_2x2();
    memset(&td, 0, sizeof(td));
    td.width = 2; td.height = 2; td.format = NEOX_FORMAT_A8;
    td.pixels = apx; td.row_stride = 2;
    if (neox_texture_create(g_dev, &td, &g_atlas) != NEOX_OK) exit(2);

    /* 裁剪路径：八边形近似圆 */
    {
        float cp[16];
        uint8_t cv[9];
        for (i = 0; i < 8; i++) {
            float ang = (float)i * 3.14159265f / 4.0f;
            cp[i * 2] = 32.0f + 24.0f * cosf(ang);
            cp[i * 2 + 1] = 32.0f + 24.0f * sinf(ang);
            cv[i] = NEOX_VERB_LINE;
        }
        cv[0] = NEOX_VERB_MOVE;
        cv[8] = NEOX_VERB_CLOSE;
        memset(&d, 0, sizeof(d));
        d.verbs = cv; d.points = cp; d.verb_count = 9; d.point_count = 8;
        d.fill_rule = NEOX_FILL_NONZERO;
        if (neox_path_create(g_dev, &d, &g_clip) != NEOX_OK) exit(2);
    }
}

/* 立即模式绘制（与 scene_emit 的命令流逐一对应） */
static void draw_immediate(neox_frame *fr)
{
    neox_state st;
    neox_paint pt;
    neox_glyph gl;

    pt.color = (neox_color){ 1, 0.5f, 0, 0.8f };
    memset(&st, 0, sizeof(st));
    st.transform = (neox_affine){ 1, 0, 0, 1, 0, 0 };
    st.opacity = 0.6f;
    st.clip = &g_clip;
    CHECK_RES(neox_push_state(fr, &st), NEOX_OK);

    /* 组内：贝塞尔 blob（蓝，PLUS 混合） */
    pt.color = BLUE;
    memset(&st, 0, sizeof(st));
    st.transform = (neox_affine){ 1, 0, 0, 1, 3, 2 };
    st.opacity = 1.0f;
    st.blend = NEOX_BLEND_PLUS;
    CHECK_RES(neox_push_state(fr, &st), NEOX_OK);
    CHECK_RES(neox_draw_path(fr, g_blob, &pt), NEOX_OK);
    CHECK_RES(neox_pop_state(fr), NEOX_OK);

    /* 星形（EVENODD）+ 纹理 + 字形，各自带变换 */
    pt.color = (neox_color){ 1, 1, 0, 1 };
    memset(&st, 0, sizeof(st));
    st.transform = (neox_affine){ 1.5f, 0, 0, 1.5f, 30, 10 };
    st.opacity = 1.0f;
    CHECK_RES(neox_push_state(fr, &st), NEOX_OK);
    CHECK_RES(neox_draw_path(fr, g_star, &pt), NEOX_OK);
    CHECK_RES(neox_pop_state(fr), NEOX_OK);

    memset(&st, 0, sizeof(st));
    st.transform = (neox_affine){ 2, 0, 0, 2, 2, 40 };
    st.opacity = 1.0f;
    CHECK_RES(neox_push_state(fr, &st), NEOX_OK);
    CHECK_RES(neox_draw_texture(fr, g_tex, (neox_rect){ 0, 0, 2, 2 },
                                (neox_rect){ 0, 0, 8, 8 },
                                NEOX_FILTER_LINEAR, 1.0f), NEOX_OK);
    CHECK_RES(neox_pop_state(fr), NEOX_OK);

    memset(&gl, 0, sizeof(gl));
    gl.x = 40; gl.y = 40; gl.sx = 0; gl.sy = 0; gl.sw = 2; gl.sh = 2;
    pt.color = (neox_color){ 0, 1, 1, 0.7f };
    memset(&st, 0, sizeof(st));
    st.transform = (neox_affine){ 3, 0, 0, 3, 0, 0 };
    st.opacity = 1.0f;
    CHECK_RES(neox_push_state(fr, &st), NEOX_OK);
    CHECK_RES(neox_draw_glyphs(fr, g_atlas, &gl, 1, &pt), NEOX_OK);
    CHECK_RES(neox_pop_state(fr), NEOX_OK);

    CHECK_RES(neox_pop_state(fr), NEOX_OK); /* 组 */
}

/* 保留模式：相同内容的节点树 */
static void build_scene(neox_scene *out_scene)
{
    neox_scene sc = neox_scene_create(g_dev);
    neox_node group, n;
    neox_node_desc nd;
    neox_glyph gl;
    CHECK(sc.id != 0);

    /* 根组：opacity 0.6 + 裁剪（与立即模式最外层 push 对应） */
    memset(&nd, 0, sizeof(nd));
    nd.kind = NEOX_NODE_GROUP;
    nd.transform = (neox_affine){ 1, 0, 0, 1, 0, 0 };
    nd.opacity = 0.6f;
    nd.visible = 1;
    nd.group.clip = g_clip;
    CHECK_RES(neox_node_create(g_dev, sc, &nd, &group), NEOX_OK);

    memset(&nd, 0, sizeof(nd));
    nd.kind = NEOX_NODE_PATH;
    nd.transform = (neox_affine){ 1, 0, 0, 1, 3, 2 };
    nd.blend = NEOX_BLEND_PLUS;
    nd.opacity = 1.0f;
    nd.visible = 1;
    nd.path.path = g_blob;
    nd.path.paint.color = BLUE;
    CHECK_RES(neox_node_create(g_dev, sc, &nd, &n), NEOX_OK);
    CHECK_RES(neox_node_append_child(g_dev, group, n), NEOX_OK);

    memset(&nd, 0, sizeof(nd));
    nd.kind = NEOX_NODE_PATH;
    nd.transform = (neox_affine){ 1.5f, 0, 0, 1.5f, 30, 10 };
    nd.opacity = 1.0f;
    nd.visible = 1;
    nd.path.path = g_star;
    nd.path.paint.color = (neox_color){ 1, 1, 0, 1 };
    CHECK_RES(neox_node_create(g_dev, sc, &nd, &n), NEOX_OK);
    CHECK_RES(neox_node_append_child(g_dev, group, n), NEOX_OK);

    memset(&nd, 0, sizeof(nd));
    nd.kind = NEOX_NODE_IMAGE;
    nd.transform = (neox_affine){ 2, 0, 0, 2, 2, 40 };
    nd.opacity = 1.0f;
    nd.visible = 1;
    nd.image.tex = g_tex;
    nd.image.src = (neox_rect){ 0, 0, 2, 2 };
    nd.image.dst = (neox_rect){ 0, 0, 8, 8 };
    nd.image.filter = NEOX_FILTER_LINEAR;
    CHECK_RES(neox_node_create(g_dev, sc, &nd, &n), NEOX_OK);
    CHECK_RES(neox_node_append_child(g_dev, group, n), NEOX_OK);

    memset(&gl, 0, sizeof(gl));
    gl.x = 40; gl.y = 40; gl.sx = 0; gl.sy = 0; gl.sw = 2; gl.sh = 2;
    memset(&nd, 0, sizeof(nd));
    nd.kind = NEOX_NODE_GLYPHS;
    nd.transform = (neox_affine){ 3, 0, 0, 3, 0, 0 };
    nd.opacity = 1.0f;
    nd.visible = 1;
    nd.glyphs.atlas = g_atlas;
    nd.glyphs.glyphs = &gl;
    nd.glyphs.count = 1;
    nd.glyphs.paint.color = (neox_color){ 0, 1, 1, 0.7f };
    CHECK_RES(neox_node_create(g_dev, sc, &nd, &n), NEOX_OK);
    CHECK_RES(neox_node_append_child(g_dev, group, n), NEOX_OK);

    *out_scene = sc;
}

static void test_determinism_and_equivalence(void)
{
    uint8_t a[64 * 64 * 4], b[64 * 64 * 4], c[64 * 64 * 4];
    neox_scene sc;
    neox_frame *fr;
    neox_color clear = { 0.15f, 0.2f, 0.3f, 1.0f };

    build_scene_resources();
    build_scene(&sc);

    /* 保留模式两次（§9：同后端同输入逐位一致） */
    CHECK_RES(begin_frame(&fr, a, 64, 64, &clear), NEOX_OK);
    CHECK_RES(neox_scene_render(fr, sc, (neox_node){ 0 }), NEOX_OK);
    end_and_pump(fr);
    CHECK_RES(begin_frame(&fr, b, 64, 64, &clear), NEOX_OK);
    CHECK_RES(neox_scene_render(fr, sc, (neox_node){ 0 }), NEOX_OK);
    end_and_pump(fr);
    CHECK(memcmp(a, b, sizeof(a)) == 0);

    /* 立即模式等价（§10.2-1） */
    CHECK_RES(begin_frame(&fr, c, 64, 64, &clear), NEOX_OK);
    draw_immediate(fr);
    end_and_pump(fr);
    CHECK(memcmp(a, c, sizeof(a)) == 0);

    /* 定点修改后再渲染：不崩溃、确定性保持（属性修改接口接通） */
    {
        neox_node root_child;
        neox_node_desc nd;
        memset(&nd, 0, sizeof(nd));
        nd.kind = NEOX_NODE_PATH;
        nd.transform = (neox_affine){ 1, 0, 0, 1, 50, 4 };
        nd.opacity = 1.0f;
        nd.visible = 1;
        nd.path.path = g_blob;
        nd.path.paint.color = (neox_color){ 1, 0, 1, 0.5f };
        CHECK_RES(neox_node_create(g_dev, sc, &nd, &root_child), NEOX_OK);
        CHECK_RES(neox_node_set_opacity(g_dev, root_child, 0.5f), NEOX_OK);
        CHECK_RES(neox_node_set_visible(g_dev, root_child, 0), NEOX_OK);
        CHECK_RES(begin_frame(&fr, b, 64, 64, &clear), NEOX_OK);
        CHECK_RES(neox_scene_render(fr, sc, (neox_node){ 0 }), NEOX_OK);
        end_and_pump(fr);
        CHECK(memcmp(a, b, sizeof(a)) == 0); /* 不可见节点不影响输出 */
        CHECK_RES(neox_node_set_visible(g_dev, root_child, 1), NEOX_OK);
        CHECK_RES(neox_node_set_transform(g_dev, root_child,
                    (neox_affine){ 1, 0, 0, 1, 52, 4 }), NEOX_OK);
        CHECK_RES(begin_frame(&fr, b, 64, 64, &clear), NEOX_OK);
        CHECK_RES(neox_scene_render(fr, sc, (neox_node){ 0 }), NEOX_OK);
        end_and_pump(fr);
        CHECK(memcmp(a, b, sizeof(a)) != 0); /* 可见修改改变输出 */
        neox_node_release(g_dev, root_child);
    }

    neox_scene_destroy(g_dev, sc);
    neox_path_release(g_dev, g_star);
    neox_path_release(g_dev, g_blob);
    neox_path_release(g_dev, g_clip);
    neox_texture_release(g_dev, g_tex);
    neox_texture_release(g_dev, g_atlas);
    neox_device_pump(g_dev);
}

/* ------------------------------------------------------------------ ⑩ capture / replay */
static void test_capture_replay(void)
{
    uint8_t a[32 * 32 * 4], b[32 * 32 * 4], c[32 * 32 * 4];
    neox_capture *cap;
    uint8_t *data = NULL;
    size_t size = 0;
    neox_frame *fr;
    neox_submission sub;
    neox_color clear = { 0.2f, 0.4f, 0.6f, 1.0f };
    neox_device *dev2;
    neox_device_desc dd;
    neox_path rect;
    neox_texture tex;
    neox_glyph gl;
    neox_paint pt;
    neox_state st;

    build_scene_resources();
    rect = make_rect(2, 2, 20, 20, NEOX_FILL_NONZERO);
    tex = make_2x2();
    memset(&gl, 0, sizeof(gl));
    gl.x = 22; gl.y = 22; gl.sx = 0; gl.sy = 0; gl.sw = 2; gl.sh = 2;
    pt.color = (neox_color){ 1, 0, 0.5f, 0.75f };

    /* 录制帧 */
    CHECK_RES(neox_capture_begin(g_dev, &cap), NEOX_OK);
    CHECK_RES(begin_frame(&fr, a, 32, 32, &clear), NEOX_OK);
    memset(&st, 0, sizeof(st));
    st.transform = (neox_affine){ 1, 0, 0, 1, 0, 0 };
    st.opacity = 0.7f;
    CHECK_RES(neox_push_state(fr, &st), NEOX_OK);
    CHECK_RES(neox_draw_path(fr, rect, &pt), NEOX_OK);
    CHECK_RES(neox_draw_texture(fr, tex, (neox_rect){ 0, 0, 2, 2 },
                                (neox_rect){ 4, 4, 12, 12 },
                                NEOX_FILTER_LINEAR, 0.8f), NEOX_OK);
    CHECK_RES(neox_draw_glyphs(fr, g_atlas, &gl, 1, &pt), NEOX_OK);
    CHECK_RES(neox_pop_state(fr), NEOX_OK);
    CHECK_RES(neox_frame_end(fr, &sub), NEOX_OK);
    CHECK_RES(neox_capture_end(cap, &data, &size), NEOX_OK);
    CHECK(data != NULL && size > 16);
    CHECK(memcmp(data, "NXCA", 4) == 0);

    /* 原帧执行 → a */
    neox_device_pump(g_dev);

    /* 同 device 回放 → b，逐位一致 */
    CHECK_RES(begin_frame(&fr, b, 32, 32, &clear), NEOX_OK);
    CHECK_RES(neox_capture_replay(fr, data, size), NEOX_OK);
    CHECK_RES(neox_frame_end(fr, &sub), NEOX_OK);
    neox_device_pump(g_dev);
    CHECK(memcmp(a, b, sizeof(a)) == 0);

    /* 跨 device 回放 → c，逐位一致 */
    memset(&dd, 0, sizeof(dd));
    dd.backend = NEOX_BACKEND_SOFT;
    CHECK_RES(neox_device_create(g_ctx, &dd, &dev2), NEOX_OK);
    {
        neox_target_desc td;
        neox_frame *fr2;
        neox_submission sub2;
        memset(&td, 0, sizeof(td));
        td.kind = NEOX_BACKEND_SOFT;
        td.soft.pixels = c;
        td.soft.stride = 32 * 4;
        td.soft.width = 32;
        td.soft.height = 32;
        td.clear = &clear;
        CHECK_RES(neox_frame_begin(dev2, &td, &fr2), NEOX_OK);
        CHECK_RES(neox_capture_replay(fr2, data, size), NEOX_OK);
        CHECK_RES(neox_frame_end(fr2, &sub2), NEOX_OK);
        neox_device_pump(dev2);
        CHECK(neox_submission_get_status(dev2, sub2) == NEOX_SUBMISSION_COMPLETE);
    }
    CHECK(memcmp(a, c, sizeof(a)) == 0);
    neox_device_destroy(dev2);

    neox_capture_free_buffer(g_ctx, data, size);
    neox_path_release(g_dev, rect);
    neox_texture_release(g_dev, tex);
    neox_path_release(g_dev, g_star);
    neox_path_release(g_dev, g_blob);
    neox_path_release(g_dev, g_clip);
    neox_texture_release(g_dev, g_tex);
    neox_texture_release(g_dev, g_atlas);
    neox_device_pump(g_dev);
}

/* ------------------------------------------------------------------ neox_context_get_allocator（公开查询：模块据此决定转发语义） */
static void test_context_allocator_query(void)
{
    neox_context_desc cd;
    neox_context *c1 = NULL, *c2 = NULL;

    /* 注入路径：返回创建时的同一分配器指针 */
    memset(&cd, 0, sizeof(cd));
    {
        static neox_allocator a;
        a.user = &g_stat; a.alloc = t_alloc; a.free = t_free;
        cd.allocator = &a;
    }
    CHECK_RES(neox_context_create(&cd, &c1), NEOX_OK);
    CHECK(neox_context_get_allocator(c1) == cd.allocator);
    /* 默认路径（系统分配器）：返回 NULL */
    memset(&cd, 0, sizeof(cd));
    CHECK_RES(neox_context_create(&cd, &c2), NEOX_OK);
    CHECK(neox_context_get_allocator(c2) == NULL);
    /* 参数 NULL */
    CHECK(neox_context_get_allocator(NULL) == NULL);
    neox_context_destroy(c1);
    neox_context_destroy(c2);
}

/* ------------------------------------------------------------------ main */
int main(void)
{
    neox_context_desc cd;
    neox_device_desc dd;

    memset(&g_stat, 0, sizeof(g_stat));
    memset(&cd, 0, sizeof(cd));
    {
        static neox_allocator a;
        a.user = &g_stat; a.alloc = t_alloc; a.free = t_free;
        cd.allocator = &a;
    }
    cd.memory_budget = 0;
    if (neox_context_create(&cd, &g_ctx) != NEOX_OK) {
        fprintf(stderr, "context_create failed\n");
        return 2;
    }
    memset(&dd, 0, sizeof(dd));
    dd.backend = NEOX_BACKEND_SOFT;
    if (neox_device_create(g_ctx, &dd, &g_dev) != NEOX_OK) {
        fprintf(stderr, "device_create failed\n");
        return 2;
    }
    CHECK(neox_version() == 0x000100u);

    test_fill_rules();
    test_porter_duff();
    test_group_opacity();
    test_nested_clip();
    test_texture_sampling();
    test_glyphs();
    test_blend_coverage_interpolation();
    test_platform_size_contract();
    test_context_allocator_query();
    test_failure_semantics();
    test_handles();
    test_determinism_and_equivalence();
    test_capture_replay();

    /* 泄漏检查：销毁全部对象（含 context 自身）后 outstanding 归零 */
    neox_device_destroy(g_dev);
    neox_context_destroy(g_ctx);
    if (g_stat.outstanding != 0) {
        uint32_t i;
        fprintf(stderr, "live blocks after destroy: %llu\n",
                (unsigned long long)g_stat.outstanding);
        for (i = 0; i < g_stat.n; i++)
            fprintf(stderr, "  block %u size=%zu\n", i, g_stat.sizes[i]);
    }
    CHECK(g_stat.outstanding == 0);
    CHECK(g_stat.live == 0);

    if (g_failures == 0) {
        printf("ALL PASS (%d checks)\n", g_checks);
        return 0;
    }
    printf("%d/%d checks failed\n", g_failures, g_checks);
    return 1;
}
