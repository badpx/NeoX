#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "internal.h"
#include "neox_gl.h"
#include <stdio.h>
#include <time.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define OK(x) CHECK((x)==NEOX_OK)
enum { W=256,H=64 };
static const neox_color CLEAR={.15f,.22f,.4f,.7f};
static neox_frame *begin(neox_device *d,neox_gl_surface *surface,const neox_color *clear) {
    neox_target_desc t={0};neox_frame*f;t.kind=NEOX_BACKEND_GPU;t.surface=surface;t.clear=clear;
    OK(neox_frame_begin(d,&t,&f));return f;
}
static neox_submission end(neox_device *d,neox_frame*f) {
    (void)d;neox_submission sub;OK(neox_frame_end(f,&sub));return sub;
}
static void finish(neox_device*d,neox_submission sub) {
    OK(neox_submission_wait(d,sub));neox_submission_release(d,sub);neox_device_pump(d);
}
static double now(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9; }
static void draw(neox_frame*f,neox_texture tex,float opacity) {
    OK(neox_draw_texture(f,tex,(neox_rect){0,0,W,H},(neox_rect){0,0,W,H},NEOX_FILTER_NEAREST,opacity));
}
static void compare(const uint8_t*a,const uint8_t*b) {
    int max=0;for(int i=0;i<W*H*4;i++){int d=abs(a[i]-b[i]);if(d>max)max=d;}
    printf("output max code-value delta=%d\n",max);CHECK(max<=1);
}
static int run(int native,uint8_t output[2][W*H*4],neox_gl_stats *stats,double *ms) {
    neox_context*c;neox_device*d;neox_gl_surface *surface;neox_texture tex;
    OK(neox_context_create(NULL,&c));
    neox_gl_device_desc gd={0};gd.struct_size=sizeof(gd);gd.magic=NEOX_GL_DESC_MAGIC;
    gd.output_mode=native?NEOX_GL_OUTPUT_GPU:NEOX_GL_OUTPUT_REFERENCE;
    neox_device_desc dd={0};dd.backend=NEOX_BACKEND_GPU;dd.platform=&gd;dd.platform_size=sizeof(gd);
    if(neox_device_create(c,&dd,&d)!=NEOX_OK) {
        fprintf(stderr,"GL unavailable: %s\n",neox_get_error_detail(c));neox_context_destroy(c);
        if(getenv("NEOX_REQUIRE_GL"))exit(1);
        return 0;
    }
    OK(neox_gl_surface_create(d,W,H,&surface));
    uint8_t pixels[W*H*4];
    for(int i=0;i<W*H;i++) {pixels[i*4]=i%256;pixels[i*4+1]=(i/256)*4;pixels[i*4+2]=(i*17)%256;pixels[i*4+3]=i%256;}
    neox_texture_desc td={W,H,NEOX_FORMAT_RGBA8_SRGB,pixels,W*4};OK(neox_texture_create(d,&td,&tex));
    neox_frame*f=begin(d,surface,&CLEAR);draw(f,tex,1);finish(d,end(d,f));
    OK(neox_gl_surface_read_pixels(surface,output[0]));
    /* Cancelled clear snapshots must not change the real target. */
    neox_color red={1,0,0,1};f=begin(d,surface,&red);neox_frame_cancel(f);
    OK(neox_gl_surface_read_pixels(surface,output[1]));CHECK(!memcmp(output[0],output[1],sizeof(pixels)));
    /* A queued frame snapshots the original target at begin, even if an older
     * pending submission changes that target before this frame executes. */
    f=begin(d,surface,&red);neox_submission red_sub=end(d,f);
    f=begin(d,surface,NULL);draw(f,tex,.35f);neox_submission snapshot_sub=end(d,f);
    finish(d,red_sub);finish(d,snapshot_sub);
    OK(neox_gl_surface_read_pixels(surface,output[1]));
    neox_gl_stats before,after;OK(neox_gl_device_get_stats(d,&before));
    double start=now();
    for(int i=0;i<40;i++) {
        f=begin(d,surface,i&1 ? NULL : &CLEAR);draw(f,tex,.75f);finish(d,end(d,f));
    }
    *ms=(now()-start)*1000/40;
    OK(neox_gl_device_get_stats(d,&after));*stats=after;
    stats->pixel_upload_bytes-=before.pixel_upload_bytes;
    stats->pixel_readback_bytes-=before.pixel_readback_bytes;
    stats->readback_calls-=before.readback_calls;
    stats->finish_calls-=before.finish_calls;
    if(native){CHECK(stats->readback_calls==0);CHECK(stats->pixel_readback_bytes==0);CHECK(stats->pixel_upload_bytes==0);}
    else {CHECK(stats->readback_calls==60);CHECK(stats->pixel_readback_bytes==(uint64_t)W*H*(40*16+20*4));CHECK(stats->pixel_upload_bytes==(uint64_t)W*H*40*20);}
    CHECK(stats->finish_calls==40);CHECK(stats->tracked_gpu_bytes>0);
    if(native) {
        /* Failed recording leaves its GPU snapshot cancellable. */
        f=begin(d,surface,&CLEAR);uint64_t budget=c->budget;c->budget=c->used;
        /* Force a new command allocation to fail, without changing the target. */
        CHECK(neox_draw_texture(f,tex,(neox_rect){0,0,W,H},(neox_rect){0,0,W,H},NEOX_FILTER_NEAREST,1)==NEOX_ERROR_OUT_OF_MEMORY);
        c->budget=budget;neox_frame_cancel(f);
        /* Queued payload and active snapshot are reclaimed during destruction. */
        f=begin(d,surface,&CLEAR);(void)end(d,f);f=begin(d,surface,NULL);(void)f;
    }
    neox_device_destroy(d);CHECK(c->used==0);neox_context_destroy(c);return 1;
}
int main(void) {
    static uint8_t reference[2][W*H*4],native[2][W*H*4];neox_gl_stats a,b;double ta,tb;
    if(!run(0,reference,&a,&ta))return 77;
    CHECK(run(1,native,&b,&tb));
    compare(reference[0],native[0]);compare(reference[1],native[1]);
    printf("40 warm frames %dx%d: reference upload=%llu readback=%llu calls=%llu finish=%llu CPUpeak=%llu GPUstorage=%llu ms/frame=%.3f\n",W,H,(unsigned long long)a.pixel_upload_bytes,(unsigned long long)a.pixel_readback_bytes,(unsigned long long)a.readback_calls,(unsigned long long)a.finish_calls,(unsigned long long)a.context_peak_bytes,(unsigned long long)a.tracked_gpu_bytes,ta);
    printf("40 warm frames %dx%d: GPU output upload=%llu readback=%llu calls=%llu finish=%llu CPUpeak=%llu GPUstorage=%llu ms/frame=%.3f\n",W,H,(unsigned long long)b.pixel_upload_bytes,(unsigned long long)b.pixel_readback_bytes,(unsigned long long)b.readback_calls,(unsigned long long)b.finish_calls,(unsigned long long)b.context_peak_bytes,(unsigned long long)b.tracked_gpu_bytes,tb);
    puts("ALL PASS (GPU snapshots, output conversion, lifecycle, transfer counters)");return 0;
}
