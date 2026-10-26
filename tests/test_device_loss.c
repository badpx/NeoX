#include "internal.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define OK(x) CHECK((x) == NEOX_OK)

static neox_result injected;
static int executions, blocks;
static uint32_t ready_index;
static const nx_backend *soft;
static neox_result execute(neox_device *dev, nx_sub_slot *sub)
{
    executions++;
    return injected != NEOX_OK ? injected : soft->execute(dev, sub);
}
static neox_result async_execute(neox_device *dev, nx_sub_slot *sub)
{
    (void)dev; (void)sub; executions++;
    return NEOX_OK;
}
static uint32_t poll(neox_device *dev)
{
    if (ready_index == UINT32_MAX) return 0;
    nx_sub_slot *sub = &((nx_sub_slot *)dev->subs.slots)[ready_index];
    CHECK(sub->started && sub->status == NEOX_SUBMISSION_PENDING);
    ready_index = UINT32_MAX;
    nx_submission_complete(dev, sub, injected);
    return 1;
}
static void block(neox_device *dev)
{
    blocks++;
    for (uint32_t i = 0; i < dev->pending_count; i++) {
        uint32_t index = dev->pending[i];
        nx_sub_slot *sub = &((nx_sub_slot *)dev->subs.slots)[index];
        if (sub->started && sub->status == NEOX_SUBMISSION_PENDING) {
            ready_index = index;
            return;
        }
    }
    CHECK(0);
}
static neox_target_desc target(uint8_t pixels[4])
{
    static const neox_color clear = {0};
    neox_target_desc td = {0};
    td.soft.pixels = pixels; td.soft.width = td.soft.height = 1;
    td.soft.stride = 4; td.clear = &clear;
    return td;
}
static neox_frame *begin(neox_device *dev, uint8_t pixels[4], neox_texture texture)
{
    neox_target_desc td = target(pixels);
    neox_frame *frame;
    OK(neox_frame_begin(dev, &td, &frame));
    OK(neox_draw_texture(frame, texture, (neox_rect){0,0,1,1},
                         (neox_rect){0,0,1,1}, NEOX_FILTER_NEAREST, 1));
    return frame;
}
static neox_texture texture(neox_device *dev)
{
    uint8_t red[] = {255,0,0,255};
    neox_texture_desc desc = {1,1,NEOX_FORMAT_RGBA8_SRGB,red,4};
    neox_texture result;
    OK(neox_texture_create(dev, &desc, &result));
    return result;
}
static void synchronous(neox_result error)
{
    neox_context *ctx; neox_device *dev;
    OK(neox_context_create(NULL, &ctx)); OK(neox_device_create(ctx, NULL, &dev));
    soft = dev->vt;
    nx_backend mock = *soft; mock.execute = execute; dev->vt = &mock;
    uint8_t pixels[4] = {17,17,17,17};
    neox_texture tex = texture(dev);
    neox_submission first, second, invalid, completed;
    injected = NEOX_OK;
    OK(neox_frame_end(begin(dev, pixels, tex), &completed));
    OK(neox_submission_wait(dev, completed));
    memset(pixels, 17, sizeof(pixels));
    OK(neox_frame_end(begin(dev, pixels, tex), &first));
    OK(neox_frame_end(begin(dev, pixels, tex), &second));
    neox_capture *cap; OK(neox_capture_begin(dev, &cap));
    neox_frame *active = begin(dev, pixels, tex);
    neox_texture_release(dev, tex);
    injected = error; executions = 0;
    CHECK(neox_submission_wait(dev, first) == error);
    CHECK(neox_submission_wait(dev, second) == error);
    CHECK(pixels[0] == 17 && pixels[3] == 17);
    CHECK(neox_device_is_lost(dev) == (error == NEOX_ERROR_DEVICE_LOST));
    CHECK(neox_submission_get_status(dev, completed) == NEOX_SUBMISSION_COMPLETE);
    OK(neox_submission_wait(dev, completed));
    neox_submission_release(dev, completed);
    CHECK(executions == (error == NEOX_ERROR_DEVICE_LOST ? 1 : 2));
    if (error == NEOX_ERROR_DEVICE_LOST) {
        CHECK(neox_frame_end(active, &invalid) == NEOX_ERROR_DEVICE_LOST);
        CHECK(invalid.id == 0);
    }
    neox_frame_cancel(active);
    uint8_t *data; size_t size;
    OK(neox_capture_end(cap, &data, &size)); CHECK(size == 16);
    neox_capture_free_buffer(ctx, data, size);
    neox_submission_release(dev, first); neox_submission_release(dev, second);
    neox_device_pump(dev);
    neox_frame *frame = NULL; neox_target_desc td = target(pixels);
    injected = NEOX_OK;
    if (error == NEOX_ERROR_DEVICE_LOST) {
        size_t before = ctx->used;
        CHECK(neox_frame_begin(dev, &td, &frame) == NEOX_ERROR_DEVICE_LOST);
        CHECK(!frame && ctx->used == before);
        CHECK(neox_device_is_lost(dev));
    } else {
        OK(neox_frame_begin(dev, &td, &frame));
        OK(neox_frame_end(frame, &first)); OK(neox_submission_wait(dev, first));
        neox_submission_release(dev, first);
    }
    /* Recovery is explicit: another device in the same context is healthy. */
    neox_device *fresh; OK(neox_device_create(ctx, NULL, &fresh));
    CHECK(!neox_device_is_lost(fresh));
    tex = texture(fresh);
    OK(neox_frame_end(begin(fresh, pixels, tex), &first));
    OK(neox_submission_wait(fresh, first)); CHECK(pixels[0] == 255 && pixels[3] == 255);
    neox_device_destroy(fresh); neox_device_destroy(dev);
    CHECK(ctx->used == 0); neox_context_destroy(ctx);
}
static void asynchronous(int destroy_inflight)
{
    neox_context *ctx; neox_device *dev;
    OK(neox_context_create(NULL, &ctx)); OK(neox_device_create(ctx, NULL, &dev));
    nx_backend mock = *dev->vt;
    mock.execute = async_execute; mock.poll = poll; mock.block = block; dev->vt = &mock;
    executions = blocks = 0; ready_index = UINT32_MAX;
    uint8_t pixels[4] = {23,23,23,23};
    neox_texture tex = texture(dev);
    uint32_t tindex = nx_handle_index(tex.id);
    neox_submission first, inflight, queued;
    OK(neox_frame_end(begin(dev, pixels, tex), &first));
    OK(neox_frame_end(begin(dev, pixels, tex), &inflight));
    neox_device_pump(dev); CHECK(executions == 2);
    OK(neox_frame_end(begin(dev, pixels, tex), &queued));
    neox_texture_release(dev, tex);
    ready_index = nx_handle_index(first.id); injected = NEOX_ERROR_DEVICE_LOST;
    CHECK(neox_device_pump(dev) == 2); /* failed native work + cancelled queue */
    CHECK(neox_device_is_lost(dev) && executions == 2);
    CHECK(neox_submission_get_status(dev, inflight) == NEOX_SUBMISSION_PENDING);
    CHECK(neox_submission_wait(dev, queued) == NEOX_ERROR_DEVICE_LOST);
    nx_tex_slot *slot = &((nx_tex_slot *)dev->textures.slots)[tindex];
    CHECK(slot->refcount == 1 && slot->data); /* GPU still owns this version */
    neox_submission_release(dev, first); neox_submission_release(dev, queued);
    injected = NEOX_OK; /* Even a later native success cannot revive the device. */
    if (!destroy_inflight) {
        CHECK(neox_submission_wait(dev, inflight) == NEOX_ERROR_DEVICE_LOST);
        CHECK(!slot->in_use && !slot->data && dev->textures.free_head == tindex);
    }
    neox_submission_release(dev, inflight);
    neox_device_destroy(dev); /* drain only started work; no resubmission */
    CHECK(blocks == 1 && executions == 2 && pixels[0] == 23);
    CHECK(ctx->used == 0); neox_context_destroy(ctx);
}
int main(void)
{
    synchronous(NEOX_ERROR_DEVICE_LOST);
    synchronous(NEOX_ERROR_OUT_OF_MEMORY);
    synchronous(NEOX_ERROR_BACKEND);
    asynchronous(0); asynchronous(1);
    puts("ALL PASS (terminal loss, recoverable failure, queue cancellation, inflight drain, recreation)");
    return 0;
}
