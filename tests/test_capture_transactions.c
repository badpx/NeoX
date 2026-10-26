#include "internal.h"
#include <stdio.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define OK(x) CHECK((x)==NEOX_OK)
static neox_frame *begin(neox_device*d,uint8_t *pixels) {
    static const neox_color clear={0};neox_target_desc t={0};neox_frame*f;
    t.clear=&clear;t.soft.pixels=pixels;t.soft.width=2;t.soft.height=1;t.soft.stride=8;
    OK(neox_frame_begin(d,&t,&f));return f;
}
static void draw(neox_frame*f,neox_texture tex) {
    OK(neox_draw_texture(f,tex,(neox_rect){0,0,1,1},(neox_rect){0,0,2,1},NEOX_FILTER_NEAREST,1));
}
static void finish(neox_device*d,neox_frame*f) {
    neox_submission sub;OK(neox_frame_end(f,&sub));OK(neox_submission_wait(d,sub));neox_submission_release(d,sub);neox_device_pump(d);
}
int main(void) {
    neox_context*c;neox_device*d;neox_capture*cap,*invalid;neox_frame*f;neox_texture tex;
    uint8_t red[]={255,0,0,255},blue[]={0,0,255,255},px[8],*bytes=NULL;size_t size=0;
    OK(neox_context_create(NULL,&c));OK(neox_device_create(c,NULL,&d));
    neox_texture_desc td={1,1,NEOX_FORMAT_RGBA8_SRGB,red,4};OK(neox_texture_create(d,&td,&tex));
    f=begin(d,px);CHECK(neox_capture_begin(d,&invalid)==NEOX_ERROR_INVALID_ARGUMENT);CHECK(!invalid);neox_frame_cancel(f);
    OK(neox_capture_begin(d,&cap));f=begin(d,px);
    neox_state state={0};state.transform=nx_affine_identity();state.opacity=1;
    OK(neox_push_state(f,&state));draw(f,tex);
    CHECK(neox_capture_end(cap,&bytes,&size)==NEOX_ERROR_INVALID_ARGUMENT);CHECK(!bytes && !size);
    neox_submission sub;CHECK(neox_frame_end(f,&sub)==NEOX_ERROR_INVALID_ARGUMENT);
    neox_frame_cancel(f);OK(neox_capture_end(cap,&bytes,&size));CHECK(size==16);
    neox_capture_free_buffer(c,bytes,size);
    /* Cancel a later version without losing an earlier committed frame. */
    OK(neox_capture_begin(d,&cap));f=begin(d,px);draw(f,tex);finish(d,f);
    nx_tex_slot*slot=&((nx_tex_slot*)d->textures.slots)[nx_handle_index(tex.id)];nx_tex_data*red_version=slot->data;
    CHECK(red_version->refs==2);
    OK(neox_texture_update(d,tex,(neox_rect){0,0,1,1},blue,4));
    f=begin(d,px);draw(f,tex);CHECK(slot->data->refs==3);neox_frame_cancel(f);
    CHECK(slot->data->refs==1 && red_version->refs==1);
    size_t before=c->used;c->budget=before;
    CHECK(neox_capture_end(cap,&bytes,&size)==NEOX_ERROR_OUT_OF_MEMORY);CHECK(!bytes && !size && c->used==before);
    c->budget=0;OK(neox_capture_end(cap,&bytes,&size));
    f=begin(d,px);OK(neox_capture_replay(f,bytes,size));finish(d,f);CHECK(px[0]==255 && px[2]==0);
    neox_capture_free_buffer(c,bytes,size);
    /* Capture cancellation is independent of the frame's command lifetime. */
    OK(neox_capture_begin(d,&cap));f=begin(d,px);draw(f,tex);neox_capture_cancel(cap);
    CHECK(!f->capture_owner);draw(f,tex);finish(d,f);CHECK(px[0]==0 && px[2]==255);
    /* frame_end OOM does not commit or detach the capture checkpoint. */
    OK(neox_capture_begin(d,&cap));f=begin(d,px);draw(f,tex);
    /* A full table forces an allocation before publication. Restore the test
     * fixture's logical count afterward so ordinary slots remain valid. */
    uint32_t old_count=d->subs.count,old_free=d->subs.free_head;
    d->subs.count=d->subs.capacity;d->subs.free_head=UINT32_MAX;c->budget=c->used;
    CHECK(neox_frame_end(f,&sub)==NEOX_ERROR_OUT_OF_MEMORY);CHECK(f->capture_owner==cap);
    d->subs.count=old_count;d->subs.free_head=old_free;c->budget=0;
    neox_frame_cancel(f);OK(neox_capture_end(cap,&bytes,&size));CHECK(size==16);neox_capture_free_buffer(c,bytes,size);
    OK(neox_capture_begin(d,&cap));f=begin(d,px);draw(f,tex);
    neox_device_destroy(d);CHECK(c->used==0);neox_context_destroy(c);
    puts("ALL PASS (capture boundaries, cancel rollback, retry and ownership)");return 0;
}
