/* No malloc in this allocator: fixed 160 KiB storage with 16-byte blocks.
 * Offline 3x5 NEOX glyphs avoid a runtime font parser on constrained targets. */
#include "neox.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define OK(x) CHECK((x)==NEOX_OK)
#define CAP (160u*1024u)
#define UNIT 16u
static _Alignas(16) unsigned char arena[CAP];
static unsigned char occupied[CAP/UNIT];
static size_t live, peak;
static int fail_at;
static void *take(void *u,size_t n,size_t align) {
    (void)u;CHECK(align<=UNIT);
    if(fail_at && !--fail_at)return NULL;
    size_t blocks=(n+UNIT-1)/UNIT,run=0;
    for(size_t i=0;i<CAP/UNIT;i++) {
        run=occupied[i]?0:run+1;
        if(run==blocks){size_t start=i+1-blocks;memset(occupied+start,1,blocks);live+=n;if(live>peak)peak=live;return arena+start*UNIT;}
    }
    return NULL;
}
static void give(void *u,void *p,size_t n,size_t align) {
    (void)u;(void)align;if(!p)return;
    size_t offset=(unsigned char *)p-arena;CHECK(offset<CAP && offset%UNIT==0 && live>=n);
    size_t blocks=(n+UNIT-1)/UNIT;for(size_t i=0;i<blocks;i++)CHECK(occupied[offset/UNIT+i]);
    memset(occupied+offset/UNIT,0,blocks);live-=n;
}
static neox_path rectangle(neox_device *d,float x,float y,float w,float h) {
    uint8_t verbs[]={0,1,1,1,4};float points[]={x,y,x+w,y,x+w,y+h,x,y+h};
    neox_path_desc pd={verbs,points,5,4,NEOX_FILL_NONZERO};neox_path p;OK(neox_path_create(d,&pd,&p));return p;
}
static neox_submission record(neox_device *d,uint8_t *pixels,neox_path a,neox_path b,neox_path fill,neox_texture atlas) {
    neox_target_desc td={0};neox_color clear={0};td.clear=&clear;
    td.soft.pixels=pixels;td.soft.width=48;td.soft.height=24;td.soft.stride=48*4;
    neox_frame *f;OK(neox_frame_begin(d,&td,&f));
    neox_state sa={0};sa.transform=(neox_affine){1,0,0,1,0,0};sa.opacity=1;sa.clip=&a;
    neox_state sb=sa;sb.clip=&b;
    neox_paint blue={{0,0,1,1}},white={{1,1,1,1}};
    neox_glyph glyphs[4];for(int i=0;i<4;i++)glyphs[i]=(neox_glyph){8+i*5,8,(uint32_t)i*4,0,3,5};
    /* 12 sibling pairs, depth two: old recording retained 24 full-frame mask pairs. */
    for(int i=0;i<12;i++) {
        OK(neox_push_state(f,&sa));OK(neox_push_state(f,&sb));
        OK(neox_draw_path(f,fill,&blue));OK(neox_draw_glyphs(f,atlas,glyphs,4,&white));
        OK(neox_pop_state(f));OK(neox_pop_state(f));
    }
    neox_submission sub;OK(neox_frame_end(f,&sub));return sub;
}
int main(void) {
    const uint8_t rows[4][5]={{5,7,7,7,5},{7,4,6,4,7},{7,5,5,5,7},{5,5,2,5,5}};
    uint8_t atlas_data[16*8]={0},pixels[48*24*4],expected[sizeof(pixels)];
    for(int g=0;g<4;g++)for(int y=0;y<5;y++)for(int x=0;x<3;x++)atlas_data[y*16+g*4+x]=(rows[g][y]&(4>>x))?255:0;
    memset(expected,0,sizeof(expected));
    for(int y=4;y<20;y++)for(int x=4;x<44;x++){expected[(y*48+x)*4+2]=255;expected[(y*48+x)*4+3]=255;}
    for(int g=0;g<4;g++)for(int y=0;y<5;y++)for(int x=0;x<3;x++)if(rows[g][y]&(4>>x))memset(expected+((y+8)*48+8+g*5+x)*4,255,4);
    neox_allocator al={NULL,take,give};neox_context_desc cd={&al,150*1024,0};neox_context *ctx;neox_device *d;
    OK(neox_context_create(&cd,&ctx));OK(neox_device_create(ctx,NULL,&d));
    neox_path a=rectangle(d,2,2,44,20),b=rectangle(d,4,4,40,16),fill=rectangle(d,0,0,48,24);
    neox_texture_desc td={16,8,NEOX_FORMAT_A8,atlas_data,16};neox_texture atlas;OK(neox_texture_create(d,&td,&atlas));
    size_t stable=0;
    for(int frame=0;frame<40;frame++) {
        neox_submission sub=record(d,pixels,a,b,fill,atlas);OK(neox_submission_wait(d,sub));neox_submission_release(d,sub);
        CHECK(!memcmp(pixels,expected,sizeof(pixels)));if(!frame)stable=live;else CHECK(live==stable);
    }
    /* Every execution allocation failure must preserve the borrowed target and
     * free temporary clip scopes. A new submission then succeeds in the same arena. */
    int completed=0;
    for(int nth=1;nth<1500;nth++) {
        memset(pixels,0x7b,sizeof(pixels));neox_submission sub=record(d,pixels,a,b,fill,atlas);
        fail_at=nth;neox_result result=neox_submission_wait(d,sub);fail_at=0;
        neox_submission_release(d,sub);CHECK(live==stable);
        if(result==NEOX_OK){CHECK(!memcmp(pixels,expected,sizeof(pixels)));completed=1;printf("execution allocation failure points: %d\n",nth-1);break;}
        CHECK(result==NEOX_ERROR_OUT_OF_MEMORY);for(size_t i=0;i<sizeof(pixels);i++)CHECK(pixels[i]==0x7b);
    }
    CHECK(completed);neox_cpu_memory_stats stats;OK(neox_context_get_memory_stats(ctx,&stats));CHECK(stats.peak_bytes<=cd.memory_budget);
    printf("ALL PASS (fixed arena=%u, requested peak=%zu, budget peak=%llu)\n",CAP,peak,(unsigned long long)stats.peak_bytes);
    neox_context_destroy(ctx);CHECK(live==0);for(size_t i=0;i<sizeof(occupied);i++)CHECK(!occupied[i]);return 0;
}
