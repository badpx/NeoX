/* Compile the production geometry unit to exercise counter exhaustion without
 * allocating billions of points. Curve quality uses an independent Bernstein
 * evaluator and point-to-polyline distances, not another flattening renderer. */
#include "../src/geom.c"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define OK(x) CHECK((x)==NEOX_OK)
static nx_path_slot path(uint8_t *verbs, unsigned n, float *points)
{
    nx_path_slot p={0};p.verbs=verbs;p.verb_count=n;p.points=points;p.fill_rule=NEOX_FILL_NONZERO;
    return p;
}
static void reference(const float *p,unsigned degree,double t,double *x,double *y)
{
    /* Explicit polynomial in the Bernstein basis, independently evaluated. */
    long double u=1.0L-t,v=t;
    long double weights[4];
    if(degree==2){weights[0]=u*u;weights[1]=2*u*v;weights[2]=v*v;}
    else {weights[0]=u*u*u;weights[1]=3*u*u*v;weights[2]=3*u*v*v;weights[3]=v*v*v;}
    long double rx=0,ry=0;
    for(unsigned i=0;i<=degree;i++){rx+=weights[i]*p[i*2];ry+=weights[i]*p[i*2+1];}
    *x=(double)rx;*y=(double)ry;
}
static double distance_to_polyline(const nx_flat *f,double x,double y)
{
    double best=DBL_MAX;
    for(unsigned i=2;i<f->npts;i+=2) {
        double ax=f->pts[i-2],ay=f->pts[i-1],bx=f->pts[i],by=f->pts[i+1];
        double length=hypot(bx-ax,by-ay);
        double along=length ? ((x-ax)*(bx-ax)+(y-ay)*(by-ay))/length : 0;
        double d;
        if(along<=0)d=hypot(x-ax,y-ay);
        else if(along>=length)d=hypot(x-bx,y-by);
        else d=fabs((x-ax)*(by-ay)-(y-ay)*(bx-ax))/length;
        if(d<best)best=d;
    }
    return best;
}
static void curve_quality(neox_context *ctx,unsigned degree,float *points)
{
    uint8_t verbs[]={NEOX_VERB_MOVE,degree==2?NEOX_VERB_QUAD:NEOX_VERB_CUBIC};
    nx_path_slot p=path(verbs,2,points);nx_flat f;
    OK(nx_flatten_path(ctx,&p,nx_affine_identity(),&f));CHECK(f.npts>=4);
    double worst=0;
    for(unsigned i=0;i<=8192;i++) {
        double x,y;reference(points,degree,(double)i/8192,&x,&y);
        double d=distance_to_polyline(&f,x,y);if(d>worst)worst=d;
    }
    if(worst>NEOX_FLAT_TOL+1e-5)fprintf(stderr,"curve error %.9g, vertices %u\n",worst,f.npts/2);
    CHECK(worst<=NEOX_FLAT_TOL+1e-5);nx_flat_free(ctx,&f);
}
static void curves(neox_context *ctx)
{
    /* First half has opposing control deviations whose sum is exactly zero.
     * The previous midpoint-mean flatness test discarded the inflection. */
    float inflection[]={0,3,4,7,8,-5,12,31};curve_quality(ctx,3,inflection);
    float loop[]={0,0,40,80,-40,80,0,0};curve_quality(ctx,3,loop);
    float cusp[]={0,0,100,100,-100,-100,1,0};curve_quality(ctx,3,cusp);
    float backtrack[]={0,0,10,0,1,0};curve_quality(ctx,2,backtrack);
    float quad[]={0,0,30,60,60,0};curve_quality(ctx,2,quad);
    for(unsigned k=1;k<=12;k++) {
        float p[]={0,0, k*31.25f,k*-13.0f, k*-7.5f,k*37.0f,64,32};
        curve_quality(ctx,3,p);
    }
    uint8_t v[]={NEOX_VERB_MOVE,NEOX_VERB_CUBIC};
    float constant[]={FLT_MAX,FLT_MAX,FLT_MAX,FLT_MAX,FLT_MAX,FLT_MAX,FLT_MAX,FLT_MAX};
    nx_path_slot p=path(v,2,constant);nx_flat f;
    OK(nx_flatten_path(ctx,&p,nx_affine_identity(),&f));CHECK(f.npts==4);nx_flat_free(ctx,&f);
    v[1]=NEOX_VERB_QUAD;
    OK(nx_flatten_path(ctx,&p,nx_affine_identity(),&f));CHECK(f.npts==4);nx_flat_free(ctx,&f);
    float line[]={0,0,0,0,FLT_MAX,FLT_MAX};p.points=line;
    OK(nx_flatten_path(ctx,&p,nx_affine_identity(),&f));CHECK(f.npts==4);nx_flat_free(ctx,&f);
    float huge[]={0,0,0,1e20f,1e20f,1e20f};p.points=huge;
    size_t used=ctx->used;
    CHECK(nx_flatten_path(ctx,&p,nx_affine_identity(),&f)==NEOX_ERROR_UNSUPPORTED);
    CHECK(!f.pts && !f.polys && ctx->used==used);
    float invalid[]={0,0,FLT_MAX,0,1,1};p.points=invalid;
    CHECK(nx_flatten_path(ctx,&p,(neox_affine){2,0,0,2,0,0},&f)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(!f.pts && !f.polys && ctx->used==used);
    /* Singular matrices are legal empty geometry, even if transforming the
     * control values individually would overflow the float coordinate range. */
    OK(nx_flatten_path(ctx,&p,(neox_affine){2,2,2,2,0,0},&f));CHECK(!f.npts);
}
static void origins(neox_context *ctx)
{
    neox_affine ctm={1,0,0,1,2,3};
    uint8_t v[]={NEOX_VERB_LINE,NEOX_VERB_LINE,NEOX_VERB_CLOSE};
    float pts[]={4,0,0,4};nx_path_slot p=path(v,3,pts);float mask[8*8];
    OK(nx_rasterize_path(ctx,&p,ctm,8,8,mask));
    for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++) {
        float expected=0;
        if(x>=2 && y>=3){unsigned diagonal=(x-2)+(y-3);expected=diagonal<3?1:diagonal==3?0.5f:0;}
        CHECK(mask[y*8+x]==expected);
    }
    /* QUAD/CUBIC without MOVE, and after CLOSE, must match explicit starts. */
    for(unsigned degree=2;degree<=3;degree++) {
        uint8_t imp[]={NEOX_VERB_CLOSE,degree==2?NEOX_VERB_QUAD:NEOX_VERB_CUBIC};
        uint8_t exp[]={NEOX_VERB_MOVE,imp[1]};
        float implicit[]={3,8,8,4,10,2};float explicit[]={0,0,3,8,8,4,10,2};
        nx_path_slot a=path(imp,2,implicit),b=path(exp,2,explicit);nx_flat fa,fb;
        OK(nx_flatten_path(ctx,&a,ctm,&fa));OK(nx_flatten_path(ctx,&b,ctm,&fb));
        CHECK(fa.npts==fb.npts && memcmp(fa.pts,fb.pts,fa.npts*sizeof(float))==0);
        nx_flat_free(ctx,&fa);nx_flat_free(ctx,&fb);
        uint8_t after[]={NEOX_VERB_MOVE,NEOX_VERB_CLOSE,imp[1]};
        uint8_t restart[]={NEOX_VERB_MOVE,NEOX_VERB_CLOSE,NEOX_VERB_MOVE,imp[1]};
        float pa[]={2,4,3,8,8,4,10,2},pb[]={2,4,2,4,3,8,8,4,10,2};
        a=path(after,3,pa);b=path(restart,4,pb);
        OK(nx_flatten_path(ctx,&a,ctm,&fa));OK(nx_flatten_path(ctx,&b,ctm,&fb));
        CHECK(fa.npts==fb.npts && memcmp(fa.pts,fb.pts,fa.npts*sizeof(float))==0);
        nx_flat_free(ctx,&fa);nx_flat_free(ctx,&fb);
    }
    uint8_t after[]={NEOX_VERB_MOVE,NEOX_VERB_CLOSE,NEOX_VERB_LINE};
    float pa[]={2,4,3,8};nx_flat f;p=path(after,3,pa);
    OK(nx_flatten_path(ctx,&p,ctm,&f));CHECK(f.npolys==2 && f.polys[1].count==2);
    CHECK(f.pts[2]==4 && f.pts[3]==7);nx_flat_free(ctx,&f);
}
static void boundaries(neox_context *ctx)
{
    nx_flat f={0};f.npts=UINT32_MAX-1;
    CHECK(flat_put_pt(ctx,&f,0,0)==NEOX_ERROR_OUT_OF_MEMORY && !f.pts);
    f.npolys=UINT32_MAX;
    CHECK(flat_begin_poly(ctx,&f)==NEOX_ERROR_OUT_OF_MEMORY && !f.polys);
    float row[4]={0};nx_span_add(row,4,-FLT_MAX,FLT_MAX,0.5f);
    for(unsigned i=0;i<4;i++)CHECK(row[i]==0.5f);
    nx_span_add(row,4,0.25,1.75,0.5f);CHECK(row[0]==0.875f && row[1]==0.875f);
    /* A span starting at INT32_MAX is empty even though float(INT32_MAX)
     * rounds upward. It must return before addressing the small test row. */
    nx_span_add(row,INT32_MAX,INT32_MAX,(double)INT32_MAX+100,1);
    uint8_t verbs[]={NEOX_VERB_MOVE,NEOX_VERB_QUAD};
    float pts[]={0,0,0,1e20f,1e20f,1e20f};nx_path_slot p=path(verbs,2,pts);
    float before[4];memcpy(before,row,sizeof(row));
    CHECK(nx_rasterize_path(ctx,&p,nx_affine_identity(),2,2,row)==NEOX_ERROR_UNSUPPORTED);
    CHECK(memcmp(before,row,sizeof(row))==0);
    nx_clip_path input={&p,nx_affine_identity()};
    CHECK(nx_rasterize_intersection(ctx,&input,1,UINT32_MAX,1,row)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(nx_rasterize_intersection(ctx,&input,0,2,2,row)==NEOX_ERROR_INVALID_ARGUMENT);
    neox_result r=nx_rasterize_intersection(ctx,&input,UINT32_MAX,2,2,row);
    CHECK(r==(SIZE_MAX>UINT32_MAX?NEOX_ERROR_OUT_OF_MEMORY:NEOX_ERROR_INVALID_ARGUMENT));
    CHECK(memcmp(before,row,sizeof(row))==0);
}
static void clip_rollback(neox_context *ctx)
{
    neox_device *dev;OK(neox_device_create(ctx,NULL,&dev));neox_path curve;
    uint8_t v[]={NEOX_VERB_MOVE,NEOX_VERB_QUAD};float p[]={0,0,0,1e20f,1e20f,1e20f};
    neox_path_desc pd={v,p,2,3,NEOX_FILL_NONZERO};OK(neox_path_create(dev,&pd,&curve));
    uint8_t pixels[16]={0};neox_target_desc target={0};target.soft.pixels=pixels;
    target.soft.width=target.soft.height=2;target.soft.stride=8;neox_frame *fr;
    OK(neox_frame_begin(dev,&target,&fr));
    neox_state state={0};state.opacity=1;state.transform=nx_affine_identity();state.clip=&curve;
    /* Recording contains geometry only; executor precision limits are reported
     * by wait, with target atomicity and complete temporary-storage rollback. */
    neox_frame_cancel(fr);
    size_t baseline=0;
    for(int attempt=0;attempt<3;attempt++) {
        memset(pixels,0x7b,sizeof(pixels));
        OK(neox_frame_begin(dev,&target,&fr));OK(neox_push_state(fr,&state));
        OK(neox_pop_state(fr));neox_submission sub;OK(neox_frame_end(fr,&sub));
        CHECK(neox_submission_wait(dev,sub)==NEOX_ERROR_UNSUPPORTED);
        for(size_t i=0;i<sizeof(pixels);i++)CHECK(pixels[i]==0x7b);
        neox_submission_release(dev,sub);
        if(!attempt)baseline=ctx->used;else CHECK(ctx->used==baseline);
    }
    neox_device_destroy(dev);
}
int main(void)
{
    neox_context_desc desc={0};desc.memory_budget=256*1024;neox_context *ctx;
    OK(neox_context_create(&desc,&ctx));curves(ctx);origins(ctx);boundaries(ctx);clip_rollback(ctx);
    CHECK(ctx->used==0);neox_context_destroy(ctx);
    puts("ALL PASS (independent curve reference, implicit origins, numeric bounds, precision failure rollback)");
    return 0;
}
