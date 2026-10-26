/* Production Text implementation, with internal access only for deterministic
 * counter exhaustion and font-metric boundary fixtures. No display required. */
#include "internal.h"
#include "../text/text.c"
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);} }while(0)
#define OK(x) CHECK((x)==NEOX_OK)
typedef struct {size_t live,calls,fail;int failed;} tracker;
typedef union { _Alignas(16) char align[16]; struct {size_t size;} h; } allocation;
static void *allocate(void *user,size_t n,size_t align)
{
    tracker *t=user;CHECK(align<=16);t->calls++;
    if(t->fail && t->calls==t->fail){t->failed=1;return NULL;}
    allocation *a=malloc(sizeof(*a)+n);CHECK(a);a->h.size=n;t->live+=n;return a+1;
}
static void deallocate(void *user,void *ptr,size_t n,size_t align)
{
    tracker *t=user;(void)align;if(!ptr)return;allocation *a=(allocation*)ptr-1;
    CHECK(a->h.size==n);t->live-=n;free(a);
}
typedef struct {float advance,kern,bx,by;int w,h;neox_result result;unsigned freed;} mock;
static float advance(void *u,uint32_t g,float size)
{(void)g;(void)size;return ((mock*)u)->advance;}
static float kern(void *u,uint32_t a,uint32_t b,float size)
{(void)a;(void)b;(void)size;return ((mock*)u)->kern;}
static neox_result raster(void *u,uint32_t g,float size,uint8_t **bmp,int32_t *w,int32_t *h,float *bx,float *by)
{
    mock *m=u;(void)g;(void)size;*w=m->w;*h=m->h;*bx=m->bx;*by=m->by;
    *bmp=malloc(1);CHECK(*bmp);**bmp=255;return m->result;
}
static void free_bitmap(void *u,uint8_t *bmp){((mock*)u)->freed++;free(bmp);}
static nx_font *custom(nx_text_context *t,mock *m)
{
    nx_font_backend b={0};b.user=m;b.glyph_advance=advance;b.glyph_rasterize=raster;
    b.glyph_free_bitmap=free_bitmap;b.ascent=1;nx_font *f;OK(nx_font_create_custom(t,&b,&f));return f;
}
static void invalid_layout(nx_text_context *t,nx_font *f,float size,const char *s,float width,nx_text_align align)
{
    size_t used=t->core->used;nx_layout *lo=(nx_layout*)(uintptr_t)1;
    CHECK(nx_text_layout(t,f,size,s,width,align,&lo)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(!lo && t->core->used==used);
}
static void layout_numeric(nx_text_context *t)
{
    mock m={.advance=2,.w=1,.h=1};nx_font *f=custom(t,&m);nx_layout *lo;
    CHECK(nx_font_ascent(f,INFINITY)==0 && nx_text_last_error(t));
    CHECK(nx_font_descent(f,NAN)==0 && nx_font_line_gap(f,-1)==0);
    CHECK(nx_font_ascent(f,0)==0 && nx_font_ascent(f,2)==2);
    invalid_layout(t,f,INFINITY,"a",0,NX_ALIGN_LEFT);
    invalid_layout(t,f,NAN,"a",0,NX_ALIGN_LEFT);
    invalid_layout(t,f,1,"a",INFINITY,NX_ALIGN_LEFT);
    invalid_layout(t,f,1,"a",NAN,NX_ALIGN_LEFT);
    invalid_layout(t,f,1,"a",0,(nx_text_align)99);
    /* NULL kerning is a valid zero-kerning backend. */
    OK(nx_text_layout(t,f,1,"ab",0,NX_ALIGN_LEFT,&lo));
    CHECK(lo->glyph_count==2 && lo->glyphs[1].x==2 && lo->width==4);nx_layout_destroy(lo);
    m.advance=NAN;invalid_layout(t,f,1,"a",0,NX_ALIGN_LEFT);
    m.advance=INFINITY;invalid_layout(t,f,1,"a",0,NX_ALIGN_LEFT);
    m.advance=FLT_MAX*0.75f;invalid_layout(t,f,1,"aaa",0,NX_ALIGN_LEFT);
    f->be.glyph_kerning=kern;m.advance=2;m.kern=NAN;
    invalid_layout(t,f,1,"ab",0,NX_ALIGN_LEFT);m.kern=INFINITY;
    invalid_layout(t,f,1,"ab",0,NX_ALIGN_LEFT);
    /* Finite cancellation must not be rejected because a float intermediate
     * would overflow. Only the resulting positions and metrics need fit. */
    m.advance=FLT_MAX;m.kern=-FLT_MAX;
    OK(nx_text_layout(t,f,1,"ab",0,NX_ALIGN_LEFT,&lo));
    CHECK(lo->width==FLT_MAX && lo->glyphs[1].x==0);nx_layout_destroy(lo);
    m.advance=2;m.kern=0;f->be.ascent=FLT_MAX*0.75f;f->be.descent=-FLT_MAX*0.75f;
    invalid_layout(t,f,1,"a",0,NX_ALIGN_LEFT);
    f->be.descent=0;invalid_layout(t,f,1,"a\na\na",0,NX_ALIGN_LEFT);
    f->be.ascent=1;m.advance=-FLT_MAX;
    invalid_layout(t,f,1,"a",FLT_MAX,NX_ALIGN_RIGHT);
    m.advance=2;
    const char *truncated[]={"\xe2","\xe2\x82","\xf0","\xf0\x90","\xf0\x90\x80"};
    for(unsigned i=0;i<5;i++) {
        OK(nx_text_layout(t,f,1,truncated[i],0,NX_ALIGN_LEFT,&lo));
        CHECK(lo->glyph_count==strlen(truncated[i]));
        for(unsigned j=0;j<lo->glyph_count;j++)CHECK(lo->glyphs[j].glyph_index==0xfffd);
        nx_layout_destroy(lo);
    }
    nx_layout fake={0};fake.line_count=3;fake.line_height=FLT_MAX;
    float box[4]={1,2,3,4},before[4];memcpy(before,box,sizeof(box));
    CHECK(nx_text_measure(&fake,box)==NEOX_ERROR_INVALID_ARGUMENT && !memcmp(box,before,sizeof(box)));
    fake.tctx=t;fake.line_count=UINT32_MAX;
    CHECK(layout_finish_line(&fake,0,0)==NEOX_ERROR_OUT_OF_MEMORY && !fake.line);
    nx_font_backend bad=f->be;bad.ascent=NAN;nx_font *rejected=(nx_font*)(uintptr_t)1;
    CHECK(nx_font_create_custom(t,&bad,&rejected)==NEOX_ERROR_INVALID_ARGUMENT && !rejected);
    unsigned char header[12]={0};nx_font *invalid=NULL;size_t used=t->core->used;
    for(size_t size=0;size<12;size++)
        CHECK(nx_font_load_memory(t,header,size,0,&invalid)==NEOX_ERROR_INVALID_ARGUMENT && !invalid);
    CHECK(nx_font_load_memory(t,header,12,-1,&invalid)==NEOX_ERROR_INVALID_ARGUMENT && !invalid);
    CHECK(nx_font_load_memory(t,header,(size_t)INT32_MAX+1,0,&invalid)==NEOX_ERROR_UNSUPPORTED && !invalid);
    CHECK(t->core->used==used);nx_font_destroy(f);
}
static void layout_oom(nx_text_context *t,tracker *tr)
{
    mock m={.advance=2,.w=1,.h=1};nx_font *f=custom(t,&m);f->be.glyph_kerning=kern;
    char text[1024];size_t len=0;
    for(unsigned i=0;i<40;i++){memcpy(text+len,"abcdefghijklmnopqrst\n",21);len+=21;}text[len]=0;
    size_t used=t->core->used,live=tr->live;
    for(unsigned fail=1;fail<100;fail++) {
        tr->calls=0;tr->failed=0;tr->fail=fail;nx_layout *lo=NULL;
        neox_result r=nx_text_layout(t,f,12,text,0,NX_ALIGN_LEFT,&lo);tr->fail=0;
        if(tr->failed) {
            if(r!=NEOX_ERROR_OUT_OF_MEMORY)fprintf(stderr,"allocation %u returned %d, output %p\n",fail,r,(void*)lo);
            CHECK(r==NEOX_ERROR_OUT_OF_MEMORY && !lo);
        } else {CHECK(r==NEOX_OK && lo && lo->line_count==41);nx_layout_destroy(lo);}
        CHECK(t->core->used==used && tr->live==live);
        if(!tr->failed){printf("layout: %u allocation failure points\n",fail-1);break;}
        CHECK(fail<99);
    }
    nx_font_destroy(f);
}
static void atlas_numeric(nx_text_context *t,neox_device *d)
{
    mock m={.advance=2,.w=1,.h=1};nx_font *f=custom(t,&m);nx_atlas *a;
    OK(nx_atlas_create(t,d,8,8,&a));nx_atlas_rect rc={9,9,9,9};size_t used=t->core->used;
    for(unsigned i=0;i<4;i++) {
        m.bx=i==0?NAN:0;m.by=i==1?INFINITY:0;m.w=i==2?-1:1;
        m.result=i==3?NEOX_ERROR_BACKEND:NEOX_OK;
        CHECK(nx_atlas_glyph(a,f,7,12,&rc)==(i==3?NEOX_ERROR_BACKEND:NEOX_ERROR_INVALID_ARGUMENT));
        CHECK(m.freed==i+1 && !a->nshelves && !a->dirty && !a->hits && !a->misses);
        CHECK(rc.x==9 && t->core->used==used);
    }
    m.bx=FLT_MAX;m.by=0;m.w=1;m.result=NEOX_OK;
    OK(nx_atlas_glyph(a,f,7,12,&rc));CHECK(a->misses==1);
    nx_layout *lo;OK(nx_text_layout(t,f,12,"\x07",0,NX_ALIGN_LEFT,&lo));lo->glyphs[0].x=FLT_MAX;
    uint8_t pixels[16]={0};neox_target_desc td={0};td.soft.pixels=pixels;td.soft.width=td.soft.height=2;td.soft.stride=8;
    neox_frame *frame;OK(neox_frame_begin(d,&td,&frame));neox_paint paint={{1,1,1,1}};unsigned n=frame->cmd_count;
    CHECK(nx_text_draw(frame,a,lo,&paint)==NEOX_ERROR_INVALID_ARGUMENT && frame->cmd_count==n);
    nx_layout_destroy(lo);neox_frame_cancel(frame);
    int count=INT32_MAX,cap=INT32_MAX;nx_msdf_seg *segments=NULL;t->stb_failed=0;
    CHECK(!nx_msdf_push_seg(t,&segments,&count,&cap,0,0,1,1,0));CHECK(t->stb_failed && !segments);
    nx_atlas_destroy(a);nx_font_destroy(f);neox_device_pump(d);
}
static int real_font(nx_text_context *t,neox_device *d,tracker *tr)
{
    const char *paths[]={getenv("NEOX_TEST_FONT"),"/System/Library/Fonts/Supplemental/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};nx_font *f=NULL;
    for(unsigned i=0;i<3;i++)if(paths[i] && nx_font_load_file(t,paths[i],0,&f)==NEOX_OK)break;
    if(!f){puts("SKIP: no font fixture");return 77;}
    uint32_t glyph=font_glyph_index(f,'W');CHECK(glyph>0);
    /* An invalid vertical scale is rejected during load, not discovered while
     * rasterizing a later glyph. The copied font and failed object are freed. */
    unsigned char *vm=f->data+f->stb.hhea+4,saved_vm[4];memcpy(saved_vm,vm,4);
    memset(vm,0,4);size_t before=t->core->used;nx_font *bad=NULL;
    CHECK(nx_font_load_memory(t,f->data,f->data_size,0,&bad)==NEOX_ERROR_INVALID_ARGUMENT && !bad);
    CHECK(t->core->used==before);memcpy(vm,saved_vm,4);
    for(unsigned mode=0;mode<3;mode++) {
        nx_atlas *a;OK(nx_atlas_create_ex(t,d,64,64,(neox_atlas_mode)mode,4,&a));
        nx_atlas_rect rect={9,9,9,9};size_t used=t->core->used;tr->calls=0;
        CHECK(nx_atlas_glyph(a,f,UINT32_MAX,24,&rect)==NEOX_ERROR_INVALID_ARGUMENT);
        CHECK(nx_atlas_glyph(a,f,(uint32_t)f->stb.numGlyphs,24,&rect)==NEOX_ERROR_INVALID_ARGUMENT);
        CHECK(nx_atlas_glyph(a,f,glyph,16384,&rect)==NEOX_ERROR_OUT_OF_MEMORY);
        /* Tiny positive font height with otherwise valid outlines exposes
         * stb's int bitmap-area multiplication if rasterization runs first. */
        unsigned char *metrics=f->data+f->stb.hhea+4,saved[4];memcpy(saved,metrics,4);
        metrics[0]=0;metrics[1]=1;metrics[2]=metrics[3]=0;
        CHECK(nx_atlas_glyph(a,f,glyph,16384,&rect)==NEOX_ERROR_OUT_OF_MEMORY);
        memcpy(metrics,saved,4);
        CHECK(tr->calls==0 && t->core->used==used && !a->nshelves && !a->dirty && !a->misses && rect.x==9);
        OK(nx_atlas_glyph(a,f,glyph,24,&rect));CHECK(rect.w>0 && rect.h>0);nx_atlas_destroy(a);
    }
    nx_atlas *a;nx_atlas_rect rect;
    OK(nx_atlas_create_ex(t,d,64,64,NEOX_ATLAS_SDF,FLT_TRUE_MIN,&a));tr->calls=0;
    CHECK(nx_atlas_glyph(a,f,glyph,24,&rect)==NEOX_ERROR_UNSUPPORTED && tr->calls==0);nx_atlas_destroy(a);
    OK(nx_atlas_create_ex(t,d,64,64,NEOX_ATLAS_MSDF,FLT_TRUE_MIN,&a));
    OK(nx_atlas_glyph(a,f,glyph,24,&rect));CHECK(rect.w>0);nx_atlas_destroy(a);
    nx_font_destroy(f);neox_device_pump(d);return 0;
}
int main(int argc,char **argv)
{
    tracker tr={0};neox_allocator al={&tr,allocate,deallocate};neox_context_desc desc={0};
    desc.allocator=&al;desc.memory_budget=16*1024*1024;neox_context *ctx;neox_device *d;nx_text_context *t;
    OK(neox_context_create(&desc,&ctx));OK(neox_device_create(ctx,NULL,&d));OK(nx_text_context_create(ctx,NULL,&t));
    int result=0;
    if(argc>1 && !strcmp(argv[1],"font"))result=real_font(t,d,&tr);
    else if(argc>1 && !strcmp(argv[1],"oom"))layout_oom(t,&tr);
    else {layout_numeric(t);layout_oom(t,&tr);atlas_numeric(t,d);}
    nx_text_context_destroy(t);neox_device_destroy(d);CHECK(ctx->used==0);neox_context_destroy(ctx);CHECK(!tr.live);
    if(!result)puts("ALL PASS (text finite metrics, allocation rollback, raster bounds)");
    return result;
}
