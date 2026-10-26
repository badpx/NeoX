#include "internal.h"
#ifdef NEOX_ENABLE_GL
#include "neox_gl.h"
#endif
#ifdef NEOX_ENABLE_METAL
#include "neox_mtl.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define OK(x) do { if ((x)!=NEOX_OK) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } }while(0)
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } }while(0)
static void ownership(neox_context *ctx, neox_device *dev, void *surface,
                      const neox_device_desc *desc, int metal)
{
    /* A device with the same public GPU kind is not necessarily the right backend. */
#ifdef NEOX_ENABLE_GL
    if (metal) {
        neox_gl_surface *invalid = NULL;
        uint32_t visual = 99;
        CHECK(neox_gl_surface_create(dev, 8, 8, &invalid) == NEOX_ERROR_INVALID_ARGUMENT);
        CHECK(!invalid);
        CHECK(neox_gl_device_native_visual(dev, &visual) == NEOX_ERROR_INVALID_ARGUMENT);
        CHECK(visual == 0);
        neox_gl_window_desc wd = {0};
        wd.struct_size = sizeof(wd); wd.magic = NEOX_GL_WINSURF_MAGIC;
        wd.width = wd.height = 8; wd.native_window = (void *)(uintptr_t)1;
        CHECK(neox_gl_surface_create_window(dev, &wd, &invalid) == NEOX_ERROR_INVALID_ARGUMENT);
        CHECK(!invalid);
    }
#endif
#ifdef NEOX_ENABLE_METAL
    if (!metal) {
        neox_mtl_surface *invalid = NULL;
        CHECK(neox_mtl_surface_create(dev, 8, 8, &invalid) == NEOX_ERROR_INVALID_ARGUMENT);
        CHECK(!invalid);
    }
#endif
    /* Even a device of the correct backend cannot destroy someone else's surface. */
    neox_device *other;
    OK(neox_device_create(ctx, desc, &other));
#ifdef NEOX_ENABLE_GL
    if (!metal) neox_gl_surface_destroy(other, surface);
#endif
#ifdef NEOX_ENABLE_METAL
    if (metal) neox_mtl_surface_destroy(other, surface);
#endif
    neox_target_desc target = {0}; target.kind = NEOX_BACKEND_GPU; target.surface = surface;
    neox_frame *frame = NULL;
    CHECK(neox_frame_begin(other, &target, &frame) == NEOX_ERROR_OWNERSHIP_VIOLATION);
    CHECK(!frame);
    neox_device_destroy(other);
    /* The caller proceeds to draw and read the untouched owner's surface. */
}
static void odd_width_clip(neox_device *dev, int metal)
{
    void *surface=NULL;
#ifdef NEOX_ENABLE_GL
    if(!metal){neox_gl_surface *p;OK(neox_gl_surface_create(dev,7,3,&p));surface=p;}
#endif
#ifdef NEOX_ENABLE_METAL
    if(metal){neox_mtl_surface *p;OK(neox_mtl_surface_create(dev,7,3,&p));surface=p;}
#endif
    uint8_t verbs[]={0,1,1,1,4};
    float clip_points[]={0,1,7,1,7,2,0,2},fill_points[]={0,0,7,0,7,3,0,3};
    neox_path clip,fill;neox_path_desc pd={verbs,clip_points,5,4,NEOX_FILL_NONZERO};
    OK(neox_path_create(dev,&pd,&clip));pd.points=fill_points;OK(neox_path_create(dev,&pd,&fill));
    neox_target_desc td={0};neox_color clear={0};td.kind=NEOX_BACKEND_GPU;td.surface=surface;td.clear=&clear;
    neox_frame *f;neox_submission sub;neox_paint paint={{1,1,1,1}};
    neox_state state={0};state.transform=(neox_affine){1,0,0,1,0,0};state.opacity=1;state.clip=&clip;
    OK(neox_frame_begin(dev,&td,&f));OK(neox_push_state(f,&state));OK(neox_draw_path(f,fill,&paint));
    OK(neox_pop_state(f));OK(neox_frame_end(f,&sub));OK(neox_submission_wait(dev,sub));
    uint8_t pixels[7*3*4];
#ifdef NEOX_ENABLE_GL
    if(!metal)OK(neox_gl_surface_read_pixels(surface,pixels));
#endif
#ifdef NEOX_ENABLE_METAL
    if(metal)OK(neox_mtl_surface_read_pixels(surface,pixels));
#endif
    for(int y=0;y<3;y++)for(int x=0;x<7;x++)CHECK(pixels[(y*7+x)*4+3]==(y==1?255:0));
    neox_submission_release(dev,sub);neox_path_release(dev,clip);neox_path_release(dev,fill);neox_device_pump(dev);
#ifdef NEOX_ENABLE_GL
    if(!metal)neox_gl_surface_destroy(dev,surface);
#endif
#ifdef NEOX_ENABLE_METAL
    if(metal)neox_mtl_surface_destroy(dev,surface);
#endif
}
static void versions(neox_context *ctx,neox_device *dev,void *surface,int metal)
{
    uint8_t red[]={255,0,0,255},blue[]={0,0,255,255},green[]={0,255,0,255};
    neox_texture_desc td={1,1,NEOX_FORMAT_RGBA8_SRGB,red,4};neox_texture tex;
    OK(neox_texture_create(dev,&td,&tex));
    neox_capture *cap;OK(neox_capture_begin(dev,&cap));
    neox_target_desc target={0};neox_color clear={0};
    target.kind=NEOX_BACKEND_GPU;target.surface=surface;target.clear=&clear;
    neox_frame *fr;neox_submission sub;
    OK(neox_frame_begin(dev,&target,&fr));
    OK(neox_draw_texture(fr,tex,(neox_rect){0,0,1,1},(neox_rect){0,0,4,8},NEOX_FILTER_NEAREST,1));
    OK(neox_texture_update(dev,tex,(neox_rect){0,0,1,1},blue,4));
    OK(neox_draw_texture(fr,tex,(neox_rect){0,0,1,1},(neox_rect){4,0,4,8},NEOX_FILTER_NEAREST,1));
    OK(neox_frame_end(fr,&sub));
    OK(neox_texture_update(dev,tex,(neox_rect){0,0,1,1},green,4));
    neox_device_pump(dev);
    /* Updating an in-flight slot cannot mutate the GPU objects already encoded. */
    OK(neox_texture_update(dev,tex,(neox_rect){0,0,1,1},red,4));
    neox_texture_release(dev,tex);
    OK(neox_submission_wait(dev,sub));neox_submission_release(dev,sub);
    uint8_t *bytes;size_t size;OK(neox_capture_end(cap,&bytes,&size));
    for(int replay=0;replay<2;replay++) {
        if(replay) {
            OK(neox_frame_begin(dev,&target,&fr));OK(neox_capture_replay(fr,bytes,size));
            OK(neox_frame_end(fr,&sub));OK(neox_submission_wait(dev,sub));neox_submission_release(dev,sub);
        }
        uint8_t px[256];
#ifdef NEOX_ENABLE_GL
        if(!metal)OK(neox_gl_surface_read_pixels(surface,px));
#endif
#ifdef NEOX_ENABLE_METAL
        if(metal)OK(neox_mtl_surface_read_pixels(surface,px));
#endif
        for(int y=0;y<8;y++)for(int x=0;x<8;x++)
            if(memcmp(px+(y*8+x)*4,x<4?red:blue,4)) {
                fprintf(stderr,"%s texture version changed (CAP=%d)\n",metal?"Metal":"GL",replay);exit(1);
            }
    }
    neox_capture_free_buffer(ctx,bytes,size);
}
static void vertex_size_limit(neox_device *dev, void *surface)
{
    neox_target_desc target={0};neox_color clear={0};
    target.kind=NEOX_BACKEND_GPU;target.surface=surface;target.clear=&clear;
    neox_frame *frame;neox_submission sub;
    OK(neox_frame_begin(dev,&target,&frame));OK(neox_frame_end(frame,&sub));
    nx_sub_slot *slot=&((nx_sub_slot *)dev->subs.slots)[nx_handle_index(sub.id)];
    nx_cmd commands[2]={{0},{0}};commands[0].op=commands[1].op=NXC_GLYPHS;
    /* Exercise real Metal's staging preflight without allocating a huge input.
     * Counts would overflow the old uint32_t aggregate/formula. No glyph array
     * may be read, and the untouched real submission must remain executable. */
    slot->cmds=commands;slot->cmd_count=2;
    uint64_t used=dev->ctx->used;
    commands[0].glyph_count=UINT32_MAX-1;
    CHECK(dev->vt->execute(dev,slot)==NEOX_ERROR_OUT_OF_MEMORY);
    commands[1].glyph_count=UINT32_MAX;
    CHECK(dev->vt->execute(dev,slot)==NEOX_ERROR_OUT_OF_MEMORY);
    CHECK(dev->ctx->used==used);
    slot->cmds=NULL;slot->cmd_count=0;
    OK(neox_submission_wait(dev,sub));neox_submission_release(dev,sub);
}
static void lost_device(neox_device *dev, void *surface, int metal)
{
    /* Inject the observed terminal event, not a physical GPU reset. The real
     * backend must reject GPU operations and still destroy its native objects. */
    neox_target_desc target = {0}; neox_color clear = {0};
    target.kind = NEOX_BACKEND_GPU; target.surface = surface; target.clear = &clear;
    neox_frame *frame; neox_submission sub;
    OK(neox_frame_begin(dev, &target, &frame)); OK(neox_frame_end(frame, &sub));
    CHECK(nx_device_mark_lost(dev, "injected device loss") == NEOX_ERROR_DEVICE_LOST);
    CHECK(neox_device_is_lost(dev));
    CHECK(neox_submission_wait(dev, sub) == NEOX_ERROR_DEVICE_LOST);
    neox_submission_release(dev, sub);
    CHECK(neox_frame_begin(dev, &target, &frame) == NEOX_ERROR_DEVICE_LOST && !frame);
    uint8_t pixels[256]; memset(pixels, 0xa5, sizeof(pixels));
#ifdef NEOX_ENABLE_GL
    if (!metal) {
        neox_gl_surface *extra = NULL;
        CHECK(neox_gl_surface_create(dev, 8, 8, &extra) == NEOX_ERROR_DEVICE_LOST && !extra);
        CHECK(neox_gl_surface_read_pixels(surface, pixels) == NEOX_ERROR_DEVICE_LOST);
        CHECK(neox_gl_surface_resize(surface, 8, 8) == NEOX_ERROR_DEVICE_LOST);
        CHECK(neox_gl_surface_present(surface) == NEOX_ERROR_DEVICE_LOST);
    }
#endif
#ifdef NEOX_ENABLE_METAL
    if (metal) {
        neox_mtl_surface *extra = NULL;
        CHECK(neox_mtl_surface_create(dev, 8, 8, &extra) == NEOX_ERROR_DEVICE_LOST && !extra);
        CHECK(neox_mtl_surface_read_pixels(surface, pixels) == NEOX_ERROR_DEVICE_LOST);
    }
#endif
    for (size_t i = 0; i < sizeof(pixels); i++) CHECK(pixels[i] == 0xa5);
}
static int run(int metal, int destroy_pending) {
    neox_context *ctx; neox_device *dev; neox_device_desc dd={0}; void *surface=NULL;
    OK(neox_context_create(NULL,&ctx)); dd.backend=NEOX_BACKEND_GPU;
#ifdef NEOX_ENABLE_GL
    neox_gl_device_desc gd={0}; gd.struct_size=sizeof(gd); gd.magic=NEOX_GL_DESC_MAGIC;
    if (!metal) { dd.platform=&gd; dd.platform_size=sizeof(gd); }
#endif
#ifdef NEOX_ENABLE_METAL
    neox_mtl_device_desc md={0}; md.struct_size=sizeof(md); md.magic=NEOX_MTL_DESC_MAGIC;
    if (metal) { dd.platform=&md; dd.platform_size=sizeof(md); }
#endif
    if (neox_device_create(ctx,&dd,&dev)!=NEOX_OK) {
        const char *required=getenv(metal?"NEOX_REQUIRE_METAL":"NEOX_REQUIRE_GL");
        printf("SKIP %s: %s\n",metal?"Metal":"GL",neox_get_error_detail(ctx));
        neox_context_destroy(ctx); if (required && required[0]) exit(1); return 0;
    }
#ifdef NEOX_ENABLE_GL
    if (!metal) { neox_gl_surface *s; OK(neox_gl_surface_create(dev,8,8,&s)); surface=s; }
#endif
#ifdef NEOX_ENABLE_METAL
    if (metal) { neox_mtl_surface *s; OK(neox_mtl_surface_create(dev,8,8,&s)); surface=s; }
#endif
    neox_gpu_memory_stats memory;
    OK(neox_device_get_memory_stats(dev,&memory));
    CHECK(memory.texture_bytes==8u*8*4 && !memory.pool_texture_bytes);
    if (!destroy_pending) ownership(ctx, dev, surface, &dd, metal);
    for (int i=0;i<2;i++) {
        uint8_t color[8]={(uint8_t)(i?0:255),0,(uint8_t)(i?255:0),255};
        memcpy(color+4,color,4);
        neox_texture_desc td={(uint32_t)(i+1),1,NEOX_FORMAT_RGBA8_SRGB,color,(size_t)(i+1)*4};
        neox_texture tex; OK(neox_texture_create(dev,&td,&tex));
        neox_color clear={0,0,0,0}; neox_target_desc target={0};
        target.kind=NEOX_BACKEND_GPU; target.surface=surface; target.clear=&clear;
        neox_frame *fr; neox_submission sub;
        OK(neox_frame_begin(dev,&target,&fr));
        OK(neox_draw_texture(fr,tex,(neox_rect){0,0,(float)(i+1),1},(neox_rect){0,0,8,8},NEOX_FILTER_NEAREST,1));
        OK(neox_frame_end(fr,&sub)); neox_device_pump(dev);
        if (destroy_pending) {
            neox_submission_release(dev,sub);
            neox_texture_release(dev,tex);
            neox_context_destroy(ctx); return 1;
        }
        OK(neox_submission_wait(dev,sub)); uint8_t px[256];
        neox_exec_report report;OK(neox_submission_get_report(dev,sub,&report));
        CHECK(report.peak_bytes>0);
        OK(neox_device_get_memory_stats(dev,&memory));
        CHECK(memory.texture_bytes>=memory.pool_texture_bytes+8u*8*4);
        CHECK(memory.pool_texture_bytes>0);
        CHECK(memory.idle_pool_texture_bytes==memory.pool_texture_bytes);
#ifdef NEOX_ENABLE_GL
        if (!metal) OK(neox_gl_surface_read_pixels(surface,px));
#endif
#ifdef NEOX_ENABLE_METAL
        if (metal) OK(neox_mtl_surface_read_pixels(surface,px));
#endif
        if (memcmp(px,color,4)) { fprintf(stderr,"stale %s texture cache\n",metal?"Metal":"GL"); exit(1); }
        neox_submission_release(dev,sub); neox_texture_release(dev,tex); neox_device_pump(dev);
    }
    for (int alpha=0;alpha<=255;alpha+=255) {
        uint8_t data[64];
        for (int i=0;i<16;i++) { data[i*4]=data[i*4+1]=data[i*4+2]=128; data[i*4+3]=(uint8_t)alpha; }
        neox_texture_desc td={4,4,NEOX_FORMAT_RGBA8_DATA,data,16};neox_texture tex;
        OK(neox_texture_create(dev,&td,&tex));
        neox_color clear={0};neox_target_desc target={0};target.kind=NEOX_BACKEND_GPU;target.surface=surface;target.clear=&clear;
        neox_frame *fr;neox_submission sub;neox_glyph glyph={0,0,0,0,4,4};
        neox_paint paint={{1,1,1,1}};neox_glyph_style style={NEOX_ATLAS_MSDF,4,1};
        OK(neox_frame_begin(dev,&target,&fr));OK(neox_draw_glyphs_ex(fr,tex,&glyph,1,&paint,&style));
        OK(neox_frame_end(fr,&sub));OK(neox_submission_wait(dev,sub));
        uint8_t px[256];
#ifdef NEOX_ENABLE_GL
        if (!metal) OK(neox_gl_surface_read_pixels(surface,px));
#endif
#ifdef NEOX_ENABLE_METAL
        if (metal) OK(neox_mtl_surface_read_pixels(surface,px));
#endif
        if (px[(1*8+1)*4+3]!=125) { fprintf(stderr,"MSDF alpha changed numeric data\n");exit(1); }
        neox_submission_release(dev,sub);neox_texture_release(dev,tex);neox_device_pump(dev);
    }
    odd_width_clip(dev,metal);
    versions(ctx,dev,surface,metal);
    if(metal)vertex_size_limit(dev,surface);
    lost_device(dev,surface,metal);
    neox_context_destroy(ctx); return 1;
}
int main(void) {
    int ran=0;
#ifdef NEOX_ENABLE_GL
    ran+=run(0,0); ran+=run(0,1);
#endif
#ifdef NEOX_ENABLE_METAL
    ran+=run(1,0); ran+=run(1,1);
#endif
    if (!ran) return 77;
    printf("ALL PASS (%d GPU runtime scenarios)\n",ran); return 0;
}
