/* AppKit stays in the optional presentation module. All window calls, and
 * destruction of contexts/devices attached to windows, require the main thread. */
#include "present_internal.h"
#include "internal.h"
#if NEOX_ENABLE_METAL
#include "metal_present.h"
#endif
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

@interface NXView : NSView
@property(nonatomic, assign) neox_window *owner;
@end
@interface NXWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) neox_window *owner;
@end

typedef struct nx_cocoa {
    NSWindow *window;
    NXView *view;
    NXWindowDelegate *delegate;
    CGImageRef image;
    uint32_t image_w, image_h;
    uint32_t geometry_w, geometry_h;
    float geometry_scale;
#if NEOX_ENABLE_METAL
    neox_mtl_surface *surface;
    CAMetalLayer *metal_layer;
    uint32_t width, height;
    uint8_t *snapshot;
    size_t snapshot_bytes;
    int target_valid, capture, captured;
#endif
} nx_cocoa;

static int main_thread(void)
{
    if ([NSThread isMainThread]) return 1;
    nx_win_set_error("Cocoa window operations require the main thread");
    return 0;
}
static void update_geometry(neox_window *w)
{
    nx_cocoa *p=(nx_cocoa *)w->plat;
    NSSize sz=p->view.bounds.size;
    if (!isfinite(sz.width) || !isfinite(sz.height) || sz.width<1 || sz.height<1 ||
        sz.width>INT32_MAX || sz.height>INT32_MAX) return;
    /* present.c publishes requested metrics before calling resize. Compare
     * against the last native geometry, not those speculative metrics. */
    uint32_t oldw=p->geometry_w, oldh=p->geometry_h;
    float oldscale=p->geometry_scale;
    float scale=w->scale_locked?w->scale:(float)p->window.backingScaleFactor;
    if(nx_win_set_geometry(w,(uint32_t)llround(sz.width),(uint32_t)llround(sz.height),scale)!=NEOX_OK)return;
    neox_window_event ev={};
    ev.width=w->pt_w;ev.height=w->pt_h;ev.scale=w->scale;
    if(oldscale!=w->scale){ev.kind=NEOX_WINDOW_SCALE;nx_win_push_event(w,&ev);}
    if(oldw!=w->px_w || oldh!=w->px_h || oldscale!=w->scale){
        ev.kind=NEOX_WINDOW_RESIZE;nx_win_push_event(w,&ev);
        w->soft_w=w->soft_h=0;
#if NEOX_ENABLE_METAL
        p->target_valid=p->capture=p->captured=0;
#endif
    }
    p->geometry_w=w->px_w;p->geometry_h=w->px_h;p->geometry_scale=w->scale;
#if NEOX_ENABLE_METAL
    if(p->metal_layer){
        p->metal_layer.contentsScale=w->scale;
        p->metal_layer.drawableSize=CGSizeMake(w->px_w,w->px_h);
    }
#endif
}
static uint32_t buttons(NSUInteger b)
{ return (uint32_t)((b&1)|((b&4)>>1)|((b&2)<<1)); }
@implementation NXView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (void)pointer:(NSEvent *)event button:(int)button pressed:(int)pressed {
    if(!self.owner)return;
    NSPoint pt=[self convertPoint:event.locationInWindow fromView:nil];
    neox_window_event ev={};ev.kind=NEOX_WINDOW_POINTER;ev.x=(float)pt.x;ev.y=(float)pt.y;
    ev.button=button;ev.pressed=pressed;ev.buttons=buttons([NSEvent pressedMouseButtons]);
    nx_win_push_event(self.owner,&ev);
}
- (void)mouseMoved:(NSEvent *)e {[self pointer:e button:0 pressed:0];}
- (void)mouseDragged:(NSEvent *)e {[self pointer:e button:0 pressed:0];}
- (void)rightMouseDragged:(NSEvent *)e {[self pointer:e button:0 pressed:0];}
- (void)otherMouseDragged:(NSEvent *)e {[self pointer:e button:0 pressed:0];}
- (void)mouseDown:(NSEvent *)e {[self pointer:e button:1 pressed:1];}
- (void)mouseUp:(NSEvent *)e {[self pointer:e button:1 pressed:0];}
- (void)rightMouseDown:(NSEvent *)e {[self pointer:e button:3 pressed:1];}
- (void)rightMouseUp:(NSEvent *)e {[self pointer:e button:3 pressed:0];}
- (void)otherMouseDown:(NSEvent *)e {[self pointer:e button:2 pressed:1];}
- (void)otherMouseUp:(NSEvent *)e {[self pointer:e button:2 pressed:0];}
- (void)key:(NSEvent *)e pressed:(int)pressed {
    if(!self.owner)return;
    neox_window_event ev={};ev.kind=NEOX_WINDOW_KEY;ev.keycode=e.keyCode;ev.pressed=pressed;
    NSString *characters=e.charactersIgnoringModifiers;
    if(e.keyCode==53)ev.keysym=0xff1b;
    else if(characters.length)ev.keysym=[characters characterAtIndex:0];
    nx_win_push_event(self.owner,&ev);
}
- (void)keyDown:(NSEvent *)e {[self key:e pressed:1];}
- (void)keyUp:(NSEvent *)e {[self key:e pressed:0];}
@end
@implementation NXWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    (void)sender;if(self.owner)neox_window_request_close(self.owner);
    return NO; /* application consumes CLOSE and decides when to destroy */
}
- (void)windowDidResize:(NSNotification *)n {(void)n;if(self.owner)update_geometry(self.owner);}
- (void)windowDidChangeBackingProperties:(NSNotification *)n {(void)n;if(self.owner)update_geometry(self.owner);}
@end

static neox_result cocoa_open(neox_window *w)
{
    if(!main_thread())return NEOX_ERROR_INVALID_ARGUMENT;
    /* Bound native geometry before passing it into AppKit. Rendering backends
     * also have their own limits; the public arithmetic bound is not a promise
     * that the window system can allocate INT32_MAX-sized windows. */
    if(w->pt_w>16384 || w->pt_h>16384 || w->px_w>16384 || w->px_h>16384)
        return NEOX_ERROR_INVALID_ARGUMENT;
    @autoreleasepool {
        if(!NSApp){[NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];[NSApp finishLaunching];}
        if(![NSScreen screens].count){nx_win_set_error("Cocoa has no active display");return NEOX_ERROR_BACKEND;}
        nx_cocoa *p=(nx_cocoa *)calloc(1,sizeof(*p));if(!p)return NEOX_ERROR_OUT_OF_MEMORY;
        w->plat=p;
        p->window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,w->pt_w,w->pt_h)
            styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable|NSWindowStyleMaskMiniaturizable
            backing:NSBackingStoreBuffered defer:NO];
        if(!p->window)return NEOX_ERROR_BACKEND;
        p->window.releasedWhenClosed=NO;
        p->window.title=[NSString stringWithUTF8String:w->title]?:@"NeoX";
        p->view=[[NXView alloc] initWithFrame:NSMakeRect(0,0,w->pt_w,w->pt_h)];
        p->view.owner=w;p->view.wantsLayer=YES;
        p->window.contentView=p->view;
        p->delegate=[[NXWindowDelegate alloc] init];p->delegate.owner=w;p->window.delegate=p->delegate;
        p->window.acceptsMouseMovedEvents=YES;
        [p->window center];[p->window makeKeyAndOrderFront:nil];[p->window makeFirstResponder:p->view];
        update_geometry(w);[NSApp updateWindows];return NEOX_OK;
    }
}
static void cocoa_close(neox_window *w)
{
    nx_cocoa *p=(nx_cocoa *)w->plat;if(!p)return;
    @autoreleasepool {
        p->view.owner=NULL;p->delegate.owner=NULL;p->window.delegate=nil;
        [p->window orderOut:nil];[p->window close];
        [p->window release];[p->delegate release];[p->view release];
        if(p->image)CGImageRelease(p->image);
#if NEOX_ENABLE_METAL
        [p->metal_layer release];free(p->snapshot);
#endif
        free(p);w->plat=NULL;
    }
}
static int cocoa_poll(neox_window *w)
{
    (void)w;if(!main_thread())return 0;
    @autoreleasepool {
        /* Bound work to keep a busy input stream from starving the host loop. */
        for(int i=0;i<256;i++){
            NSEvent *event=[NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast]
                inMode:NSDefaultRunLoopMode dequeue:YES];
            if(!event)break;[NSApp sendEvent:event];
        }
        [NSApp updateWindows];
    }
    return 0;
}
static neox_result cocoa_resize(neox_window *w,uint32_t width,uint32_t height)
{
    if(!main_thread())return NEOX_ERROR_INVALID_ARGUMENT;
    if(width>16384 || height>16384 || w->px_w>16384 || w->px_h>16384)return NEOX_ERROR_INVALID_ARGUMENT;
    @autoreleasepool {
        nx_cocoa *p=(nx_cocoa *)w->plat;
        [p->window setContentSize:NSMakeSize(width,height)];update_geometry(w);
        return NEOX_OK;
    }
}
static neox_result cocoa_soft(neox_window *w)
{
    if(!main_thread())return NEOX_ERROR_INVALID_ARGUMENT;
    if(w->dev->active_frame || w->dev->pending_count)return NEOX_ERROR_INVALID_ARGUMENT;
    nx_cocoa *p=(nx_cocoa *)w->plat;
    @autoreleasepool {
        CFDataRef data=CFDataCreate(kCFAllocatorDefault,w->soft_px,(CFIndex)((size_t)w->px_w*w->px_h*4));
        if(!data)return NEOX_ERROR_OUT_OF_MEMORY;
        CGDataProviderRef provider=CGDataProviderCreateWithCFData(data);CFRelease(data);
        CGColorSpaceRef color=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGImageRef image=CGImageCreate(w->px_w,w->px_h,8,32,(size_t)w->px_w*4,color,
            kCGImageAlphaNoneSkipLast|kCGBitmapByteOrder32Big,provider,NULL,false,kCGRenderingIntentDefault);
        CGColorSpaceRelease(color);CGDataProviderRelease(provider);
        if(!image)return NEOX_ERROR_OUT_OF_MEMORY;
        if(p->image)CGImageRelease(p->image);p->image=image;p->image_w=w->px_w;p->image_h=w->px_h;
        p->view.layer.contents=(id)image;p->view.layer.contentsGravity=kCAGravityResize;
        [CATransaction flush];return NEOX_OK;
    }
}
static int cocoa_read(neox_window *w,uint8_t *out)
{
    nx_cocoa *p=(nx_cocoa *)w->plat;
    if(!p->image || p->image_w!=w->px_w || p->image_h!=w->px_h)return -1;
    CFDataRef data=CGDataProviderCopyData(CGImageGetDataProvider(p->image));
    if(!data)return -1;
    memcpy(out,CFDataGetBytePtr(data),(size_t)w->px_w*w->px_h*4);CFRelease(data);return 0;
}
#if NEOX_ENABLE_METAL
static int current(neox_window *w)
{ nx_cocoa *p=(nx_cocoa *)w->plat;return p->target_valid && p->surface && p->width==w->px_w && p->height==w->px_h; }
static neox_result cocoa_gpu_create(neox_window *w,neox_context *ctx,neox_device **out)
{
    if(!main_thread())return NEOX_ERROR_INVALID_ARGUMENT;
    neox_mtl_device_desc md={};md.struct_size=sizeof(md);md.magic=NEOX_MTL_DESC_MAGIC;
    neox_device_desc dd={};dd.backend=NEOX_BACKEND_GPU;dd.platform=&md;dd.platform_size=sizeof(md);
    neox_result r=neox_device_create(ctx,&dd,out);if(r!=NEOX_OK)return r;
    @autoreleasepool {
        nx_cocoa *p=(nx_cocoa *)w->plat;
        p->metal_layer=[[CAMetalLayer alloc] init];p->metal_layer.opaque=YES;
        p->metal_layer.displaySyncEnabled=w->vsync?YES:NO;
        p->metal_layer.contentsScale=w->scale;
        p->metal_layer.drawableSize=CGSizeMake(w->px_w,w->px_h);
        CGColorSpaceRef color=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        p->metal_layer.colorspace=color;CGColorSpaceRelease(color);
        p->view.layer=p->metal_layer;return NEOX_OK;
    }
}
static neox_result cocoa_gpu_acquire(neox_window *w,neox_target_desc *out)
{
    if(!main_thread())return NEOX_ERROR_INVALID_ARGUMENT;
    if(w->dev->active_frame || w->dev->pending_count)return NEOX_ERROR_INVALID_ARGUMENT;
    nx_cocoa *p=(nx_cocoa *)w->plat;
    if(!p->surface || p->width!=w->px_w || p->height!=w->px_h) {
        neox_mtl_surface *next=NULL;
        neox_result r=neox_mtl_surface_create(w->dev,w->px_w,w->px_h,&next);if(r!=NEOX_OK)return r;
        if(p->surface)neox_mtl_surface_destroy(w->dev,p->surface);
        p->surface=next;p->width=w->px_w;p->height=w->px_h;
        p->metal_layer.drawableSize=CGSizeMake(w->px_w,w->px_h);
    }
    p->target_valid=1;p->capture=p->captured=0;
    out->kind=NEOX_BACKEND_GPU;out->surface=p->surface;return NEOX_OK;
}
static neox_result cocoa_gpu_present(neox_window *w)
{
    if(!main_thread() || !current(w))return NEOX_ERROR_INVALID_ARGUMENT;
    nx_cocoa *p=(nx_cocoa *)w->plat;p->captured=0;
    if(w->dev->active_frame || w->dev->pending_count)return NEOX_ERROR_INVALID_ARGUMENT;
    if(p->capture) {
        size_t bytes=(size_t)w->px_w*w->px_h*4;
        if(bytes>p->snapshot_bytes){uint8_t *next=(uint8_t *)realloc(p->snapshot,bytes);if(!next)return NEOX_ERROR_OUT_OF_MEMORY;p->snapshot=next;p->snapshot_bytes=bytes;}
    }
    neox_result r=nx_mtl_present(p->surface,p->metal_layer,p->capture?p->snapshot:NULL);
    p->captured=r==NEOX_OK && p->capture;p->capture=0;return r;
}
static neox_result cocoa_gpu_request(neox_window *w)
{
    if(!main_thread() || !current(w))return NEOX_ERROR_INVALID_ARGUMENT;
    ((nx_cocoa *)w->plat)->capture=1;return NEOX_OK;
}
static neox_result cocoa_gpu_read(neox_window *w,uint8_t *out)
{
    if(!main_thread() || !current(w))return NEOX_ERROR_INVALID_ARGUMENT;
    nx_cocoa *p=(nx_cocoa *)w->plat;if(!p->captured)return NEOX_ERROR_UNSUPPORTED;
    memcpy(out,p->snapshot,(size_t)w->px_w*w->px_h*4);return NEOX_OK;
}
static void cocoa_gpu_detach(neox_window *w)
{
    nx_cocoa *p=(nx_cocoa *)w->plat;p->surface=NULL;p->width=p->height=0;p->target_valid=p->capture=p->captured=0;
    @autoreleasepool {p->view.layer=[CALayer layer];[p->metal_layer release];p->metal_layer=nil;}
    if(p->image){CGImageRelease(p->image);p->image=NULL;}
}
static const nx_win_gpu_vt gpu={cocoa_gpu_create,cocoa_gpu_acquire,cocoa_gpu_present,cocoa_gpu_request,cocoa_gpu_read,cocoa_gpu_detach};
#else
static neox_result cocoa_no_gpu(neox_window *w,neox_context *c,neox_device **out)
{(void)w;(void)c;(void)out;nx_win_set_error("Metal backend not built");return NEOX_ERROR_UNSUPPORTED;}
static const nx_win_gpu_vt gpu={cocoa_no_gpu,NULL,NULL,NULL,NULL,NULL};
#endif
const nx_win_vt *nx_present_cocoa_vt(void)
{
    static const nx_win_vt vt={"cocoa",cocoa_open,cocoa_close,cocoa_poll,cocoa_resize,cocoa_soft,cocoa_read,NULL,NULL,NULL,&gpu};
    return &vt;
}
