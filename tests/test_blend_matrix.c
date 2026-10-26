/* Independent Porter-Duff oracle, including fractional geometric coverage.
 * No renderer or internal blending helper supplies expected pixels. */
#include "internal.h"
#include "neox_gl.h"
#include "neox_mtl.h"
#include <stdio.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);} }while(0)
#define OK(x) CHECK((x)==NEOX_OK)
enum { W=6, H=4 };
static unsigned renders;
typedef struct allocation { void *base; size_t size,align; } allocation;
static size_t live;
static void *allocate(void *user,size_t size,size_t align)
{
    (void)user;
    if(align<sizeof(void*))align=sizeof(void*);
    void *base=malloc(size+align+sizeof(allocation));if(!base)return NULL;
    uintptr_t address=((uintptr_t)base+sizeof(allocation)+align-1)&~(uintptr_t)(align-1);
    allocation *a=(allocation*)address-1;*a=(allocation){base,size,align};live++;
    return (void*)address;
}
static void deallocate(void *user,void *ptr,size_t size,size_t align)
{
    (void)user;if(align<sizeof(void*))align=sizeof(void*);
    allocation *a=(allocation*)ptr-1;CHECK(a->size==size && a->align==align);
    live--;free(a->base);
}
static double encode(double x) { return x<=0.0031308 ? 12.92*x : 1.055*pow(x,1.0/2.4)-0.055; }
static void expected(neox_blend mode, double sa, double da, double coverage, uint8_t out[4])
{
    double fs=1,fd=0,s[4]={sa,0,0,sa},d[4]={0,0,da,da},p[4];
    switch(mode){
    case NEOX_BLEND_SRC_OVER:fd=1-sa;break;
    case NEOX_BLEND_CLEAR:fs=0;break;
    case NEOX_BLEND_SRC:break;
    case NEOX_BLEND_DST:fs=0;fd=1;break;
    case NEOX_BLEND_DST_OVER:fs=1-da;fd=1;break;
    case NEOX_BLEND_SRC_IN:fs=da;break;
    case NEOX_BLEND_DST_IN:fs=0;fd=sa;break;
    case NEOX_BLEND_SRC_OUT:fs=1-da;break;
    case NEOX_BLEND_DST_OUT:fs=0;fd=1-sa;break;
    case NEOX_BLEND_SRC_ATOP:fs=da;fd=1-sa;break;
    case NEOX_BLEND_DST_ATOP:fs=1-da;fd=sa;break;
    case NEOX_BLEND_XOR:fs=1-da;fd=1-sa;break;
    case NEOX_BLEND_PLUS:fd=1;break;
    }
    for(int c=0;c<4;c++)p[c]=coverage*fmin(1,s[c]*fs+d[c]*fd)+(1-coverage)*d[c];
    for(int c=0;c<3;c++)out[c]=(uint8_t)lrint(255*encode(p[3]>0?p[c]/p[3]:0));
    out[3]=(uint8_t)lrint(255*p[3]);
}
int main(int argc,char **argv)
{
    int kind=argc>1&&!strcmp(argv[1],"gl")?1:argc>1&&!strcmp(argv[1],"metal")?2:0;
    neox_context *ctx;neox_device *dev;neox_device_desc dd={0};
    neox_allocator allocator={NULL,allocate,deallocate};neox_context_desc cd={0};cd.allocator=&allocator;
    OK(neox_context_create(&cd,&ctx));
#ifdef NEOX_ENABLE_GL
    neox_gl_device_desc gd={0};gd.struct_size=sizeof(gd);gd.magic=NEOX_GL_DESC_MAGIC;
    if(kind==1){dd.backend=NEOX_BACKEND_GPU;dd.platform=&gd;dd.platform_size=sizeof(gd);}
#endif
#ifdef NEOX_ENABLE_METAL
    neox_mtl_device_desc md={0};md.struct_size=sizeof(md);md.magic=NEOX_MTL_DESC_MAGIC;
    if(kind==2){dd.backend=NEOX_BACKEND_GPU;dd.platform=&md;dd.platform_size=sizeof(md);}
#endif
    if(neox_device_create(ctx,&dd,&dev)!=NEOX_OK){
        const char *req=getenv(kind==1?"NEOX_REQUIRE_GL":"NEOX_REQUIRE_METAL");
        CHECK(kind && (!req||!*req));neox_context_destroy(ctx);return 77;
    }
    uint8_t pixels[W*H*4];neox_target_desc target={0};target.kind=dd.backend;
    if(!kind){target.soft.pixels=pixels;target.soft.width=W;target.soft.height=H;target.soft.stride=W*4;}
#ifdef NEOX_ENABLE_GL
    if(kind==1){neox_gl_surface *s;OK(neox_gl_surface_create(dev,W,H,&s));target.surface=s;}
#endif
#ifdef NEOX_ENABLE_METAL
    if(kind==2){neox_mtl_surface *s;OK(neox_mtl_surface_create(dev,W,H,&s));target.surface=s;}
#endif
    const uint8_t verbs[]={NEOX_VERB_MOVE,NEOX_VERB_LINE,NEOX_VERB_LINE,NEOX_VERB_LINE,NEOX_VERB_CLOSE};
    const float points[]={1.5f,1,3.5f,1,3.5f,3,1.5f,3};
    neox_path_desc pd={verbs,points,5,4,NEOX_FILL_NONZERO};neox_path path;
    OK(neox_path_create(dev,&pd,&path));
    const uint8_t alphas[]={0,128,255};
    for(int type=0;type<3;type++)for(unsigned ai=0;ai<3;ai++)for(unsigned ci=0;ci<3;ci++){
        uint8_t rgba[16],mask[4];neox_texture tex,atlas;
        for(int i=0;i<4;i++){rgba[i*4]=255;rgba[i*4+1]=rgba[i*4+2]=0;rgba[i*4+3]=alphas[ai];mask[i]=alphas[ci];}
        neox_texture_desc td={2,2,NEOX_FORMAT_RGBA8_LINEAR,rgba,8};OK(neox_texture_create(dev,&td,&tex));
        td=(neox_texture_desc){2,2,NEOX_FORMAT_A8,mask,2};OK(neox_texture_create(dev,&td,&atlas));
        for(int transform=0;transform<(type==0?1:2);transform++)for(int mode=NEOX_BLEND_SRC_OVER;mode<=NEOX_BLEND_PLUS;mode++){
            neox_color clear={0,0,1,0.6f};target.clear=&clear;neox_frame *fr;
            OK(neox_frame_begin(dev,&target,&fr));
            neox_state state={0};state.transform=nx_affine_identity();state.opacity=1;state.blend=(neox_blend)mode;
            if(transform){state.transform.c=0.25f;state.transform.tx=0.1f;}
            OK(neox_push_state(fr,&state));
            float opacity=0.75f;neox_paint paint={{1,0,0,(alphas[ai]/255.0f)*opacity}};
            if(type==0)OK(neox_draw_path(fr,path,&paint));
            else if(type==1)OK(neox_draw_texture(fr,tex,(neox_rect){0,0,2,2},(neox_rect){1,1,2,2},ci%2?NEOX_FILTER_LINEAR:NEOX_FILTER_NEAREST,opacity));
            else {neox_glyph glyph={1,1,0,0,2,2};OK(neox_draw_glyphs(fr,atlas,&glyph,1,&paint));}
            OK(neox_pop_state(fr));neox_submission sub;
            OK(neox_frame_end(fr,&sub));OK(neox_submission_wait(dev,sub));neox_submission_release(dev,sub);
#ifdef NEOX_ENABLE_GL
            if(kind==1)OK(neox_gl_surface_read_pixels(target.surface,pixels));
#endif
#ifdef NEOX_ENABLE_METAL
            if(kind==2)OK(neox_mtl_surface_read_pixels(target.surface,pixels));
#endif
            for(int y=0;y<H;y++)for(int x=0;x<W;x++){
                double cov=0;
                if(y>=1&&y<3){
                    if(type==0)cov=x==2?1:(x==1||x==3?0.5:0);
                    else {double lx=x+0.5-(transform?0.25*(y+0.5)+0.1:0);
                        if(lx>=1&&lx<=3)cov=type==1?1:alphas[ci]/255.0;}
                }
                uint8_t ref[4];expected((neox_blend)mode,(alphas[ai]/255.0)*opacity,clear.a,cov,ref);
                for(int c=0;c<4;c++)if(abs((int)pixels[(y*W+x)*4+c]-ref[c])>1){
                    fprintf(stderr,"backend=%d type=%d mode=%d alpha=%u coverage=%u pixel=%d,%d channel=%d actual=%u expected=%u\n",kind,type,mode,alphas[ai],alphas[ci],x,y,c,pixels[(y*W+x)*4+c],ref[c]);exit(1);
                }
            }
            renders++;
        }
        neox_texture_release(dev,tex);neox_texture_release(dev,atlas);
    }
    neox_device_destroy(dev);CHECK(ctx->used==0);neox_context_destroy(ctx);CHECK(live==0);
    printf("ALL PASS (%u blend cases; independent analytic coverage and Porter-Duff)\n",renders);return 0;
}
