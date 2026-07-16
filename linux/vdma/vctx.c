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
#define HAILO_VDMA_VCTX_WAIT_EVENT_CHANNEL_ERROR BIT(0)
#define HAILO_VDMA_VCTX_WAIT_EVENT_CHANNEL_INACTIVE BIT(1)
#define HAILO_VDMA_VCTX_WAIT_EVENT_DISABLE_WAKEUP BIT(2)
#define HAILO_VDMA_VCTX_WAIT_INDEX(engine_index, channel_index) \
    (((engine_index) * MAX_VDMA_CHANNELS_PER_ENGINE) + (channel_index))

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
    VCTX_TRACE("VCTX_CREATE vctx=%llu gen=%llu quota=%u\n",
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

static bool vctx_channel_is_logically_enabled(struct hailo_vdma_vctx *vctx,
    u8 engine_index, u8 channel_index)
{
    unsigned long flags;
    bool enabled;

    spin_lock_irqsave(&vctx->lock, flags);
    enabled = (vctx->state == HAILO_VDMA_VCTX_ACTIVE) && !vctx->cancel_requested &&
        !!(vctx->logical_channels_bitmap[engine_index] & BIT(channel_index));
    spin_unlock_irqrestore(&vctx->lock, flags);
    return enabled;
}

static enum hailo_vdma_vctx_fw_state vctx_get_fw_state(struct hailo_vdma_vctx *vctx,
    bool *resource_registered)
{
    enum hailo_vdma_vctx_fw_state state;
    unsigned long flags;

    spin_lock_irqsave(&vctx->lock, flags);
    state = vctx->fw_state;
    *resource_registered = vctx->resource_registered;
    spin_unlock_irqrestore(&vctx->lock, flags);
    return state;
}

static bool vctx_fw_is_runnable(struct hailo_vdma_vctx *vctx)
{
    bool resource_registered;
    enum hailo_vdma_vctx_fw_state state =
        vctx_get_fw_state(vctx, &resource_registered);

    /* Non-NNC users do not call HAILO_MARK_AS_IN_USE and keep the legacy
     * VDMA-only path. Registered NNC VCTXs must complete FW configuration. */
    return !resource_registered || state == HAILO_VDMA_VCTX_FW_RUNNABLE;
}

static bool vctx_fw_is_terminal(struct hailo_vdma_vctx *vctx)
{
    bool resource_registered;
    enum hailo_vdma_vctx_fw_state state =
        vctx_get_fw_state(vctx, &resource_registered);

    return resource_registered && state == HAILO_VDMA_VCTX_FW_ERROR;
}

static bool vctx_fw_allows_owner_release(struct hailo_vdma_vctx *vctx)
{
    bool resource_registered;
    enum hailo_vdma_vctx_fw_state state =
        vctx_get_fw_state(vctx, &resource_registered);

    return !resource_registered || state != HAILO_VDMA_VCTX_FW_CONFIGURING;
}

static bool vctx_uses_fw_dispatch(struct hailo_vdma_vctx *vctx)
{
    bool resource_registered;

    vctx_get_fw_state(vctx, &resource_registered);
    return resource_registered;
}

static bool vctx_device_dispatch_ready(struct hailo_vdma_vctx *vctx,
    struct hailo_vdma_controller *controller)
{
    u64 dispatched_vctx_id;

    if (!vctx_uses_fw_dispatch(vctx)) {
        return true;
    }
    dispatched_vctx_id = atomic64_read(&controller->dispatched_vctx_id);
    return dispatched_vctx_id == vctx->vctx_id ||
        atomic_read(&controller->total_ongoing_count) == 0;
}

static bool channel_owner_fw_allows_release(struct hailo_vdma_channel_context *channel_context)
{
    enum hailo_vdma_vctx_fw_state state = READ_ONCE(channel_context->owner_fw_state);

    return !READ_ONCE(channel_context->owner_resource_registered) ||
        state != HAILO_VDMA_VCTX_FW_CONFIGURING;
}

void hailo_vdma_vctx_fw_state_changed(struct hailo_vdma_vctx *vctx)
{
    struct hailo_vdma_controller *controller = vctx->controller;
    enum hailo_vdma_vctx_fw_state state;
    bool resource_registered;
    u8 engine_index;
    u8 channel_index;

    if (!controller) {
        return;
    }
    state = vctx_get_fw_state(vctx, &resource_registered);
    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            struct hailo_vdma_channel_context *channel_context =
                &controller->channel_contexts[engine_index][channel_index];

            mutex_lock(&channel_context->lock);
            if (channel_context->owner == vctx) {
                channel_context->owner_fw_state = state;
                channel_context->owner_resource_registered = resource_registered;
                channel_context->owner_active = vctx_generation_is_active(vctx,
                    channel_context->owner_generation);
            }
            mutex_unlock(&channel_context->lock);
            wake_up_all(&channel_context->admission_wq);
        }
    }
}

void hailo_vdma_vctx_wake_all_admission(struct hailo_vdma_controller *controller)
{
    u8 engine_index;
    u8 channel_index;

    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            wake_up_all(&controller->channel_contexts[engine_index][channel_index].admission_wq);
        }
    }
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

static void transfer_finish_accounting(struct hailo_vdma_channel_context *channel_context,
    struct hailo_vdma_controller *controller)
{
    atomic_dec(&channel_context->ongoing_count);
    if (atomic_dec_and_test(&controller->total_ongoing_count)) {
        hailo_vdma_vctx_wake_all_admission(controller);
    }
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
    transfer_finish_accounting(channel_context, transfer->vctx->controller);
    VCTX_TRACE("TRANSFER_ABORT vctx=%llu gen=%llu seq=%llu engine=%u channel=%u status=%d device_ongoing=%d\n",
        (unsigned long long)transfer->vctx->vctx_id,
        (unsigned long long)transfer->generation,
        (unsigned long long)transfer->sequence,
        (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
        transfer->status,
        atomic_read(&transfer->vctx->controller->total_ongoing_count));
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

    transfer_finish_accounting(channel_context, completion->controller);
    VCTX_TRACE("TRANSFER_COMPLETE vctx=%llu gen=%llu seq=%llu engine=%u channel=%u published=%u status=%d pending=%u device_ongoing=%d\n",
        (unsigned long long)vctx->vctx_id,
        (unsigned long long)transfer->generation,
        (unsigned long long)transfer->sequence,
        (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
        (unsigned int)publish, transfer->status, pending_count,
        atomic_read(&completion->controller->total_ongoing_count));
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

static void cancel_vctx_channel_admission(struct hailo_vdma_channel_context *channel_context,
    struct hailo_vdma_vctx *vctx)
{
    struct hailo_vdma_transfer *transfer;
    unsigned long flags;

    spin_lock_irqsave(&channel_context->admission_lock, flags);
    list_for_each_entry(transfer, &channel_context->admission_queue, admission_node) {
        if (transfer->vctx == vctx) {
            transfer->cancel_requested = true;
            transfer->state = HAILO_VDMA_TRANSFER_CANCELED;
        }
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

static int bind_channel_to_vctx_locked(struct hailo_vdma_controller *controller,
    u8 engine_index, u8 channel_index, struct hailo_vdma_vctx *next_owner)
{
    struct hailo_vdma_channel_context *channel_context =
        &controller->channel_contexts[engine_index][channel_index];
    struct hailo_vdma_engine *engine = &controller->vdma_engines[engine_index];
    struct hailo_vdma_channel *channel = &engine->channels[channel_index];
    struct hailo_vdma_vctx *previous_owner = channel_context->owner;
    struct hailo_vdma_file_context *next_context;
    unsigned long flags;
    u64 previous_owner_id = previous_owner ? previous_owner->vctx_id : 0;
    u32 channel_bit = BIT(channel_index);

    if (previous_owner == next_owner && channel_context->enabled &&
        channel_context->owner_generation == next_owner->generation) {
        return 0;
    }
    if (atomic_read(&channel_context->ongoing_count) != 0) {
        return -EAGAIN;
    }
    if (!vctx_channel_is_logically_enabled(next_owner, engine_index, channel_index)) {
        return -ECANCELED;
    }
    if (!vctx_fw_is_runnable(next_owner)) {
        return -EAGAIN;
    }
    if (previous_owner && previous_owner != next_owner) {
        channel_context->owner_active = vctx_generation_is_active(previous_owner,
            channel_context->owner_generation);
        channel_context->owner_fw_state = vctx_get_fw_state(previous_owner,
            &channel_context->owner_resource_registered);
        if (!channel_context->owner_active ||
            !channel_owner_fw_allows_release(channel_context)) {
            return -EAGAIN;
        }
    }

    if (previous_owner) {
        /* Preserve the descriptor-ring software state before resetting the
         * physical channel binding. */
        spin_lock_irqsave(&previous_owner->lock, flags);
        previous_owner->channel_states[engine_index][channel_index] = channel->state;
        previous_owner->channel_state_valid[engine_index][channel_index] = true;
        spin_unlock_irqrestore(&previous_owner->lock, flags);
    }

    if (channel_context->enabled) {
        hailo_vdma_engine_disable_channels_with_callback(engine, channel_bit, NULL, NULL);
    }
    if (channel_context->bound_descriptors) {
        hailo_desc_list_put(channel_context->bound_descriptors);
        channel_context->bound_descriptors = NULL;
    }

    spin_lock_irqsave(&controller->interrupts_lock, flags);
    hailo_vdma_engine_clear_channel_interrupts(engine, channel_bit);
    spin_unlock_irqrestore(&controller->interrupts_lock, flags);

    next_context = container_of(next_owner, struct hailo_vdma_file_context, vctx);
    hailo_vdma_vctx_get(next_owner);
    channel_context->owner = next_owner;
    channel_context->owner_generation = next_owner->generation;
    channel_context->owner_fw_state =
        vctx_get_fw_state(next_owner, &channel_context->owner_resource_registered);
    channel_context->owner_active = true;
    channel_context->enabled = true;
    channel_context->dispatch_sequence++;
    channel_context->last_dispatched_vctx_id = next_owner->vctx_id;
    hailo_vdma_engine_enable_channels(engine, channel_bit,
        next_owner->enable_timestamps_measure);

    spin_lock_irqsave(&next_owner->lock, flags);
    if (next_owner->channel_state_valid[engine_index][channel_index]) {
        channel->state = next_owner->channel_states[engine_index][channel_index];
    } else {
        channel->state.num_avail = 0;
        channel->state.num_proc = 0;
        channel->state.desc_count_mask = U32_MAX;
    }
    next_owner->events[engine_index][channel_index].channel_error = false;
    next_owner->events[engine_index][channel_index].channel_inactive = false;
    next_owner->events[engine_index][channel_index].disable_wakeup = false;
    spin_unlock_irqrestore(&next_owner->lock, flags);
    hailo_vdma_set_num_avail(channel->host_regs, channel->state.num_avail);
    next_context->enabled_channels_bitmap[engine_index] |= channel_bit;

    if (previous_owner) {
        hailo_vdma_vctx_put(previous_owner);
    }
    if (controller->dev) {
        hailo_vdma_update_interrupts_mask(controller, engine_index);
    }
    VCTX_TRACE("CHANNEL_SWITCH from=%llu to=%llu gen=%llu engine=%u channel=%u dispatch=%llu num_avail=%u num_proc=%u mask=0x%x\n",
        (unsigned long long)previous_owner_id,
        (unsigned long long)next_owner->vctx_id,
        (unsigned long long)next_owner->generation,
        (unsigned int)engine_index, (unsigned int)channel_index,
        (unsigned long long)channel_context->dispatch_sequence,
        (unsigned int)channel->state.num_avail,
        (unsigned int)channel->state.num_proc,
        channel->state.desc_count_mask);
    return 0;
}

static void disable_vctx_channel(struct hailo_vdma_controller *controller,
    u8 engine_index, u8 channel_index, struct hailo_vdma_file_context *context,
    bool notify_waiter)
{
    struct hailo_vdma_channel_context *channel_context =
        &controller->channel_contexts[engine_index][channel_index];
    struct hailo_vdma_engine *engine = &controller->vdma_engines[engine_index];
    struct hailo_vdma_vctx *vctx = &context->vctx;
    struct hailo_vdma_vctx *released_owner = NULL;
    unsigned long flags;
    u32 channel_bit = BIT(channel_index);
    bool was_logically_enabled;

    spin_lock_irqsave(&vctx->lock, flags);
    was_logically_enabled = !!(vctx->logical_channels_bitmap[engine_index] & channel_bit);
    vctx->logical_channels_bitmap[engine_index] &= ~channel_bit;
    vctx->channel_states[engine_index][channel_index].num_avail = 0;
    vctx->channel_states[engine_index][channel_index].num_proc = 0;
    vctx->channel_states[engine_index][channel_index].desc_count_mask = U32_MAX;
    vctx->channel_state_valid[engine_index][channel_index] = false;
    vctx->events[engine_index][channel_index].disable_wakeup = notify_waiter;
    spin_unlock_irqrestore(&vctx->lock, flags);
    context->enabled_channels_bitmap[engine_index] &= ~channel_bit;

    cancel_vctx_channel_admission(channel_context, vctx);
    mutex_lock(&channel_context->lock);
    if (was_logically_enabled && channel_context->logical_users > 0) {
        channel_context->logical_users--;
    }
    if (channel_context->owner == vctx) {
        channel_context->shutting_down = true;
        if (channel_context->enabled) {
            hailo_vdma_engine_disable_channels_with_callback(engine, channel_bit,
                transfer_abort_callback, controller);
        }
        if (channel_context->bound_descriptors) {
            hailo_desc_list_put(channel_context->bound_descriptors);
            channel_context->bound_descriptors = NULL;
        }
        WARN_ON_ONCE(atomic_read(&channel_context->ongoing_count) != 0);
        atomic_set(&channel_context->ongoing_count, 0);
        released_owner = channel_context->owner;
        channel_context->owner = NULL;
        channel_context->owner_generation = 0;
        channel_context->owner_fw_state = HAILO_VDMA_VCTX_FW_UNCONFIGURED;
        channel_context->owner_resource_registered = false;
        channel_context->owner_active = false;
        channel_context->enabled = false;
        channel_context->shutting_down = false;

        spin_lock_irqsave(&controller->interrupts_lock, flags);
        hailo_vdma_engine_clear_channel_interrupts(engine, channel_bit);
        spin_unlock_irqrestore(&controller->interrupts_lock, flags);
        if (controller->dev) {
            hailo_vdma_update_interrupts_mask(controller, engine_index);
        }
    }
    mutex_unlock(&channel_context->lock);

    purge_completed_channel(vctx, engine_index, channel_index, true);
    if (released_owner) {
        hailo_vdma_vctx_put(released_owner);
    }
    if (notify_waiter) {
        wake_up_interruptible_all(&vctx->events_wq);
    }
    wake_up_all(&channel_context->admission_wq);
    VCTX_TRACE("CHANNEL_LOGICAL_DISABLE vctx=%llu gen=%llu engine=%u channel=%u users=%u notify=%u\n",
        (unsigned long long)vctx->vctx_id,
        (unsigned long long)vctx->generation,
        (unsigned int)engine_index, (unsigned int)channel_index,
        channel_context->logical_users, (unsigned int)notify_waiter);
}

long hailo_vdma_vctx_enable_channels(struct hailo_vdma_controller *controller,
    unsigned long arg, struct hailo_vdma_file_context *context)
{
    struct hailo_vdma_enable_channels_params input;
    unsigned long flags;
    u8 engine_index;
    u8 channel_index;

    if (copy_from_user(&input, (void __user *)arg, sizeof(input))) {
        return -EFAULT;
    }
    if (!vctx_is_active(&context->vctx)) {
        return -ECANCELED;
    }

    spin_lock_irqsave(&context->vctx.lock, flags);
    context->vctx.enable_timestamps_measure = input.enable_timestamps_measure;
    spin_unlock_irqrestore(&context->vctx.lock, flags);

    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        u32 bitmap = input.channels_bitmap_per_engine[engine_index];

        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            struct hailo_vdma_channel_context *channel_context =
                &controller->channel_contexts[engine_index][channel_index];
            u32 channel_bit = BIT(channel_index);
            bool newly_enabled;
            u32 logical_users;
            u64 physical_owner_id;

            if (!(bitmap & channel_bit)) {
                continue;
            }

            spin_lock_irqsave(&context->vctx.lock, flags);
            newly_enabled = !(context->vctx.logical_channels_bitmap[engine_index] & channel_bit);
            context->vctx.logical_channels_bitmap[engine_index] |= channel_bit;
            context->vctx.events[engine_index][channel_index].channel_error = false;
            context->vctx.events[engine_index][channel_index].channel_inactive = false;
            context->vctx.events[engine_index][channel_index].disable_wakeup = false;
            spin_unlock_irqrestore(&context->vctx.lock, flags);
            context->enabled_channels_bitmap[engine_index] |= channel_bit;

            mutex_lock(&channel_context->lock);
            if (newly_enabled) {
                channel_context->logical_users++;
            }
            if (!channel_context->owner) {
                bind_channel_to_vctx_locked(controller, engine_index, channel_index,
                    &context->vctx);
            } else if (channel_context->owner == &context->vctx) {
                hailo_vdma_engine_enable_channels(&controller->vdma_engines[engine_index],
                    channel_bit, input.enable_timestamps_measure);
            }
            logical_users = channel_context->logical_users;
            physical_owner_id = channel_context->owner ?
                channel_context->owner->vctx_id : 0;
            mutex_unlock(&channel_context->lock);
            VCTX_TRACE("CHANNEL_LOGICAL_ENABLE vctx=%llu gen=%llu engine=%u channel=%u users=%u physical_owner=%llu timestamps=%u\n",
                (unsigned long long)context->vctx.vctx_id,
                (unsigned long long)context->vctx.generation,
                (unsigned int)engine_index, (unsigned int)channel_index,
                logical_users,
                (unsigned long long)physical_owner_id,
                (unsigned int)input.enable_timestamps_measure);
        }
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
        return -EFAULT;
    }

    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        u32 bitmap = input.channels_bitmap_per_engine[engine_index];

        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            if (bitmap & BIT(channel_index)) {
                disable_vctx_channel(controller, engine_index, channel_index, context, true);
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
    u8 event_flags[MAX_VDMA_ENGINES * MAX_VDMA_CHANNELS_PER_ENGINE] = {0};
    u8 completed_to_consume[MAX_VDMA_ENGINES * MAX_VDMA_CHANNELS_PER_ENGINE] = {0};
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
            unsigned long flags;

            if (!(bitmap & BIT(channel_index))) {
                continue;
            }
            if (!vctx_channel_is_logically_enabled(&context->vctx,
                engine_index, channel_index)) {
                spin_lock_irqsave(&context->vctx.lock, flags);
                context->vctx.events[engine_index][channel_index].disable_wakeup = true;
                spin_unlock_irqrestore(&context->vctx.lock, flags);
            }
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
            u32 wait_index;
            u32 completed;

            if (!(params.channels_bitmap_per_engine[engine_index] & BIT(channel_index))) {
                continue;
            }
            wait_index = HAILO_VDMA_VCTX_WAIT_INDEX(engine_index, channel_index);
            spin_lock_irqsave(&context->vctx.lock, flags);
            snapshot = context->vctx.events[engine_index][channel_index];
            context->vctx.events[engine_index][channel_index].channel_error = false;
            context->vctx.events[engine_index][channel_index].channel_inactive = false;
            context->vctx.events[engine_index][channel_index].disable_wakeup = false;
            spin_unlock_irqrestore(&context->vctx.lock, flags);
            event_flags[wait_index] =
                (snapshot.channel_error ? HAILO_VDMA_VCTX_WAIT_EVENT_CHANNEL_ERROR : 0) |
                (snapshot.channel_inactive ? HAILO_VDMA_VCTX_WAIT_EVENT_CHANNEL_INACTIVE : 0) |
                (snapshot.disable_wakeup ? HAILO_VDMA_VCTX_WAIT_EVENT_DISABLE_WAKEUP : 0);

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
            completed_to_consume[wait_index] = (u8)completed;
        }
    }

    if (copy_to_user((void __user *)arg, &params, sizeof(params))) {
        err = -ENOMEM;
        goto restore_events;
    }

    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            u32 wait_index = HAILO_VDMA_VCTX_WAIT_INDEX(engine_index, channel_index);
            u8 snapshot_flags = event_flags[wait_index];
            u8 completed = completed_to_consume[wait_index];
            u8 data;

            if (!(params.channels_bitmap_per_engine[engine_index] & BIT(channel_index))) {
                continue;
            }
            if (snapshot_flags & (HAILO_VDMA_VCTX_WAIT_EVENT_CHANNEL_ERROR |
                HAILO_VDMA_VCTX_WAIT_EVENT_CHANNEL_INACTIVE)) {
                data = (snapshot_flags & HAILO_VDMA_VCTX_WAIT_EVENT_CHANNEL_ERROR) ?
                    HAILO_VDMA_TRANSFER_DATA_CHANNEL_WITH_ERROR :
                    HAILO_VDMA_TRANSFER_DATA_CHANNEL_NOT_ACTIVE;
            } else if (completed) {
                data = (u8)completed;
            } else if (snapshot_flags & HAILO_VDMA_VCTX_WAIT_EVENT_DISABLE_WAKEUP) {
                data = 0;
            } else {
                continue;
            }
            VCTX_TRACE("WAIT_EVENT vctx=%llu gen=%llu engine=%u channel=%u data=%u disabled=%u\n",
                (unsigned long long)context->vctx.vctx_id,
                (unsigned long long)context->vctx.generation,
                (unsigned int)engine_index, (unsigned int)channel_index,
                (unsigned int)data,
                (unsigned int)!!(snapshot_flags & HAILO_VDMA_VCTX_WAIT_EVENT_DISABLE_WAKEUP));
        }
    }
    VCTX_TRACE("WAIT_DELIVER vctx=%llu gen=%llu channels=%u\n",
        (unsigned long long)context->vctx.vctx_id,
        (unsigned long long)context->vctx.generation,
        (unsigned int)params.channels_count);

    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            u32 wait_index = HAILO_VDMA_VCTX_WAIT_INDEX(engine_index, channel_index);
            u8 completed = completed_to_consume[wait_index];

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
                u32 wait_index = HAILO_VDMA_VCTX_WAIT_INDEX(engine_index, channel_index);
                u8 snapshot_flags = event_flags[wait_index];

                event->channel_error |= !!(snapshot_flags & HAILO_VDMA_VCTX_WAIT_EVENT_CHANNEL_ERROR);
                event->channel_inactive |=
                    !!(snapshot_flags & HAILO_VDMA_VCTX_WAIT_EVENT_CHANNEL_INACTIVE);
                event->disable_wakeup |=
                    !!(snapshot_flags & HAILO_VDMA_VCTX_WAIT_EVENT_DISABLE_WAKEUP);
            }
        }
        spin_unlock_irqrestore(&context->vctx.lock, flags);
        unlock_wait_channels(controller, params.channels_bitmap_per_engine);
        wake_up_interruptible_all(&context->vctx.events_wq);
    }
    VCTX_TRACE("WAIT_ROLLBACK vctx=%llu gen=%llu status=%ld\n",
        (unsigned long long)context->vctx.vctx_id,
        (unsigned long long)context->vctx.generation, err);
    return err;
}

static bool admission_ready(struct hailo_vdma_transfer *transfer,
    struct hailo_vdma_channel_context *channel_context)
{
    struct hailo_vdma_vctx *owner;
    unsigned long flags;
    bool ready;

    spin_lock_irqsave(&channel_context->admission_lock, flags);
    owner = READ_ONCE(channel_context->owner);
    ready = transfer->cancel_requested || transfer->abort_requested ||
        !vctx_generation_is_active(transfer->vctx, transfer->generation) ||
        !vctx_channel_is_logically_enabled(transfer->vctx, transfer->engine_index,
            transfer->channel_index) || vctx_fw_is_terminal(transfer->vctx);
    if (!ready && !list_empty(&channel_context->admission_queue) &&
        list_first_entry(&channel_context->admission_queue,
            struct hailo_vdma_transfer, admission_node) == transfer &&
        !channel_context->shutting_down && vctx_fw_is_runnable(transfer->vctx) &&
        vctx_device_dispatch_ready(transfer->vctx, transfer->vctx->controller) &&
        ((owner == transfer->vctx && channel_context->enabled &&
            channel_context->owner_generation == transfer->generation &&
            atomic_read(&channel_context->ongoing_count) < HAILO_VDMA_CHANNEL_TRANSFER_CAPACITY) ||
         (owner != transfer->vctx && atomic_read(&channel_context->ongoing_count) == 0 &&
            (!owner || (READ_ONCE(channel_context->owner_active) &&
                channel_owner_fw_allows_release(channel_context))))) &&
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
    VCTX_TRACE("TRANSFER_CANCEL vctx=%llu gen=%llu seq=%llu engine=%u channel=%u status=%d stage=wait\n",
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

static int dispatch_vctx_if_needed(struct hailo_vdma_controller *controller,
    struct hailo_vdma_vctx *vctx)
{
    u64 dispatched_vctx_id;
    u64 dispatched_generation;
    bool activated = false;
    int err = 0;

    if (!vctx_uses_fw_dispatch(vctx)) {
        return 0;
    }

    mutex_lock(&controller->dispatch_lock);
    dispatched_vctx_id = atomic64_read(&controller->dispatched_vctx_id);
    dispatched_generation = atomic64_read(&controller->dispatched_generation);
    if (dispatched_vctx_id == vctx->vctx_id && dispatched_generation == vctx->generation) {
        goto exit;
    }
    if (atomic_read(&controller->total_ongoing_count) != 0) {
        err = -EAGAIN;
        goto exit;
    }
    if (!controller->ops->activate_vctx) {
        err = -EOPNOTSUPP;
        goto exit;
    }

    err = controller->ops->activate_vctx(controller, vctx);
    if (!err) {
        activated = true;
        VCTX_TRACE("DEVICE_SWITCH vctx=%llu gen=%llu\n",
            (unsigned long long)vctx->vctx_id,
            (unsigned long long)vctx->generation);
    }

exit:
    mutex_unlock(&controller->dispatch_lock);
    if (activated) {
        hailo_vdma_vctx_wake_all_admission(controller);
    }
    return err;
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
    if (!vctx_fw_is_runnable(&context->vctx)) {
        VCTX_TRACE("TRANSFER_DENY requester=%llu gen=%llu engine=%u channel=%u status=%d stage=firmware-state\n",
            (unsigned long long)context->vctx.vctx_id,
            (unsigned long long)context->vctx.generation,
            (unsigned int)params.engine_index, (unsigned int)params.channel_index,
            -EAGAIN);
        return -EAGAIN;
    }

    channel_context = &controller->channel_contexts[params.engine_index][params.channel_index];
    if (!vctx_channel_is_logically_enabled(&context->vctx,
        params.engine_index, params.channel_index)) {
        VCTX_TRACE("TRANSFER_DENY requester=%llu gen=%llu engine=%u channel=%u status=%d stage=logical-channel\n",
            (unsigned long long)context->vctx.vctx_id,
            (unsigned long long)context->vctx.generation,
            (unsigned int)params.engine_index, (unsigned int)params.channel_index,
            -EPERM);
        return -EPERM;
    }

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
        VCTX_TRACE("TRANSFER_REJECT vctx=%llu gen=%llu seq=%llu engine=%u channel=%u status=%ld stage=resources\n",
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
        VCTX_TRACE("TRANSFER_REJECT vctx=%llu gen=%llu seq=%llu engine=%u channel=%u status=%d stage=descriptor\n",
            (unsigned long long)transfer->vctx->vctx_id,
            (unsigned long long)transfer->generation,
            (unsigned long long)transfer->sequence,
            (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
            -EINVAL);
        transfer_destroy(transfer);
        return -EINVAL;
    }

    spin_lock_irqsave(&channel_context->admission_lock, flags);
    if (channel_context->shutting_down ||
        !vctx_generation_is_active(&context->vctx, transfer->generation) ||
        !vctx_channel_is_logically_enabled(&context->vctx,
            params.engine_index, params.channel_index)) {
        spin_unlock_irqrestore(&channel_context->admission_lock, flags);
        VCTX_TRACE("TRANSFER_CANCEL vctx=%llu gen=%llu seq=%llu engine=%u channel=%u status=%d stage=queue\n",
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
    VCTX_TRACE("TRANSFER_QUEUE vctx=%llu gen=%llu seq=%llu engine=%u channel=%u start=%u buffers=%u\n",
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

        spin_lock_irqsave(&channel_context->admission_lock, flags);
        if (transfer->cancel_requested || transfer->abort_requested ||
            !vctx_generation_is_active(&context->vctx, transfer->generation) ||
            !vctx_channel_is_logically_enabled(&context->vctx,
                params.engine_index, params.channel_index) ||
            vctx_fw_is_terminal(&context->vctx)) {
            spin_unlock_irqrestore(&channel_context->admission_lock, flags);
            cancel_waiting_transfer(transfer, channel_context);
            return -ECANCELED;
        }
        if (!vctx_fw_is_runnable(&context->vctx) ||
            READ_ONCE(channel_context->shutting_down)) {
            spin_unlock_irqrestore(&channel_context->admission_lock, flags);
            continue;
        }
        spin_unlock_irqrestore(&channel_context->admission_lock, flags);

        /* Do not take fw_control.mutex while holding a channel mutex. FW
         * control completion updates every channel's cached FW state. */
        err = dispatch_vctx_if_needed(controller, &context->vctx);
        if (err) {
            if (err == -EAGAIN) {
                continue;
            }
            cancel_waiting_transfer(transfer, channel_context);
            return err;
        }

        mutex_lock(&channel_context->lock);
        spin_lock_irqsave(&channel_context->admission_lock, flags);
        if (transfer->cancel_requested ||
            !vctx_generation_is_active(&context->vctx, transfer->generation) ||
            !vctx_channel_is_logically_enabled(&context->vctx,
                params.engine_index, params.channel_index) ||
            vctx_fw_is_terminal(&context->vctx) || channel_context->shutting_down) {
            if (!list_empty(&transfer->admission_node)) {
                list_del_init(&transfer->admission_node);
            }
            spin_unlock_irqrestore(&channel_context->admission_lock, flags);
            mutex_unlock(&channel_context->lock);
            transfer_remove_from_vctx(transfer);
            VCTX_TRACE("TRANSFER_CANCEL vctx=%llu gen=%llu seq=%llu engine=%u channel=%u status=%d stage=admission\n",
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
            ((channel_context->owner == &context->vctx && channel_context->enabled &&
                channel_context->owner_generation == transfer->generation &&
                atomic_read(&channel_context->ongoing_count) < HAILO_VDMA_CHANNEL_TRANSFER_CAPACITY) ||
             (channel_context->owner != &context->vctx &&
                atomic_read(&channel_context->ongoing_count) == 0 &&
                (!channel_context->owner ||
                 (vctx_generation_is_active(channel_context->owner,
                    channel_context->owner_generation) &&
                  vctx_fw_allows_owner_release(channel_context->owner))))) &&
            vctx_fw_is_runnable(&context->vctx) &&
            vctx_device_dispatch_ready(&context->vctx, controller) &&
            atomic_read(&context->vctx.transfer_count) < context->vctx.transfer_quota) {
            transfer->state = HAILO_VDMA_TRANSFER_ADMITTED;
            spin_unlock_irqrestore(&channel_context->admission_lock, flags);
            if (channel_context->owner != &context->vctx || !channel_context->enabled ||
                channel_context->owner_generation != transfer->generation) {
                err = bind_channel_to_vctx_locked(controller, params.engine_index,
                    params.channel_index, &context->vctx);
                if (err) {
                    if (err == -EAGAIN) {
                        transfer->state = HAILO_VDMA_TRANSFER_WAITING;
                        mutex_unlock(&channel_context->lock);
                        continue;
                    }
                    spin_lock_irqsave(&channel_context->admission_lock, flags);
                    if (!list_empty(&transfer->admission_node)) {
                        list_del_init(&transfer->admission_node);
                    }
                    spin_unlock_irqrestore(&channel_context->admission_lock, flags);
                    mutex_unlock(&channel_context->lock);
                    transfer_remove_from_vctx(transfer);
                    transfer->status = err;
                    transfer->state = HAILO_VDMA_TRANSFER_ABORTED;
                    transfer_destroy(transfer);
                    wake_up_all(&channel_context->admission_wq);
                    return err;
                }
            }
            VCTX_TRACE("TRANSFER_ADMIT vctx=%llu gen=%llu seq=%llu engine=%u channel=%u ongoing=%d quota=%d/%u\n",
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

    if (!vctx_fw_is_runnable(&context->vctx)) {
        err = -EAGAIN;
        transfer->status = err;
        transfer->state = HAILO_VDMA_TRANSFER_ABORTED;
        goto commit_done;
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
        atomic_inc(&controller->total_ongoing_count);
        atomic_inc(&context->vctx.transfer_count);
        transfer->quota_charged = true;
        spin_lock_irqsave(&context->vctx.lock, flags);
        list_del_init(&transfer->vctx_node);
        list_add_tail(&transfer->vctx_node, &context->vctx.ongoing_transfers);
        spin_unlock_irqrestore(&context->vctx.lock, flags);
        VCTX_TRACE("TRANSFER_COMMIT vctx=%llu gen=%llu seq=%llu engine=%u channel=%u descriptors=%u ongoing=%d device_ongoing=%d quota=%d/%u\n",
            (unsigned long long)transfer->vctx->vctx_id,
            (unsigned long long)transfer->generation,
            (unsigned long long)transfer->sequence,
            (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
            transfer->programmed_descs, atomic_read(&channel_context->ongoing_count),
            atomic_read(&controller->total_ongoing_count),
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
        VCTX_TRACE("TRANSFER_REJECT vctx=%llu gen=%llu seq=%llu engine=%u channel=%u status=%ld stage=commit\n",
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
                VCTX_TRACE("WORKER_DRAIN vctx=%llu gen=%llu engine=%u channel=%u data=%u ongoing=%d\n",
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

void hailo_vdma_vctx_reset_channels(struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller, bool notify_waiter)
{
    u8 engine_index;
    u8 channel_index;

    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            struct hailo_vdma_channel_context *channel_context =
                &controller->channel_contexts[engine_index][channel_index];

            if ((READ_ONCE(context->vctx.logical_channels_bitmap[engine_index]) & BIT(channel_index)) ||
                READ_ONCE(channel_context->owner) == &context->vctx) {
                disable_vctx_channel(controller, engine_index, channel_index, context, notify_waiter);
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
    hailo_vdma_vctx_fw_state_changed(&context->vctx);
    VCTX_TRACE("VCTX_CLOSE_BEGIN vctx=%llu gen=%llu\n",
        (unsigned long long)context->vctx.vctx_id,
        (unsigned long long)context->vctx.generation);
    wake_up_interruptible_all(&context->vctx.events_wq);

    hailo_vdma_vctx_reset_channels(context, controller, true);

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
    VCTX_TRACE("VCTX_CLOSE_END vctx=%llu gen=%llu\n",
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
            struct hailo_vdma_channel_context *channel_context =
                &controller->channel_contexts[engine_index][channel_index];
            struct hailo_vdma_vctx *owner = READ_ONCE(channel_context->owner);

            cancel_channel_admission(channel_context);

            if (owner) {
                struct hailo_vdma_file_context *owner_context =
                    container_of(owner, struct hailo_vdma_file_context, vctx);
                unsigned long owner_flags;

                spin_lock_irqsave(&owner->lock, owner_flags);
                owner->generation++;
                spin_unlock_irqrestore(&owner->lock, owner_flags);
                disable_vctx_channel(controller, engine_index, channel_index,
                    owner_context, true);
            }
        }
    }
    spin_lock_irqsave(&controller->interrupts_lock, flags);
    for (engine_index = 0; engine_index < controller->vdma_engines_count; engine_index++) {
        controller->vdma_engines[engine_index].interrupted_channels = 0;
    }
    WRITE_ONCE(controller->completion_stopped, permanently_stopped);
    spin_unlock_irqrestore(&controller->interrupts_lock, flags);
    WARN_ON_ONCE(atomic_read(&controller->total_ongoing_count) != 0);
    atomic_set(&controller->total_ongoing_count, 0);
    atomic64_set(&controller->dispatched_vctx_id, 0);
    atomic64_set(&controller->dispatched_generation, 0);
    atomic64_set(&controller->notification_vctx_id, 0);
    hailo_vdma_vctx_wake_all_admission(controller);
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
