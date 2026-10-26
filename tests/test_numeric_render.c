/* Extreme finite inputs compared with analytically known pixels, not another
 * renderer. Errors must preserve a previously rendered blue target. */
#include "internal.h"
#include "neox_gl.h"
#include "neox_mtl.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL case %d line %d: %s\n",test_case,__LINE__,#x); exit(1); } } while (0)
#define OK(x) CHECK((x) == NEOX_OK)
static int test_case = -1;
static neox_frame *begin(neox_device *dev, neox_target_desc *target, const neox_color *clear)
{ neox_frame *f; target->clear=clear; OK(neox_frame_begin(dev,target,&f)); return f; }
static neox_result finish(neox_device *dev, neox_frame *fr)
{
    neox_submission sub; OK(neox_frame_end(fr,&sub));
    neox_result r=neox_submission_wait(dev,sub); neox_submission_release(dev,sub); return r;
}
int main(int argc, char **argv)
{
    int kind=argc>1 && !strcmp(argv[1],"gl") ? 1 : argc>1 && !strcmp(argv[1],"metal") ? 2 : 0;
    neox_context *ctx; neox_device *dev; neox_device_desc dd={0};
    neox_target_desc target={0}; uint8_t pixels[64];
    OK(neox_context_create(NULL,&ctx));
#ifdef NEOX_ENABLE_GL
    neox_gl_device_desc gd={0}; gd.struct_size=sizeof(gd); gd.magic=NEOX_GL_DESC_MAGIC;
    if(kind==1){dd.backend=NEOX_BACKEND_GPU;dd.platform=&gd;dd.platform_size=sizeof(gd);}
#endif
#ifdef NEOX_ENABLE_METAL
    neox_mtl_device_desc md={0}; md.struct_size=sizeof(md); md.magic=NEOX_MTL_DESC_MAGIC;
    if(kind==2){dd.backend=NEOX_BACKEND_GPU;dd.platform=&md;dd.platform_size=sizeof(md);}
#endif
    if(neox_device_create(ctx,&dd,&dev)!=NEOX_OK){
        const char *req=getenv(kind==1?"NEOX_REQUIRE_GL":"NEOX_REQUIRE_METAL");
        CHECK(!req || !*req); neox_context_destroy(ctx); return 77;
    }
    target.kind=dd.backend;
    if(!kind){target.soft.pixels=pixels;target.soft.width=target.soft.height=4;target.soft.stride=16;}
#ifdef NEOX_ENABLE_GL
    if(kind==1){neox_gl_surface*s;OK(neox_gl_surface_create(dev,4,4,&s));target.surface=s;}
#endif
#ifdef NEOX_ENABLE_METAL
    if(kind==2){neox_mtl_surface*s;OK(neox_mtl_surface_create(dev,4,4,&s));target.surface=s;}
#endif
    uint8_t red[]={255,0,0,255},alpha[16];memset(alpha,255,sizeof(alpha));neox_texture tex,atlas;
    neox_texture_desc td={1,1,NEOX_FORMAT_RGBA8_SRGB,red,4};OK(neox_texture_create(dev,&td,&tex));
    td=(neox_texture_desc){4,4,NEOX_FORMAT_A8,alpha,4};OK(neox_texture_create(dev,&td,&atlas));
    uint8_t corners[]={0,0,255,255,0,255,0,255,255,255,255,255,255,0,0,255};
    neox_texture edges;td=(neox_texture_desc){2,2,NEOX_FORMAT_RGBA8_SRGB,corners,8};OK(neox_texture_create(dev,&td,&edges));
    neox_color blue={0,0,1,1},green={0,1,0,1};neox_paint paint={{1,0,0,1}};
    for(test_case=0;test_case<28;test_case++){
        OK(finish(dev,begin(dev,&target,&blue)));
        neox_frame *fr=begin(dev,&target,&green);
        neox_state state={0};state.transform=nx_affine_identity();state.opacity=1;
        state.blend=NEOX_BLEND_SRC; /* exercises the GL manual blend fallback too */
        neox_rect src={0,0,1,1},dst={0,0,1,1};neox_texture selected=tex;
        neox_filter filter=NEOX_FILTER_NEAREST;neox_glyph glyph={0,0,0,0,1,1};
        int error=0,full_red=0,one_red=0,glyph_draw=0,path_draw=0,rect_red=0;
        uint8_t verbs[6]={NEOX_VERB_MOVE,NEOX_VERB_QUAD};
        float points[8]={0,0,0,1e20f,1e20f,1e20f};
        neox_path_desc pd={verbs,points,2,3,NEOX_FILL_NONZERO};
        switch(test_case){
        case 0: state.transform.tx=1e20f;break; /* outside target */
        case 1: dst=(neox_rect){-1e10f,-1e10f,2e10f,2e10f};full_red=1;break;
        case 2: state.transform.a=state.transform.d=1e20f;full_red=1;break;
        case 3: state.transform.a=state.transform.d=1e-30f;dst.w=dst.h=1e30f;one_red=1;break;
        case 4: state.transform.a=state.transform.d=1e-40f;error=1;break;
        case 5: state.transform.a=state.transform.d=1e30f;dst.x=1e30f;error=1;break;
        case 6: state.transform.a=state.transform.b=state.transform.c=state.transform.d=1e30f;break;
        case 7: state.transform.tx=1e20f;glyph_draw=1;break;
        case 9: src=(neox_rect){1e20f,1e20f,1,1};selected=edges;one_red=1;break;
        case 10: src=(neox_rect){1e20f,1e20f,1,1};selected=edges;one_red=1;filter=NEOX_FILTER_LINEAR;break;
        case 8: state.transform.a=state.transform.d=1e30f;glyph.x=1e30f;glyph_draw=1;error=1;break;
        case 11: path_draw=1;error=2;break; /* cannot flatten within tolerance */
        case 12: path_draw=1;error=1;points[2]=FLT_MAX;points[3]=0;
            points[4]=points[5]=1;state.transform.a=state.transform.d=2;break;
        case 13: path_draw=1;points[2]=FLT_MAX;
            state.transform.a=state.transform.b=state.transform.c=state.transform.d=2;break;
        case 14: path_draw=1;for(unsigned i=0;i<6;i++)points[i]=FLT_MAX;break;
        case 15: path_draw=1;rect_red=1;state.transform.tx=state.transform.ty=1;
            verbs[0]=verbs[1]=verbs[2]=NEOX_VERB_LINE;verbs[3]=NEOX_VERB_CLOSE;pd.verb_count=4;
            points[0]=2;points[1]=0;points[2]=points[3]=2;points[4]=0;points[5]=2;break;
        case 16: path_draw=1;rect_red=1;
            verbs[0]=NEOX_VERB_MOVE;verbs[1]=NEOX_VERB_CLOSE;
            verbs[2]=verbs[3]=verbs[4]=NEOX_VERB_LINE;verbs[5]=NEOX_VERB_CLOSE;
            pd.verb_count=6;pd.point_count=4;
            points[0]=points[1]=1;points[2]=3;points[3]=1;
            points[4]=points[5]=3;points[6]=1;points[7]=3;break;
        case 17: case 18: {
            float extent=test_case==17?1e20f:FLT_MAX;
            path_draw=1;full_red=1;pd.verb_count=5;pd.point_count=4;
            verbs[0]=NEOX_VERB_MOVE;verbs[1]=verbs[2]=verbs[3]=NEOX_VERB_LINE;verbs[4]=NEOX_VERB_CLOSE;
            points[0]=points[1]=-extent;points[2]=extent;points[3]=-extent;
            points[4]=points[5]=extent;points[6]=-extent;points[7]=extent;break;
        }
        case 19: /* Finite inverse; a*x overflows before adding its translation. */
            state.transform.a=1e-38f;state.transform.tx=3;
            dst=(neox_rect){0,0,1e38f,4};break;
        case 20: /* The transposed case exercises the other matrix row. */
            state.transform.d=1e-38f;state.transform.ty=3;
            dst=(neox_rect){0,0,4,1e38f};break;
        case 21: /* Two overflowing products cancel; diagonal is well inside. */
            state.transform=(neox_affine){1e-38f,0,1,1,-0.25f,0};
            dst=(neox_rect){0,0,1e38f,4};break;
        case 22: case 23: /* No pixel center is in this narrow sheared glyph. */
            state.transform=(neox_affine){1e-38f,0,1,1,-0.25f,0};
            glyph=(neox_glyph){0,0,0,0,4,4};glyph_draw=1;
            if(test_case==23)state.blend=NEOX_BLEND_PLUS;
            break;
        case 24: case 25: /* Required normalization cannot retain a tiny term. */
            state.transform=(neox_affine){1e-38f,0,1e-38f,1e38f,0,0};
            dst=(neox_rect){0,0,1e38f,1e-38f};error=2;
            glyph_draw=test_case==25;break;
        case 26: /* Nonconstant source checks restored sampling coordinates. */
            state.transform.a=1e-38f;state.transform.tx=3;
            dst=(neox_rect){0,0,1e38f,4};src=(neox_rect){0,0,1,2};selected=edges;break;
        case 27: /* Negative destination width keeps mirror semantics. */
            state.transform.a=1e-38f;state.transform.tx=3;
            dst=(neox_rect){1e38f,0,-1e38f,4};break;
        }
        OK(neox_push_state(fr,&state));
        if(path_draw) {
            neox_path path;OK(neox_path_create(dev,&pd,&path));
            OK(neox_draw_path(fr,path,&paint));neox_path_release(dev,path);
        }
        else if(glyph_draw)OK(neox_draw_glyphs(fr,atlas,&glyph,1,&paint));
        else OK(neox_draw_texture(fr,selected,src,dst,filter,1));
        OK(neox_pop_state(fr));
        neox_result r=finish(dev,fr);CHECK(r==(error==2?NEOX_ERROR_UNSUPPORTED:error?NEOX_ERROR_INVALID_ARGUMENT:NEOX_OK));
#ifdef NEOX_ENABLE_GL
        if(kind==1)OK(neox_gl_surface_read_pixels(target.surface,pixels));
#endif
#ifdef NEOX_ENABLE_METAL
        if(kind==2)OK(neox_mtl_surface_read_pixels(target.surface,pixels));
#endif
        for(unsigned i=0;i<16;i++){
            int red_pixel=full_red || (one_red && !i) ||
                (rect_red && i/4>=1 && i/4<=2 && i%4>=1 && i%4<=2) ||
                ((test_case==19 || test_case==27) && i%4==3) || (test_case==20 && i/4==3) ||
                (test_case==21 && i/4==i%4);
            if(test_case==26 && i%4==3) {
                CHECK(pixels[i*4]==(i/4<2?0:255));
                CHECK(pixels[i*4+1]==(i/4<2?0:255));
                CHECK(pixels[i*4+2]==255 && pixels[i*4+3]==255);continue;
            }
            CHECK(pixels[i*4]==(red_pixel?255:0));
            CHECK(pixels[i*4+1]==(!error && !red_pixel?255:0));
            CHECK(pixels[i*4+2]==(error?255:0));CHECK(pixels[i*4+3]==255);
        }
    }
    neox_device_destroy(dev);if(ctx->used)fprintf(stderr,"remaining CPU bytes: %zu\n",(size_t)ctx->used);CHECK(ctx->used==0);neox_context_destroy(ctx);
    puts("ALL PASS (large/tiny/degenerate transforms, bounds, overflow atomicity)");return 0;
}
