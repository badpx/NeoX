#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } }while(0)
#define OK(x) CHECK((x)==NEOX_OK)
static neox_frame *begin(neox_device *d,uint8_t *px) {
    static const neox_color clear={0}; neox_target_desc t={0}; neox_frame *f;
    t.soft.pixels=px;t.soft.width=t.soft.height=8;t.soft.stride=32;t.clear=&clear;
    OK(neox_frame_begin(d,&t,&f));return f;
}
static void finish(neox_device*d,neox_frame*f) {
    neox_submission s;OK(neox_frame_end(f,&s));OK(neox_submission_wait(d,s));
    neox_submission_release(d,s);neox_device_pump(d);
}
static neox_node_desc group(void) {
    neox_node_desc n={0};n.kind=NEOX_NODE_GROUP;n.visible=1;n.opacity=1;n.transform=nx_affine_identity();return n;
}
int main(void) {
    neox_context*c;neox_device*d;uint8_t a[256],b[256],pixels[64];
    neox_frame*f;neox_texture tex;neox_scene scene;neox_node glyph,parent;
    neox_glyph g={0,0,0,0,4,4};neox_paint paint={{1,1,1,1}};
    neox_glyph_style style={NEOX_ATLAS_MSDF,4,1};
    OK(neox_context_create(NULL,&c));OK(neox_device_create(c,NULL,&d));
    for(int i=0;i<16;i++){pixels[i*4]=pixels[i*4+1]=pixels[i*4+2]=128;pixels[i*4+3]=255;}
    neox_texture_desc td={4,4,NEOX_FORMAT_RGBA8_DATA,pixels,16};OK(neox_texture_create(d,&td,&tex));
    f=begin(d,a);OK(neox_draw_glyphs_ex(f,tex,&g,1,&paint,&style));finish(d,f);
    for(int i=0;i<16;i++)pixels[i*4+3]=0;
    OK(neox_texture_update(d,tex,(neox_rect){0,0,4,4},pixels,16));
    f=begin(d,b);OK(neox_draw_glyphs_ex(f,tex,&g,1,&paint,&style));finish(d,f);
    CHECK(!memcmp(a,b,sizeof(a)));CHECK(a[(1*8+1)*4+3]==125);
    scene=neox_scene_create(d);CHECK(scene.id);
    neox_node_desc nd=group();OK(neox_node_create(d,scene,&nd,&parent));
    nd.kind=NEOX_NODE_GLYPHS;nd.glyphs.atlas=tex;nd.glyphs.glyphs=&g;
    nd.glyphs.count=1;nd.glyphs.paint=paint;nd.glyphs.style=style;
    OK(neox_node_create(d,scene,&nd,&glyph));
    OK(neox_node_remove_from_parent(d,glyph));OK(neox_node_append_child(d,parent,glyph));
    CHECK(neox_node_append_child(d,parent,parent)==NEOX_ERROR_INVALID_ARGUMENT);
    neox_state state={0};state.transform=nx_affine_identity();state.transform.tx=3;state.opacity=1;
    f=begin(d,a);OK(neox_push_state(f,&state));OK(neox_draw_glyphs_ex(f,tex,&g,1,&paint,&style));OK(neox_pop_state(f));finish(d,f);
    f=begin(d,b);OK(neox_push_state(f,&state));OK(neox_scene_render(f,scene,(neox_node){0}));OK(neox_pop_state(f));finish(d,f);
    CHECK(!memcmp(a,b,sizeof(a)));CHECK(b[3]==0 && b[(1*8+4)*4+3]==125);
    /* Capture the retained frontend, replay through validated commands. */
    neox_capture*cap;uint8_t*bytes;size_t size;
    OK(neox_capture_begin(d,&cap));f=begin(d,a);
    OK(neox_scene_render(f,scene,(neox_node){0}));finish(d,f);
    OK(neox_capture_end(cap,&bytes,&size));
    f=begin(d,b);OK(neox_capture_replay(f,bytes,size));finish(d,f);CHECK(!memcmp(a,b,sizeof(a)));
    size_t before=c->used;
    for(int i=0;i<100;i++){f=begin(d,b);OK(neox_capture_replay(f,bytes,size));finish(d,f);}
    CHECK(c->used==before);
    /* Every truncated prefix must fail without changing the existing frame. */
    for(size_t n=16;n<size;n++) {
        f=begin(d,b);OK(neox_draw_glyphs_ex(f,tex,&g,1,&paint,&style));
        uint32_t commands=f->cmd_count,draws=f->draw_calls;
        CHECK(neox_capture_replay(f,bytes,n)!=NEOX_OK);
        CHECK(f->cmd_count==commands && f->draw_calls==draws);
        finish(d,f);CHECK(!memcmp(a,b,sizeof(a)));
    }
    uint32_t bad[]={0x4143584e,3,0,1,NXC_POP};
    f=begin(d,b);CHECK(neox_capture_replay(f,(uint8_t*)bad,sizeof(bad))!=NEOX_OK);CHECK(f->cmd_count==0);neox_frame_cancel(f);
    /* Partial traversal failure leaves both frame and active capture unchanged. */
    neox_texture_release(d,tex);neox_device_pump(d);
    OK(neox_capture_begin(d,&cap));f=begin(d,b);
    CHECK(neox_scene_render(f,scene,(neox_node){0})==NEOX_ERROR_STALE_HANDLE);
    CHECK(f->cmd_count==0);neox_frame_cancel(f);
    uint8_t*empty;size_t empty_size;OK(neox_capture_end(cap,&empty,&empty_size));CHECK(empty_size==16);
    neox_capture_free_buffer(c,empty,empty_size);neox_capture_free_buffer(c,bytes,size);
    /* Detached nodes remain scene-owned and are invalidated by scene destruction. */
    OK(neox_node_remove_from_parent(d,glyph));neox_scene_destroy(d,scene);
    CHECK(neox_node_set_visible(d,glyph,0)==NEOX_ERROR_STALE_HANDLE);
    neox_device_destroy(d);CHECK(c->used==0);neox_context_destroy(c);
    puts("ALL PASS (data, retained, CAP, rollback and lifetime contracts)");return 0;
}
