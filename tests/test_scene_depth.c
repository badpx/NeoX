/* P1-11：场景树深度。深链的销毁使用常数额外空间的迭代遍历，超过上限的
 * 挂接与渲染返回 UNSUPPORTED 而不是把默认线程栈压爆。
 * 测试自身也只用迭代构造，不依赖递归。 */
#include "neox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define OK(x) CHECK((x) == NEOX_OK)

typedef struct { size_t live, allocs; int fail; } scene_tracker;
typedef struct { size_t size; } scene_header;
static void *scene_alloc(void *user, size_t size, size_t align)
{
    scene_tracker *tr = user;
    scene_header *h;
    (void)align;
    if (tr->fail) return NULL;
    h = malloc(sizeof(*h) + size);
    if (!h) return NULL;
    h->size = size;
    tr->live += size;
    tr->allocs++;
    return h + 1;
}
static void scene_free(void *user, void *ptr, size_t size, size_t align)
{
    scene_tracker *tr = user;
    scene_header *h;
    (void)align;
    if (!ptr) return;
    h = (scene_header *)ptr - 1;
    CHECK(h->size == size);
    tr->live -= size;
    free(h);
}

static neox_node group_node(neox_device *dev, neox_scene sc)
{
    neox_node_desc nd;
    neox_node n = { 0 };
    memset(&nd, 0, sizeof nd);
    nd.kind = NEOX_NODE_GROUP;
    nd.transform = (neox_affine){ 1, 0, 0, 1, 0, 0 };
    nd.opacity = 1.0f;
    nd.visible = 1;
    OK(neox_node_create(dev, sc, &nd, &n));
    return n;
}

/* 构造 depth 层嵌套组（最外层是场景根的孩子），返回最深节点。 */
static neox_node build_chain(neox_device *dev, neox_scene sc, int depth)
{
    neox_node prev = { 0 }, cur;
    int i;
    for (i = 0; i < depth; i++) {
        cur = group_node(dev, sc);
        if (prev.id) OK(neox_node_append_child(dev, cur, prev));
        prev = cur;
    }
    return prev;
}

int main(void)
{
    neox_context *ctx = NULL;
    neox_device *dev = NULL;
    neox_scene sc;
    uint8_t px[4 * 4 * 4];
    neox_target_desc td;

    OK(neox_context_create(NULL, &ctx));
    OK(neox_device_create(ctx, NULL, &dev));

    /* 深链构造与销毁：旧实现递归释放，在浅栈上直接段错误。 */
    {
        int depth = 3000;
        sc = neox_scene_create(dev);
        CHECK(sc.id != 0);
        (void)build_chain(dev, sc, depth);
        neox_scene_destroy(dev, sc);
        sc = neox_scene_create(dev);
        CHECK(sc.id != 0);
        (void)build_chain(dev, sc, depth);
        neox_device_destroy(dev);
        dev = NULL;
        /* 设备销毁会级联释放场景（另一条深链释放路径）。 */
    }

    neox_context_destroy(ctx);
    ctx = NULL;
    OK(neox_context_create(NULL, &ctx));
    OK(neox_device_create(ctx, NULL, &dev));

    /* 超过深度上限的挂接返回 UNSUPPORTED，并且不改动树。 */
    {
        int limit = 4096;
        int i;
        neox_node prev = { 0 }, cur;
        neox_result r = NEOX_OK;
        sc = neox_scene_create(dev);
        CHECK(sc.id != 0);
        for (i = 0; i < limit; i++) {
            cur = group_node(dev, sc);
            if (prev.id) {
                r = neox_node_append_child(dev, cur, prev);
                if (r != NEOX_OK) break;
            }
            prev = cur;
        }
        CHECK(r == NEOX_OK);
        /* 场景根 → 链首是第 1 层；此时链长 limit，最深节点深度为 limit。 */
        cur = group_node(dev, sc);
        r = neox_node_append_child(dev, cur, prev);
        CHECK(r == NEOX_ERROR_UNSUPPORTED);
        /* 被拒绝的节点仍挂在场景根上，随场景一起正常释放。 */
        neox_scene_destroy(dev, sc);
    }

    /* 渲染是递归的：超限的树必须返回 UNSUPPORTED（这里借助“未经
     * append_child 手工加深”不现实，因此验证上限内的深链可以渲染）。 */
    {
        neox_frame *fr = NULL;
        neox_submission sub = { 0 };
        sc = neox_scene_create(dev);
        CHECK(sc.id != 0);
        (void)build_chain(dev, sc, 64);
        memset(&td, 0, sizeof td);
        td.kind = NEOX_BACKEND_SOFT;
        td.soft.pixels = px;
        td.soft.width = 4;
        td.soft.height = 4;
        td.soft.stride = 16;
        OK(neox_frame_begin(dev, &td, &fr));
        OK(neox_scene_render(fr, sc, (neox_node){ 0 }));
        OK(neox_frame_end(fr, &sub));
        OK(neox_submission_wait(dev, sub));
        neox_submission_release(dev, sub);
        neox_scene_destroy(dev, sc);
    }

    {
        int depth = 3000;
        sc = neox_scene_create(dev);
        CHECK(sc.id != 0);
        (void)build_chain(dev, sc, depth);
        neox_scene_destroy(dev, sc);
        sc = neox_scene_create(dev);
        CHECK(sc.id != 0);
        (void)build_chain(dev, sc, depth);
        neox_device_destroy(dev);
        dev = NULL;
        /* 设备销毁会级联释放场景（另一条深链释放路径）。 */
    }

    neox_context_destroy(ctx);
    ctx = NULL;
    OK(neox_context_create(NULL, &ctx));
    OK(neox_device_create(ctx, NULL, &dev));

    /* 超过深度上限的挂接返回 UNSUPPORTED，并且不改动树。 */
    {
        int limit = 4096;
        int i;
        neox_node prev = { 0 }, cur;
        neox_result r = NEOX_OK;
        sc = neox_scene_create(dev);
        CHECK(sc.id != 0);
        for (i = 0; i < limit; i++) {
            cur = group_node(dev, sc);
            if (prev.id) {
                r = neox_node_append_child(dev, cur, prev);
                if (r != NEOX_OK) break;
            }
            prev = cur;
        }
        CHECK(r == NEOX_OK);
        /* 场景根 → 链首是第 1 层；此时链长 limit，最深节点深度为 limit。 */
        cur = group_node(dev, sc);
        r = neox_node_append_child(dev, cur, prev);
        CHECK(r == NEOX_ERROR_UNSUPPORTED);
        /* 被拒绝的节点仍挂在场景根上，随场景一起正常释放。 */
        neox_scene_destroy(dev, sc);
    }

    /* 渲染是递归的：超限的树必须返回 UNSUPPORTED（这里借助“未经
     * append_child 手工加深”不现实，因此验证上限内的深链可以渲染）。 */
    {
        neox_frame *fr = NULL;
        neox_submission sub = { 0 };
        sc = neox_scene_create(dev);
        CHECK(sc.id != 0);
        (void)build_chain(dev, sc, 64);
        memset(&td, 0, sizeof td);
        td.kind = NEOX_BACKEND_SOFT;
        td.soft.pixels = px;
        td.soft.width = 4;
        td.soft.height = 4;
        td.soft.stride = 16;
        OK(neox_frame_begin(dev, &td, &fr));
        OK(neox_scene_render(fr, sc, (neox_node){ 0 }));
        OK(neox_frame_end(fr, &sub));
        OK(neox_submission_wait(dev, sub));
        neox_submission_release(dev, sub);
        neox_scene_destroy(dev, sc);
    }

    /* 评审 4489969077：detach 必须保留子树真实高度。深子树从别处摘下后
     * 再挂到接近上限的链上，深度检查仍要看见整棵子树；旧实现把 height 清零，
     * 能挂出超过 4096 层的树，直到渲染才失败。场景根不在公开 API 里，所以
     * 用 holder（根的孩子，深度 1）当锚点。 */
    {
        neox_scene s3 = neox_scene_create(dev);
        neox_node holder, chain, tail, leaf, n2;
        uint32_t k;
        CHECK(s3.id != 0);
        holder = group_node(dev, s3);
        chain = group_node(dev, s3);
        leaf = build_chain(dev, s3, 3000);        /* 最深节点 = 3000 层 */
        OK(neox_node_append_child(dev, holder, chain));
        tail = chain;
        for (k = 0; k < 500; k++) {
            n2 = group_node(dev, s3);
            OK(neox_node_append_child(dev, tail, n2));
            tail = n2;
        }
        /* 1(holder) + 1(chain) + 500 + 1 + 3000 = 3503：允许。 */
        OK(neox_node_append_child(dev, holder, leaf));
        OK(neox_node_remove_from_parent(dev, leaf));
        OK(neox_node_append_child(dev, holder, leaf));
        OK(neox_node_remove_from_parent(dev, leaf));
        {
            neox_node big = build_chain(dev, s3, 4000); /* 4000 层子树 */
            OK(neox_node_append_child(dev, holder, big));
            OK(neox_node_remove_from_parent(dev, big));
            /* 1 + 1 + 600 + 1 + 4000 = 4603 > 4096：旧实现（detach 时把
             * height 清零）会成功，修复后必须拒绝。 */
            CHECK(neox_node_append_child(dev, tail, big) == NEOX_ERROR_UNSUPPORTED);
            n2 = group_node(dev, s3);
            OK(neox_node_append_child(dev, tail, n2));   /* 一层叶节点仍允许 */
        }
        neox_scene_destroy(dev, s3);
    }

    neox_device_destroy(dev);
    neox_context_destroy(ctx);

    {
        scene_tracker tr = {0};
        neox_allocator allocator = { &tr, scene_alloc, scene_free };
        neox_context_desc desc = { &allocator, 0, 0 };
        size_t allocs;
        OK(neox_context_create(&desc, &ctx));
        OK(neox_device_create(ctx, NULL, &dev));
        sc = neox_scene_create(dev);
        CHECK(sc.id != 0);
        {
            neox_node top = build_chain(dev, sc, 128);
            allocs = tr.allocs;
            tr.fail = 1;
            neox_node_release(dev, top);
            CHECK(tr.allocs == allocs);
            tr.fail = 0;
        }
        (void)build_chain(dev, sc, 128);
        allocs = tr.allocs;
        tr.fail = 1;
        neox_scene_destroy(dev, sc);
        CHECK(tr.allocs == allocs);
        tr.fail = 0;
        sc = neox_scene_create(dev);
        CHECK(sc.id != 0);
        (void)build_chain(dev, sc, 128);
        allocs = tr.allocs;
        tr.fail = 1;
        neox_device_destroy(dev);
        CHECK(tr.allocs == allocs);
        neox_context_destroy(ctx);
        CHECK(tr.live == 0);
    }
    puts("ALL PASS (scene depth limit and allocation-free iterative teardown)");
    return 0;
}
