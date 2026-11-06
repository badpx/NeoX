/* Exercise the actual Metal executor and present pipeline. Internal access is
 * only used to inject failed/out-of-order completion; rendering uses real GPU. */
#include "../src/metal.mm"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define OK(x) CHECK((x) == NEOX_OK)

static neox_submission clear_frame(neox_device *dev, neox_mtl_surface *surf,
                                   neox_color color)
{
    neox_target_desc td = {}; td.kind=NEOX_BACKEND_GPU; td.surface=surf; td.clear=&color;
    neox_frame *frame; neox_submission sub;
    OK(neox_frame_begin(dev,&td,&frame)); OK(neox_frame_end(frame,&sub));
    return sub;
}
static void pixel(neox_mtl_surface *surf, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint8_t bytes[8*8*4]; OK(neox_mtl_surface_read_pixels(surf,bytes));
    for (unsigned i=0;i<64;i++) {
        CHECK(bytes[4*i]==r && bytes[4*i+1]==g && bytes[4*i+2]==b && bytes[4*i+3]==a);
    }
}
static void completed(neox_device *dev, neox_submission sub)
{
    OK(neox_submission_wait(dev,sub)); neox_submission_release(dev,sub);
}
static void failures_and_order(neox_device *dev, neox_mtl_surface *surf)
{
    neox_context *ctx=dev->ctx; nx_mtl_dev *g=(nx_mtl_dev *)dev->bstate;
    completed(dev,clear_frame(dev,surf,(neox_color){0.5f,0.25f,0.75f,1}));
    pixel(surf,128,64,191,255); /* Snapshot must be RGBA, not R32Float. */
    uint64_t seq=surf->submit_seq, budget=ctx->budget;
    neox_submission fail=clear_frame(dev,surf,(neox_color){1,0,0,1});
    ctx->budget=ctx->used; /* Fail the executor's first host allocation. */
    CHECK(neox_submission_wait(dev,fail)==NEOX_ERROR_OUT_OF_MEMORY);
    CHECK(surf->submit_seq==seq && g->ninflight==0);
    ctx->budget=budget; neox_submission_release(dev,fail);
    completed(dev,clear_frame(dev,surf,(neox_color){0,1,0,1}));
    pixel(surf,0,255,0,255);

    neox_submission subs[3];
    for (unsigned i=0;i<3;i++) {
        neox_color c={(float)(i==0),(float)(i==1),(float)(i==2),1};
        subs[i]=clear_frame(dev,surf,c);
        nx_sub_slot *slot=&((nx_sub_slot *)dev->subs.slots)[nx_handle_index(subs[i].id)];
        slot->started=1;
        OK(mtl_execute(dev,slot));
        CHECK(slot->status==NEOX_SUBMISSION_PENDING);
    }
    CHECK(g->ninflight==3);
    for (unsigned i=0;i<3;i++) [g->inflight[i].cb waitUntilCompleted];
    /* All buffers are finished; model delivery of later callbacks first. */
    pthread_mutex_lock(&g->lock);
    for (unsigned i=0;i<3;i++) g->inflight[i].completed=i!=0;
    pthread_mutex_unlock(&g->lock);
    CHECK(mtl_poll(dev)==0);
    /* Public read_pixels intentionally waits for inflight work. Inspect only
     * the already-published texture while the first callback is suppressed. */
    uint8_t published[8*8*4];
    [surf->tex getBytes:published bytesPerRow:32 fromRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0];
    CHECK(published[0]==0 && published[1]==255 && published[2]==0);
    pthread_mutex_lock(&g->lock); g->inflight[0].completed=1; pthread_mutex_unlock(&g->lock);
    CHECK(mtl_poll(dev)==3 && g->ninflight==0);
    neox_device_pump(dev); pixel(surf,0,0,255,255);
    for (unsigned i=0;i<3;i++) completed(dev,subs[i]);
    for (unsigned i=0;i<g->nrts;i++) CHECK(!g->rts[i].in_use);
}
static void glyph_staging(neox_device *dev, neox_mtl_surface *surf)
{
    uint8_t coverage=255; neox_texture atlas;
    neox_texture_desc tex={1,1,NEOX_FORMAT_A8,&coverage,1};
    OK(neox_texture_create(dev,&tex,&atlas));
    neox_glyph glyphs[32];
    for (unsigned i=0;i<32;i++) glyphs[i]=(neox_glyph){(float)(i%8),(float)(i/8),0,0,1,1};
    neox_color clear={}; neox_target_desc td={}; td.kind=NEOX_BACKEND_GPU; td.surface=surf; td.clear=&clear;
    for (unsigned mode=0;mode<2;mode++) {
        neox_frame *frame; neox_submission sub; neox_paint paint={{1,1,1,1}};
        OK(neox_frame_begin(dev,&td,&frame));
        neox_state state={}; state.transform=nx_affine_identity(); state.opacity=1;
        state.blend=mode?NEOX_BLEND_PLUS:NEOX_BLEND_SRC_OVER;
        OK(neox_push_state(frame,&state));
        OK(neox_draw_glyphs(frame,atlas,glyphs,32,&paint));
        OK(neox_pop_state(frame)); OK(neox_frame_end(frame,&sub)); completed(dev,sub);
        uint8_t bytes[8*8*4]; OK(neox_mtl_surface_read_pixels(surf,bytes));
        for(unsigned i=0;i<64;i++) CHECK(bytes[4*i+3]==(i<32?255:0));
    }
    neox_texture_release(dev,atlas);
}
static void group_then_upload(neox_device *dev, neox_mtl_surface *surf)
{
    uint8_t verbs[]={0,1,1,1,4};
    float full_points[]={0,0,8,0,8,8,0,8}, half_points[]={4,0,8,0,8,8,4,8};
    neox_path full,half; neox_path_desc pd={verbs,full_points,5,4,NEOX_FILL_NONZERO};
    OK(neox_path_create(dev,&pd,&full)); pd.points=half_points; OK(neox_path_create(dev,&pd,&half));
    neox_color clear={}; neox_target_desc td={}; td.kind=NEOX_BACKEND_GPU; td.surface=surf; td.clear=&clear;
    neox_frame *frame; neox_submission sub;
    OK(neox_frame_begin(dev,&td,&frame));
    neox_state state={}; state.transform=nx_affine_identity(); state.opacity=0.5f;
    neox_paint red={{1,0,0,1}}, blue={{0,0,1,1}};
    OK(neox_push_state(frame,&state)); OK(neox_draw_path(frame,full,&red)); OK(neox_pop_state(frame));
    /* This path uploads premultiplied RGBA matching the popped layer's format.
     * Reusing that layer before command-buffer completion corrupts the draw. */
    OK(neox_draw_path(frame,half,&blue)); OK(neox_frame_end(frame,&sub)); completed(dev,sub);
    uint8_t bytes[8*8*4]; OK(neox_mtl_surface_read_pixels(surf,bytes));
    for (unsigned y=0;y<8;y++) for(unsigned x=0;x<8;x++) {
        const uint8_t *p=bytes+(y*8+x)*4;
        CHECK(p[0]==(x<4?255:0) && p[1]==0 && p[2]==(x<4?0:255) && p[3]==(x<4?128:255));
    }
    neox_path_release(dev,full); neox_path_release(dev,half);
}
static void present_color(neox_device *dev, neox_mtl_surface *surf)
{
    nx_mtl_dev *g=(nx_mtl_dev *)dev->bstate;
    OK(mtl_present_pipeline(g)); /* Compile the exact runtime present shader. */
    MTLTextureDescriptor *desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
        width:8 height:8 mipmapped:NO];
    desc.usage=MTLTextureUsageRenderTarget; desc.storageMode=MTLStorageModeShared;
    id<MTLTexture> dst=[g->device newTextureWithDescriptor:desc]; CHECK(dst);
    for(unsigned translucent=0;translucent<2;translucent++) {
        completed(dev,clear_frame(dev,surf,(neox_color){0.5f,0.5f,0.5f,translucent?0.5f:1.0f}));
        id<MTLCommandBuffer> cb=[g->queue commandBuffer];
        id<MTLRenderCommandEncoder> enc=[cb renderCommandEncoderWithDescriptor:mtl_desc(dst,MTLLoadActionDontCare)];
        [enc setRenderPipelineState:g->ps_present]; [enc setFragmentTexture:surf->tex atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [enc endEncoding];
        [cb commit]; [cb waitUntilCompleted]; CHECK(cb.status==MTLCommandBufferStatusCompleted);
        uint8_t bytes[8*8*4]; [dst getBytes:bytes bytesPerRow:32 fromRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0];
        for(unsigned i=0;i<64;i++) CHECK(bytes[4*i]==(translucent?64:128) &&
            bytes[4*i+1]==bytes[4*i] && bytes[4*i+2]==bytes[4*i] && bytes[4*i+3]==255);
    }
    [dst release];
}
int main(void)
{
    @autoreleasepool {
        neox_context *ctx; neox_device *dev; neox_mtl_surface *surf;
        OK(neox_context_create(NULL,&ctx));
        neox_mtl_device_desc md={}; md.struct_size=sizeof(md); md.magic=NEOX_MTL_DESC_MAGIC;
        neox_device_desc dd={}; dd.backend=NEOX_BACKEND_GPU; dd.platform=&md; dd.platform_size=sizeof(md);
        if(neox_device_create(ctx,&dd,&dev)!=NEOX_OK) {
            const char *required=getenv("NEOX_REQUIRE_METAL"); CHECK(!required || !*required);
            fprintf(stderr,"SKIP Metal: %s\n",neox_get_error_detail(ctx)); neox_context_destroy(ctx); return 77;
        }
        OK(neox_mtl_surface_create(dev,8,8,&surf));
        failures_and_order(dev,surf); glyph_staging(dev,surf); group_then_upload(dev,surf); present_color(dev,surf);
        neox_mtl_surface_destroy(dev,surf); neox_device_destroy(dev); CHECK(ctx->used==0);
        neox_context_destroy(ctx);
    }
    puts("ALL PASS (Metal cleanup, failed retry, ordered publication, glyph staging and present colors)");
    return 0;
}
