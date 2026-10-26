/* Execute the actual adapter boundary code with no compositor or display.
 * Invalid geometry must return before dispatching to any native API. */
#include "../src/present_wl.c"
#include "../src/present_x11.c"
#include <float.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define OK(x) CHECK((x)==NEOX_OK)
static int native_resizes, native_syncs;
static int mock_resize(void *d,unsigned long w,unsigned int x,unsigned int y)
{ (void)d;(void)w;CHECK(x==65535 && y==1);native_resizes++;return 1; }
static int mock_sync(void *d,int discard)
{ (void)d;(void)discard;native_syncs++;return 1; }
static int event_left;
static int mock_pending(void *d) { (void)d;return event_left; }
static int mock_next(void *d,void *event)
{
    unsigned char *e=event; int kind=22,width=101,height=50;unsigned long window=1;
    (void)d;memset(e,0,NX_XEVENT);memcpy(e,&kind,sizeof(kind));
    memcpy(e+40,&window,sizeof(window));memcpy(e+56,&width,sizeof(width));
    memcpy(e+60,&height,sizeof(height));event_left=0;return 0;
}
int main(void)
{
    neox_window w={0}; nx_wl wl={0}; nx_x11 x={0};
    /* Wayland rejects the pool before dropping the previous buffers. */
    void *sentinel=(void*)(uintptr_t)1;wl.pool=sentinel;wl.map=sentinel;
    wl.shm_fd=-1;wl.buf_w=wl.buf_h=8;
    CHECK(ensure_pool(&wl,16384,16384)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(ensure_pool(&wl,UINT32_MAX,UINT32_MAX)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(wl.pool==sentinel && wl.map==sentinel && wl.buf_w==8);
    /* Largest whole pixel count fitting two signed-size buffers: no allocation
     * because it matches the existing pool; the next pixel must be rejected. */
    wl.buf_w=268435455u;wl.buf_h=1;
    OK(ensure_pool(&wl,wl.buf_w,wl.buf_h));
    CHECK(ensure_pool(&wl,268435456u,1)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(wl.pool==sentinel && wl.map==sentinel);
    w.scale=FLT_MAX;CHECK(shell_scale(&w)==0);
    w.scale=(float)INT32_MAX;CHECK(shell_scale(&w)==0);
    w.scale=1.5f;CHECK(shell_scale(&w)==2);
    w.scale=0.5f;CHECK(shell_scale(&w)==1);
    memset(&wl,0,sizeof(wl));wl.win=&w;wl.opened=1;
    OK(nx_win_set_geometry(&w,8,8,2));
    neox_window before=w;
    on_out_scale(&wl,NULL,INT32_MAX);CHECK(memcmp(&before,&w,sizeof(w))==0);
    on_tl_configure(&wl,NULL,INT32_MAX,8,NULL);CHECK(memcmp(&before,&w,sizeof(w))==0);
    on_tl_configure(&wl,NULL,9,7,NULL);
    CHECK(w.pt_w==9 && w.pt_h==7 && w.px_w==18 && w.px_h==14 && w.qn==1);
    on_out_scale(&wl,NULL,3);CHECK(w.scale==3 && w.px_w==27 && w.qn==2);
    /* X11 CARD16 boundary and ConfigureNotify's exact physical dimensions. */
    memset(&w,0,sizeof(w));w.plat=&x;w.vt=nx_present_x11_vt();x.win=1;x.dpy=sentinel;
    OK(nx_win_set_geometry(&w,8,8,1));before=w;
    CHECK(neox_window_resize(&w,65536,1)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(memcmp(&before,&w,sizeof(w))==0);
    g_x.resize=mock_resize;g_x.sync=mock_sync;
    OK(neox_window_resize(&w,65535,1));CHECK(native_resizes==1 && native_syncs==1);
    OK(nx_win_set_geometry(&w,67,33,1.5f));
    /* 67*1.5 rounds to 101; 33*1.5 rounds to 50. Now set a prior different
     * physical extent with the same logical dimensions, as a compositor may. */
    w.px_w=100;w.px_h=49;
    g_x.pending=mock_pending;g_x.next=mock_next;event_left=1;
    CHECK(x11_poll(&w)==0 && w.px_w==101 && w.px_h==50 && w.qn==1);
    puts("ALL PASS (actual X11/Wayland bounds, events, and rejection before native calls)");
    return 0;
}
