/* Backend-neutral texture/path ownership and compact, versioned pixels. */
#include "internal.h"

static uint32_t tex_bpp(neox_pixel_format format) { return format==NEOX_FORMAT_A8 ? 1u : 4u; }
static int tex_format_valid(neox_pixel_format format) { return (unsigned)format<=NEOX_FORMAT_RGBA8_DATA; }
static int tex_stride_valid(uint32_t w,uint32_t h,uint32_t bpp,size_t stride)
{
    size_t span;
    return nx_image_span(w, h, bpp, stride, &span);
}
static nx_tex_data *tex_data_create(neox_context *ctx,uint32_t w,uint32_t h,
                                   neox_pixel_format format,uint32_t linear)
{
    uint32_t bpp=linear ? 16u : tex_bpp(format);
    size_t pixels, bytes, total;
    if (!w || !h || !nx_size_mul(w, h, &pixels) ||
        !nx_size_mul(pixels, bpp, &bytes) ||
        !nx_size_add(sizeof(nx_tex_data), bytes, &total)) return NULL;
    nx_tex_data *d=nx_alloc(ctx,total,16);
    if (!d) return NULL;
    d->version=1;d->refs=1;d->linear=linear;d->bytes=bytes;d->pixels=d+1;
    return d;
}
void nx_tex_data_release(neox_context *ctx,nx_tex_data *d)
{
    if(d && --d->refs==0) nx_free(ctx,d,sizeof(*d)+d->bytes,16);
}
static void tex_decode(neox_pixel_format format,const uint8_t *p,float out[4])
{
    if(format==NEOX_FORMAT_A8){out[0]=out[1]=out[2]=out[3]=p[0]/255.0f;return;}
    float a=p[3]/255.0f, factor=format==NEOX_FORMAT_RGBA8_DATA ? 1.0f : a;
    for(int i=0;i<3;i++) {
        float v=p[i]/255.0f;
        if(format==NEOX_FORMAT_RGBA8_SRGB)v=nx_srgb_to_linear(v);
        out[i]=v*factor;
    }
    out[3]=a;
}
void nx_texel_load(const nx_tex_slot *s,const nx_tex_data *d,size_t pixel,float out[4])
{
    if(d->linear) memcpy(out,(const float *)d->pixels+pixel*4,16);
    else tex_decode(s->format,(const uint8_t *)d->pixels+pixel*tex_bpp(s->format),out);
}
void nx_texture_expand_row(const nx_tex_slot *s,const nx_tex_data *d,uint32_t y,float *out)
{
    for(uint32_t x=0;x<s->width;x++) nx_texel_load(s,d,(size_t)y*s->width+x,out+x*4);
}
static neox_result tex_publish(neox_device *dev,uint32_t w,uint32_t h,
                               neox_pixel_format format,nx_tex_data *data,neox_texture *out)
{
    uint32_t idx=nx_table_alloc(dev->ctx,&dev->textures);
    if(idx==UINT32_MAX){nx_tex_data_release(dev->ctx,data);return NEOX_ERROR_OUT_OF_MEMORY;}
    nx_tex_slot *s=&((nx_tex_slot *)dev->textures.slots)[idx];
    s->width=w;s->height=h;s->format=format;s->data=data;
    *out=(neox_texture){nx_make_handle(dev->id,NX_TYPE_TEXTURE,idx,s->gen)};
    return NEOX_OK;
}
neox_result nx_texture_from_linear(neox_device *dev,uint32_t w,uint32_t h,
                                    neox_pixel_format format,const float *pixels,neox_texture *out)
{
    nx_tex_data *d=tex_data_create(dev->ctx,w,h,format,1);
    if(!d)return NEOX_ERROR_OUT_OF_MEMORY;
    memcpy(d->pixels,pixels,d->bytes);
    return tex_publish(dev,w,h,format,d,out);
}
neox_result neox_texture_create(neox_device *dev,const neox_texture_desc *desc,neox_texture *out)
{
    if(!dev || !out)return NEOX_ERROR_INVALID_ARGUMENT;
    *out=(neox_texture){0};neox_context *ctx=dev->ctx;
    if(!desc || !desc->pixels || !desc->width || !desc->height || !tex_format_valid(desc->format))
        NX_ERR(ctx,"invalid texture descriptor",NEOX_ERROR_INVALID_ARGUMENT);
    if(desc->width>NX_MAX_TEXTURE_SIZE || desc->height>NX_MAX_TEXTURE_SIZE)
        NX_ERR(ctx,"texture size exceeds limit",NEOX_ERROR_UNSUPPORTED);
    size_t row=(size_t)desc->width*tex_bpp(desc->format);
    size_t stride=desc->row_stride ? desc->row_stride : row;
    if(!tex_stride_valid(desc->width,desc->height,tex_bpp(desc->format),stride))
        NX_ERR(ctx,"invalid or overflowing texture stride",NEOX_ERROR_INVALID_ARGUMENT);
    nx_tex_data *d=tex_data_create(ctx,desc->width,desc->height,desc->format,0);
    if(!d)NX_ERR(ctx,"texture allocation failed",NEOX_ERROR_OUT_OF_MEMORY);
    for(uint32_t y=0;y<desc->height;y++)
        memcpy((uint8_t *)d->pixels+y*row,(const uint8_t *)desc->pixels+y*stride,row);
    return tex_publish(dev,desc->width,desc->height,desc->format,d,out);
}
neox_result neox_texture_update(neox_device *dev,neox_texture tex,
                                neox_rect region,const void *pixels,size_t row_stride)
{
    if(!dev || !pixels)return NEOX_ERROR_INVALID_ARGUMENT;
    neox_context *ctx=dev->ctx;uint32_t idx;
    neox_result result=nx_resolve(dev,tex.id,NX_TYPE_TEXTURE,&idx);
    if(result!=NEOX_OK)return result;
    nx_tex_slot *s=&((nx_tex_slot *)dev->textures.slots)[idx];
    const float *f=&region.x;
    for(int i=0;i<4;i++)
        if(!isfinite(f[i]) || f[i]<0 || floorf(f[i])!=f[i])
            NX_ERR(ctx,"update region must contain finite nonnegative integers",NEOX_ERROR_INVALID_ARGUMENT);
    if(region.x>s->width || region.y>s->height || region.w>s->width-region.x || region.h>s->height-region.y)
        NX_ERR(ctx,"update region out of bounds",NEOX_ERROR_INVALID_ARGUMENT);
    uint32_t x0=(uint32_t)region.x,y0=(uint32_t)region.y,w=(uint32_t)region.w,h=(uint32_t)region.h;
    if(!w || !h)return NEOX_OK;
    uint32_t bpp=tex_bpp(s->format);
    if(!row_stride)row_stride=(size_t)w*bpp;
    if(!tex_stride_valid(w,h,bpp,row_stride))
        NX_ERR(ctx,"invalid or overflowing update stride",NEOX_ERROR_INVALID_ARGUMENT);
    nx_tex_data *old=s->data,*next=old;
    if(old->version==UINT64_MAX)NX_ERR(ctx,"texture version exhausted",NEOX_ERROR_UNSUPPORTED);
    if(old->refs>1) {
        next=tex_data_create(ctx,s->width,s->height,s->format,old->linear);
        if(!next)NX_ERR(ctx,"texture version allocation failed",NEOX_ERROR_OUT_OF_MEMORY);
        memcpy(next->pixels,old->pixels,old->bytes);
    }
    for(uint32_t y=0;y<h;y++) {
        const uint8_t *row=(const uint8_t *)pixels+(size_t)y*row_stride;
        size_t offset=(size_t)(y0+y)*s->width+x0;
        if(next->linear) {
            for(uint32_t x=0;x<w;x++) tex_decode(s->format,row+x*bpp,(float *)next->pixels+(offset+x)*4);
        } else memcpy((uint8_t *)next->pixels+offset*bpp,row,(size_t)w*bpp);
    }
    next->version=old->version+1;
    if(next!=old){s->data=next;nx_tex_data_release(ctx,old);}
    return NEOX_OK;
}

void neox_texture_release(neox_device *dev, neox_texture tex)
{
    nx_tex_slot *s;
    uint32_t idx;
    neox_result r;
    if (!dev) return;
    r = nx_resolve(dev, tex.id, NX_TYPE_TEXTURE, &idx);
    if (r != NEOX_OK) return; /* 句柄已失效：静默忽略（void 契约） */
    s = &((nx_tex_slot *)dev->textures.slots)[idx];
    s->in_use = 0;          /* invalidate now; storage remains pinned */
    s->deferred = 1;
    s->defer_next = dev->defer_head;
    dev->defer_head = (NX_TYPE_TEXTURE << 24) | idx;
}

/* ------------------------------------------------------------------ 路径 */

static uint32_t verb_points(neox_path_verb v)
{
    switch (v) {
    case NEOX_VERB_MOVE:  return 1;
    case NEOX_VERB_LINE:  return 1;
    case NEOX_VERB_QUAD:  return 2;
    case NEOX_VERB_CUBIC: return 3;
    case NEOX_VERB_CLOSE: return 0;
    }
    return 0;
}

/* 内部构造：直接以路径数据拷贝建路径（CAP 回放用） */
static neox_result path_from_copy(neox_device *dev, const uint8_t *verbs,
                                       uint32_t verb_count, const float *points,
                                       uint32_t point_count,
                                       neox_fill_rule rule, neox_path *out)
{
    neox_context *ctx = dev->ctx;
    nx_path_slot *s;
    uint8_t *vb;
    float *pt;
    uint32_t idx;
    vb = (uint8_t *)nx_alloc(ctx, verb_count ? verb_count : 1, 4);
    if (!vb) return NEOX_ERROR_OUT_OF_MEMORY;
    pt = (float *)nx_alloc(ctx, point_count ? (size_t)point_count * 8 : 1, 16);
    if (!pt) {
        nx_free(ctx, vb, verb_count ? verb_count : 1, 4);
        return NEOX_ERROR_OUT_OF_MEMORY;
    }
    if (verb_count) memcpy(vb, verbs, verb_count);
    if (point_count) memcpy(pt, points, (size_t)point_count * 8);
    idx = nx_table_alloc(ctx, &dev->paths);
    if (idx == UINT32_MAX) {
        nx_free(ctx, vb, verb_count ? verb_count : 1, 4);
        nx_free(ctx, pt, (size_t)point_count * 8, 16);
        return NEOX_ERROR_OUT_OF_MEMORY;
    }
    s = &((nx_path_slot *)dev->paths.slots)[idx];
    s->verbs = vb; s->points = pt;
    s->verb_count = verb_count; s->point_count = point_count;
    s->fill_rule = rule;
    *out = (neox_path){ nx_make_handle(dev->id, NX_TYPE_PATH, idx, s->gen) };
    return NEOX_OK;
}

neox_result neox_path_create(neox_device *dev, const neox_path_desc *desc,
                             neox_path *out_path)
{
    neox_context *ctx;
    uint32_t i, need = 0;
    if (!dev) return NEOX_ERROR_INVALID_ARGUMENT;
    ctx = dev->ctx;
    if (!out_path) NX_ERR(ctx, "out_path is NULL", NEOX_ERROR_INVALID_ARGUMENT);
    *out_path = (neox_path){ 0 };
    if (!desc || (!desc->verbs && desc->verb_count) ||
        (!desc->points && desc->point_count))
        NX_ERR(ctx, "invalid path desc", NEOX_ERROR_INVALID_ARGUMENT);
    size_t point_bytes;
    if (!nx_size_mul(desc->point_count, 2 * sizeof(float), &point_bytes))
        NX_ERR(ctx, "path point byte size overflow", NEOX_ERROR_INVALID_ARGUMENT);
    if ((unsigned)desc->fill_rule > NEOX_FILL_EVENODD)
        NX_ERR(ctx, "invalid fill rule", NEOX_ERROR_INVALID_ARGUMENT);
    for (i = 0; i < desc->verb_count; i++) {
        neox_path_verb v = (neox_path_verb)desc->verbs[i];
        if (v < NEOX_VERB_MOVE || v > NEOX_VERB_CLOSE)
            NX_ERR(ctx, "unknown path verb", NEOX_ERROR_INVALID_ARGUMENT);
        if (verb_points(v) > UINT32_MAX - need) return NEOX_ERROR_INVALID_ARGUMENT;
        need += verb_points(v);
    }
    if (need != desc->point_count)
        NX_ERR(ctx, "point_count does not match verbs", NEOX_ERROR_INVALID_ARGUMENT);
    for (size_t j = 0; j < point_bytes / sizeof(float); j++)
        if (nx_float_bad(desc->points[j]))
            NX_ERR(ctx, "path contains NaN/Inf", NEOX_ERROR_INVALID_ARGUMENT);
    return path_from_copy(dev, desc->verbs, desc->verb_count,
                               desc->points, desc->point_count,
                               desc->fill_rule, out_path);
}

void neox_path_release(neox_device *dev, neox_path path)
{
    nx_path_slot *s;
    uint32_t idx;
    if (!dev) return;
    if (nx_resolve(dev, path.id, NX_TYPE_PATH, &idx) != NEOX_OK) return;
    s = &((nx_path_slot *)dev->paths.slots)[idx];
    s->in_use = 0;
    s->deferred = 1;
    s->defer_next = dev->defer_head;
    dev->defer_head = (NX_TYPE_PATH << 24) | idx;
}
