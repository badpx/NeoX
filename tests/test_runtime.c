/* Runtime contracts, including slow asynchronous progress and exhausted pools.
 * Internal backend injection is intentional: no real GPU timing is assumed. */
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while(0)
#define OK(c) CHECK((c) == NEOX_OK)
static unsigned checks;
typedef struct { size_t bytes; int fail; } tracker;
typedef struct { size_t n, pad; } header;
static void *alloc(void *u, size_t n, size_t align) {
    tracker *t = u; header *h; (void)align;
    if (t->fail && --t->fail == 0) return NULL;
    h = malloc(sizeof(*h) + n); if (!h) return NULL;
    h->n = n; t->bytes += n; return h + 1;
}
static void release(void *u, void *p, size_t n, size_t align) {
    tracker *t = u; header *h; (void)align; if (!p) return;
    h = (header *)p - 1; CHECK(h->n == n); t->bytes -= n; free(h);
}
static neox_frame *frame(neox_device *d, uint8_t *px) {
    neox_target_desc td = {0}; neox_frame *fr = NULL;
    static const neox_color clear = {0,0,0,0};
    td.soft.pixels = px; td.soft.width = td.soft.height = 2;
    td.soft.stride = 8; td.clear = &clear; OK(neox_frame_begin(d,&td,&fr)); return fr;
}
static neox_texture texture(neox_device *d) {
    static const uint8_t red[] = {255,0,0,255};
    neox_texture_desc td = {1,1,NEOX_FORMAT_RGBA8_SRGB,red,4}; neox_texture t;
    OK(neox_texture_create(d,&td,&t)); return t;
}
static int executions, ready;
static neox_result delayed_execute(neox_device *d, nx_sub_slot *sub) {
    (void)d; (void)sub; executions++; return NEOX_OK;
}
static uint32_t delayed_poll(neox_device *d) {
    if (!ready) return 0;
    for (uint32_t i=0;i<d->pending_count;i++) {
        nx_sub_slot *s=&((nx_sub_slot *)d->subs.slots)[d->pending[i]];
        if (s->started && s->status==NEOX_SUBMISSION_PENDING) {
            s->status=NEOX_SUBMISSION_COMPLETE; nx_sub_release_payload(d,s); return 1;
        }
    }
    return 0;
}
int main(void) {
    tracker tr={0}; neox_allocator al={&tr,alloc,release};
    neox_context_desc cd={&al,0,0}; neox_context *ctx,*other;
    neox_device *d,*d2; neox_submission sub, stale_sub;
    uint8_t px[16]={0}, blue[]={0,0,255,255};
    OK(neox_context_create(&cd,&ctx)); OK(neox_device_create(ctx,NULL,&d));
    OK(neox_context_create(NULL,&other)); OK(neox_device_create(other,NULL,&d2));
    neox_texture old=texture(d), foreign=texture(d2);
    CHECK(old.id!=foreign.id);
    CHECK(neox_texture_update(d2,old,(neox_rect){0,0,1,1},blue,4)==NEOX_ERROR_OWNERSHIP_VIOLATION);
    neox_context_destroy(other);
    neox_texture_release(d,old); neox_device_pump(d);
    neox_texture current=texture(d); CHECK(old.id!=current.id);
    CHECK(neox_texture_update(d,old,(neox_rect){0,0,1,1},blue,4)==NEOX_ERROR_STALE_HANDLE);
    /* Release during recording + pump cannot reclaim a command's resource. */
    neox_frame *fr=frame(d,px);
    OK(neox_draw_texture(fr,current,(neox_rect){0,0,1,1},(neox_rect){0,0,2,2},NEOX_FILTER_NEAREST,1));
    tr.fail=1; neox_texture_release(d,current); CHECK(tr.fail==1); tr.fail=0;
    neox_device_pump(d);
    OK(neox_frame_end(fr,&sub)); OK(neox_submission_wait(d,sub));
    CHECK(px[0]==255 && px[2]==0); neox_submission_release(d,sub); neox_device_pump(d);
    stale_sub=sub;
    size_t before=tr.bytes;
    for (int i=0;i<1000;i++) {
        fr=frame(d,px); OK(neox_frame_end(fr,&sub)); OK(neox_submission_wait(d,sub));
        neox_submission_release(d,sub);
    }
    CHECK(tr.bytes==before); CHECK(d->subs.count==1);
    CHECK(neox_submission_wait(d,stale_sub)==NEOX_ERROR_STALE_HANDLE);
    /* Async execute must run once; releasing a pending handle is nonblocking. */
    const nx_backend *original=d->vt; nx_backend mock=*original;
    mock.execute=delayed_execute; mock.poll=delayed_poll; d->vt=&mock;
    fr=frame(d,px); OK(neox_frame_end(fr,&sub)); neox_device_pump(d);
    neox_device_pump(d); neox_device_pump(d); CHECK(executions==1);
    neox_submission_release(d,sub); CHECK(neox_submission_wait(d,sub)==NEOX_ERROR_STALE_HANDLE);
    ready=1; neox_device_pump(d); CHECK(d->pending_count==0); d->vt=original;
    /* A backend failure is returned by wait, not just the status query. */
    fr=frame(d,px); OK(neox_frame_end(fr,&sub)); tr.fail=1;
    CHECK(neox_submission_wait(d,sub)==NEOX_ERROR_OUT_OF_MEMORY);
    CHECK(neox_submission_get_status(d,sub)==NEOX_SUBMISSION_FAILED);
    neox_submission_release(d,sub);
    /* Generation wrap retires a slot instead of resurrecting its first handle. */
    current=texture(d); uint32_t idx=nx_handle_index(current.id);
    nx_tex_slot *slot=&((nx_tex_slot *)d->textures.slots)[idx];
    slot->gen=0xffffu; current.id=nx_make_handle(d->id,NX_TYPE_TEXTURE,idx,slot->gen);
    neox_texture_release(d,current); neox_device_pump(d);
    current=texture(d); CHECK(nx_handle_index(current.id)!=idx);
    neox_texture_release(d,current); neox_device_pump(d);
    /* Each allocation failure while constructing a nested geometric clip must
     * leave the command/state counts intact and free exact allocation sizes. */
    uint8_t verbs[]={0,1,1,1,4};float points[]={.5f,0,1.5f,0,1.5f,2,.5f,2};
    neox_path_desc path_desc={verbs,points,5,4,NEOX_FILL_NONZERO};neox_path clip;
    OK(neox_path_create(d,&path_desc,&clip));
    neox_state clip_state={0};clip_state.transform=nx_affine_identity();clip_state.opacity=1;clip_state.clip=&clip;
    int completed=0;
    for (int nth=1;nth<40;nth++) {
        fr=frame(d,px);OK(neox_push_state(fr,&clip_state));
        uint32_t commands=fr->cmd_count,states=fr->ctm_count;
        tr.fail=nth;neox_result result=neox_push_state(fr,&clip_state);tr.fail=0;
        if(result!=NEOX_OK) {
            CHECK(result==NEOX_ERROR_OUT_OF_MEMORY);
            CHECK(fr->cmd_count==commands && fr->ctm_count==states);
        } else completed=1;
        neox_frame_cancel(fr);
        if(completed)break;
    }
    CHECK(completed);neox_path_release(d,clip);neox_device_pump(d);
    /* Queued work is cancelled on destroy, never written to a borrowed target. */
    memset(px,0x7b,sizeof(px)); fr=frame(d,px); OK(neox_frame_end(fr,&sub));
    neox_context_destroy(ctx); CHECK(px[0]==0x7b); CHECK(tr.bytes==0);
    printf("ALL PASS (%u runtime checks)\n",checks); return 0;
}
