/* Backend-neutral device, submission and deferred-resource lifecycle. */
#include "internal.h"

void neox_device_destroy(neox_device *dev)
{
    neox_context *ctx;
    neox_device **link;
    uint32_t i;
    if (!dev) return;
    ctx = dev->ctx;
#if NEOX_ENABLE_CAPTURE
    if (dev->cap) neox_capture_cancel(dev->cap);
#endif

    if (dev->active_frame) {
        nx_frame_destroy(dev, dev->active_frame);
        dev->active_frame = NULL;
    }
    /* Cancel work that has never reached the backend. Only submitted work
     * needs draining; cancellation must not dereference borrowed targets. */
    for (i = 0; i < dev->pending_count; i++) {
        nx_sub_slot *sub = &((nx_sub_slot *)dev->subs.slots)[dev->pending[i]];
        if (!sub->started) {
            sub->status = NEOX_SUBMISSION_FAILED;
            sub->result = NEOX_ERROR_DEVICE_LOST;
            nx_sub_release_payload(dev, sub);
        }
    }
    while (dev->pending_count) {
        neox_device_pump(dev);
        if (dev->pending_count && dev->vt->block) dev->vt->block(dev);
    }
    if (dev->owner_detach) {
        dev->owner_detach(dev->owner);
        dev->owner_detach = NULL; dev->owner = NULL;
    }
    /* 释放全部槽位内容 */
    for (i = 0; i < dev->textures.count; i++) {
        nx_tex_slot *s = &((nx_tex_slot *)dev->textures.slots)[i];
        nx_tex_data_release(ctx,s->data);
    }
    for (i = 0; i < dev->paths.count; i++) {
        nx_path_slot *s = &((nx_path_slot *)dev->paths.slots)[i];
        if (s->verbs || s->points) {
            if (s->verbs)  nx_free(ctx, s->verbs, s->verb_count, 4);
            if (s->points) nx_free(ctx, s->points, (size_t)s->point_count * 8, 16);
        }
    }
    for (i = 0; i < dev->nodes.count; i++) {
        nx_node_slot *s = &((nx_node_slot *)dev->nodes.slots)[i];
        if (s->in_use && s->children)
            nx_free(ctx, s->children, (size_t)s->child_cap * 4, 16);
        if (s->in_use && s->kind == NEOX_NODE_GLYPHS && s->u.glyphs.glyphs)
            nx_free(ctx, s->u.glyphs.glyphs, (size_t)s->u.glyphs.count * sizeof(neox_glyph), 16);
    }
    for (i = 0; i < dev->subs.count; i++) {
        nx_sub_slot *s = &((nx_sub_slot *)dev->subs.slots)[i];
        if (s->in_use && s->cmds)
            nx_free(ctx, s->cmds, (size_t)s->cmd_cap * sizeof(nx_cmd), 16);
        if (s->in_use && s->accum)
            nx_free(ctx, s->accum, (size_t)s->width * s->height * 4 * sizeof(float), 16);
    }
    nx_table_destroy(ctx, &dev->textures);
    nx_table_destroy(ctx, &dev->paths);
    nx_table_destroy(ctx, &dev->scenes);
    nx_table_destroy(ctx, &dev->nodes);
    nx_table_destroy(ctx, &dev->subs);
    if (dev->vt && dev->vt->shutdown)
        dev->vt->shutdown(dev); /* 后端状态（GL 对象、上下文）随设备销毁 */
    if (dev->pending) nx_free(ctx, dev->pending, (size_t)dev->pending_cap * 4, 16);
    /* 从 context 链表摘除 */
    link = &ctx->devices;
    while (*link && *link != dev) link = &(*link)->next;
    if (*link) *link = dev->next;
    nx_free(ctx, dev, sizeof(neox_device), 16);
}

void neox_device_get_caps(neox_device *dev, neox_caps *out_caps)
{
    if (!out_caps) return;
    memset(out_caps, 0, sizeof(*out_caps));
    if (!dev || !dev->vt) return;
    dev->vt->get_caps(dev, out_caps);
}

int neox_device_is_lost(neox_device *dev)
{
    return dev && dev->loss_reason != NULL;
}

neox_result nx_device_mark_lost(neox_device *dev, const char *reason)
{
    if (!dev->loss_reason)
        dev->loss_reason = reason ? reason : "backend reported device loss";
    dev->ctx->last_error = dev->loss_reason;
    return NEOX_ERROR_DEVICE_LOST;
}

neox_result nx_device_check_live(neox_device *dev)
{
    if (!dev->loss_reason) return NEOX_OK;
    dev->ctx->last_error = dev->loss_reason;
    return NEOX_ERROR_DEVICE_LOST;
}

void nx_submission_complete(neox_device *dev, nx_sub_slot *sub, neox_result result)
{
    if (result == NEOX_ERROR_DEVICE_LOST)
        nx_device_mark_lost(dev, NULL);
    if (dev->loss_reason) result = NEOX_ERROR_DEVICE_LOST;
    sub->result = result;
    sub->status = result == NEOX_OK ? NEOX_SUBMISSION_COMPLETE : NEOX_SUBMISSION_FAILED;
    nx_sub_release_payload(dev, sub);
}

void nx_reap_resources(neox_device *dev)
{
    /* Intrusive deferred queue: releasing an object never allocates memory. */
    {
        uint32_t *link = &dev->defer_head;
        while (*link) {
            uint32_t type = *link >> 24, idx = *link & 0xffffffu;
            uint32_t *next, rc;
            if (type == NX_TYPE_TEXTURE) {
                nx_tex_slot *s = &((nx_tex_slot *)dev->textures.slots)[idx];
                rc = s->refcount; next = &s->defer_next;
            } else {
                nx_path_slot *s = &((nx_path_slot *)dev->paths.slots)[idx];
                rc = s->refcount; next = &s->defer_next;
            }
            if (!rc) {
                *link = *next;
                nx_slot_recycle(dev, type, idx);
            } else link = next;
        }
    }
}

uint32_t neox_device_pump(neox_device *dev)
{
    uint32_t done = 0, i, out;
    if (!dev) return 0;
    /* 异步后端（Metal）完成进度：completedHandler 只登记事件，状态推进、
     * 载荷释放与延迟回收全部发生在本调用线程（核心线程模型不变） */
    if (dev->vt->poll) done += dev->vt->poll(dev);
    /* 同步后端（poll == NULL，Soft/GL）：execute 返回即由 pump 标记终态。
     * 异步后端（Metal）：execute 只提交命令缓冲并保持 PENDING；编码期失败
     * 由 execute 自行置 FAILED（此处收割），成功路径待 poll 在 GPU 完成时
     * 做 resolve + 状态推进。 */
    for (i = 0; i < dev->pending_count; i++) {
        nx_sub_slot *sub = &((nx_sub_slot *)dev->subs.slots)[dev->pending[i]];
        neox_result r;
        if (sub->status != NEOX_SUBMISSION_PENDING || sub->started) continue;
        if (dev->loss_reason) {
            nx_submission_complete(dev, sub, NEOX_ERROR_DEVICE_LOST);
            done++;
            continue;
        }
        sub->started = 1;
        nx_memory_probe probe;
        nx_memory_probe_begin(dev->ctx, &probe);
        r = dev->vt->execute(dev, sub);
        nx_memory_probe_end(dev->ctx, &probe, &sub->report);
        sub->result = r;
        if (dev->vt->poll == NULL || r != NEOX_OK) {
            nx_submission_complete(dev, sub, r);
            done++;
        } else if (sub->status != NEOX_SUBMISSION_PENDING) {
            nx_sub_release_payload(dev, sub); /* 异步编码期失败（已置 FAILED） */
            done++;
        }
        /* else：真异步在途，保持 PENDING */
    }
    if (dev->vt->poll) done += dev->vt->poll(dev);
    /* 压缩 pending：仅保留仍在途的提交 */
    out = 0;
    for (i = 0; i < dev->pending_count; i++) {
        uint32_t sidx = dev->pending[i];
        nx_sub_slot *sub = &((nx_sub_slot *)dev->subs.slots)[sidx];
        if (sub->status == NEOX_SUBMISSION_PENDING)
            dev->pending[out++] = sidx;
        else if (sub->released)
            nx_table_release(dev->ctx, &dev->subs, sidx);
    }
    dev->pending_count = out;
    nx_reap_resources(dev);
    return done;
}

neox_submission_status neox_submission_get_status(neox_device *dev,
                                                  neox_submission sub)
{
    uint32_t idx;
    if (!dev) return NEOX_SUBMISSION_FAILED;
    if (nx_resolve(dev, sub.id, NX_TYPE_SUB, &idx) != NEOX_OK)
        return NEOX_SUBMISSION_FAILED;
    return ((nx_sub_slot *)dev->subs.slots)[idx].status;
}

neox_result neox_submission_wait(neox_device *dev, neox_submission sub)
{
    uint32_t idx;
    neox_result r;
    nx_sub_slot *s;
    if (!dev) return NEOX_ERROR_INVALID_ARGUMENT;
    r = nx_resolve(dev, sub.id, NX_TYPE_SUB, &idx);
    if (r != NEOX_OK) return r;
    for (;;) {
        s = &((nx_sub_slot *)dev->subs.slots)[idx];
        if (s->status != NEOX_SUBMISSION_PENDING) break;
        neox_device_pump(dev);
        if (s->status == NEOX_SUBMISSION_PENDING && dev->vt->block)
            dev->vt->block(dev);
    }
    return s->status == NEOX_SUBMISSION_COMPLETE ? NEOX_OK
         : (s->result != NEOX_OK ? s->result : NEOX_ERROR_BACKEND);
}

void neox_submission_release(neox_device *dev, neox_submission sub)
{
    uint32_t idx;
    nx_sub_slot *s;
    if (!dev || nx_resolve(dev, sub.id, NX_TYPE_SUB, &idx) != NEOX_OK) return;
    s = &((nx_sub_slot *)dev->subs.slots)[idx];
    s->in_use = 0;
    s->released = 1;
    if (s->status != NEOX_SUBMISSION_PENDING) {
        uint32_t out = 0;
        for (uint32_t i = 0; i < dev->pending_count; i++)
            if (dev->pending[i] != idx) dev->pending[out++] = dev->pending[i];
        dev->pending_count = out;
        nx_table_release(dev->ctx, &dev->subs, idx);
    }
}

neox_result neox_submission_get_report(neox_device *dev, neox_submission sub,
                                       neox_exec_report *out_report)
{
    uint32_t idx;
    neox_result r;
    if (!dev) return NEOX_ERROR_INVALID_ARGUMENT;
    if (!out_report) {
        dev->ctx->last_error = "out_report is NULL";
        return NEOX_ERROR_INVALID_ARGUMENT;
    }
    r = nx_resolve(dev, sub.id, NX_TYPE_SUB, &idx);
    if (r != NEOX_OK) return r;
    *out_report = ((nx_sub_slot *)dev->subs.slots)[idx].report;
    return NEOX_OK;
}
