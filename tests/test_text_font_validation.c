/* P0-3：不可信字体输入必须在进入 stb_truetype 之前被拒绝。
 * 旧实现在 12 字节的伪造 sfnt 头上让 stbtt_InitFont 越界读（ASan
 * heap-buffer-overflow）。这里逐类构造畸形/截断输入，要求统一返回
 * INVALID_ARGUMENT 且 *out 保持 NULL；最后用一个结构合法的最小字体确认
 * 校验不会误伤正常输入。 */
#include "neox.h"
#include "neox_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, unsigned long v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void expect_bad(nx_text_context *t, const uint8_t *d, size_t n, int ttc,
                       const char *what)
{
    nx_font *f = NULL;
    neox_result r = nx_font_load_memory(t, d, n, ttc, &f);
    if (r != NEOX_ERROR_INVALID_ARGUMENT || f != NULL) {
        fprintf(stderr, "%s: r=%d f=%p (expected INVALID_ARGUMENT/NULL)\n",
                what, (int)r, (void *)f);
        exit(1);
    }
}

int main(void)
{
    neox_context *ctx = NULL;
    nx_text_context *t = NULL;
    uint8_t buf[4096];
    neox_context_desc cd;
    memset(&cd, 0, sizeof cd);
    CHECK(neox_context_create(&cd, &ctx) == NEOX_OK);
    CHECK(nx_text_context_create(ctx, NULL, &t) == NEOX_OK);

    /* 1. review 复现用例：12 字节、表数 1，但目录/表都不存在。 */
    memset(buf, 0, sizeof buf);
    put32(buf, 0x00010000u);
    put16(buf + 4, 1); /* numTables */
    expect_bad(t, buf, 12, 0, "truncated sfnt header");

    /* 2. 目录完整但表偏移越过缓冲区。 */
    memset(buf, 0, sizeof buf);
    put32(buf, 0x00010000u);
    put16(buf + 4, 1);
    memcpy(buf + 12, "cmap", 4);
    put32(buf + 20, 0x10000u); /* offset 溢出/越界 */
    put32(buf + 24, 16);
    expect_bad(t, buf, 64, 0, "table offset beyond buffer");

    /* 3. 表长度声明越界（offset 合法、length 溢出）。 */
    memset(buf, 0, sizeof buf);
    put32(buf, 0x00010000u);
    put16(buf + 4, 1);
    memcpy(buf + 12, "head", 4);
    put32(buf + 20, 28);
    put32(buf + 24, 0xFFFFFFF0u);
    expect_bad(t, buf, 64, 0, "table length beyond buffer");

    /* 4. 表数远超目录容量。 */
    memset(buf, 0, sizeof buf);
    put32(buf, 0x00010000u);
    put16(buf + 4, 0xFFFF);
    expect_bad(t, buf, 64, 0, "table count exceeds directory");

    /* 5. 未知 sfnt 版本。 */
    memset(buf, 0, sizeof buf);
    put32(buf, 0x12345678u);
    expect_bad(t, buf, 64, 0, "unknown sfnt tag");

    /* 6. 非集合字体却要求 index 1。 */
    memset(buf, 0x11, sizeof buf);
    put32(buf, 0x00010000u);
    put16(buf + 4, 0);
    expect_bad(t, buf, 64, 1, "non-TTC with nonzero index");

    /* 7. TTC：numFonts 巨大 / 子字体偏移越界 / index 越界。 */
    memset(buf, 0, sizeof buf);
    put32(buf, 0x74746366u); /* 'ttcf' */
    put32(buf + 8, 0xFFFFFFu);
    expect_bad(t, buf, 64, 0, "TTC numFonts too large");
    memset(buf, 0, sizeof buf);
    put32(buf, 0x74746366u);
    put32(buf + 8, 1);
    put32(buf + 12, 0xFFFFu); /* 字体偏移越界 */
    expect_bad(t, buf, 64, 0, "TTC font offset beyond buffer");
    memset(buf, 0, sizeof buf);
    put32(buf, 0x74746366u);
    put32(buf + 8, 1);
    put32(buf + 12, 16);
    expect_bad(t, buf, 64, 1, "TTC index out of range");

    /* 8. 空指针 / 过短 / 负 index。 */
    expect_bad(t, NULL, 12, 0, "NULL data");
    expect_bad(t, buf, 0, 0, "zero length");
    expect_bad(t, buf, 8, 0, "shorter than sfnt header");
    expect_bad(t, buf, 64, -1, "negative TTC index");

    /* 9. 结构合法但内容不可解析的最小 sfnt：校验通过后由 stb 拒绝。 */
    {
        nx_font *f = NULL;
        neox_result r;
        memset(buf, 0, sizeof buf);
        put32(buf, 0x00010000u);
        put16(buf + 4, 1);
        memcpy(buf + 12, "head", 4);
        put32(buf + 20, 28);
        put32(buf + 24, 54);
        r = nx_font_load_memory(t, buf, 256, 0, &f);
        CHECK(r == NEOX_ERROR_INVALID_ARGUMENT && f == NULL);
    }

    nx_text_context_destroy(t);
    neox_context_destroy(ctx);
    puts("ALL PASS (font header/TTC validation rejects malformed input before stb)");
    return 0;
}
