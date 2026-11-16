#ifndef NEOX_EXECUTION_H
#define NEOX_EXECUTION_H
#include "internal.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Reference executors share this implementation, not its storage in the IR.
 * Only active clip scopes retain full/conditional masks. */
typedef struct nx_clip_scope { float *full, *conditional; } nx_clip_scope;
/* 隔离层内子绘制一律 SRC_OVER；组 blend 只在 POP 组合成时使用（P1-8）。
 * 没有隔离层的作用域（SRC_OVER、不透明、无裁剪）直接以自身 blend 绘制到
 * 父层，与组合成前的层内结果相同。Soft/GL/Metal 三个执行器共用本判定。 */
static inline neox_blend nx_exec_isolated_blend(neox_blend group_blend, int owns_layer)
{
    (void)group_blend;
    return owns_layer ? NEOX_BLEND_SRC_OVER : group_blend;
}

typedef struct nx_execution_clips {
    nx_clip_path *paths;
    nx_clip_scope *scopes;
    uint32_t count, path_capacity, scope_capacity;
    size_t bytes;
} nx_execution_clips;
neox_result nx_execution_clip_push(neox_device *dev, nx_execution_clips *clips,
    const nx_cmd *cmd, uint32_t w, uint32_t h, const float **out);
void nx_execution_clip_pop(neox_context *ctx, nx_execution_clips *clips);
void nx_execution_clips_destroy(neox_context *ctx, nx_execution_clips *clips);
#ifdef __cplusplus
}
#endif
#endif
