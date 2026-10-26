#include "internal.h"
#include "neox_text.h"
#include <stdio.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define OK(x) CHECK((x)==NEOX_OK)
typedef struct {size_t live,blocks,calls,fail_at;int failed;} tracker;
typedef union {_Alignas(16) unsigned char align[16];struct {size_t size;tracker *owner;} meta;} allocation;
static void *allocate(void *user,size_t size,size_t align) {
    tracker*t=user;CHECK(align<=_Alignof(allocation));t->calls++;
    if(t->fail_at && t->calls==t->fail_at){t->failed=1;return NULL;}
    allocation*p=malloc(sizeof(*p)+size);CHECK(p);p->meta.size=size;p->meta.owner=t;
    t->live+=size;t->blocks++;return p+1;
}
static void release(void *user,void *ptr,size_t size,size_t align) {
    (void)align;if(!ptr)return;tracker*t=user;allocation*p=(allocation*)ptr-1;
    CHECK(p->meta.owner==t && p->meta.size==size);t->live-=size;t->blocks--;free(p);
}
static void *font_bytes;static size_t font_size;
static int load_fixture(void) {
    const char *paths[]={getenv("NEOX_TEST_FONT"),"/System/Library/Fonts/Supplemental/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    for(size_t i=0;i<sizeof(paths)/sizeof(*paths);i++) {
        if(!paths[i])continue;
        FILE*f=fopen(paths[i],"rb");if(!f)continue;
        CHECK(fseek(f,0,SEEK_END)==0);long n=ftell(f);CHECK(n>0);rewind(f);
        font_size=(size_t)n;font_bytes=malloc(font_size);CHECK(font_bytes);
        CHECK(fread(font_bytes,1,font_size,f)==font_size);fclose(f);return 1;
    }return 0;
}
static void persistent_budget(void) {
    tracker a={0},b={0};neox_allocator aa={&a,allocate,release},bb={&b,allocate,release};
    neox_context_desc cd={0};cd.allocator=&aa;cd.memory_budget=1;
    neox_context*c;nx_text_context*t=NULL;OK(neox_context_create(&cd,&c));
    CHECK(nx_text_context_create(c,&bb,&t)==NEOX_ERROR_OUT_OF_MEMORY);CHECK(!t && b.calls==0);
    c->budget=8*1024*1024;OK(nx_text_context_create(c,&bb,&t));CHECK(c->used==b.live);
    nx_font*f;OK(nx_font_load_memory(t,font_bytes,font_size,0,&f));CHECK(c->used==b.live);
    size_t before=c->used;c->budget=before;nx_layout*l=NULL;
    CHECK(nx_text_layout(t,f,96,"W",0,NX_ALIGN_LEFT,&l)==NEOX_ERROR_OUT_OF_MEMORY);
    CHECK(c->used==before && !l);c->budget=8*1024*1024;
    nx_text_context_destroy(t);CHECK(c->used==0 && b.live==0 && b.blocks==0);
    neox_context_destroy(c);CHECK(a.live==0 && a.blocks==0);
}
static void sweep(int mode) {
    for(size_t fail=1;fail<100;fail++) {
        tracker a={0};neox_allocator al={&a,allocate,release};neox_context_desc cd={0};cd.allocator=&al;cd.memory_budget=8*1024*1024;
        neox_context*c;neox_device*d;nx_text_context*t;nx_font*f;nx_layout*l;nx_atlas*at;
        OK(neox_context_create(&cd,&c));OK(neox_device_create(c,NULL,&d));
        OK(nx_text_context_create(c,NULL,&t));OK(nx_font_load_memory(t,font_bytes,font_size,0,&f));
        OK(nx_text_layout(t,f,96,"W",0,NX_ALIGN_LEFT,&l));CHECK(l->glyph_count==1);
        OK(nx_atlas_create_ex(t,d,256,256,(neox_atlas_mode)mode,4,&at));
        size_t before=c->used,live=a.live;a.calls=0;a.fail_at=fail;nx_atlas_rect rect;
        neox_result result=nx_atlas_glyph(at,f,l->glyphs[0].glyph_index,96,&rect);
        size_t calls=a.calls;a.fail_at=0;
        if(a.failed) {
            CHECK(result==NEOX_ERROR_OUT_OF_MEMORY);CHECK(c->used==before && a.live==live);
            uint64_t hits,misses;nx_atlas_stats(at,&hits,&misses);CHECK(hits==0 && misses==0);
            OK(nx_atlas_glyph(at,f,l->glyphs[0].glyph_index,96,&rect));
        } else CHECK(result==NEOX_OK);
        CHECK(rect.w>0 && rect.h>0);OK(nx_atlas_flush(at));
        /* Distance-field padding must represent outside, including unused atlas pixels. */
        neox_texture tex=nx_atlas_texture(at);nx_tex_slot *slot=&((nx_tex_slot*)d->textures.slots)[nx_handle_index(tex.id)];
        uint8_t *px=slot->data->pixels;size_t last=(size_t)256*256*(mode==NEOX_ATLAS_MSDF?4:1)-1;
        CHECK(px[last]==(mode==NEOX_ATLAS_COVERAGE?0:255));
        nx_layout_destroy(l);nx_text_context_destroy(t);neox_device_destroy(d);
        CHECK(c->used==0);neox_context_destroy(c);CHECK(a.blocks==0 && a.live==0);
        if(!a.failed){printf("mode %d: all %zu allocation points rejected safely and retried\n",mode,calls);return;}
    }CHECK(0);
}
int main(void) {
    if(!load_fixture()){puts("SKIP: set NEOX_TEST_FONT to a TrueType fixture");return 77;}
    persistent_budget();for(int mode=0;mode<3;mode++)sweep(mode);free(font_bytes);
    puts("ALL PASS (shared budget, custom allocator, stb failures, atlas recovery)");return 0;
}
