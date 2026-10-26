/* Uses installed headers and exported targets only: no src/ or build-tree paths. */
#include <neox.h>
#if NEOX_ENABLE_TEXT
#include <neox_text.h>
#endif
#ifdef NEOX_ENABLE_PRESENT
#include <neox_present.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"consumer line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define OK(x) CHECK((x)==NEOX_OK)
static void finish(neox_device *dev,neox_frame *frame)
{
    neox_submission sub;OK(neox_frame_end(frame,&sub));
    OK(neox_submission_wait(dev,sub));neox_submission_release(dev,sub);
}
int main(void)
{
    neox_context *ctx;neox_device *dev;neox_frame *frame;
    OK(neox_context_create(NULL,&ctx));OK(neox_device_create(ctx,NULL,&dev));
    uint8_t pixel[4]={0},red[4]={255,0,0,255};
    neox_texture tex;neox_texture_desc texture={1,1,NEOX_FORMAT_RGBA8_SRGB,red,4};
    OK(neox_texture_create(dev,&texture,&tex));
    neox_target_desc target={0};neox_color clear={0};target.clear=&clear;
    target.soft.pixels=pixel;target.soft.width=target.soft.height=1;target.soft.stride=4;
#if NEOX_ENABLE_CAPTURE
    neox_capture *cap;OK(neox_capture_begin(dev,&cap));
#endif
    OK(neox_frame_begin(dev,&target,&frame));
#if NEOX_ENABLE_SCENE
    neox_scene scene=neox_scene_create(dev);CHECK(scene.id);
    neox_node node;neox_node_desc desc={0};desc.kind=NEOX_NODE_IMAGE;
    desc.visible=1;desc.opacity=1;desc.transform.a=desc.transform.d=1;
    desc.image.tex=tex;desc.image.src.w=desc.image.src.h=1;desc.image.dst=desc.image.src;
    OK(neox_node_create(dev,scene,&desc,&node));
    OK(neox_scene_render(frame,scene,(neox_node){0}));neox_scene_destroy(dev,scene);
#else
    neox_rect rect={0,0,1,1};OK(neox_draw_texture(frame,tex,rect,rect,NEOX_FILTER_NEAREST,1));
#endif
    finish(dev,frame);CHECK(pixel[0]==255 && !pixel[1] && !pixel[2] && pixel[3]==255);
#if NEOX_ENABLE_CAPTURE
    uint8_t *data;size_t size;OK(neox_capture_end(cap,&data,&size));
    OK(neox_frame_begin(dev,&target,&frame));OK(neox_capture_replay(frame,data,size));
    finish(dev,frame);CHECK(pixel[0]==255 && pixel[3]==255);
    neox_capture_free_buffer(ctx,data,size);
#endif
#if NEOX_ENABLE_TEXT
    nx_text_context *text;nx_atlas *atlas;
    OK(nx_text_context_create(ctx,NULL,&text));OK(nx_atlas_create(text,dev,8,8,&atlas));
    nx_atlas_destroy(atlas);nx_text_context_destroy(text);
#endif
#ifdef NEOX_ENABLE_PRESENT
    /* Resolves the optional library without requiring a display server here. */
    CHECK(neox_window_present(NULL)==NEOX_ERROR_INVALID_ARGUMENT);
#endif
    neox_texture_release(dev,tex);neox_device_destroy(dev);neox_context_destroy(ctx);
    printf("consumer PASS: scene=%d capture=%d text=%d\n",NEOX_ENABLE_SCENE,NEOX_ENABLE_CAPTURE,NEOX_ENABLE_TEXT);
    return 0;
}
