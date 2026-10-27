#ifndef NEOX_METAL_PRESENT_H
#define NEOX_METAL_PRESENT_H
#include "neox_mtl.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Private bridge: layer is a borrowed CAMetalLayer. No AppKit dependency here.
 * Caller serializes the device and completes rendering before presenting.
 * Snapshot, when non-NULL, receives the actual drawable in RGBA top-left order. */
neox_result nx_mtl_present(neox_mtl_surface *surface, void *layer, uint8_t *snapshot);
#ifdef __cplusplus
}
#endif
#endif
