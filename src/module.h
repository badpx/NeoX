/* Private, opaque services shared with optional modules. Not installed. */
#ifndef NEOX_MODULE_H
#define NEOX_MODULE_H
#include "neox.h"
#include "checked.h"
#ifdef __cplusplus
extern "C" {
#endif
void *nx_alloc(neox_context *ctx, size_t size, size_t align);
void nx_free(neox_context *ctx, void *ptr, size_t size, size_t align);
void *nx_alloc_using(neox_context *ctx, const neox_allocator *al, size_t size, size_t align);
void nx_free_using(neox_context *ctx, const neox_allocator *al, void *ptr, size_t size, size_t align);
const neox_allocator *nx_context_allocator(neox_context *ctx);
neox_context *nx_device_context(neox_device *dev);
neox_device *nx_frame_device(neox_frame *frame);
#ifdef __cplusplus
}
#endif
#endif
