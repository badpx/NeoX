/* Exercise exhaustion without allocating billions of commands. Include the
 * capture implementation to inspect private transaction checkpoints; link all
 * other production modules normally. Restore injected counters before cleanup. */
#include "../src/capture.c"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define OK(x) CHECK((x) == NEOX_OK)
#define OOM(x) CHECK((x) == NEOX_ERROR_OUT_OF_MEMORY)
static neox_result draw(neox_frame *f, neox_texture t)
{
    return neox_draw_texture(f,t,(neox_rect){0,0,1,1},(neox_rect){0,0,1,1},NEOX_FILTER_NEAREST,1);
}
int main(void)
{
    neox_context *ctx; neox_device *dev; neox_capture *cap; neox_frame *f;
    neox_texture tex; uint8_t red[4]={255,0,0,255}, pixels[4]={0};
    OK(neox_context_create(NULL,&ctx)); OK(neox_device_create(ctx,NULL,&dev));
    uint32_t capacity=8;uint64_t before=ctx->used;
    uint8_t storage[8]={0x69};
    CHECK(!nx_array_grow(ctx,storage,1,&capacity,(uint64_t)UINT32_MAX+1));
    CHECK(capacity==8 && storage[0]==0x69 && ctx->used==before);
    neox_texture_desc td={1,1,NEOX_FORMAT_RGBA8_SRGB,red,4};
    OK(neox_texture_create(dev,&td,&tex));
    nx_tex_slot *slot=&((nx_tex_slot *)dev->textures.slots)[nx_handle_index(tex.id)];
    nx_tex_data *data=slot->data;
    OK(neox_capture_begin(dev,&cap));
    neox_target_desc target={0};target.soft.pixels=pixels;
    target.soft.width=target.soft.height=1;target.soft.stride=4;
    OK(neox_frame_begin(dev,&target,&f));
    neox_state state={0};state.transform=nx_affine_identity();state.opacity=1;
    /* Warm capacities with an unrelated balanced scope. */
    OK(neox_push_state(f,&state)); OK(neox_pop_state(f));
    uint32_t cmds=f->cmd_count, cap_cmds=cap->cmd_count;
    uint64_t used=ctx->used;
    f->cmd_count=UINT32_MAX; OOM(draw(f,tex));
    CHECK(f->cmd_count==UINT32_MAX && !slot->refcount && data->refs==1);
    f->cmd_count=cmds;
    f->ctm_count=UINT32_MAX; OOM(neox_push_state(f,&state));
    CHECK(f->ctm_count==UINT32_MAX);f->ctm_count=1;
    CHECK(ctx->used==used && cap->cmd_count==cap_cmds && !cap->res_count);

    /* Slot and version limits are independent, and neither may partially pin. */
    slot->refcount=UINT32_MAX;OOM(draw(f,tex));
    CHECK(slot->refcount==UINT32_MAX && data->refs==1);slot->refcount=0;
    data->refs=UINT32_MAX;OOM(draw(f,tex));
    CHECK(!slot->refcount && data->refs==UINT32_MAX);data->refs=1;
    CHECK(f->cmd_count==cmds && cap->cmd_count==cap_cmds && !cap->res_count);

    /* Command pin succeeds at MAX-1, but a new CAP snapshot cannot add its
     * second pin. Both recording destinations must roll back. */
    data->refs=UINT32_MAX-1;OOM(draw(f,tex));
    CHECK(!slot->refcount && data->refs==UINT32_MAX-1);
    CHECK(f->cmd_count==cmds && cap->cmd_count==cap_cmds && !cap->res_count);
    data->refs=1;
    cap->cmd_count=UINT32_MAX;OOM(draw(f,tex));
    CHECK(!slot->refcount && data->refs==1 && f->cmd_count==cmds);
    cap->cmd_count=UINT32_MAX-1;
    nx_cmd pair[2]={{0},{0}};pair[0].op=pair[1].op=NXC_POP;
    OOM(nx_capture_record_batch(cap,pair,2));CHECK(cap->cmd_count==UINT32_MAX-1);
    cap->cmd_count=cap_cmds;

    /* Existing snapshots need no extra version pin; at the last available
     * reference, a second command fails but the first remains intact. */
    OK(draw(f,tex));CHECK(cap->res_count==1 && data->refs==3 && slot->refcount==1);
    data->refs=UINT32_MAX-1;OK(draw(f,tex));OOM(draw(f,tex));
    CHECK(data->refs==UINT32_MAX && slot->refcount==2);
    CHECK(f->cmd_count==cmds+2 && cap->cmd_count==cap_cmds+2 && cap->res_count==1);
    data->refs=4;
    neox_frame_cancel(f);CHECK(!slot->refcount && data->refs==1 && !cap->res_count && !cap->cmd_count);
    neox_capture_cancel(cap);

    /* Retained rendering must undo its earlier successful commands when a
     * later reference fails; no partial scene may reach the caller or CAP. */
    neox_scene scene;neox_node node;
    scene=neox_scene_create(dev);CHECK(scene.id);
    neox_node_desc desc={0};desc.kind=NEOX_NODE_IMAGE;desc.transform=nx_affine_identity();
    desc.opacity=1;desc.visible=1;desc.image.tex=tex;
    desc.image.src=desc.image.dst=(neox_rect){0,0,1,1};
    OK(neox_node_create(dev,scene,&desc,&node));OK(neox_node_create(dev,scene,&desc,&node));
    OK(neox_capture_begin(dev,&cap));OK(neox_frame_begin(dev,&target,&f));
    slot->refcount=UINT32_MAX-1;
    OOM(neox_scene_render(f,scene,(neox_node){0}));
    CHECK(slot->refcount==UINT32_MAX-1 && data->refs==1 && !f->cmd_count && !cap->cmd_count);
    slot->refcount=0;OK(neox_scene_render(f,scene,(neox_node){0}));
    neox_frame_cancel(f);neox_capture_cancel(cap);neox_scene_destroy(dev,scene);

    /* Path commands and clipped PUSH use the same checked pin operation. */
    uint8_t verbs[]={NEOX_VERB_MOVE,NEOX_VERB_LINE,NEOX_VERB_LINE,NEOX_VERB_CLOSE};
    float points[]={0,0,1,0,0,1};neox_path path;
    neox_path_desc pd={verbs,points,4,3,NEOX_FILL_NONZERO};OK(neox_path_create(dev,&pd,&path));
    nx_path_slot *ps=&((nx_path_slot *)dev->paths.slots)[nx_handle_index(path.id)];
    OK(neox_frame_begin(dev,&target,&f));ps->refcount=UINT32_MAX;
    neox_paint paint={{1,0,0,1}};OOM(neox_draw_path(f,path,&paint));
    state.clip=&path;OOM(neox_push_state(f,&state));
    CHECK(ps->refcount==UINT32_MAX && !f->cmd_count && f->ctm_count==1);
    ps->refcount=0;neox_frame_cancel(f);neox_path_release(dev,path);
    neox_texture_release(dev,tex);neox_device_destroy(dev);CHECK(!ctx->used);neox_context_destroy(ctx);
    puts("ALL PASS (recording count exhaustion, atomic pins, CAP and scene rollback)");
    return 0;
}
