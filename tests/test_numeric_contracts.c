#include "internal.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define OK(x) CHECK((x) == NEOX_OK)
static neox_frame *begin(neox_device *dev, uint8_t pixels[16])
{
    neox_target_desc td = {0}; static const neox_color clear = {0};
    td.soft.pixels = pixels; td.soft.width = td.soft.height = 2; td.soft.stride = 8; td.clear = &clear;
    neox_frame *frame; OK(neox_frame_begin(dev, &td, &frame)); return frame;
}
static neox_state state(float a, float d)
{
    neox_state s = {0}; s.opacity = 1; s.transform = (neox_affine){a,0,0,d,0,0}; return s;
}
static void unchanged(neox_frame *fr, uint32_t commands, uint32_t depth)
{
    CHECK(fr->cmd_count == commands && fr->ctm_count == depth);
    CHECK(nx_affine_is_finite(fr->ctm_stack[depth-1]));
}
static void scale(neox_affine matrix, double expected)
{
    double actual = nx_glyph_scale_factor(matrix);
    CHECK(isfinite(actual));
    CHECK(fabs(actual-expected) <= fmax(1e-40, fabs(expected)*2e-7));
}
static void storage_boundaries(void)
{
    size_t out = 37;
    CHECK(!nx_size_mul(SIZE_MAX, 2, &out) && out == 37);
    CHECK(!nx_size_add(SIZE_MAX, 1, &out) && out == 37);
    CHECK(nx_size_mul(SIZE_MAX, 0, &out) && out == 0);
    CHECK(nx_size_add(SIZE_MAX-4, 4, &out) && out == SIZE_MAX);
    CHECK(!nx_image_span(1, 2, 4, SIZE_MAX, &out));
    CHECK(!nx_image_span(1, 4, 4, SIZE_MAX/2, &out));
    CHECK(nx_image_span(1, 1, 4, SIZE_MAX, &out) && out == 4);
    CHECK(nx_image_span(1, 2, 4, 8, &out) && out == 12);
    CHECK(!nx_image_span(1, 1, 4, 3, &out));
    neox_context_desc cd = {0}; cd.memory_budget = 256*1024;
    neox_context *ctx; neox_device *dev;
    OK(neox_context_create(&cd, &ctx)); OK(neox_device_create(ctx, NULL, &dev));
    /* Invalid platform headers fail before selecting an optional backend. */
    neox_device_desc gpu={0};gpu.backend=NEOX_BACKEND_GPU;gpu.platform_size=8;
    neox_device *invalid=NULL;
    CHECK(neox_device_create(ctx,&gpu,&invalid)==NEOX_ERROR_INVALID_ARGUMENT && !invalid);
    uint32_t header[2]={8,0x4e584d54u};gpu.platform=header;gpu.platform_size=4;
    CHECK(neox_device_create(ctx,&gpu,&invalid)==NEOX_ERROR_INVALID_ARGUMENT && !invalid);
    gpu.platform_size=8;header[0]=12;
    CHECK(neox_device_create(ctx,&gpu,&invalid)==NEOX_ERROR_INVALID_ARGUMENT && !invalid);
    header[0]=8;header[1]=0xdeadbeefu;
    CHECK(neox_device_create(ctx,&gpu,&invalid)==NEOX_ERROR_INVALID_ARGUMENT && !invalid);
    size_t used = ctx->used;
    uint8_t pixels[16]; memset(pixels, 0x59, sizeof(pixels));
    static const neox_color clear = {1,0,0,1};
    neox_target_desc td = {0}; td.soft.pixels = pixels; td.clear = &clear;
    td.soft.width = 1; td.soft.height = 2; td.soft.stride = SIZE_MAX;
    neox_frame *fr = NULL;
    CHECK(neox_frame_begin(dev, &td, &fr) == NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(!fr && !dev->active_frame && ctx->used == used && pixels[0] == 0x59);
    td.soft.height = 4; td.soft.stride = SIZE_MAX/2;
    CHECK(neox_frame_begin(dev, &td, &fr) == NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(!fr && ctx->used == used);
    /* 2^30 x 2^30 RGBA floats wrap a 64-bit byte count to zero; 32-bit
     * builds reject the row/span even earlier. No caller pixels are touched. */
    td.soft.width = td.soft.height = 1u<<30;
    /* Express row bytes without an out-of-range shift on 32-bit hosts. */
    td.soft.stride = SIZE_MAX > UINT32_MAX ? (size_t)UINT32_MAX+1 : SIZE_MAX;
    CHECK(neox_frame_begin(dev, &td, &fr) == NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(!fr && ctx->used == used);
    td.soft.width = 1; td.soft.height = (uint32_t)INT32_MAX+1; td.soft.stride = 4;
    CHECK(neox_frame_begin(dev, &td, &fr) != NEOX_OK);
    CHECK(!fr && ctx->used == used);
    td.soft.height = 1; td.soft.stride = SIZE_MAX;
    OK(neox_frame_begin(dev, &td, &fr));
    neox_submission sub; OK(neox_frame_end(fr, &sub)); OK(neox_submission_wait(dev, sub));
    neox_submission_release(dev, sub);
    CHECK(pixels[0] == 255 && pixels[3] == 255 && pixels[4] == 0x59);
    td.clear = NULL; OK(neox_frame_begin(dev, &td, &fr)); neox_frame_cancel(fr);
    memset(pixels, 0x59, sizeof(pixels)); td.clear = &clear; td.soft.height = 2; td.soft.stride = 8;
    OK(neox_frame_begin(dev, &td, &fr)); OK(neox_frame_end(fr, &sub));
    OK(neox_submission_wait(dev, sub)); neox_submission_release(dev, sub);
    for (unsigned i = 0; i < sizeof(pixels); i++)
        CHECK(pixels[i] == ((i/4)%2 ? 0x59 : (i%4==0 || i%4==3 ? 255 : 0)));
    uint8_t source[4] = {255,0,0,255}; neox_texture tex = {0};
    neox_texture_desc texture = {1,2,NEOX_FORMAT_RGBA8_SRGB,source,SIZE_MAX};
    used = ctx->used;
    CHECK(neox_texture_create(dev, &texture, &tex) == NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(!tex.id && ctx->used == used);
    texture.height = 1; OK(neox_texture_create(dev, &texture, &tex));
    neox_texture_release(dev, tex); neox_device_pump(dev);
    uint8_t verb = NEOX_VERB_MOVE; float point[2] = {0}; neox_path path = {0};
    neox_path_desc pd = {&verb,point,1,UINT32_MAX,NEOX_FILL_NONZERO};
    CHECK(neox_path_create(dev, &pd, &path) == NEOX_ERROR_INVALID_ARGUMENT && !path.id);
    neox_device_destroy(dev); CHECK(ctx->used == 0); neox_context_destroy(ctx);
}
static void sampling_ranges(void)
{
    neox_affine inv={2,1,-3,4,5,6},out;
    neox_rect rect={-1,2,3,4},local;
    CHECK(nx_sample_transform(inv,rect,2048,1024,&out,&local));
    CHECK(!memcmp(&out,&inv,sizeof(inv)) && !memcmp(&local,&rect,sizeof(rect)));
    inv=(neox_affine){1e38f,-1e38f,-1e38f,1e38f,2e37f,3e37f};
    rect=(neox_rect){-1e38f,-1e38f,2e38f,-1e38f};
    CHECK(nx_sample_transform(inv,rect,16,8,&out,&local));
    /* Independent double oracle evaluates the original unscaled expression.
     * Float products and partial sums must be finite, including cancellation. */
    for(unsigned y=0;y<8;y++)for(unsigned x=0;x<16;x++) {
        float px=x+0.5f,py=y+0.5f;
        float ax=out.a*px,cx=out.c*py,by=out.b*px,dy=out.d*py;
        CHECK(isfinite(ax) && isfinite(cx) && isfinite(ax+cx));
        CHECK(isfinite(by) && isfinite(dy) && isfinite(by+dy));
        float u=(ax+cx+out.tx-local.x)/local.w;
        float v=(by+dy+out.ty-local.y)/local.h;
        double eu=((double)inv.a*px+(double)inv.c*py+inv.tx-rect.x)/rect.w;
        double ev=((double)inv.b*px+(double)inv.d*py+inv.ty-rect.y)/rect.h;
        CHECK(isfinite(u) && isfinite(v));
        CHECK(fabs(u-eu)<2e-6 && fabs(v-ev)<2e-6);
    }
    neox_affine saved=out;neox_rect saved_local=local;
    inv=(neox_affine){1e38f,0,1e-38f,1,0,0};
    CHECK(!nx_sample_transform(inv,(neox_rect){0,0,1,1},4,4,&out,&local));
    CHECK(!memcmp(&out,&saved,sizeof(out)) && !memcmp(&local,&saved_local,sizeof(local)));
}
int main(void)
{
    sampling_ranges();
    neox_affine inverse = nx_affine_identity();
    CHECK(nx_affine_invert((neox_affine){1e20f,0,0,1e20f,0,0}, &inverse) == 1);
    CHECK(fabs((double)inverse.a*1e20f-1) < 1e-6);
    CHECK(nx_affine_invert((neox_affine){1e-30f,0,0,1e-30f,0,0}, &inverse) == 1);
    CHECK(fabs((double)inverse.a*1e-30f-1) < 1e-6);
    neox_affine saved = inverse;
    CHECK(nx_affine_invert((neox_affine){1e-40f,0,0,1e-40f,0,0}, &inverse) == -1);
    CHECK(!memcmp(&inverse,&saved,sizeof(saved)));
    CHECK(nx_affine_invert((neox_affine){1e30f,1e30f,1e30f,1e30f,0,0}, &inverse) == 0);
    CHECK(nx_pixel_floor(1e30f,INT32_MAX)==INT32_MAX);
    CHECK(nx_pixel_ceil(-1e30f,INT32_MAX)==0);
    storage_boundaries();
    /* Analytic singular values for diagonal, orthogonal and rank-one matrices. */
    scale((neox_affine){1e10f,0,0,1,0,0}, ((double)1e10f+1)*0.5);
    scale((neox_affine){0,3,-3,0,0,0}, 3);
    scale((neox_affine){3,0,0,-7,0,0}, 5);
    scale((neox_affine){1e30f,1e30f,1e30f,1e30f,0,0}, 1e30f);
    scale((neox_affine){1e-30f,0,0,1e-30f,0,0}, 1e-30f);
    scale((neox_affine){FLT_MAX,0,0,FLT_MAX,0,0}, FLT_MAX);
    scale((neox_affine){0}, 0);
    neox_context *ctx; neox_device *dev; uint8_t pixels[16];
    OK(neox_context_create(NULL, &ctx)); OK(neox_device_create(ctx, NULL, &dev));
    neox_state huge = state(1e30f,1e30f);
    neox_capture *capture; OK(neox_capture_begin(dev, &capture));
    neox_frame *fr = begin(dev, pixels);
    OK(neox_push_state(fr, &huge));
    uint32_t count = fr->cmd_count, depth = fr->ctm_count;
    CHECK(neox_push_state(fr, &huge) == NEOX_ERROR_INVALID_ARGUMENT);
    unchanged(fr, count, depth);
    OK(neox_pop_state(fr));
    neox_submission sub; OK(neox_frame_end(fr, &sub));
    OK(neox_submission_wait(dev, sub)); neox_submission_release(dev, sub);
    uint8_t *bytes; size_t size; OK(neox_capture_end(capture, &bytes, &size));
    /* The failed push must not be captured: this valid pair replays at identity. */
    fr = begin(dev, pixels); OK(neox_capture_replay(fr, bytes, size));
    OK(neox_frame_end(fr, &sub)); OK(neox_submission_wait(dev, sub)); neox_submission_release(dev, sub);
    /* Replaying that finite pair inside another huge transform overflows, and
     * must fail transactionally through the same common validation boundary. */
    OK(neox_capture_begin(dev, &capture)); fr = begin(dev, pixels); OK(neox_push_state(fr, &huge));
    count = fr->cmd_count; depth = fr->ctm_count;
    CHECK(neox_capture_replay(fr, bytes, size) == NEOX_ERROR_INVALID_ARGUMENT);
    unchanged(fr, count, depth);
    neox_frame_cancel(fr);
    uint8_t *empty; size_t empty_size; OK(neox_capture_end(capture, &empty, &empty_size));
    CHECK(empty_size == 16); neox_capture_free_buffer(ctx, empty, empty_size);
    neox_capture_free_buffer(ctx, bytes, size);
    /* A retained tree can contain individually finite matrices whose parent
     * composition overflows. Its failed expansion cannot publish a prefix. */
    neox_scene scene = neox_scene_create(dev); CHECK(scene.id);
    neox_node_desc nd = {0}; nd.kind = NEOX_NODE_GROUP; nd.visible = 1; nd.opacity = 1;
    nd.transform = huge.transform; neox_node parent, child;
    OK(neox_node_create(dev, scene, &nd, &parent)); OK(neox_node_create(dev, scene, &nd, &child));
    OK(neox_node_append_child(dev, parent, child));
    fr = begin(dev, pixels);
    CHECK(neox_scene_render(fr, scene, (neox_node){0}) == NEOX_ERROR_INVALID_ARGUMENT);
    unchanged(fr, 0, 1); neox_frame_cancel(fr); neox_scene_destroy(dev, scene);
    /* Translation addition overflow and unrepresentable singular-value mean. */
    fr = begin(dev, pixels); neox_state translated = state(1,1); translated.transform.tx = FLT_MAX;
    OK(neox_push_state(fr, &translated)); count = fr->cmd_count; depth = fr->ctm_count;
    CHECK(neox_push_state(fr, &translated) == NEOX_ERROR_INVALID_ARGUMENT);
    unchanged(fr, count, depth); neox_frame_cancel(fr);
    fr = begin(dev, pixels); neox_state wide = state(FLT_MAX,FLT_MAX);
    wide.transform.b = FLT_MAX; wide.transform.c = -FLT_MAX;
    CHECK(neox_push_state(fr, &wide) == NEOX_ERROR_INVALID_ARGUMENT);
    unchanged(fr, 0, 1); neox_frame_cancel(fr);
    neox_device_destroy(dev); CHECK(ctx->used == 0); neox_context_destroy(ctx);
    puts("ALL PASS (finite composition, frontend rollback, stable distance-field scale)"); return 0;
}
