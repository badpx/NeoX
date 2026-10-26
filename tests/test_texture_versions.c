#include "internal.h"
#include <stdio.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define OK(x) CHECK((x)==NEOX_OK)
static neox_frame *begin(neox_device*d,uint8_t *pixels) {
    neox_target_desc t={0};static const neox_color clear={0};neox_frame*f;
    t.clear=&clear;t.soft.pixels=pixels;t.soft.width=t.soft.height=8;t.soft.stride=32;
    OK(neox_frame_begin(d,&t,&f));return f;
}
static neox_submission end(neox_device*d,neox_frame*f) {
    (void)d;neox_submission s;OK(neox_frame_end(f,&s));return s;
}
static void wait_release(neox_device*d,neox_submission s) {
    OK(neox_submission_wait(d,s));neox_submission_release(d,s);neox_device_pump(d);
}
static void draw(neox_frame*f,neox_texture t,float x,float w) {
    OK(neox_draw_texture(f,t,(neox_rect){0,0,1,1},(neox_rect){x,0,w,8},NEOX_FILTER_NEAREST,1));
}
int main(void) {
    neox_context*c;neox_device*d;uint8_t pixels[256];
    OK(neox_context_create(NULL,&c));OK(neox_device_create(c,NULL,&d));
    uint8_t red[]={255,0,0,255},blue[]={0,0,255,255},green[]={0,255,0,255};
    neox_texture_desc td={1,1,NEOX_FORMAT_RGBA8_SRGB,red,4};neox_texture tex;
    OK(neox_texture_create(d,&td,&tex));neox_capture*cap;OK(neox_capture_begin(d,&cap));
    neox_frame*f=begin(d,pixels);draw(f,tex,0,4);
    nx_tex_slot *slot=&((nx_tex_slot *)d->textures.slots)[nx_handle_index(tex.id)];
    nx_tex_data *old=slot->data;CHECK(old->refs==3); /* slot, command, capture */
    /* Budget failure preserves both recorded and current content. */
    c->budget=c->used;
    CHECK(neox_texture_update(d,tex,(neox_rect){0,0,1,1},blue,4)==NEOX_ERROR_OUT_OF_MEMORY);
    CHECK(slot->data==old && old->version==1 && ((uint8_t *)old->pixels)[0]==255);
    c->budget=0;
    OK(neox_texture_update(d,tex,(neox_rect){0,0,1,1},blue,4));CHECK(slot->data!=old);
    draw(f,tex,4,4);neox_submission sub=end(d,f);
    OK(neox_texture_update(d,tex,(neox_rect){0,0,1,1},green,4));
    neox_texture_release(d,tex);wait_release(d,sub);
    CHECK(pixels[0]==255 && pixels[2]==0 && pixels[4*4]==0 && pixels[4*4+2]==255);
    uint8_t *capture;size_t size;OK(neox_capture_end(cap,&capture,&size));
    uint8_t reference[256];memcpy(reference,pixels,256);
    f=begin(d,pixels);OK(neox_capture_replay(f,capture,size));wait_release(d,end(d,f));
    CHECK(!memcmp(reference,pixels,256));
    neox_capture_free_buffer(c,capture,size);
    /* Old float-based CAP versions remain readable without requantization. */
    uint32_t legacy[]={0x4143584e,1,1,1, 1,1, 1,1,NEOX_FORMAT_RGBA8_SRGB,16,
        0x3f800000,0,0,0x3f800000, NXC_TEX,1, 0,0,0x3f800000,0x3f800000,
        0,0,0x41000000,0x41000000,NEOX_FILTER_NEAREST,0x3f800000};
    for(uint32_t version=1;version<=3;version++) {
        legacy[1]=version;f=begin(d,pixels);
        OK(neox_capture_replay(f,(uint8_t *)legacy,sizeof(legacy)));wait_release(d,end(d,f));
        CHECK(pixels[0]==255 && pixels[1]==0 && pixels[3]==255);
    }
    neox_device_destroy(d);CHECK(c->used==0);neox_context_destroy(c);
    /* 512x512 A8: a 320 KiB total budget can upload and render. Previously the
     * pixel storage alone required 4 MiB. No hidden system-heap fallback. */
    neox_context_desc cd={0};cd.memory_budget=320*1024;
    OK(neox_context_create(&cd,&c));OK(neox_device_create(c,NULL,&d));
    static uint8_t atlas[512*512];memset(atlas,255,sizeof(atlas));
    size_t before=c->used;td=(neox_texture_desc){512,512,NEOX_FORMAT_A8,atlas,512};
    OK(neox_texture_create(d,&td,&tex));CHECK(c->used-before==sizeof(nx_tex_data)+sizeof(atlas));
    f=begin(d,pixels);draw(f,tex,0,8);wait_release(d,end(d,f));CHECK(pixels[3]==255);
    slot=&((nx_tex_slot *)d->textures.slots)[nx_handle_index(tex.id)];old=slot->data;
    CHECK(neox_texture_update(d,tex,(neox_rect){NAN,0,1,1},atlas,1)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(neox_texture_update(d,tex,(neox_rect){0,0,INFINITY,1},atlas,1)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(neox_texture_update(d,tex,(neox_rect){.5f,0,1,1},atlas,1)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(neox_texture_update(d,tex,(neox_rect){0,0,1,2},atlas,SIZE_MAX)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(slot->data==old && old->version==1);
    OK(neox_texture_update(d,tex,(neox_rect){0,0,1,1},atlas,1));CHECK(slot->data==old && old->version==2);
    old->version=UINT64_MAX;
    CHECK(neox_texture_update(d,tex,(neox_rect){0,0,1,1},atlas,1)==NEOX_ERROR_UNSUPPORTED);
    neox_device_destroy(d);CHECK(c->used==0);neox_context_destroy(c);
    puts("ALL PASS (immutable versions, CAP snapshots, legacy replay, compact budget)");return 0;
}
