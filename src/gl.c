/* ===========================================================================
 * gl.c — NeoX GPU 后端：OpenGL 4.1 Core Profile
 *
 * 上下文不在本文件创建：平台编译单元实现 gl_ctx.h（macOS 离屏 / Linux
 * 无头 surfaceless），本文件只经该接口触碰上下文。Linux 的 GL 入口由
 * gl_load.c 在上下文 current 之后填充；macOS 保持框架静态链接。
 *
 * 定位：链路验证后端（docs/semantics-v0.md §11 实施顺序说明）。验证"同一套
 * 语义描述（SD）可被两个差异明显的后端执行"：本后端与 Soft 共享全部上层
 * 设施（句柄表、命令流录制、scene 展开、CAP、submission/延迟回收，
 * 见 internal.h 后端 vtable 注释），仅替换执行引擎。
 *
 * 渲染管线（三角形批，neox_exec_report.algorithm = NEOX_ALG_TRIANGLE）：
 *   - 路径填充：stencil-then-cover。共享 geom.c 的 flatten 结果（与 Soft
 *     逐点一致），以包围盒外一点为扇心的三角扇写模板
 *     （NONZERO：双面 INCR_WRAP/DECR_WRAP，测试 NOTEQUAL 0；EVENODD：双面
 *     INVERT，测试 EQUAL 1），再以包围盒四边形在模板测试下着底色。
 *   - 抗锯齿：4×4 超采样（NEOX_GL_SS），帧末以 fragment shader 对 16 纹素
 *     盒滤波下采样到 1×。不使用 MSAA（采样数不满足 §9 容差）。
 *   - 线性光混合：层附件为 RGBA32F（预乘线性 float，与 Soft 内部表示一致），
 *     混合全程浮点无中间量化 → 实心内部可与 Soft 逐位一致；帧缓冲边界在
 *     resolve pass 做一次 sRGB 编码（rint ties-to-even，与 Soft 的
 *     nx_premul_linear_to_srgb8 逐步对齐）。GL_FRAMEBUFFER_SRGB 恒关闭。
 *   - Porter-Duff 13 模式 + §6.1 覆盖率插值：绘制走 gl_blend_cov 因子表
 *     （fragment alpha 按模式编码 oa，使 1−oa 展开出 cov·Fb₀+(1−cov)，
 *     与 soft.c nx_blend_pixel_cov 逐项同式）。SRC/SRC_IN/SRC_OUT/DST_ATOP
 *     需要 dual-source blending；初始化时探测，不支持或设置了
 *     NEOX_GL_FORCE_NO_DUAL 时这四模式改走 ping-pong 手动混合（与 Metal
 *     ADR 0005 同款）。PLUS 的 clamp 非线性始终走 ping-pong（fill/glyph
 *     读 dst 算插值式写临时层再拷回；tex 覆盖二值，折源 + 整层 clamp）。
 *     组合成（§7）走 gl_blend_plain 真实 Porter-Duff。
 *   - 组不透明度：PUSH owns_layer → 渲染到离屏层纹理（RGBA32F SS），POP 时
 *     以 opacity × 裁剪掩码整体合成回父层（§7 隔离合成）。
 *   - 裁剪：掩码在 push 期经 geom.c 急切光栅化（与 Soft 同一 CPU 覆盖率），
 *     执行期上传 R32F 纹理；绘制采样当前掩码（组合成时调制）。
 *   - draw_texture / draw_glyphs：纹理四边形 / 字形四边形；NEAREST/LINEAR 与
 *     A8 双线性遮罩在 shader 内以 texelFetch 手动实现（RGBA32F 不可滤波），
 *     公式与 Soft 的 sample_nearest/sample_linear 逐步一致；采样相位取设备
 *     像素中心（floor(frag/SS)+0.5），与 Soft 的像素中心采样对齐。
 *     负宽高目标矩形镜像由 tu/tv ∈ [0,1] 反转自然产生（§8）。
 *
 * 设备语义：pump 即执行 pending submission（录制阶段不触碰 GL；执行末
 * glFlush + glGetError 检查，同步点用 glFinish——最小实现允许阻塞）。
 * neox_device_is_lost 恒 false。执行期仅在最后 resolve 步骤写表面，
 * 之前任何 GL 错误 → submission FAILED 且表面保持调用前状态（§10.1）。
 *
 * 坐标约定：设备坐标（左上原点，y 向下）直接作为 GL window 坐标使用
 * （顶点映射 ndc = 2·p/H − 1），因此 glReadPixels 行序即设备行序，
 * 全链路无 y 翻转；内层纹理一律以 window 坐标 texelFetch 采样。
 *
 * 已知偏差（§1 未指定/容差外，报告备案）：
 *   - 混合单元与 CPU 的 float 表达式求值顺序（FMA 收缩）可能有 1 ulp 差，
 *     仅当结果恰落在 sRGB 量化 ties 边界时表现为 1 LSB（测试场景未发现）。
 * =========================================================================*/
#include "internal.h"
#include "neox_gl.h"
#include "gl_api.h"

#include <stdio.h>

/* 超采样倍数（规范 §9 修订记录：GPU 超采样盒滤波 4×4 网格 = 1/16 步长） */
/* 超采样倍数。§9 容差要求跨后端覆盖率一致，因此路径覆盖率直接复用 Soft 的
 * 共享光栅化（geom.c，垂直 8 子扫描线 × 水平解析），所有绘制按设备像素恒定，
 * 超采样网格退化为 1×1（SS 管线保留：层/下采样/编码路径不变，供未来
 * 纯 GPU 覆盖率（GLES 移植）在容差模型允许时重新启用）。 */
#define NEOX_GL_SS 1

/* ------------------------------------------------------------------ GLSL（410 core） */

static const char *NX_GLSL_VS =
    "#version 410 core\n"
    "uniform vec2 u_scale;\n"
    "layout(location = 0) in vec2 a_pos;\n"
    "void main(void) { gl_Position = vec4(a_pos * u_scale - 1.0, 0.0, 1.0); }\n";

/* #version 与 NX_DUAL 由 gl_prog_build 前置。index=1 才是 dual-source 的
 * SRC1（layout location=1 是第二条颜色附件，Mesa 不会把它送进混合单元）。 */
static const char *NX_GLSL_FS_FILL =
    "uniform sampler2D u_mask;\n"        /* R32F 设备尺寸覆盖率（geom.c 光栅化） */
    "uniform sampler2D u_premul;\n"      /* RGBA32F paint×cov（CPU 预乘，与参照逐位一致） */
    "uniform float u_ss;\n"
    "uniform int u_mode;\n"
    "uniform float u_paint_a;\n"
    "#if NX_DUAL\n"
    "layout(location = 0, index = 0) out vec4 o_color;\n"
    "layout(location = 0, index = 1) out vec4 o_color1;\n"
    "#else\n"
    "out vec4 o_color;\n"
    "#endif\n"
    "void main(void)\n"
    "{\n"
    /* cov==0 时 out = dst：整体跳过（§6.1 插值在 cov=0 处的退化） */
    "    float cov = texelFetch(u_mask, ivec2(gl_FragCoord.xy / u_ss), 0).r;\n"
    "    if (cov <= 0.0) discard;\n"
    "    vec4 p = texelFetch(u_premul, ivec2(gl_FragCoord.xy / u_ss), 0);\n"
    /* p.rgb = s·cov；p.a = sa·cov。oa/o1a 编码与 soft.c nx_blend_pixel_cov
     * 及 gl_blend_cov 因子表逐项对齐（o1 仅 alpha 分量参与混合） */
    "    float oa;\n"
    "    if (u_mode == 1) oa = cov;\n"
    "    else if (u_mode == 6) oa = cov * (1.0 - u_paint_a);\n"
    "    else oa = p.a;\n"
    "    o_color = vec4(p.rgb, oa);\n"
    "#if NX_DUAL\n"
    "    float o1a = 0.0;\n"
    "    if (u_mode == 2 || u_mode == 5 || u_mode == 7) o1a = cov;\n"
    "    else if (u_mode == 10) o1a = cov * (1.0 - u_paint_a);\n"
    "    o_color1 = vec4(0.0, 0.0, 0.0, o1a);\n"
    "#endif\n"
    "}\n";

static const char *NX_GLSL_FS_TEX =
    "uniform sampler2D u_tex;\n"        /* RGBA32F 预乘线性 */
    "uniform ivec2 u_tex_size;\n"
    "uniform int u_linear;\n"
    "uniform mat3 u_inv;\n"             /* 设备坐标 → 局部坐标 */
    "uniform vec4 u_src;\n"
    "uniform vec4 u_dst;\n"
    "uniform float u_ss;\n"
    "uniform float u_opacity;\n"
    "uniform int u_mode;\n"
    "#if NX_DUAL\n"
    "layout(location = 0, index = 0) out vec4 o_color;\n"
    "layout(location = 0, index = 1) out vec4 o_color1;\n"
    "#else\n"
    "out vec4 o_color;\n"
    "#endif\n"
    "void main(void)\n"
    "{\n"
    /* 采样位置取设备像素中心（floor(frag/ss)+0.5），与参照后端的像素中心采样对齐 */
    "    vec2 devc = floor(gl_FragCoord.xy / u_ss) + 0.5;\n"
    "    vec3 l = u_inv * vec3(devc, 1.0);\n"
    "    float tu = (l.x - u_dst.x) / u_dst[2];\n"
    "    float tv = (l.y - u_dst.y) / u_dst[3];\n"
    /* 像素中心二值包含（与参照后端一致：中心在目标矩形外则该像素不变） */
    "    if (tu < 0.0 || tu > 1.0 || tv < 0.0 || tv > 1.0) discard;\n"
    "    float u = u_src.x + tu * u_src[2];\n"
    "    float v = u_src.y + tv * u_src[3];\n"
    "    vec4 c;\n"
    "    if (u_linear == 0) {\n"
    "        int i = clamp(int(floor(u)), 0, u_tex_size.x - 1);\n"
    "        int j = clamp(int(floor(v)), 0, u_tex_size.y - 1);\n"
    "        c = texelFetch(u_tex, ivec2(i, j), 0);\n"
    "    } else {\n"
    "        float x = u - 0.5, y = v - 0.5;\n"
    "        float fx = x - floor(x), fy = y - floor(y);\n"
    "        int i0 = clamp(int(floor(x)), 0, u_tex_size.x - 1);\n"
    "        int i1 = clamp(int(floor(x)) + 1, 0, u_tex_size.x - 1);\n"
    "        int j0 = clamp(int(floor(y)), 0, u_tex_size.y - 1);\n"
    "        int j1 = clamp(int(floor(y)) + 1, 0, u_tex_size.y - 1);\n"
    "        vec4 p00 = texelFetch(u_tex, ivec2(i0, j0), 0);\n"
    "        vec4 p10 = texelFetch(u_tex, ivec2(i1, j0), 0);\n"
    "        vec4 p01 = texelFetch(u_tex, ivec2(i0, j1), 0);\n"
    "        vec4 p11 = texelFetch(u_tex, ivec2(i1, j1), 0);\n"
    "        c = p00 * ((1.0 - fx) * (1.0 - fy)) + p10 * (fx * (1.0 - fy))\n"
    "          + p01 * ((1.0 - fx) * fy) + p11 * (fx * fy);\n"
    "    }\n"
    /* 覆盖为二值（像素中心包含）：cov=1，oa/o1a 编码见 gl_blend_cov */
    "    float sa = c.a * u_opacity;\n"
    "    float oa;\n"
    "    if (u_mode == 1) oa = 1.0;\n"
    "    else if (u_mode == 6) oa = 1.0 - sa;\n"
    "    else oa = sa;\n"
    "    o_color = vec4(c.rgb * u_opacity, oa);\n"
    "#if NX_DUAL\n"
    "    float o1a = 0.0;\n"
    "    if (u_mode == 2 || u_mode == 5 || u_mode == 7) o1a = 1.0;\n"
    "    else if (u_mode == 10) o1a = 1.0 - sa;\n"
    "    o_color1 = vec4(0.0, 0.0, 0.0, o1a);\n"
    "#endif\n"
    "}\n";

static const char *NX_GLSL_FS_GLYPH =
    "uniform sampler2D u_atlas;\n"       /* A8（R 通道 = 覆盖率） */
    "uniform vec4 u_color;\n"            /* 预乘线性 paint */
    "uniform mat3 u_inv;\n"
    "uniform vec4 u_glyph;\n"            /* x,y,w,h（局部坐标） */
    "uniform vec4 u_atlas_rect;\n"       /* sx,sy,sw,sh */
    "uniform float u_ss;\n"
    "uniform int u_mode;\n"
    "#if NX_DUAL\n"
    "layout(location = 0, index = 0) out vec4 o_color;\n"
    "layout(location = 0, index = 1) out vec4 o_color1;\n"
    "#else\n"
    "out vec4 o_color;\n"
    "#endif\n"
    "void main(void)\n"
    "{\n"
    "    vec2 devc = floor(gl_FragCoord.xy / u_ss) + 0.5;\n"
    "    vec3 l = u_inv * vec3(devc, 1.0);\n"
    /* 像素中心二值包含（与参照后端一致） */
    "    if (l.x < u_glyph.x || l.x > u_glyph.x + u_glyph.z ||\n"
    "        l.y < u_glyph.y || l.y > u_glyph.y + u_glyph.w) discard;\n"
    "    float u = u_atlas_rect.x + (l.x - u_glyph.x);\n"
    "    float v = u_atlas_rect.y + (l.y - u_glyph.y);\n"
    "    float x = u - 0.5, y = v - 0.5;\n"
    "    float fx = x - floor(x), fy = y - floor(y);\n"
    "    ivec2 sz = textureSize(u_atlas, 0);\n"
    "    int i0 = clamp(int(floor(x)), 0, sz.x - 1);\n"
    "    int i1 = clamp(int(floor(x)) + 1, 0, sz.x - 1);\n"
    "    int j0 = clamp(int(floor(y)), 0, sz.y - 1);\n"
    "    int j1 = clamp(int(floor(y)) + 1, 0, sz.y - 1);\n"
    "    float cov = texelFetch(u_atlas, ivec2(i0, j0), 0).a * ((1.0 - fx) * (1.0 - fy))\n"
    "              + texelFetch(u_atlas, ivec2(i1, j0), 0).a * (fx * (1.0 - fy))\n"
    "              + texelFetch(u_atlas, ivec2(i0, j1), 0).a * ((1.0 - fx) * fy)\n"
    "              + texelFetch(u_atlas, ivec2(i1, j1), 0).a * (fx * fy);\n"
    /* oa/o1a 编码与 soft.c nx_blend_pixel_cov 同式（sa = u_color.a） */
    "    float oa;\n"
    "    if (u_mode == 1) oa = cov;\n"
    "    else if (u_mode == 6) oa = cov * (1.0 - u_color.a);\n"
    "    else oa = u_color.a * cov;\n"
    "    o_color = vec4(u_color.rgb * cov, oa);\n"
    "#if NX_DUAL\n"
    "    float o1a = 0.0;\n"
    "    if (u_mode == 2 || u_mode == 5 || u_mode == 7) o1a = cov;\n"
    "    else if (u_mode == 10) o1a = cov * (1.0 - u_color.a);\n"
    "    o_color1 = vec4(0.0, 0.0, 0.0, o1a);\n"
    "#endif\n"
    "}\n";

static const char *NX_GLSL_FS_CLAMP =
    "#version 410 core\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 o_color;\n"
    "void main(void)\n"
    "{\n"
    "    o_color = clamp(texelFetch(u_tex, ivec2(gl_FragCoord.xy), 0), 0.0, 1.0);\n"
    "}\n";

/* 窗口呈现：resolve 纹理的第 0 行是设备顶行，但按 GL 纹素原点放在底边。
 * 真实窗口的底边是视觉底边，所以这里按高度翻转，让设备顶行出现在窗口顶边。
 * u_ss 复用为像素高度。离屏 read_pixels 不走这个 shader。 */
static const char *NX_GLSL_FS_PRESENT =
    "#version 410 core\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_ss;\n"
    "out vec4 o_color;\n"
    "void main(void)\n"
    "{\n"
    "    int y = int(u_ss) - 1 - int(gl_FragCoord.y);\n"
    "    o_color = texelFetch(u_tex, ivec2(int(gl_FragCoord.x), y), 0);\n"
    "}\n";

/* PLUS（§6.1）：clamp 非线性 + 覆盖率插值无法由固定功能因子表达。
 * 读取层内容 dst，计算 out = cov·clamp(s+d,1) + (1−cov)·d（cov≥0 时等于
 * min(s·cov + cov·d, cov) + (1−cov)·d），写入临时层后由调用方拷回。 */
static const char *NX_GLSL_FS_PLUSFILL =
    "#version 410 core\n"
    "uniform sampler2D u_premul;\n"      /* s·cov（CPU 预乘） */
    "uniform sampler2D u_mask;\n"        /* cov */
    "uniform sampler2D u_layer;\n"       /* dst */
    "uniform float u_ss;\n"
    "out vec4 o_color;\n"
    "void main(void)\n"
    "{\n"
    "    ivec2 dp = ivec2(gl_FragCoord.xy / u_ss);\n"
    "    vec4 dst = texelFetch(u_layer, dp, 0);\n"
    "    float cov = texelFetch(u_mask, dp, 0).r;\n"
    /* cov=0 时 out = dst：透传（拷贝回后与不绘制等价；discard 会留下
     * 临时层陈旧内容并被拷回） */
    "    if (cov <= 0.0) { o_color = dst; return; }\n"
    "    vec4 sc = texelFetch(u_premul, dp, 0);\n"
    "    o_color = min(sc + cov * dst, vec4(cov)) + (1.0 - cov) * dst;\n"
    "}\n";

static const char *NX_GLSL_FS_PLUSGLYPH =
    "#version 410 core\n"
    "uniform sampler2D u_atlas;\n"
    "uniform sampler2D u_layer;\n"       /* dst */
    "uniform vec4 u_color;\n"
    "uniform mat3 u_inv;\n"
    "uniform vec4 u_glyph;\n"
    "uniform vec4 u_atlas_rect;\n"
    "uniform float u_ss;\n"
    "out vec4 o_color;\n"
    "void main(void)\n"
    "{\n"
    "    vec2 devc = floor(gl_FragCoord.xy / u_ss) + 0.5;\n"
    "    vec3 l = u_inv * vec3(devc, 1.0);\n"
    "    if (l.x < u_glyph.x || l.x > u_glyph.x + u_glyph.z ||\n"
    "        l.y < u_glyph.y || l.y > u_glyph.y + u_glyph.w) discard;\n"
    "    float u = u_atlas_rect.x + (l.x - u_glyph.x);\n"
    "    float v = u_atlas_rect.y + (l.y - u_glyph.y);\n"
    "    float x = u - 0.5, y = v - 0.5;\n"
    "    float fx = x - floor(x), fy = y - floor(y);\n"
    "    ivec2 sz = textureSize(u_atlas, 0);\n"
    "    int i0 = clamp(int(floor(x)), 0, sz.x - 1);\n"
    "    int i1 = clamp(int(floor(x)) + 1, 0, sz.x - 1);\n"
    "    int j0 = clamp(int(floor(y)), 0, sz.y - 1);\n"
    "    int j1 = clamp(int(floor(y)) + 1, 0, sz.y - 1);\n"
    "    float cov = texelFetch(u_atlas, ivec2(i0, j0), 0).a * ((1.0 - fx) * (1.0 - fy))\n"
    "              + texelFetch(u_atlas, ivec2(i1, j0), 0).a * (fx * (1.0 - fy))\n"
    "              + texelFetch(u_atlas, ivec2(i0, j1), 0).a * ((1.0 - fx) * fy)\n"
    "              + texelFetch(u_atlas, ivec2(i1, j1), 0).a * (fx * fy);\n"
    "    vec4 dst = texelFetch(u_layer, ivec2(gl_FragCoord.xy / u_ss), 0);\n"
    "    if (cov <= 0.0) { o_color = dst; return; }\n"
    "    vec4 sc = u_color * cov;\n"
    "    o_color = min(sc + cov * dst, vec4(cov)) + (1.0 - cov) * dst;\n"
    "}\n";

/* dual-source 不可用时的回退（仅 SRC=2 / SRC_IN=5 / SRC_OUT=7 / DST_ATOP=10）。
 * 直接用 sc = s·cov，避免 sc/cov 再乘回去的舍入。与 soft.c nx_blend_pixel_cov
 * 同一展开：out = sc·Fa + dst·Fb，Fb 已含 (1−cov)。 */
static const char *NX_GLSL_FS_MANUALFILL =
    "#version 410 core\n"
    "uniform sampler2D u_premul;\n"
    "uniform sampler2D u_mask;\n"
    "uniform sampler2D u_layer;\n"
    "uniform float u_ss;\n"
    "uniform int u_mode;\n"
    "out vec4 o_color;\n"
    "void main(void)\n"
    "{\n"
    "    ivec2 dp = ivec2(gl_FragCoord.xy / u_ss);\n"
    "    vec4 dst = texelFetch(u_layer, dp, 0);\n"
    "    float cov = texelFetch(u_mask, dp, 0).r;\n"
    "    if (cov <= 0.0) { o_color = dst; return; }\n"
    "    vec4 sc = texelFetch(u_premul, dp, 0);\n"
    "    float ic = 1.0 - cov;\n"
    "    if (u_mode == 2) o_color = sc + ic * dst;\n"
    "    else if (u_mode == 5) o_color = sc * dst.a + ic * dst;\n"
    "    else if (u_mode == 7) o_color = sc * (1.0 - dst.a) + ic * dst;\n"
    "    else o_color = sc * (1.0 - dst.a) + dst * (sc.a + ic);\n"
    "}\n";

static const char *NX_GLSL_FS_MANUALGLYPH =
    "#version 410 core\n"
    "uniform sampler2D u_atlas;\n"
    "uniform sampler2D u_layer;\n"
    "uniform vec4 u_color;\n"
    "uniform mat3 u_inv;\n"
    "uniform vec4 u_glyph;\n"
    "uniform vec4 u_atlas_rect;\n"
    "uniform float u_ss;\n"
    "uniform int u_mode;\n"
    "out vec4 o_color;\n"
    "void main(void)\n"
    "{\n"
    "    ivec2 dp = ivec2(gl_FragCoord.xy / u_ss);\n"
    "    vec4 dst = texelFetch(u_layer, dp, 0);\n"
    "    vec2 devc = floor(gl_FragCoord.xy / u_ss) + 0.5;\n"
    "    vec3 l = u_inv * vec3(devc, 1.0);\n"
    "    if (l.x < u_glyph.x || l.x > u_glyph.x + u_glyph.z ||\n"
    "        l.y < u_glyph.y || l.y > u_glyph.y + u_glyph.w) {\n"
    "        o_color = dst; return;\n"
    "    }\n"
    "    float u = u_atlas_rect.x + (l.x - u_glyph.x);\n"
    "    float v = u_atlas_rect.y + (l.y - u_glyph.y);\n"
    "    float x = u - 0.5, y = v - 0.5;\n"
    "    float fx = x - floor(x), fy = y - floor(y);\n"
    "    ivec2 sz = textureSize(u_atlas, 0);\n"
    "    int i0 = clamp(int(floor(x)), 0, sz.x - 1);\n"
    "    int i1 = clamp(int(floor(x)) + 1, 0, sz.x - 1);\n"
    "    int j0 = clamp(int(floor(y)), 0, sz.y - 1);\n"
    "    int j1 = clamp(int(floor(y)) + 1, 0, sz.y - 1);\n"
    "    float cov = texelFetch(u_atlas, ivec2(i0, j0), 0).a * ((1.0 - fx) * (1.0 - fy))\n"
    "              + texelFetch(u_atlas, ivec2(i1, j0), 0).a * (fx * (1.0 - fy))\n"
    "              + texelFetch(u_atlas, ivec2(i0, j1), 0).a * ((1.0 - fx) * fy)\n"
    "              + texelFetch(u_atlas, ivec2(i1, j1), 0).a * (fx * fy);\n"
    "    if (cov <= 0.0) { o_color = dst; return; }\n"
    "    vec4 sc = u_color * cov;\n"
    "    float ic = 1.0 - cov;\n"
    "    if (u_mode == 2) o_color = sc + ic * dst;\n"
    "    else if (u_mode == 5) o_color = sc * dst.a + ic * dst;\n"
    "    else if (u_mode == 7) o_color = sc * (1.0 - dst.a) + ic * dst;\n"
    "    else o_color = sc * (1.0 - dst.a) + dst * (sc.a + ic);\n"
    "}\n";

/* 纹理覆盖是像素中心二值（cov=1）。矩形外写回 dst，供 ping-pong 的
 * AABB 角点保持原内容。 */
static const char *NX_GLSL_FS_MANUALTEX =
    "#version 410 core\n"
    "uniform sampler2D u_tex;\n"
    "uniform sampler2D u_layer;\n"
    "uniform ivec2 u_tex_size;\n"
    "uniform int u_linear;\n"
    "uniform mat3 u_inv;\n"
    "uniform vec4 u_src;\n"
    "uniform vec4 u_dst;\n"
    "uniform float u_ss;\n"
    "uniform float u_opacity;\n"
    "uniform int u_mode;\n"
    "out vec4 o_color;\n"
    "void main(void)\n"
    "{\n"
    "    ivec2 dp = ivec2(gl_FragCoord.xy / u_ss);\n"
    "    vec4 dst = texelFetch(u_layer, dp, 0);\n"
    "    vec2 devc = floor(gl_FragCoord.xy / u_ss) + 0.5;\n"
    "    vec3 l = u_inv * vec3(devc, 1.0);\n"
    "    float tu = (l.x - u_dst.x) / u_dst[2];\n"
    "    float tv = (l.y - u_dst.y) / u_dst[3];\n"
    "    if (tu < 0.0 || tu > 1.0 || tv < 0.0 || tv > 1.0) {\n"
    "        o_color = dst; return;\n"
    "    }\n"
    "    float u = u_src.x + tu * u_src[2];\n"
    "    float v = u_src.y + tv * u_src[3];\n"
    "    vec4 c;\n"
    "    if (u_linear == 0) {\n"
    "        int i = clamp(int(floor(u)), 0, u_tex_size.x - 1);\n"
    "        int j = clamp(int(floor(v)), 0, u_tex_size.y - 1);\n"
    "        c = texelFetch(u_tex, ivec2(i, j), 0);\n"
    "    } else {\n"
    "        float x = u - 0.5, y = v - 0.5;\n"
    "        float fx = x - floor(x), fy = y - floor(y);\n"
    "        int i0 = clamp(int(floor(x)), 0, u_tex_size.x - 1);\n"
    "        int i1 = clamp(int(floor(x)) + 1, 0, u_tex_size.x - 1);\n"
    "        int j0 = clamp(int(floor(y)), 0, u_tex_size.y - 1);\n"
    "        int j1 = clamp(int(floor(y)) + 1, 0, u_tex_size.y - 1);\n"
    "        vec4 p00 = texelFetch(u_tex, ivec2(i0, j0), 0);\n"
    "        vec4 p10 = texelFetch(u_tex, ivec2(i1, j0), 0);\n"
    "        vec4 p01 = texelFetch(u_tex, ivec2(i0, j1), 0);\n"
    "        vec4 p11 = texelFetch(u_tex, ivec2(i1, j1), 0);\n"
    "        c = p00 * ((1.0 - fx) * (1.0 - fy)) + p10 * (fx * (1.0 - fy))\n"
    "          + p01 * ((1.0 - fx) * fy) + p11 * (fx * fy);\n"
    "    }\n"
    "    vec4 sc = vec4(c.rgb * u_opacity, c.a * u_opacity);\n"
    "    if (u_mode == 2) o_color = sc;\n"
    "    else if (u_mode == 5) o_color = sc * dst.a;\n"
    "    else if (u_mode == 7) o_color = sc * (1.0 - dst.a);\n"
    "    else o_color = sc * (1.0 - dst.a) + dst * sc.a;\n"
    "}\n";

static const char *NX_GLSL_FS_COMP =
    "#version 410 core\n"
    "uniform sampler2D u_layer;\n"       /* RGBA32F 超采样层 */
    "uniform sampler2D u_clip;\n"        /* R32F 设备尺寸覆盖率 */
    "uniform int u_has_clip;\n"
    "uniform float u_opacity;\n"
    "uniform float u_ss;\n"
    "out vec4 o_color;\n"
    "void main(void)\n"
    "{\n"
    "    vec4 s = texelFetch(u_layer, ivec2(gl_FragCoord.xy), 0);\n"
    "    float m = 1.0;\n"
    "    if (u_has_clip != 0) {\n"
    "        ivec2 dp = ivec2(gl_FragCoord.xy / u_ss);\n"
    "        m = texelFetch(u_clip, dp, 0).r;\n"
    "    }\n"
    "    o_color = s * (u_opacity * m);\n"
    "}\n";

static const char *NX_GLSL_FS_BLIT =
    "#version 410 core\n"
    "uniform sampler2D u_tex;\n"         /* RGBA32F W×H 基底 */
    "uniform float u_ss;\n"
    "out vec4 o_color;\n"
    "void main(void)\n"
    "{\n"
    "    ivec2 dp = ivec2(gl_FragCoord.xy / u_ss);\n"
    "    o_color = texelFetch(u_tex, dp, 0);\n"
    "}\n";

/* ------------------------------------------------------------------ 后端状态 */

typedef struct nx_gl_prog {
    GLuint id;
    GLint u_scale, u_color, u_opacity, u_tex, u_tex_size, u_linear, u_inv,
          u_src, u_dst, u_ss, u_layer, u_clip, u_has_clip, u_glyph,
          u_atlas_rect, u_atlas, u_premul, u_mode, u_paint_a, u_mask;
} nx_gl_prog;

typedef struct nx_gl_rt {       /* 渲染目标池项（层 / 裁剪掩码纹理） */
    GLuint fbo, tex, srb;       /* srb：保留字段（当前无模板附件） */
    uint32_t w, h;              /* 像素（层为超采样尺寸） */
    int in_use;
    int is_layer;
} nx_gl_rt;

typedef struct nx_gl_texent {   /* 纹理槽 → GL 纹理（惰性上传） */
    GLuint id;
    uint32_t version;
} nx_gl_texent;

struct neox_gl_surface {
    neox_device *dev;           /* 所属设备（上下文归属与借用校验） */
    GLuint fbo, tex;
    uint32_t w, h;
    void *win_egl;              /* EGLSurface；离屏为 NULL */
    void *native_window;
    int vsync;
    uint8_t *presented;         /* 最近一次 present 的窗口读回，左上原点 */
};

typedef struct nx_gl_dev {
    nx_gl_ctx *ctx;             /* 平台上下文；shared_context 只加入共享组 */
    int gl_ready;               /* 入口已解析，销毁时可以发 GL 调用 */
    int dual_ok;                /* GL_MAX_DUAL_SOURCE_DRAW_BUFFERS >= 1 且探测链接成功 */
    GLuint vao, vbo;
    nx_gl_prog p_fill, p_tex, p_glyph, p_comp, p_blit, p_clamp, p_plusfill, p_plusglyph;
    nx_gl_prog p_manualfill, p_manualglyph, p_manualtex, p_present;
    nx_gl_rt *rts;              /* 层池（is_layer=1）与裁剪纹理池（is_layer=0） */
    uint32_t nrts, caprts;
    nx_gl_texent *texs;         /* 并行于 textures 槽位表 */
    uint32_t ntexs, captexs;
    neox_gl_surface **surfs;    /* 表面注册表（所有权借用校验与级联清理） */
    uint32_t nsurfs, capsurfs;
    GLuint snap_tex;            /* 基底快照纹理（W×H RGBA32F） */
    uint32_t snap_w, snap_h;
    float *mask;                /* 路径覆盖率掩码 scratch（W×H，geom.c 光栅化） */
    uint32_t mask_w, mask_h;
    float *premul;              /* paint×cov scratch（W×H×4，CPU 预乘） */
    uint32_t premul_w, premul_h;
    uint32_t max_tex;
    int failed;                 /* 执行期 GL 错误粘滞标记（跨 draw  early-exit） */
} nx_gl_dev;

/* ------------------------------------------------------------------ GL 小工具 */

static int gl_make_current(nx_gl_dev *g)
{
    return g->ctx && nx_gl_ctx_make_current(g->ctx);
}

/* SRC/SRC_IN/SRC_OUT/DST_ATOP 的 dst 因子要用 SRC1，单源混合表达不了。 */
static int gl_mode_needs_dual(neox_blend m)
{
    return m == NEOX_BLEND_SRC || m == NEOX_BLEND_SRC_IN ||
           m == NEOX_BLEND_SRC_OUT || m == NEOX_BLEND_DST_ATOP;
}

static void gl_prog_free(nx_gl_prog *p)
{
    if (p->id) glDeleteProgram(p->id);
    memset(p, 0, sizeof(*p));
}

static GLint gl_ul(GLuint prog, const char *name)
{
    return glGetUniformLocation(prog, name);
}

/* 编译 + 链接；失败返回 NEOX_ERROR_BACKEND 并留静态错误细节 */
/* dual < 0：fs_src 自带 #version。dual 为 0/1：前置 #version 与 NX_DUAL。 */
static neox_result gl_prog_build(neox_context *ctx, GLuint vs,
                                 const char *fs_src, const char *what,
                                 nx_gl_prog *out, int dual)
{
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    GLuint p;
    GLint ok = GL_FALSE;
    const char *srcs[2];
    char hdr[80];
    GLsizei nsrc = 1;
    srcs[0] = fs_src;
    if (dual >= 0) {
        snprintf(hdr, sizeof hdr, "#version 410 core\n#define NX_DUAL %d\n",
                 dual ? 1 : 0);
        srcs[0] = hdr;
        srcs[1] = fs_src;
        nsrc = 2;
    }
    glShaderSource(fs, nsrc, srcs, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        glDeleteShader(fs);
        ctx->last_error = "GL fragment shader compile failed";
        (void)what;
        return NEOX_ERROR_BACKEND;
    }
    p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(fs);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        glDeleteProgram(p);
        ctx->last_error = "GL program link failed";
        return NEOX_ERROR_BACKEND;
    }
    memset(out, 0, sizeof(*out));
    out->id = p;
    out->u_scale      = gl_ul(p, "u_scale");
    out->u_color      = gl_ul(p, "u_color");
    out->u_opacity    = gl_ul(p, "u_opacity");
    out->u_tex        = gl_ul(p, "u_tex");
    out->u_tex_size   = gl_ul(p, "u_tex_size");
    out->u_linear     = gl_ul(p, "u_linear");
    out->u_inv        = gl_ul(p, "u_inv");
    out->u_src        = gl_ul(p, "u_src");
    out->u_dst        = gl_ul(p, "u_dst");
    out->u_ss         = gl_ul(p, "u_ss");
    out->u_layer      = gl_ul(p, "u_layer");
    out->u_clip       = gl_ul(p, "u_clip");
    out->u_has_clip   = gl_ul(p, "u_has_clip");
    out->u_glyph      = gl_ul(p, "u_glyph");
    out->u_atlas_rect = gl_ul(p, "u_atlas_rect");
    out->u_atlas      = gl_ul(p, "u_atlas");
    out->u_premul     = gl_ul(p, "u_premul");
    out->u_mode       = gl_ul(p, "u_mode");
    out->u_paint_a    = gl_ul(p, "u_paint_a");
    out->u_mask       = gl_ul(p, "u_mask");
    return NEOX_OK;
}

static void gl_use(const nx_gl_prog *p, float sx, float sy)
{
    glUseProgram(p->id);
    glUniform2f(p->u_scale, sx, sy);
}

/* 上传顶点（设备坐标 ×2 分量）并绘制三角形 */
static void gl_draw(const nx_gl_dev *g, const float *v, int nverts)
{
    glBindVertexArray(g->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nverts * 2 * (GLsizei)sizeof(float),
                 v, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const GLvoid *)0);
    glDrawArrays(GL_TRIANGLES, 0, nverts);
}

/* 全屏三角形（设备坐标 [0,w]×[0,h]，放大 2 倍保证覆盖） */
static void gl_draw_full(const nx_gl_dev *g, const nx_gl_prog *p,
                         float w, float h, float sx, float sy)
{
    float v[6] = { 0.0f, 0.0f,  2.0f * w, 0.0f,  0.0f, 2.0f * h };
    gl_use(p, sx, sy);
    gl_draw(g, v, 3);
}

/* 凸四边形各边向外偏移 d（设备像素）。
 * 用途：纹理/字形四边形的覆盖判定在参照后端是"设备像素中心二值包含"
 * （无边缘 AA，§8/测试 ⑤⑥ 的精确字节断言即此语义）。GL 侧将几何外扩，
 * 使被保留像素的全部超采样子像素都产生 fragment，再由 shader 按像素中心
 * 判定丢弃——与参照后端逐位一致。d 取 0.6 > 子像素中心最大偏移 √(2)·3/8。 */
static void gl_inflate_quad(const float xc[4], const float yc[4], float d,
                            float ox[4], float oy[4])
{
    float cx = (xc[0] + xc[1] + xc[2] + xc[3]) * 0.25f;
    float cy = (yc[0] + yc[1] + yc[2] + yc[3]) * 0.25f;
    float A[4], B[4], C[4];
    int i;
    for (i = 0; i < 4; i++) {
        int i2 = (i + 1) & 3;
        float dx = xc[i2] - xc[i], dy = yc[i2] - yc[i];
        float len = sqrtf(dx * dx + dy * dy);
        float nx, ny;
        if (len == 0.0f) { A[i] = 0.0f; B[i] = 0.0f; C[i] = 0.0f; continue; }
        nx = -dy / len;
        ny = dx / len;
        if (nx * (cx - xc[i]) + ny * (cy - yc[i]) > 0.0f) { nx = -nx; ny = -ny; }
        A[i] = nx; B[i] = ny;
        C[i] = nx * xc[i] + ny * yc[i] + d;
    }
    for (i = 0; i < 4; i++) {
        int i0 = (i + 3) & 3;
        float det = A[i0] * B[i] - A[i] * B[i0];
        if (det == 0.0f) { ox[i] = xc[i]; oy[i] = yc[i]; continue; }
        ox[i] = (C[i0] * B[i] - C[i] * B[i0]) / det;
        oy[i] = (A[i0] * C[i] - A[i] * C[i0]) / det;
    }
}

/* Porter-Duff 混合 → glBlendFuncSeparate 因子表（方程均 ADD，颜色与
 * alpha 同因子）。
 *
 * gl_blend_cov：绘制路径（§6.1 覆盖率插值语义）。fragment 输出
 * o = (src·cov, oa)，oa 按模式编码使 Fb = 1−oa 展开后给出
 * cov·Fb₀ + (1−cov)（Fb₀ 为 §6 表中原因子）：
 *   CLEAR/SRC/SRC_IN/SRC_OUT → oa = cov；
 *   DST_IN/DST_ATOP          → oa = cov·(1−sa)；
 *   其余                     → oa = sa·cov。
 * cov=1 时退化为 §6 原公式；cov=0 时各模式 dst 项系数均为 1（绘制在
 * fragment shader 中 discard，不触碰 dst）。PLUS 的 clamp 非线性无法
 * 由因子表达，走 ping-pong 手动混合（fill/glyph）或 clamp pass（tex）。
 *
 * gl_blend_plain：组/裁剪组合成（§7）使用真实 Porter-Duff（fragment
 * alpha = 源 alpha，无覆盖率编码）。 */
/* §6.1 覆盖率插值混合 → glBlendFuncSeparate 因子表（方程均 ADD）。
 *
 * fragment 双输出（ARB_blend_func_extended，GL 4.0 core）：
 *   o  = (src·cov, oa)，o1 = (_, o1a)
 * co_rgb = o.rgb·Fa + d·Fb；co_a = oa·FaA + da·FbA。
 * oa/o1a 编码使各模式展开为 cov·blend(s,d) + (1−cov)·d（推导见 soft.c
 * nx_blend_pixel_cov 注释表）：oa = CLEAR?cov : DST_IN?cov(1−sa) : sa·cov；
 * o1a = SRC/SRC_IN/SRC_OUT?cov : DST_ATOP?cov(1−sa) : 0。
 * SRC/SRC_IN/SRC_OUT/DST_ATOP 的 dst 因子用 GL_SRC1_ALPHA 族（oa 与 o1a
 * 取值不同，单源无法表达）。cov=1 时退化为 §6 原 Porter-Duff。
 * PLUS 的 clamp 非线性走 ping-pong 手动混合（fill/glyph）或 clamp pass
 * （tex），此处 (ONE,ONE) 仅用于 tex。 */
static void gl_blend_cov(neox_blend m)
{
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    switch (m) {
    case NEOX_BLEND_DST:      glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ZERO, GL_ONE); break;
    case NEOX_BLEND_DST_OVER: glBlendFuncSeparate(GL_ONE_MINUS_DST_ALPHA, GL_ONE,
                                                  GL_ONE_MINUS_DST_ALPHA, GL_ONE); break;
    case NEOX_BLEND_SRC:      glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC1_ALPHA,
                                                  GL_ONE, GL_ONE_MINUS_SRC1_ALPHA); break;
    case NEOX_BLEND_SRC_IN:   glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE_MINUS_SRC1_ALPHA,
                                                  GL_DST_ALPHA, GL_ONE_MINUS_SRC1_ALPHA); break;
    case NEOX_BLEND_SRC_OUT:  glBlendFuncSeparate(GL_ONE_MINUS_DST_ALPHA,
                                                  GL_ONE_MINUS_SRC1_ALPHA,
                                                  GL_ONE_MINUS_DST_ALPHA,
                                                  GL_ONE_MINUS_SRC1_ALPHA); break;
    case NEOX_BLEND_DST_ATOP: glBlendFuncSeparate(GL_ONE_MINUS_DST_ALPHA,
                                                  GL_ONE_MINUS_SRC1_ALPHA,
                                                  GL_ONE_MINUS_DST_ALPHA,
                                                  GL_ONE_MINUS_SRC1_ALPHA); break;
    case NEOX_BLEND_SRC_ATOP: glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                                  GL_DST_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
    case NEOX_BLEND_DST_IN:   glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA,
                                                  GL_ZERO, GL_ONE_MINUS_SRC_ALPHA); break;
    case NEOX_BLEND_CLEAR:
    case NEOX_BLEND_DST_OUT:  glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA,
                                                  GL_ZERO, GL_ONE_MINUS_SRC_ALPHA); break;
    case NEOX_BLEND_XOR:      glBlendFuncSeparate(GL_ONE_MINUS_DST_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                                  GL_ONE_MINUS_DST_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
    case NEOX_BLEND_PLUS:     glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE); break;
    case NEOX_BLEND_SRC_OVER:
    default:                  glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                                                  GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
    }
}

static void gl_blend_plain(neox_blend m)
{
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    switch (m) {
    case NEOX_BLEND_CLEAR:    glBlendFuncSeparate(GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO); break;
    case NEOX_BLEND_SRC:      glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO); break;
    case NEOX_BLEND_DST:      glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ZERO, GL_ONE); break;
    case NEOX_BLEND_DST_OVER: glBlendFuncSeparate(GL_ONE_MINUS_DST_ALPHA, GL_ONE,
                                                  GL_ONE_MINUS_DST_ALPHA, GL_ONE); break;
    case NEOX_BLEND_SRC_IN:   glBlendFuncSeparate(GL_DST_ALPHA, GL_ZERO,
                                                  GL_DST_ALPHA, GL_ZERO); break;
    case NEOX_BLEND_DST_IN:   glBlendFuncSeparate(GL_ZERO, GL_SRC_ALPHA,
                                                  GL_ZERO, GL_SRC_ALPHA); break;
    case NEOX_BLEND_SRC_OUT:  glBlendFuncSeparate(GL_ONE_MINUS_DST_ALPHA, GL_ZERO,
                                                  GL_ONE_MINUS_DST_ALPHA, GL_ZERO); break;
    case NEOX_BLEND_DST_OUT:  glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA,
                                                  GL_ZERO, GL_ONE_MINUS_SRC_ALPHA); break;
    case NEOX_BLEND_SRC_ATOP: glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                                  GL_DST_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
    case NEOX_BLEND_DST_ATOP: glBlendFuncSeparate(GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA,
                                                  GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA); break;
    case NEOX_BLEND_XOR:      glBlendFuncSeparate(GL_ONE_MINUS_DST_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                                  GL_ONE_MINUS_DST_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
    case NEOX_BLEND_PLUS:     glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE); break;
    case NEOX_BLEND_SRC_OVER:
    default:                  glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                                                  GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
    }
}

/* ------------------------------------------------------------------ 资源池 */

static void gl_rt_destroy(nx_gl_rt *rt)
{
    if (rt->tex) glDeleteTextures(1, &rt->tex);
    if (rt->srb) glDeleteRenderbuffers(1, &rt->srb);
    if (rt->fbo) glDeleteFramebuffers(1, &rt->fbo);
    memset(rt, 0, sizeof(*rt));
}

/* 获取渲染目标（优先复用空闲同尺寸项）；is_layer 区分层与掩码纹理池。
 * 池数组固定容量、init 时一次分配、永不扩容——池项指针被 layers/clips
 * 跨调用持有（元素内嵌于数组），nx_array_grow 扩容会整体搬迁数组使这些
 * 指针悬垂（深层嵌套超过初始容量 8 时触发，ASan 实测 heap-use-after-free）。
 * 超容量显式返回 OUT_OF_MEMORY 而非悬垂。容量按验证后端定位取 1024
 * （结构体数组 ~64KB；GL 对象按需创建）。 */
#define NX_GL_RT_POOL_CAP 1024u

static neox_result gl_rt_pool_init(neox_context *ctx, nx_gl_dev *g)
{
    g->rts = (nx_gl_rt *)nx_alloc(ctx, NX_GL_RT_POOL_CAP * sizeof(nx_gl_rt), 16);
    if (!g->rts) return NEOX_ERROR_OUT_OF_MEMORY;
    memset(g->rts, 0, NX_GL_RT_POOL_CAP * sizeof(nx_gl_rt));
    g->caprts = NX_GL_RT_POOL_CAP;
    return NEOX_OK;
}

static neox_result gl_rt_acquire(neox_context *ctx, nx_gl_dev *g,
                                 uint32_t w, uint32_t h, int is_layer,
                                 nx_gl_rt **out)
{
    uint32_t i;
    nx_gl_rt *rt;
    GLenum fmt = is_layer ? GL_RGBA32F : GL_R32F;
    for (i = 0; i < g->nrts; i++) {
        rt = &g->rts[i];
        if (!rt->in_use && rt->is_layer == is_layer &&
            rt->w == w && rt->h == h) {
            rt->in_use = 1;
            *out = rt;
            return NEOX_OK;
        }
    }
    if (g->nrts >= g->caprts) {
        ctx->last_error = "GL render-target pool exhausted";
        return NEOX_ERROR_OUT_OF_MEMORY;
    }
    rt = &g->rts[g->nrts];
    memset(rt, 0, sizeof(*rt));
    rt->is_layer = is_layer;
    rt->w = w; rt->h = h;
    glGenTextures(1, &rt->tex);
    glBindTexture(GL_TEXTURE_2D, rt->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)fmt, (GLsizei)w, (GLsizei)h, 0,
                 is_layer ? GL_RGBA : GL_RED,
                 GL_FLOAT, NULL);
    glGenFramebuffers(1, &rt->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, rt->tex, 0);
    if (is_layer) {
        glGenRenderbuffers(1, &rt->srb);
        glBindRenderbuffer(GL_RENDERBUFFER, rt->srb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8,
                              (GLsizei)w, (GLsizei)h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, rt->srb);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        glGetError() != GL_NO_ERROR) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        gl_rt_destroy(rt);
        ctx->last_error = "GL framebuffer object incomplete";
        return NEOX_ERROR_BACKEND;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    rt->in_use = 1;
    g->nrts++;
    *out = rt;
    return NEOX_OK;
}

static void gl_rt_release(nx_gl_rt *rt) { rt->in_use = 0; }

/* ------------------------------------------------------------------ 纹理上传 */

static neox_result gl_tex_ensure(neox_context *ctx, nx_gl_dev *g,
                                 neox_device *dev, uint32_t idx)
{
    const nx_tex_slot *s = &((nx_tex_slot *)dev->textures.slots)[idx];
    nx_gl_texent *e;
    if (idx >= g->ntexs) {
        uint32_t old = g->ntexs;
        nx_gl_texent *ne = (nx_gl_texent *)nx_array_grow(ctx, g->texs,
                             sizeof(nx_gl_texent), &g->captexs, idx + 1);
        if (!ne) return NEOX_ERROR_OUT_OF_MEMORY;
        g->texs = ne;
        memset(&g->texs[old], 0, (size_t)(idx + 1 - old) * sizeof(nx_gl_texent));
        g->ntexs = idx + 1;
    }
    e = &g->texs[idx];
    if (e->id && e->version == s->version) return NEOX_OK;
    if (!e->id) glGenTextures(1, &e->id);
    glBindTexture(GL_TEXTURE_2D, e->id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* 槽位 px 已是内部统一表示（预乘线性 float，§5），RGBA32F 原样上传；
     * A8 的覆盖率在 .a 通道（glyph shader 采样 .a） */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, (GLsizei)s->width,
                 (GLsizei)s->height, 0, GL_RGBA, GL_FLOAT, s->px);
    e->version = s->version;
    return NEOX_OK;
}

/* ------------------------------------------------------------------ 设备初始化 / 销毁 */

static void gl_state_destroy(neox_context *ctx, nx_gl_dev *g)
{
    uint32_t i;
    if (g->ctx && g->gl_ready && gl_make_current(g)) {
        gl_prog_free(&g->p_fill);
        gl_prog_free(&g->p_clamp);
        gl_prog_free(&g->p_plusfill);
        gl_prog_free(&g->p_plusglyph);
        gl_prog_free(&g->p_manualfill);
        gl_prog_free(&g->p_manualglyph);
        gl_prog_free(&g->p_manualtex);
        gl_prog_free(&g->p_tex);
        gl_prog_free(&g->p_glyph);
        gl_prog_free(&g->p_comp);
        gl_prog_free(&g->p_blit);
        gl_prog_free(&g->p_present);
        if (g->vao) glDeleteVertexArrays(1, &g->vao);
        if (g->vbo) glDeleteBuffers(1, &g->vbo);
        if (g->snap_tex) glDeleteTextures(1, &g->snap_tex);
        for (i = 0; i < g->nrts; i++) gl_rt_destroy(&g->rts[i]);
        for (i = 0; i < g->ntexs; i++)
            if (g->texs[i].id) glDeleteTextures(1, &g->texs[i].id);
        /* 表面注册表中仍存活的表面。窗口的 EGLSurface 不随 GL 上下文一起没，
         * 必须先拆掉。颜色附件随后随上下文销毁。 */
        for (i = 0; i < g->nsurfs; i++) {
            neox_gl_surface *s = g->surfs[i];
            if (s->win_egl) nx_gl_ctx_window_destroy(g->ctx, s->win_egl);
            if (s->presented)
                nx_free(ctx, s->presented, (size_t)s->w * s->h * 4, 16);
            nx_free(ctx, s, sizeof(neox_gl_surface), 16);
        }
    }
    if (g->ctx) {
        nx_gl_ctx_destroy(g->ctx);
        g->ctx = NULL;
    }
    if (g->mask) nx_free(ctx, g->mask, (size_t)g->mask_w * g->mask_h * 4, 16);
    if (g->premul) nx_free(ctx, g->premul, (size_t)g->premul_w * g->premul_h * 16, 16);
    if (g->rts) nx_free(ctx, g->rts, (size_t)g->caprts * sizeof(nx_gl_rt), 16);
    if (g->texs) nx_free(ctx, g->texs, (size_t)g->captexs * sizeof(nx_gl_texent), 16);
    if (g->surfs) nx_free(ctx, g->surfs, (size_t)g->capsurfs * sizeof(void *), 16);
    nx_free(ctx, g, sizeof(nx_gl_dev), 16);
}

/* 探测 RGBA32F + 模板附件可渲染（本后端正确性前提；Core Profile 规定可混合） */
static int gl_probe_float_fbo(void)
{
    GLuint tex, srb, fbo;
    GLint ok;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 4, 4, 0, GL_RGBA, GL_FLOAT, NULL);
    glGenRenderbuffers(1, &srb);
    glBindRenderbuffer(GL_RENDERBUFFER, srb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, 4, 4);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, srb);
    ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
         glGetError() == GL_NO_ERROR;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &srb);
    glDeleteTextures(1, &tex);
    return ok;
}

/* 链接一个写出 index=1 的最小程序。查询值不够：有的驱动报了 dual-source
 * 上限，着色器仍链不上。失败不是设备错误，只是关掉 dual 路径。 */
static int gl_probe_dual(void)
{
    const char *vs_src =
        "#version 410 core\n"
        "void main(void) { gl_Position = vec4(0.0); }\n";
    const char *fs_src =
        "#version 410 core\n"
        "layout(location = 0, index = 0) out vec4 a;\n"
        "layout(location = 0, index = 1) out vec4 b;\n"
        "void main(void) { a = vec4(1.0); b = vec4(0.0, 0.0, 0.0, 1.0); }\n";
    const char *vss = vs_src, *fss = fs_src;
    GLint n = 0, ok = GL_FALSE;
    GLuint vs, fs, p;
    glGetError();
    glGetIntegerv(GL_MAX_DUAL_SOURCE_DRAW_BUFFERS, &n);
    if (glGetError() != GL_NO_ERROR || n < 1) return 0;
    vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vss, NULL);
    glCompileShader(vs);
    fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fss, NULL);
    glCompileShader(fs);
    p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glDeleteProgram(p);
    glGetError();
    return ok == GL_TRUE;
}

static neox_result gl_device_init(neox_device *dev, const void *platform,
                                  size_t plat_size)
{
    neox_context *ctx = dev->ctx;
    nx_gl_dev *g;
    neox_gl_device_desc gld;
    GLint major = 0, minor = 0;
    GLuint vs;
    neox_result r;

    r = nx_device_init_tables(dev);
    if (r != NEOX_OK) return r;

    /* platform 版本协商（neox_device_desc 约定，neox.h）：
     * platform==NULL ⇔ plat_size==0（缺省：自建离屏上下文）；
     * platform!=NULL 时 plat_size 须至少覆盖首字段 struct_size，
     * 且声明的 struct_size 不得超过 plat_size；plat_size 大于本版
     * 结构体时按前向兼容处理（只用 sizeof(neox_gl_device_desc) 前缀，
     * 其余字段未来版本定义）。 */
    memset(&gld, 0, sizeof(gld));
    if (platform) {
        const neox_gl_device_desc *d = (const neox_gl_device_desc *)platform;
        uint32_t hdr_size;
        if (plat_size < sizeof(uint32_t))
            NX_ERR(ctx, "platform_size too small for struct_size",
                   NEOX_ERROR_INVALID_ARGUMENT);
        hdr_size = *(const uint32_t *)platform; /* 首字段 struct_size */
        if (hdr_size > plat_size)
            NX_ERR(ctx, "struct_size exceeds platform_size",
                   NEOX_ERROR_INVALID_ARGUMENT);
        if (plat_size < sizeof(neox_gl_device_desc))
            NX_ERR(ctx, "platform_size smaller than neox_gl_device_desc",
                   NEOX_ERROR_INVALID_ARGUMENT);
        if (d->struct_size != (uint32_t)sizeof(neox_gl_device_desc))
            NX_ERR(ctx, "neox_gl_device_desc version mismatch",
                   NEOX_ERROR_INVALID_ARGUMENT);
        if (d->magic != NEOX_GL_DESC_MAGIC)
            NX_ERR(ctx, "neox_gl_device_desc magic mismatch",
                   NEOX_ERROR_INVALID_ARGUMENT);
        gld = *d;
    } else if (plat_size != 0) {
        NX_ERR(ctx, "platform is NULL but platform_size is nonzero",
               NEOX_ERROR_INVALID_ARGUMENT);
    }

    g = (nx_gl_dev *)nx_alloc(ctx, sizeof(nx_gl_dev), 16);
    if (!g) NX_ERR(ctx, "GL backend state allocation failed",
                   NEOX_ERROR_OUT_OF_MEMORY);
    memset(g, 0, sizeof(*g));
    g->ctx = nx_gl_ctx_create(platform ? &gld : NULL);
    if (!g->ctx) {
        const char *e = nx_gl_ctx_last_error();
        gl_state_destroy(ctx, g);
        NX_ERR(ctx, e ? e : "GL context creation failed", NEOX_ERROR_BACKEND);
    }
    if (!gl_make_current(g)) {
        gl_state_destroy(ctx, g);
        NX_ERR(ctx, "GL: could not make context current", NEOX_ERROR_BACKEND);
    }
    if (!nx_gl_load(g->ctx)) {
        const char *e = nx_gl_load_last_error();
        gl_state_destroy(ctx, g);
        NX_ERR(ctx, e ? e : "GL entry point load failed", NEOX_ERROR_BACKEND);
    }
    g->gl_ready = 1;
    /* 版本确认（GL 3.0+ 提供 GL_MAJOR/MINOR_VERSION） */
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (major < 4 || (major == 4 && minor < 1)) {
        gl_state_destroy(ctx, g);
        NX_ERR(ctx, "OpenGL 4.1 Core not available", NEOX_ERROR_BACKEND);
    }
    if (!gl_probe_float_fbo()) {
        gl_state_destroy(ctx, g);
        NX_ERR(ctx, "GL_RGBA32F framebuffer not renderable",
               NEOX_ERROR_BACKEND);
    }
    g->dual_ok = gl_probe_dual();
    if (getenv("NEOX_GL_FORCE_NO_DUAL")) g->dual_ok = 0;
    if (getenv("NEOX_GL_LOG")) {
        const GLubyte *rend = glGetString(GL_RENDERER);
        const GLubyte *ver = glGetString(GL_VERSION);
        fprintf(stderr, "neox gl: kind=%s renderer=%s version=%s dual_source=%d\n",
                nx_gl_ctx_kind(g->ctx),
                rend ? (const char *)rend : "?",
                ver ? (const char *)ver : "?",
                g->dual_ok);
    }

    /* 共享顶点着色器 + 片段程序。fill/tex/glyph 按 dual_ok 选输出声明。 */
    vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &NX_GLSL_VS, NULL);
    glCompileShader(vs);
    {
        GLint ok = GL_FALSE;
        glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
        if (ok != GL_TRUE) {
            glDeleteShader(vs);
            gl_state_destroy(ctx, g);
            NX_ERR(ctx, "GL vertex shader compile failed", NEOX_ERROR_BACKEND);
        }
    }
#define NX_BUILD_MODE(prog, src, what, dual) do { \
        r = gl_prog_build(ctx, vs, src, what, &(prog), (dual)); \
        if (r != NEOX_OK) { glDeleteShader(vs); gl_state_destroy(ctx, g); return r; } \
    } while (0)
#define NX_BUILD(prog, src, what) NX_BUILD_MODE(prog, src, what, -1)
#define NX_BUILD_FS(prog, src, what) NX_BUILD_MODE(prog, src, what, g->dual_ok)
    NX_BUILD_FS(g->p_fill, NX_GLSL_FS_FILL, "fill");
    NX_BUILD(g->p_clamp, NX_GLSL_FS_CLAMP, "clamp");
    NX_BUILD(g->p_plusfill, NX_GLSL_FS_PLUSFILL, "plusfill");
    NX_BUILD(g->p_plusglyph, NX_GLSL_FS_PLUSGLYPH, "plusglyph");
    NX_BUILD(g->p_manualfill, NX_GLSL_FS_MANUALFILL, "manualfill");
    NX_BUILD(g->p_manualglyph, NX_GLSL_FS_MANUALGLYPH, "manualglyph");
    NX_BUILD(g->p_manualtex, NX_GLSL_FS_MANUALTEX, "manualtex");
    NX_BUILD_FS(g->p_tex, NX_GLSL_FS_TEX, "tex");
    NX_BUILD_FS(g->p_glyph, NX_GLSL_FS_GLYPH, "glyph");
    NX_BUILD(g->p_comp, NX_GLSL_FS_COMP, "comp");
    NX_BUILD(g->p_present, NX_GLSL_FS_PRESENT, "present");
    NX_BUILD(g->p_blit, NX_GLSL_FS_BLIT, "blit");
#undef NX_BUILD_FS
#undef NX_BUILD
#undef NX_BUILD_MODE
    glDeleteShader(vs);
    if (glGetError() != GL_NO_ERROR) {
        gl_state_destroy(ctx, g);
        NX_ERR(ctx, "GL program setup failed", NEOX_ERROR_BACKEND);
    }

    r = gl_rt_pool_init(ctx, g);
    if (r != NEOX_OK) {
        gl_state_destroy(ctx, g);
        NX_ERR(ctx, "GL render-target pool allocation failed",
               NEOX_ERROR_OUT_OF_MEMORY);
    }
    glGenVertexArrays(1, &g->vao);
    glGenBuffers(1, &g->vbo);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, (GLint *)&major); /* 复用变量读 max tex */
    g->max_tex = (uint32_t)major;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);          /* 浮点目标上本应是空操作；关掉以免量化抖动 */
    glDisable(GL_FRAMEBUFFER_SRGB); /* sRGB 编码由 resolve shader 手工完成 */
    dev->bstate = g;
    return NEOX_OK;
}

static void gl_device_shutdown(neox_device *dev)
{
    if (dev->bstate) {
        gl_state_destroy(dev->ctx, (nx_gl_dev *)dev->bstate);
        dev->bstate = NULL;
    }
}

static void gl_get_caps(neox_device *dev, neox_caps *out_caps)
{
    nx_gl_dev *g = (nx_gl_dev *)dev->bstate;
    /* 全部 Phase 1 语义以全精度执行：层附件 RGBA32F，混合全程浮点 */
    out_caps->features = NEOX_CAP_FLOAT_BLEND;
    out_caps->max_texture_size = g ? g->max_tex : 0;
    out_caps->backend_memory_limit = 0;
}

/* ------------------------------------------------------------------ begin_target
 * 校验表面归属、确定帧尺寸、装载基底（clear = 清屏色；否则快照表面内容，
 * 与 Soft 同样在 frame_begin 期完成，基底统一为预乘线性 float） */
static neox_result gl_begin_target(neox_frame *fr,
                                   const neox_target_desc *target)
{
    neox_device *dev = fr->dev;
    neox_context *ctx = dev->ctx;
    nx_gl_dev *g = (nx_gl_dev *)dev->bstate;
    neox_gl_surface *surf = (neox_gl_surface *)target->surface;
    size_t npix, i;
    if (!surf)
        NX_ERR(ctx, "invalid GL target (surface is NULL)",
               NEOX_ERROR_INVALID_ARGUMENT);
    if (surf->dev != dev)
        NX_ERR(ctx, "surface belongs to another device",
               NEOX_ERROR_OWNERSHIP_VIOLATION);
    if (!gl_make_current(g))
        NX_ERR(ctx, "GL: could not make context current", NEOX_ERROR_BACKEND);
    fr->surface = surf;
    fr->width = surf->w;
    fr->height = surf->h;
    npix = (size_t)fr->width * fr->height;
    fr->base = (float *)nx_alloc(ctx, npix * 4 * sizeof(float), 16);
    if (!fr->base) NX_ERR(ctx, "frame buffer allocation failed",
                          NEOX_ERROR_OUT_OF_MEMORY);
    if (target->clear) {
        float p[4];
        nx_paint_to_premul_linear(*target->clear, p);
        for (i = 0; i < npix; i++)
            memcpy(&fr->base[i * 4], p, sizeof(p));
    } else {
        /* 快照执行前表面内容（目标在执行成功前不被写入，§10.1） */
        uint8_t *snap = (uint8_t *)nx_alloc(ctx, npix * 4, 16);
        if (!snap) {
            nx_free(ctx, fr->base, npix * 4 * sizeof(float), 16);
            fr->base = NULL;
            NX_ERR(ctx, "surface snapshot allocation failed",
                   NEOX_ERROR_OUT_OF_MEMORY);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, surf->fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, (GLsizei)surf->w, (GLsizei)surf->h,
                     GL_RGBA, GL_UNSIGNED_BYTE, snap);
        if (glGetError() != GL_NO_ERROR) {
            nx_free(ctx, snap, npix * 4, 16);
            nx_free(ctx, fr->base, npix * 4 * sizeof(float), 16);
            fr->base = NULL;
            NX_ERR(ctx, "GL surface readback failed", NEOX_ERROR_BACKEND);
        }
        nx_pixels_to_premul_linear(snap, (size_t)fr->width * 4,
                                   fr->width, fr->height, fr->base);
        nx_free(ctx, snap, npix * 4, 16);
    }
    return NEOX_OK;
}

/* ------------------------------------------------------------------ 绘制基元 */

typedef struct gl_ex_state {
    neox_affine ctm;
    float       opacity;
    neox_blend  blend;
    const float *clip;      /* 裁剪掩码（命令拥有，借用）；NULL = 无 */
    int32_t     layer;      /* 层池槽索引 */
} gl_ex_state;

/* PLUS 混合后把层内容 clamp 到 [0,1]（§6：PLUS 逐通道 clamp；GL float
 * 附件混合不 clamp，此处以整层拷贝 pass 复刻 Soft 的逐步 clamp 语义） */
static neox_result gl_clamp_layer(neox_context *ctx, nx_gl_dev *g,
                                  const nx_gl_rt *layer, uint32_t w, uint32_t h)
{
    uint32_t SSW = w * NEOX_GL_SS, SSH = h * NEOX_GL_SS;
    nx_gl_rt *tmp;
    neox_result r = gl_rt_acquire(ctx, g, SSW, SSH, 1, &tmp);
    if (r != NEOX_OK) return r;
    glBindFramebuffer(GL_FRAMEBUFFER, tmp->fbo);
    glViewport(0, 0, (GLsizei)SSW, (GLsizei)SSH);
    gl_use(&g->p_clamp, 2.0f / (float)SSW, 2.0f / (float)SSH);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, layer->tex);
    glUniform1i(g->p_clamp.u_tex, 0);
    glDisable(GL_BLEND);
    gl_draw_full(g, &g->p_clamp, (float)SSW, (float)SSH,
                 2.0f / (float)SSW, 2.0f / (float)SSH);
    glBindFramebuffer(GL_FRAMEBUFFER, layer->fbo);
    glViewport(0, 0, (GLsizei)SSW, (GLsizei)SSH);
    gl_use(&g->p_blit, 2.0f / (float)SSW, 2.0f / (float)SSH);
    glBindTexture(GL_TEXTURE_2D, tmp->tex);
    glUniform1i(g->p_blit.u_tex, 0);
    glUniform1f(g->p_blit.u_ss, (GLfloat)NEOX_GL_SS);
    gl_draw_full(g, &g->p_blit, (float)SSW, (float)SSH,
                 2.0f / (float)SSW, 2.0f / (float)SSH);
    gl_rt_release(tmp);
    if (glGetError() != GL_NO_ERROR) {
        ctx->last_error = "GL clamp pass failed";
        return NEOX_ERROR_BACKEND;
    }
    return NEOX_OK;
}

/* 路径填充：覆盖率掩码（共享 geom.c 光栅化，与 Soft 逐值一致）→ R32F 纹理
 * → cover pass 调制 paint 并按当前混合模式合成。按设备像素恒定（SS=1），
 * 因此混合的源/目标序列与 Soft 逐位相同（实心内部与边界均精确一致）。
 *
 * 说明：本后端曾实现 stencil-then-cover（包围盒外扇心三角扇 + 双面
 * INCR_WRAP/DECR_WRAP/INVERT 模板，几何输入与 Soft 逐点一致），在简单
 * 几何下结果正确；但 §9 容差在"半透明边缘相互叠乘"（如 0.5 alpha 图形
 * 边缘上方再叠 0.7 alpha 图形）时不可达——Soft 按设备像素解析覆盖率后
 * 单次混合，而超采样模板按子像素混合，两种覆盖率量化在多边缘相互作用
 * 处差可超过 32/255。按"正确性优先"改用共享覆盖率掩码方案（stencil 与
 * CLEAR 特判随之移除，见提交报告）。 */
static neox_result gl_fill_path(neox_device *dev, nx_gl_dev *g,
                                const nx_path_slot *ps, neox_affine ctm,
                                const neox_color *color, neox_blend blend,
                                const nx_gl_rt *layer, uint32_t w, uint32_t h)
{
    neox_context *ctx = dev->ctx;
    uint32_t SSW = w * NEOX_GL_SS, SSH = h * NEOX_GL_SS;
    nx_flat flat;                  /* 包围盒计算仍需 flatten（共享 geom.c） */
    float minx, miny, maxx, maxy, paint[4];
    nx_gl_rt *mrt;
    uint32_t i;
    neox_result r;

    r = nx_flatten_path(ctx, ps, ctm, &flat);
    if (r != NEOX_OK) return r;
    if (flat.npts / 2 < 3 || flat.npolys == 0) {
        nx_flat_free(ctx, &flat);
        return NEOX_OK; /* 退化路径：无输出（§4.1） */
    }
    minx = maxx = flat.pts[0];
    miny = maxy = flat.pts[1];
    for (i = 1; i < flat.npts / 2; i++) {
        float x = flat.pts[i * 2], y = flat.pts[i * 2 + 1];
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    nx_flat_free(ctx, &flat);
    nx_paint_to_premul_linear(*color, paint);

    nx_gl_rt *prt;
    uint32_t npx = w * h, k;
    /* 覆盖率掩码：与 Soft 完全相同的光栅化（急切分配，失败按 §10.1 传播） */
    if (!g->mask || g->mask_w != w || g->mask_h != h) {
        if (g->mask) nx_free(ctx, g->mask, (size_t)g->mask_w * g->mask_h * 4, 16);
        g->mask = (float *)nx_alloc(ctx, (size_t)w * h * 4, 16);
        if (!g->mask) {
            g->mask_w = g->mask_h = 0;
            return NEOX_ERROR_OUT_OF_MEMORY;
        }
        g->mask_w = w;
        g->mask_h = h;
    }
    if (!g->premul || g->premul_w != w || g->premul_h != h) {
        if (g->premul)
            nx_free(ctx, g->premul, (size_t)g->premul_w * g->premul_h * 16, 16);
        g->premul = (float *)nx_alloc(ctx, (size_t)w * h * 16, 16);
        if (!g->premul) {
            g->premul_w = g->premul_h = 0;
            return NEOX_ERROR_OUT_OF_MEMORY;
        }
        g->premul_w = w;
        g->premul_h = h;
    }
    r = nx_rasterize_path(ctx, ps, ctm, w, h, g->mask);
    if (r != NEOX_OK) return r;
    /* CPU 预乘：与 Soft 的 s[c] = base[c] * cov 同一表达式、同一位舍入，
     * 消除 GPU 着色器乘法与 libm/libm 之间的 1 ulp 差在量化 ties 处的表现 */
    for (k = 0; k < npx; k++) {
        float cov = g->mask[k];
        g->premul[k * 4 + 0] = paint[0] * cov;
        g->premul[k * 4 + 1] = paint[1] * cov;
        g->premul[k * 4 + 2] = paint[2] * cov;
        g->premul[k * 4 + 3] = paint[3] * cov;
    }

    r = gl_rt_acquire(ctx, g, w, h, 0, &mrt);
    if (r != NEOX_OK) return r;
    r = gl_rt_acquire(ctx, g, w, h, 1, &prt);
    if (r != NEOX_OK) {
        gl_rt_release(mrt);
        return r;
    }
    glBindTexture(GL_TEXTURE_2D, mrt->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h,
                    GL_RED, GL_FLOAT, g->mask);
    glBindTexture(GL_TEXTURE_2D, prt->tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h,
                    GL_RGBA, GL_FLOAT, g->premul);

    glBindFramebuffer(GL_FRAMEBUFFER, layer->fbo);
    glViewport(0, 0, (GLsizei)SSW, (GLsizei)SSH);
    gl_use(&g->p_fill, 2.0f / (float)SSW, 2.0f / (float)SSH);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mrt->tex);
    glUniform1i(g->p_fill.u_tex, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, prt->tex);
    glUniform1i(g->p_fill.u_premul, 1);
    glActiveTexture(GL_TEXTURE0);
    glUniform1f(g->p_fill.u_ss, (GLfloat)NEOX_GL_SS);
    glUniform1i(g->p_fill.u_mode, (GLint)blend);
    glUniform1f(g->p_fill.u_paint_a, paint[3]);
    {
        /* cover 四边形扩到整像素：覆盖率按子扫描线归属像素行，形状包围盒
         * 浮点边界外的像素行/列仍可能有覆盖率（像素中心可在包围盒外） */
        float q[12];
        float x0 = floorf(minx) * NEOX_GL_SS, y0 = floorf(miny) * NEOX_GL_SS;
        float x1 = ceilf(maxx) * NEOX_GL_SS, y1 = ceilf(maxy) * NEOX_GL_SS;
        q[0] = x0; q[1]  = y0;  q[2]  = x1; q[3]  = y0;  q[4]  = x1; q[5]  = y1;
        q[6] = x0; q[7]  = y0;  q[8]  = x1; q[9]  = y1;  q[10] = x0; q[11] = y1;
        {
            int manual = !g->dual_ok && gl_mode_needs_dual(blend);
            if (blend == NEOX_BLEND_PLUS || manual) {
            /* ping-pong：PLUS 或无 dual-source 的四模式。读 dst 写入临时层再拷回。 */
            nx_gl_rt *tmp;
            const nx_gl_prog *prog = manual ? &g->p_manualfill : &g->p_plusfill;
            r = gl_rt_acquire(ctx, g, SSW, SSH, 1, &tmp);
            if (r != NEOX_OK) {
                gl_rt_release(mrt);
                gl_rt_release(prt);
                return r;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, tmp->fbo);
            glViewport(0, 0, (GLsizei)SSW, (GLsizei)SSH);
            gl_use(prog, 2.0f / (float)SSW, 2.0f / (float)SSH);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, prt->tex);
            glUniform1i(prog->u_premul, 0);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, mrt->tex);
            glUniform1i(prog->u_mask, 1);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, layer->tex);
            glUniform1i(prog->u_layer, 2);
            glActiveTexture(GL_TEXTURE0);
            glUniform1f(prog->u_ss, (GLfloat)NEOX_GL_SS);
            if (manual) glUniform1i(prog->u_mode, (GLint)blend);
            glDisable(GL_BLEND);
            gl_draw(g, q, 6);
            /* 拷回（限 bbox 像素区域，临时层在包围盒外是池内陈旧内容） */
            glEnable(GL_SCISSOR_TEST);
            glScissor((GLsizei)floorf(minx) * NEOX_GL_SS,
                      (GLsizei)floorf(miny) * NEOX_GL_SS,
                      (GLsizei)(ceilf(maxx) - floorf(minx)) * NEOX_GL_SS,
                      (GLsizei)(ceilf(maxy) - floorf(miny)) * NEOX_GL_SS);
            glBindFramebuffer(GL_FRAMEBUFFER, layer->fbo);
            glViewport(0, 0, (GLsizei)SSW, (GLsizei)SSH);
            gl_use(&g->p_blit, 2.0f / (float)SSW, 2.0f / (float)SSH);
            glBindTexture(GL_TEXTURE_2D, tmp->tex);
            glUniform1i(g->p_blit.u_tex, 0);
            glUniform1f(g->p_blit.u_ss, (GLfloat)NEOX_GL_SS);
            gl_draw_full(g, &g->p_blit, (float)SSW, (float)SSH,
                         2.0f / (float)SSW, 2.0f / (float)SSH);
            glDisable(GL_SCISSOR_TEST);
            gl_rt_release(tmp);
            if (glGetError() != GL_NO_ERROR) {
                gl_rt_release(mrt);
                gl_rt_release(prt);
                ctx->last_error = manual ? "GL manual fill failed"
                                         : "GL PLUS fill failed";
                return NEOX_ERROR_BACKEND;
            }
            } else {
                gl_blend_cov(blend);
                gl_draw(g, q, 6);
            }
        }
    }
    gl_rt_release(mrt);
    gl_rt_release(prt);
    return NEOX_OK;
}

/* draw_texture：目标四角经 CTM 映射；采样在 shader 内按设备像素中心完成（§8） */
static neox_result gl_draw_tex(neox_device *dev, nx_gl_dev *g,
                               const nx_tex_slot *tx, const nx_cmd *cmd,
                               neox_affine ctm, neox_blend blend,
                               const nx_gl_rt *layer, uint32_t w, uint32_t h)
{
    uint32_t SSW = w * NEOX_GL_SS, SSH = h * NEOX_GL_SS;
    float xc[4], yc[4];
    neox_affine inv;
    float m3[9];
    if (cmd->dst.w == 0.0f || cmd->dst.h == 0.0f) return NEOX_OK;
    if (nx_affine_det(ctm) == 0.0f) return NEOX_OK; /* §3 退化变换：无输出 */
    if (!nx_affine_invert(ctm, &inv)) return NEOX_OK;
    /* 环序角点：(x,y) (x+w,y) (x+w,y+h) (x,y+h) */
    nx_affine_apply(ctm, cmd->dst.x, cmd->dst.y, &xc[0], &yc[0]);
    nx_affine_apply(ctm, cmd->dst.x + cmd->dst.w, cmd->dst.y, &xc[1], &yc[1]);
    nx_affine_apply(ctm, cmd->dst.x + cmd->dst.w, cmd->dst.y + cmd->dst.h,
                    &xc[2], &yc[2]);
    nx_affine_apply(ctm, cmd->dst.x, cmd->dst.y + cmd->dst.h, &xc[3], &yc[3]);
    /* 两个三角形：(0,1,2) (0,2,3)；几何外扩 0.6px，包含判定在 shader 内
     * 按设备像素中心完成（与参照后端逐位对齐）；负宽高目标矩形镜像由
     * tu/tv 反转自然产生（§8） */
    {
        float q[12], ix[4], iy[4];
        int k;
        gl_inflate_quad(xc, yc, 0.6f, ix, iy);
        for (k = 0; k < 4; k++) {
            ix[k] *= NEOX_GL_SS;
            iy[k] *= NEOX_GL_SS;
        }
        q[0]  = ix[0]; q[1]  = iy[0];
        q[2]  = ix[1]; q[3]  = iy[1];
        q[4]  = ix[2]; q[5]  = iy[2];
        q[6]  = ix[0]; q[7]  = iy[0];
        q[8]  = ix[2]; q[9]  = iy[2];
        q[10] = ix[3]; q[11] = iy[3];
        glBindFramebuffer(GL_FRAMEBUFFER, layer->fbo);
        glViewport(0, 0, (GLsizei)SSW, (GLsizei)SSH);
        /* 上传/获取纹理对象（槽位 px 已是内部统一表示） */
        {
            uint32_t idx = (uint32_t)(tx - (const nx_tex_slot *)dev->textures.slots);
            neox_result r = gl_tex_ensure(dev->ctx, g, dev, idx);
            if (r != NEOX_OK) return r;
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g->texs[idx].id);
        }
    gl_use(&g->p_tex, 2.0f / (float)SSW, 2.0f / (float)SSH);
    glUniform1i(g->p_tex.u_tex, 0);
    glUniform2i(g->p_tex.u_tex_size, (GLint)tx->width, (GLint)tx->height);
    glUniform1i(g->p_tex.u_linear, cmd->filter == NEOX_FILTER_LINEAR ? 1 : 0);
    /* mat3 列主序：设备 → 局部 */
    m3[0] = inv.a;  m3[1] = inv.b;  m3[2] = 0.0f;
    m3[3] = inv.c;  m3[4] = inv.d;  m3[5] = 0.0f;
    m3[6] = inv.tx; m3[7] = inv.ty; m3[8] = 1.0f;
    glUniformMatrix3fv(g->p_tex.u_inv, 1, GL_FALSE, m3);
    glUniform4f(g->p_tex.u_src, cmd->src.x, cmd->src.y,
                cmd->src.w, cmd->src.h);
    glUniform4f(g->p_tex.u_dst, cmd->dst.x, cmd->dst.y,
                cmd->dst.w, cmd->dst.h);
    glUniform1f(g->p_tex.u_ss, (GLfloat)NEOX_GL_SS);
    glUniform1f(g->p_tex.u_opacity, cmd->tex_opacity);
    glUniform1i(g->p_tex.u_mode, (GLint)blend);
    if (!g->dual_ok && gl_mode_needs_dual(blend)) {
        /* 无 dual-source：AABB 矩形覆盖 scissor，矩形外着色器写回 dst。 */
        float bx0 = ix[0], by0 = iy[0], bx1 = ix[0], by1 = iy[0];
        float rq[12], rx0, ry0, rx1, ry1;
        nx_gl_rt *tmp;
        neox_result tr;
        int ti;
        GLuint tex_id = 0;
        for (ti = 1; ti < 4; ti++) {
            if (ix[ti] < bx0) bx0 = ix[ti];
            if (ix[ti] > bx1) bx1 = ix[ti];
            if (iy[ti] < by0) by0 = iy[ti];
            if (iy[ti] > by1) by1 = iy[ti];
        }
        rx0 = floorf(bx0); ry0 = floorf(by0);
        rx1 = ceilf(bx1);  ry1 = ceilf(by1);
        rq[0] = rx0; rq[1] = ry0; rq[2] = rx1; rq[3] = ry0; rq[4] = rx1; rq[5] = ry1;
        rq[6] = rx0; rq[7] = ry0; rq[8] = rx1; rq[9] = ry1; rq[10] = rx0; rq[11] = ry1;
        tr = gl_rt_acquire(dev->ctx, g, SSW, SSH, 1, &tmp);
        if (tr != NEOX_OK) return tr;
        tex_id = (GLuint)g->texs[(uint32_t)(tx - (const nx_tex_slot *)dev->textures.slots)].id;
        glBindFramebuffer(GL_FRAMEBUFFER, tmp->fbo);
        glViewport(0, 0, (GLsizei)SSW, (GLsizei)SSH);
        gl_use(&g->p_manualtex, 2.0f / (float)SSW, 2.0f / (float)SSH);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_id);
        glUniform1i(g->p_manualtex.u_tex, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, layer->tex);
        glUniform1i(g->p_manualtex.u_layer, 1);
        glActiveTexture(GL_TEXTURE0);
        glUniform2i(g->p_manualtex.u_tex_size, (GLint)tx->width, (GLint)tx->height);
        glUniform1i(g->p_manualtex.u_linear, cmd->filter == NEOX_FILTER_LINEAR ? 1 : 0);
        glUniformMatrix3fv(g->p_manualtex.u_inv, 1, GL_FALSE, m3);
        glUniform4f(g->p_manualtex.u_src, cmd->src.x, cmd->src.y, cmd->src.w, cmd->src.h);
        glUniform4f(g->p_manualtex.u_dst, cmd->dst.x, cmd->dst.y, cmd->dst.w, cmd->dst.h);
        glUniform1f(g->p_manualtex.u_ss, (GLfloat)NEOX_GL_SS);
        glUniform1f(g->p_manualtex.u_opacity, cmd->tex_opacity);
        glUniform1i(g->p_manualtex.u_mode, (GLint)blend);
        glDisable(GL_BLEND);
        gl_draw(g, rq, 6);
        glEnable(GL_SCISSOR_TEST);
        glScissor((GLint)rx0, (GLint)ry0, (GLsizei)(rx1 - rx0), (GLsizei)(ry1 - ry0));
        glBindFramebuffer(GL_FRAMEBUFFER, layer->fbo);
        glViewport(0, 0, (GLsizei)SSW, (GLsizei)SSH);
        gl_use(&g->p_blit, 2.0f / (float)SSW, 2.0f / (float)SSH);
        glBindTexture(GL_TEXTURE_2D, tmp->tex);
        glUniform1i(g->p_blit.u_tex, 0);
        glUniform1f(g->p_blit.u_ss, (GLfloat)NEOX_GL_SS);
        gl_draw_full(g, &g->p_blit, (float)SSW, (float)SSH,
                     2.0f / (float)SSW, 2.0f / (float)SSH);
        glDisable(GL_SCISSOR_TEST);
        gl_rt_release(tmp);
        if (glGetError() != GL_NO_ERROR) {
            dev->ctx->last_error = "GL manual texture draw failed";
            return NEOX_ERROR_BACKEND;
        }
    } else {
        gl_blend_cov(blend);   /* PLUS: cov=1 折源 + 整层 clamp（下方） */
        gl_draw(g, q, 6);
    }
    }
    if (blend == NEOX_BLEND_PLUS)
        return gl_clamp_layer(dev->ctx, g, layer, w, h);
    return NEOX_OK;
}

/* draw_glyphs：每个字形一个四边形，A8 图集双线性遮罩 × paint（§5） */
static neox_result gl_draw_glyphs(neox_device *dev, nx_gl_dev *g,
                                  const nx_tex_slot *at, const nx_cmd *cmd,
                                  neox_affine ctm, neox_blend blend,
                                  const nx_gl_rt *layer, uint32_t w, uint32_t h)
{
    uint32_t SSW = w * NEOX_GL_SS, SSH = h * NEOX_GL_SS;
    neox_affine inv;
    float m3[9], paint[4];
    uint32_t gi, idx;
    neox_result r;
    if (nx_affine_det(ctm) == 0.0f) return NEOX_OK; /* §3 */
    if (!nx_affine_invert(ctm, &inv)) return NEOX_OK;
    nx_paint_to_premul_linear(cmd->color, paint);
    m3[0] = inv.a;  m3[1] = inv.b;  m3[2] = 0.0f;
    m3[3] = inv.c;  m3[4] = inv.d;  m3[5] = 0.0f;
    m3[6] = inv.tx; m3[7] = inv.ty; m3[8] = 1.0f;
    glBindFramebuffer(GL_FRAMEBUFFER, layer->fbo);
    glViewport(0, 0, (GLsizei)SSW, (GLsizei)SSH);
    idx = (uint32_t)(at - (const nx_tex_slot *)dev->textures.slots);
    r = gl_tex_ensure(dev->ctx, g, dev, idx);
    if (r != NEOX_OK) return r;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->texs[idx].id);
    gl_use(&g->p_glyph, 2.0f / (float)SSW, 2.0f / (float)SSH);
    glUniform1i(g->p_glyph.u_atlas, 0);
    glUniform4f(g->p_glyph.u_color, paint[0], paint[1], paint[2], paint[3]);
    glUniformMatrix3fv(g->p_glyph.u_inv, 1, GL_FALSE, m3);
    glUniform1f(g->p_glyph.u_ss, (GLfloat)NEOX_GL_SS);
    glUniform1i(g->p_glyph.u_mode, (GLint)blend);
    if (blend != NEOX_BLEND_PLUS && !(!g->dual_ok && gl_mode_needs_dual(blend)))
        gl_blend_cov(blend);
    for (gi = 0; gi < cmd->glyph_count; gi++) {
        const neox_glyph *gl2 = &cmd->glyphs[gi];
        float gw = (float)gl2->sw, gh = (float)gl2->sh;
        float xc[4], yc[4], q[12], ix[4], iy[4];
        float gx0, gy0, gx1, gy1;
        int k;
        if (gw == 0.0f || gh == 0.0f) continue;
        /* 环序角点（同 gl_draw_tex） */
        nx_affine_apply(ctm, gl2->x, gl2->y, &xc[0], &yc[0]);
        nx_affine_apply(ctm, gl2->x + gw, gl2->y, &xc[1], &yc[1]);
        nx_affine_apply(ctm, gl2->x + gw, gl2->y + gh, &xc[2], &yc[2]);
        nx_affine_apply(ctm, gl2->x, gl2->y + gh, &xc[3], &yc[3]);
        gl_inflate_quad(xc, yc, 0.6f, ix, iy);
        gx0 = floorf(ix[0]); gy0 = floorf(iy[0]);
        gx1 = ceilf(ix[2]);  gy1 = ceilf(iy[2]);
        /* 保守 device bbox（inflate 后角点未必有序，取全角点范围） */
        {
            float bx0 = ix[0], by0 = iy[0], bx1 = ix[0], by1 = iy[0];
            for (k = 1; k < 4; k++) {
                if (ix[k] < bx0) bx0 = ix[k];
                if (ix[k] > bx1) bx1 = ix[k];
                if (iy[k] < by0) by0 = iy[k];
                if (iy[k] > by1) by1 = iy[k];
            }
            gx0 = floorf(bx0); gy0 = floorf(by0);
            gx1 = ceilf(bx1);  gy1 = ceilf(by1);
        }
        for (k = 0; k < 4; k++) {
            ix[k] *= NEOX_GL_SS;
            iy[k] *= NEOX_GL_SS;
        }
        q[0]  = ix[0]; q[1]  = iy[0];
        q[2]  = ix[1]; q[3]  = iy[1];
        q[4]  = ix[2]; q[5]  = iy[2];
        q[6]  = ix[0]; q[7]  = iy[0];
        q[8]  = ix[2]; q[9]  = iy[2];
        q[10] = ix[3]; q[11] = iy[3];
        if (blend == NEOX_BLEND_PLUS ||
            (!g->dual_ok && gl_mode_needs_dual(blend))) {
            /* ping-pong：PLUS，或无 dual-source 时的四模式。manual 画 AABB，
             * 字形外写回 dst，避免 scissor 角点拷到陈旧内容。 */
            nx_gl_rt *tmp;
            int manual = blend != NEOX_BLEND_PLUS;
            const nx_gl_prog *prog = manual ? &g->p_manualglyph : &g->p_plusglyph;
            float rq[12];
            const float *dv = q;
            r = gl_rt_acquire(dev->ctx, g, SSW, SSH, 1, &tmp);
            if (r != NEOX_OK) return r;
            if (manual) {
                float rx0 = gx0 * (float)NEOX_GL_SS, ry0 = gy0 * (float)NEOX_GL_SS;
                float rx1 = gx1 * (float)NEOX_GL_SS, ry1 = gy1 * (float)NEOX_GL_SS;
                rq[0] = rx0; rq[1] = ry0; rq[2] = rx1; rq[3] = ry0; rq[4] = rx1; rq[5] = ry1;
                rq[6] = rx0; rq[7] = ry0; rq[8] = rx1; rq[9] = ry1; rq[10] = rx0; rq[11] = ry1;
                dv = rq;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, tmp->fbo);
            glViewport(0, 0, (GLsizei)SSW, (GLsizei)SSH);
            gl_use(prog, 2.0f / (float)SSW, 2.0f / (float)SSH);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g->texs[idx].id);
            glUniform1i(prog->u_atlas, 0);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, layer->tex);
            glUniform1i(prog->u_layer, 1);
            glActiveTexture(GL_TEXTURE0);
            glUniform4f(prog->u_color, paint[0], paint[1], paint[2], paint[3]);
            glUniformMatrix3fv(prog->u_inv, 1, GL_FALSE, m3);
            glUniform4f(prog->u_glyph, gl2->x, gl2->y, gw, gh);
            glUniform4f(prog->u_atlas_rect, (GLfloat)gl2->sx,
                        (GLfloat)gl2->sy, gw, gh);
            glUniform1f(prog->u_ss, (GLfloat)NEOX_GL_SS);
            if (manual) glUniform1i(prog->u_mode, (GLint)blend);
            glDisable(GL_BLEND);
            gl_draw(g, dv, 6);
            glEnable(GL_SCISSOR_TEST);
            glScissor((GLsizei)gx0 * NEOX_GL_SS, (GLsizei)gy0 * NEOX_GL_SS,
                      (GLsizei)(gx1 - gx0) * NEOX_GL_SS,
                      (GLsizei)(gy1 - gy0) * NEOX_GL_SS);
            glBindFramebuffer(GL_FRAMEBUFFER, layer->fbo);
            glViewport(0, 0, (GLsizei)SSW, (GLsizei)SSH);
            gl_use(&g->p_blit, 2.0f / (float)SSW, 2.0f / (float)SSH);
            glBindTexture(GL_TEXTURE_2D, tmp->tex);
            glUniform1i(g->p_blit.u_tex, 0);
            glUniform1f(g->p_blit.u_ss, (GLfloat)NEOX_GL_SS);
            gl_draw_full(g, &g->p_blit, (float)SSW, (float)SSH,
                         2.0f / (float)SSW, 2.0f / (float)SSH);
            glDisable(GL_SCISSOR_TEST);
            gl_rt_release(tmp);
            if (glGetError() != GL_NO_ERROR) {
                dev->ctx->last_error = manual ? "GL manual glyph failed"
                                              : "GL PLUS glyph failed";
                return NEOX_ERROR_BACKEND;
            }
        } else {
            glUniform4f(g->p_glyph.u_glyph, gl2->x, gl2->y, gw, gh);
            glUniform4f(g->p_glyph.u_atlas_rect, (GLfloat)gl2->sx, (GLfloat)gl2->sy,
                        gw, gh);
            gl_draw(g, q, 6);
        }
    }
    return NEOX_OK;
}

/* 组/裁剪层整体合成回父层（§7：opacity × 裁剪掩码 × 组混合模式） */
static void gl_composite(neox_context *ctx, nx_gl_dev *g,
                         const nx_gl_rt *child,
                         const nx_gl_rt *parent, float opacity,
                         neox_blend blend, const nx_gl_rt *clip,
                         uint32_t w, uint32_t h)
{
    uint32_t SSW = w * NEOX_GL_SS, SSH = h * NEOX_GL_SS;
    glBindFramebuffer(GL_FRAMEBUFFER, parent->fbo);
    glViewport(0, 0, (GLsizei)SSW, (GLsizei)SSH);
    gl_use(&g->p_comp, 2.0f / (float)SSW, 2.0f / (float)SSH);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, child->tex);
    glUniform1i(g->p_comp.u_layer, 0);
    glUniform1f(g->p_comp.u_opacity, opacity);
    glUniform1f(g->p_comp.u_ss, (GLfloat)NEOX_GL_SS);
    if (clip) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, clip->tex);
        glUniform1i(g->p_comp.u_clip, 1);
        glUniform1i(g->p_comp.u_has_clip, 1);
    } else {
        glUniform1i(g->p_comp.u_has_clip, 0);
    }
    gl_blend_plain(blend); /* §7 组合成 = 真实 Porter-Duff（fragment 即源） */
    gl_draw_full(g, &g->p_comp, (float)SSW, (float)SSH,
                 2.0f / (float)SSW, 2.0f / (float)SSH);
    glActiveTexture(GL_TEXTURE0);
    if (blend == NEOX_BLEND_PLUS)
        (void)gl_clamp_layer(ctx, g, parent, w, h); /* ex_composite 对 PLUS 逐步 clamp */
}

/* 基底装载：fr->base（W×H 预乘线性）→ 快照纹理 → 放大 blit 进基底层 */
static neox_result gl_load_base(neox_context *ctx, nx_gl_dev *g,
                                const float *base, uint32_t w, uint32_t h,
                                const nx_gl_rt *layer)
{
    uint32_t SSW = w * NEOX_GL_SS, SSH = h * NEOX_GL_SS;
    (void)ctx;
    if (!g->snap_tex || g->snap_w != w || g->snap_h != h) {
        if (g->snap_tex) glDeleteTextures(1, &g->snap_tex);
        glGenTextures(1, &g->snap_tex);
        g->snap_w = w;
        g->snap_h = h;
        glBindTexture(GL_TEXTURE_2D, g->snap_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, (GLsizei)w, (GLsizei)h, 0,
                     GL_RGBA, GL_FLOAT, NULL);
    }
    glBindTexture(GL_TEXTURE_2D, g->snap_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h,
                    GL_RGBA, GL_FLOAT, base);
    glBindFramebuffer(GL_FRAMEBUFFER, layer->fbo);
    glViewport(0, 0, (GLsizei)SSW, (GLsizei)SSH);
    gl_use(&g->p_blit, 2.0f / (float)SSW, 2.0f / (float)SSH);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->snap_tex);
    glUniform1i(g->p_blit.u_tex, 0);
    glUniform1f(g->p_blit.u_ss, (GLfloat)NEOX_GL_SS);
    glDisable(GL_BLEND);
    gl_draw_full(g, &g->p_blit, (float)SSW, (float)SSH,
                 2.0f / (float)SSW, 2.0f / (float)SSH);
    return NEOX_OK;
}

/* 帧末：超采样基底层 → 盒滤波下采样 + sRGB 量化 → 表面（唯一写表面的步骤）。
 *
 * 下采样与量化在 CPU 完成：量化函数与 Soft 后端逐行一致
 * （nx_premul_linear_to_srgb8，§9 ties-to-even），消除 GPU pow 与 libm
 * powf 的 1 ulp 差在量化 ties 边界处的 1 LSB 偏差；16 个子像素求和顺序
 * 与 shader 版一致（i 内层、j 外层），实心内部结果逐位相同。 */
static neox_result gl_resolve(neox_context *ctx, nx_gl_dev *g,
                              const nx_gl_rt *base, const neox_gl_surface *surf)
{
    uint32_t W = surf->w, H = surf->h;
    uint32_t SSW = W * NEOX_GL_SS, SSH = H * NEOX_GL_SS;
    float *ss;
    uint8_t *out;
    uint32_t x, y, i, j;
    neox_result r = NEOX_OK;
    (void)g;
    ss = (float *)nx_alloc(ctx, (size_t)SSW * SSH * 4 * sizeof(float), 16);
    out = (uint8_t *)nx_alloc(ctx, (size_t)W * H * 4, 16);
    if (!ss || !out) {
        if (ss) nx_free(ctx, ss, (size_t)SSW * SSH * 4 * sizeof(float), 16);
        if (out) nx_free(ctx, out, (size_t)W * H * 4, 16);
        ctx->last_error = "resolve buffer allocation failed";
        return NEOX_ERROR_OUT_OF_MEMORY;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, base->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, (GLsizei)SSW, (GLsizei)SSH, GL_RGBA, GL_FLOAT, ss);
    if (glGetError() != GL_NO_ERROR) {
        ctx->last_error = "GL SS layer readback failed";
        r = NEOX_ERROR_BACKEND;
        goto done;
    }
    /* window 行序 == 设备行序（顶点映射 y_ndc = 2y/H − 1），无需翻转 */
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            float p[4];
            uint8_t q[4];
            int c;
            for (j = 0; j < NEOX_GL_SS; j++) {
                for (i = 0; i < NEOX_GL_SS; i++) {
                    const float *t =
                        &ss[(((size_t)y * NEOX_GL_SS + j) * SSW +
                             (size_t)x * NEOX_GL_SS + i) * 4];
                    for (c = 0; c < 4; c++) acc[c] += t[c];
                }
            }
            for (c = 0; c < 4; c++) p[c] = acc[c] / (float)(NEOX_GL_SS * NEOX_GL_SS);
            nx_premul_linear_to_srgb8(p, q);
            memcpy(&out[((size_t)y * W + x) * 4], q, 4);
        }
    }
    glBindTexture(GL_TEXTURE_2D, surf->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)W, (GLsizei)H,
                    GL_RGBA, GL_UNSIGNED_BYTE, out);
    if (glGetError() != GL_NO_ERROR) {
        ctx->last_error = "GL surface write failed";
        r = NEOX_ERROR_BACKEND;
    }
done:
    nx_free(ctx, ss, (size_t)SSW * SSH * 4 * sizeof(float), 16);
    nx_free(ctx, out, (size_t)W * H * 4, 16);
    return r;
}

/* ------------------------------------------------------------------ 执行引擎（pump 驱动）
 *
 * 镜像 Soft 的 ex_state 层栈语义：基底为 layers[0]，owns_layer 的 PUSH 获取
 * 离屏层（透明黑清零），POP 按组不透明度/混合/裁剪掩码整体合成回父层（§7）。
 * 任何 GL 错误 → submission FAILED；resolve（写表面）之前检出错误可保持
 * 表面完全未被触碰（§10.1 原子性）。 */
static neox_result gl_execute(neox_device *dev, nx_sub_slot *sub)
{
    neox_context *ctx = dev->ctx;
    nx_gl_dev *g = (nx_gl_dev *)dev->bstate;
    neox_gl_surface *surf = (neox_gl_surface *)sub->surface;
    uint32_t W = sub->width, H = sub->height;
    uint32_t SSW = W * NEOX_GL_SS, SSH = H * NEOX_GL_SS;
    gl_ex_state *stk = NULL;
    uint32_t nstk = 0, capstk = 0;
    nx_gl_rt **layers = NULL;
    uint32_t nlayers = 0, caplayers = 0;
    nx_gl_rt **clips = NULL;
    uint32_t nclips = 0, capclips = 0;
    uint32_t max_layers = 1;
    uint32_t i;
    neox_result r = NEOX_ERROR_OUT_OF_MEMORY;

    sub->report.algorithm = NEOX_ALG_TRIANGLE;
    if (!surf) {
        ctx->last_error = "submission has no GL surface";
        return NEOX_ERROR_BACKEND;
    }
    if (!gl_make_current(g)) {
        ctx->last_error = "GL: could not make context current";
        return NEOX_ERROR_BACKEND;
    }
    glGetError(); /* 清粘滞错误 */
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);

    {
        nx_gl_rt *base;
        r = gl_rt_acquire(ctx, g, SSW, SSH, 1, &base);
        if (r != NEOX_OK) goto cleanup;
        layers = (nx_gl_rt **)nx_alloc(ctx, sizeof(nx_gl_rt *) * 8, 16);
        stk = (gl_ex_state *)nx_alloc(ctx, sizeof(gl_ex_state) * 8, 16);
        if (!layers || !stk) {
            if (layers) nx_free(ctx, layers, sizeof(nx_gl_rt *) * 8, 16);
            if (stk) nx_free(ctx, stk, sizeof(gl_ex_state) * 8, 16);
            gl_rt_release(base);
            ctx->last_error = "GL execute stack allocation failed";
            return NEOX_ERROR_OUT_OF_MEMORY;
        }
        caplayers = 8;
        capstk = 8;
        layers[0] = base;
        nlayers = 1;
        stk[0].ctm = nx_affine_identity();
        stk[0].opacity = 1.0f;
        stk[0].blend = NEOX_BLEND_SRC_OVER;
        stk[0].clip = NULL;
        stk[0].layer = 0;
        nstk = 1;
    }
    /* 基底：清屏色或快照内容 → 基底层 */
    r = gl_load_base(ctx, g, sub->accum, W, H, layers[0]);
    if (r != NEOX_OK) goto cleanup;

#define GL_CHECK(msg) do { \
        if (glGetError() != GL_NO_ERROR) { \
            ctx->last_error = (msg); \
            r = NEOX_ERROR_BACKEND; \
            goto cleanup; \
        } \
    } while (0)

    for (i = 0; i < sub->cmd_count; i++) {
        const nx_cmd *c = &sub->cmds[i];
        gl_ex_state *top = &stk[nstk - 1];
        switch (c->op) {
        case NXC_PUSH: {
            gl_ex_state *nst;
            nx_gl_rt **nl;
            int32_t layer_idx = top->layer;
            if (c->owns_layer) {
                nx_gl_rt *lb;
                r = gl_rt_acquire(ctx, g, SSW, SSH, 1, &lb);
                if (r != NEOX_OK) goto cleanup;
                nl = (nx_gl_rt **)nx_array_grow(ctx, layers, sizeof(nx_gl_rt *),
                                                &caplayers, nlayers + 1);
                if (!nl) {
                    gl_rt_release(lb);
                    ctx->last_error = "GL layer stack allocation failed";
                    r = NEOX_ERROR_OUT_OF_MEMORY;
                    goto cleanup;
                }
                layers = nl;
                layers[nlayers] = lb;
                layer_idx = (int32_t)nlayers;
                nlayers++;
                if (nlayers > max_layers) max_layers = nlayers;
                /* 隔离层清零（透明黑） */
                glBindFramebuffer(GL_FRAMEBUFFER, lb->fbo);
                glViewport(0, 0, (GLsizei)SSW, (GLsizei)SSH);
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glDisable(GL_BLEND);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            nst = (gl_ex_state *)nx_array_grow(ctx, stk, sizeof(gl_ex_state),
                                               &capstk, nstk + 1);
            if (!nst) {
                if (c->owns_layer) {
                    gl_rt_release(layers[nlayers - 1]);
                    nlayers--;
                }
                ctx->last_error = "GL state stack allocation failed";
                r = NEOX_ERROR_OUT_OF_MEMORY;
                goto cleanup;
            }
            stk = nst;
            stk[nstk].ctm = c->ctm;
            stk[nstk].opacity = c->opacity;
            stk[nstk].blend = c->blend;
            stk[nstk].clip = c->clip_mask;
            stk[nstk].layer = layer_idx;
            nstk++;
            break;
        }
        case NXC_POP: {
            gl_ex_state *top2 = &stk[nstk - 1];
            gl_ex_state *parent = &stk[nstk - 2];
            if (top2->layer != parent->layer) {
                nx_gl_rt *clip_rt = NULL;
                if (top2->clip) {
                    nx_gl_rt **nc;
                    r = gl_rt_acquire(ctx, g, W, H, 0, &clip_rt);
                    if (r != NEOX_OK) goto cleanup;
                    glBindTexture(GL_TEXTURE_2D, clip_rt->tex);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)W, (GLsizei)H,
                                    GL_RED, GL_FLOAT, top2->clip);
                    nc = (nx_gl_rt **)nx_array_grow(ctx, clips, sizeof(nx_gl_rt *),
                                                    &capclips, nclips + 1);
                    if (!nc) {
                        gl_rt_release(clip_rt);
                        ctx->last_error = "GL clip stack allocation failed";
                        r = NEOX_ERROR_OUT_OF_MEMORY;
                        goto cleanup;
                    }
                    clips = nc;
                    clips[nclips++] = clip_rt;
                }
                gl_composite(ctx, g, layers[top2->layer],
                             layers[parent->layer],
                             top2->opacity, top2->blend, clip_rt, W, H);
                if (clip_rt) {
                    clips[nclips - 1] = NULL;
                    nclips--;
                    gl_rt_release(clip_rt);
                }
                gl_rt_release(layers[top2->layer]);
                nlayers--;
            }
            nstk--;
            break;
        }
        case NXC_PATH: {
            const nx_path_slot *ps =
                &((nx_path_slot *)dev->paths.slots)[c->res_idx];
            r = gl_fill_path(dev, g, ps, top->ctm, &c->color, top->blend,
                             layers[top->layer], W, H);
            if (r != NEOX_OK) goto cleanup;
            GL_CHECK("GL error during path fill");
            break;
        }
        case NXC_TEX: {
            const nx_tex_slot *tx =
                &((nx_tex_slot *)dev->textures.slots)[c->res_idx];
            r = gl_draw_tex(dev, g, tx, c, top->ctm, top->blend,
                            layers[top->layer], W, H);
            if (r != NEOX_OK) goto cleanup;
            GL_CHECK("GL error during texture draw");
            break;
        }
        case NXC_GLYPHS: {
            const nx_tex_slot *at =
                &((nx_tex_slot *)dev->textures.slots)[c->res_idx];
            r = gl_draw_glyphs(dev, g, at, c, top->ctm, top->blend,
                               layers[top->layer], W, H);
            if (r != NEOX_OK) goto cleanup;
            GL_CHECK("GL error during glyph draw");
            break;
        }
        default:
            ctx->last_error = "unknown command op";
            r = NEOX_ERROR_BACKEND;
            goto cleanup;
        }
    }
    /* 成功路径的最后一步才触碰表面（§10.1 原子性） */
    r = gl_resolve(ctx, g, layers[0], surf);
    if (r != NEOX_OK) goto cleanup;
    glFlush(); /* pump 即执行：提交至 GL；同步点用 glFinish（最小实现允许阻塞） */
    glFinish();
    GL_CHECK("GL error during resolve");
    sub->report.peak_bytes = (uint64_t)max_layers * (uint64_t)SSW * SSH * 16u;
    r = NEOX_OK;

cleanup:
    /* 释放执行期占用的层（返回池内空闲） */
    {
        uint32_t k;
        for (k = 0; k < nlayers; k++)
            if (layers[k]) gl_rt_release(layers[k]);
        for (k = 0; k < nclips; k++)
            if (clips[k]) gl_rt_release(clips[k]);
    }
    if (clips) nx_free(ctx, clips, (size_t)capclips * sizeof(nx_gl_rt *), 16);
    if (layers) nx_free(ctx, layers, (size_t)caplayers * sizeof(nx_gl_rt *), 16);
    if (stk) nx_free(ctx, stk, (size_t)capstk * sizeof(gl_ex_state), 16);
    return r;
#undef GL_CHECK
}

/* ------------------------------------------------------------------ 表面 API（neox_gl.h） */

static void gl_surface_release_color(neox_gl_surface *surface);
static int gl_surface_alloc_color(neox_gl_surface *surface, uint32_t w, uint32_t h);

neox_result neox_gl_surface_create(neox_device *dev,
                                   uint32_t width, uint32_t height,
                                   neox_gl_surface **out_surface)
{
    neox_context *ctx;
    nx_gl_dev *g;
    neox_gl_surface *s;
    neox_gl_surface **ns;
    if (!dev || !out_surface) return NEOX_ERROR_INVALID_ARGUMENT;
    *out_surface = NULL;
    if (dev->kind != NEOX_BACKEND_GPU) return NEOX_ERROR_INVALID_ARGUMENT;
    ctx = dev->ctx;
    g = (nx_gl_dev *)dev->bstate;
    if (width == 0 || height == 0 || width > g->max_tex || height > g->max_tex)
        NX_ERR(ctx, "invalid surface size", NEOX_ERROR_INVALID_ARGUMENT);
    if (!gl_make_current(g))
        NX_ERR(ctx, "GL: could not make context current", NEOX_ERROR_BACKEND);
    s = (neox_gl_surface *)nx_alloc(ctx, sizeof(neox_gl_surface), 16);
    if (!s) NX_ERR(ctx, "surface allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    memset(s, 0, sizeof(*s));
    s->dev = dev;
    if (!gl_surface_alloc_color(s, width, height)) {
        nx_free(ctx, s, sizeof(neox_gl_surface), 16);
        NX_ERR(ctx, "GL surface framebuffer incomplete", NEOX_ERROR_BACKEND);
    }
    ns = (neox_gl_surface **)nx_array_grow(ctx, g->surfs, sizeof(void *),
                                           &g->capsurfs, g->nsurfs + 1);
    if (!ns) {
        gl_surface_release_color(s);
        nx_free(ctx, s, sizeof(neox_gl_surface), 16);
        NX_ERR(ctx, "surface registry allocation failed",
               NEOX_ERROR_OUT_OF_MEMORY);
    }
    g->surfs = ns;
    g->surfs[g->nsurfs++] = s;
    *out_surface = s;
    return NEOX_OK;
}

static void gl_surface_release_color(neox_gl_surface *surface)
{
    if (surface->fbo) glDeleteFramebuffers(1, &surface->fbo);
    if (surface->tex) glDeleteTextures(1, &surface->tex);
    surface->fbo = 0;
    surface->tex = 0;
}

static int gl_surface_alloc_color(neox_gl_surface *surface, uint32_t w, uint32_t h)
{
    glGenTextures(1, &surface->tex);
    glBindTexture(GL_TEXTURE_2D, surface->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &surface->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, surface->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, surface->tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        glGetError() != GL_NO_ERROR) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        gl_surface_release_color(surface);
        return 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    surface->w = w;
    surface->h = h;
    return 1;
}

void neox_gl_surface_destroy(neox_device *dev, neox_gl_surface *surface)
{
    nx_gl_dev *g;
    uint32_t i;
    if (!dev || !surface) return;
    g = (nx_gl_dev *)dev->bstate;
    if (!g) return;
    if (gl_make_current(g))
        gl_surface_release_color(surface);
    if (g->ctx && surface->win_egl)
        nx_gl_ctx_window_destroy(g->ctx, surface->win_egl);
    if (surface->presented)
        nx_free(dev->ctx, surface->presented,
                (size_t)surface->w * surface->h * 4, 16);
    for (i = 0; i < g->nsurfs; i++) {
        if (g->surfs[i] == surface) {
            g->surfs[i] = g->surfs[g->nsurfs - 1];
            g->nsurfs--;
            break;
        }
    }
    nx_free(dev->ctx, surface, sizeof(neox_gl_surface), 16);
}

neox_result neox_gl_surface_read_pixels(neox_gl_surface *surface,
                                        uint8_t *out_pixels)
{
    if (!surface || !out_pixels) return NEOX_ERROR_INVALID_ARGUMENT;
    {
        nx_gl_dev *g = (nx_gl_dev *)surface->dev->bstate;
        if (!g || !gl_make_current(g))
            return NEOX_ERROR_BACKEND;
        glBindFramebuffer(GL_FRAMEBUFFER, surface->fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, (GLsizei)surface->w, (GLsizei)surface->h,
                     GL_RGBA, GL_UNSIGNED_BYTE, out_pixels);
        if (glGetError() != GL_NO_ERROR) {
            surface->dev->ctx->last_error = "GL surface readback failed";
            return NEOX_ERROR_BACKEND;
        }
    }
    return NEOX_OK;
}

static neox_result gl_surface_track(neox_device *dev, neox_gl_surface *s)
{
    neox_context *ctx = dev->ctx;
    nx_gl_dev *g = (nx_gl_dev *)dev->bstate;
    neox_gl_surface **ns =
        (neox_gl_surface **)nx_array_grow(ctx, g->surfs, sizeof(void *),
                                          &g->capsurfs, g->nsurfs + 1);
    if (!ns) return NEOX_ERROR_OUT_OF_MEMORY;
    g->surfs = ns;
    g->surfs[g->nsurfs++] = s;
    return NEOX_OK;
}

neox_result neox_gl_surface_create_window(neox_device *dev,
                                          const neox_gl_window_desc *desc,
                                          neox_gl_surface **out_surface)
{
    neox_context *ctx;
    nx_gl_dev *g;
    neox_gl_surface *s;
    neox_result tr;
    if (!dev || !desc || !out_surface) return NEOX_ERROR_INVALID_ARGUMENT;
    *out_surface = NULL;
    ctx = dev->ctx;
    if (desc->struct_size != (uint32_t)sizeof(*desc) ||
        desc->magic != NEOX_GL_WINSURF_MAGIC)
        NX_ERR(ctx, "neox_gl_window_desc version mismatch",
               NEOX_ERROR_INVALID_ARGUMENT);
    if (!desc->native_window)
        NX_ERR(ctx, "window surface requires a native window",
               NEOX_ERROR_INVALID_ARGUMENT);
    if (dev->kind != NEOX_BACKEND_GPU) return NEOX_ERROR_INVALID_ARGUMENT;
    g = (nx_gl_dev *)dev->bstate;
    if (!g || !nx_gl_ctx_can_window(g->ctx))
        NX_ERR(ctx, "GL device was not created on a window display",
               NEOX_ERROR_UNSUPPORTED);
    if (desc->width == 0 || desc->height == 0 ||
        desc->width > g->max_tex || desc->height > g->max_tex)
        NX_ERR(ctx, "invalid window surface size", NEOX_ERROR_INVALID_ARGUMENT);
    if (!gl_make_current(g))
        NX_ERR(ctx, "GL: could not make context current", NEOX_ERROR_BACKEND);
    s = (neox_gl_surface *)nx_alloc(ctx, sizeof(*s), 16);
    if (!s) NX_ERR(ctx, "surface allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    memset(s, 0, sizeof(*s));
    s->dev = dev;
    s->native_window = desc->native_window;
    s->vsync = desc->vsync ? 1 : 0;
    if (!gl_surface_alloc_color(s, desc->width, desc->height)) {
        nx_free(ctx, s, sizeof(*s), 16);
        NX_ERR(ctx, "GL surface framebuffer incomplete", NEOX_ERROR_BACKEND);
    }
    s->win_egl = nx_gl_ctx_window_surface(g->ctx, desc->native_window, s->vsync);
    if (!s->win_egl) {
        const char *e = nx_gl_ctx_last_error();
        gl_surface_release_color(s);
        nx_free(ctx, s, sizeof(*s), 16);
        NX_ERR(ctx, e ? e : "EGL window surface failed", NEOX_ERROR_BACKEND);
    }
    /* 离屏绘制仍绑 FBO；把 current 从窗口表面换回辅助表面。 */
    if (!nx_gl_ctx_make_current(g->ctx)) {
        nx_gl_ctx_window_destroy(g->ctx, s->win_egl);
        gl_surface_release_color(s);
        nx_free(ctx, s, sizeof(*s), 16);
        NX_ERR(ctx, "GL: could not restore offscreen current", NEOX_ERROR_BACKEND);
    }
    tr = gl_surface_track(dev, s);
    if (tr != NEOX_OK) {
        nx_gl_ctx_window_destroy(g->ctx, s->win_egl);
        gl_surface_release_color(s);
        nx_free(ctx, s, sizeof(*s), 16);
        NX_ERR(ctx, "surface registry allocation failed", tr);
    }
    *out_surface = s;
    return NEOX_OK;
}

neox_result neox_gl_surface_resize(neox_gl_surface *surface,
                                   uint32_t width, uint32_t height)
{
    nx_gl_dev *g;
    neox_context *ctx;
    void *win;
    if (!surface || !surface->dev) return NEOX_ERROR_INVALID_ARGUMENT;
    if (width == surface->w && height == surface->h) return NEOX_OK;
    g = (nx_gl_dev *)surface->dev->bstate;
    ctx = surface->dev->ctx;
    if (!g) return NEOX_ERROR_BACKEND;
    if (width == 0 || height == 0 || width > g->max_tex || height > g->max_tex)
        NX_ERR(ctx, "invalid window surface size", NEOX_ERROR_INVALID_ARGUMENT);
    if (!gl_make_current(g))
        NX_ERR(ctx, "GL: could not make context current", NEOX_ERROR_BACKEND);
    gl_surface_release_color(surface);
    if (surface->presented) {
        nx_free(ctx, surface->presented, (size_t)surface->w * surface->h * 4, 16);
        surface->presented = NULL;
    }
    if (!gl_surface_alloc_color(surface, width, height))
        NX_ERR(ctx, "GL surface resize failed", NEOX_ERROR_BACKEND);
    if (!surface->win_egl) return NEOX_OK;
    win = surface->native_window;
    nx_gl_ctx_window_destroy(g->ctx, surface->win_egl);
    surface->win_egl = nx_gl_ctx_window_surface(g->ctx, win, surface->vsync);
    if (!nx_gl_ctx_make_current(g->ctx))
        NX_ERR(ctx, "GL: could not restore offscreen current", NEOX_ERROR_BACKEND);
    if (!surface->win_egl) {
        const char *e = nx_gl_ctx_last_error();
        NX_ERR(ctx, e ? e : "EGL window surface resize failed", NEOX_ERROR_BACKEND);
    }
    return NEOX_OK;
}

neox_result neox_gl_surface_present(neox_gl_surface *surface)
{
    nx_gl_dev *g;
    neox_context *ctx;
    uint32_t w, h, y;
    uint8_t *raw, *dst;
    float v[6];
    if (!surface || !surface->dev) return NEOX_ERROR_INVALID_ARGUMENT;
    if (!surface->win_egl) return NEOX_ERROR_UNSUPPORTED;
    g = (nx_gl_dev *)surface->dev->bstate;
    ctx = surface->dev->ctx;
    if (!g || !g->p_present.id) return NEOX_ERROR_BACKEND;
    w = surface->w;
    h = surface->h;
    if (!nx_gl_ctx_window_make_current(g->ctx, surface->win_egl))
        NX_ERR(ctx, "GL: could not make window current", NEOX_ERROR_BACKEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, (GLsizei)w, (GLsizei)h);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, surface->tex);
    gl_use(&g->p_present, 2.0f / (float)w, 2.0f / (float)h);
    glUniform1i(g->p_present.u_tex, 0);
    glUniform1f(g->p_present.u_ss, (GLfloat)h);
    v[0] = 0.0f; v[1] = 0.0f;
    v[2] = 2.0f * (float)w; v[3] = 0.0f;
    v[4] = 0.0f; v[5] = 2.0f * (float)h;
    gl_draw(g, v, 3);
    raw = (uint8_t *)nx_alloc(ctx, (size_t)w * h * 4, 16);
    dst = (uint8_t *)nx_alloc(ctx, (size_t)w * h * 4, 16);
    if (!raw || !dst) {
        if (raw) nx_free(ctx, raw, (size_t)w * h * 4, 16);
        if (dst) nx_free(ctx, dst, (size_t)w * h * 4, 16);
        NX_ERR(ctx, "present readback allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, raw);
    if (glGetError() != GL_NO_ERROR) {
        nx_free(ctx, raw, (size_t)w * h * 4, 16);
        nx_free(ctx, dst, (size_t)w * h * 4, 16);
        NX_ERR(ctx, "GL window readback failed", NEOX_ERROR_BACKEND);
    }
    /* glReadPixels 底行在前；翻转成左上原点，与表面 read_pixels 同向。 */
    for (y = 0; y < h; y++)
        memcpy(dst + (size_t)y * w * 4,
               raw + (size_t)(h - 1 - y) * w * 4,
               (size_t)w * 4);
    nx_free(ctx, raw, (size_t)w * h * 4, 16);
    if (surface->presented)
        nx_free(ctx, surface->presented, (size_t)w * h * 4, 16);
    surface->presented = dst;
    glFinish();
    nx_gl_ctx_window_swap(g->ctx, surface->win_egl);
    if (!nx_gl_ctx_make_current(g->ctx))
        NX_ERR(ctx, "GL: could not restore offscreen current", NEOX_ERROR_BACKEND);
    return NEOX_OK;
}

neox_result neox_gl_surface_read_presented(neox_gl_surface *surface,
                                           uint8_t *out_pixels)
{
    if (!surface || !out_pixels) return NEOX_ERROR_INVALID_ARGUMENT;
    if (!surface->presented) return NEOX_ERROR_UNSUPPORTED;
    memcpy(out_pixels, surface->presented, (size_t)surface->w * surface->h * 4);
    return NEOX_OK;
}

neox_result neox_gl_device_native_visual(neox_device *dev, uint32_t *out_visual_id)
{
    nx_gl_dev *g;
    unsigned long id = 0;
    if (!dev || !out_visual_id) return NEOX_ERROR_INVALID_ARGUMENT;
    if (dev->kind != NEOX_BACKEND_GPU) return NEOX_ERROR_INVALID_ARGUMENT;
    g = (nx_gl_dev *)dev->bstate;
    if (!g || !nx_gl_ctx_native_visual(g->ctx, &id))
        return NEOX_ERROR_UNSUPPORTED;
    *out_visual_id = (uint32_t)id;
    return NEOX_OK;
}

/* ------------------------------------------------------------------ GL 后端 vtable */

const nx_backend *nx_backend_gl(void)
{
    static const nx_backend vt = {
        "gl",
        gl_device_init,
        gl_device_shutdown,
        gl_begin_target,
        gl_execute,
        gl_get_caps,
        NULL,               /* poll：execute 内 glFinish 同步完成 */
        NULL                /* block：同步后端 */
    };
    return &vt;
}
