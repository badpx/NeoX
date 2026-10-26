/* Independent allocator high-water oracle, including early failure paths. */
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
#define OK(x) CHECK((x)==NEOX_OK)
typedef struct { uint64_t live, peak; int fail; } tracker;
static void *allocate(void *u,size_t n,size_t a) {
    tracker *t=u;(void)a;
    if(t->fail && !--t->fail)return NULL;
    void *p=malloc(n);if(p){t->live+=n;if(t->live>t->peak)t->peak=t->live;}return p;
}
static void release(void *u,void *p,size_t n,size_t a) {
    tracker *t=u;(void)a;if(p){CHECK(t->live>=n);t->live-=n;free(p);}
}
static neox_result allocate_sequence(neox_device *d,nx_sub_slot *s) {
    (void)s;
    void *a=nx_alloc(d->ctx,101,16),*b=nx_alloc(d->ctx,203,16);
    CHECK(a && b);nx_free(d->ctx,a,101,16);
    a=nx_alloc(d->ctx,53,16);CHECK(a);
    nx_free(d->ctx,b,203,16);nx_free(d->ctx,a,53,16);
    return NEOX_ERROR_OUT_OF_MEMORY; /* completed allocations still count on failure */
}
int main(void) {
    tracker t={0};neox_allocator al={&t,allocate,release};
    neox_context_desc cd={&al,0,0};neox_context *ctx;neox_device *d;
    OK(neox_context_create(&cd,&ctx));uint64_t header=t.live;
    OK(neox_device_create(ctx,NULL,&d));
    neox_cpu_memory_stats cpu;neox_gpu_memory_stats gpu;
    CHECK(neox_context_get_memory_stats(NULL,&cpu)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(neox_device_get_memory_stats(d,NULL)==NEOX_ERROR_INVALID_ARGUMENT);
    OK(neox_device_get_memory_stats(d,&gpu));CHECK(!gpu.texture_bytes && !gpu.pool_texture_bytes && !gpu.idle_pool_texture_bytes);
    /* Establish a historical peak higher than any execute call below. */
    void *large=nx_alloc(ctx,1<<20,16);CHECK(large);nx_free(ctx,large,1<<20,16);
    OK(neox_context_get_memory_stats(ctx,&cpu));uint64_t historic=cpu.peak_bytes;
    CHECK(cpu.live_bytes==t.live-header && historic==t.peak-header);
    const nx_backend *original=d->vt;nx_backend mock=*original;mock.execute=allocate_sequence;
    uint8_t pixels[16];neox_color clear={0};neox_target_desc td={0};
    td.soft.pixels=pixels;td.soft.width=td.soft.height=2;td.soft.stride=8;td.clear=&clear;
    for(int scenario=0;scenario<4;scenario++) {
        neox_frame *fr;neox_submission sub;OK(neox_frame_begin(d,&td,&fr));OK(neox_frame_end(fr,&sub));
        uint64_t baseline=t.live;t.peak=baseline;
        d->vt=scenario==0?&mock:original;
        t.fail=scenario==1?1:scenario==2?2:0;
        neox_result result=neox_submission_wait(d,sub);t.fail=0;
        CHECK(result==(scenario==3?NEOX_OK:NEOX_ERROR_OUT_OF_MEMORY));
        neox_exec_report report;OK(neox_submission_get_report(d,sub,&report));
        CHECK(report.peak_bytes==t.peak-baseline);
        if(!scenario)CHECK(report.peak_bytes==304);
        OK(neox_context_get_memory_stats(ctx,&cpu));
        CHECK(cpu.peak_bytes==historic && cpu.live_bytes==t.live-header);
        CHECK(ctx->memory_probe==NULL);neox_submission_release(d,sub);
    }
    d->vt=original;neox_context_destroy(ctx);CHECK(!t.live);
    puts("ALL PASS (CPU phase peak, failed phases, monotonic context peak, Soft GPU zeros)");return 0;
}
