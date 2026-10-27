/* Native Cocoa integration: actual windows, event translation and target lifetime. */
#include "neox.h"
#include "neox_present.h"
#import <AppKit/AppKit.h>
#include <pthread.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"FAIL %d: %s (%s)\n",__LINE__,#c,neox_window_last_error()?neox_window_last_error():""); exit(1); } } while(0)
#define OK(c) CHECK((c)==NEOX_OK)
static unsigned checks;

static neox_present_desc descriptor(void)
{
    neox_present_desc d={};
    d.struct_size=sizeof(d);d.magic=NEOX_PRESENT_DESC_MAGIC;
    d.title="NeoX Cocoa integration";d.width=128;d.height=96;d.vsync=1;
    return d;
}
static void drain(neox_window *w)
{
    neox_window_event e;
    for(int i=0;i<512 && neox_window_poll(w,&e);i++){}
}
static int event(neox_window *w, uint32_t kind, neox_window_event *e)
{
    for(int i=0;i<512;i++) {
        if(!neox_window_poll(w,e))return 0;
        if(e->kind==kind)return 1;
    }
    return 0;
}
static NSWindow *native_window(void)
{
    for(NSWindow *w in NSApp.windows)
        if([w.title isEqualToString:@"NeoX Cocoa integration"])return w;
    return nil;
}
static void *wrong_thread(void *unused)
{
    (void)unused;
    neox_present_desc d=descriptor();neox_window *w=NULL;
    CHECK(neox_window_create(&d,&w)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(w==NULL);
    return NULL;
}
static void test_events(neox_window *w)
{
    NSWindow *native=native_window();CHECK(native!=nil);
    uint32_t pw,ph,width,height;float scale;
    neox_window_metrics(w,&width,&height,&scale,&pw,&ph);
    CHECK(width==128 && height==96);
    CHECK(fabs(scale-native.backingScaleFactor)<0.001);
    CHECK(pw==(uint32_t)lround(width*scale) && ph==(uint32_t)lround(height*scale));
    CHECK(strcmp(neox_window_backend_name(w),"cocoa")==0);
    drain(w);
    NSView *view=native.contentView;
    NSEvent *key=[NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0
        timestamp:0 windowNumber:native.windowNumber context:nil characters:@"\033"
        charactersIgnoringModifiers:@"\033" isARepeat:NO keyCode:53];
    [view keyDown:key];
    neox_window_event e;
    CHECK(event(w,NEOX_WINDOW_KEY,&e));CHECK(e.keysym==0xff1b && e.keycode==53 && e.pressed==1);
    key=[NSEvent keyEventWithType:NSEventTypeKeyUp location:NSZeroPoint modifierFlags:0
        timestamp:0 windowNumber:native.windowNumber context:nil characters:@"a"
        charactersIgnoringModifiers:@"a" isARepeat:NO keyCode:0];
    [view keyUp:key];CHECK(event(w,NEOX_WINDOW_KEY,&e));CHECK(e.keysym=='a' && !e.pressed);
    NSPoint point=[view convertPoint:NSMakePoint(17,23) toView:nil];
    NSEvent *mouse=[NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:point modifierFlags:0
        timestamp:0 windowNumber:native.windowNumber context:nil eventNumber:1 clickCount:1 pressure:1];
    [view mouseDown:mouse];CHECK(event(w,NEOX_WINDOW_POINTER,&e));
    CHECK(e.button==1 && e.pressed && fabs(e.x-17)<0.01 && fabs(e.y-23)<0.01);
    [view mouseUp:mouse];CHECK(event(w,NEOX_WINDOW_POINTER,&e));CHECK(e.button==1 && !e.pressed);
    [native setContentSize:NSMakeSize(144,104)];
    CHECK(event(w,NEOX_WINDOW_RESIZE,&e));CHECK(e.width==144 && e.height==104);
    OK(neox_window_resize(w,128,96));drain(w);
    pthread_t thread;CHECK(pthread_create(&thread,NULL,wrong_thread,NULL)==0);
    CHECK(pthread_join(thread,NULL)==0);
}
static void draw(neox_window *w,neox_device *dev,int snapshot)
{
    neox_target_desc td={};neox_frame *fr=NULL;neox_submission sub={};
    neox_color red={1,0,0,1};
    OK(neox_window_acquire_target(w,dev,&td));td.clear=&red;
    OK(neox_frame_begin(dev,&td,&fr));OK(neox_frame_end(fr,&sub));
    OK(neox_submission_wait(dev,sub));neox_submission_release(dev,sub);
    if(snapshot)OK(neox_window_request_readback(w));
    OK(neox_window_present(w));
}
static void test_target(neox_window *w,neox_context *ctx,neox_backend_kind kind)
{
    neox_device *dev=NULL;neox_target_desc td={},unused={};neox_frame *fr=NULL;neox_submission sub={};
    neox_color red={1,0,0,1};
    OK(neox_window_create_device(w,ctx,kind,&dev));
    neox_device *duplicate=NULL;
    CHECK(neox_window_create_device(w,ctx,kind,&duplicate)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(duplicate==NULL);
    OK(neox_window_acquire_target(w,dev,&td));td.clear=&red;
    OK(neox_frame_begin(dev,&td,&fr));
    CHECK(neox_window_acquire_target(w,dev,&unused)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(neox_window_present(w)==NEOX_ERROR_INVALID_ARGUMENT);
    OK(neox_frame_end(fr,&sub));
    CHECK(neox_window_acquire_target(w,dev,&unused)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(neox_window_present(w)==NEOX_ERROR_INVALID_ARGUMENT);
    OK(neox_window_resize(w,144,104));
    CHECK(neox_window_acquire_target(w,dev,&unused)==NEOX_ERROR_INVALID_ARGUMENT);
    OK(neox_submission_wait(dev,sub));neox_submission_release(dev,sub);
    draw(w,dev,1);
    uint32_t pw,ph;neox_window_metrics(w,NULL,NULL,NULL,&pw,&ph);
    uint8_t *pixels=(uint8_t *)malloc((size_t)pw*ph*4);CHECK(pixels!=NULL);
    OK(neox_window_read_pixels(w,pixels));CHECK(pixels[0]==255 && pixels[1]==0 && pixels[2]==0 && pixels[3]==255);
    /* Rejected resize preserves both metrics and the presented snapshot. */
    CHECK(neox_window_resize(w,20000,104)==NEOX_ERROR_INVALID_ARGUMENT);
    OK(neox_window_read_pixels(w,pixels));
    /* Returning to the old dimensions must not resurrect a borrowed target. */
    OK(neox_window_resize(w,152,112));OK(neox_window_resize(w,144,104));
    CHECK(neox_window_present(w)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(neox_window_read_pixels(w,pixels)==NEOX_ERROR_INVALID_ARGUMENT);
    draw(w,dev,1);
    /* A native resize follows the same invalidation rules. */
    NSWindow *native=native_window();[native setContentSize:NSMakeSize(152,112)];drain(w);
    [native setContentSize:NSMakeSize(144,104)];drain(w);
    CHECK(neox_window_present(w)==NEOX_ERROR_INVALID_ARGUMENT);
    CHECK(neox_window_read_pixels(w,pixels)==NEOX_ERROR_INVALID_ARGUMENT);
    draw(w,dev,1);
    OK(neox_window_acquire_target(w,dev,&td));
    if(kind==NEOX_BACKEND_GPU)CHECK(neox_window_read_pixels(w,pixels)==NEOX_ERROR_UNSUPPORTED);
    free(pixels);
    /* Destroying a device drains queued work and detaches the window. */
    td.clear=&red;OK(neox_frame_begin(dev,&td,&fr));OK(neox_frame_end(fr,&sub));
    neox_device_destroy(dev);
    CHECK(neox_window_present(w)==NEOX_ERROR_INVALID_ARGUMENT);
}
int main(int argc, char **argv)
{
    (void)argv;
    @autoreleasepool {
        neox_present_desc desc=descriptor();neox_window *w=NULL;neox_context *ctx=NULL;neox_device *dev=NULL;
        OK(neox_window_create(&desc,&w));test_events(w);
        OK(neox_context_create(NULL,&ctx));
#if NEOX_ENABLE_METAL
        if(argc>1) {
            /* Report unavailable GPU explicitly in hosted CI; hardware acceptance
             * sets REQUIRE_METAL/PRESENT so it cannot pass by skipping. */
            neox_result r=neox_window_create_device(w,ctx,NEOX_BACKEND_GPU,&dev);
            if(r!=NEOX_OK) {
                fprintf(stderr,"Cocoa Metal unavailable: %s\n",neox_get_error_detail(ctx));
                neox_window_destroy(w);neox_context_destroy(ctx);
                return getenv("NEOX_REQUIRE_METAL") || getenv("NEOX_REQUIRE_PRESENT") ? 1 : 77;
            }
            neox_device_destroy(dev);
            test_target(w,ctx,NEOX_BACKEND_GPU);
        } else test_target(w,ctx,NEOX_BACKEND_SOFT);
#else
        (void)argc;test_target(w,ctx,NEOX_BACKEND_SOFT);
        CHECK(neox_window_create_device(w,ctx,NEOX_BACKEND_GPU,&dev)==NEOX_ERROR_UNSUPPORTED);
        CHECK(dev==NULL);
#endif
        /* Reattach Soft after GPU detach; context-first destruction is safe. */
        OK(neox_window_create_device(w,ctx,NEOX_BACKEND_SOFT,&dev));draw(w,dev,0);
        neox_context_destroy(ctx);CHECK(neox_window_present(w)==NEOX_ERROR_INVALID_ARGUMENT);
        drain(w);[native_window() performClose:nil];
        neox_window_event e;CHECK(event(w,NEOX_WINDOW_CLOSE,&e));neox_window_destroy(w);
        /* Window-first GPU destruction drains a queued frame before layer close. */
#if NEOX_ENABLE_METAL
        if(argc>1) {
        OK(neox_window_create(&desc,&w));OK(neox_context_create(NULL,&ctx));
        OK(neox_window_create_device(w,ctx,NEOX_BACKEND_GPU,&dev));
        neox_target_desc td={};neox_frame *fr=NULL;neox_submission sub={};neox_color blue={0,0,1,1};
        OK(neox_window_acquire_target(w,dev,&td));td.clear=&blue;
        OK(neox_frame_begin(dev,&td,&fr));OK(neox_frame_end(fr,&sub));
        neox_window_destroy(w);neox_context_destroy(ctx);
        }
#endif
        printf("cocoa: %u checks passed\n",checks);
    }
    return 0;
}
