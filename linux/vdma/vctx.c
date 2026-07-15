// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2025 Hailo Technologies Ltd. All rights reserved.
 **/

#define pr_fmt(fmt) "hailo: " fmt

#include "vctx.h"
#include "memory.h"
#include "utils/logs.h"

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define HAILO_VDMA_CHANNEL_TRANSFER_CAPACITY (HAILO_VDMA_MAX_ONGOING_TRANSFERS - 1)

static bool vctx_trace_enabled;
module_param_named(vctx_trace, vctx_trace_enabled, bool, 0644);
MODULE_PARM_DESC(vctx_trace, "Enable Hailo VCTX lifecycle tracing in dmesg");

#define VCTX_TRACE(fmt, ...) \
    do { \
        if (unlikely(READ_ONCE(vctx_trace_enabled))) \
            pr_info("vctx-trace: " fmt, ##__VA_ARGS__); \
    } while (0)

enum hailo_vdma_transfer_state {
    HAILO_VDMA_TRANSFER_NEW = 0,
    HAILO_VDMA_TRANSFER_WAITING,
    HAILO_VDMA_TRANSFER_ADMITTED,
    HAILO_VDMA_TRANSFER_COMMITTED,
    HAILO_VDMA_TRANSFER_COMPLETING,
    HAILO_VDMA_TRANSFER_COMPLETED,
    HAILO_VDMA_TRANSFER_CANCELED,
    HAILO_VDMA_TRANSFER_ABORTING,
    HAILO_VDMA_TRANSFER_ABORTED,
};

struct hailo_vdma_transfer {
    struct list_head admission_node;
    struct list_head vctx_node;
    struct list_head completed_node;
    struct hailo_vdma_vctx *vctx;
    struct hailo_descriptors_list_buffer *descriptors;
    struct hailo_vdma_mapped_transfer_buffer buffers[HAILO_MAX_BUFFERS_PER_SINGLE_TRANSFER];
    enum hailo_vdma_transfer_state state;
    u64 sequence;
    u64 generation;
    u8 engine_index;
    u8 channel_index;
    u8 buffers_count;
    u32 starting_desc;
    u32 programmed_descs;
    int status;
    bool cancel_requested;
    bool abort_requested;
    bool event_delivered;
    bool resources_held;
    bool quota_charged;
};

struct hailo_vdma_completion_context {
    struct hailo_vdma_controller *controller;
    bool publish_event;
};

void hailo_vdma_vctx_trace_created(struct hailo_vdma_vctx *vctx)
{
    VCTX_TRACE("VCTX_CREATE vctx=%llu generation=%llu quota=%u\n",
        (unsigned long long)vctx->vctx_id,
        (unsigned long long)vctx->generation, vctx->transfer_quota);
}

static bool vctx_is_active(struct hailo_vdma_vctx *vctx)
{
    unsigned long flags;
    bool active;

    spin_lock_irqsave(&vctx->lock, flags);
    active = (vctx->state == HAILO_VDMA_VCTX_ACTIVE) && !vctx->cancel_requested;
    spin_unlock_irqrestore(&vctx->lock, flags);
    return active;
}

static bool vctx_generation_is_active(struct hailo_vdma_vctx *vctx, u64 generation)
{
    unsigned long flags;
    bool active;

    spin_lock_irqsave(&vctx->lock, flags);
    active = (vctx->state == HAILO_VDMA_VCTX_ACTIVE) && !vctx->cancel_requested &&
        (vctx->generation == generation);
    spin_unlock_irqrestore(&vctx->lock, flags);
    return active;
}

void hailo_vdma_vctx_get(struct hailo_vdma_vctx *vctx)
{
    kref_get(&vctx->refcount);
}

static void vctx_refs_zero(struct kref *kref)
{
    struct hailo_vdma_vctx *vctx = container_of(kref, struct hailo_vdma_vctx, refcount);

    complete(&vctx->refs_zero);
}

void hailo_vdma_vctx_put(struct hailo_vdma_vctx *vctx)
{
    kref_put(&vctx->refcount, vctx_refs_zero);
}

static void transfer_release_resources(struct hailo_vdma_transfer *transfer)
{
    u8 i;

    if (!transfer->resources_held) {
        return;
    }

    for (i = 0; i < transfer->buffers_count; i++) {
        struct hailo_vdma_buffer *buffer = transfer->buffers[i].opaque;

        hailo_vdma_buffer_put(buffer);
    }
    hailo_desc_list_put(transfer->descriptors);
    transfer->resources_held = false;
}

static void transfer_drop_quota(struct hailo_vdma_transfer *transfer)
{
    struct hailo_vdma_controller *controller;
    int new_count;
    u8 engine_index;
    u8 channel_index;

    if (!transfer->quota_charged) {
        return;
    }

    new_count = atomic_dec_return(&transfer->vctx->transfer_count);
    transfer->quota_charged = false;
    if (new_count != (int)transfer->vctx->transfer_quota - 1) {
        return;
    }

    controller = transfer->vctx->controller;
    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            wake_up_all(&controller->channel_contexts[engine_index][channel_index].admission_wq);
        }
    }
}

static void transfer_destroy(struct hailo_vdma_transfer *transfer)
{
    struct hailo_vdma_channel_context *channel_context;

    channel_context = &transfer->vctx->controller->channel_contexts
        [transfer->engine_index][transfer->channel_index];
    transfer_release_resources(transfer);
    transfer_drop_quota(transfer);
    wake_up_all(&channel_context->admission_wq);
    hailo_vdma_vctx_put(transfer->vctx);
    kfree(transfer);
}

static void transfer_remove_from_vctx(struct hailo_vdma_transfer *transfer)
{
    unsigned long flags;

    spin_lock_irqsave(&transfer->vctx->lock, flags);
    if (!list_empty(&transfer->vctx_node)) {
        list_del_init(&transfer->vctx_node);
    }
    spin_unlock_irqrestore(&transfer->vctx->lock, flags);
}

static void transfer_abort_callback(struct hailo_ongoing_transfer *ongoing, void *opaque)
{
    struct hailo_vdma_transfer *transfer = ongoing->opaque;
    struct hailo_vdma_channel_context *channel_context;

    (void)opaque;
    if (!transfer) {
        return;
    }

    channel_context = &transfer->vctx->controller->channel_contexts
        [transfer->engine_index][transfer->channel_index];
    transfer->abort_requested = true;
    transfer->state = HAILO_VDMA_TRANSFER_ABORTING;
    transfer->status = -ECANCELED;
    transfer->state = HAILO_VDMA_TRANSFER_ABORTED;
    transfer_remove_from_vctx(transfer);
    atomic_dec(&channel_context->ongoing_count);
    VCTX_TRACE("TRANSFER_ABORT vctx=%llu generation=%llu sequence=%llu engine=%u channel=%u status=%d\n",
        (unsigned long long)transfer->vctx->vctx_id,
        (unsigned long long)transfer->generation,
        (unsigned long long)transfer->sequence,
        (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
        transfer->status);
    transfer_destroy(transfer);
}

static void transfer_complete_callback(struct hailo_ongoing_transfer *ongoing, void *opaque)
{
    struct hailo_vdma_completion_context *completion = opaque;
    struct hailo_vdma_transfer *transfer = ongoing->opaque;
    struct hailo_vdma_vctx *vctx;
    struct hailo_vdma_channel_context *channel_context;
    unsigned long flags;
    bool publish;
    u32 pending_count = 0;
    u8 i;

    if (!transfer) {
        return;
    }
    if (!completion->publish_event) {
        transfer_abort_callback(ongoing, opaque);
        return;
    }

    vctx = transfer->vctx;
    channel_context = &vctx->controller->channel_contexts
        [transfer->engine_index][transfer->channel_index];
    transfer->state = HAILO_VDMA_TRANSFER_COMPLETING;
    for (i = 0; i < transfer->buffers_count; i++) {
        struct hailo_vdma_buffer *buffer = transfer->buffers[i].opaque;

        hailo_vdma_buffer_sync(completion->controller, buffer, HAILO_SYNC_FOR_CPU,
            transfer->buffers[i].offset, transfer->buffers[i].size);
    }

    spin_lock_irqsave(&vctx->lock, flags);
    publish = (vctx->state == HAILO_VDMA_VCTX_ACTIVE) &&
        (vctx->generation == transfer->generation) && !transfer->cancel_requested;
    if (!list_empty(&transfer->vctx_node)) {
        list_del_init(&transfer->vctx_node);
    }
    if (publish) {
        transfer->state = HAILO_VDMA_TRANSFER_COMPLETED;
        transfer->status = 0;
        list_add_tail(&transfer->completed_node,
            &vctx->completed_transfers[transfer->engine_index][transfer->channel_index]);
        pending_count = ++vctx->events[transfer->engine_index][transfer->channel_index].completed_count;
    } else {
        transfer->state = HAILO_VDMA_TRANSFER_ABORTED;
        transfer->status = -ECANCELED;
    }
    spin_unlock_irqrestore(&vctx->lock, flags);

    atomic_dec(&channel_context->ongoing_count);
    VCTX_TRACE("TRANSFER_COMPLETE vctx=%llu generation=%llu sequence=%llu engine=%u channel=%u published=%u status=%d pending=%u\n",
        (unsigned long long)vctx->vctx_id,
        (unsigned long long)transfer->generation,
        (unsigned long long)transfer->sequence,
        (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
        (unsigned int)publish, transfer->status, pending_count);
    transfer_release_resources(transfer);
    wake_up_all(&channel_context->admission_wq);
    if (publish) {
        wake_up_interruptible_all(&vctx->events_wq);
    } else {
        transfer_destroy(transfer);
    }
}

static void cancel_channel_admission(struct hailo_vdma_channel_context *channel_context)
{
    struct hailo_vdma_transfer *transfer;
    unsigned long flags;

    spin_lock_irqsave(&channel_context->admission_lock, flags);
    list_for_each_entry(transfer, &channel_context->admission_queue, admission_node) {
        transfer->cancel_requested = true;
        transfer->state = HAILO_VDMA_TRANSFER_CANCELED;
    }
    spin_unlock_irqrestore(&channel_context->admission_lock, flags);
    wake_up_all(&channel_context->admission_wq);
}

static void purge_completed_channel(struct hailo_vdma_vctx *vctx, u8 engine_index,
    u8 channel_index, bool delivered)
{
    struct hailo_vdma_transfer *transfer;
    struct hailo_vdma_transfer *next;
    LIST_HEAD(release_list);
    unsigned long flags;

    spin_lock_irqsave(&vctx->lock, flags);
    list_splice_init(&vctx->completed_transfers[engine_index][channel_index], &release_list);
    vctx->events[engine_index][channel_index].completed_count = 0;
    spin_unlock_irqrestore(&vctx->lock, flags);

    list_for_each_entry_safe(transfer, next, &release_list, completed_node) {
        list_del_init(&transfer->completed_node);
        transfer->event_delivered = delivered;
        transfer_destroy(transfer);
    }
}

static void set_channel_terminal_event(struct hailo_vdma_vctx *vctx, u8 engine_index,
    u8 channel_index, u8 data)
{
    unsigned long flags;

    purge_completed_channel(vctx, engine_index, channel_index, true);
    spin_lock_irqsave(&vctx->lock, flags);
    if (data == HAILO_VDMA_TRANSFER_DATA_CHANNEL_WITH_ERROR) {
        vctx->events[engine_index][channel_index].channel_error = true;
    } else {
        vctx->events[engine_index][channel_index].channel_inactive = true;
    }
    spin_unlock_irqrestore(&vctx->lock, flags);
    wake_up_interruptible_all(&vctx->events_wq);
}

static void disable_one_channel(struct hailo_vdma_controller *controller,
    u8 engine_index, u8 channel_index, bool notify_waiter)
{
    struct hailo_vdma_channel_context *channel_context =
        &controller->channel_contexts[engine_index][channel_index];
    struct hailo_vdma_engine *engine = &controller->vdma_engines[engine_index];
    struct hailo_vdma_vctx *owner;
    struct hailo_vdma_file_context *file_context;
    unsigned long flags;
    u64 owner_generation;
    u32 channel_bit = BIT(channel_index);

    channel_context->shutting_down = true;
    cancel_channel_admission(channel_context);

    mutex_lock(&channel_context->lock);
    owner = channel_context->owner;
    if (!channel_context->enabled || !owner) {
        channel_context->shutting_down = false;
        mutex_unlock(&channel_context->lock);
        return;
    }

    hailo_vdma_engine_disable_channels_with_callback(engine, channel_bit,
        transfer_abort_callback, controller);
    if (channel_context->bound_descriptors) {
        hailo_desc_list_put(channel_context->bound_descriptors);
        channel_context->bound_descriptors = NULL;
    }
    WARN_ON_ONCE(atomic_read(&channel_context->ongoing_count) != 0);
    atomic_set(&channel_context->ongoing_count, 0);

    file_context = container_of(owner, struct hailo_vdma_file_context, vctx);
    owner_generation = channel_context->owner_generation;
    file_context->enabled_channels_bitmap[engine_index] &= ~channel_bit;
    purge_completed_channel(owner, engine_index, channel_index, true);
    spin_lock_irqsave(&owner->lock, flags);
    owner->events[engine_index][channel_index].channel_error = false;
    owner->events[engine_index][channel_index].channel_inactive = false;
    owner->events[engine_index][channel_index].disable_wakeup = notify_waiter;
    spin_unlock_irqrestore(&owner->lock, flags);

    channel_context->enabled = false;
    channel_context->owner = NULL;
    channel_context->owner_generation = 0;
    channel_context->shutting_down = false;
    mutex_unlock(&channel_context->lock);

    spin_lock_irqsave(&controller->interrupts_lock, flags);
    hailo_vdma_engine_clear_channel_interrupts(engine, channel_bit);
    spin_unlock_irqrestore(&controller->interrupts_lock, flags);
    if (controller->dev) {
        hailo_vdma_update_interrupts_mask(controller, engine_index);
    }
    if (notify_waiter) {
        wake_up_interruptible_all(&owner->events_wq);
    }
    wake_up_all(&channel_context->admission_wq);
    VCTX_TRACE("CHANNEL_DISABLE vctx=%llu generation=%llu engine=%u channel=%u notify=%u\n",
        (unsigned long long)owner->vctx_id,
        (unsigned long long)owner_generation, (unsigned int)engine_index,
        (unsigned int)channel_index, (unsigned int)notify_waiter);
    hailo_vdma_vctx_put(owner);
}

long hailo_vdma_vctx_enable_channels(struct hailo_vdma_controller *controller,
    unsigned long arg, struct hailo_vdma_file_context *context)
{
    struct hailo_vdma_enable_channels_params input;
    unsigned long flags;
    u8 engine_index;
    u8 channel_index;

    if (copy_from_user(&input, (void __user *)arg, sizeof(input))) {
        return -ENOMEM;
    }
    if (!vctx_is_active(&context->vctx)) {
        return -ECANCELED;
    }

    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        u32 bitmap = input.channels_bitmap_per_engine[engine_index];

        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            struct hailo_vdma_channel_context *channel_context;

            if (!(bitmap & BIT(channel_index))) {
                continue;
            }
            channel_context = &controller->channel_contexts[engine_index][channel_index];
            mutex_lock(&channel_context->lock);
            if (channel_context->enabled || channel_context->owner) {
                u64 owner_vctx_id = channel_context->owner ?
                    channel_context->owner->vctx_id : 0;
                u64 owner_generation = channel_context->owner_generation;
                bool enabled = channel_context->enabled;

                mutex_unlock(&channel_context->lock);
                VCTX_TRACE("CHANNEL_ENABLE_DENY requester=%llu generation=%llu owner=%llu owner_generation=%llu engine=%u channel=%u enabled=%u status=%d\n",
                    (unsigned long long)context->vctx.vctx_id,
                    (unsigned long long)context->vctx.generation,
                    (unsigned long long)owner_vctx_id,
                    (unsigned long long)owner_generation,
                    (unsigned int)engine_index, (unsigned int)channel_index,
                    (unsigned int)enabled, -EINVAL);
                return -EINVAL;
            }
            mutex_unlock(&channel_context->lock);
        }
    }

    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        struct hailo_vdma_engine *engine = &controller->vdma_engines[engine_index];
        u32 bitmap = input.channels_bitmap_per_engine[engine_index];

        hailo_vdma_engine_enable_channels(engine, bitmap, input.enable_timestamps_measure);
        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            struct hailo_vdma_channel_context *channel_context;

            if (!(bitmap & BIT(channel_index))) {
                continue;
            }
            channel_context = &controller->channel_contexts[engine_index][channel_index];
            mutex_lock(&channel_context->lock);
            spin_lock_irqsave(&context->vctx.lock, flags);
            context->vctx.events[engine_index][channel_index].channel_error = false;
            context->vctx.events[engine_index][channel_index].channel_inactive = false;
            context->vctx.events[engine_index][channel_index].disable_wakeup = false;
            spin_unlock_irqrestore(&context->vctx.lock, flags);
            channel_context->owner = &context->vctx;
            channel_context->owner_generation = context->vctx.generation;
            channel_context->enabled = true;
            channel_context->shutting_down = false;
            hailo_vdma_vctx_get(&context->vctx);
            mutex_unlock(&channel_context->lock);
            VCTX_TRACE("CHANNEL_ENABLE vctx=%llu generation=%llu engine=%u channel=%u timestamps=%u\n",
                (unsigned long long)context->vctx.vctx_id,
                (unsigned long long)context->vctx.generation,
                (unsigned int)engine_index, (unsigned int)channel_index,
                (unsigned int)input.enable_timestamps_measure);
        }
        context->enabled_channels_bitmap[engine_index] |= bitmap;
        hailo_vdma_update_interrupts_mask(controller, engine_index);
    }
    return 0;
}

long hailo_vdma_vctx_disable_channels(struct hailo_vdma_controller *controller,
    unsigned long arg, struct hailo_vdma_file_context *context)
{
    struct hailo_vdma_disable_channels_params input;
    u8 engine_index;
    u8 channel_index;

    if (copy_from_user(&input, (void __user *)arg, sizeof(input))) {
        return -ENOMEM;
    }

    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        u32 bitmap = input.channels_bitmap_per_engine[engine_index];

        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            struct hailo_vdma_channel_context *channel_context;

            if (!(bitmap & BIT(channel_index))) {
                continue;
            }
            channel_context = &controller->channel_contexts[engine_index][channel_index];
            mutex_lock(&channel_context->lock);
            if (channel_context->owner && channel_context->owner != &context->vctx) {
                u64 owner_vctx_id = channel_context->owner->vctx_id;
                u64 owner_generation = channel_context->owner_generation;

                mutex_unlock(&channel_context->lock);
                VCTX_TRACE("CHANNEL_DISABLE_DENY requester=%llu generation=%llu owner=%llu owner_generation=%llu engine=%u channel=%u status=%d\n",
                    (unsigned long long)context->vctx.vctx_id,
                    (unsigned long long)context->vctx.generation,
                    (unsigned long long)owner_vctx_id,
                    (unsigned long long)owner_generation,
                    (unsigned int)engine_index, (unsigned int)channel_index, -EPERM);
                return -EPERM;
            }
            mutex_unlock(&channel_context->lock);
        }
    }

    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        u32 bitmap = input.channels_bitmap_per_engine[engine_index];

        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            if (bitmap & BIT(channel_index)) {
                disable_one_channel(controller, engine_index, channel_index, true);
            }
        }
    }
    return 0;
}

static bool wait_has_event(struct hailo_vdma_vctx *vctx,
    const u32 channels_bitmap_per_engine[MAX_VDMA_ENGINES])
{
    unsigned long flags;
    u8 engine_index;
    u8 channel_index;
    bool ready = false;

    spin_lock_irqsave(&vctx->lock, flags);
    if (vctx->state != HAILO_VDMA_VCTX_ACTIVE) {
        ready = true;
        goto unlock;
    }
    for (engine_index = 0; engine_index < MAX_VDMA_ENGINES; engine_index++) {
        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            struct hailo_vdma_vctx_event *event;

            if (!(channels_bitmap_per_engine[engine_index] & BIT(channel_index))) {
                continue;
            }
            event = &vctx->events[engine_index][channel_index];
            if (event->completed_count || event->channel_error || event->channel_inactive ||
                event->disable_wakeup) {
                ready = true;
                goto unlock;
            }
        }
    }

unlock:
    spin_unlock_irqrestore(&vctx->lock, flags);
    return ready;
}

static void consume_completed(struct hailo_vdma_vctx *vctx, u8 engine_index,
    u8 channel_index, u32 count)
{
    struct hailo_vdma_transfer *transfer;
    struct hailo_vdma_transfer *next;
    LIST_HEAD(release_list);
    unsigned long flags;
    u32 consumed = 0;

    spin_lock_irqsave(&vctx->lock, flags);
    list_for_each_entry_safe(transfer, next,
        &vctx->completed_transfers[engine_index][channel_index], completed_node) {
        if (consumed == count) {
            break;
        }
        list_move_tail(&transfer->completed_node, &release_list);
        consumed++;
    }
    vctx->events[engine_index][channel_index].completed_count -= consumed;
    spin_unlock_irqrestore(&vctx->lock, flags);

    list_for_each_entry_safe(transfer, next, &release_list, completed_node) {
        list_del_init(&transfer->completed_node);
        transfer->event_delivered = true;
        transfer_destroy(transfer);
    }
}

static void lock_wait_channels(struct hailo_vdma_controller *controller,
    const u32 channels_bitmap_per_engine[MAX_VDMA_ENGINES])
{
    u8 engine_index;
    u8 channel_index;

    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            if (channels_bitmap_per_engine[engine_index] & BIT(channel_index)) {
                mutex_lock(&controller->channel_contexts[engine_index][channel_index].lock);
            }
        }
    }
}

static void unlock_wait_channels(struct hailo_vdma_controller *controller,
    const u32 channels_bitmap_per_engine[MAX_VDMA_ENGINES])
{
    int engine_index;
    int channel_index;

    for (engine_index = (int)controller->vdma_engines_count - 1; engine_index >= 0; engine_index--) {
        for (channel_index = MAX_VDMA_CHANNELS_PER_ENGINE - 1; channel_index >= 0; channel_index--) {
            if (channels_bitmap_per_engine[engine_index] & BIT(channel_index)) {
                mutex_unlock(&controller->channel_contexts[engine_index][channel_index].lock);
            }
        }
    }
}

long hailo_vdma_vctx_wait(struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller, unsigned long arg,
    struct semaphore *board_mutex, bool *should_up_board_mutex)
{
    struct hailo_vdma_interrupts_wait_params params = {0};
    struct hailo_vdma_vctx_event snapshots[MAX_VDMA_ENGINES][MAX_VDMA_CHANNELS_PER_ENGINE] = {{0}};
    u32 completed_to_consume[MAX_VDMA_ENGINES][MAX_VDMA_CHANNELS_PER_ENGINE] = {{0}};
    u8 engine_index;
    u8 channel_index;
    bool bitmap_not_empty = false;
    long err;

    if (copy_from_user(&params, (void __user *)arg, sizeof(params))) {
        return -ENOMEM;
    }

    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        u32 bitmap = params.channels_bitmap_per_engine[engine_index];

        bitmap_not_empty |= bitmap != 0;
        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            struct hailo_vdma_channel_context *channel_context;
            unsigned long flags;

            if (!(bitmap & BIT(channel_index))) {
                continue;
            }
            channel_context = &controller->channel_contexts[engine_index][channel_index];
            mutex_lock(&channel_context->lock);
            if (channel_context->owner && channel_context->owner != &context->vctx) {
                u64 owner_vctx_id = channel_context->owner->vctx_id;
                u64 owner_generation = channel_context->owner_generation;

                mutex_unlock(&channel_context->lock);
                VCTX_TRACE("WAIT_DENY requester=%llu generation=%llu owner=%llu owner_generation=%llu engine=%u channel=%u status=%d\n",
                    (unsigned long long)context->vctx.vctx_id,
                    (unsigned long long)context->vctx.generation,
                    (unsigned long long)owner_vctx_id,
                    (unsigned long long)owner_generation,
                    (unsigned int)engine_index, (unsigned int)channel_index, -EPERM);
                return -EPERM;
            }
            if (!channel_context->owner) {
                spin_lock_irqsave(&context->vctx.lock, flags);
                context->vctx.events[engine_index][channel_index].disable_wakeup = true;
                spin_unlock_irqrestore(&context->vctx.lock, flags);
            }
            mutex_unlock(&channel_context->lock);
        }
    }
    if (!bitmap_not_empty) {
        return -EINVAL;
    }

    up(board_mutex);
    err = wait_event_interruptible(context->vctx.events_wq,
        wait_has_event(&context->vctx, params.channels_bitmap_per_engine));
    if (err) {
        *should_up_board_mutex = false;
        return err;
    }
    if (down_interruptible(board_mutex)) {
        *should_up_board_mutex = false;
        return -ERESTARTSYS;
    }
    if (!vctx_is_active(&context->vctx)) {
        return -ECANCELED;
    }

    lock_wait_channels(controller, params.channels_bitmap_per_engine);
    params.channels_count = 0;
    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            struct hailo_vdma_vctx_event snapshot;
            unsigned long flags;
            u32 completed;

            if (!(params.channels_bitmap_per_engine[engine_index] & BIT(channel_index))) {
                continue;
            }
            spin_lock_irqsave(&context->vctx.lock, flags);
            snapshot = context->vctx.events[engine_index][channel_index];
            snapshots[engine_index][channel_index] = snapshot;
            context->vctx.events[engine_index][channel_index].channel_error = false;
            context->vctx.events[engine_index][channel_index].channel_inactive = false;
            context->vctx.events[engine_index][channel_index].disable_wakeup = false;
            spin_unlock_irqrestore(&context->vctx.lock, flags);

            if (snapshot.channel_error || snapshot.channel_inactive) {
                if (params.channels_count >= ARRAY_SIZE(params.irq_data)) {
                    err = -EINVAL;
                    goto restore_events;
                }
                params.irq_data[params.channels_count].engine_index = engine_index;
                params.irq_data[params.channels_count].channel_index = channel_index;
                params.irq_data[params.channels_count].data = snapshot.channel_error ?
                    HAILO_VDMA_TRANSFER_DATA_CHANNEL_WITH_ERROR :
                    HAILO_VDMA_TRANSFER_DATA_CHANNEL_NOT_ACTIVE;
                params.channels_count++;
                continue;
            }
            if (!snapshot.completed_count) {
                continue;
            }
            if (params.channels_count >= ARRAY_SIZE(params.irq_data)) {
                err = -EINVAL;
                goto restore_events;
            }
            completed = min_t(u32, snapshot.completed_count,
                HAILO_VDMA_TRANSFER_DATA_CHANNEL_WITH_ERROR - 1);
            params.irq_data[params.channels_count].engine_index = engine_index;
            params.irq_data[params.channels_count].channel_index = channel_index;
            params.irq_data[params.channels_count].data = (u8)completed;
            params.channels_count++;
            completed_to_consume[engine_index][channel_index] = completed;
        }
    }

    if (copy_to_user((void __user *)arg, &params, sizeof(params))) {
        err = -ENOMEM;
        goto restore_events;
    }

    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            const struct hailo_vdma_vctx_event *snapshot =
                &snapshots[engine_index][channel_index];
            u32 completed = completed_to_consume[engine_index][channel_index];
            u8 data;

            if (!(params.channels_bitmap_per_engine[engine_index] & BIT(channel_index))) {
                continue;
            }
            if (snapshot->channel_error || snapshot->channel_inactive) {
                data = snapshot->channel_error ? HAILO_VDMA_TRANSFER_DATA_CHANNEL_WITH_ERROR :
                    HAILO_VDMA_TRANSFER_DATA_CHANNEL_NOT_ACTIVE;
            } else if (completed) {
                data = (u8)completed;
            } else if (snapshot->disable_wakeup) {
                data = 0;
            } else {
                continue;
            }
            VCTX_TRACE("WAIT_EVENT vctx=%llu generation=%llu engine=%u channel=%u data=%u disabled=%u\n",
                (unsigned long long)context->vctx.vctx_id,
                (unsigned long long)context->vctx.generation,
                (unsigned int)engine_index, (unsigned int)channel_index,
                (unsigned int)data, (unsigned int)snapshot->disable_wakeup);
        }
    }
    VCTX_TRACE("WAIT_DELIVER vctx=%llu generation=%llu channels=%u\n",
        (unsigned long long)context->vctx.vctx_id,
        (unsigned long long)context->vctx.generation,
        (unsigned int)params.channels_count);

    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            u32 completed = completed_to_consume[engine_index][channel_index];

            if (completed) {
                consume_completed(&context->vctx, engine_index, channel_index, completed);
            }
        }
    }
    unlock_wait_channels(controller, params.channels_bitmap_per_engine);
    return 0;

restore_events:
    {
        unsigned long flags;

        spin_lock_irqsave(&context->vctx.lock, flags);
        for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
            for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
                struct hailo_vdma_vctx_event *event =
                    &context->vctx.events[engine_index][channel_index];
                const struct hailo_vdma_vctx_event *snapshot =
                    &snapshots[engine_index][channel_index];

                event->channel_error |= snapshot->channel_error;
                event->channel_inactive |= snapshot->channel_inactive;
                event->disable_wakeup |= snapshot->disable_wakeup;
            }
        }
        spin_unlock_irqrestore(&context->vctx.lock, flags);
        unlock_wait_channels(controller, params.channels_bitmap_per_engine);
        wake_up_interruptible_all(&context->vctx.events_wq);
    }
    VCTX_TRACE("WAIT_ROLLBACK vctx=%llu generation=%llu status=%ld\n",
        (unsigned long long)context->vctx.vctx_id,
        (unsigned long long)context->vctx.generation, err);
    return err;
}

static bool admission_ready(struct hailo_vdma_transfer *transfer,
    struct hailo_vdma_channel_context *channel_context)
{
    unsigned long flags;
    bool ready;

    spin_lock_irqsave(&channel_context->admission_lock, flags);
    ready = transfer->cancel_requested || transfer->abort_requested ||
        !vctx_generation_is_active(transfer->vctx, transfer->generation) ||
        channel_context->shutting_down ||
        channel_context->owner != transfer->vctx || !channel_context->enabled;
    if (!ready && !list_empty(&channel_context->admission_queue) &&
        list_first_entry(&channel_context->admission_queue,
            struct hailo_vdma_transfer, admission_node) == transfer &&
        atomic_read(&channel_context->ongoing_count) < HAILO_VDMA_CHANNEL_TRANSFER_CAPACITY &&
        atomic_read(&transfer->vctx->transfer_count) < transfer->vctx->transfer_quota) {
        ready = true;
    }
    spin_unlock_irqrestore(&channel_context->admission_lock, flags);
    return ready;
}

static void cancel_waiting_transfer(struct hailo_vdma_transfer *transfer,
    struct hailo_vdma_channel_context *channel_context)
{
    unsigned long flags;

    spin_lock_irqsave(&channel_context->admission_lock, flags);
    if (!list_empty(&transfer->admission_node)) {
        list_del_init(&transfer->admission_node);
    }
    transfer->cancel_requested = true;
    transfer->state = HAILO_VDMA_TRANSFER_CANCELED;
    transfer->status = -ECANCELED;
    spin_unlock_irqrestore(&channel_context->admission_lock, flags);
    transfer_remove_from_vctx(transfer);
    wake_up_all(&channel_context->admission_wq);
    VCTX_TRACE("TRANSFER_CANCEL vctx=%llu generation=%llu sequence=%llu engine=%u channel=%u status=%d stage=wait\n",
        (unsigned long long)transfer->vctx->vctx_id,
        (unsigned long long)transfer->generation,
        (unsigned long long)transfer->sequence,
        (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
        transfer->status);
    transfer_destroy(transfer);
}

static int transfer_acquire_resources(struct hailo_vdma_transfer *transfer,
    struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller,
    const struct hailo_vdma_launch_transfer_params *params)
{
    enum dma_data_direction direction;
    u8 i;

    transfer->descriptors = hailo_vdma_find_descriptors_buffer(context, params->desc_handle);
    if (!transfer->descriptors || transfer->descriptors->owner != context) {
        return -EFAULT;
    }
    hailo_desc_list_get(transfer->descriptors);
    transfer->resources_held = true;
    direction = hailo_test_bit(params->channel_index, &controller->hw->src_channels_bitmask) ?
        DMA_TO_DEVICE : DMA_FROM_DEVICE;

    for (i = 0; i < params->buffers_count; i++) {
        struct hailo_vdma_buffer *buffer;
        u32 offset = 0;

        if (params->buffers[i].buffer_type == HAILO_DMA_USER_PTR_BUFFER) {
            buffer = hailo_vdma_find_mapped_buffer_by_address(context,
                params->buffers[i].addr_or_fd, params->buffers[i].size, direction);
            if (buffer) {
                offset = (u32)(params->buffers[i].addr_or_fd - buffer->addr_or_fd);
            }
        } else if (params->buffers[i].buffer_type == HAILO_DMA_DMABUF_BUFFER) {
            buffer = hailo_vdma_find_mapped_buffer_by_fd(context,
                params->buffers[i].addr_or_fd, params->buffers[i].size, direction);
        } else {
            buffer = NULL;
        }
        if (!buffer || buffer->owner != context) {
            transfer->buffers_count = i;
            transfer_release_resources(transfer);
            return -EFAULT;
        }
        hailo_vdma_buffer_get(buffer);
        transfer->buffers[i].sg_table = &buffer->sg_table;
        transfer->buffers[i].size = params->buffers[i].size;
        transfer->buffers[i].offset = offset;
        transfer->buffers[i].opaque = buffer;
        transfer->buffers_count = i + 1;
    }
    return 0;
}

long hailo_vdma_vctx_launch(struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller, unsigned long arg,
    struct semaphore *board_mutex, bool *should_up_board_mutex)
{
    struct hailo_vdma_launch_transfer_params params;
    struct hailo_vdma_transfer *transfer;
    struct hailo_vdma_channel_context *channel_context;
    struct hailo_vdma_channel *channel;
    unsigned long flags;
    long err;
    u8 i;

    if (copy_from_user(&params, (void __user *)arg, sizeof(params))) {
        return -EFAULT;
    }
    if (!vctx_is_active(&context->vctx)) {
        return -ECANCELED;
    }
    if (params.engine_index >= controller->vdma_engines_count ||
        params.channel_index >= MAX_VDMA_CHANNELS_PER_ENGINE ||
        params.buffers_count == 0 || params.buffers_count > ARRAY_SIZE(params.buffers)) {
        return -EINVAL;
    }
    for (i = 0; i < params.buffers_count; i++) {
        if (params.buffers[i].size == 0) {
            return -EINVAL;
        }
    }

    channel_context = &controller->channel_contexts[params.engine_index][params.channel_index];
    mutex_lock(&channel_context->lock);
    if (!channel_context->enabled || channel_context->owner != &context->vctx ||
        channel_context->owner_generation != context->vctx.generation) {
        u64 owner_vctx_id = channel_context->owner ? channel_context->owner->vctx_id : 0;
        u64 owner_generation = channel_context->owner_generation;
        bool enabled = channel_context->enabled;

        mutex_unlock(&channel_context->lock);
        VCTX_TRACE("TRANSFER_DENY requester=%llu generation=%llu owner=%llu owner_generation=%llu engine=%u channel=%u enabled=%u status=%d stage=lease\n",
            (unsigned long long)context->vctx.vctx_id,
            (unsigned long long)context->vctx.generation,
            (unsigned long long)owner_vctx_id,
            (unsigned long long)owner_generation,
            (unsigned int)params.engine_index, (unsigned int)params.channel_index,
            (unsigned int)enabled, -EPERM);
        return -EPERM;
    }
    mutex_unlock(&channel_context->lock);

    transfer = kzalloc(sizeof(*transfer), GFP_KERNEL);
    if (!transfer) {
        return -ENOMEM;
    }
    INIT_LIST_HEAD(&transfer->admission_node);
    INIT_LIST_HEAD(&transfer->vctx_node);
    INIT_LIST_HEAD(&transfer->completed_node);
    transfer->vctx = &context->vctx;
    transfer->engine_index = params.engine_index;
    transfer->channel_index = params.channel_index;
    transfer->generation = context->vctx.generation;
    transfer->starting_desc = params.starting_desc;
    transfer->sequence = atomic64_inc_return(&context->vctx.next_transfer_sequence);
    transfer->state = HAILO_VDMA_TRANSFER_NEW;
    hailo_vdma_vctx_get(&context->vctx);

    err = transfer_acquire_resources(transfer, context, controller, &params);
    if (err) {
        VCTX_TRACE("TRANSFER_REJECT vctx=%llu generation=%llu sequence=%llu engine=%u channel=%u status=%ld stage=resources\n",
            (unsigned long long)transfer->vctx->vctx_id,
            (unsigned long long)transfer->generation,
            (unsigned long long)transfer->sequence,
            (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
            err);
        hailo_vdma_vctx_put(&context->vctx);
        kfree(transfer);
        return err;
    }
    if (params.starting_desc >= transfer->descriptors->desc_list.desc_count) {
        VCTX_TRACE("TRANSFER_REJECT vctx=%llu generation=%llu sequence=%llu engine=%u channel=%u status=%d stage=descriptor\n",
            (unsigned long long)transfer->vctx->vctx_id,
            (unsigned long long)transfer->generation,
            (unsigned long long)transfer->sequence,
            (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
            -EINVAL);
        transfer_destroy(transfer);
        return -EINVAL;
    }

    spin_lock_irqsave(&channel_context->admission_lock, flags);
    if (!channel_context->enabled || channel_context->owner != &context->vctx ||
        channel_context->owner_generation != transfer->generation ||
        channel_context->shutting_down ||
        !vctx_generation_is_active(&context->vctx, transfer->generation)) {
        spin_unlock_irqrestore(&channel_context->admission_lock, flags);
        VCTX_TRACE("TRANSFER_CANCEL vctx=%llu generation=%llu sequence=%llu engine=%u channel=%u status=%d stage=queue\n",
            (unsigned long long)transfer->vctx->vctx_id,
            (unsigned long long)transfer->generation,
            (unsigned long long)transfer->sequence,
            (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
            -ECANCELED);
        transfer_destroy(transfer);
        return -ECANCELED;
    }
    transfer->state = HAILO_VDMA_TRANSFER_WAITING;
    list_add_tail(&transfer->admission_node, &channel_context->admission_queue);
    spin_unlock_irqrestore(&channel_context->admission_lock, flags);
    spin_lock_irqsave(&context->vctx.lock, flags);
    list_add_tail(&transfer->vctx_node, &context->vctx.queued_transfers);
    spin_unlock_irqrestore(&context->vctx.lock, flags);
    VCTX_TRACE("TRANSFER_QUEUE vctx=%llu generation=%llu sequence=%llu engine=%u channel=%u start=%u buffers=%u\n",
        (unsigned long long)transfer->vctx->vctx_id,
        (unsigned long long)transfer->generation,
        (unsigned long long)transfer->sequence,
        (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
        transfer->starting_desc, (unsigned int)transfer->buffers_count);
    wake_up_all(&channel_context->admission_wq);

    for (;;) {
        up(board_mutex);
        err = wait_event_interruptible(channel_context->admission_wq,
            admission_ready(transfer, channel_context));
        if (err) {
            *should_up_board_mutex = false;
            cancel_waiting_transfer(transfer, channel_context);
            return err;
        }
        if (down_interruptible(board_mutex)) {
            *should_up_board_mutex = false;
            cancel_waiting_transfer(transfer, channel_context);
            return -ERESTARTSYS;
        }

        mutex_lock(&channel_context->lock);
        spin_lock_irqsave(&channel_context->admission_lock, flags);
        if (transfer->cancel_requested ||
            !vctx_generation_is_active(&context->vctx, transfer->generation) ||
            channel_context->owner != &context->vctx ||
            channel_context->owner_generation != transfer->generation ||
            !channel_context->enabled || channel_context->shutting_down) {
            if (!list_empty(&transfer->admission_node)) {
                list_del_init(&transfer->admission_node);
            }
            spin_unlock_irqrestore(&channel_context->admission_lock, flags);
            mutex_unlock(&channel_context->lock);
            transfer_remove_from_vctx(transfer);
            VCTX_TRACE("TRANSFER_CANCEL vctx=%llu generation=%llu sequence=%llu engine=%u channel=%u status=%d stage=admission\n",
                (unsigned long long)transfer->vctx->vctx_id,
                (unsigned long long)transfer->generation,
                (unsigned long long)transfer->sequence,
                (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
                -ECANCELED);
            transfer_destroy(transfer);
            wake_up_all(&channel_context->admission_wq);
            return -ECANCELED;
        }
        if (!list_empty(&channel_context->admission_queue) &&
            list_first_entry(&channel_context->admission_queue,
                struct hailo_vdma_transfer, admission_node) == transfer &&
            atomic_read(&channel_context->ongoing_count) < HAILO_VDMA_CHANNEL_TRANSFER_CAPACITY &&
            atomic_read(&context->vctx.transfer_count) < context->vctx.transfer_quota) {
            transfer->state = HAILO_VDMA_TRANSFER_ADMITTED;
            spin_unlock_irqrestore(&channel_context->admission_lock, flags);
            VCTX_TRACE("TRANSFER_ADMIT vctx=%llu generation=%llu sequence=%llu engine=%u channel=%u ongoing=%d quota=%d/%u\n",
                (unsigned long long)transfer->vctx->vctx_id,
                (unsigned long long)transfer->generation,
                (unsigned long long)transfer->sequence,
                (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
                atomic_read(&channel_context->ongoing_count),
                atomic_read(&context->vctx.transfer_count), context->vctx.transfer_quota);
            break;
        }
        spin_unlock_irqrestore(&channel_context->admission_lock, flags);
        mutex_unlock(&channel_context->lock);
    }

    if (!channel_context->bound_descriptors) {
        hailo_desc_list_get(transfer->descriptors);
        channel_context->bound_descriptors = transfer->descriptors;
    } else if (channel_context->bound_descriptors != transfer->descriptors) {
        err = -EINVAL;
        transfer->status = err;
        transfer->state = HAILO_VDMA_TRANSFER_ABORTED;
        goto commit_done;
    }

    for (i = 0; i < transfer->buffers_count; i++) {
        struct hailo_vdma_buffer *buffer = transfer->buffers[i].opaque;

        hailo_vdma_buffer_sync(controller, buffer, HAILO_SYNC_FOR_DEVICE,
            transfer->buffers[i].offset, transfer->buffers[i].size);
    }

    channel = &controller->vdma_engines[params.engine_index].channels[params.channel_index];
    err = hailo_vdma_launch_transfer(controller->hw, channel,
        &transfer->descriptors->desc_list, params.starting_desc,
        params.buffers_count, transfer->buffers, params.should_bind,
        params.first_interrupts_domain, params.last_interrupts_domain,
        params.is_debug, transfer);
    if (err >= 0) {
        transfer->state = HAILO_VDMA_TRANSFER_COMMITTED;
        transfer->programmed_descs = err;
        atomic_inc(&channel_context->ongoing_count);
        atomic_inc(&context->vctx.transfer_count);
        transfer->quota_charged = true;
        spin_lock_irqsave(&context->vctx.lock, flags);
        list_del_init(&transfer->vctx_node);
        list_add_tail(&transfer->vctx_node, &context->vctx.ongoing_transfers);
        spin_unlock_irqrestore(&context->vctx.lock, flags);
        VCTX_TRACE("TRANSFER_COMMIT vctx=%llu generation=%llu sequence=%llu engine=%u channel=%u descriptors=%u ongoing=%d quota=%d/%u\n",
            (unsigned long long)transfer->vctx->vctx_id,
            (unsigned long long)transfer->generation,
            (unsigned long long)transfer->sequence,
            (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
            transfer->programmed_descs, atomic_read(&channel_context->ongoing_count),
            atomic_read(&context->vctx.transfer_count), context->vctx.transfer_quota);
    } else {
        transfer->status = err;
        transfer->state = HAILO_VDMA_TRANSFER_ABORTED;
    }

commit_done:
    spin_lock_irqsave(&channel_context->admission_lock, flags);
    if (!list_empty(&transfer->admission_node)) {
        list_del_init(&transfer->admission_node);
    }
    spin_unlock_irqrestore(&channel_context->admission_lock, flags);
    mutex_unlock(&channel_context->lock);
    wake_up_all(&channel_context->admission_wq);

    if (err < 0) {
        transfer_remove_from_vctx(transfer);
        VCTX_TRACE("TRANSFER_REJECT vctx=%llu generation=%llu sequence=%llu engine=%u channel=%u status=%ld stage=commit\n",
            (unsigned long long)transfer->vctx->vctx_id,
            (unsigned long long)transfer->generation,
            (unsigned long long)transfer->sequence,
            (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
            err);
        transfer_destroy(transfer);
    }
    return err;
}

void hailo_vdma_vctx_completion_work(struct work_struct *work)
{
    struct hailo_vdma_controller *controller =
        container_of(work, struct hailo_vdma_controller, completion_work);
    struct hailo_vdma_completion_context completion = {
        .controller = controller,
        .publish_event = true,
    };
    u32 pending[MAX_VDMA_ENGINES];
    unsigned long flags;
    u8 engine_index;
    bool any;

    for (;;) {
        any = false;
        spin_lock_irqsave(&controller->interrupts_lock, flags);
        for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
            pending[engine_index] = controller->vdma_engines[engine_index].interrupted_channels;
            controller->vdma_engines[engine_index].interrupted_channels = 0;
            any |= pending[engine_index] != 0;
        }
        spin_unlock_irqrestore(&controller->interrupts_lock, flags);
        if (!any || READ_ONCE(controller->completion_stopped)) {
            break;
        }

        for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
            struct hailo_vdma_engine *engine = &controller->vdma_engines[engine_index];

            while (pending[engine_index]) {
                struct hailo_vdma_interrupts_channel_data data = {0};
                struct hailo_vdma_channel_context *channel_context;
                struct hailo_vdma_vctx *owner;
                u8 channel_index = (u8)__builtin_ctzl(pending[engine_index]);

                pending[engine_index] &= ~BIT(channel_index);
                channel_context = &controller->channel_contexts[engine_index][channel_index];
                mutex_lock(&channel_context->lock);
                owner = channel_context->owner;
                if (!channel_context->enabled || !owner ||
                    !engine->channels[channel_index].last_desc_list) {
                    mutex_unlock(&channel_context->lock);
                    continue;
                }

                hailo_vdma_engine_push_timestamps(engine, BIT(channel_index));
                hailo_vdma_channel_fill_irq_data(&data, engine,
                    &engine->channels[channel_index], transfer_complete_callback, &completion);
                VCTX_TRACE("WORKER_DRAIN vctx=%llu generation=%llu engine=%u channel=%u data=%u ongoing=%d\n",
                    (unsigned long long)owner->vctx_id,
                    (unsigned long long)channel_context->owner_generation,
                    (unsigned int)engine_index, (unsigned int)channel_index,
                    (unsigned int)data.data, atomic_read(&channel_context->ongoing_count));
                if (data.data == HAILO_VDMA_TRANSFER_DATA_CHANNEL_WITH_ERROR ||
                    data.data == HAILO_VDMA_TRANSFER_DATA_CHANNEL_NOT_ACTIVE) {
                    set_channel_terminal_event(owner, engine_index, channel_index, data.data);
                }
                mutex_unlock(&channel_context->lock);
            }
        }
    }
}

void hailo_vdma_vctx_finalize(struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller)
{
    unsigned long flags;
    u8 engine_index;
    u8 channel_index;

    spin_lock_irqsave(&context->vctx.lock, flags);
    if (context->vctx.state != HAILO_VDMA_VCTX_ACTIVE) {
        spin_unlock_irqrestore(&context->vctx.lock, flags);
        return;
    }
    context->vctx.state = HAILO_VDMA_VCTX_CLOSING;
    context->vctx.cancel_requested = true;
    context->vctx.generation++;
    spin_unlock_irqrestore(&context->vctx.lock, flags);
    VCTX_TRACE("VCTX_CLOSE_BEGIN vctx=%llu generation=%llu\n",
        (unsigned long long)context->vctx.vctx_id,
        (unsigned long long)context->vctx.generation);
    wake_up_interruptible_all(&context->vctx.events_wq);

    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            struct hailo_vdma_channel_context *channel_context =
                &controller->channel_contexts[engine_index][channel_index];

            if (READ_ONCE(channel_context->owner) == &context->vctx) {
                disable_one_channel(controller, engine_index, channel_index, true);
            }
        }
    }

    for (engine_index = 0; engine_index < MAX_VDMA_ENGINES; engine_index++) {
        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            purge_completed_channel(&context->vctx, engine_index, channel_index, false);
        }
    }
    spin_lock_irqsave(&context->vctx.lock, flags);
    WARN_ON_ONCE(!list_empty(&context->vctx.queued_transfers));
    WARN_ON_ONCE(!list_empty(&context->vctx.ongoing_transfers));
    spin_unlock_irqrestore(&context->vctx.lock, flags);
    WARN_ON_ONCE(atomic_read(&context->vctx.transfer_count) != 0);
    hailo_vdma_vctx_put(&context->vctx);
    wait_for_completion(&context->vctx.refs_zero);
    spin_lock_irqsave(&context->vctx.lock, flags);
    context->vctx.state = HAILO_VDMA_VCTX_DEAD;
    spin_unlock_irqrestore(&context->vctx.lock, flags);
    VCTX_TRACE("VCTX_CLOSE_END vctx=%llu generation=%llu\n",
        (unsigned long long)context->vctx.vctx_id,
        (unsigned long long)context->vctx.generation);
}

static void controller_abort_channels(struct hailo_vdma_controller *controller,
    bool permanently_stopped)
{
    unsigned long flags;
    u8 engine_index;
    u8 channel_index;

    VCTX_TRACE("CONTROLLER_ABORT_BEGIN permanent=%u engines=%u\n",
        (unsigned int)permanently_stopped, (unsigned int)controller->vdma_engines_count);
    WRITE_ONCE(controller->completion_stopped, true);
    cancel_work_sync(&controller->completion_work);
    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            struct hailo_vdma_vctx *owner =
                READ_ONCE(controller->channel_contexts[engine_index][channel_index].owner);

            if (owner) {
                unsigned long owner_flags;

                spin_lock_irqsave(&owner->lock, owner_flags);
                owner->generation++;
                spin_unlock_irqrestore(&owner->lock, owner_flags);
                disable_one_channel(controller, engine_index, channel_index, true);
            }
        }
    }
    spin_lock_irqsave(&controller->interrupts_lock, flags);
    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        controller->vdma_engines[engine_index].interrupted_channels = 0;
    }
    WRITE_ONCE(controller->completion_stopped, permanently_stopped);
    spin_unlock_irqrestore(&controller->interrupts_lock, flags);
    VCTX_TRACE("CONTROLLER_ABORT_END permanent=%u\n", (unsigned int)permanently_stopped);
}

void hailo_vdma_vctx_controller_quiesce(struct hailo_vdma_controller *controller)
{
    controller_abort_channels(controller, true);
}

void hailo_vdma_vctx_controller_reset(struct hailo_vdma_controller *controller)
{
    controller_abort_channels(controller, false);
}
