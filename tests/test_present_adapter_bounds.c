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
struct fake_marshal_rec {
    void *proxy;
    uint32_t op;
    const struct wl_interface *iface;
    uint32_t ver;
    void *args[8];
};
static struct fake_marshal_rec *g_marshals;
static void *capture_marshal(void *proxy, uint32_t op,
                             const struct wl_interface *iface, uint32_t ver,
                             uint32_t flags, ...)
{
    (void)flags;
    if (g_marshals) {
        g_marshals->proxy = proxy;
        g_marshals->op = op;
        g_marshals->iface = iface;
        g_marshals->ver = ver;
    }
    return (void *)(uintptr_t)0x9000;
}
struct fake_listen_rec {
    void *proxy;
    void *data;
    void *listener;
};
static struct fake_listen_rec *g_listens;
static int capture_listen(void *proxy, void *lis, void *data)
{
    if (g_listens) {
        g_listens->proxy = proxy;
        g_listens->data = data;
        g_listens->listener = lis;
    }
    return 0;
}
static int stub_getattr_unmapped(void *dpy, unsigned long win, void *attr)
{
    int state = 0; /* IsUnmapped */
    (void)dpy; (void)win;
    memset(attr, 0, 136);
    memcpy((unsigned char *)attr + 82, &state, sizeof state);
    return 1;
}
static int stub_getattr_fail(void *dpy, unsigned long win, void *attr)
{
    (void)dpy; (void)win; (void)attr;
    return 0;
}
static int stub_flush_ok(void *dpy) { (void)dpy; return 0; }
static int stub_pending_zero(void *dpy) { (void)dpy; return 0; }
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
    /* surf_ver >= 3（现代合成器）：输出 scale 正常跟随；v<3 的 1 倍约束
     * 单独在下面验证（评审 4489969163）。 */
    memset(&wl,0,sizeof(wl));wl.win=&w;wl.opened=1;wl.surf_ver=4;
    /* 输出 scale 只跟随"当前 output"（P1-6）：先登记一个已绑定的 wl_output
     * 槽位，未 enter 之前的第一份 scale 作为窗口初值。 */
    wl.outputs[0]=sentinel;wl.noutputs=1;wl.output_scale=0;
    OK(nx_win_set_geometry(&w,8,8,2));
    neox_window before=w;
    on_out_scale(&wl,NULL,INT32_MAX);CHECK(memcmp(&before,&w,sizeof(w))==0);
    on_tl_configure(&wl,NULL,INT32_MAX,8,NULL);CHECK(memcmp(&before,&w,sizeof(w))==0);
    on_tl_configure(&wl,NULL,9,7,NULL);
    CHECK(w.pt_w==9 && w.pt_h==7 && w.px_w==18 && w.px_h==14 && w.qn==1);
    on_out_scale(&wl,sentinel,3);CHECK(w.scale==3 && w.px_w==27 && w.qn==2);
    /* 第二个 output 的 scale 不能改变窗口；compositor 把窗口挪过去（enter）
     * 之后才跟随。 */
    wl.outputs[1]=(void*)(uintptr_t)2;wl.noutputs=2;
    before=w;
    on_out_scale(&wl,wl.outputs[1],2);CHECK(memcmp(&before,&w,sizeof(w))==0);
    on_surf_enter(&wl,NULL,wl.outputs[1]);
    CHECK(w.scale==2 && w.px_w==18);
    on_surf_enter(&wl,NULL,sentinel);
    CHECK(w.scale==3 && w.px_w==27);
    /* wl_surface v<3 没有 set_buffer_scale：shell_scale 仍按整数倍，但
     * wl_open 会强制 1 倍（P2-1），此处验证按钮映射与指针坐标缓存。 */
    {
        neox_window_event ev;
        memset(&ev,0,sizeof(ev));
        on_ptr_enter(&wl,NULL,0,NULL,256*7,256*5);
        CHECK(wl.pointer_x==7.0f && wl.pointer_y==5.0f);
        on_button(&wl,NULL,0,0,0x111u,1);   /* BTN_RIGHT */
        CHECK(wl.buttons==4u);
        on_button(&wl,NULL,0,0,0x112u,1);   /* BTN_MIDDLE */
        CHECK(wl.buttons==6u);
        on_button(&wl,NULL,0,0,0x110u,1);   /* BTN_LEFT */
        CHECK(wl.buttons==7u);
        on_button(&wl,NULL,0,0,0x111u,0);
        CHECK(wl.buttons==3u);
        /* 评审 4489969148：只有 enter、还没有 motion 时按下右键，按钮事件
         * 必须携带 enter 给出的坐标（旧实现给 0,0）。 */
        {
            neox_window_event ev2;
            wl.buttons = 0;
            on_ptr_enter(&wl,NULL,1,NULL,256*13,256*11);
            CHECK(wl.pointer_x==13.0f && wl.pointer_y==11.0f);
            w.qn = 0; w.qh = w.qt = 0;
            on_button(&wl,NULL,0,0,0x111u,1);
            CHECK(w.qn == 1);
            ev2 = w.q[0];
            CHECK(ev2.kind == NEOX_WINDOW_POINTER);
            CHECK(ev2.x == 13.0f && ev2.y == 11.0f);
            CHECK(ev2.buttons == 4u);
            wl.buttons = 0;
            w.qn = 0; w.qh = w.qt = 0;
        }
    }
    /* Bound versions must not exceed either server support or our v5 cap. */
    {
        struct fake_marshal_rec rec;
        PFN_marshal saved_marshal = g_wl.marshal;
        nx_wl wl2;
        memset(&wl2, 0, sizeof wl2);
        memset(&rec, 0, sizeof rec);
        g_wl.marshal = capture_marshal;
        g_marshals = &rec;
        /* 服务端宣告 v7：应绑定到 5（不是 4），并把版本写进 comp_ver。 */
        on_global(&wl2, (void *)(uintptr_t)0x77, 3, "wl_compositor", 7);
        g_wl.marshal = saved_marshal;
        g_marshals = NULL;
        CHECK(rec.iface == g_wl.compositor && rec.ver == 5);
        CHECK(wl2.comp_ver == 5);
        /* v4 服务端：绑定 4，仍支持 enter/leave。 */
        memset(&wl2, 0, sizeof wl2);
        g_wl.marshal = capture_marshal;
        g_marshals = &rec;
        on_global(&wl2, (void *)(uintptr_t)0x77, 3, "wl_compositor", 4);
        g_wl.marshal = saved_marshal;
        g_marshals = NULL;
        CHECK(rec.ver == 4 && wl2.comp_ver == 4);
        /* 评审 4489969163：v<3 时 resize/输出 scale 更新也强制 1 倍，
         * 与 wl_open 的初始创建约束一致。 */
        {
            neox_window wv;
            memset(&wl2, 0, sizeof wl2);
            memset(&wv, 0, sizeof wv);
            wl2.win = &wv;
            wl2.opened = 1;
            wl2.surf_ver = 2;
            OK(nx_win_set_geometry(&wv, 8, 8, 1));
            on_out_scale(&wl2, NULL, 3);
            apply_output_scale(&wl2, 3);
            CHECK(wv.scale == 1.0f && wv.px_w == 8 && wv.px_h == 8);
            wl2.surf_ver = 3;
            apply_output_scale(&wl2, 2);
            CHECK(wv.scale == 2.0f && wv.px_w == 16 && wv.px_h == 16);
        }
    }
    /* Exercise the actual registration path and dispatch through the listener
     * for every supported version, including v1-v4 compositors. */
    {
        uint32_t version;
        PFN_listen saved_listen = g_wl.listen;
        struct surface_listener {
            void (*enter)(void *, void *, void *);
            void (*leave)(void *, void *, void *);
        };
        CHECK(!nx_wl_surface_enter_ready(0));
        for (version = 1; version <= 5; version++) {
            struct fake_listen_rec lrec = {0};
            nx_wl wl3 = {0};
            neox_window ww = {0};
            const struct surface_listener *listener;
            void *surface = (void *)(uintptr_t)0x11;
            void *output = (void *)(uintptr_t)0x22;
            g_wl.listen = capture_listen;
            g_listens = &lrec;
            wl3.surf_ver = version;
            wl3.win = &ww;
            wl3.opened = 1;
            wl3.outputs[0] = output;
            wl3.output_scales[0] = 2;
            wl3.noutputs = 1;
            OK(nx_win_set_geometry(&ww, 8, 8, 1));
            CHECK(nx_wl_register_enter(&wl3, surface));
            CHECK(lrec.proxy == surface && lrec.data == &wl3 && wl3.surf_enter_ready);
            listener = (const struct surface_listener *)lrec.listener;
            listener->enter(lrec.data, surface, output);
            CHECK(wl3.enter_output == output);
            CHECK(ww.scale == (version >= 3 ? 2.0f : 1.0f));
            listener->leave(lrec.data, surface, output);
            CHECK(wl3.enter_output == NULL);
        }
        g_wl.listen = saved_listen;
        g_listens = NULL;
    }

    /* 评审 4489969158：两个槽都在合成器手里时，present 必须返回可重试的
     * BACKEND 错误，且不写任何 busy 槽（P2-2）；连接已死时同样返回失败
     * 而不是"成功但没上屏"（P1-4）。这里用真实的 wl_present_soft。 */
    {
        nx_wl wp;
        neox_window ww;
        uint8_t frame[4 * 4 * 4];
        int k;
        memset(&wp, 0, sizeof wp);
        memset(&ww, 0, sizeof ww);
        memset(frame, 0xff, sizeof frame);
        /* 挂到已存在的栈上 pool：ensure_pool 发现尺寸一致就直接复用。 */
        wp.pool = sentinel;
        wp.map = sentinel;
        wp.buf_w = 4; wp.buf_h = 4;
        wp.shm_fd = -1;
        for (k = 0; k < 2; k++) { wp.busy[k] = 1; wp.bufs[k] = (void *)(uintptr_t)(0x40 + k); }
        wp.surf_ver = 4;
        /* 两个槽都 busy 时只 flush+pump：这里给出"无连接"的安全桩。 */
        g_wl.flush = stub_flush_ok;
        g_wl.pending = stub_pending_zero;
        ww.plat = &wp;
        ww.soft_px = frame;
        ww.px_w = ww.px_h = 4;
        ww.pt_w = ww.pt_h = 4;
        CHECK(wl_present_soft(&ww) == NEOX_ERROR_BACKEND);
        CHECK(wp.busy[0] && wp.busy[1]);          /* 没有被覆盖 */
        CHECK(wp.bufs[0] == (void *)(uintptr_t)0x40 && wp.bufs[1] == (void *)(uintptr_t)0x41);
        /* 连接死亡：同样的错误路径，不再尝试等待。 */
        wp.dead = 1;
        CHECK(wl_present_soft(&ww) == NEOX_ERROR_BACKEND);
        CHECK(wp.busy[0] && wp.busy[1]);
    }

    /* 评审 4489969168：viewable 等待必须有界且失败时返回明确错误码（不是把
     * 空白图当成功）。这里用桩：窗口始终 IsViewable=false，等待耗尽后
     * x11_wait_ready 返回 BACKEND；XGetWindowAttributes 失败也返回 BACKEND。 */
    {
        struct fk_attr { unsigned char raw[136]; } attr;
        nx_x11 xp;
        neox_window wq;
        int saved_ready;
        memset(&xp, 0, sizeof xp);
        memset(&wq, 0, sizeof wq);
        xp.win = (unsigned long)0x5a5a;
        xp.dpy = sentinel;
        wq.plat = &xp;
        wq.px_w = 8; wq.px_h = 8;
        g_x.getattr = stub_getattr_unmapped;   /* 永不 Viewable */
        g_x.resize = NULL;
        g_x.pending = NULL;
        saved_ready = 1;
        (void)saved_ready;
        CHECK(x11_wait_ready(&wq) == NEOX_ERROR_BACKEND);
        CHECK(neox_window_last_error() != NULL);
        g_x.getattr = stub_getattr_fail;       /* 属性查询失败 */
        CHECK(x11_wait_ready(&wq) == NEOX_ERROR_BACKEND);
        g_x.getattr = NULL;                    /* 无 XGetWindowAttributes 的桩：直接成功 */
        OK(x11_wait_ready(&wq));
        (void)attr;
    }

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
