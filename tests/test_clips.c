/* Analytic rectangle intersections, independent of backend rasterization. */
#include "neox.h"
#if NEOX_ENABLE_GL
#include "neox_gl.h"
#endif
#if NEOX_ENABLE_METAL
#include "neox_mtl.h"
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);} }while(0)
#define OK(x) CHECK((x)==NEOX_OK)
static neox_affine identity(void) { return (neox_affine){1,0,0,1,0,0}; }
static neox_path rect(neox_device*d,float lo,float hi) {
    uint8_t v[]={0,1,1,1,4};float p[]={lo,0,hi,0,hi,8,lo,8};
    neox_path_desc desc={v,p,5,4,NEOX_FILL_NONZERO};neox_path h;
    OK(neox_path_create(d,&desc,&h));return h;
}
static neox_state state(neox_path *clip,float opacity) {
    neox_state s={0};s.transform=identity();s.opacity=opacity;s.clip=clip;return s;
}
static void finish(neox_device*d,neox_frame*f) {
    neox_submission sub;OK(neox_frame_end(f,&sub));OK(neox_submission_wait(d,sub));
    neox_submission_release(d,sub);
}
static int metal;
static void run(neox_context *ctx,neox_device*d,neox_target_desc*t,neox_path a,neox_path b,
                float lo,float hi,float opacity,int frontend) {
    neox_frame*f;neox_path fill=rect(d,0,8);neox_paint white={{1,1,1,1}};
    neox_state sa=state(&a,1),sb=state(&b,opacity);
    uint8_t *capture=NULL;size_t capture_size=0;neox_scene scene={0};
    if (frontend==1) {
        scene=neox_scene_create(d);CHECK(scene.id);neox_node group,node;
        neox_node_desc n={0};n.kind=NEOX_NODE_GROUP;n.visible=1;n.opacity=opacity;n.transform=identity();n.group.clip=b;
        OK(neox_node_create(d,scene,&n,&group));
        n.kind=NEOX_NODE_PATH;n.opacity=1;n.path.path=fill;n.path.paint=white;
        OK(neox_node_create(d,scene,&n,&node));OK(neox_node_append_child(d,group,node));
    } else if (frontend==2) {
        neox_capture*c;OK(neox_capture_begin(d,&c));OK(neox_frame_begin(d,t,&f));
        OK(neox_push_state(f,&sb));OK(neox_draw_path(f,fill,&white));OK(neox_pop_state(f));finish(d,f);
        OK(neox_capture_end(c,&capture,&capture_size));
    }
    OK(neox_frame_begin(d,t,&f));
    /* A closed earlier clip must not participate in the intersection. */
    neox_path empty=rect(d,0,0);neox_state se=state(&empty,1);
    OK(neox_push_state(f,&se));OK(neox_pop_state(f));
    OK(neox_push_state(f,&sa));
    if (frontend==1) OK(neox_scene_render(f,scene,(neox_node){0}));
    else if (frontend==2) OK(neox_capture_replay(f,capture,capture_size));
    else {OK(neox_push_state(f,&sb));OK(neox_draw_path(f,fill,&white));OK(neox_pop_state(f));}
    OK(neox_pop_state(f));finish(d,f);
    uint8_t pixels[256];
    if (t->kind==NEOX_BACKEND_SOFT) memcpy(pixels,t->soft.pixels,sizeof(pixels));
#if NEOX_ENABLE_GL
    else if (t->kind==NEOX_BACKEND_GPU && !metal) OK(neox_gl_surface_read_pixels(t->surface,pixels));
#endif
#if NEOX_ENABLE_METAL
    else if (metal) OK(neox_mtl_surface_read_pixels(t->surface,pixels));
#endif
    else CHECK(0);
    for(int y=0;y<8;y++) for(int x=0;x<8;x++) {
        float cov=fmaxf(0,fminf(hi,x+1)-fmaxf(lo,x));
        int expected=(int)floorf(cov*opacity*255+.5f);
        int actual=pixels[(y*8+x)*4+3];
        if(abs(expected-actual)>1) {fprintf(stderr,"frontend=%d pixel=%d,%d expected=%d actual=%d\n",frontend,x,y,expected,actual);exit(1);}
    }
    neox_path_release(d,empty);neox_path_release(d,fill);
    if(scene.id) neox_scene_destroy(d,scene);
    if(capture) neox_capture_free_buffer(ctx,capture,capture_size);
}
int main(int argc,char**argv) {
    neox_backend_kind kind=NEOX_BACKEND_SOFT;
    if(argc>1&&!strcmp(argv[1],"gl"))kind=NEOX_BACKEND_GPU;
    if(argc>1&&!strcmp(argv[1],"metal")){kind=NEOX_BACKEND_GPU;metal=1;}
    neox_context *test_context;
    neox_device*d;neox_device_desc dd={0};dd.backend=kind;
#if NEOX_ENABLE_GL
    neox_gl_device_desc gd={0};gd.struct_size=sizeof(gd);gd.magic=NEOX_GL_DESC_MAGIC;
    if(kind==NEOX_BACKEND_GPU&&!metal){dd.platform=&gd;dd.platform_size=sizeof(gd);}
#endif
#if NEOX_ENABLE_METAL
    neox_mtl_device_desc md={0};md.struct_size=sizeof(md);md.magic=NEOX_MTL_DESC_MAGIC;
    if(metal){dd.platform=&md;dd.platform_size=sizeof(md);}
#endif
    OK(neox_context_create(NULL,&test_context));
    if(neox_device_create(test_context,&dd,&d)!=NEOX_OK) {
        neox_context_destroy(test_context);
        CHECK(!(metal?getenv("NEOX_REQUIRE_METAL"):getenv("NEOX_REQUIRE_GL")));
        return 77;
    }
    uint8_t pixels[256];static const neox_color clear={0};neox_target_desc target={0};
    target.kind=kind;target.clear=&clear;
    if(kind==NEOX_BACKEND_SOFT){target.soft.pixels=pixels;target.soft.width=target.soft.height=8;target.soft.stride=32;}
#if NEOX_ENABLE_GL
    if(kind==NEOX_BACKEND_GPU&&!metal){neox_gl_surface*s;OK(neox_gl_surface_create(d,8,8,&s));target.surface=s;}
#endif
#if NEOX_ENABLE_METAL
    if(metal){neox_mtl_surface*s;OK(neox_mtl_surface_create(d,8,8,&s));target.surface=s;}
#endif
    neox_path a=rect(d,.5f,4.5f),b=rect(d,1.25f,4.75f),left=rect(d,0,.375f),right=rect(d,.625f,1);
    for(int frontend=0;frontend<3;frontend++) {
        run(test_context,d,&target,a,a,.5f,4.5f,1,frontend);
        run(test_context,d,&target,a,a,.5f,4.5f,.5f,frontend);
        run(test_context,d,&target,a,b,1.25f,4.5f,1,frontend);
        run(test_context,d,&target,b,a,1.25f,4.5f,1,frontend);
        run(test_context,d,&target,left,right,0,0,1,frontend);
    }
#if NEOX_ENABLE_GL
    if(kind==NEOX_BACKEND_GPU&&!metal)neox_gl_surface_destroy(d,target.surface);
#endif
#if NEOX_ENABLE_METAL
    if(metal)neox_mtl_surface_destroy(d,target.surface);
#endif
    neox_context_destroy(test_context);puts("ALL PASS (analytic clipping across immediate/retained/CAP)");return 0;
}
