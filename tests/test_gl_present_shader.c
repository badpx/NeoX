/* P1：GL present shader 的类型与颜色链回归。
 *
 * 本轮评审的起点是一个“静态可判定”的 shader 错误：present 片元里
 * enc(clamp(p.rgb, ...)) 只有 float 版本、外层 vec4 也缺分量，导致
 * gl_device_init 无条件编译失败（所有 GL 设备都创建不出来）。这里把 gl.c
 * 里的 GLSL 源码原样取出，用真实的 GL 上下文逐段编译验证：
 *
 *   1. 源码形态：present 不再跨类型调用 enc、不再做二次 sRGB 编码，输出
 *      显式不透明 alpha，行翻转仍保留；
 *   2. 设备可用时（EGL surfaceless / X11）：创建离屏 GL 设备，逐段编译全部
 *      fragment program（含 NX_DUAL=0/1 两种前缀），任何一段失败即 FAIL；
 *   3. 端到端半透明呈现：有 DISPLAY 时在 X11 窗口上画非预乘 sRGB (0,0.5,0,0.5)，
 *      读回必须是 (0,64,0,255)——RGB 预乘、sRGB 只编码一次，alpha 恒 255。
 *
 * 设备创建失败不得当作通过：只有环境确实没有 GL 时才返回 77（not_run），
 * 并且第 1 步仍必须已经通过。
 */
#include "internal.h"
#include "neox_config.h"
#include "neox_gl.h"
#include "neox.h"
#include "neox_present.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

#ifdef NEOX_ENABLE_GL
/* 直接包含执行器：本测试要拿到 GLSL 源码与 GL 入口（与其他 GL 测试一致，
 * 编译进本执行文件，不链接额外库）。 */
#include "../src/gl.c"

#define PROGRAM_LIST(X) \
    X(0, NX_GLSL_FS_FILL) X(1, NX_GLSL_FS_TEX) X(2, NX_GLSL_FS_GLYPH) \
    X(3, NX_GLSL_FS_CLAMP) X(4, NX_GLSL_FS_PRESENT) X(5, NX_GLSL_FS_PLUSFILL) \
    X(6, NX_GLSL_FS_PLUSGLYPH) X(7, NX_GLSL_FS_MANUALFILL) \
    X(8, NX_GLSL_FS_MANUALGLYPH) X(9, NX_GLSL_FS_MANUALTEX) \
    X(10, NX_GLSL_FS_COMP) X(11, NX_GLSL_FS_BLIT) X(12, NX_GLSL_FS_ENCODE) \
    X(13, NX_GLSL_FS_DECODE)

/* 与 gl_device_init 的 NX_BUILD 列表一一对应。 */
static void program_list(const char **out)
{
#define ASSIGN(i, src) out[i] = (src);
    PROGRAM_LIST(ASSIGN)
#undef ASSIGN
}

static void check_present_source(void)
{
    const char *src = NX_GLSL_FS_PRESENT;
    static const char *programs[14];
    program_list(programs);
    /* present 只输出，不做任何 sRGB 编码（surface 已编码过一次）。 */
    CHECK(strstr(src, "enc(") == NULL);
    CHECK(strstr(src, "pow(") == NULL);
    CHECK(strstr(src, "clamp(p.rgb") != NULL);
    CHECK(strstr(src, ", 1.0);") != NULL); /* 不透明 alpha */
    CHECK(strstr(src, "int(u_ss) - 1 - int(gl_FragCoord.y)") != NULL);
}

/* 用设备自己的 GL 入口编译一段 fragment shader（不链接，足以发现类型错误）。 */
static int compile_fragment(const char *body, const char *hdr, char *log, size_t loglen)
{
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    GLint ok = GL_FALSE;
    const char *srcs[2] = { hdr, body };
    GLsizei nsrc = hdr ? 2 : 1;
    if (!fs) return 0;
    glShaderSource(fs, nsrc, srcs, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE && log) {
        GLsizei l = 0;
        log[0] = '\0';
        glGetShaderInfoLog(fs, (GLsizei)loglen, &l, log);
        if (l < 0) l = 0;
        log[l < (GLsizei)loglen ? l : (GLsizei)loglen - 1] = '\0';
    }
    glDeleteShader(fs);
    return ok == GL_TRUE;
}

static int compile_all(void)
{
    char hdr[96], log[2048];
    static const char *programs[14];
    program_list(programs);
    unsigned i;
    int bad = 0;
    for (i = 0; i < 14; i++) {
        int d;
        /* 与 gl_prog_build 一致：dual < 0 时源码自带 #version；否则前置
         * #version + NX_DUAL=0/1（fill/tex/glyph 带 #if NX_DUAL 分支）。
         * 只有带 #if NX_DUAL 的程序需要 dual 变体，其余按原样编译一次。 */
        int needs_dual = strstr(programs[i], "NX_DUAL") != NULL;
        int passes = needs_dual ? 3 : 1;
        for (d = 0; d < passes; d++) {
            if (!needs_dual)
                hdr[0] = '\0'; /* 自带 #version：不加前缀，原样编译 */
            else
                snprintf(hdr, sizeof hdr, "#version 410 core\n#define NX_DUAL %d\n",
                         d - 1);
            log[0] = '\0';
            if (!compile_fragment(programs[i], hdr, log, sizeof log)) {
                fprintf(stderr, "FAIL GLSL program #%u (%s): %s\n", i, hdr, log);
                bad++;
            }
        }
    }
    return bad == 0;
}

/* 主机端渲染：只有 alpha=0.5 的红色矩形。 */
static neox_path make_half_red_rect(neox_device *dev)
{
    const uint8_t verbs[] = { NEOX_VERB_MOVE, NEOX_VERB_LINE, NEOX_VERB_LINE,
                              NEOX_VERB_LINE, NEOX_VERB_CLOSE };
    float pts[8] = { 0, 0, 32, 0, 32, 32, 0, 32 };
    neox_path_desc pd;
    neox_path p;
    memset(&pd, 0, sizeof pd);
    pd.verbs = verbs;
    pd.points = pts;
    pd.verb_count = 5;
    pd.point_count = 4;
    pd.fill_rule = NEOX_FILL_NONZERO;
    CHECK(neox_path_create(dev, &pd, &p) == NEOX_OK);
    return p;
}

static int draw_half_red(neox_device *dev, neox_gl_surface *surf, const neox_color *clear)
{
    /* 清屏色必须是有限色（同时验证 recording.c 的颜色校验不会误拒绝平台值）。 */
    neox_target_desc td;
    neox_frame *fr = NULL;
    neox_submission sub = { 0 };
    neox_paint paint;
    neox_path p = make_half_red_rect(dev);
    memset(&td, 0, sizeof td);
    td.kind = NEOX_BACKEND_GPU;
    td.surface = surf;
    td.clear = clear;
    memset(&paint, 0, sizeof paint);
    /* API 颜色是 sRGB 非预乘：绿 0.5（= 线性约 0.2140）配 alpha 0.5。 */
    paint.color = (neox_color){ 0.0f, 0.5f, 0.0f, 0.5f };
    if (neox_frame_begin(dev, &td, &fr) != NEOX_OK) {
        fprintf(stderr, "frame_begin: %s\n", neox_get_error_detail(dev->ctx));
        neox_path_release(dev, p);
        return 0;
    }
    CHECK(neox_draw_path(fr, p, &paint) == NEOX_OK);
    CHECK(neox_frame_end(fr, &sub) == NEOX_OK);
    CHECK(neox_submission_wait(dev, sub) == NEOX_OK);
    neox_submission_release(dev, sub);
    neox_path_release(dev, p);
    return 1;
}

/* 端到端半透明呈现：窗口 GL 表面（需 DISPLAY，GL 窗口表面只支持本地 display）。 */
static int translucent_present(neox_context *ctx)
{
#if NEOX_ENABLE_PRESENT && defined(__linux__)
    neox_present_desc wd;
    neox_window *w = NULL;
    neox_device *dev = NULL;
    neox_target_desc td;
    uint8_t px[64 * 64 * 4];
    uint32_t pxw = 0, pxh = 0;
    int i;
    if (!getenv("DISPLAY")) return 0;
    memset(&wd, 0, sizeof wd);
    wd.struct_size = (uint32_t)sizeof wd;
    wd.magic = NEOX_PRESENT_DESC_MAGIC;
    wd.title = "present-shader";
    wd.width = 32; wd.height = 32;
    if (neox_window_create(&wd, &w) != NEOX_OK) {
        printf("translucent present: no window (%s)\n", neox_window_last_error());
        return 0;
    }
    if (neox_window_create_device(w, ctx, NEOX_BACKEND_GPU, &dev) != NEOX_OK) {
        printf("translucent present: GL window device unavailable (%s)\n",
               neox_window_last_error());
        neox_window_destroy(w);
        return 0;
    }
    /* acquire 不写 clear：调用方必须自己初始化（含 clear=NULL 的零值）。 */
    memset(&td, 0, sizeof td);
    CHECK(neox_window_acquire_target(w, dev, &td) == NEOX_OK);
    neox_window_metrics(w, NULL, NULL, NULL, &pxw, &pxh);
    CHECK(pxw > 16 && pxh > 16);
    CHECK(td.surface != NULL);
    /* 不透明的靛蓝底 + alpha=0.5 的绿：绘制必须做 SRC_OVER，结果等于
     * sRGB 0.5 的绿；若后端忽略 alpha（如直接覆盖），读回会是别的值。 */
    /* 透明快照基底 + alpha=0.5 的绿：绘制按 SRC_OVER 合成，窗口呈现时
     * RGB 已预乘、sRGB 只编码一次，读回必须是 (0,64,0,255)；若 present
     * 再编码一次会读到约 (0,188,0,255) 或其他值。 */
    if (!draw_half_red(dev, (neox_gl_surface *)td.surface, NULL)) {
        neox_window_destroy(w);
        return 0;
    }
    CHECK(neox_window_request_readback(w) == NEOX_OK);
    CHECK(neox_window_present(w) == NEOX_OK);
    CHECK(neox_window_read_pixels(w, px) == NEOX_OK);
    for (i = 0; i < 4; i++) {
        /* 不透明窗口：RGB = 预乘后的 sRGB 分量（与 Soft 相同口径）+ alpha 255。 */
        static const int want[4] = { 0, 64, 0, 255 };
        if (px[(((size_t)pxh / 2) * pxw + pxw / 2) * 4 + (size_t)i] != want[i]) break;
    }
    if (i != 4) {
        const uint8_t *p = px + (((size_t)pxh / 2) * pxw + pxw / 2) * 4;
        fprintf(stderr, "FAIL translucent present: got %u,%u,%u,%u want 0,64,0,255\n",
                p[0], p[1], p[2], p[3]);
        neox_window_destroy(w);
        return -1;
    }
    neox_window_destroy(w);
    return 1;
#else
    (void)ctx;
    return 0;
#endif
}

/* Exercise the real presentation shader offscreen as well: CGL has no native
 * GL window surface, but must obey the same alpha and color-space contract. */
static void present_pixels(neox_device *dev, neox_gl_surface *source)
{
    nx_gl_dev *g = (nx_gl_dev *)dev->bstate;
    neox_gl_surface *output = NULL;
    uint8_t pixels[32 * 32 * 4];
    float v[6] = {0, 0, 64, 0, 0, 64};
    CHECK(neox_gl_surface_create(dev, 32, 32, &output) == NEOX_OK);
    glBindFramebuffer(GL_FRAMEBUFFER, output->fbo);
    glViewport(0, 0, 32, 32);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source->tex);
    gl_use(&g->p_present, 2.0f / 32, 2.0f / 32);
    glUniform1i(g->p_present.u_tex, 0);
    glUniform1f(g->p_present.u_ss, 32);
    gl_draw(g, v, 3);
    glReadPixels(0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    CHECK(gl_error(g) == GL_NO_ERROR);
    for (size_t i = 0; i < 32 * 32; ++i) {
        CHECK(pixels[i * 4] == 0 && pixels[i * 4 + 1] == 64);
        CHECK(pixels[i * 4 + 2] == 0 && pixels[i * 4 + 3] == 255);
    }
    neox_gl_surface_destroy(dev, output);
}

static int offscreen(neox_context *ctx, neox_device *dev)
{
    neox_gl_surface *surf = NULL;
    /* 设备创建已经无条件编译过全部程序；这里逐段再编一次，失败能指出是哪段。 */
    CHECK(compile_all());
    /* 离屏表面：alpha=0.5，读回应保留 alpha=128（非预乘）。 */
    if (neox_gl_surface_create(dev, 32, 32, &surf) != NEOX_OK) {
        fprintf(stderr, "offscreen surface failed: %s\n", neox_get_error_detail(ctx));
        return 0;
    }
    CHECK(draw_half_red(dev, surf, NULL));
    {
        uint8_t px[32 * 32 * 4];
        CHECK(neox_gl_surface_read_pixels(surf, px) == NEOX_OK);
        CHECK(px[(16 * 32 + 16) * 4 + 0] == 0);
        /* 非预乘读回：RGB 仍是 sRGB 0.5（128），alpha 保留 128。 */
        CHECK(px[(16 * 32 + 16) * 4 + 1] == 128);
        CHECK(px[(16 * 32 + 16) * 4 + 3] == 128);
    }
    present_pixels(dev, surf);
    neox_gl_surface_destroy(dev, surf);
    return 1;
}
#endif

int main(void)
{
#ifdef NEOX_ENABLE_GL
    neox_context *ctx = NULL;
    int runtime, window;
    check_present_source();
    printf("present shader source: ok (no vec3->float enc, no double encode)\n");
    {
        neox_device *dev = NULL;
        neox_gl_device_desc gld;
        neox_device_desc dd;
        CHECK(neox_context_create(NULL, &ctx) == NEOX_OK);
        memset(&gld, 0, sizeof gld);
        gld.struct_size = (uint32_t)sizeof gld;
        gld.magic = NEOX_GL_DESC_MAGIC;
        memset(&dd, 0, sizeof dd);
        dd.backend = NEOX_BACKEND_GPU;
        dd.platform = &gld;
        dd.platform_size = (uint32_t)sizeof gld;
        if (neox_device_create(ctx, &dd, &dev) != NEOX_OK) {
            printf("GL device creation unavailable: %s\n", neox_get_error_detail(ctx));
            CHECK(!getenv("NEOX_REQUIRE_GL") || !*getenv("NEOX_REQUIRE_GL"));
            runtime = 0;
        } else {
            runtime = offscreen(ctx, dev);
            neox_device_destroy(dev);
        }
        /* 窗口 GL 设备不能与同一 context 的离屏设备并存于同一窗口外表面使用
         * 顺序上无冲突：离屏设备已销毁，这里再建独立 context。 */
        if (runtime) {
            neox_context_destroy(ctx);
            ctx = NULL;
            CHECK(neox_context_create(NULL, &ctx) == NEOX_OK);
            window = translucent_present(ctx);
        } else {
            window = 0;
        }
        if (ctx) neox_context_destroy(ctx);
    }
    if (window < 0) return 1;
    if (runtime == 0) {
        printf("SKIP: no GL device in this environment (shader source checks passed)\n");
        return 77;
    }
    printf("present shader: all GLSL programs compile%s\n",
           window ? "; translucent present 0,64,0,255 ok"
                  : "; native GL window not_run (offscreen shader pixels verified)");
    return 0;
#else
    puts("SKIP: GL backend not built");
    return 77;
#endif
}
