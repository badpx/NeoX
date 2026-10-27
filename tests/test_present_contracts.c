/* Exercise adapter ownership without a display server or real window system. */
#include "present_internal.h"
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <float.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);} }while(0)
#define OK(x) CHECK((x)==NEOX_OK)
static int closed, opened, resize_calls;
static neox_result resize_result = NEOX_OK;
static neox_result open_window(neox_window*w){(void)w;opened++;return NEOX_OK;}
static void close_window(neox_window*w){CHECK(!w->dev);closed++;}
static neox_result show(neox_window*w){CHECK(w->soft_px);return NEOX_OK;}
static neox_result resize_window(neox_window*w,uint32_t x,uint32_t y)
{ CHECK(w->pt_w==x && w->pt_h==y); resize_calls++; return resize_result; }
static const nx_win_vt vt={.name="test",.open=open_window,.close=close_window,.resize=resize_window,.present_soft=show};
const nx_win_vt *nx_present_x11_vt(void){return &vt;}
const nx_win_vt *nx_present_wl_vt(void){return &vt;}
static void test_geometry(void)
{
    neox_present_desc desc={0}; neox_window *w;
    desc.struct_size=sizeof(desc);desc.magic=NEOX_PRESENT_DESC_MAGIC;
    desc.width=desc.height=8;
    const float bad[]={NAN,INFINITY,-INFINITY,-1.0f,FLT_MAX};
    int opens=opened;
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++) {
        desc.scale=bad[i];w=(neox_window*)(uintptr_t)1;
        CHECK(neox_window_create(&desc,&w)==NEOX_ERROR_INVALID_ARGUMENT);
        CHECK(!w && opened==opens);
    }
    desc.scale=1;desc.width=UINT32_MAX;
    CHECK(neox_window_create(&desc,&w)==NEOX_ERROR_INVALID_ARGUMENT && !w);
    desc.width=8;desc.scale=1.25f;OK(neox_window_create(&desc,&w));
    CHECK(w->px_w==10 && w->px_h==10);
    neox_window old=*w;
    CHECK(nx_win_set_geometry(w,8,8,INFINITY)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(memcmp(&old,w,sizeof(old))==0);
    CHECK(neox_window_resize(w,UINT32_MAX,8)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(memcmp(&old,w,sizeof(old))==0);
    resize_result=NEOX_ERROR_BACKEND;
    CHECK(neox_window_resize(w,20,30)==NEOX_ERROR_BACKEND);
    CHECK(memcmp(&old,w,sizeof(old))==0 && resize_calls==1);
    resize_result=NEOX_OK;OK(neox_window_resize(w,20,30));
    CHECK(w->px_w==25 && w->px_h==38 && resize_calls==2);
    neox_window_destroy(w);
    neox_window raw={0};
    if(SIZE_MAX>UINT32_MAX) {
        OK(nx_win_set_geometry(&raw,INT32_MAX,1,1));CHECK(raw.px_w==INT32_MAX);
    } else CHECK(nx_win_set_geometry(&raw,INT32_MAX,1,1)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(nx_win_set_geometry(&raw,INT32_MAX,1,2)==NEOX_ERROR_INVALID_ARGUMENT);
    size_t bytes=99;CHECK(!nx_win_rgba_size(UINT32_MAX,UINT32_MAX,&bytes));
    CHECK(bytes==99);OK(nx_win_set_geometry(&raw,1,1,0.5f));CHECK(raw.px_w==1);
}

static void test_resize_buffers(void)
{
    neox_context *ctx;neox_device *dev;neox_window *w;
    neox_present_desc desc={0};desc.struct_size=sizeof(desc);desc.magic=NEOX_PRESENT_DESC_MAGIC;
    desc.width=desc.height=8;OK(neox_context_create(NULL,&ctx));OK(neox_window_create(&desc,&w));
    OK(neox_window_create_device(w,ctx,NEOX_BACKEND_SOFT,&dev));
    uint8_t pixels[16*16*4];memset(pixels,0x59,sizeof(pixels));
    CHECK(neox_window_present(w)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(neox_window_read_pixels(w,pixels)==NEOX_ERROR_INVALID_ARGUMENT);
    neox_target_desc t={0},unchanged;OK(neox_window_acquire_target(w,dev,&t));
    neox_color red={1,0,0,1};t.clear=&red;neox_frame *frame;neox_submission sub;
    OK(neox_frame_begin(dev,&t,&frame));unchanged=t;
    CHECK(neox_window_acquire_target(w,dev,&t)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(memcmp(&t,&unchanged,sizeof(t))==0);
    OK(neox_frame_end(frame,&sub));uint8_t *old_buffer=w->soft_px;
    OK(neox_window_resize(w,16,16));
    CHECK(neox_window_acquire_target(w,dev,&t)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(w->soft_px==old_buffer && w->soft_w==8);
    CHECK(neox_window_present(w)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(neox_window_read_pixels(w,pixels)==NEOX_ERROR_INVALID_ARGUMENT);
    for(size_t i=0;i<sizeof(pixels);i++)CHECK(pixels[i]==0x59);
    OK(neox_submission_wait(dev,sub));neox_submission_release(dev,sub);
    CHECK(old_buffer[0]==255 && old_buffer[3]==255);
    OK(neox_window_acquire_target(w,dev,&t));CHECK(t.soft.width==16 && w->soft_w==16);
    OK(neox_window_present(w));OK(neox_window_read_pixels(w,pixels));
    /* Equal byte counts are not equal image layouts (16x16 -> 32x8). */
    OK(neox_window_resize(w,32,8));
    CHECK(neox_window_read_pixels(w,pixels)==NEOX_ERROR_INVALID_ARGUMENT);
    OK(neox_window_acquire_target(w,dev,&t));CHECK(t.soft.width==32 && t.soft.height==8);
    /* Shrinking invalidates readback too; no old larger snapshot is copied. */
    OK(neox_window_resize(w,1,1));
    CHECK(neox_window_read_pixels(w,pixels)==NEOX_ERROR_INVALID_ARGUMENT);
#if NEOX_ENABLE_GL
    /* A stale GL snapshot may be larger than the caller's new output buffer.
     * Mismatched dimensions must fail before dereferencing the surface. */
    w->is_gpu=1;w->gl_surf=(neox_gl_surface*)(uintptr_t)1;w->gl_w=32;w->gl_h=8;
    CHECK(neox_window_present(w)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(neox_window_request_readback(w)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(neox_window_read_pixels(w,pixels)==NEOX_ERROR_INVALID_ARGUMENT);
    w->is_gpu=0;w->gl_surf=NULL;
#endif
    neox_window_destroy(w);neox_context_destroy(ctx);
}

int main(void) {
    for(int order=0;order<3;order++) {
        neox_context*ctx;neox_window*w;neox_device*d,*duplicate;
        neox_present_desc desc={0};desc.struct_size=sizeof(desc);desc.magic=NEOX_PRESENT_DESC_MAGIC;
        desc.width=desc.height=8;
        OK(neox_context_create(NULL,&ctx));OK(neox_window_create(&desc,&w));
        OK(neox_window_create_device(w,ctx,NEOX_BACKEND_SOFT,&d));
        CHECK(neox_window_create_device(w,ctx,NEOX_BACKEND_SOFT,&duplicate)==NEOX_ERROR_INVALID_ARGUMENT);
        CHECK(!duplicate && w->dev==d && ctx->devices==d && !d->next);
        neox_target_desc t;OK(neox_window_acquire_target(w,d,&t));
        neox_frame*f;neox_submission sub;neox_color red={1,0,0,1};t.clear=&red;
        OK(neox_frame_begin(d,&t,&f));OK(neox_frame_end(f,&sub));
        OK(neox_submission_wait(d,sub));neox_submission_release(d,sub);
        OK(neox_window_present(w));uint8_t px[256];OK(neox_window_read_pixels(w,px));CHECK(px[0]==255);
        if(order==0){neox_window_destroy(w);CHECK(!ctx->devices);neox_context_destroy(ctx);}
        if(order==1){
            neox_context_destroy(ctx);CHECK(!w->dev && !w->ctx);
            CHECK(neox_window_present(w)==NEOX_ERROR_INVALID_ARGUMENT);
            CHECK(neox_window_read_pixels(w,px)==NEOX_ERROR_INVALID_ARGUMENT);
            neox_window_destroy(w);
        }
        if(order==2){
            neox_device_destroy(d);CHECK(!w->dev && !ctx->devices);
            OK(neox_window_create_device(w,ctx,NEOX_BACKEND_SOFT,&d));
            OK(neox_window_acquire_target(w,d,&t));t.clear=&red;
            OK(neox_frame_begin(d,&t,&f));OK(neox_frame_end(f,&sub));
            /* Window teardown cancels queued work before freeing its target. */
            neox_window_destroy(w);CHECK(!ctx->devices);neox_context_destroy(ctx);
        }
    }
    CHECK(closed==3);test_geometry();test_resize_buffers();CHECK(closed==5);puts("ALL PASS (window/device/context teardown and reattachment)");return 0;
}
