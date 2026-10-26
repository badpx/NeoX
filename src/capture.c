/* Versioned capture format: snapshots, serialization, validation and atomic replay. */
#include "internal.h"

/* ------------------------------------------------------------------ CAP 捕获/回放（最小版，第一阶段仅本地）
 *
 * 格式（自包含，含版本头与资源映射表；不承诺跨版本兼容）：
 *   header  : magic[4]={'N','X','C','A'} | version u32 (=4) | res_count u32 | cmd_count u32
 *   resource: type u32 (1=texture,2=path) | cap_id u32
 *             texture: w u32 | h u32 | format u32 | storage u32 (0=bytes,1=legacy floats) | px_bytes u32 | pixels
 *             path   : verb_count u32 | point_count u32 | fill_rule u32 | verbs | points
 *   command : op u32
 *             PUSH: ctm 6×f32 | opacity f32 | blend u32 | flags u32 | clip_cap_id u32 (0xFFFFFFFF=无) | owns u32
 *             POP : -
 *             PATH: res cap_id u32 | color 4×f32
 *             TEX : res cap_id u32 | src 4×f32 | dst 4×f32 | filter u32 | opacity f32
 *             GLYPHS: res cap_id u32 | color 4×f32 | count u32 | glyphs (x,y f32; sx,sy,sw,sh u16)
 * 多字节整数为原生小端（第一阶段目标平台约定；CAP 稳定格式属 Phase 2）。
 * CAP 内 ID → 本地句柄的映射在 replay 时经会话内映射表重建（neox.h 契约）。 */
#define NX_CAP_VERSION 4u
#define NX_CAP_NO_RES  0xFFFFFFFFu

typedef struct nx_cap_res {
    uint64_t orig_id;       /* 录制侧原始句柄（映射表键） */
    uint32_t cap_id;        /* CAP 内 ID（序列化值，从 1 起） */
    uint32_t type;          /* NX_TYPE_TEXTURE / NX_TYPE_PATH */
    /* 纹理快照 */
    uint32_t w, h, format;
    nx_tex_data *pixels;
    /* 路径快照 */
    uint8_t *verbs;
    float   *points;
    uint32_t verb_count, point_count, fill_rule;
} nx_cap_res;

struct neox_capture {
    nx_device *dev;
    nx_cap_res *res;
    uint32_t res_count, res_cap;
    nx_cmd   *cmds;
    uint32_t  cmd_count, cmd_cap;
};

static void cap_free_res(neox_context *ctx, nx_cap_res *r)
{
    nx_tex_data_release(ctx,r->pixels);
    if (r->verbs)  nx_free(ctx, r->verbs, r->verb_count, 4);
    if (r->points) nx_free(ctx, r->points, (size_t)r->point_count * 8, 16);
    memset(r, 0, sizeof(*r));
}

/* 资源入表（纹理持有像素版本，路径首次引用时深拷贝）；返回 cap_id；id==0 返回 NX_CAP_NO_RES */
static uint32_t cap_resource(neox_capture *cap, nx_device *dev, uint32_t type,
                             uint64_t id, nx_tex_data *data)
{
    neox_context *ctx = dev->ctx;
    nx_cap_res *nr;
    uint32_t i;
    if (id == 0) return NX_CAP_NO_RES;
    for (i = 0; i < cap->res_count; i++) {
        if (cap->res[i].orig_id == id && cap->res[i].type == type &&
            (type!=NX_TYPE_TEXTURE || cap->res[i].pixels==data))
            return cap->res[i].cap_id;
    }
    /* IDs start at one; UINT32_MAX is the no-resource sentinel. */
    if (cap->res_count >= NX_CAP_NO_RES - 1) return NX_CAP_NO_RES;
    nr = (nx_cap_res *)nx_array_grow(ctx, cap->res, sizeof(nx_cap_res),
                                     &cap->res_cap, cap->res_count + 1);
    if (!nr) return NX_CAP_NO_RES;
    cap->res = nr;
    {
        nx_cap_res *e = &cap->res[cap->res_count];
        memset(e, 0, sizeof(*e));
        e->orig_id = id;
        e->cap_id = cap->res_count + 1;
        e->type = type;
        if (type == NX_TYPE_TEXTURE) {
            uint32_t idx;
            const nx_tex_slot *s;
            if (!data || data->refs == UINT32_MAX ||
                nx_resolve(dev,id,NX_TYPE_TEXTURE,&idx)!=NEOX_OK) return NX_CAP_NO_RES;
            s=&((nx_tex_slot *)dev->textures.slots)[idx];
            e->pixels=data;data->refs++;
            e->w=s->width;e->h=s->height;e->format=s->format;
        } else {
            uint32_t idx;
            const nx_path_slot *s;
            if (nx_resolve(dev, id, NX_TYPE_PATH, &idx) != NEOX_OK)
                return NX_CAP_NO_RES;
            s = &((nx_path_slot *)dev->paths.slots)[idx];
            e->verb_count=s->verb_count; e->point_count=s->point_count;
            e->verbs = (uint8_t *)nx_alloc(ctx, s->verb_count ? s->verb_count : 1, 4);
            e->points = (float *)nx_alloc(ctx,
                          s->point_count ? (size_t)s->point_count * 8 : 1, 16);
            if (!e->verbs || !e->points) {
                cap_free_res(ctx, e);
                return NX_CAP_NO_RES;
            }
            memcpy(e->verbs, s->verbs, s->verb_count);
            memcpy(e->points, s->points, (size_t)s->point_count * 8);
            e->verb_count = s->verb_count;
            e->point_count = s->point_count;
            e->fill_rule = (uint32_t)s->fill_rule;
        }
        cap->res_count++;
        return e->cap_id;
    }
}

/* 录制一条命令（深拷贝 glyphs；资源经映射表登记） */
neox_result nx_capture_record(neox_capture *cap, const nx_cmd *c)
{
    neox_device *dev=cap->dev; neox_context *ctx=dev->ctx;
    nx_cmd copy=*c; uint32_t id=0, type=0;
    if (cap->cmd_count == UINT32_MAX)
        NX_ERR(ctx, "capture command count exhausted", NEOX_ERROR_OUT_OF_MEMORY);
    nx_cmd *nc=(nx_cmd *)nx_array_grow(ctx,cap->cmds,sizeof(nx_cmd),&cap->cmd_cap,cap->cmd_count+1);
    if (!nc) return NEOX_ERROR_OUT_OF_MEMORY;
    cap->cmds=nc; copy.glyphs=NULL; copy.texture=NULL;
    if (c->op==NXC_GLYPHS && c->glyph_count) {
        copy.glyphs=(neox_glyph *)nx_alloc(ctx,(size_t)c->glyph_count*sizeof(neox_glyph),16);
        if (!copy.glyphs) return NEOX_ERROR_OUT_OF_MEMORY;
        memcpy(copy.glyphs,c->glyphs,(size_t)c->glyph_count*sizeof(neox_glyph));
    }
    if (c->op==NXC_PUSH && c->clip_idx>=0) {
        type=NX_TYPE_PATH; id=cap_resource(cap,dev,type,c->clip_id,NULL);
    } else if (c->op==NXC_PATH || c->op==NXC_TEX || c->op==NXC_GLYPHS) {
        type=c->op==NXC_PATH ? NX_TYPE_PATH : NX_TYPE_TEXTURE;
        id=cap_resource(cap,dev,type,c->res_id,c->texture);
    }
    if (type && id==NX_CAP_NO_RES) {
        if (copy.glyphs) nx_free(ctx,copy.glyphs,(size_t)c->glyph_count*sizeof(neox_glyph),16);
        ctx->last_error="capture resource snapshot failed";
        return NEOX_ERROR_OUT_OF_MEMORY;
    }
    if(c->op==NXC_PUSH) copy.clip_id=type ? id : NX_CAP_NO_RES;
    else if(type) copy.res_id=id;
    cap->cmds[cap->cmd_count++]=copy;
    return NEOX_OK;
}

static void cap_rollback(neox_capture *cap, uint32_t commands, uint32_t resources)
{
    neox_context *ctx = cap->dev->ctx;
    for (uint32_t i=commands;i<cap->cmd_count;i++)
        if (cap->cmds[i].op == NXC_GLYPHS && cap->cmds[i].glyphs)
            nx_free(ctx,cap->cmds[i].glyphs,(size_t)cap->cmds[i].glyph_count*sizeof(neox_glyph),16);
    for (uint32_t i=resources;i<cap->res_count;i++) cap_free_res(ctx,&cap->res[i]);
    cap->cmd_count=commands; cap->res_count=resources;
}

/* Only balanced frames are committed to a capture. Its resource table may keep
 * spare capacity after rollback, but none of the cancelled snapshots survive. */
void nx_capture_frame_begin(neox_frame *frame)
{
    neox_capture *cap=frame->dev->cap;
    if(!cap)return;
    frame->capture_owner=cap;
    frame->capture_cmd_start=cap->cmd_count;
    frame->capture_res_start=cap->res_count;
}
void nx_capture_frame_rollback(neox_frame *frame)
{
    if(frame->capture_owner && frame->dev->cap==frame->capture_owner)
        cap_rollback(frame->capture_owner,frame->capture_cmd_start,frame->capture_res_start);
    frame->capture_owner=NULL;
}

static void capture_destroy(neox_capture *cap)
{
    neox_context *ctx=cap->dev->ctx;
    if(cap->dev->active_frame)cap->dev->active_frame->capture_owner=NULL;
    cap_rollback(cap,0,0);
    if (cap->cmds) nx_free(ctx,cap->cmds,(size_t)cap->cmd_cap*sizeof(nx_cmd),16);
    if (cap->res) nx_free(ctx,cap->res,(size_t)cap->res_cap*sizeof(nx_cap_res),16);
    cap->dev->cap=NULL;
    nx_free(ctx,cap,sizeof(*cap),16);
}

void neox_capture_cancel(neox_capture *cap) { if (cap) capture_destroy(cap); }

neox_result nx_capture_record_batch(neox_capture *cap,const nx_cmd *cmds,uint32_t count)
{
    uint32_t nc=cap->cmd_count,nr=cap->res_count;
    if (count > UINT32_MAX - nc)
        NX_ERR(cap->dev->ctx, "capture command count exhausted", NEOX_ERROR_OUT_OF_MEMORY);
    for (uint32_t i=0;i<count;i++) {
        neox_result result=nx_capture_record(cap,&cmds[i]);
        if (result!=NEOX_OK) { cap_rollback(cap,nc,nr); return result; }
    }
    return NEOX_OK;
}

neox_result neox_capture_begin(neox_device *dev, neox_capture **out_cap)
{
    neox_context *ctx;
    neox_capture *cap;
    if (!dev || !out_cap) return NEOX_ERROR_INVALID_ARGUMENT;
    ctx = dev->ctx;
    *out_cap = NULL;
    if (dev->cap || dev->active_frame)
        NX_ERR(ctx, "capture must begin between frames with no active capture", NEOX_ERROR_INVALID_ARGUMENT);
    cap = (neox_capture *)nx_alloc(ctx, sizeof(neox_capture), 16);
    if (!cap) NX_ERR(ctx, "capture allocation failed", NEOX_ERROR_OUT_OF_MEMORY);
    memset(cap, 0, sizeof(*cap));
    cap->dev = dev;
    dev->cap = cap;
    *out_cap = cap;
    return NEOX_OK;
}

/* 序列化小工具 */
static neox_result cap_put_u32(neox_context *ctx, nx_buf *b, uint32_t v)
{
    return nx_buf_put(ctx, b, &v, 4);
}
static neox_result cap_put_f32(neox_context *ctx, nx_buf *b, float v)
{
    return nx_buf_put(ctx, b, &v, 4);
}
static neox_result cap_put_u16(neox_context *ctx, nx_buf *b, uint16_t v)
{
    return nx_buf_put(ctx, b, &v, 2);
}

neox_result neox_capture_end(neox_capture *cap, uint8_t **out_data, size_t *out_size)
{
    neox_context *ctx;
    nx_buf b;
    uint32_t i, j;
    uint8_t *exact;
    static const char magic[4] = { 'N', 'X', 'C', 'A' };
    if (!cap || !out_data || !out_size)
        return NEOX_ERROR_INVALID_ARGUMENT;
    ctx = cap->dev->ctx;
    *out_data = NULL;
    *out_size = 0;
    if(cap->dev->active_frame)NX_ERR(ctx,"capture must end between frames",NEOX_ERROR_INVALID_ARGUMENT);
    memset(&b, 0, sizeof(b));
    if (nx_buf_put(ctx, &b, magic, 4) != NEOX_OK ||
        cap_put_u32(ctx, &b, NX_CAP_VERSION) != NEOX_OK ||
        cap_put_u32(ctx, &b, cap->res_count) != NEOX_OK ||
        cap_put_u32(ctx, &b, cap->cmd_count) != NEOX_OK)
        goto oom;
    for (i = 0; i < cap->res_count; i++) {
        const nx_cap_res *e = &cap->res[i];
        if (cap_put_u32(ctx, &b, e->type) != NEOX_OK ||
            cap_put_u32(ctx, &b, e->cap_id) != NEOX_OK)
            goto oom;
        if (e->type == NX_TYPE_TEXTURE) {
            if(e->pixels->bytes>UINT32_MAX) {
                if(b.p)nx_free(ctx,b.p,b.cap,16);
                ctx->last_error="CAP texture exceeds format byte limit";
                return NEOX_ERROR_UNSUPPORTED;
            }
            uint32_t nbytes=(uint32_t)e->pixels->bytes;
            if (cap_put_u32(ctx, &b, e->w) != NEOX_OK ||
                cap_put_u32(ctx, &b, e->h) != NEOX_OK ||
                cap_put_u32(ctx, &b, e->format) != NEOX_OK ||
                cap_put_u32(ctx, &b, e->pixels->linear) != NEOX_OK ||
                cap_put_u32(ctx, &b, nbytes) != NEOX_OK ||
                nx_buf_put(ctx, &b, e->pixels->pixels, nbytes) != NEOX_OK)
                goto oom;
        } else {
            if (cap_put_u32(ctx, &b, e->verb_count) != NEOX_OK ||
                cap_put_u32(ctx, &b, e->point_count) != NEOX_OK ||
                cap_put_u32(ctx, &b, e->fill_rule) != NEOX_OK ||
                nx_buf_put(ctx, &b, e->verbs, e->verb_count) != NEOX_OK ||
                nx_buf_put(ctx, &b, e->points, (size_t)e->point_count * 8) != NEOX_OK)
                goto oom;
        }
    }
    for (i = 0; i < cap->cmd_count; i++) {
        const nx_cmd *c = &cap->cmds[i];
        const float *m = &c->ctm.a;
        if (cap_put_u32(ctx, &b, (uint32_t)c->op) != NEOX_OK)
            goto oom;
        switch (c->op) {
        case NXC_PUSH: {
            uint32_t clip_cap = (uint32_t)c->clip_id;
            for (j = 0; j < 6; j++)
                if (cap_put_f32(ctx, &b, m[j]) != NEOX_OK) goto oom;
            if (cap_put_f32(ctx, &b, c->opacity) != NEOX_OK ||
                cap_put_u32(ctx, &b, (uint32_t)c->blend) != NEOX_OK ||
                cap_put_u32(ctx, &b, c->flags) != NEOX_OK ||
                cap_put_u32(ctx, &b, clip_cap) != NEOX_OK ||
                cap_put_u32(ctx, &b, (uint32_t)c->owns_layer) != NEOX_OK)
                goto oom;
            break;
        }
        case NXC_POP:
            break;
        case NXC_PATH: {
            uint32_t rc = (uint32_t)c->res_id;
            if (cap_put_u32(ctx, &b, rc) != NEOX_OK ||
                cap_put_f32(ctx, &b, c->color.r) != NEOX_OK ||
                cap_put_f32(ctx, &b, c->color.g) != NEOX_OK ||
                cap_put_f32(ctx, &b, c->color.b) != NEOX_OK ||
                cap_put_f32(ctx, &b, c->color.a) != NEOX_OK)
                goto oom;
            break;
        }
        case NXC_TEX: {
            uint32_t rc = (uint32_t)c->res_id;
            const float *f = &c->src.x;
            if (cap_put_u32(ctx, &b, rc) != NEOX_OK) goto oom;
            for (j = 0; j < 8; j++)
                if (cap_put_f32(ctx, &b, f[j]) != NEOX_OK) goto oom;
            if (cap_put_u32(ctx, &b, (uint32_t)c->filter) != NEOX_OK ||
                cap_put_f32(ctx, &b, c->tex_opacity) != NEOX_OK)
                goto oom;
            break;
        }
        case NXC_GLYPHS: {
            uint32_t rc = (uint32_t)c->res_id;
            if (cap_put_u32(ctx, &b, rc) != NEOX_OK ||
                cap_put_f32(ctx, &b, c->color.r) != NEOX_OK ||
                cap_put_f32(ctx, &b, c->color.g) != NEOX_OK ||
                cap_put_f32(ctx, &b, c->color.b) != NEOX_OK ||
                cap_put_f32(ctx, &b, c->color.a) != NEOX_OK ||
                cap_put_u32(ctx, &b, c->glyph_count) != NEOX_OK ||
                cap_put_u32(ctx, &b, (uint32_t)c->atlas_mode) != NEOX_OK ||
                cap_put_f32(ctx, &b, c->dist_range) != NEOX_OK ||
                cap_put_f32(ctx, &b, c->softness) != NEOX_OK)
                goto oom;
            for (j = 0; j < c->glyph_count; j++) {
                const neox_glyph *g = &c->glyphs[j];
                if (cap_put_f32(ctx, &b, g->x) != NEOX_OK ||
                    cap_put_f32(ctx, &b, g->y) != NEOX_OK ||
                    cap_put_u16(ctx, &b, g->sx) != NEOX_OK ||
                    cap_put_u16(ctx, &b, g->sy) != NEOX_OK ||
                    cap_put_u16(ctx, &b, g->sw) != NEOX_OK ||
                    cap_put_u16(ctx, &b, g->sh) != NEOX_OK)
                    goto oom;
            }
            break;
        }
        default:
            if (b.p) nx_free(ctx, b.p, b.cap, 16);
            ctx->last_error = "unknown command during capture serialize";
            return NEOX_ERROR_BACKEND;
        }
    }
    /* 精确大小缓冲移交（free 时可重现 size） */
    exact = (uint8_t *)nx_alloc(ctx, b.len ? b.len : 1, 16);
    if (!exact) goto oom;
    memcpy(exact, b.p, b.len);
    nx_free(ctx, b.p, b.cap, 16);
    capture_destroy(cap);
    *out_data = exact;
    *out_size = b.len;
    return NEOX_OK;
oom:
    if (b.p) nx_free(ctx, b.p, b.cap, 16);
    ctx->last_error = "capture buffer allocation failed";
    return NEOX_ERROR_OUT_OF_MEMORY;
}

void neox_capture_free_buffer(neox_context *ctx, uint8_t *data, size_t size)
{
    if (!ctx || !data) return;
    nx_free(ctx, data, size ? size : 1, 16);
}

/* 边界检查的 CAP 读取器 */
typedef struct nx_reader {
    const uint8_t *p;
    size_t len, pos;
    neox_context *ctx;
} nx_reader;

static int rd_bytes(nx_reader *r, void *out, size_t n)
{
    if (n > r->len - r->pos) {
        r->ctx->last_error = "CAP buffer truncated";
        return 0;
    }
    memcpy(out, r->p + r->pos, n);
    r->pos += n;
    return 1;
}
static int rd_u32(nx_reader *r, uint32_t *v) { return rd_bytes(r, v, 4); }
static int rd_f32(nx_reader *r, float *v) { return rd_bytes(r, v, 4) && isfinite(*v); }
static int rd_u16(nx_reader *r, uint16_t *v) { return rd_bytes(r, v, 2); }

typedef struct nx_replay_resource {
    uint32_t id, type;
    uint64_t handle;
} nx_replay_resource;

static uint64_t replay_handle(const nx_replay_resource *map, uint32_t count,
                              uint32_t id, uint32_t type)
{
    for (uint32_t i = 0; i < count; i++)
        if (map[i].id == id && map[i].type == type) return map[i].handle;
    return 0;
}

neox_result neox_capture_replay(neox_frame *frame, const uint8_t *data, size_t size)
{
    nx_reader rd;
    neox_device *dev;
    neox_context *ctx;
    neox_frame staged;
    neox_capture *capture;
    nx_replay_resource *map = NULL;
    uint32_t version, nr = 0, nc, made = 0, depth = 0;
    size_t map_bytes = 0;
    neox_result result = NEOX_ERROR_UNSUPPORTED;
    char magic[4];
    if (!frame || !data || size < 16) return NEOX_ERROR_INVALID_ARGUMENT;
    dev = frame->dev; ctx = dev->ctx;
    rd = (nx_reader){data, size, 0, ctx};
    if (!rd_bytes(&rd, magic, 4) || memcmp(magic, "NXCA", 4) ||
        !rd_u32(&rd, &version) || version < 1 || version > NX_CAP_VERSION ||
        !rd_u32(&rd, &nr) || !rd_u32(&rd, &nc) || nr > size / 8 || nc > size / 4)
        NX_ERR(ctx, "invalid CAP header", NEOX_ERROR_UNSUPPORTED);
    staged = *frame;
    staged.cmds = NULL; staged.cmd_count = staged.cmd_cap = staged.draw_calls = 0;
    capture = dev->cap; dev->cap = NULL;
    if (nr) {
        if (!nx_size_mul(nr, sizeof(*map), &map_bytes)) {
            result = NEOX_ERROR_OUT_OF_MEMORY; goto done;
        }
        map = (nx_replay_resource *)nx_alloc(ctx, map_bytes, 16);
        if (!map) { result = NEOX_ERROR_OUT_OF_MEMORY; goto done; }
    }
#define READ(expr) do { if (!(expr)) goto invalid; } while (0)
#define CALL(expr) do { result = (expr); if (result != NEOX_OK) goto done; } while (0)
    for (uint32_t i = 0; i < nr; i++) {
        uint32_t type, id;
        READ(rd_u32(&rd, &type) && rd_u32(&rd, &id));
        READ(id && id != NX_CAP_NO_RES);
        for (uint32_t j = 0; j < made; j++) READ(map[j].id != id);
        map[i].id = id; map[i].type = type;
        if (type == NX_TYPE_TEXTURE) {
            uint32_t w,h,fmt,bytes,linear=1;
            neox_texture tex;
            READ(rd_u32(&rd,&w) && rd_u32(&rd,&h) && rd_u32(&rd,&fmt));
            if(version>=4) READ(rd_u32(&rd,&linear));
            READ(rd_u32(&rd,&bytes));
            READ(w && h && w<=NX_MAX_TEXTURE_SIZE && h<=NX_MAX_TEXTURE_SIZE &&
                 fmt<=NEOX_FORMAT_RGBA8_DATA && linear<=1);
            uint32_t bpp=linear ? 16u : (fmt==NEOX_FORMAT_A8 ? 1u : 4u);
            READ((uint64_t)w*h*bpp==bytes && bytes<=rd.len-rd.pos);
            if(linear) {
                float *px=nx_alloc(ctx,bytes,16);
                if(!px){result=NEOX_ERROR_OUT_OF_MEMORY;goto done;}
                (void)rd_bytes(&rd,px,bytes);
                int valid=1;
                for(size_t j=0;j<bytes/sizeof(float);j++)
                    if(!isfinite(px[j]) || px[j]<0 || px[j]>1){valid=0;break;}
                result=valid ? nx_texture_from_linear(dev,w,h,(neox_pixel_format)fmt,px,&tex)
                             : NEOX_ERROR_UNSUPPORTED;
                nx_free(ctx,px,bytes,16);
            } else {
                neox_texture_desc desc={w,h,(neox_pixel_format)fmt,rd.p+rd.pos,(size_t)w*bpp};
                result=neox_texture_create(dev,&desc,&tex);rd.pos+=bytes;
            }
            if(result!=NEOX_OK)goto done;
            map[i].handle = tex.id;
        } else if (type == NX_TYPE_PATH) {
            uint32_t vc, pc, rule;
            float *points;
            neox_path path;
            READ(rd_u32(&rd,&vc) && rd_u32(&rd,&pc) && rd_u32(&rd,&rule));
            READ(vc <= rd.len-rd.pos && (uint64_t)pc*8 <= rd.len-rd.pos-vc);
            const uint8_t *verbs = rd.p + rd.pos; rd.pos += vc;
            size_t bytes = (size_t)pc * 8;
            points = (float *)nx_alloc(ctx, bytes, 16);
            if (!points) { result = NEOX_ERROR_OUT_OF_MEMORY; goto done; }
            (void)rd_bytes(&rd,points,bytes);
            neox_path_desc desc = {verbs,points,vc,pc,(neox_fill_rule)rule};
            result = neox_path_create(dev,&desc,&path);
            nx_free(ctx,points,bytes,16);
            if (result != NEOX_OK) goto done;
            map[i].handle = path.id;
        } else goto invalid;
        made++;
    }
    for (uint32_t i=0;i<nc;i++) {
        uint32_t op, id;
        uint64_t handle;
        READ(rd_u32(&rd,&op));
        switch (op) {
        case NXC_PUSH: {
            neox_affine m;
            float opacity;
            uint32_t blend, flags, clip, owns;
            for (unsigned j=0;j<6;j++) READ(rd_f32(&rd,(&m.a)+j));
            READ(rd_f32(&rd,&opacity) && rd_u32(&rd,&blend) && rd_u32(&rd,&flags) &&
                 rd_u32(&rd,&clip) && rd_u32(&rd,&owns));
            READ(opacity >= 0 && opacity <= 1 && blend <= NEOX_BLEND_PLUS &&
                 !(flags & ~NEOX_ALLOW_APPROXIMATE));
            handle = clip == NX_CAP_NO_RES ? 0 : replay_handle(map,made,clip,NX_TYPE_PATH);
            READ(clip == NX_CAP_NO_RES || handle);
            m = nx_affine_mul(frame->ctm_stack[frame->ctm_count-1],m);
            CALL(nx_emit_push(&staged,m,opacity,(neox_blend)blend,flags,
                          handle ? (int32_t)nx_handle_index(handle) : -1,handle));
            depth++;
            break;
        }
        case NXC_POP:
            READ(depth > 0); CALL(nx_emit_pop(&staged)); depth--; break;
        case NXC_PATH: {
            neox_paint paint;
            READ(rd_u32(&rd,&id));
            for (unsigned j=0;j<4;j++) READ(rd_f32(&rd,(&paint.color.r)+j));
            handle = replay_handle(map,made,id,NX_TYPE_PATH); READ(handle);
            CALL(neox_draw_path(&staged,(neox_path){handle},&paint)); break;
        }
        case NXC_TEX: {
            neox_rect src,dst;
            float opacity; uint32_t filter;
            READ(rd_u32(&rd,&id));
            for (unsigned j=0;j<4;j++) READ(rd_f32(&rd,(&src.x)+j));
            for (unsigned j=0;j<4;j++) READ(rd_f32(&rd,(&dst.x)+j));
            READ(rd_u32(&rd,&filter) && rd_f32(&rd,&opacity));
            handle = replay_handle(map,made,id,NX_TYPE_TEXTURE); READ(handle);
            CALL(neox_draw_texture(&staged,(neox_texture){handle},src,dst,(neox_filter)filter,opacity)); break;
        }
        case NXC_GLYPHS: {
            neox_paint paint; neox_glyph_style style={0}; neox_glyph *glyphs;
            uint32_t count,mode=0;
            READ(rd_u32(&rd,&id));
            for (unsigned j=0;j<4;j++) READ(rd_f32(&rd,(&paint.color.r)+j));
            READ(rd_u32(&rd,&count));
            if (version >= 2) {
                READ(rd_u32(&rd,&mode) && rd_f32(&rd,&style.dist_range_px) && rd_f32(&rd,&style.softness));
                style.atlas_mode=(neox_atlas_mode)mode;
            }
            READ(mode <= NEOX_ATLAS_MSDF && count <= (rd.len-rd.pos)/16);
            handle = replay_handle(map,made,id,NX_TYPE_TEXTURE); READ(handle);
            glyphs = (neox_glyph *)nx_alloc(ctx,(size_t)count*sizeof(*glyphs),16);
            if (!glyphs) { result=NEOX_ERROR_OUT_OF_MEMORY; goto done; }
            int valid=1;
            for (uint32_t j=0;j<count;j++) {
                neox_glyph *g=&glyphs[j];
                if (!(rd_f32(&rd,&g->x) && rd_f32(&rd,&g->y) && rd_u16(&rd,&g->sx) &&
                      rd_u16(&rd,&g->sy) && rd_u16(&rd,&g->sw) && rd_u16(&rd,&g->sh))) {
                    valid=0; break;
                }
            }
            result=valid ? neox_draw_glyphs_ex(&staged,(neox_texture){handle},glyphs,count,&paint,&style)
                         : NEOX_ERROR_UNSUPPORTED;
            nx_free(ctx,glyphs,(size_t)count*sizeof(*glyphs),16);
            if (result!=NEOX_OK) goto done;
            break;
        }
        default: goto invalid;
        }
    }
    READ(depth == 0 && rd.pos == rd.len);
    result=NEOX_OK;
    goto done;
invalid:
    result=NEOX_ERROR_UNSUPPORTED;
    ctx->last_error="invalid CAP resource, command or state stack";
done:
    dev->cap=capture;
    result=nx_frame_finish_staged(frame,&staged,capture,result);
    /* Frame commands own pins. Replay never leaves anonymous public resources. */
    for (uint32_t i=0;i<made;i++) {
        if (map[i].type==NX_TYPE_TEXTURE) neox_texture_release(dev,(neox_texture){map[i].handle});
        else neox_path_release(dev,(neox_path){map[i].handle});
    }
    if (map) nx_free(ctx,map,map_bytes,16);
    nx_reap_resources(dev);
    return result;
#undef READ
#undef CALL
}
