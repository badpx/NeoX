#ifndef NEOX_EXECUTION_H
#define NEOX_EXECUTION_H
#include "internal.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Reference executors share this implementation, not its storage in the IR.
 * Only active clip scopes retain full/conditional masks. */
typedef struct nx_clip_scope { float *full, *conditional; } nx_clip_scope;
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
