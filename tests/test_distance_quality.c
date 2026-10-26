/* Independent analytic geometry reference: no NeoX rasterizer, distance
 * reconstruction, stb bitmap, or cross-backend image serves as the oracle. */
#include "neox.h"
#include "neox_text.h"
#ifdef NEOX_ENABLE_GL
#include "neox_gl.h"
#endif
#ifdef NEOX_ENABLE_METAL
#include "neox_mtl.h"
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define OK(x) CHECK((x)==NEOX_OK)
#define W 160
#define H 144
static uint8_t pixels[W*H*4];
/* Font coordinates; ascent 800, descent -200; 32 px => .032 px/unit. */
static int inside(int shape,double x,double y) {
    if(shape==0)return y>=0 && y<=800 && x>=100+y*.5 && x<=900-y*.5;
    if(shape==1)return x>=100 && x<=900 && y>=0 && y<=800 && !(x>300 && x<700 && y>200 && y<600);
    return x>=475 && x<=525 && y>=0 && y<=800;
}
int main(int argc,char **argv) {
    int backend=argc>1?(!strcmp(argv[1],"metal")?2:1):0;
    neox_context *ctx;neox_device *d;neox_device_desc dd={0};OK(neox_context_create(NULL,&ctx));
#ifdef NEOX_ENABLE_GL
    neox_gl_device_desc gd={0};gd.struct_size=sizeof(gd);gd.magic=NEOX_GL_DESC_MAGIC;
    if(backend==1){dd.backend=NEOX_BACKEND_GPU;dd.platform=&gd;dd.platform_size=sizeof(gd);}
#endif
#ifdef NEOX_ENABLE_METAL
    neox_mtl_device_desc md={0};md.struct_size=sizeof(md);md.magic=NEOX_MTL_DESC_MAGIC;
    if(backend==2){dd.backend=NEOX_BACKEND_GPU;dd.platform=&md;dd.platform_size=sizeof(md);}
#endif
    if(neox_device_create(ctx,&dd,&d)!=NEOX_OK){CHECK(!getenv(backend==2?"NEOX_REQUIRE_METAL":"NEOX_REQUIRE_GL"));neox_context_destroy(ctx);return 77;}
    neox_target_desc td={0};neox_color clear={0};td.clear=&clear;td.kind=dd.backend;
    if(!backend){td.soft.pixels=pixels;td.soft.width=W;td.soft.height=H;td.soft.stride=W*4;}
#ifdef NEOX_ENABLE_GL
    if(backend==1){neox_gl_surface *s;OK(neox_gl_surface_create(d,W,H,&s));td.surface=s;}
#endif
#ifdef NEOX_ENABLE_METAL
    if(backend==2){neox_mtl_surface *s;OK(neox_mtl_surface_create(d,W,H,&s));td.surface=s;}
#endif
    nx_text_context *text;nx_font *font;OK(nx_text_context_create(ctx,NULL,&text));
    OK(nx_font_load_file(text,NEOX_QUALITY_FONT,0,&font));
    const float scales[][2]={{.5f,.5f},{1,1},{2,2},{4,4},{4,1}};
    puts("mode,shape,sx,sy,normalized_L1,relative_area_error,max_alpha_error");
    for(int mode=0;mode<3;mode++) {
        nx_atlas *atlas;OK(nx_atlas_create_ex(text,d,256,128,(neox_atlas_mode)mode,4,&atlas));
        for(int shape=0;shape<3;shape++) {
            char letter[2]={"THI"[shape],0};nx_layout *layout;OK(nx_text_layout(text,font,32,letter,0,NX_ALIGN_LEFT,&layout));
            for(unsigned scale=0;scale<sizeof(scales)/sizeof(scales[0]);scale++) {
                float sx=scales[scale][0],sy=scales[scale][1];
                neox_state state={0};state.transform=(neox_affine){sx,0,0,sy,16,120};state.opacity=1;
                neox_frame *frame;neox_submission sub;neox_paint white={{1,1,1,1}};
                OK(neox_frame_begin(d,&td,&frame));OK(neox_push_state(frame,&state));
                OK(nx_text_draw(frame,atlas,layout,&white));OK(neox_pop_state(frame));
                OK(neox_frame_end(frame,&sub));OK(neox_submission_wait(d,sub));neox_submission_release(d,sub);
#ifdef NEOX_ENABLE_GL
                if(backend==1)OK(neox_gl_surface_read_pixels(td.surface,pixels));
#endif
#ifdef NEOX_ENABLE_METAL
                if(backend==2)OK(neox_mtl_surface_read_pixels(td.surface,pixels));
#endif
                double area=0,actual_area=0,l1=0,maxerr=0;
                for(int y=0;y<H;y++)for(int x=0;x<W;x++) {
                    int count=0;
                    /* 16x16 direct inside tests integrate each output pixel. */
                    for(int j=0;j<16;j++)for(int i=0;i<16;i++)
                        count+=inside(shape,(x+(i+.5)/16-16)/sx/.032,(120-y-(j+.5)/16)/sy/.032);
                    double ref=count/256.,actual=pixels[(y*W+x)*4+3]/255.,err=fabs(actual-ref);
                    area+=ref;actual_area+=actual;l1+=err;if(err>maxerr)maxerr=err;
                }
                CHECK(area>0);
                printf("%d,%d,%.1f,%.1f,%.6f,%.6f,%.6f\n",mode,shape,sx,sy,l1/area,fabs(actual_area-area)/area,maxerr);
                /* Versioned regression envelopes, not universal font-quality
                 * guarantees. Thin strokes and sharp corners remain difficult. */
                static const double limits[3][3]={{.08,.11,.36},{.03,.04,.14},{.09,.09,.27}};
                CHECK(l1/area<limits[mode][shape]);
                /* Topology checks supplement global error: the hole must stay
                 * empty and the triangle must preserve its solid interior. */
                int cx=(int)(16+500*.032*sx),cy=(int)(120-400*.032*sy);
                if(shape==1)CHECK(pixels[(cy*W+cx)*4+3]==0);
                if(shape==0)CHECK(pixels[(cy*W+cx)*4+3]==255);
            }
            nx_layout_destroy(layout);
        }
        nx_atlas_destroy(atlas);
    }
    nx_text_context_destroy(text);neox_context_destroy(ctx);return 0;
}
