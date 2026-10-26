#include "execution.h"
neox_result nx_execution_clip_push(neox_device *dev, nx_execution_clips *s,
    const nx_cmd *cmd, uint32_t w, uint32_t h, const float **out)
{
    neox_context *ctx=dev->ctx;
    *out=NULL;
    if(cmd->clip_idx<0)return NEOX_OK;
    nx_clip_path *paths=nx_array_grow(ctx,s->paths,sizeof(*paths),&s->path_capacity,(uint64_t)s->count+1);
    if(!paths)return NEOX_ERROR_OUT_OF_MEMORY;
    s->paths=paths;
    nx_clip_scope *scopes=nx_array_grow(ctx,s->scopes,sizeof(*scopes),&s->scope_capacity,(uint64_t)s->count+1);
    if(!scopes)return NEOX_ERROR_OUT_OF_MEMORY;
    s->scopes=scopes;
    s->bytes=(size_t)w*h*sizeof(float); /* target validation checked product */
    nx_clip_scope next={0};
    next.full=nx_alloc(ctx,s->bytes,16);next.conditional=nx_alloc(ctx,s->bytes,16);
    neox_result r=NEOX_ERROR_OUT_OF_MEMORY;
    if(!next.full || !next.conditional)goto fail;
    s->paths[s->count]=(nx_clip_path){&((nx_path_slot *)dev->paths.slots)[cmd->clip_idx],cmd->ctm};
    r=nx_rasterize_intersection(ctx,s->paths,s->count+1,w,h,next.full);
    if(r!=NEOX_OK)goto fail;
    const float *parent=s->count?s->scopes[s->count-1].full:NULL;
    for(size_t i=0;i<(size_t)w*h;i++)
        next.conditional[i]=parent?(parent[i]>0?fminf(1,fmaxf(0,next.full[i]/parent[i])):0):next.full[i];
    s->scopes[s->count++]=next;*out=next.conditional;return NEOX_OK;
fail:
    nx_free(ctx,next.full,s->bytes,16);nx_free(ctx,next.conditional,s->bytes,16);return r;
}
void nx_execution_clip_pop(neox_context *ctx,nx_execution_clips *s)
{
    nx_clip_scope *top=&s->scopes[--s->count];
    nx_free(ctx,top->full,s->bytes,16);nx_free(ctx,top->conditional,s->bytes,16);
}
void nx_execution_clips_destroy(neox_context *ctx,nx_execution_clips *s)
{
    while(s->count)nx_execution_clip_pop(ctx,s);
    nx_free(ctx,s->paths,(size_t)s->path_capacity*sizeof(*s->paths),16);
    nx_free(ctx,s->scopes,(size_t)s->scope_capacity*sizeof(*s->scopes),16);
}
