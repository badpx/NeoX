/* P1-11：场景树深度。深链的销毁必须走显式栈（不递归），超过上限的挂接
 * 与渲染返回 UNSUPPORTED 而不是把默认线程栈压爆。
 * 测试自身也只用迭代构造，不依赖递归。 */
#include "neox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define OK(x) CHECK((x) == NEOX_OK)

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

    neox_device_destroy(dev);
    neox_context_destroy(ctx);
    puts("ALL PASS (scene depth limit and iterative teardown)");
    return 0;
}
