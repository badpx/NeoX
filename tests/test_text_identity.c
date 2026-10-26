#include "neox_text.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define OK(x) CHECK((x) == NEOX_OK)

/* Reuse freed storage deterministically, including under ASan quarantine. */
typedef struct block { void *ptr; size_t size; struct block *next; } block;
typedef struct pool { block *free; size_t live; } pool;
static void *allocate(void *user, size_t size, size_t align)
{
    pool *p = user;
    block **link = &p->free;
    CHECK(align <= 16);
    p->live += size;
    while (*link) {
        block *b = *link;
        if (b->size == size) {
            void *ptr = b->ptr;
            *link = b->next;
            free(b);
            return ptr;
        }
        link = &b->next;
    }
    void *ptr = malloc(size);
    CHECK(ptr);
    return ptr;
}
static void release(void *user, void *ptr, size_t size, size_t align)
{
    (void)align;
    pool *p = user;
    CHECK(p->live >= size);
    block *b = malloc(sizeof(*b));
    CHECK(b);
    b->ptr = ptr; b->size = size; b->next = p->free;
    p->free = b;
    p->live -= size;
}

typedef struct font_data { int width, raster_calls; } font_data;
static float advance(void *user, uint32_t glyph, float size)
{
    (void)glyph; (void)size;
    return (float)((font_data *)user)->width;
}
static neox_result raster(void *user, uint32_t glyph, float size,
                          uint8_t **pixels, int32_t *w, int32_t *h, float *x, float *y)
{
    (void)glyph; (void)size;
    font_data *data = user;
    data->raster_calls++;
    *w = data->width; *h = 1; *x = *y = 0;
    *pixels = malloc((size_t)*w);
    CHECK(*pixels);
    memset(*pixels, 255, (size_t)*w);
    return NEOX_OK;
}
static void free_bitmap(void *user, uint8_t *pixels) { (void)user; free(pixels); }
static nx_font *font_create(nx_text_context *text, font_data *data)
{
    nx_font_backend backend = {0};
    nx_font *font;
    backend.user = data;
    backend.glyph_advance = advance;
    backend.glyph_rasterize = raster;
    backend.glyph_free_bitmap = free_bitmap;
    OK(nx_font_create_custom(text, &backend, &font));
    return font;
}

int main(void)
{
    pool p = {0};
    neox_allocator alloc = {&p, allocate, release};
    neox_context_desc cd = {0}; cd.allocator = &alloc;
    neox_context *core, *foreign_core;
    neox_device *device, *foreign_device;
    nx_text_context *text, *other_text;
    nx_atlas *atlas, *invalid_atlas = NULL;
    nx_atlas_rect first, second;
    OK(neox_context_create(&cd, &core));
    OK(neox_device_create(core, NULL, &device));
    OK(nx_text_context_create(core, NULL, &text));
    OK(nx_text_context_create(core, NULL, &other_text));
    OK(nx_atlas_create(text, device, 32, 32, &atlas));

    font_data a = {1, 0}, b = {2, 0};
    nx_font *font = font_create(text, &a);
    OK(nx_atlas_glyph(atlas, font, 65, 16, &first));
    uintptr_t old_address = (uintptr_t)font;
    nx_font_destroy(font);
    font = font_create(text, &b);
    CHECK((uintptr_t)font == old_address);
    OK(nx_atlas_glyph(atlas, font, 65, 16, &second));
    CHECK(first.w == 1 && second.w == 2 && a.raster_calls == 1 && b.raster_calls == 1);
    OK(nx_atlas_glyph(atlas, font, 65, 16, &second));
    CHECK(b.raster_calls == 1); /* same live identity still caches */
    uint64_t hits, misses;
    nx_atlas_stats(atlas, &hits, &misses);
    CHECK(hits == 1 && misses == 2);

    /* Distinct text contexts can allocate identical local IDs; reject before lookup. */
    nx_font *foreign_font = font_create(other_text, &a);
    nx_layout *invalid_layout = NULL, *layout;
    CHECK(nx_atlas_glyph(atlas, foreign_font, 65, 16, &second) == NEOX_ERROR_OWNERSHIP_VIOLATION);
    CHECK(nx_text_layout(text, foreign_font, 16, "A", 0, NX_ALIGN_LEFT, &invalid_layout)
          == NEOX_ERROR_OWNERSHIP_VIOLATION);
    CHECK(!invalid_layout);
    OK(nx_text_layout(other_text, foreign_font, 16, "", 0, NX_ALIGN_LEFT, &layout));

    OK(neox_context_create(&cd, &foreign_core));
    OK(neox_device_create(foreign_core, NULL, &foreign_device));
    CHECK(nx_atlas_create(text, foreign_device, 32, 32, &invalid_atlas)
          == NEOX_ERROR_OWNERSHIP_VIOLATION);
    CHECK(!invalid_atlas);
    uint8_t pixels[4] = {0};
    neox_target_desc target = {0};
    target.soft.pixels = pixels; target.soft.width = target.soft.height = 1; target.soft.stride = 4;
    neox_paint paint = {{1, 1, 1, 1}};
    neox_frame *frame;
    OK(neox_frame_begin(device, &target, &frame));
    CHECK(nx_text_draw(frame, atlas, layout, &paint) == NEOX_ERROR_OWNERSHIP_VIOLATION);
    neox_frame_cancel(frame);
    nx_layout_destroy(layout);
    OK(nx_text_layout(text, font, 16, "A", 0, NX_ALIGN_LEFT, &layout));
    OK(neox_frame_begin(foreign_device, &target, &frame));
    CHECK(nx_text_draw(frame, atlas, layout, &paint) == NEOX_ERROR_OWNERSHIP_VIOLATION);
    neox_frame_cancel(frame);
    nx_atlas_stats(atlas, &hits, &misses);
    CHECK(hits == 1 && misses == 2); /* rejected draws do not warm or mutate cache */
    nx_layout_destroy(layout);

    nx_text_context_destroy(other_text);
    nx_text_context_destroy(text);
    neox_context_destroy(foreign_core);
    neox_context_destroy(core);
    CHECK(p.live == 0);
    while (p.free) { block *b2 = p.free; p.free = b2->next; free(b2->ptr); free(b2); }
    puts("ALL PASS (font address reuse, cache identity, text/core/device ownership)");
    return 0;
}
