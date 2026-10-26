#ifndef NEOX_CHECKED_H
#define NEOX_CHECKED_H
#include <stddef.h>
#include <stdint.h>
/* Checked byte arithmetic. Outputs are only written on success. */
static inline int nx_size_mul(size_t a, size_t b, size_t *out)
{
    if (b && a > SIZE_MAX / b) return 0;
    *out = a * b;
    return 1;
}
static inline int nx_size_add(size_t a, size_t b, size_t *out)
{
    if (b > SIZE_MAX - a) return 0;
    *out = a + b;
    return 1;
}
/* Bytes actually accessed: the last row has no required trailing padding.
 * A one-row view never advances by stride, even if stride is SIZE_MAX. */
static inline int nx_image_span(uint32_t w, uint32_t h, size_t bpp,
                                size_t stride, size_t *out)
{
    size_t row, offset;
    if (!w || !h || !bpp || !nx_size_mul(w, bpp, &row) || stride < row ||
        !nx_size_mul(h - 1, stride, &offset)) return 0;
    return nx_size_add(offset, row, out);
}

#endif
