/* ===========================================================================
 * neox_mtl.h — NeoX Metal 后端公开接口
 *
 * 状态：随 NEOX_API_DRAFT，后端专属扩展。仅当构建启用 NEOX_BACKEND_METAL
 * （macOS 原生，Objective-C++ 实现，零第三方依赖）时可用。
 *
 * 与 GL 后端共享 neox_backend_kind 的 NEOX_BACKEND_GPU：经 platform 参数块
 * 的 magic 字段区分（见 neox_device_desc 约定）。Metal 后端是核心首个
 * 真异步实现：submission 状态经 neox_device_pump 收割完成事件推进，
 * neox_device_pump 不阻塞；neox_submission_wait 内部阻塞至完成。
 *
 * 用法：
 *   neox_mtl_device_desc mtd;
 *   memset(&mtd, 0, sizeof(mtd));
 *   mtd.struct_size = sizeof(mtd);                     // 首字段：结构体版本
 *   mtd.magic = NEOX_MTL_DESC_MAGIC;                   // 后端标识（Metal）
 *   mtd.device = NULL;         // 外部 MTLDevice（__bridge void*），NULL = 系统默认
 *   mtd.command_queue = NULL;  // 外部 MTLCommandQueue，NULL = 自建
 *   neox_device_desc dd;
 *   memset(&dd, 0, sizeof(dd));
 *   dd.backend = NEOX_BACKEND_GPU;
 *   dd.platform = &mtd;
 *   dd.platform_size = sizeof(mtd);
 *   neox_device_create(ctx, &dd, &dev);
 *
 *   neox_mtl_surface *surf;
 *   neox_mtl_surface_create(dev, w, h, &surf);
 *
 *   neox_target_desc td = { 0 };
 *   td.kind = NEOX_BACKEND_GPU;
 *   td.surface = surf;                                  // neox_frame_begin 目标
 *   ...
 *   neox_device_pump(dev);               // 提交；不阻塞
 *   neox_submission_wait(dev, sub);      // 需要结果时阻塞
 *   neox_mtl_surface_read_pixels(surf, pixels);
 *
 * 语义与 Soft/GL 后端完全一致（docs/semantics-v0.md §9；实测逐位一致）。
 * =========================================================================*/
#ifndef NEOX_MTL_H
#define NEOX_MTL_H

#include "neox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 设备参数
 * 经 neox_device_desc.platform 传入（[borrow]，创建时读取）。 */

#define NEOX_MTL_DESC_MAGIC 0x4e584d54u  /* 'NXMT'：NEOX_BACKEND_GPU 参数块后端标识 */

typedef struct neox_mtl_device_desc {
    uint32_t struct_size;   /* 必须 = sizeof(neox_mtl_device_desc)（版本协商） */
    uint32_t magic;         /* 必须 = NEOX_MTL_DESC_MAGIC（后端区分） */
    void *device;           /* 外部 MTLDevice（以 void* 传入），NULL = 系统默认设备 */
    void *command_queue;    /* 外部 MTLCommandQueue，NULL = 自建（每设备一个） */
} neox_mtl_device_desc;
/* 约定：platform_size >= sizeof(neox_mtl_device_desc) 且首字段 struct_size
 * 与之一致；platform_size 更大时按前向兼容处理（只用本版前缀字段）。 */

/* ------------------------------------------------------------------ 离屏表面
 * 不透明句柄；neox_frame_begin 的 target.surface 接受本指针。
 * 表面生命周期由调用方管理（[own]），须先于 device_destroy 释放。 */

typedef struct neox_mtl_surface neox_mtl_surface;

/* [own] 创建 RGBA8 sRGB 非预乘离屏表面 */
neox_result neox_mtl_surface_create(neox_device *dev,
                                    uint32_t width, uint32_t height,
                                    neox_mtl_surface **out_surface);
/* [own] 销毁表面（不得在提交尚未完成时销毁被引用的表面） */
void        neox_mtl_surface_destroy(neox_device *dev,
                                     neox_mtl_surface *surface);

/* 读回整幅表面：RGBA8 sRGB 非预乘，行距 width*4 字节，左上原点。
 * 正式读回通道（测试与无窗口环境）；内部先同步在途命令缓冲再拷贝。 */
neox_result neox_mtl_surface_read_pixels(neox_mtl_surface *surface,
                                         uint8_t *out_pixels);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NEOX_MTL_H */
