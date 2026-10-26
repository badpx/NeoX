/* ===========================================================================
 * metal.mm — NeoX Metal 后端：macOS 原生（Objective-C++，零第三方依赖）
 *
 * 定位：第三个执行后端，核心首个**真异步**实现。架构决策严格镜像 gl.c
 * （docs/adr/0003-shared-coverage-rasterization.md）：
 *
 *   - 路径覆盖率：共享 src/geom.c CPU 光栅化 → R32Float 纹理（texelRead）；
 *     paint×cov 在 CPU 预乘（与 Soft 同一表达式）→ RGBA32Float 纹理。
 *   - 层附件 RGBA32Float 预乘线性；混合全程浮点无中间量化。
 *   - Porter-Duff 13 模式 + §6.1 覆盖率插值：映射到 MTLBlendFactor；
 *     fragment 双输出（dual-source，GL_SRC1_ALPHA 对应物）表达
 *     SRC/SRC_IN/SRC_OUT/DST_ATOP 四模式；不支持时回退 ping-pong 手动混合
 *     （启动时用管线创建探测，本机 Apple Silicon 实测支持）。
 *   - PLUS 的 clamp 非线性：ping-pong 手动混合（读 dst 算插值式写临时层，
 *     scissor 拷回），与 gl.c 同构。
 *   - 帧末 resolve 在 **GPU 完成后**于 poll（pump 内）执行：读回浮点层，
 *     用 nx_premul_linear_to_srgb8（与 Soft 同一函数）量化后写入表面。
 *   - draw_texture/draw_glyphs：shader 内手动 NEAREST/LINEAR（texelRead），
 *     采样相位取设备像素中心；四边形外扩 0.6px + 像素中心二值包含。
 *
 * 异步模型（与 Soft/GL 的本质区别，验证"提交后存活期"契约）：
 *   - execute 只编码 + 提交 command buffer，submission 保持 PENDING；
 *   - completedHandler（Metal 内部线程）只登记完成事件与释放顶点缓冲，
 *     不触碰 neox 状态；
 *   - neox_device_pump 经 vt->poll 在本调用线程收割：resolve 读回、
 *     状态推进 COMPLETE、载荷释放、延迟回收 —— pump 不阻塞；
 *   - neox_submission_wait 经 vt->block（waitUntilCompleted 最旧在途
 *     command buffer）等待真实进度，不死等空转。
 *
 * 坐标约定：与 gl.c 相同（设备坐标 = 纹理行序，无 y 翻转）；顶点映射
 * ndc = (2x/W−1, 1−2y/H)（Metal clip y 向上，故 y 分量取反）。
 *
 * 内存管理：本文件以 -fno-objc-arc 编译（MRC），MTL/NS 对象手动
 * retain/release；所有 CPU 分配走注入分配器。GL 对象随 device destroy 全清
 * （shutdown 前等待在途 command buffer 完成，避免完成回调触碰已释放状态）。
 * =========================================================================*/
#include "internal.h"
#include "neox_mtl.h"

#import <Metal/Metal.h>
#import <Foundation/NSString.h>
#import <pthread.h>
#include <stdlib.h>

/* 超采样倍数：与 gl.c 同一决策（ADR 0003，覆盖率共享 CPU 光栅化） */
#define NEOX_MTL_SS 1

/* 顶点缓冲每次绘制的偏移需 256 字节对齐 */
#define NX_MTL_VOFF_ALIGN 256u

/* ------------------------------------------------------------------ MSL 源码（init 时编译；失败 → NEOX_ERROR_BACKEND） */

static const char *NX_MSL_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct NxVsOut { float4 p [[position]]; };\n"
    "\n"
    "vertex NxVsOut nx_vs(const device float2 *pos [[buffer(0)]],\n"
    "                     constant float2 &scale [[buffer(1)]],\n"
    "                     uint vid [[vertex_id]]) {\n"
    "    NxVsOut o;\n"
    "    float2 q = pos[vid];\n"
    "    o.p = float4(q.x * scale.x - 1.0, 1.0 - q.y * scale.y, 0.0, 1.0);\n"
    "    return o;\n"
    "}\n"
    "\n"
    "/* §6.1 oa/o1a 编码（与 soft.c nx_blend_pixel_cov、gl.c gl_blend_cov 逐项对齐） */\n"
    "#define NX_OA(m, cov, pa, p_a) ((m) == 1 ? (cov) : ((m) == 6 ? (cov) * (1.0f - (pa)) : (p_a)))\n"
    "#define NX_O1A(m, cov, pa) \\\n"
    "    ((m) == 2 || (m) == 5 || (m) == 7 ? (cov) : ((m) == 10 ? (cov) * (1.0f - (pa)) : 0.0f))\n"
    "\n"
    "struct NxUFill { float4 a; };  /* a = {ss, mode, paint_a, 0} */\n"
    "\n"
    "float4 nx_fill_core(NxVsOut in, constant NxUFill &u,\n"
    "                    texture2d<float> mask, texture2d<float> premul,\n"
    "                    thread float &o1a) {\n"
    "    uint2 dp = uint2(in.p.xy / u.a.x);\n"
    "    float cov = mask.read(dp).r;\n"
    "    if (cov <= 0.0f) discard_fragment();\n"
    "    float4 p = premul.read(dp);\n"
    "    int m = int(u.a.y);\n"
    "    o1a = NX_O1A(m, cov, u.a.z);\n"
    "    return float4(p.rgb, NX_OA(m, cov, u.a.z, p.a));\n"
    "}\n"
    "\n"
    "fragment float4 nx_fs_fill(NxVsOut in [[stage_in]], constant NxUFill &u [[buffer(0)]],\n"
    "                           texture2d<float> mask [[texture(0)]],\n"
    "                           texture2d<float> premul [[texture(1)]]) {\n"
    "    float o1a;\n"
    "    return nx_fill_core(in, u, mask, premul, o1a);\n"
    "}\n"
    "\n"
    "struct NxDsOut { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; };\n"
    "\n"
    "fragment NxDsOut nx_fs_fill_ds(NxVsOut in [[stage_in]], constant NxUFill &u [[buffer(0)]],\n"
    "                               texture2d<float> mask [[texture(0)]],\n"
    "                               texture2d<float> premul [[texture(1)]]) {\n"
    "    float o1a;\n"
    "    float4 c0 = nx_fill_core(in, u, mask, premul, o1a);\n"
    "    NxDsOut o;\n"
    "    o.c0 = c0;\n"
    "    o.c1 = float4(0.0, 0.0, 0.0, o1a);\n"
    "    return o;\n"
    "}\n"
    "\n"
    "struct NxUTex { float4 a, b, c, d, e, f, g; };\n"
    "/* a = {ss, mode, linear, opacity}；b = src.xywh；c = dst.xywh；d = {tex_w, tex_h, 0, 0}\n"
    " * e/f/g = u_inv 三列（设备 → 局部） */\n"
    "\n"
    "float4 nx_tex_core(NxVsOut in, constant NxUTex &u, texture2d<float> tex,\n"
    "                   thread float &o1a) {\n"
    "    float2 devc = floor(in.p.xy / u.a.x) + 0.5f;\n"
    "    float3 l = u.e.xyz * devc.x + u.f.xyz * devc.y + u.g.xyz;\n"
    "    float tu = (l.x - u.c.x) / u.c.z;\n"
    "    float tv = (l.y - u.c.y) / u.c.w;\n"
    "    if (tu < 0.0f || tu > 1.0f || tv < 0.0f || tv > 1.0f) discard_fragment();\n"
    "    float uu = u.b.x + tu * u.b.z;\n"
    "    float vv = u.b.y + tv * u.b.w;\n"
    "    int tw = int(u.d.x), th = int(u.d.y);\n"
    "    float4 cc;\n"
    "    if (u.a.z < 0.5f) {\n"
    "        int i = clamp(int(floor(uu)), 0, tw - 1);\n"
    "        int j = clamp(int(floor(vv)), 0, th - 1);\n"
    "        cc = tex.read(uint2(i, j));\n"
    "    } else {\n"
    "        float x = uu - 0.5f, y = vv - 0.5f;\n"
    "        float fx = x - floor(x), fy = y - floor(y);\n"
    "        int i0 = clamp(int(floor(x)), 0, tw - 1);\n"
    "        int i1 = clamp(int(floor(x)) + 1, 0, tw - 1);\n"
    "        int j0 = clamp(int(floor(y)), 0, th - 1);\n"
    "        int j1 = clamp(int(floor(y)) + 1, 0, th - 1);\n"
    "        float4 p00 = tex.read(uint2(i0, j0));\n"
    "        float4 p10 = tex.read(uint2(i1, j0));\n"
    "        float4 p01 = tex.read(uint2(i0, j1));\n"
    "        float4 p11 = tex.read(uint2(i1, j1));\n"
    "        cc = p00 * ((1.0f - fx) * (1.0f - fy)) + p10 * (fx * (1.0f - fy))\n"
    "           + p01 * ((1.0f - fx) * fy) + p11 * (fx * fy);\n"
    "    }\n"
    "    float sa = cc.a * u.a.w;\n"
    "    int m = int(u.a.y);\n"
    "    o1a = NX_O1A(m, 1.0f, sa);\n"
    "    float oa = (m == 1) ? 1.0f : ((m == 6) ? 1.0f - sa : sa);\n"
    "    return float4(cc.rgb * u.a.w, oa);\n"
    "}\n"
    "\n"
    "fragment float4 nx_fs_tex(NxVsOut in [[stage_in]], constant NxUTex &u [[buffer(0)]],\n"
    "                          texture2d<float> tex [[texture(0)]]) {\n"
    "    float o1a;\n"
    "    return nx_tex_core(in, u, tex, o1a);\n"
    "}\n"
    "\n"
    "fragment NxDsOut nx_fs_tex_ds(NxVsOut in [[stage_in]], constant NxUTex &u [[buffer(0)]],\n"
    "                              texture2d<float> tex [[texture(0)]]) {\n"
    "    float o1a;\n"
    "    float4 c0 = nx_tex_core(in, u, tex, o1a);\n"
    "    NxDsOut o;\n"
    "    o.c0 = c0;\n"
    "    o.c1 = float4(0.0, 0.0, 0.0, o1a);\n"
    "    return o;\n"
    "}\n"
    "\n"
    "float nx_median3(float a, float b, float c) {\n"
    "    return max(min(a, b), min(max(a, b), c));\n"
    "}\n"
    /* f = {mode, dist_range, softness, sdf_scale}；重建（§8.2，与 soft.c/GLSL 同式） */
    "float nx_recon_cov(float v, float4 f) {\n"
    "    float w = f.z > 0.0f ? f.z : 1.0f;\n"
    "    float t = clamp((0.5f - v) * f.y * f.w / w + 0.5f, 0.0f, 1.0f);\n"
    "    return t * t * (3.0f - 2.0f * t);\n"
    "}\n"
    /* 双线性采样 + 三模式重建（uv 为图集像素坐标；§8.2） */
    "float nx_glyph_cov(float2 uv, float4 f, texture2d<float> atlas) {\n"
    "    float x = uv.x - 0.5f, y = uv.y - 0.5f;\n"
    "    float fx = x - floor(x), fy = y - floor(y);\n"
    "    uint2 sz = uint2(atlas.get_width(), atlas.get_height());\n"
    "    int i0 = clamp(int(floor(x)), 0, int(sz.x) - 1);\n"
    "    int i1 = clamp(int(floor(x)) + 1, 0, int(sz.x) - 1);\n"
    "    int j0 = clamp(int(floor(y)), 0, int(sz.y) - 1);\n"
    "    int j1 = clamp(int(floor(y)) + 1, 0, int(sz.y) - 1);\n"
    "    float4 p00 = atlas.read(uint2(i0, j0));\n"
    "    float4 p10 = atlas.read(uint2(i1, j0));\n"
    "    float4 p01 = atlas.read(uint2(i0, j1));\n"
    "    float4 p11 = atlas.read(uint2(i1, j1));\n"
    "    float4 pm = p00 * ((1.0f - fx) * (1.0f - fy)) + p10 * (fx * (1.0f - fy))\n"
    "              + p01 * ((1.0f - fx) * fy) + p11 * (fx * fy);\n"
    "    if (int(f.x) == 1) return nx_recon_cov(pm.a, f);\n"
    "    else if (int(f.x) == 2) return nx_recon_cov(nx_median3(pm.r, pm.g, pm.b), f);\n"
    "    else return pm.a;\n"
    "}\n"
    "\n"
    "struct NxUGlyph { float4 a, b, c, d, e, f, g, h; };\n"
    "/* a = {ss, mode, 0, 0}；b = paint；c = glyph.xywh；d = atlas_rect.xywh\n"
    " * e/f/g = u_inv 三列；h = {atlas_mode, dist_range, softness, sdf_scale} */\n"
    "\n"
    "float4 nx_glyph_core(NxVsOut in, constant NxUGlyph &u, texture2d<float> atlas,\n"
    "                     thread float &o1a) {\n"
    "    float2 devc = floor(in.p.xy / u.a.x) + 0.5f;\n"
    "    float3 l = u.e.xyz * devc.x + u.f.xyz * devc.y + u.g.xyz;\n"
    "    if (l.x < u.c.x || l.x > u.c.x + u.c.z ||\n"
    "        l.y < u.c.y || l.y > u.c.y + u.c.w) discard_fragment();\n"
    "    float cov = nx_glyph_cov(float2(u.d.x + (l.x - u.c.x),\n"
    "                                    u.d.y + (l.y - u.c.y)), u.h, atlas);\n"
    "    int m = int(u.a.y);\n"
    "    float pa = u.b.a;\n"
    "    o1a = NX_O1A(m, cov, pa);\n"
    "    float oa = NX_OA(m, cov, pa, pa * cov);\n"
    "    return float4(u.b.rgb * cov, oa);\n"
    "}\n"
    "\n"
    "fragment float4 nx_fs_glyph(NxVsOut in [[stage_in]], constant NxUGlyph &u [[buffer(0)]],\n"
    "                            texture2d<float> atlas [[texture(0)]]) {\n"
    "    float o1a;\n"
    "    return nx_glyph_core(in, u, atlas, o1a);\n"
    "}\n"
    "\n"
    "fragment NxDsOut nx_fs_glyph_ds(NxVsOut in [[stage_in]], constant NxUGlyph &u [[buffer(0)]],\n"
    "                                texture2d<float> atlas [[texture(0)]]) {\n"
    "    float o1a;\n"
    "    float4 c0 = nx_glyph_core(in, u, atlas, o1a);\n"
    "    NxDsOut o;\n"
    "    o.c0 = c0;\n"
    "    o.c1 = float4(0.0, 0.0, 0.0, o1a);\n"
    "    return o;\n"
    "}\n"
    "\n"
    "/* 组合成（§7）：fragment 即源（真实 Porter-Duff，由管线因子表实现） */\n"
    "struct NxUComp { float4 a; };  /* a = {ss, opacity, has_clip, 0} */\n"
    "\n"
    "fragment float4 nx_fs_comp(NxVsOut in [[stage_in]], constant NxUComp &u [[buffer(0)]],\n"
    "                           texture2d<float> layer [[texture(0)]],\n"
    "                           texture2d<float> clip [[texture(1)]]) {\n"
    "    float4 s = layer.read(uint2(in.p.xy));\n"
    "    float m = 1.0f;\n"
    "    if (u.a.z > 0.5f) m = clip.read(uint2(in.p.xy / u.a.x)).r;\n"
    "    return s * (u.a.y * m);\n"
    "}\n"
    "\n"
    "struct NxUBlit { float4 a; };  /* a.x = ss */\n"
    "\n"
    "fragment float4 nx_fs_blit(NxVsOut in [[stage_in]], constant NxUBlit &u [[buffer(0)]],\n"
    "                           texture2d<float> tex [[texture(0)]]) {\n"
    "    return tex.read(uint2(in.p.xy / u.a.x));\n"
    "}\n"
    "\n"
    "fragment float4 nx_fs_clamp(NxVsOut in [[stage_in]],\n"
    "                            texture2d<float> tex [[texture(0)]]) {\n"
    "    return clamp(tex.read(uint2(in.p.xy)), 0.0f, 1.0f);\n"
    "}\n"
    "\n"
    "/* PLUS（§6.1 blend 内 clamp 后插值）：out = min(s·cov + cov·d, cov) + (1−cov)·d。\n"
    " * 读 dst 计算后写临时层，由调用方 scissor 拷回（ping-pong）。cov=0 透传 dst。 */\n"
    "fragment float4 nx_fs_plusfill(NxVsOut in [[stage_in]], constant NxUFill &u [[buffer(0)]],\n"
    "                               texture2d<float> premul [[texture(0)]],\n"
    "                               texture2d<float> mask [[texture(1)]],\n"
    "                               texture2d<float> layer [[texture(2)]]) {\n"
    "    uint2 dp = uint2(in.p.xy / u.a.x);\n"
    "    float4 dst = layer.read(dp);\n"
    "    float cov = mask.read(dp).r;\n"
    "    if (cov <= 0.0f) return dst;\n"
    "    float4 sc = premul.read(dp);\n"
    "    return min(sc + cov * dst, float4(cov)) + (1.0f - cov) * dst;\n"
    "}\n"
    "\n"
    "fragment float4 nx_fs_plusglyph(NxVsOut in [[stage_in]], constant NxUGlyph &u [[buffer(0)]],\n"
    "                                texture2d<float> atlas [[texture(0)]],\n"
    "                                texture2d<float> layer [[texture(1)]]) {\n"
    "    float2 devc = floor(in.p.xy / u.a.x) + 0.5f;\n"
    "    float3 l = u.e.xyz * devc.x + u.f.xyz * devc.y + u.g.xyz;\n"
    "    if (l.x < u.c.x || l.x > u.c.x + u.c.z ||\n"
    "        l.y < u.c.y || l.y > u.c.y + u.c.w) discard_fragment();\n"
    "    float uu = u.d.x + (l.x - u.c.x);\n"
    "    float vv = u.d.y + (l.y - u.c.y);\n"
    "    float cov = nx_glyph_cov(float2(uu, vv), u.h, atlas);\n"
    "    uint2 dp = uint2(in.p.xy / u.a.x);\n"
    "    float4 dst = layer.read(dp);\n"
    "    if (cov <= 0.0f) return dst;\n"
    "    float4 sc = u.b * cov;\n"
    "    return min(sc + cov * dst, float4(cov)) + (1.0f - cov) * dst;\n"
    "}\n"
    "\n"
    "/* dual-source 不可用时的回退：读 dst 手动计算 cov·blend(s,d) + (1−cov)·d\n"
    " * （仅 SRC/SRC_IN/SRC_OUT/DST_ATOP 四模式）。s 由 sc/cov 还原。 */\n"
    "fragment float4 nx_fs_manualglyph(NxVsOut in [[stage_in]],\n"
    "                                constant NxUGlyph &u [[buffer(0)]],\n"
    "                                texture2d<float> atlas [[texture(0)]],\n"
    "                                texture2d<float> layer [[texture(1)]]) {\n"
    "    float2 devc = floor(in.p.xy / u.a.x) + 0.5f;\n"
    "    float3 l = u.e.xyz * devc.x + u.f.xyz * devc.y + u.g.xyz;\n"
    "    if (l.x < u.c.x || l.x > u.c.x + u.c.z ||\n"
    "        l.y < u.c.y || l.y > u.c.y + u.c.w) discard_fragment();\n"
    "    float uu = u.d.x + (l.x - u.c.x);\n"
    "    float vv = u.d.y + (l.y - u.c.y);\n"
    "    float cov = nx_glyph_cov(float2(uu, vv), u.h, atlas);\n"
    "    uint2 dp = uint2(in.p.xy / u.a.x);\n"
    "    float4 dst = layer.read(dp);\n"
    "    if (cov <= 0.0f) return dst;\n"
    "    float4 s = u.b;\n"
    "    int m = int(u.a.y);\n"
    "    float4 t;\n"
    "    if (m == 2) t = s;\n"
    "    else if (m == 5) t = s * dst.a;\n"
    "    else if (m == 7) t = s * (1.0f - dst.a);\n"
    "    else t = s * (1.0f - dst.a) + dst * s.a;\n"
    "    return cov * t + (1.0f - cov) * dst;\n"
    "}\n"
    "\n"
    "fragment float4 nx_fs_manual(NxVsOut in [[stage_in]], constant NxUFill &u [[buffer(0)]],\n"
    "                             texture2d<float> premul [[texture(0)]],\n"
    "                             texture2d<float> mask [[texture(1)]],\n"
    "                             texture2d<float> layer [[texture(2)]]) {\n"
    "    uint2 dp = uint2(in.p.xy / u.a.x);\n"
    "    float4 dst = layer.read(dp);\n"
    "    float cov = mask.read(dp).r;\n"
    "    if (cov <= 0.0f) return dst;\n"
    "    float4 s = premul.read(dp) / cov;\n"
    "    int m = int(u.a.y);\n"
    "    float4 t;\n"
    "    if (m == 2) t = s;\n"
    "    else if (m == 5) t = s * dst.a;\n"
    "    else if (m == 7) t = s * (1.0f - dst.a);\n"
    "    else t = s * (1.0f - dst.a) + dst * s.a;  /* DST_ATOP */\n"
    "    return cov * t + (1.0f - cov) * dst;\n"
    "}\n";

/* ------------------------------------------------------------------ 后端状态 */

typedef struct nx_mtl_rt {          /* 渲染目标池项（层 / 掩码纹理） */
    id<MTLTexture> tex;
    uint32_t w, h;
    int in_use;
    int is_layer;                   /* 1 = RGBA32Float 层；0 = R32Float 掩码 */
} nx_mtl_rt;

typedef struct nx_mtl_texent {      /* 纹理槽 → MTLTexture（惰性上传） */
    id<MTLTexture> tex;
    uint32_t version;
} nx_mtl_texent;

struct neox_mtl_surface {
    neox_device *dev;
    id<MTLTexture> tex;
    uint32_t w, h;
};

/* 在途提交：resolve 所需的基底层保持占用，cb 供 block() 等待 */
typedef struct nx_mtl_inflight {
    uint32_t sub_idx;
    id<MTLCommandBuffer> cb;        /* +1，poll/destroy 释放 */
    id<MTLBuffer> vbo;              /* +1，poll/destroy 释放 */
    nx_mtl_rt *base;
    nx_mtl_rt **held;               /* 执行期占用的纹理（poll 完成后归还池） */
    uint32_t nheld;
    int completed;                  /* 完成回调置位（lock 保护） */
} nx_mtl_inflight;

#define NX_MTL_NMODE 13

typedef struct nx_mtl_dev {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLLibrary> lib;
    id<MTLFunction> vs;
    id<MTLFunction> fs_fill, fs_fill_ds, fs_tex, fs_tex_ds, fs_glyph, fs_glyph_ds;
    id<MTLFunction> fs_comp, fs_blit, fs_clamp, fs_plusfill, fs_plusglyph, fs_manual;
    id<MTLFunction> fs_manualglyph;
    /* 每程序 × 混合模式的管线（blend 因子内建于管线；dual 模式单独一套）。
     * dual_ok = 0 时 SRC/SRC_IN/SRC_OUT/DST_ATOP 走 ps_manual ping-pong。 */
    id<MTLRenderPipelineState> ps_fill[NX_MTL_NMODE], ps_fill_ds[NX_MTL_NMODE];
    id<MTLRenderPipelineState> ps_tex[NX_MTL_NMODE], ps_tex_ds[NX_MTL_NMODE];
    id<MTLRenderPipelineState> ps_glyph[NX_MTL_NMODE], ps_glyph_ds[NX_MTL_NMODE];
    id<MTLRenderPipelineState> ps_comp[NX_MTL_NMODE];
    id<MTLRenderPipelineState> ps_blit, ps_clamp, ps_plusfill, ps_plusglyph, ps_manual;
    id<MTLRenderPipelineState> ps_manual_glyph;
    int dual_ok;
    nx_mtl_rt *rts;                 /* 池：层（is_layer=1）与掩码纹理 */
    uint32_t nrts, caprts;
    nx_mtl_texent *texs;            /* 并行于 textures 槽位表 */
    uint32_t ntexs, captexs;
    neox_mtl_surface **surfs;       /* 表面注册表 */
    uint32_t nsurfs, capsurfs;
    float *mask;                    /* 覆盖率 scratch（行距 16 字节对齐） */
    uint32_t mask_w, mask_h, mask_stride;
    float *premul;                  /* paint×cov scratch */
    uint32_t premul_w, premul_h;
    /* 异步完成（回调线程只触碰 lock/done/vbo 释放） */
    pthread_mutex_t lock;
    uint32_t *done;                 /* 已完成 sub 槽索引（回调线程写入） */
    uint32_t done_count, done_cap;
    nx_mtl_inflight *inflight;
    uint32_t ninflight, capinflight;
} nx_mtl_dev;

/* ------------------------------------------------------------------ 小工具 */

static NSUInteger mtl_align256(NSUInteger v)
{
    return (v + NX_MTL_VOFF_ALIGN - 1u) & ~(NSUInteger)(NX_MTL_VOFF_ALIGN - 1u);
}

/* 顶点暂存：拷贝到 vbo 运行偏移（256 对齐）；容量不足返回 NSUIntegerMax */
static NSUInteger mtl_stage(id<MTLBuffer> vbo, NSUInteger *used,
                            const float *v, NSUInteger nfloats)
{
    NSUInteger off = mtl_align256(*used);
    if (off + nfloats * 4 > [vbo length]) return NSUIntegerMax;
    memcpy((uint8_t *)[vbo contents] + off, v, nfloats * 4);
    *used = off + nfloats * 4;
    return off;
}

/* 渲染目标池（镜像 gl_rt_acquire/release） */
#define NX_MTL_RT_POOL_CAP 1024u

static neox_result mtl_rt_pool_init(neox_context *ctx, nx_mtl_dev *g)
{
    g->rts = (nx_mtl_rt *)nx_alloc(ctx, NX_MTL_RT_POOL_CAP * sizeof(nx_mtl_rt), 16);
    if (!g->rts) return NEOX_ERROR_OUT_OF_MEMORY;
    memset(g->rts, 0, NX_MTL_RT_POOL_CAP * sizeof(nx_mtl_rt));
    g->caprts = NX_MTL_RT_POOL_CAP;
    return NEOX_OK;
}

static void mtl_rt_destroy(nx_mtl_rt *rt)
{
    [rt->tex release];
    memset(rt, 0, sizeof(*rt));
}

static neox_result mtl_rt_acquire(neox_context *ctx, nx_mtl_dev *g,
                                  uint32_t w, uint32_t h, int is_layer,
                                  nx_mtl_rt **out)
{
    uint32_t i;
    nx_mtl_rt *rt;
    for (i = 0; i < g->nrts; i++) {
        rt = &g->rts[i];
        if (!rt->in_use && rt->is_layer == is_layer &&
            rt->w == w && rt->h == h) {
            rt->in_use = 1;
            *out = rt;
            return NEOX_OK;
        }
    }
    /* 池数组固定容量、init 时一次分配、永不扩容：池项指针（layers/held/
     * inflight.base 持有的 nx_mtl_rt*）因此稳定（nx_array_grow 扩容会整体
     * 搬迁数组，使跨调用保存的池项指针悬垂）。容量按验证后端定位取 1024。 */
    if (g->nrts >= g->caprts) {
        ctx->last_error = "MTL render-target pool exhausted";
        return NEOX_ERROR_OUT_OF_MEMORY;
    }
    rt = &g->rts[g->nrts];
    memset(rt, 0, sizeof(*rt));
    rt->is_layer = is_layer;
    rt->w = w; rt->h = h;
    {
        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                (is_layer ? MTLPixelFormatRGBA32Float : MTLPixelFormatR32Float)
                width:w height:h mipmapped:NO];
        td.usage = is_layer ? (MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead)
                            : MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        rt->tex = [g->device newTextureWithDescriptor:td];  /* +1 */
    }
    if (!rt->tex) {
        ctx->last_error = "MTL texture allocation failed";
        return NEOX_ERROR_OUT_OF_MEMORY;
    }
    rt->in_use = 1;
    g->nrts++;
    *out = rt;
    return NEOX_OK;
}

static void mtl_rt_release(nx_mtl_rt *rt) { rt->in_use = 0; }

/* CPU → 纹理上传（replaceRegion；行距由调用方按 16 字节对齐保证） */
static void mtl_upload(id<MTLTexture> tex, uint32_t w, uint32_t h,
                       const void *data, uint32_t stride_bytes)
{
    [tex replaceRegion:MTLRegionMake2D(0, 0, w, h)
           mipmapLevel:0 withBytes:data bytesPerRow:stride_bytes];
}

/* 纹理槽惰性上传（version 失效标记；同 gl_tex_ensure） */
static neox_result mtl_tex_ensure(neox_context *ctx, nx_mtl_dev *g,
                                  neox_device *dev, uint32_t idx)
{
    const nx_tex_slot *s = &((nx_tex_slot *)dev->textures.slots)[idx];
    nx_mtl_texent *e;
    if (idx >= g->ntexs) {
        uint32_t old = g->ntexs;
        nx_mtl_texent *ne = (nx_mtl_texent *)nx_array_grow(ctx, g->texs,
                             sizeof(nx_mtl_texent), &g->captexs, idx + 1);
        if (!ne) return NEOX_ERROR_OUT_OF_MEMORY;
        g->texs = ne;
        memset(&g->texs[old], 0, (size_t)(idx + 1 - old) * sizeof(nx_mtl_texent));
        g->ntexs = idx + 1;
    }
    e = &g->texs[idx];
    if (e->tex && e->version == s->version) return NEOX_OK;
    if (!e->tex) {
        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                width:s->width height:s->height mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        e->tex = [g->device newTextureWithDescriptor:td];  /* +1 */
        if (!e->tex) {
            ctx->last_error = "MTL texture allocation failed";
            return NEOX_ERROR_OUT_OF_MEMORY;
        }
    }
    /* 注：replaceRegion 与 pending GPU 读无隐式同步。纹理更新若可能撞上在途
     * 提交，调用方应先经 pump/wait 排空（帧协议保证：录制新帧前上一帧已
     * 完成——与 CAP 回放、跨帧资源复用同一契约）。 */
    mtl_upload(e->tex, s->width, s->height, s->px,
               (uint32_t)s->width * 16u);
    e->version = s->version;
    return NEOX_OK;
}

/* 管线创建：blend 因子内建；失败返回 nil（调用方转 NEOX_ERROR_BACKEND） */
static id<MTLRenderPipelineState> mtl_make_pipe(nx_mtl_dev *g, id<MTLFunction> fs,
                                                MTLBlendFactor sb, MTLBlendFactor db,
                                                int blend)
{
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    id<MTLRenderPipelineState> ps;
    NSError *err = nil;
    pd.vertexFunction = g->vs;
    pd.fragmentFunction = fs;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    if (blend) {
        pd.colorAttachments[0].blendingEnabled = YES;
        pd.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        pd.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        pd.colorAttachments[0].sourceRGBBlendFactor = sb;
        pd.colorAttachments[0].destinationRGBBlendFactor = db;
        pd.colorAttachments[0].sourceAlphaBlendFactor = sb;
        pd.colorAttachments[0].destinationAlphaBlendFactor = db;
    }
    ps = [g->device newRenderPipelineStateWithDescriptor:pd error:&err];  /* +1 */
    [pd release];
    if (err) [err release];
    return ps;
}

/* §6.1 覆盖率插值因子表（与 gl.c gl_blend_cov 逐项对齐；dual = 需双源混合） */
typedef struct { MTLBlendFactor sb, db; int dual; } nx_mtl_cov;
static const nx_mtl_cov NX_MTL_COV[NX_MTL_NMODE] = {
    { MTLBlendFactorOne, MTLBlendFactorOneMinusSourceAlpha, 0 }, /* SRC_OVER */
    { MTLBlendFactorZero, MTLBlendFactorOneMinusSourceAlpha, 0 }, /* CLEAR  */
    { MTLBlendFactorOne, MTLBlendFactorOneMinusSource1Alpha, 1 }, /* SRC    */
    { MTLBlendFactorZero, MTLBlendFactorOne, 0 },                 /* DST    */
    { MTLBlendFactorOneMinusDestinationAlpha, MTLBlendFactorOne, 0 }, /* DST_OVER */
    { MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusSource1Alpha, 1 }, /* SRC_IN */
    { MTLBlendFactorZero, MTLBlendFactorOneMinusSourceAlpha, 0 }, /* DST_IN */
    { MTLBlendFactorOneMinusDestinationAlpha, MTLBlendFactorOneMinusSource1Alpha, 1 }, /* SRC_OUT */
    { MTLBlendFactorZero, MTLBlendFactorOneMinusSourceAlpha, 0 }, /* DST_OUT */
    { MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusSourceAlpha, 0 }, /* SRC_ATOP */
    { MTLBlendFactorOneMinusDestinationAlpha, MTLBlendFactorOneMinusSource1Alpha, 1 }, /* DST_ATOP */
    { MTLBlendFactorOneMinusDestinationAlpha, MTLBlendFactorOneMinusSourceAlpha, 0 }, /* XOR */
    { MTLBlendFactorOne, MTLBlendFactorOne, 0 },                  /* PLUS（tex 用；fill/glyph 走 ping-pong） */
};

/* §7 组合成真实 Porter-Duff 因子表（与 gl.c gl_blend_plain 逐项对齐） */
static const nx_mtl_cov NX_MTL_PLAIN[NX_MTL_NMODE] = {
    { MTLBlendFactorOne, MTLBlendFactorOneMinusSourceAlpha, 0 }, /* SRC_OVER */
    { MTLBlendFactorZero, MTLBlendFactorZero, 0 },               /* CLEAR  */
    { MTLBlendFactorOne, MTLBlendFactorZero, 0 },                /* SRC    */
    { MTLBlendFactorZero, MTLBlendFactorOne, 0 },                /* DST    */
    { MTLBlendFactorOneMinusDestinationAlpha, MTLBlendFactorOne, 0 }, /* DST_OVER */
    { MTLBlendFactorDestinationAlpha, MTLBlendFactorZero, 0 },   /* SRC_IN */
    { MTLBlendFactorZero, MTLBlendFactorSourceAlpha, 0 },        /* DST_IN */
    { MTLBlendFactorOneMinusDestinationAlpha, MTLBlendFactorZero, 0 }, /* SRC_OUT */
    { MTLBlendFactorZero, MTLBlendFactorOneMinusSourceAlpha, 0 },/* DST_OUT */
    { MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusSourceAlpha, 0 }, /* SRC_ATOP */
    { MTLBlendFactorOneMinusDestinationAlpha, MTLBlendFactorSourceAlpha, 0 }, /* DST_ATOP */
    { MTLBlendFactorOneMinusDestinationAlpha, MTLBlendFactorOneMinusSourceAlpha, 0 }, /* XOR */
    { MTLBlendFactorOne, MTLBlendFactorOne, 0 },                 /* PLUS */
};

/* ------------------------------------------------------------------ 编码辅助 */

typedef struct nx_mtl_enc {
    id<MTLCommandBuffer> cb;
    id<MTLBuffer> vbo;
    NSUInteger used;
    uint32_t W, H, SSW, SSH;
    /* 执行期占用的纹理（mask/premul/snap/clip/ping-pong tmp）：
     * replaceRegion 与 pending GPU 读之间无隐式同步，须保持占用至
     * command buffer 完成（由 inflight 接管，poll 时归还池） */
    nx_mtl_rt **held;
    uint32_t nheld, capheld;
} nx_mtl_enc;

static neox_result mtl_hold(neox_context *ctx, nx_mtl_enc *e, nx_mtl_rt *rt)
{
    nx_mtl_rt **nh = (nx_mtl_rt **)nx_array_grow(ctx, e->held,
                        sizeof(nx_mtl_rt *), &e->capheld, e->nheld + 1);
    if (!nh) return NEOX_ERROR_OUT_OF_MEMORY;
    e->held = nh;
    e->held[e->nheld++] = rt;
    return NEOX_OK;
}

static id<MTLRenderCommandEncoder> mtl_pass(nx_mtl_dev *g, nx_mtl_enc *e,
                                            id<MTLTexture> tex, MTLLoadAction load)
{
    MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
    id<MTLRenderCommandEncoder> enc;
    (void)g;
    pd.colorAttachments[0].texture = tex;
    pd.colorAttachments[0].loadAction = load;
    pd.colorAttachments[0].storeAction = MTLStoreActionStore;
    pd.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
    enc = [e->cb renderCommandEncoderWithDescriptor:pd];
    return enc;
}

static void mtl_emit(id<MTLRenderCommandEncoder> enc, id<MTLRenderPipelineState> ps,
                     nx_mtl_enc *e, NSUInteger voff, NSUInteger nverts,
                     const void *fsu, NSUInteger fsulen,
                     id<MTLTexture> t0, id<MTLTexture> t1, id<MTLTexture> t2)
{
    float sc[2] = { 2.0f / (float)e->SSW, 2.0f / (float)e->SSH };
    [enc setRenderPipelineState:ps];
    [enc setVertexBuffer:e->vbo offset:voff atIndex:0];
    [enc setVertexBytes:sc length:sizeof(sc) atIndex:1];
    if (fsulen) [enc setFragmentBytes:fsu length:fsulen atIndex:0];
    if (t0) [enc setFragmentTexture:t0 atIndex:0];
    if (t1) [enc setFragmentTexture:t1 atIndex:1];
    if (t2) [enc setFragmentTexture:t2 atIndex:2];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:nverts];
}

/* 全屏三角形（设备坐标 [0,W]×[0,H]，放大 2 倍保证覆盖） */
static NSUInteger mtl_stage_full(nx_mtl_enc *e, float w, float h, float *tmp)
{
    tmp[0] = 0.0f;  tmp[1] = 0.0f;
    tmp[2] = 2.0f * w; tmp[3] = 0.0f;
    tmp[4] = 0.0f;  tmp[5] = 2.0f * h;
    return mtl_stage(e->vbo, &e->used, tmp, 6);
}

/* 层清零（隔离组层；等价 glClear） */
static void mtl_clear_layer(nx_mtl_dev *g, nx_mtl_enc *e, nx_mtl_rt *layer)
{
    id<MTLRenderCommandEncoder> enc = mtl_pass(g, e, layer->tex, MTLLoadActionClear);
    [enc endEncoding];
}

/* scissor 拷回（ping-pong 第二步；bbox 为设备像素） */
static void mtl_copy_back(nx_mtl_dev *g, nx_mtl_enc *e, nx_mtl_rt *src,
                          nx_mtl_rt *dst, float minx, float miny,
                          float maxx, float maxy)
{
    id<MTLRenderCommandEncoder> enc;
    float v[6];
    NSUInteger off;
    MTLScissorRect sc;
    float ublit[4] = { (float)NEOX_MTL_SS, 0.0f, 0.0f, 0.0f };
    enc = mtl_pass(g, e, dst->tex, MTLLoadActionLoad);
    sc.x = (NSUInteger)(minx * NEOX_MTL_SS);
    sc.y = (NSUInteger)(miny * NEOX_MTL_SS);
    sc.width = (NSUInteger)((maxx - minx) * NEOX_MTL_SS);
    sc.height = (NSUInteger)((maxy - miny) * NEOX_MTL_SS);
    [enc setScissorRect:sc];
    off = mtl_stage_full(e, (float)e->SSW, (float)e->SSH, v);
    mtl_emit(enc, g->ps_blit, e, off, 3, ublit, sizeof(ublit), src->tex, nil, nil);
    [enc endEncoding];
}

/* 凸四边形各边外扩 d（设备像素）：与 gl.c gl_inflate_quad 同算法——纹理/字形
 * 四边形的覆盖判定在参照后端是"设备像素中心二值包含"，外扩保证被保留像素
 * 的全部子像素产生 fragment。 */
static void mtl_inflate_quad(const float xc[4], const float yc[4], float d,
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

/* PLUS 后的整层 clamp（等价 gl_clamp_layer：tmp clamp 再拷回） */
static neox_result mtl_clamp_layer(neox_context *ctx, nx_mtl_dev *g, nx_mtl_enc *e,
                                   nx_mtl_rt *layer)
{
    nx_mtl_rt *tmp;
    id<MTLRenderCommandEncoder> enc;
    float v[6];
    NSUInteger off;
    neox_result r = mtl_rt_acquire(ctx, g, e->SSW, e->SSH, 1, &tmp);
    if (r != NEOX_OK) return r;
    enc = mtl_pass(g, e, tmp->tex, MTLLoadActionDontCare);
    off = mtl_stage_full(e, (float)e->SSW, (float)e->SSH, v);
    mtl_emit(enc, g->ps_clamp, e, off, 3, NULL, 0, layer->tex, nil, nil);
    [enc endEncoding];
    mtl_copy_back(g, e, tmp, layer, 0.0f, 0.0f,
                  (float)e->W, (float)e->H);
    r = mtl_hold(ctx, e, tmp);
    if (r != NEOX_OK) mtl_rt_release(tmp);
    return r;
}

/* ------------------------------------------------------------------ 设备初始化 / 销毁 */

/* 完整状态销毁（init 失败路径与 shutdown 共用；释放 ObjC 对象与全部 CPU 侧
 * 池/注册表/异步列表；不含 device 结构本身之外的 neox_device） */
static void mtl_state_destroy(neox_context *ctx, nx_mtl_dev *g)
{
    uint32_t i;
    if (!g) return;
    if (g->device) {
        /* 等待在途提交完成：完成回调会触碰 lock/done，必须先排空 */
        for (;;) {
            id<MTLCommandBuffer> cb = nil;
            pthread_mutex_lock(&g->lock);
            if (g->ninflight > 0) cb = [g->inflight[0].cb retain];
            pthread_mutex_unlock(&g->lock);
            if (!cb) break;
            [cb waitUntilCompleted];
            [cb release];
        }
    }
    pthread_mutex_lock(&g->lock);
    for (i = 0; i < g->ninflight; i++) {
        uint32_t h2;
        [g->inflight[i].cb release];
        [g->inflight[i].vbo release];
        for (h2 = 0; h2 < g->inflight[i].nheld; h2++)
            mtl_rt_release(g->inflight[i].held[h2]);
        if (g->inflight[i].held)
            nx_free(ctx, g->inflight[i].held,
                    (size_t)g->inflight[i].nheld * sizeof(nx_mtl_rt *), 16);
    }
    g->ninflight = 0;
    pthread_mutex_unlock(&g->lock);
    for (i = 0; i < NX_MTL_NMODE; i++) {
        [g->ps_fill[i] release]; [g->ps_fill_ds[i] release];
        [g->ps_tex[i] release]; [g->ps_tex_ds[i] release];
        [g->ps_glyph[i] release]; [g->ps_glyph_ds[i] release];
        [g->ps_comp[i] release];
    }
    [g->ps_blit release]; [g->ps_clamp release];
    [g->ps_plusfill release]; [g->ps_plusglyph release];
    [g->ps_manual release]; [g->ps_manual_glyph release];
    [g->vs release];
    [g->fs_fill release]; [g->fs_fill_ds release];
    [g->fs_tex release]; [g->fs_tex_ds release];
    [g->fs_glyph release]; [g->fs_glyph_ds release];
    [g->fs_comp release]; [g->fs_blit release]; [g->fs_clamp release];
    [g->fs_plusfill release]; [g->fs_plusglyph release];
    [g->fs_manual release]; [g->fs_manualglyph release];
    [g->lib release]; [g->queue release]; [g->device release];
    for (i = 0; i < g->nrts; i++) mtl_rt_destroy(&g->rts[i]);
    for (i = 0; i < g->ntexs; i++) [g->texs[i].tex release];
    for (i = 0; i < g->nsurfs; i++) {
        [g->surfs[i]->tex release];
        nx_free(ctx, g->surfs[i], sizeof(neox_mtl_surface), 16);
    }
    pthread_mutex_destroy(&g->lock);
    if (g->rts) nx_free(ctx, g->rts, (size_t)g->caprts * sizeof(nx_mtl_rt), 16);
    if (g->texs) nx_free(ctx, g->texs, (size_t)g->captexs * sizeof(nx_mtl_texent), 16);
    if (g->surfs) nx_free(ctx, g->surfs, (size_t)g->capsurfs * sizeof(void *), 16);
    if (g->done) nx_free(ctx, g->done, (size_t)g->done_cap * 4, 16);
    if (g->mask) nx_free(ctx, g->mask, (size_t)g->mask_h * g->mask_stride, 16);
    if (g->premul) nx_free(ctx, g->premul, (size_t)g->premul_w * g->premul_h * 16, 16);
    if (g->inflight)
        nx_free(ctx, g->inflight,
                (size_t)g->capinflight * sizeof(nx_mtl_inflight), 16);
    nx_free(ctx, g, sizeof(nx_mtl_dev), 16);
}

static neox_result mtl_device_init(neox_device *dev, const void *platform,
                                   size_t plat_size)
{
    neox_context *ctx = dev->ctx;
    nx_mtl_dev *g;
    neox_mtl_device_desc mtd;
    neox_result r;
    int m;
    uint32_t i;

    r = nx_device_init_tables(dev);
    if (r != NEOX_OK) return r;

    /* platform 版本协商（neox_device_desc 约定；与 gl.c 同规则） */
    memset(&mtd, 0, sizeof(mtd));
    if (platform) {
        const neox_mtl_device_desc *d = (const neox_mtl_device_desc *)platform;
        uint32_t hdr_size;
        if (plat_size < sizeof(uint32_t))
            NX_ERR(ctx, "platform_size too small for struct_size",
                   NEOX_ERROR_INVALID_ARGUMENT);
        hdr_size = *(const uint32_t *)platform;
        if (hdr_size > plat_size)
            NX_ERR(ctx, "struct_size exceeds platform_size",
                   NEOX_ERROR_INVALID_ARGUMENT);
        if (plat_size < sizeof(neox_mtl_device_desc))
            NX_ERR(ctx, "platform_size smaller than neox_mtl_device_desc",
                   NEOX_ERROR_INVALID_ARGUMENT);
        if (d->struct_size != (uint32_t)sizeof(neox_mtl_device_desc))
            NX_ERR(ctx, "neox_mtl_device_desc version mismatch",
                   NEOX_ERROR_INVALID_ARGUMENT);
        if (d->magic != NEOX_MTL_DESC_MAGIC)
            NX_ERR(ctx, "neox_mtl_device_desc magic mismatch",
                   NEOX_ERROR_INVALID_ARGUMENT);
        mtd = *d;
    } else if (plat_size != 0) {
        NX_ERR(ctx, "platform is NULL but platform_size is nonzero",
               NEOX_ERROR_INVALID_ARGUMENT);
    }

    g = (nx_mtl_dev *)nx_alloc(ctx, sizeof(nx_mtl_dev), 16);
    if (!g) return NEOX_ERROR_OUT_OF_MEMORY;
    memset(g, 0, sizeof(*g));
    pthread_mutex_init(&g->lock, NULL);
    r = mtl_rt_pool_init(ctx, g);
    if (r != NEOX_OK) {
        pthread_mutex_destroy(&g->lock);
        nx_free(ctx, g, sizeof(nx_mtl_dev), 16);
        return r;
    }

    /* 设备与命令队列：外部传入（void*）或系统默认/自建 */
    if (mtd.device) {
        g->device = [(id<MTLDevice>)mtd.device retain];
    } else {
        g->device = MTLCreateSystemDefaultDevice();
    }
    if (!g->device) {
        nx_free(ctx, g, sizeof(nx_mtl_dev), 16);
        NX_ERR(ctx, "Metal: no default MTLDevice", NEOX_ERROR_BACKEND);
    }
    if (mtd.command_queue) {
        g->queue = [(id<MTLCommandQueue>)mtd.command_queue retain];
    } else {
        g->queue = [g->device newCommandQueue];  /* +1 */
    }
    if (!g->queue) {
        pthread_mutex_destroy(&g->lock);
        [g->device release];
        nx_free(ctx, g, sizeof(nx_mtl_dev), 16);
        NX_ERR(ctx, "Metal: command queue creation failed",
               NEOX_ERROR_BACKEND);
    }

    /* MSL 编译（启动成本；失败 = 后端不可用） */
    {
        NSError *err = nil;
        NSString *src = [NSString stringWithUTF8String:NX_MSL_SRC];
        g->lib = [g->device newLibraryWithSource:src options:nil error:&err];  /* +1 */
        if (err) [err release];
        if (!g->lib) {
            mtl_state_destroy(ctx, g);
            NX_ERR(ctx, "Metal: MSL library compilation failed",
                   NEOX_ERROR_BACKEND);
        }
    }
#define NX_GETFN(v, n) do { \
        v = [g->lib newFunctionWithName:@ n]; \
        if (!v) { mtl_state_destroy(ctx, g); \
            NX_ERR(ctx, "Metal: shader function missing: " n, NEOX_ERROR_BACKEND); } \
    } while (0)
    NX_GETFN(g->vs, "nx_vs");
    NX_GETFN(g->fs_fill, "nx_fs_fill");
    NX_GETFN(g->fs_fill_ds, "nx_fs_fill_ds");
    NX_GETFN(g->fs_tex, "nx_fs_tex");
    NX_GETFN(g->fs_tex_ds, "nx_fs_tex_ds");
    NX_GETFN(g->fs_glyph, "nx_fs_glyph");
    NX_GETFN(g->fs_glyph_ds, "nx_fs_glyph_ds");
    NX_GETFN(g->fs_comp, "nx_fs_comp");
    NX_GETFN(g->fs_blit, "nx_fs_blit");
    NX_GETFN(g->fs_clamp, "nx_fs_clamp");
    NX_GETFN(g->fs_plusfill, "nx_fs_plusfill");
    NX_GETFN(g->fs_plusglyph, "nx_fs_plusglyph");
    NX_GETFN(g->fs_manual, "nx_fs_manual");
    NX_GETFN(g->fs_manualglyph, "nx_fs_manualglyph");
#undef NX_GETFN

    /* 管线：每程序 × 模式；dual 四模式先做支持探测（任一创建失败 → 回退） */
    g->dual_ok = 1;
    for (m = 0; m < NX_MTL_NMODE; m++) {
        if (NX_MTL_COV[m].dual)
            g->ps_fill_ds[m] = mtl_make_pipe(g, g->fs_fill_ds,
                                             NX_MTL_COV[m].sb, NX_MTL_COV[m].db, 1);
        else
            g->ps_fill[m] = mtl_make_pipe(g, g->fs_fill,
                                          NX_MTL_COV[m].sb, NX_MTL_COV[m].db, 1);
        g->ps_tex[m] = mtl_make_pipe(g, NX_MTL_COV[m].dual ? g->fs_tex_ds : g->fs_tex,
                                     NX_MTL_COV[m].sb, NX_MTL_COV[m].db, 1);
        g->ps_glyph[m] = mtl_make_pipe(g, NX_MTL_COV[m].dual ? g->fs_glyph_ds : g->fs_glyph,
                                       NX_MTL_COV[m].sb, NX_MTL_COV[m].db, 1);
        g->ps_comp[m] = mtl_make_pipe(g, g->fs_comp,
                                      NX_MTL_PLAIN[m].sb, NX_MTL_PLAIN[m].db, 1);
    }
    for (m = 0; m < NX_MTL_NMODE; m++) {
        if (NX_MTL_COV[m].dual && !g->ps_fill_ds[m]) g->dual_ok = 0;
    }
    /* 测试钩子：强制走 ping-pong 手动混合回退路径（验证无双源硬件下的行为） */
    if (getenv("NEOX_MTL_FORCE_NO_DUAL")) g->dual_ok = 0;
    if (!g->dual_ok) {
        /* 无双源混合：清掉 dual 管线，四模式走 ps_manual ping-pong */
        for (m = 0; m < NX_MTL_NMODE; m++) {
            if (NX_MTL_COV[m].dual) {
                [g->ps_fill[m] release]; g->ps_fill[m] = nil;
                [g->ps_fill_ds[m] release]; g->ps_fill_ds[m] = nil;
                [g->ps_tex[m] release]; g->ps_tex[m] = nil;
                [g->ps_glyph[m] release]; g->ps_glyph[m] = nil;
                g->ps_fill[m] = mtl_make_pipe(g, g->fs_fill,
                                              MTLBlendFactorOne, MTLBlendFactorZero, 0);
                g->ps_tex[m] = mtl_make_pipe(g, g->fs_tex,
                                             MTLBlendFactorOne, MTLBlendFactorZero, 0);
                g->ps_glyph[m] = mtl_make_pipe(g, g->fs_glyph,
                                               MTLBlendFactorOne, MTLBlendFactorZero, 0);
            }
        }
        g->ps_manual = mtl_make_pipe(g, g->fs_manual, MTLBlendFactorOne,
                                     MTLBlendFactorZero, 0);
        g->ps_manual_glyph = mtl_make_pipe(g, g->fs_manualglyph,
                                           MTLBlendFactorOne,
                                           MTLBlendFactorZero, 0);
    }
    g->ps_blit = mtl_make_pipe(g, g->fs_blit, MTLBlendFactorOne,
                               MTLBlendFactorZero, 0);
    g->ps_clamp = mtl_make_pipe(g, g->fs_clamp, MTLBlendFactorOne,
                                MTLBlendFactorZero, 0);
    g->ps_plusfill = mtl_make_pipe(g, g->fs_plusfill, MTLBlendFactorOne,
                                   MTLBlendFactorZero, 0);
    g->ps_plusglyph = mtl_make_pipe(g, g->fs_plusglyph, MTLBlendFactorOne,
                                    MTLBlendFactorZero, 0);
    for (m = 0; m < NX_MTL_NMODE; m++) {
        if (!g->ps_fill[m] || !g->ps_tex[m] || !g->ps_glyph[m] || !g->ps_comp[m]) {
            mtl_state_destroy(ctx, g);
            NX_ERR(ctx, "Metal: render pipeline creation failed",
                   NEOX_ERROR_BACKEND);
        }
    }
    if (!g->ps_blit || !g->ps_clamp || !g->ps_plusfill || !g->ps_plusglyph ||
        (!g->dual_ok && (!g->ps_manual || !g->ps_manual_glyph))) {
        mtl_state_destroy(ctx, g);
        NX_ERR(ctx, "Metal: render pipeline creation failed",
               NEOX_ERROR_BACKEND);
    }
    for (i = 0; i < g->nsurfs; i++) (void)i; /* 注册表为空 */
    dev->bstate = g;
    return NEOX_OK;
}

static void mtl_device_shutdown(neox_device *dev)
{
    if (dev && dev->bstate) {
        mtl_state_destroy(dev->ctx, (nx_mtl_dev *)dev->bstate);
        dev->bstate = NULL;
    }
}

static void mtl_get_caps(neox_device *dev, neox_caps *out_caps)
{
    (void)dev;
    out_caps->features = NEOX_CAP_FLOAT_BLEND;
    out_caps->max_texture_size = 16384; /* Apple 系 GPU 2D 纹理上限 */
    out_caps->backend_memory_limit = 0;
}

/* ------------------------------------------------------------------ begin_target
 * 校验表面归属、确定帧尺寸、装载基底（与 gl_begin_target 镜像；clear==NULL 时
 * getBytes 快照 → 预乘线性 float） */
static neox_result mtl_begin_target(neox_frame *fr,
                                    const neox_target_desc *target)
{
    neox_device *dev = fr->dev;
    neox_context *ctx = dev->ctx;
    neox_mtl_surface *surf = (neox_mtl_surface *)target->surface;
    size_t npix, i;
    uint32_t stride;
    if (!surf)
        NX_ERR(ctx, "invalid MTL target (surface is NULL)",
               NEOX_ERROR_INVALID_ARGUMENT);
    if (surf->dev != dev)
        NX_ERR(ctx, "surface belongs to another device",
               NEOX_ERROR_OWNERSHIP_VIOLATION);
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
        uint8_t *snap;
        stride = ((uint32_t)fr->width * 4u + 15u) & ~15u;
        snap = (uint8_t *)nx_alloc(ctx, (size_t)stride * fr->height, 16);
        if (!snap) {
            nx_free(ctx, fr->base, npix * 4 * sizeof(float), 16);
            fr->base = NULL;
            NX_ERR(ctx, "surface snapshot allocation failed",
                   NEOX_ERROR_OUT_OF_MEMORY);
        }
        [surf->tex getBytes:snap bytesPerRow:stride
                 fromRegion:MTLRegionMake2D(0, 0, fr->width, fr->height)
                mipmapLevel:0];
        nx_pixels_to_premul_linear(snap, stride, fr->width, fr->height, fr->base);
        nx_free(ctx, snap, (size_t)stride * fr->height, 16);
    }
    return NEOX_OK;
}

/* ------------------------------------------------------------------ 绘制基元 */

/* 覆盖率掩码 + CPU 预乘（与 gl_fill_path / soft ex_draw_path 同式） */
static neox_result mtl_fill_prep(neox_context *ctx, nx_mtl_dev *g,
                                 const nx_path_slot *ps, neox_affine ctm,
                                 const float paint[4], uint32_t w, uint32_t h,
                                 nx_mtl_rt **out_mrt, nx_mtl_rt **out_prt)
{
    uint32_t npx = w * h, k, mstride;
    neox_result r;
    if (!g->mask || g->mask_w != w || g->mask_h != h) {
        if (g->mask)
            nx_free(ctx, g->mask, (size_t)g->mask_h * g->mask_stride, 16);
        mstride = (w * 4u + 15u) & ~15u;
        g->mask = (float *)nx_alloc(ctx, (size_t)mstride * h, 16);
        if (!g->mask) { g->mask_w = g->mask_h = 0; return NEOX_ERROR_OUT_OF_MEMORY; }
        g->mask_w = w; g->mask_h = h; g->mask_stride = mstride;
    }
    if (!g->premul || g->premul_w != w || g->premul_h != h) {
        if (g->premul)
            nx_free(ctx, g->premul, (size_t)g->premul_w * g->premul_h * 16, 16);
        g->premul = (float *)nx_alloc(ctx, (size_t)w * h * 16, 16);
        if (!g->premul) { g->premul_w = g->premul_h = 0; return NEOX_ERROR_OUT_OF_MEMORY; }
        g->premul_w = w; g->premul_h = h;
    }
    r = nx_rasterize_path(ctx, ps, ctm, w, h, g->mask);
    if (r != NEOX_OK) return r;
    for (k = 0; k < npx; k++) {
        float cov = g->mask[k];
        g->premul[k * 4 + 0] = paint[0] * cov;
        g->premul[k * 4 + 1] = paint[1] * cov;
        g->premul[k * 4 + 2] = paint[2] * cov;
        g->premul[k * 4 + 3] = paint[3] * cov;
    }
    r = mtl_rt_acquire(ctx, g, w, h, 0, out_mrt);
    if (r != NEOX_OK) return r;
    r = mtl_rt_acquire(ctx, g, w, h, 1, out_prt);
    if (r != NEOX_OK) { mtl_rt_release(*out_mrt); return r; }
    mtl_upload((*out_mrt)->tex, w, h, g->mask, g->mask_stride);
    mtl_upload((*out_prt)->tex, w, h, g->premul, w * 16u);
    return NEOX_OK;
}

/* ping-pong 手动混合路径（PLUS 与 dual 回退共用骨架） */
static neox_result mtl_draw_pingpong(neox_context *ctx, nx_mtl_dev *g,
                                     nx_mtl_enc *e, nx_mtl_rt *layer,
                                     id<MTLRenderPipelineState> ptmp,
                                     id<MTLTexture> t0, id<MTLTexture> t1,
                                     id<MTLTexture> t2,
                                     const void *fsu, NSUInteger fsulen,
                                     const float *v, NSUInteger nfloats,
                                     float minx, float miny, float maxx, float maxy)
{
    nx_mtl_rt *tmp;
    id<MTLRenderCommandEncoder> enc;
    NSUInteger off = mtl_stage(e->vbo, &e->used, v, nfloats);
    neox_result r;
    if (off == NSUIntegerMax) {
        ctx->last_error = "MTL vertex buffer exhausted";
        return NEOX_ERROR_BACKEND;
    }
    r = mtl_rt_acquire(ctx, g, e->SSW, e->SSH, 1, &tmp);
    if (r != NEOX_OK) return r;
    enc = mtl_pass(g, e, tmp->tex, MTLLoadActionDontCare);
    mtl_emit(enc, ptmp, e, off, 6, fsu, fsulen, t0, t1, t2);
    [enc endEncoding];
    mtl_copy_back(g, e, tmp, layer, minx, miny, maxx, maxy);
    r = mtl_hold(ctx, e, tmp);   /* 保持占用至完成（拷贝回后仍被本 CB 读过） */
    if (r == NEOX_OK) return NEOX_OK;
    mtl_rt_release(tmp);
    return r;
}

static neox_result mtl_fill_path(neox_device *dev, nx_mtl_dev *g, nx_mtl_enc *e,
                                 const nx_path_slot *ps, neox_affine ctm,
                                 const neox_color *color, neox_blend blend,
                                 nx_mtl_rt *layer)
{
    neox_context *ctx = dev->ctx;
    nx_flat flat;
    float minx, miny, maxx, maxy, paint[4], q[12], fb[4];
    float x0, y0, x1, y1;
    nx_mtl_rt *mrt, *prt;
    id<MTLRenderCommandEncoder> enc;
    NSUInteger off;
    uint32_t i;
    neox_result r;

    r = nx_flatten_path(ctx, ps, ctm, &flat);
    if (r != NEOX_OK) return r;
    if (flat.npts / 2 < 3 || flat.npolys == 0) {
        nx_flat_free(ctx, &flat);
        return NEOX_OK;
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
    r = mtl_fill_prep(ctx, g, ps, ctm, paint, e->W, e->H, &mrt, &prt);
    if (r != NEOX_OK) return r;

    x0 = floorf(minx); y0 = floorf(miny);
    x1 = ceilf(maxx);  y1 = ceilf(maxy);
    q[0] = x0; q[1] = y0; q[2] = x1; q[3] = y0; q[4] = x1; q[5] = y1;
    q[6] = x0; q[7] = y0; q[8] = x1; q[9] = y1; q[10] = x0; q[11] = y1;
    fb[0] = (float)NEOX_MTL_SS; fb[1] = (float)blend; fb[2] = paint[3]; fb[3] = 0.0f;

    if (blend == NEOX_BLEND_PLUS) {
        r = mtl_draw_pingpong(ctx, g, e, layer, g->ps_plusfill,
                              prt->tex, mrt->tex, layer->tex,
                              fb, sizeof(fb), q, 12, x0, y0, x1, y1);
    } else if (NX_MTL_COV[blend].dual && !g->dual_ok) {
        r = mtl_draw_pingpong(ctx, g, e, layer, g->ps_manual,
                              prt->tex, mrt->tex, layer->tex,
                              fb, sizeof(fb), q, 12, x0, y0, x1, y1);
    } else {
        off = mtl_stage(e->vbo, &e->used, q, 12);
        if (off == NSUIntegerMax) {
            ctx->last_error = "MTL vertex buffer exhausted";
            r = NEOX_ERROR_BACKEND;
        } else {
            enc = mtl_pass(g, e, layer->tex, MTLLoadActionLoad);
            mtl_emit(enc, NX_MTL_COV[blend].dual ? g->ps_fill_ds[blend]
                                                 : g->ps_fill[blend],
                     e, off, 6, fb, sizeof(fb), mrt->tex, prt->tex, nil);
            [enc endEncoding];
        }
    }
    /* 保持占用至 command buffer 完成（replaceRegion 与 pending GPU 读无隐式
     * 同步：立即归池会被下一 fill 复用并覆盖本 pass 将要读取的内容） */
    if (r == NEOX_OK) r = mtl_hold(ctx, e, mrt);
    if (r == NEOX_OK) r = mtl_hold(ctx, e, prt);
    if (r != NEOX_OK) {
        mtl_rt_release(mrt);
        mtl_rt_release(prt);
    }
    return r;
}

static neox_result mtl_draw_tex(neox_device *dev, nx_mtl_dev *g, nx_mtl_enc *e,
                                const nx_tex_slot *tx, const nx_cmd *cmd,
                                neox_affine ctm, neox_blend blend,
                                nx_mtl_rt *layer)
{
    uint32_t idx;
    float q[12], ix[4], iy[4], ut[28], xc[4], yc[4];
    neox_affine inv;
    int k;
    id<MTLRenderCommandEncoder> enc;
    NSUInteger off;
    neox_result r;
    if (cmd->dst.w == 0.0f || cmd->dst.h == 0.0f) return NEOX_OK;
    if (nx_affine_det(ctm) == 0.0f) return NEOX_OK; /* §3 退化变换 */
    if (!nx_affine_invert(ctm, &inv)) return NEOX_OK;
    idx = (uint32_t)(tx - (const nx_tex_slot *)dev->textures.slots);
    r = mtl_tex_ensure(dev->ctx, g, dev, idx);
    if (r != NEOX_OK) return r;
    nx_affine_apply(ctm, cmd->dst.x, cmd->dst.y, &xc[0], &yc[0]);
    nx_affine_apply(ctm, cmd->dst.x + cmd->dst.w, cmd->dst.y, &xc[1], &yc[1]);
    nx_affine_apply(ctm, cmd->dst.x + cmd->dst.w, cmd->dst.y + cmd->dst.h,
                    &xc[2], &yc[2]);
    nx_affine_apply(ctm, cmd->dst.x, cmd->dst.y + cmd->dst.h, &xc[3], &yc[3]);
    mtl_inflate_quad(xc, yc, 0.6f, ix, iy);
    for (k = 0; k < 4; k++) { ix[k] *= NEOX_MTL_SS; iy[k] *= NEOX_MTL_SS; }
    q[0] = ix[0]; q[1] = iy[0]; q[2] = ix[1]; q[3] = iy[1];
    q[4] = ix[2]; q[5] = iy[2]; q[6] = ix[0]; q[7] = iy[0];
    q[8] = ix[2]; q[9] = iy[2]; q[10] = ix[3]; q[11] = iy[3];
    ut[0] = (float)NEOX_MTL_SS; ut[1] = (float)blend;
    ut[2] = cmd->filter == NEOX_FILTER_LINEAR ? 1.0f : 0.0f;
    ut[3] = cmd->tex_opacity;
    ut[4] = cmd->src.x; ut[5] = cmd->src.y; ut[6] = cmd->src.w; ut[7] = cmd->src.h;
    ut[8] = cmd->dst.x; ut[9] = cmd->dst.y; ut[10] = cmd->dst.w; ut[11] = cmd->dst.h;
    ut[12] = (float)tx->width; ut[13] = (float)tx->height; ut[14] = 0; ut[15] = 0;
    ut[16] = inv.a; ut[17] = inv.b; ut[18] = 0; ut[19] = 0;
    ut[20] = inv.c; ut[21] = inv.d; ut[22] = 0; ut[23] = 0;
    ut[24] = inv.tx; ut[25] = inv.ty; ut[26] = 1; ut[27] = 0;
    off = mtl_stage(e->vbo, &e->used, q, 12);
    if (off == NSUIntegerMax) {
        dev->ctx->last_error = "MTL vertex buffer exhausted";
        return NEOX_ERROR_BACKEND;
    }
    enc = mtl_pass(g, e, layer->tex, MTLLoadActionLoad);
    mtl_emit(enc, NX_MTL_COV[blend].dual ? g->ps_tex_ds[blend] : g->ps_tex[blend],
             e, off, 6, ut, sizeof(ut), g->texs[idx].tex, nil, nil);
    [enc endEncoding];
    if (blend == NEOX_BLEND_PLUS)
        return mtl_clamp_layer(dev->ctx, g, e, layer);
    return NEOX_OK;
}

static neox_result mtl_draw_glyphs(neox_device *dev, nx_mtl_dev *g, nx_mtl_enc *e,
                                   const nx_tex_slot *at, const nx_cmd *cmd,
                                   neox_affine ctm, neox_blend blend,
                                   nx_mtl_rt *layer)
{
    uint32_t idx, gi;
    neox_affine inv;
    float paint[4];
    neox_result r = NEOX_OK;
    int dual;
    if (nx_affine_det(ctm) == 0.0f) return NEOX_OK; /* §3 */
    if (!nx_affine_invert(ctm, &inv)) return NEOX_OK;
    nx_paint_to_premul_linear(cmd->color, paint);
    dual = NX_MTL_COV[blend].dual && g->dual_ok;
    idx = (uint32_t)(at - (const nx_tex_slot *)dev->textures.slots);
    r = mtl_tex_ensure(dev->ctx, g, dev, idx);
    if (r != NEOX_OK) return r;
    for (gi = 0; gi < cmd->glyph_count; gi++) {
        const neox_glyph *gl2 = &cmd->glyphs[gi];
        float gw = (float)gl2->sw, gh = (float)gl2->sh;
        float xc[4], yc[4], q[12], ix[4], iy[4], ug[32], bx0, by0, bx1, by1;
        float s_fac = 1.0f;
        int k;
        id<MTLRenderCommandEncoder> enc;
        NSUInteger off;
        if (gw == 0.0f || gh == 0.0f) continue;
        nx_affine_apply(ctm, gl2->x, gl2->y, &xc[0], &yc[0]);
        nx_affine_apply(ctm, gl2->x + gw, gl2->y, &xc[1], &yc[1]);
        nx_affine_apply(ctm, gl2->x + gw, gl2->y + gh, &xc[2], &yc[2]);
        nx_affine_apply(ctm, gl2->x, gl2->y + gh, &xc[3], &yc[3]);
        mtl_inflate_quad(xc, yc, 0.6f, ix, iy);
        bx0 = ix[0]; by0 = iy[0]; bx1 = ix[0]; by1 = iy[0];
        for (k = 1; k < 4; k++) {
            if (ix[k] < bx0) bx0 = ix[k];
            if (ix[k] > bx1) bx1 = ix[k];
            if (iy[k] < by0) by0 = iy[k];
            if (iy[k] > by1) by1 = iy[k];
        }
        for (k = 0; k < 4; k++) { ix[k] *= NEOX_MTL_SS; iy[k] *= NEOX_MTL_SS; }
        q[0] = ix[0]; q[1] = iy[0]; q[2] = ix[1]; q[3] = iy[1];
        q[4] = ix[2]; q[5] = iy[2]; q[6] = ix[0]; q[7] = iy[0];
        q[8] = ix[2]; q[9] = iy[2]; q[10] = ix[3]; q[11] = iy[3];
        ug[0] = (float)NEOX_MTL_SS; ug[1] = (float)blend; ug[2] = 0; ug[3] = 0;
        ug[4] = paint[0]; ug[5] = paint[1]; ug[6] = paint[2]; ug[7] = paint[3];
        ug[8] = gl2->x; ug[9] = gl2->y; ug[10] = gw; ug[11] = gh;
        ug[12] = (float)gl2->sx; ug[13] = (float)gl2->sy; ug[14] = gw; ug[15] = gh;
        ug[16] = inv.a; ug[17] = inv.b; ug[18] = 0; ug[19] = 0;
        ug[20] = inv.c; ug[21] = inv.d; ug[22] = 0; ug[23] = 0;
        ug[24] = inv.tx; ug[25] = inv.ty; ug[26] = 1; ug[27] = 0;
        /* 图集语义（§8.2）：s 因子与 soft.c/gl.c 同式（线性部分奇异值平均） */
        if (cmd->atlas_mode == NEOX_ATLAS_SDF ||
            cmd->atlas_mode == NEOX_ATLAS_MSDF)
            s_fac = nx_glyph_scale_factor(ctm);
        ug[28] = (float)cmd->atlas_mode;
        ug[29] = cmd->dist_range;
        ug[30] = cmd->softness;
        ug[31] = s_fac;
        if (blend == NEOX_BLEND_PLUS) {
            r = mtl_draw_pingpong(dev->ctx, g, e, layer, g->ps_plusglyph,
                                  g->texs[idx].tex, layer->tex, nil,
                                  ug, sizeof(ug), q, 12,
                                  floorf(bx0), floorf(by0), ceilf(bx1), ceilf(by1));
            if (r != NEOX_OK) return r;
        } else if (NX_MTL_COV[blend].dual && !g->dual_ok) {
            r = mtl_draw_pingpong(dev->ctx, g, e, layer, g->ps_manual_glyph,
                                  g->texs[idx].tex, layer->tex, nil,
                                  ug, sizeof(ug), q, 12,
                                  floorf(bx0), floorf(by0), ceilf(bx1), ceilf(by1));
            if (r != NEOX_OK) return r;
        } else {
            off = mtl_stage(e->vbo, &e->used, q, 12);
            if (off == NSUIntegerMax) {
                dev->ctx->last_error = "MTL vertex buffer exhausted";
                return NEOX_ERROR_BACKEND;
            }
            enc = mtl_pass(g, e, layer->tex, MTLLoadActionLoad);
            mtl_emit(enc, dual ? g->ps_glyph_ds[blend] : g->ps_glyph[blend],
                     e, off, 6, ug, sizeof(ug), g->texs[idx].tex, nil, nil);
            [enc endEncoding];
        }
    }
    return r;
}

/* ------------------------------------------------------------------ 帧末 resolve
 * GPU 完成后（poll 内执行）：读回浮点层 → 盒滤波 → 用 Soft 同一函数量化
 * → replaceRegion 写入表面（唯一写表面的步骤，§10.1 原子性） */
static neox_result mtl_resolve(neox_context *ctx, nx_mtl_dev *g, nx_mtl_rt *base,
                               neox_mtl_surface *surf)
{
    uint32_t W = surf->w, H = surf->h;
    uint32_t SSW = W * NEOX_MTL_SS, SSH = H * NEOX_MTL_SS;
    float *ssb;
    uint8_t *outb;
    uint32_t x, y, i, j;
    int c;
    size_t fbytes = (size_t)SSW * SSH * 16;
    (void)g;
    ssb = (float *)nx_alloc(ctx, fbytes, 16);
    outb = (uint8_t *)nx_alloc(ctx, (size_t)W * H * 4, 16);
    if (!ssb || !outb) {
        if (ssb) nx_free(ctx, ssb, fbytes, 16);
        if (outb) nx_free(ctx, outb, (size_t)W * H * 4, 16);
        ctx->last_error = "resolve buffer allocation failed";
        return NEOX_ERROR_OUT_OF_MEMORY;
    }
    [base->tex getBytes:ssb bytesPerRow:(NSUInteger)SSW * 16
             fromRegion:MTLRegionMake2D(0, 0, SSW, SSH)
            mipmapLevel:0];
    /* 行序：顶点映射 ndc_y = 1−2y/H 使设备行 0 == 纹理行 0，无需翻转 */
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            float p[4];
            uint8_t q[4];
            for (j = 0; j < NEOX_MTL_SS; j++) {
                for (i = 0; i < NEOX_MTL_SS; i++) {
                    const float *t =
                        &ssb[(((size_t)y * NEOX_MTL_SS + j) * SSW +
                              (size_t)x * NEOX_MTL_SS + i) * 4];
                    for (c = 0; c < 4; c++) acc[c] += t[c];
                }
            }
            for (c = 0; c < 4; c++)
                p[c] = acc[c] / (float)(NEOX_MTL_SS * NEOX_MTL_SS);
            nx_premul_linear_to_srgb8(p, q);
            memcpy(&outb[((size_t)y * W + x) * 4], q, 4);
        }
    }
    [surf->tex replaceRegion:MTLRegionMake2D(0, 0, W, H)
                 mipmapLevel:0
                   withBytes:outb
                 bytesPerRow:(NSUInteger)W * 4u];
    nx_free(ctx, ssb, fbytes, 16);
    nx_free(ctx, outb, (size_t)W * H * 4, 16);
    return NEOX_OK;
}

/* ------------------------------------------------------------------ 执行引擎
 * 只编码 + 提交：submission 保持 PENDING，完成事件由 completedHandler 登记
 * （回调只触碰 lock/completed 标志与 vbo 释放），poll 收割时做 resolve 与
 * 状态推进。编码期错误置 FAILED 并返回（pump 按失败收割）。 */
static neox_result mtl_execute(neox_device *dev, nx_sub_slot *sub)
{
    neox_context *ctx = dev->ctx;
    nx_mtl_dev *g = (nx_mtl_dev *)dev->bstate;
    neox_mtl_surface *surf = (neox_mtl_surface *)sub->surface;
    uint32_t W = sub->width, H = sub->height;
    uint32_t SSW = W * NEOX_MTL_SS, SSH = H * NEOX_MTL_SS;
    nx_mtl_rt **layers = NULL, **clips = NULL;
    uint32_t nlayers = 0, caplayers = 0, nclips = 0, capclips = 0;
    uint32_t nstk = 0, capstk = 0, max_layers = 1, i, glyphs_total = 0;
    uint32_t sidx;
    size_t vbo_len;
    nx_mtl_enc e;
    id<MTLCommandBuffer> cb;
    id<MTLBuffer> vbo = nil;
    nx_mtl_rt *base = nil;
    neox_result r = NEOX_ERROR_OUT_OF_MEMORY;
    int failed = 0;

    typedef struct mtl_ex_state {
        neox_affine ctm;
        float opacity;
        neox_blend blend;
        const float *clip;
        int32_t layer;
    } mtl_ex_state;
    mtl_ex_state *stk = NULL;

    sub->report.algorithm = NEOX_ALG_TRIANGLE;
    if (!surf) {
        ctx->last_error = "submission has no MTL surface";
        sub->status = NEOX_SUBMISSION_FAILED;
        return NEOX_ERROR_BACKEND;
    }
    sidx = (uint32_t)(sub - (nx_sub_slot *)dev->subs.slots);

    @autoreleasepool {
        cb = [[g->queue commandBuffer] retain];

        for (i = 0; i < sub->cmd_count; i++) {
            if (sub->cmds[i].op == NXC_GLYPHS)
                glyphs_total += sub->cmds[i].glyph_count;
        }
        vbo_len = (size_t)(sub->cmd_count * 2 + glyphs_total + 8) * 1024 + 4096;
        vbo = [g->device newBufferWithLength:vbo_len options:0];  /* +1 */
        if (!cb || !vbo) {
            ctx->last_error = "MTL command buffer/vertex buffer allocation failed";
            r = NEOX_ERROR_OUT_OF_MEMORY;
            goto enc_fail;
        }
        e.cb = cb; e.vbo = vbo; e.used = 0;
        e.W = W; e.H = H; e.SSW = SSW; e.SSH = SSH;
        e.held = NULL; e.nheld = 0; e.capheld = 0;

        /* 基底装载（快照或清屏色 → 快照纹理 → 放大 blit 进基底层） */
        {
            nx_mtl_rt *base2, *snap_rt = NULL;
            r = mtl_rt_acquire(ctx, g, SSW, SSH, 1, &base2);
            if (r != NEOX_OK) goto enc_fail;
            base = base2;
            r = mtl_rt_acquire(ctx, g, W, H, 1, &snap_rt);
            if (r != NEOX_OK) goto enc_fail;
            mtl_upload(snap_rt->tex, W, H, sub->accum, W * 16u);
            r = mtl_hold(ctx, &e, snap_rt);
            if (r != NEOX_OK) { mtl_rt_release(snap_rt); goto enc_fail; }

            {
                id<MTLRenderCommandEncoder> enc;
                float v[6];
                NSUInteger off;
                float ublit[4] = { (float)NEOX_MTL_SS, 0.0f, 0.0f, 0.0f };
                enc = mtl_pass(g, &e, base->tex, MTLLoadActionDontCare);
                off = mtl_stage_full(&e, (float)SSW, (float)SSH, v);
                if (off == NSUIntegerMax) {
                    [enc endEncoding];
                    ctx->last_error = "MTL vertex buffer exhausted";
                    r = NEOX_ERROR_BACKEND;
                    goto enc_fail;
                }
                mtl_emit(enc, g->ps_blit, &e, off, 3, ublit, sizeof(ublit),
                         snap_rt->tex, nil, nil);
                [enc endEncoding];
            }
        }

        {
            nx_mtl_rt **nb = (nx_mtl_rt **)nx_alloc(ctx, sizeof(nx_mtl_rt *) * 8, 16);
            mtl_ex_state *ns = (mtl_ex_state *)nx_alloc(ctx, sizeof(mtl_ex_state) * 8, 16);
            if (!nb || !ns) {
                if (nb) nx_free(ctx, nb, sizeof(nx_mtl_rt *) * 8, 16);
                if (ns) nx_free(ctx, ns, sizeof(mtl_ex_state) * 8, 16);
                if (base) mtl_rt_release(base);
                base = nil;
                ctx->last_error = "MTL execute stack allocation failed";
                goto enc_fail;
            }
            layers = nb; caplayers = 8;
            stk = ns; capstk = 8;
            layers[0] = base;
            nlayers = 1;
            stk[0].ctm = nx_affine_identity();
            stk[0].opacity = 1.0f;
            stk[0].blend = NEOX_BLEND_SRC_OVER;
            stk[0].clip = NULL;
            stk[0].layer = 0;
            nstk = 1;
        }

        for (i = 0; i < sub->cmd_count; i++) {
            const nx_cmd *c = &sub->cmds[i];
            mtl_ex_state *top = &stk[nstk - 1];
            switch (c->op) {
            case NXC_PUSH: {
                mtl_ex_state *nst;
                int32_t layer_idx = top->layer;
                if (c->owns_layer) {
                    nx_mtl_rt *lb;
                    nx_mtl_rt **nl;
                    r = mtl_rt_acquire(ctx, g, SSW, SSH, 1, &lb);
                    if (r != NEOX_OK) goto enc_fail;
                    nl = (nx_mtl_rt **)nx_array_grow(ctx, layers,
                                sizeof(nx_mtl_rt *), &caplayers, nlayers + 1);
                    if (!nl) {
                        mtl_rt_release(lb);
                        ctx->last_error = "MTL layer stack allocation failed";
                        r = NEOX_ERROR_OUT_OF_MEMORY;
                        goto enc_fail;
                    }
                    layers = nl;
                    layers[nlayers] = lb;
                    layer_idx = (int32_t)nlayers;
                    nlayers++;
                    if (nlayers > max_layers) max_layers = nlayers;
                    mtl_clear_layer(g, &e, lb);
                }
                nst = (mtl_ex_state *)nx_array_grow(ctx, stk, sizeof(mtl_ex_state),
                                                    &capstk, nstk + 1);
                if (!nst) {
                    if (c->owns_layer) {
                        mtl_rt_release(layers[nlayers - 1]);
                        nlayers--;
                    }
                    ctx->last_error = "MTL state stack allocation failed";
                    r = NEOX_ERROR_OUT_OF_MEMORY;
                    goto enc_fail;
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
                mtl_ex_state *top2 = &stk[nstk - 1];
                mtl_ex_state *parent = &stk[nstk - 2];
                if (top2->layer != parent->layer) {
                    nx_mtl_rt *clip_rt = nil;
                    id<MTLRenderCommandEncoder> enc;
                    float v[6];
                    NSUInteger off;
                    float uc[4];
                    if (top2->clip) {
                        nx_mtl_rt **nc;
                        r = mtl_rt_acquire(ctx, g, W, H, 0, &clip_rt);
                        if (r != NEOX_OK) goto enc_fail;
                        mtl_upload(clip_rt->tex, W, H, top2->clip,
                                   ((W * 4u + 15u) & ~15u));
                        nc = (nx_mtl_rt **)nx_array_grow(ctx, clips,
                                    sizeof(nx_mtl_rt *), &capclips, nclips + 1);
                        if (!nc) {
                            mtl_rt_release(clip_rt);
                            ctx->last_error = "MTL clip stack allocation failed";
                            r = NEOX_ERROR_OUT_OF_MEMORY;
                            goto enc_fail;
                        }
                        clips = nc;
                        clips[nclips++] = clip_rt;
                    }
                    enc = mtl_pass(g, &e, layers[parent->layer]->tex,
                                   MTLLoadActionLoad);
                    uc[0] = (float)NEOX_MTL_SS;
                    uc[1] = top2->opacity;
                    uc[2] = clip_rt ? 1.0f : 0.0f;
                    uc[3] = 0;
                    off = mtl_stage_full(&e, (float)SSW, (float)SSH, v);
                    if (off == NSUIntegerMax) {
                        [enc endEncoding];
                        ctx->last_error = "MTL vertex buffer exhausted";
                        r = NEOX_ERROR_BACKEND;
                        goto enc_fail;
                    }
                    mtl_emit(enc, g->ps_comp[top2->blend], &e, off, 3,
                             uc, sizeof(uc), layers[top2->layer]->tex,
                             clip_rt ? clip_rt->tex : nil, nil);
                    [enc endEncoding];
                    if (top2->blend == NEOX_BLEND_PLUS) {
                        r = mtl_clamp_layer(ctx, g, &e, layers[parent->layer]);
                        if (r != NEOX_OK) goto enc_fail;
                    }
                    if (clip_rt) {
                        clips[nclips - 1] = nil;
                        nclips--;
                        r = mtl_hold(ctx, &e, clip_rt);
                        if (r != NEOX_OK) { mtl_rt_release(clip_rt); goto enc_fail; }
                    }
                    mtl_rt_release(layers[top2->layer]);
                    nlayers--;
                }
                nstk--;
                break;
            }
            case NXC_PATH: {
                const nx_path_slot *ps =
                    &((nx_path_slot *)dev->paths.slots)[c->res_idx];
                r = mtl_fill_path(dev, g, &e, ps, top->ctm, &c->color,
                                  top->blend, layers[top->layer]);
                if (r != NEOX_OK) goto enc_fail;
                break;
            }
            case NXC_TEX: {
                const nx_tex_slot *tx =
                    &((nx_tex_slot *)dev->textures.slots)[c->res_idx];
                r = mtl_draw_tex(dev, g, &e, tx, c, top->ctm, top->blend,
                                 layers[top->layer]);
                if (r != NEOX_OK) goto enc_fail;
                break;
            }
            case NXC_GLYPHS: {
                const nx_tex_slot *at =
                    &((nx_tex_slot *)dev->textures.slots)[c->res_idx];
                r = mtl_draw_glyphs(dev, g, &e, at, c, top->ctm, top->blend,
                                    layers[top->layer]);
                if (r != NEOX_OK) goto enc_fail;
                break;
            }
            default:
                ctx->last_error = "unknown command op";
                r = NEOX_ERROR_BACKEND;
                goto enc_fail;
            }
        }
        [vbo didModifyRange:NSMakeRange(0, e.used)];

        /* 完成回调：只登记 completed 标志（不触碰 neox 状态与资源所有权） */
        {
            nx_mtl_dev *gc = g;
            uint32_t sidx_c = sidx;
            [cb addCompletedHandler:^(id<MTLCommandBuffer> mcb) {
                uint32_t k;
                (void)mcb;
                pthread_mutex_lock(&gc->lock);
                for (k = 0; k < gc->ninflight; k++) {
                    if (gc->inflight[k].sub_idx == sidx_c) {
                        gc->inflight[k].completed = 1;
                        break;
                    }
                }
                pthread_mutex_unlock(&gc->lock);
            }];
        }
        /* 在途登记（供 poll 收割与 block 等待）；基底层保持占用至 resolve。
         * 完成回调在 Metal 线程扫描同一数组：扩容与登记必须在 lock 保护下
         * 进行（否则回调可能扫描到 realloc 前的旧数组） */
        {
            nx_mtl_inflight *ni;
            pthread_mutex_lock(&g->lock);
            ni = (nx_mtl_inflight *)nx_array_grow(ctx, g->inflight,
                    sizeof(nx_mtl_inflight), &g->capinflight, g->ninflight + 1);
            if (!ni) {
                pthread_mutex_unlock(&g->lock);
                ctx->last_error = "MTL inflight list allocation failed";
                r = NEOX_ERROR_OUT_OF_MEMORY;
                goto enc_fail;
            }
            g->inflight = ni;
            g->inflight[g->ninflight].sub_idx = sidx;
            g->inflight[g->ninflight].cb = cb;       /* +1 所有权转移 */
            g->inflight[g->ninflight].vbo = vbo;     /* +1 所有权转移 */
            g->inflight[g->ninflight].base = base;   /* 池内保持 in_use */
            g->inflight[g->ninflight].held = e.held;
            g->inflight[g->ninflight].nheld = e.nheld;
            g->inflight[g->ninflight].completed = 0;
            g->ninflight++;
            pthread_mutex_unlock(&g->lock);
        }
        sub->report.peak_bytes =
            (uint64_t)max_layers * (uint64_t)SSW * SSH * 16u;
        [cb commit];
        /* cb/vbo/base 所有权转移至 inflight（enc_fail 路径不经过此处） */
        cb = nil;
        vbo = nil;
        base = nil;             /* inflight 持有（池 in_use） */
        r = NEOX_OK;
    }

enc_fail:
    if (r != NEOX_OK) {
        uint32_t k;
        sub->status = NEOX_SUBMISSION_FAILED;
        failed = 1;
        [cb release];
        [vbo release];
        for (k = 0; k < nlayers; k++)
            if (layers[k]) mtl_rt_release(layers[k]);
        for (k = 0; k < nclips; k++)
            if (clips[k]) mtl_rt_release(clips[k]);
        for (k = 0; k < e.nheld; k++)
            mtl_rt_release(e.held[k]);
        if (e.held)
            nx_free(ctx, e.held, (size_t)e.capheld * sizeof(nx_mtl_rt *), 16);
        e.held = NULL;
    }
    (void)failed;
    if (layers) nx_free(ctx, layers, (size_t)caplayers * sizeof(nx_mtl_rt *), 16);
    if (clips) nx_free(ctx, clips, (size_t)capclips * sizeof(nx_mtl_rt *), 16);
    if (stk) nx_free(ctx, stk, (size_t)capstk * sizeof(mtl_ex_state), 16);
    return r;
}

/* ------------------------------------------------------------------ 异步收割
 * poll（pump 内调用，调用线程）：收割完成事件 → resolve → COMPLETE →
 * 载荷释放。返回新完成的 submission 数。 */
static uint32_t mtl_poll(neox_device *dev)
{
    nx_mtl_dev *g = (nx_mtl_dev *)dev->bstate;
    uint32_t done = 0;
    uint32_t k = 0;
    if (!g) return 0;
    for (;;) {
        uint32_t sidx = 0, nheld = 0, h2;
        id<MTLCommandBuffer> cb = nil;
        id<MTLBuffer> vb = nil;
        nx_mtl_rt *base = nil;
        nx_mtl_rt **held = NULL;
        nx_sub_slot *sub;
        int found = 0;
        pthread_mutex_lock(&g->lock);
        for (; k < g->ninflight; k++) {
            if (g->inflight[k].completed) {
                sidx = g->inflight[k].sub_idx;
                cb = g->inflight[k].cb;
                vb = g->inflight[k].vbo;
                base = g->inflight[k].base;
                held = g->inflight[k].held;
                nheld = g->inflight[k].nheld;
                g->inflight[k] = g->inflight[g->ninflight - 1];
                g->ninflight--;
                found = 1;
                break;
            }
        }
        pthread_mutex_unlock(&g->lock);
        if (!found) break;
        sub = &((nx_sub_slot *)dev->subs.slots)[sidx];
        /* 成功路径的最后一步才触碰表面（§10.1 原子性） */
        if (mtl_resolve(dev->ctx, g, base, (neox_mtl_surface *)sub->surface)
                == NEOX_OK) {
            sub->status = NEOX_SUBMISSION_COMPLETE;
        } else {
            sub->status = NEOX_SUBMISSION_FAILED;
        }
        for (h2 = 0; h2 < nheld; h2++)
            mtl_rt_release(held[h2]);  /* GPU 已完成：归还执行期占用纹理 */
        if (held)
            nx_free(dev->ctx, held, (size_t)nheld * sizeof(nx_mtl_rt *), 16);
        mtl_rt_release(base);
        [cb release];   /* inflight 的 cb 引用 */
        [vb release];   /* inflight 的 vbo 引用 */
        nx_sub_release_payload(dev, sub);
        done++;
        k = 0; /* swap-remove 打乱了顺序，从头扫描 */
    }
    return done;
}

/* block（submission_wait）：等待最旧在途 command buffer 完成（真实进度，
 * 回调随即置位 completed，下一次 pump 收割） */
static void mtl_block(neox_device *dev)
{
    nx_mtl_dev *g = (nx_mtl_dev *)dev->bstate;
    id<MTLCommandBuffer> cb = nil;
    if (!g) return;
    pthread_mutex_lock(&g->lock);
    if (g->ninflight > 0) cb = [g->inflight[0].cb retain];
    pthread_mutex_unlock(&g->lock);
    if (cb) {
        [cb waitUntilCompleted];
        [cb release];
    }
}

/* ------------------------------------------------------------------ 表面 API（neox_mtl.h） */

neox_result neox_mtl_surface_create(neox_device *dev,
                                    uint32_t width, uint32_t height,
                                    neox_mtl_surface **out_surface)
{
    neox_context *ctx;
    nx_mtl_dev *g;
    neox_mtl_surface *s;
    neox_mtl_surface **ns;
    MTLTextureDescriptor *td;
    if (!dev || !out_surface) return NEOX_ERROR_INVALID_ARGUMENT;
    *out_surface = NULL;
    if (dev->kind != NEOX_BACKEND_GPU) return NEOX_ERROR_INVALID_ARGUMENT;
    ctx = dev->ctx;
    g = (nx_mtl_dev *)dev->bstate;
    (void)g;
    if (width == 0 || height == 0 || width > 16384u || height > 16384u)
        NX_ERR(ctx, "invalid surface size", NEOX_ERROR_INVALID_ARGUMENT);
    s = (neox_mtl_surface *)nx_alloc(ctx, sizeof(neox_mtl_surface), 16);
    if (!s) NX_ERR(ctx, "surface allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    memset(s, 0, sizeof(*s));
    s->dev = dev;
    s->w = width;
    s->h = height;
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:width height:height mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    s->tex = [g->device newTextureWithDescriptor:td];  /* +1 */
    if (!s->tex) {
        nx_free(ctx, s, sizeof(neox_mtl_surface), 16);
        NX_ERR(ctx, "MTL surface allocation failed", NEOX_ERROR_BACKEND);
    }
    ns = (neox_mtl_surface **)nx_array_grow(ctx, g->surfs, sizeof(void *),
                                            &g->capsurfs, g->nsurfs + 1);
    if (!ns) {
        [s->tex release];
        nx_free(ctx, s, sizeof(neox_mtl_surface), 16);
        NX_ERR(ctx, "surface registry allocation failed",
               NEOX_ERROR_OUT_OF_MEMORY);
    }
    g->surfs = ns;
    g->surfs[g->nsurfs++] = s;
    *out_surface = s;
    return NEOX_OK;
}

void neox_mtl_surface_destroy(neox_device *dev, neox_mtl_surface *surface)
{
    nx_mtl_dev *g;
    uint32_t i;
    if (!dev || !surface) return;
    g = (nx_mtl_dev *)dev->bstate;
    if (!g) return;
    [surface->tex release];
    for (i = 0; i < g->nsurfs; i++) {
        if (g->surfs[i] == surface) {
            g->surfs[i] = g->surfs[g->nsurfs - 1];
            g->nsurfs--;
            break;
        }
    }
    nx_free(dev->ctx, surface, sizeof(neox_mtl_surface), 16);
}

neox_result neox_mtl_surface_read_pixels(neox_mtl_surface *surface,
                                         uint8_t *out_pixels)
{
    nx_mtl_dev *g;
    uint32_t stride, y;
    uint8_t *tmp;
    if (!surface || !out_pixels) return NEOX_ERROR_INVALID_ARGUMENT;
    g = (nx_mtl_dev *)surface->dev->bstate;
    if (!g) return NEOX_ERROR_BACKEND;
    /* 正式读回通道：同步在途提交（resolve 由 poll 在完成后执行） */
    for (;;) {
        int any;
        pthread_mutex_lock(&g->lock);
        any = g->ninflight > 0;
        pthread_mutex_unlock(&g->lock);
        if (!any) break;
        mtl_block(surface->dev);  /* 等待最旧在途完成 */
        (void)mtl_poll(surface->dev); /* 收割（含 resolve） */
    }
    stride = (surface->w * 4u + 15u) & ~15u;
    tmp = (uint8_t *)nx_alloc(surface->dev->ctx, (size_t)stride * surface->h, 16);
    if (!tmp) return NEOX_ERROR_OUT_OF_MEMORY;
    [surface->tex getBytes:tmp bytesPerRow:stride
              fromRegion:MTLRegionMake2D(0, 0, surface->w, surface->h)
             mipmapLevel:0];
    for (y = 0; y < surface->h; y++)
        memcpy(out_pixels + (size_t)y * surface->w * 4,
               tmp + (size_t)y * stride, (size_t)surface->w * 4);
    nx_free(surface->dev->ctx, tmp, (size_t)stride * surface->h, 16);
    return NEOX_OK;
}

/* ------------------------------------------------------------------ Metal 后端 vtable */

const nx_backend *nx_backend_mtl(void)
{
    static const nx_backend vt = {
        "metal",
        mtl_device_init,
        mtl_device_shutdown,
        mtl_begin_target,
        mtl_execute,
        mtl_get_caps,
        mtl_poll,           /* 真异步：completedHandler 事件收割 */
        mtl_block           /* submission_wait 的进度等待 */
    };
    return &vt;
}
