// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2025 Hailo Technologies Ltd. All rights reserved.
 **/

#define pr_fmt(fmt) "hailo: " fmt

#include "vctx.h"
#include "memory.h"
#include "utils/logs.h"

#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
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

static unsigned int vctx_stall_warn_ms = 5000;
module_param_named(vctx_stall_warn_ms, vctx_stall_warn_ms, uint, 0644);
MODULE_PARM_DESC(vctx_stall_warn_ms,
    "Report a committed VCTX transfer with no HW cursor progress for this many milliseconds (0 disables warnings; never aborts)");

static unsigned int vctx_dispatch_quantum_ms = 50;
module_param_named(vctx_dispatch_quantum_ms, vctx_dispatch_quantum_ms, uint, 0644);
MODULE_PARM_DESC(vctx_dispatch_quantum_ms,
    "Firmware VCTX ownership time threshold in milliseconds (0 disables this threshold)");

static unsigned int vctx_dispatch_quantum_transfers = 64;
module_param_named(vctx_dispatch_quantum_transfers, vctx_dispatch_quantum_transfers, uint, 0644);
MODULE_PARM_DESC(vctx_dispatch_quantum_transfers,
    "Firmware VCTX committed-transfer threshold (0 disables this threshold)");

#define VCTX_TRACE(fmt, ...) \
    do { \
        if (unlikely(READ_ONCE(vctx_trace_enabled))) \
            pr_info("vctx-trace: " fmt, ##__VA_ARGS__); \
    } while (0)

struct hailo_vdma_transfer {
    struct list_head admission_node;
    struct list_head vctx_node;
    struct list_head completed_node;
    struct hailo_vdma_vctx *vctx;
    struct hailo_descriptors_list_buffer *descriptors;
    struct hailo_vdma_mapped_transfer_buffer buffers[HAILO_MAX_BUFFERS_PER_SINGLE_TRANSFER];
    u64 sequence;
    u64 generation;
    u8 engine_index;
    u8 channel_index;
    u8 buffers_count;
    /* starting_desc/last_desc are the VCTX-local logical ring positions
     * supplied to and tracked by userspace.  The physical positions are
     * rebased after a firmware context switch, because HW num-proc is reset
     * independently from the logical userspace ring. */
    u32 starting_desc;
    u32 programmed_descs;
    u32 last_desc;
    u32 physical_starting_desc;
    u32 physical_last_desc;
    unsigned long committed_jiffies;
    unsigned long cursor_progress_jiffies;
    u16 last_hw_num_proc;
    bool cancel_requested;
    bool resources_held;
    bool quota_charged;
    bool ring_wrapped;
    bool physical_ring_wrapped;
    bool cursor_progress_valid;
    bool stall_reported;
};

struct hailo_vdma_vctx_admission_snapshot {
    u64 generation;
    enum hailo_vdma_vctx_state state;
    enum hailo_vdma_vctx_fw_state fw_state;
    bool resource_registered;
    bool channel_enabled;
};

enum hailo_vdma_admission_wake_scope {
    HAILO_VDMA_ADMISSION_WAKE_NONE = 0,
    HAILO_VDMA_ADMISSION_WAKE_CHANNEL,
    HAILO_VDMA_ADMISSION_WAKE_ALL,
};

struct hailo_vdma_completion_context {
    struct hailo_vdma_controller *controller;
    bool publish_event;
};

struct hailo_vdma_abort_context {
    struct hailo_vdma_controller *controller;
    struct hailo_vdma_channel_context *channel_context;
    enum hailo_vdma_admission_wake_scope wake_scope;
};

void hailo_vdma_vctx_trace_created(struct hailo_vdma_vctx *vctx)
{
    VCTX_TRACE("VCTX_CREATE vctx=%llu gen=%llu quota=%u\n",
        (unsigned long long)vctx->vctx_id,
        (unsigned long long)vctx->generation, vctx->transfer_quota);
}

static void vctx_get_admission_snapshot(struct hailo_vdma_vctx *vctx,
    u8 engine_index, u8 channel_index,
    struct hailo_vdma_vctx_admission_snapshot *snapshot)
{
    unsigned long flags;

    spin_lock_irqsave(&vctx->lock, flags);
    snapshot->generation = vctx->generation;
    snapshot->state = vctx->state;
    snapshot->fw_state = vctx->fw_state;
    snapshot->resource_registered = vctx->resource_registered;
    snapshot->channel_enabled =
        !!(vctx->logical_channels_bitmap[engine_index] & BIT(channel_index));
    spin_unlock_irqrestore(&vctx->lock, flags);
}

static bool vctx_admission_snapshot_fw_runnable(
    const struct hailo_vdma_vctx_admission_snapshot *snapshot)
{
    return !snapshot->resource_registered ||
        snapshot->fw_state == HAILO_VDMA_VCTX_FW_RUNNABLE;
}

static bool vctx_admission_snapshot_fw_terminal(
    const struct hailo_vdma_vctx_admission_snapshot *snapshot)
{
    return snapshot->resource_registered &&
        snapshot->fw_state == HAILO_VDMA_VCTX_FW_ERROR;
}

static bool vctx_admission_snapshot_channel_enabled(
    const struct hailo_vdma_vctx_admission_snapshot *snapshot)
{
    return snapshot->state == HAILO_VDMA_VCTX_ACTIVE &&
        snapshot->channel_enabled;
}

static bool transfer_admission_is_canceled(
    const struct hailo_vdma_transfer *transfer,
    const struct hailo_vdma_vctx_admission_snapshot *snapshot)
{
    return transfer->cancel_requested ||
        snapshot->state != HAILO_VDMA_VCTX_ACTIVE ||
        snapshot->generation != transfer->generation ||
        !snapshot->channel_enabled ||
        vctx_admission_snapshot_fw_terminal(snapshot);
}

static bool vctx_is_active(struct hailo_vdma_vctx *vctx)
{
    unsigned long flags;
    bool active;

    spin_lock_irqsave(&vctx->lock, flags);
    active = vctx->state == HAILO_VDMA_VCTX_ACTIVE;
    spin_unlock_irqrestore(&vctx->lock, flags);
    return active;
}

static bool vctx_generation_is_active(struct hailo_vdma_vctx *vctx, u64 generation)
{
    unsigned long flags;
    bool active;

    spin_lock_irqsave(&vctx->lock, flags);
    active = (vctx->state == HAILO_VDMA_VCTX_ACTIVE) &&
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
    enabled = (vctx->state == HAILO_VDMA_VCTX_ACTIVE) &&
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

static bool vctx_dispatch_quantum_expired(struct hailo_vdma_controller *controller,
    bool *time_expired, bool *transfer_expired)
{
    unsigned int quantum_ms = READ_ONCE(vctx_dispatch_quantum_ms);
    unsigned int quantum_transfers = READ_ONCE(vctx_dispatch_quantum_transfers);
    unsigned long started = READ_ONCE(controller->dispatch_started_jiffies);
    int commit_count = atomic_read(&controller->dispatch_commit_count);

    *time_expired = quantum_ms && time_after_eq(jiffies,
        started + msecs_to_jiffies(quantum_ms));
    *transfer_expired = quantum_transfers &&
        commit_count >= (int)quantum_transfers;

    /* Setting both parameters to zero intentionally restores the original
     * immediate-switch policy for diagnostics and compatibility testing. */
    return (!quantum_ms && !quantum_transfers) ||
        *time_expired || *transfer_expired;
}

static unsigned long vctx_dispatch_wait_timeout(
    struct hailo_vdma_vctx *vctx, struct hailo_vdma_controller *controller)
{
    unsigned int quantum_ms = READ_ONCE(vctx_dispatch_quantum_ms);
    u64 dispatched_vctx_id;
    unsigned long started;
    unsigned long deadline;

    if (!vctx_uses_fw_dispatch(vctx)) {
        return MAX_SCHEDULE_TIMEOUT;
    }
    dispatched_vctx_id = atomic64_read(&controller->dispatched_vctx_id);
    if (!quantum_ms || !dispatched_vctx_id ||
        dispatched_vctx_id == vctx->vctx_id ||
        atomic64_read(&controller->dispatch_request_vctx_id)) {
        return MAX_SCHEDULE_TIMEOUT;
    }

    started = READ_ONCE(controller->dispatch_started_jiffies);
    deadline = started + msecs_to_jiffies(quantum_ms);
    if (time_after_eq(jiffies, deadline)) {
        return 1;
    }
    return max_t(unsigned long, 1, deadline - jiffies);
}

static bool vctx_dispatch_cancel_request(struct hailo_vdma_vctx *vctx)
{
    struct hailo_vdma_controller *controller = vctx->controller;

    return (u64)atomic64_cmpxchg(&controller->dispatch_request_vctx_id,
        vctx->vctx_id, 0) == vctx->vctx_id;
}

static u64 vctx_dispatch_request_next(struct hailo_vdma_controller *controller,
    u64 current_vctx_id, bool *new_request)
{
    struct hailo_vdma_vctx *vctx;
    unsigned long flags;
    u64 requested_vctx_id;
    u64 next_vctx_id = 0;
    u64 wrapped_vctx_id = 0;

    *new_request = false;
    spin_lock_irqsave(&controller->dispatch_vctxs_lock, flags);
    requested_vctx_id = atomic64_read(&controller->dispatch_request_vctx_id);
    if (requested_vctx_id) {
        goto exit;
    }
    list_for_each_entry(vctx, &controller->dispatch_vctxs, dispatch_node) {
        u64 vctx_id = vctx->vctx_id;

        if (vctx_id == current_vctx_id ||
            atomic_read(&vctx->pending_transfer_count) <= 0 ||
            !READ_ONCE(vctx->resource_registered) ||
            READ_ONCE(vctx->state) != HAILO_VDMA_VCTX_ACTIVE ||
            READ_ONCE(vctx->fw_state) != HAILO_VDMA_VCTX_FW_RUNNABLE) {
            continue;
        }
        if (!wrapped_vctx_id || vctx_id < wrapped_vctx_id) {
            wrapped_vctx_id = vctx_id;
        }
        if (vctx_id > current_vctx_id &&
            (!next_vctx_id || vctx_id < next_vctx_id)) {
            next_vctx_id = vctx_id;
        }
    }
    next_vctx_id = next_vctx_id ? next_vctx_id : wrapped_vctx_id;
    if (!next_vctx_id) {
        requested_vctx_id = 0;
        goto exit;
    }
    requested_vctx_id = atomic64_cmpxchg(
        &controller->dispatch_request_vctx_id, 0, next_vctx_id);
    if (!requested_vctx_id) {
        requested_vctx_id = next_vctx_id;
        *new_request = true;
    }

exit:
    spin_unlock_irqrestore(&controller->dispatch_vctxs_lock, flags);
    return requested_vctx_id;
}

static bool vctx_device_dispatch_ready(struct hailo_vdma_vctx *vctx,
    struct hailo_vdma_controller *controller, bool uses_fw_dispatch)
{
    u64 dispatched_vctx_id;
    u64 requested_vctx_id;
    bool new_request;
    bool time_expired;
    bool transfer_expired;

    if (!uses_fw_dispatch) {
        return true;
    }
    dispatched_vctx_id = atomic64_read(&controller->dispatched_vctx_id);
    if (!dispatched_vctx_id) {
        return atomic_read(&controller->total_ongoing_count) == 0;
    }

    requested_vctx_id = atomic64_read(&controller->dispatch_request_vctx_id);
    if (dispatched_vctx_id == vctx->vctx_id) {
        /* Once a competitor has closed the quantum, do not admit more work
         * for the old owner. Existing transfers are allowed to drain. */
        return !requested_vctx_id || requested_vctx_id == vctx->vctx_id;
    }

    if (!vctx_dispatch_quantum_expired(controller,
            &time_expired, &transfer_expired)) {
        return false;
    }

    if (!requested_vctx_id) {
        requested_vctx_id = vctx_dispatch_request_next(controller,
            dispatched_vctx_id, &new_request);
        if (!requested_vctx_id) {
            return false;
        }
        if (new_request) {
            VCTX_TRACE("VCTX_QUANTUM_REQUEST requester=%llu owner=%llu commits=%d age_ms=%u time_expired=%u transfer_expired=%u device_ongoing=%d\n",
                (unsigned long long)requested_vctx_id,
                (unsigned long long)dispatched_vctx_id,
                atomic_read(&controller->dispatch_commit_count),
                jiffies_to_msecs(jiffies -
                    READ_ONCE(controller->dispatch_started_jiffies)),
                (unsigned int)time_expired,
                (unsigned int)transfer_expired,
                atomic_read(&controller->total_ongoing_count));
        }
    }

    return requested_vctx_id == vctx->vctx_id &&
        atomic_read(&controller->total_ongoing_count) == 0;
}

/* admission_lock must be held.  A global FIFO would leave the active VCTX
 * blocked behind the first competing transfer and collapse every quantum to
 * one transfer.  Select the first queued transfer of the current quantum
 * owner (or of the VCTX that requested the next quantum) instead. */
static bool channel_admission_candidate_locked(
    struct hailo_vdma_transfer *transfer,
    struct hailo_vdma_channel_context *channel_context,
    bool uses_fw_dispatch)
{
    struct hailo_vdma_controller *controller = transfer->vctx->controller;
    struct hailo_vdma_transfer *queued;
    u64 preferred_vctx_id;

    if (list_empty(&channel_context->admission_queue)) {
        return false;
    }
    if (!uses_fw_dispatch) {
        return list_first_entry(&channel_context->admission_queue,
            struct hailo_vdma_transfer, admission_node) == transfer;
    }

    preferred_vctx_id = atomic64_read(&controller->dispatch_request_vctx_id);
    if (!preferred_vctx_id) {
        preferred_vctx_id = atomic64_read(&controller->dispatched_vctx_id);
    }
    if (!preferred_vctx_id) {
        return list_first_entry(&channel_context->admission_queue,
            struct hailo_vdma_transfer, admission_node) == transfer;
    }

    list_for_each_entry(queued, &channel_context->admission_queue,
        admission_node) {
        if (queued->vctx->vctx_id == preferred_vctx_id) {
            return queued == transfer;
        }
    }
    return false;
}

static bool channel_owner_fw_allows_release(struct hailo_vdma_channel_context *channel_context)
{
    enum hailo_vdma_vctx_fw_state state = READ_ONCE(channel_context->owner_fw_state);

    return !READ_ONCE(channel_context->owner_resource_registered) ||
        state != HAILO_VDMA_VCTX_FW_CONFIGURING;
}

static bool channel_has_admission_capacity(
    const struct hailo_vdma_transfer *transfer,
    struct hailo_vdma_channel_context *channel_context,
    struct hailo_vdma_vctx *owner, u64 owner_dispatch_epoch,
    u64 dispatch_epoch, bool owner_releasable)
{
    int ongoing_count = atomic_read(&channel_context->ongoing_count);

    if (owner == transfer->vctx && READ_ONCE(channel_context->enabled) &&
        READ_ONCE(channel_context->owner_generation) == transfer->generation &&
        owner_dispatch_epoch == dispatch_epoch) {
        return ongoing_count < HAILO_VDMA_CHANNEL_TRANSFER_CAPACITY;
    }

    if ((owner != transfer->vctx || owner_dispatch_epoch != dispatch_epoch) &&
        ongoing_count == 0) {
        return !owner || owner == transfer->vctx || owner_releasable;
    }

    return false;
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

static enum hailo_vdma_admission_wake_scope merge_admission_wake_scope(
    enum hailo_vdma_admission_wake_scope existing_scope,
    enum hailo_vdma_admission_wake_scope added)
{
    return existing_scope > added ? existing_scope : added;
}

static void apply_admission_wake_scope(struct hailo_vdma_controller *controller,
    struct hailo_vdma_channel_context *channel_context,
    enum hailo_vdma_admission_wake_scope scope)
{
    if (scope == HAILO_VDMA_ADMISSION_WAKE_ALL) {
        hailo_vdma_vctx_wake_all_admission(controller);
    } else if (scope == HAILO_VDMA_ADMISSION_WAKE_CHANNEL) {
        wake_up_all(&channel_context->admission_wq);
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

static enum hailo_vdma_admission_wake_scope transfer_drop_quota(
    struct hailo_vdma_transfer *transfer)
{
    int new_count;

    if (!transfer->quota_charged) {
        return HAILO_VDMA_ADMISSION_WAKE_NONE;
    }

    new_count = atomic_dec_return(&transfer->vctx->transfer_count);
    transfer->quota_charged = false;
    if (new_count != (int)transfer->vctx->transfer_quota - 1) {
        return HAILO_VDMA_ADMISSION_WAKE_NONE;
    }

    return HAILO_VDMA_ADMISSION_WAKE_ALL;
}

static enum hailo_vdma_admission_wake_scope transfer_destroy(
    struct hailo_vdma_transfer *transfer)
{
    enum hailo_vdma_admission_wake_scope wake_scope;

    transfer_release_resources(transfer);
    wake_scope = transfer_drop_quota(transfer);
    hailo_vdma_vctx_put(transfer->vctx);
    kfree(transfer);
    return wake_scope;
}

static void transfer_destroy_uncommitted(struct hailo_vdma_transfer *transfer)
{
    enum hailo_vdma_admission_wake_scope wake_scope =
        transfer_destroy(transfer);

    WARN_ON_ONCE(wake_scope != HAILO_VDMA_ADMISSION_WAKE_NONE);
}

static enum hailo_vdma_admission_wake_scope transfer_finish_accounting(
    struct hailo_vdma_channel_context *channel_context,
    struct hailo_vdma_controller *controller)
{
    atomic_dec(&channel_context->ongoing_count);
    if (atomic_dec_and_test(&controller->total_ongoing_count)) {
        return HAILO_VDMA_ADMISSION_WAKE_ALL;
    }
    return HAILO_VDMA_ADMISSION_WAKE_CHANNEL;
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
    struct hailo_vdma_abort_context *abort_context = opaque;
    struct hailo_vdma_transfer *transfer = ongoing->opaque;
    struct hailo_vdma_channel_context *channel_context;
    struct hailo_vdma_controller *controller;
    enum hailo_vdma_admission_wake_scope wake_scope;

    if (!transfer || !abort_context) {
        return;
    }

    controller = abort_context->controller;
    channel_context = abort_context->channel_context;
    transfer_remove_from_vctx(transfer);
    wake_scope = transfer_finish_accounting(channel_context, controller);
    VCTX_TRACE("TRANSFER_ABORT vctx=%llu gen=%llu seq=%llu engine=%u channel=%u status=%d device_ongoing=%d\n",
        (unsigned long long)transfer->vctx->vctx_id,
        (unsigned long long)transfer->generation,
        (unsigned long long)transfer->sequence,
        (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
        -ECANCELED,
        atomic_read(&controller->total_ongoing_count));
    wake_scope = merge_admission_wake_scope(wake_scope,
        transfer_destroy(transfer));
    abort_context->wake_scope = merge_admission_wake_scope(
        abort_context->wake_scope, wake_scope);
}

static void transfer_complete_callback(struct hailo_ongoing_transfer *ongoing, void *opaque)
{
    struct hailo_vdma_completion_context *completion = opaque;
    struct hailo_vdma_transfer *transfer = ongoing->opaque;
    struct hailo_vdma_vctx *vctx;
    struct hailo_vdma_channel_context *channel_context;
    struct hailo_vdma_channel_state *logical_state;
    enum hailo_vdma_admission_wake_scope wake_scope;
    unsigned long flags;
    bool publish;
    u32 pending_count = 0;
    u8 i;

    if (!transfer) {
        return;
    }
    if (!completion->publish_event) {
        struct hailo_vdma_abort_context abort_context = {
            .controller = completion->controller,
            .channel_context = &transfer->vctx->controller->channel_contexts
                [transfer->engine_index][transfer->channel_index],
            .wake_scope = HAILO_VDMA_ADMISSION_WAKE_NONE,
        };

        transfer_abort_callback(ongoing, &abort_context);
        apply_admission_wake_scope(abort_context.controller,
            abort_context.channel_context, abort_context.wake_scope);
        return;
    }

    vctx = transfer->vctx;
    channel_context = &vctx->controller->channel_contexts
        [transfer->engine_index][transfer->channel_index];
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
        logical_state = &vctx->channel_states
            [transfer->engine_index][transfer->channel_index];

        if (vctx->channel_state_valid[transfer->engine_index][transfer->channel_index] &&
            logical_state->desc_count_mask != U32_MAX) {
            logical_state->num_proc = (u16)((transfer->last_desc + 1) &
                logical_state->desc_count_mask);
        }
        list_add_tail(&transfer->completed_node,
            &vctx->completed_transfers[transfer->engine_index][transfer->channel_index]);
        pending_count = ++vctx->events[transfer->engine_index][transfer->channel_index].completed_count;
    }
    spin_unlock_irqrestore(&vctx->lock, flags);

    wake_scope = transfer_finish_accounting(channel_context,
        completion->controller);
    VCTX_TRACE("TRANSFER_COMPLETE vctx=%llu gen=%llu seq=%llu engine=%u channel=%u logical_start=%u logical_last=%u logical_ring_wrap=%u physical_start=%u physical_last=%u physical_ring_wrap=%u age_ms=%u published=%u status=%d pending=%u device_ongoing=%d\n",
        (unsigned long long)vctx->vctx_id,
        (unsigned long long)transfer->generation,
        (unsigned long long)transfer->sequence,
        (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
        transfer->starting_desc, transfer->last_desc,
        (unsigned int)transfer->ring_wrapped,
        transfer->physical_starting_desc, transfer->physical_last_desc,
        (unsigned int)transfer->physical_ring_wrapped,
        jiffies_to_msecs(jiffies - transfer->committed_jiffies),
        (unsigned int)publish, publish ? 0 : -ECANCELED, pending_count,
        atomic_read(&completion->controller->total_ongoing_count));
    transfer_release_resources(transfer);
    if (publish) {
        apply_admission_wake_scope(completion->controller, channel_context,
            wake_scope);
        wake_up_interruptible_all(&vctx->events_wq);
    } else {
        wake_scope = merge_admission_wake_scope(wake_scope,
            transfer_destroy(transfer));
        apply_admission_wake_scope(completion->controller, channel_context,
            wake_scope);
    }
}

static void cancel_channel_admission(struct hailo_vdma_channel_context *channel_context)
{
    struct hailo_vdma_transfer *transfer;
    unsigned long flags;

    spin_lock_irqsave(&channel_context->admission_lock, flags);
    list_for_each_entry(transfer, &channel_context->admission_queue, admission_node) {
        transfer->cancel_requested = true;
    }
    spin_unlock_irqrestore(&channel_context->admission_lock, flags);
    wake_up_all(&channel_context->admission_wq);
}

static void mark_vctx_channel_admission_canceled(
    struct hailo_vdma_channel_context *channel_context,
    struct hailo_vdma_vctx *vctx)
{
    struct hailo_vdma_transfer *transfer;
    unsigned long flags;

    spin_lock_irqsave(&channel_context->admission_lock, flags);
    list_for_each_entry(transfer, &channel_context->admission_queue, admission_node) {
        if (transfer->vctx == vctx) {
            transfer->cancel_requested = true;
        }
    }
    spin_unlock_irqrestore(&channel_context->admission_lock, flags);
}

static enum hailo_vdma_admission_wake_scope purge_completed_channel(
    struct hailo_vdma_vctx *vctx, u8 engine_index, u8 channel_index)
{
    struct hailo_vdma_transfer *transfer;
    struct hailo_vdma_transfer *next;
    enum hailo_vdma_admission_wake_scope wake_scope =
        HAILO_VDMA_ADMISSION_WAKE_NONE;
    LIST_HEAD(release_list);
    unsigned long flags;

    spin_lock_irqsave(&vctx->lock, flags);
    list_splice_init(&vctx->completed_transfers[engine_index][channel_index], &release_list);
    vctx->events[engine_index][channel_index].completed_count = 0;
    spin_unlock_irqrestore(&vctx->lock, flags);

    list_for_each_entry_safe(transfer, next, &release_list, completed_node) {
        list_del_init(&transfer->completed_node);
        wake_scope = merge_admission_wake_scope(wake_scope,
            transfer_destroy(transfer));
    }
    return wake_scope;
}

static void set_channel_terminal_event(struct hailo_vdma_vctx *vctx, u8 engine_index,
    u8 channel_index, u8 data)
{
    struct hailo_vdma_channel_context *channel_context =
        &vctx->controller->channel_contexts[engine_index][channel_index];
    enum hailo_vdma_admission_wake_scope wake_scope;
    unsigned long flags;

    wake_scope = purge_completed_channel(vctx, engine_index, channel_index);
    spin_lock_irqsave(&vctx->lock, flags);
    if (data == HAILO_VDMA_TRANSFER_DATA_CHANNEL_WITH_ERROR) {
        vctx->events[engine_index][channel_index].channel_error = true;
    } else {
        vctx->events[engine_index][channel_index].channel_inactive = true;
    }
    spin_unlock_irqrestore(&vctx->lock, flags);
    apply_admission_wake_scope(vctx->controller, channel_context, wake_scope);
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
    unsigned long flags;
    u64 previous_owner_id = previous_owner ? previous_owner->vctx_id : 0;
    u64 dispatch_epoch = atomic64_read(&controller->dispatch_epoch);
    u32 channel_bit = BIT(channel_index);
    u16 hw_num_avail_before;
    u16 hw_num_avail_after;
    u16 hw_num_proc_before;
    u16 hw_num_proc_after;
    u16 logical_num_avail = 0;
    u16 logical_num_proc = 0;
    u32 logical_desc_count_mask = U32_MAX;
    bool next_state_valid;

    if (previous_owner == next_owner && channel_context->enabled &&
        channel_context->owner_generation == next_owner->generation &&
        channel_context->owner_dispatch_epoch == dispatch_epoch) {
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

    hailo_vdma_vctx_get(next_owner);
    channel_context->owner = next_owner;
    channel_context->owner_generation = next_owner->generation;
    channel_context->owner_fw_state =
        vctx_get_fw_state(next_owner, &channel_context->owner_resource_registered);
    channel_context->owner_active = true;
    channel_context->enabled = true;
    channel_context->owner_dispatch_epoch = dispatch_epoch;
    hailo_vdma_engine_enable_channels(engine, channel_bit,
        next_owner->enable_timestamps_measure);

    spin_lock_irqsave(&next_owner->lock, flags);
    next_state_valid = next_owner->channel_state_valid[engine_index][channel_index];
    if (next_state_valid) {
        logical_num_avail =
            next_owner->channel_states[engine_index][channel_index].num_avail;
        logical_num_proc =
            next_owner->channel_states[engine_index][channel_index].num_proc;
        logical_desc_count_mask =
            next_owner->channel_states[engine_index][channel_index].desc_count_mask;
    }
    next_owner->events[engine_index][channel_index].channel_error = false;
    next_owner->events[engine_index][channel_index].channel_inactive = false;
    next_owner->events[engine_index][channel_index].disable_wakeup = false;
    spin_unlock_irqrestore(&next_owner->lock, flags);

    /* Firmware activation happens before this binding and may reset HW
     * num-proc.  NUM_PROC has no supported write API, so make the physical
     * ring empty at the cursor reported by HW.  The next launch programs the
     * transfer at this physical cursor while advancing the VCTX-local logical
     * cursor independently. */
    channel->state.num_avail = 0;
    channel->state.num_proc = 0;
    channel->state.desc_count_mask = U32_MAX;
    hw_num_avail_before = hailo_vdma_get_num_avail(channel->host_regs);
    hw_num_proc_before = hailo_vdma_get_num_proc(channel->host_regs);
    hailo_vdma_set_num_avail(channel->host_regs, hw_num_proc_before);
    hw_num_proc_after = hailo_vdma_get_num_proc(channel->host_regs);
    if (hw_num_proc_after != hw_num_proc_before) {
        hailo_vdma_set_num_avail(channel->host_regs, hw_num_proc_after);
    }
    hw_num_avail_after = hailo_vdma_get_num_avail(channel->host_regs);
    channel->state.num_avail = hw_num_proc_after;
    channel->state.num_proc = hw_num_proc_after;
    if (previous_owner) {
        hailo_vdma_vctx_put(previous_owner);
    }
    if (controller->dev) {
        hailo_vdma_update_interrupts_mask(controller, engine_index);
    }
    VCTX_TRACE("CHANNEL_CURSOR_REBASE from=%llu to=%llu gen=%llu dispatch_epoch=%llu engine=%u channel=%u logical_valid=%u logical_avail=%u logical_proc=%u logical_mask=0x%x hw_before_avail=%u hw_before_proc=%u hw_after_avail=%u hw_after_proc=%u physical_idle_failed=%u\n",
        (unsigned long long)previous_owner_id,
        (unsigned long long)next_owner->vctx_id,
        (unsigned long long)next_owner->generation,
        (unsigned long long)dispatch_epoch,
        (unsigned int)engine_index, (unsigned int)channel_index,
        (unsigned int)next_state_valid,
        (unsigned int)logical_num_avail,
        (unsigned int)logical_num_proc,
        logical_desc_count_mask,
        (unsigned int)hw_num_avail_before,
        (unsigned int)hw_num_proc_before,
        (unsigned int)hw_num_avail_after,
        (unsigned int)hw_num_proc_after,
        (unsigned int)(hw_num_avail_after != hw_num_proc_after));
    VCTX_TRACE("CHANNEL_SWITCH from=%llu to=%llu gen=%llu dispatch_epoch=%llu engine=%u channel=%u physical_avail=%u physical_proc=%u physical_mask=0x%x\n",
        (unsigned long long)previous_owner_id,
        (unsigned long long)next_owner->vctx_id,
        (unsigned long long)next_owner->generation,
        (unsigned long long)dispatch_epoch,
        (unsigned int)engine_index, (unsigned int)channel_index,
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
    enum hailo_vdma_admission_wake_scope wake_scope;
    struct hailo_vdma_abort_context abort_context = {
        .controller = controller,
        .channel_context = channel_context,
        .wake_scope = HAILO_VDMA_ADMISSION_WAKE_NONE,
    };
    unsigned long flags;
    u32 channel_bit = BIT(channel_index);

    spin_lock_irqsave(&vctx->lock, flags);
    vctx->logical_channels_bitmap[engine_index] &= ~channel_bit;
    vctx->channel_states[engine_index][channel_index].num_avail = 0;
    vctx->channel_states[engine_index][channel_index].num_proc = 0;
    vctx->channel_states[engine_index][channel_index].desc_count_mask = U32_MAX;
    vctx->channel_state_valid[engine_index][channel_index] = false;
    vctx->events[engine_index][channel_index].disable_wakeup = notify_waiter;
    spin_unlock_irqrestore(&vctx->lock, flags);
    mark_vctx_channel_admission_canceled(channel_context, vctx);
    mutex_lock(&channel_context->lock);
    if (channel_context->owner == vctx) {
        channel_context->shutting_down = true;
        if (channel_context->enabled) {
            hailo_vdma_engine_disable_channels_with_callback(engine, channel_bit,
                transfer_abort_callback, &abort_context);
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
        channel_context->owner_dispatch_epoch = 0;
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

    wake_scope = merge_admission_wake_scope(abort_context.wake_scope,
        purge_completed_channel(vctx, engine_index, channel_index));
    if (released_owner) {
        hailo_vdma_vctx_put(released_owner);
    }
    if (notify_waiter) {
        wake_up_interruptible_all(&vctx->events_wq);
    }
    wake_scope = merge_admission_wake_scope(wake_scope,
        HAILO_VDMA_ADMISSION_WAKE_CHANNEL);
    apply_admission_wake_scope(controller, channel_context, wake_scope);
    VCTX_TRACE("CHANNEL_LOGICAL_DISABLE vctx=%llu gen=%llu engine=%u channel=%u notify=%u\n",
        (unsigned long long)vctx->vctx_id,
        (unsigned long long)vctx->generation,
        (unsigned int)engine_index, (unsigned int)channel_index,
        (unsigned int)notify_waiter);
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
            u64 physical_owner_id;

            if (!(bitmap & channel_bit)) {
                continue;
            }

            spin_lock_irqsave(&context->vctx.lock, flags);
            context->vctx.logical_channels_bitmap[engine_index] |= channel_bit;
            context->vctx.events[engine_index][channel_index].channel_error = false;
            context->vctx.events[engine_index][channel_index].channel_inactive = false;
            context->vctx.events[engine_index][channel_index].disable_wakeup = false;
            spin_unlock_irqrestore(&context->vctx.lock, flags);
            mutex_lock(&channel_context->lock);
            if (!channel_context->owner) {
                bind_channel_to_vctx_locked(controller, engine_index, channel_index,
                    &context->vctx);
            } else if (channel_context->owner == &context->vctx) {
                hailo_vdma_engine_enable_channels(&controller->vdma_engines[engine_index],
                    channel_bit, input.enable_timestamps_measure);
            }
            physical_owner_id = channel_context->owner ?
                channel_context->owner->vctx_id : 0;
            mutex_unlock(&channel_context->lock);
            VCTX_TRACE("CHANNEL_LOGICAL_ENABLE vctx=%llu gen=%llu engine=%u channel=%u physical_owner=%llu timestamps=%u\n",
                (unsigned long long)context->vctx.vctx_id,
                (unsigned long long)context->vctx.generation,
                (unsigned int)engine_index, (unsigned int)channel_index,
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
    struct hailo_vdma_controller *controller = vctx->controller;
    struct hailo_vdma_channel_context *channel_context =
        &controller->channel_contexts[engine_index][channel_index];
    struct hailo_vdma_transfer *transfer;
    struct hailo_vdma_transfer *next;
    enum hailo_vdma_admission_wake_scope wake_scope =
        HAILO_VDMA_ADMISSION_WAKE_NONE;
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
        wake_scope = merge_admission_wake_scope(wake_scope,
            transfer_destroy(transfer));
    }
    apply_admission_wake_scope(controller, channel_context, wake_scope);
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
    struct hailo_vdma_vctx_admission_snapshot snapshot;
    struct hailo_vdma_vctx *owner;
    u64 owner_dispatch_epoch;
    u64 dispatch_epoch;
    unsigned long flags;
    bool admission_policy_ready;
    bool owner_releasable;
    bool ready;

    spin_lock_irqsave(&channel_context->admission_lock, flags);
    owner = READ_ONCE(channel_context->owner);
    owner_dispatch_epoch = READ_ONCE(channel_context->owner_dispatch_epoch);
    dispatch_epoch = atomic64_read(&transfer->vctx->controller->dispatch_epoch);
    vctx_get_admission_snapshot(transfer->vctx, transfer->engine_index,
        transfer->channel_index, &snapshot);
    ready = transfer_admission_is_canceled(transfer, &snapshot);
    admission_policy_ready = !ready && !channel_context->shutting_down &&
        vctx_admission_snapshot_fw_runnable(&snapshot) &&
        vctx_device_dispatch_ready(transfer->vctx,
            transfer->vctx->controller, snapshot.resource_registered) &&
        channel_admission_candidate_locked(transfer, channel_context,
            snapshot.resource_registered);
    owner_releasable = admission_policy_ready && owner &&
        owner != transfer->vctx && READ_ONCE(channel_context->owner_active) &&
        channel_owner_fw_allows_release(channel_context);
    if (admission_policy_ready &&
        channel_has_admission_capacity(transfer, channel_context, owner,
            owner_dispatch_epoch, dispatch_epoch, owner_releasable) &&
        atomic_read(&transfer->vctx->transfer_count) < transfer->vctx->transfer_quota) {
        ready = true;
    }
    spin_unlock_irqrestore(&channel_context->admission_lock, flags);
    return ready;
}

static void transfer_dequeue_admission_locked(
    struct hailo_vdma_transfer *transfer)
{
    if (!list_empty(&transfer->admission_node)) {
        list_del_init(&transfer->admission_node);
        WARN_ON_ONCE(atomic_dec_return(
            &transfer->vctx->pending_transfer_count) < 0);
    }
}

static void cancel_waiting_transfer(struct hailo_vdma_transfer *transfer,
    struct hailo_vdma_channel_context *channel_context)
{
    enum hailo_vdma_admission_wake_scope wake_scope;
    unsigned long flags;

    spin_lock_irqsave(&channel_context->admission_lock, flags);
    transfer_dequeue_admission_locked(transfer);
    transfer->cancel_requested = true;
    spin_unlock_irqrestore(&channel_context->admission_lock, flags);
    transfer_remove_from_vctx(transfer);
    wake_scope = vctx_dispatch_cancel_request(transfer->vctx) ?
        HAILO_VDMA_ADMISSION_WAKE_ALL : HAILO_VDMA_ADMISSION_WAKE_CHANNEL;
    apply_admission_wake_scope(transfer->vctx->controller,
        channel_context, wake_scope);
    VCTX_TRACE("TRANSFER_CANCEL vctx=%llu gen=%llu seq=%llu engine=%u channel=%u status=%d stage=wait\n",
        (unsigned long long)transfer->vctx->vctx_id,
        (unsigned long long)transfer->generation,
        (unsigned long long)transfer->sequence,
        (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
        -ECANCELED);
    transfer_destroy_uncommitted(transfer);
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

/* controller->dispatch_lock must stay held until the caller either commits
 * the transfer and charges total_ongoing_count or abandons admission.  This
 * closes the window where another process could switch firmware after this
 * function returns but before the DMA doorbell is written. */
static int dispatch_vctx_if_needed_locked(struct hailo_vdma_controller *controller,
    struct hailo_vdma_vctx *vctx,
    enum hailo_vdma_admission_wake_scope *wake_scope)
{
    u64 dispatched_vctx_id;
    u64 dispatched_generation;
    u64 requested_vctx_id;
    u64 dispatch_epoch;
    unsigned int previous_age_ms;
    int previous_commit_count;
    int err = 0;

    *wake_scope = HAILO_VDMA_ADMISSION_WAKE_NONE;
    if (!vctx_uses_fw_dispatch(vctx)) {
        return 0;
    }

    dispatched_vctx_id = atomic64_read(&controller->dispatched_vctx_id);
    dispatched_generation = atomic64_read(&controller->dispatched_generation);
    requested_vctx_id = atomic64_read(&controller->dispatch_request_vctx_id);
    if (dispatched_vctx_id == vctx->vctx_id && dispatched_generation == vctx->generation) {
        if (requested_vctx_id && requested_vctx_id != vctx->vctx_id) {
            err = -EAGAIN;
        }
        goto exit;
    }
    /* When an owner exists, only the VCTX that closed its quantum may replace
     * it.  This rechecks the lockless admission decision under dispatch_lock. */
    if (dispatched_vctx_id && dispatched_vctx_id != vctx->vctx_id &&
        requested_vctx_id != vctx->vctx_id) {
        err = -EAGAIN;
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

    previous_commit_count = atomic_read(&controller->dispatch_commit_count);
    previous_age_ms = dispatched_vctx_id ?
        jiffies_to_msecs(jiffies -
            READ_ONCE(controller->dispatch_started_jiffies)) : 0;
    err = controller->ops->activate_vctx(controller, vctx);
    if (!err) {
        WRITE_ONCE(controller->dispatch_started_jiffies, jiffies);
        atomic_set(&controller->dispatch_commit_count, 0);
        atomic64_set(&controller->dispatch_request_vctx_id, 0);
        dispatch_epoch = atomic64_inc_return(&controller->dispatch_epoch);
        *wake_scope = HAILO_VDMA_ADMISSION_WAKE_ALL;
        VCTX_TRACE("VCTX_QUANTUM_BEGIN vctx=%llu previous=%llu previous_commits=%d previous_age_ms=%u quantum_ms=%u quantum_transfers=%u dispatch_epoch=%llu\n",
            (unsigned long long)vctx->vctx_id,
            (unsigned long long)dispatched_vctx_id,
            previous_commit_count, previous_age_ms,
            READ_ONCE(vctx_dispatch_quantum_ms),
            READ_ONCE(vctx_dispatch_quantum_transfers),
            (unsigned long long)dispatch_epoch);
        VCTX_TRACE("DEVICE_SWITCH vctx=%llu gen=%llu dispatch_epoch=%llu\n",
            (unsigned long long)vctx->vctx_id,
            (unsigned long long)vctx->generation,
            (unsigned long long)dispatch_epoch);
    }

exit:
    return err;
}

static int validate_vctx_logical_cursor(struct hailo_vdma_vctx *vctx,
    u8 engine_index, u8 channel_index, u32 desc_count_mask,
    u32 starting_desc)
{
    struct hailo_vdma_channel_state *logical_state;
    unsigned long flags;
    int err = 0;

    spin_lock_irqsave(&vctx->lock, flags);
    logical_state = &vctx->channel_states[engine_index][channel_index];
    if (!vctx->channel_state_valid[engine_index][channel_index]) {
        if (starting_desc != 0) {
            err = -EFAULT;
        }
    } else if (logical_state->desc_count_mask != desc_count_mask) {
        err = -EINVAL;
    } else if (logical_state->num_avail != (u16)starting_desc) {
        err = -EFAULT;
    }
    spin_unlock_irqrestore(&vctx->lock, flags);
    return err;
}

static void advance_vctx_logical_cursor(struct hailo_vdma_vctx *vctx,
    u8 engine_index, u8 channel_index, u32 desc_count_mask,
    u32 last_desc)
{
    struct hailo_vdma_channel_state *logical_state;
    unsigned long flags;

    spin_lock_irqsave(&vctx->lock, flags);
    logical_state = &vctx->channel_states[engine_index][channel_index];
    if (!vctx->channel_state_valid[engine_index][channel_index]) {
        logical_state->num_proc = 0;
        logical_state->desc_count_mask = desc_count_mask;
        vctx->channel_state_valid[engine_index][channel_index] = true;
    }
    logical_state->num_avail = (u16)((last_desc + 1) & desc_count_mask);
    spin_unlock_irqrestore(&vctx->lock, flags);
}

static int prepare_physical_channel_cursor(struct hailo_vdma_channel *channel,
    u32 desc_count_mask, u32 *physical_starting_desc)
{
    u16 hw_num_avail;
    u16 hw_num_proc;

    if (channel->state.desc_count_mask == U32_MAX) {
        hw_num_proc = hailo_vdma_get_num_proc(channel->host_regs) &
            (u16)desc_count_mask;
        hailo_vdma_set_num_avail(channel->host_regs, hw_num_proc);
        hw_num_avail = hailo_vdma_get_num_avail(channel->host_regs);
        if (hw_num_avail != hw_num_proc) {
            return -EIO;
        }
        channel->state.num_avail = hw_num_proc;
        channel->state.num_proc = hw_num_proc;
        channel->state.desc_count_mask = desc_count_mask;
    } else if (channel->state.desc_count_mask != desc_count_mask) {
        return -EINVAL;
    }

    *physical_starting_desc = channel->state.num_avail;
    return 0;
}

long hailo_vdma_vctx_launch(struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller, unsigned long arg,
    struct semaphore *board_mutex, bool *should_up_board_mutex)
{
    struct hailo_vdma_launch_transfer_params params;
    struct hailo_vdma_vctx_admission_snapshot vctx_snapshot;
    struct hailo_vdma_transfer *transfer;
    struct hailo_vdma_channel_context *channel_context;
    struct hailo_vdma_channel *channel;
    struct hailo_vdma_vctx *physical_owner;
    enum hailo_vdma_admission_wake_scope wake_scope =
        HAILO_VDMA_ADMISSION_WAKE_NONE;
    unsigned long flags;
    u64 dispatch_epoch;
    u32 desc_count_mask;
    long err;
    int quantum_commit_count = 0;
    bool admission_policy_ready;
    bool owner_releasable;
    bool wake_quantum_waiters = false;
    bool trace_admission_cancel = false;
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
    vctx_get_admission_snapshot(&context->vctx, params.engine_index,
        params.channel_index, &vctx_snapshot);
    if (!vctx_admission_snapshot_fw_runnable(&vctx_snapshot)) {
        VCTX_TRACE("TRANSFER_DENY requester=%llu gen=%llu engine=%u channel=%u status=%d stage=firmware-state\n",
            (unsigned long long)context->vctx.vctx_id,
            (unsigned long long)context->vctx.generation,
            (unsigned int)params.engine_index, (unsigned int)params.channel_index,
            -EAGAIN);
        return -EAGAIN;
    }

    channel_context = &controller->channel_contexts[params.engine_index][params.channel_index];
    if (!vctx_admission_snapshot_channel_enabled(&vctx_snapshot)) {
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
    transfer->generation = vctx_snapshot.generation;
    transfer->starting_desc = params.starting_desc;
    transfer->sequence = atomic64_inc_return(&context->vctx.next_transfer_sequence);
    hailo_vdma_vctx_get(&context->vctx);

    err = transfer_acquire_resources(transfer, context, controller, &params);
    if (err) {
        VCTX_TRACE("TRANSFER_REJECT vctx=%llu gen=%llu seq=%llu engine=%u channel=%u status=%ld stage=resources\n",
            (unsigned long long)transfer->vctx->vctx_id,
            (unsigned long long)transfer->generation,
            (unsigned long long)transfer->sequence,
            (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
            err);
        transfer_destroy_uncommitted(transfer);
        return err;
    }
    if (params.starting_desc >= transfer->descriptors->desc_list.desc_count) {
        VCTX_TRACE("TRANSFER_REJECT vctx=%llu gen=%llu seq=%llu engine=%u channel=%u status=%d stage=descriptor\n",
            (unsigned long long)transfer->vctx->vctx_id,
            (unsigned long long)transfer->generation,
            (unsigned long long)transfer->sequence,
            (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
            -EINVAL);
        transfer_destroy_uncommitted(transfer);
        return -EINVAL;
    }

    spin_lock_irqsave(&channel_context->admission_lock, flags);
    vctx_get_admission_snapshot(&context->vctx, params.engine_index,
        params.channel_index, &vctx_snapshot);
    if (channel_context->shutting_down ||
        vctx_snapshot.state != HAILO_VDMA_VCTX_ACTIVE ||
        vctx_snapshot.generation != transfer->generation ||
        !vctx_snapshot.channel_enabled) {
        spin_unlock_irqrestore(&channel_context->admission_lock, flags);
        VCTX_TRACE("TRANSFER_CANCEL vctx=%llu gen=%llu seq=%llu engine=%u channel=%u status=%d stage=queue\n",
            (unsigned long long)transfer->vctx->vctx_id,
            (unsigned long long)transfer->generation,
            (unsigned long long)transfer->sequence,
            (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
            -ECANCELED);
        transfer_destroy_uncommitted(transfer);
        return -ECANCELED;
    }
    list_add_tail(&transfer->admission_node, &channel_context->admission_queue);
    atomic_inc(&context->vctx.pending_transfer_count);
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

    for (;;) {
        up(board_mutex);
        err = wait_event_interruptible_timeout(channel_context->admission_wq,
            admission_ready(transfer, channel_context),
            vctx_dispatch_wait_timeout(&context->vctx, controller));
        if (err < 0) {
            *should_up_board_mutex = false;
            cancel_waiting_transfer(transfer, channel_context);
            return err;
        }
        if (down_interruptible(board_mutex)) {
            *should_up_board_mutex = false;
            cancel_waiting_transfer(transfer, channel_context);
            return -ERESTARTSYS;
        }
        /* A timeout is expected when a competing VCTX waits for the time
         * quantum.  Re-evaluate admission while holding board_mutex again. */
        if (!err) {
            continue;
        }

        spin_lock_irqsave(&channel_context->admission_lock, flags);
        vctx_get_admission_snapshot(&context->vctx, params.engine_index,
            params.channel_index, &vctx_snapshot);
        if (transfer_admission_is_canceled(transfer, &vctx_snapshot)) {
            spin_unlock_irqrestore(&channel_context->admission_lock, flags);
            cancel_waiting_transfer(transfer, channel_context);
            return -ECANCELED;
        }
        if (!vctx_admission_snapshot_fw_runnable(&vctx_snapshot) ||
            READ_ONCE(channel_context->shutting_down)) {
            spin_unlock_irqrestore(&channel_context->admission_lock, flags);
            continue;
        }
        spin_unlock_irqrestore(&channel_context->admission_lock, flags);

        /* Do not take fw_control.mutex while holding a channel mutex. FW
         * control completion updates every channel's cached FW state. */
        mutex_lock(&controller->dispatch_lock);
        err = dispatch_vctx_if_needed_locked(controller, &context->vctx,
            &wake_scope);
        if (err) {
            mutex_unlock(&controller->dispatch_lock);
            if (err == -EAGAIN) {
                continue;
            }
            cancel_waiting_transfer(transfer, channel_context);
            return err;
        }

        mutex_lock(&channel_context->lock);
        spin_lock_irqsave(&channel_context->admission_lock, flags);
        vctx_get_admission_snapshot(&context->vctx, params.engine_index,
            params.channel_index, &vctx_snapshot);
        if (transfer_admission_is_canceled(transfer, &vctx_snapshot) ||
            channel_context->shutting_down) {
            err = -ECANCELED;
            trace_admission_cancel = true;
            goto reject_after_dispatch_locked;
        }
        admission_policy_ready =
            vctx_admission_snapshot_fw_runnable(&vctx_snapshot) &&
            vctx_device_dispatch_ready(&context->vctx, controller,
                vctx_snapshot.resource_registered) &&
            channel_admission_candidate_locked(transfer, channel_context,
                vctx_snapshot.resource_registered);
        physical_owner = channel_context->owner;
        dispatch_epoch = atomic64_read(&controller->dispatch_epoch);
        owner_releasable = true;
        if (admission_policy_ready && physical_owner &&
            physical_owner != &context->vctx) {
            owner_releasable = vctx_generation_is_active(physical_owner,
                channel_context->owner_generation) &&
                vctx_fw_allows_owner_release(physical_owner);
        }
        if (admission_policy_ready &&
            channel_has_admission_capacity(transfer, channel_context,
                physical_owner, channel_context->owner_dispatch_epoch,
                dispatch_epoch, owner_releasable) &&
            atomic_read(&context->vctx.transfer_count) < context->vctx.transfer_quota) {
            spin_unlock_irqrestore(&channel_context->admission_lock, flags);
            if (channel_context->owner != &context->vctx || !channel_context->enabled ||
                channel_context->owner_generation != transfer->generation ||
                channel_context->owner_dispatch_epoch !=
                    atomic64_read(&controller->dispatch_epoch)) {
                err = bind_channel_to_vctx_locked(controller, params.engine_index,
                    params.channel_index, &context->vctx);
                if (err) {
                    if (err == -EAGAIN) {
                        goto retry_after_dispatch;
                    }
                    spin_lock_irqsave(&channel_context->admission_lock, flags);
                    goto reject_after_dispatch_locked;
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

retry_after_dispatch:
        mutex_unlock(&channel_context->lock);
        mutex_unlock(&controller->dispatch_lock);
        apply_admission_wake_scope(controller, channel_context, wake_scope);
        continue;

reject_after_dispatch_locked:
        transfer_dequeue_admission_locked(transfer);
        spin_unlock_irqrestore(&channel_context->admission_lock, flags);
        mutex_unlock(&channel_context->lock);
        mutex_unlock(&controller->dispatch_lock);
        transfer_remove_from_vctx(transfer);
        if (trace_admission_cancel) {
            VCTX_TRACE("TRANSFER_CANCEL vctx=%llu gen=%llu seq=%llu engine=%u channel=%u status=%d stage=admission\n",
                (unsigned long long)transfer->vctx->vctx_id,
                (unsigned long long)transfer->generation,
                (unsigned long long)transfer->sequence,
                (unsigned int)transfer->engine_index,
                (unsigned int)transfer->channel_index,
                -ECANCELED);
        }
        transfer_destroy_uncommitted(transfer);
        wake_scope = merge_admission_wake_scope(wake_scope,
            HAILO_VDMA_ADMISSION_WAKE_CHANNEL);
        apply_admission_wake_scope(controller, channel_context, wake_scope);
        return err;
    }

    if (!channel_context->bound_descriptors) {
        hailo_desc_list_get(transfer->descriptors);
        channel_context->bound_descriptors = transfer->descriptors;
    } else if (channel_context->bound_descriptors != transfer->descriptors) {
        err = -EINVAL;
        goto commit_done;
    }

    for (i = 0; i < transfer->buffers_count; i++) {
        struct hailo_vdma_buffer *buffer = transfer->buffers[i].opaque;

        hailo_vdma_buffer_sync(controller, buffer, HAILO_SYNC_FOR_DEVICE,
            transfer->buffers[i].offset, transfer->buffers[i].size);
    }

    if (!vctx_fw_is_runnable(&context->vctx)) {
        err = -EAGAIN;
        goto commit_done;
    }

    channel = &controller->vdma_engines[params.engine_index].channels[params.channel_index];
    desc_count_mask = transfer->descriptors->desc_list.desc_count - 1;
    err = validate_vctx_logical_cursor(&context->vctx, params.engine_index,
        params.channel_index, desc_count_mask, params.starting_desc);
    if (err) {
        VCTX_TRACE("TRANSFER_CURSOR_REJECT vctx=%llu gen=%llu seq=%llu engine=%u channel=%u logical_start=%u status=%ld\n",
            (unsigned long long)transfer->vctx->vctx_id,
            (unsigned long long)transfer->generation,
            (unsigned long long)transfer->sequence,
            (unsigned int)transfer->engine_index,
            (unsigned int)transfer->channel_index,
            transfer->starting_desc, err);
        goto commit_done;
    }
    err = prepare_physical_channel_cursor(channel, desc_count_mask,
        &transfer->physical_starting_desc);
    if (err) {
        goto commit_done;
    }

    /* VCTX rebasing overwrites descriptor positions that no longer have a
     * stable one-to-one relation with the UMD logical ring.  Always program
     * the complete descriptor range at the selected physical cursor; using
     * should_bind=false here could reuse a mapping from another logical slot. */
    err = hailo_vdma_launch_transfer(controller->hw, channel,
        &transfer->descriptors->desc_list, transfer->physical_starting_desc,
        params.buffers_count, transfer->buffers, true,
        params.first_interrupts_domain, params.last_interrupts_domain,
        params.is_debug, transfer);
    if (err >= 0) {
        transfer->programmed_descs = err;
        transfer->last_desc = (u32)(((u64)transfer->starting_desc +
            transfer->programmed_descs - 1) % transfer->descriptors->desc_list.desc_count);
        transfer->ring_wrapped = transfer->descriptors->desc_list.is_circular &&
            (((u64)transfer->starting_desc + transfer->programmed_descs) >=
                transfer->descriptors->desc_list.desc_count);
        transfer->physical_last_desc = (u32)(((u64)transfer->physical_starting_desc +
            transfer->programmed_descs - 1) %
            transfer->descriptors->desc_list.desc_count);
        transfer->physical_ring_wrapped =
            transfer->descriptors->desc_list.is_circular &&
            (((u64)transfer->physical_starting_desc + transfer->programmed_descs) >=
                transfer->descriptors->desc_list.desc_count);
        advance_vctx_logical_cursor(&context->vctx, params.engine_index,
            params.channel_index, desc_count_mask, transfer->last_desc);
        transfer->committed_jiffies = jiffies;
        transfer->cursor_progress_jiffies = transfer->committed_jiffies;
        transfer->last_hw_num_proc = hailo_vdma_get_num_proc(channel->host_regs) &
            (u16)channel->state.desc_count_mask;
        transfer->cursor_progress_valid = true;
        atomic_inc(&channel_context->ongoing_count);
        atomic_inc(&controller->total_ongoing_count);
        if (vctx_uses_fw_dispatch(&context->vctx) &&
            atomic64_read(&controller->dispatched_vctx_id) ==
                context->vctx.vctx_id) {
            quantum_commit_count =
                atomic_inc_return(&controller->dispatch_commit_count);
            wake_quantum_waiters = READ_ONCE(vctx_dispatch_quantum_transfers) &&
                quantum_commit_count ==
                    (int)READ_ONCE(vctx_dispatch_quantum_transfers);
        }
        atomic_inc(&context->vctx.transfer_count);
        transfer->quota_charged = true;
        spin_lock_irqsave(&context->vctx.lock, flags);
        list_del_init(&transfer->vctx_node);
        list_add_tail(&transfer->vctx_node, &context->vctx.ongoing_transfers);
        spin_unlock_irqrestore(&context->vctx.lock, flags);
        VCTX_TRACE("TRANSFER_COMMIT vctx=%llu gen=%llu seq=%llu engine=%u channel=%u logical_start=%u logical_last=%u descriptors=%u logical_ring_wrap=%u logical_avail=%u logical_proc=%u physical_start=%u physical_last=%u physical_ring_wrap=%u physical_avail=%u physical_proc=%u hw_avail=%u hw_proc=%u user_should_bind=%u forced_bind=1 ongoing=%d device_ongoing=%d quota=%d/%u quantum_commits=%d\n",
            (unsigned long long)transfer->vctx->vctx_id,
            (unsigned long long)transfer->generation,
            (unsigned long long)transfer->sequence,
            (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
            transfer->starting_desc, transfer->last_desc, transfer->programmed_descs,
            (unsigned int)transfer->ring_wrapped,
            (unsigned int)READ_ONCE(context->vctx.channel_states
                [params.engine_index][params.channel_index].num_avail),
            (unsigned int)READ_ONCE(context->vctx.channel_states
                [params.engine_index][params.channel_index].num_proc),
            transfer->physical_starting_desc, transfer->physical_last_desc,
            (unsigned int)transfer->physical_ring_wrapped,
            (unsigned int)channel->state.num_avail,
            (unsigned int)channel->state.num_proc,
            (unsigned int)hailo_vdma_get_num_avail(channel->host_regs),
            (unsigned int)transfer->last_hw_num_proc,
            (unsigned int)params.should_bind,
            atomic_read(&channel_context->ongoing_count),
            atomic_read(&controller->total_ongoing_count),
            atomic_read(&context->vctx.transfer_count), context->vctx.transfer_quota,
            quantum_commit_count);
    }

commit_done:
    spin_lock_irqsave(&channel_context->admission_lock, flags);
    transfer_dequeue_admission_locked(transfer);
    spin_unlock_irqrestore(&channel_context->admission_lock, flags);
    mutex_unlock(&channel_context->lock);
    mutex_unlock(&controller->dispatch_lock);
    wake_scope = merge_admission_wake_scope(wake_scope,
        wake_quantum_waiters ? HAILO_VDMA_ADMISSION_WAKE_ALL :
            HAILO_VDMA_ADMISSION_WAKE_CHANNEL);

    if (err < 0) {
        transfer_remove_from_vctx(transfer);
        VCTX_TRACE("TRANSFER_REJECT vctx=%llu gen=%llu seq=%llu engine=%u channel=%u status=%ld stage=commit\n",
            (unsigned long long)transfer->vctx->vctx_id,
            (unsigned long long)transfer->generation,
            (unsigned long long)transfer->sequence,
            (unsigned int)transfer->engine_index, (unsigned int)transfer->channel_index,
            err);
        transfer_destroy_uncommitted(transfer);
    }
    apply_admission_wake_scope(controller, channel_context, wake_scope);
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
                VCTX_TRACE("WORKER_DRAIN vctx=%llu gen=%llu engine=%u channel=%u data=%u logical_avail=%u logical_proc=%u physical_avail=%u physical_proc=%u hw_avail=%u hw_proc=%u ongoing=%d\n",
                    (unsigned long long)owner->vctx_id,
                    (unsigned long long)channel_context->owner_generation,
                    (unsigned int)engine_index, (unsigned int)channel_index,
                    (unsigned int)data.data,
                    (unsigned int)READ_ONCE(owner->channel_states
                        [engine_index][channel_index].num_avail),
                    (unsigned int)READ_ONCE(owner->channel_states
                        [engine_index][channel_index].num_proc),
                    (unsigned int)engine->channels[channel_index].state.num_avail,
                    (unsigned int)engine->channels[channel_index].state.num_proc,
                    (unsigned int)hailo_vdma_get_num_avail(
                        engine->channels[channel_index].host_regs),
                    (unsigned int)(hailo_vdma_get_num_proc(
                        engine->channels[channel_index].host_regs) &
                        (u16)engine->channels[channel_index].state.desc_count_mask),
                    atomic_read(&channel_context->ongoing_count));
                if (data.data == HAILO_VDMA_TRANSFER_DATA_CHANNEL_WITH_ERROR ||
                    data.data == HAILO_VDMA_TRANSFER_DATA_CHANNEL_NOT_ACTIVE) {
                    set_channel_terminal_event(owner, engine_index, channel_index, data.data);
                }
                mutex_unlock(&channel_context->lock);
            }
        }
    }
}

static unsigned long vctx_stall_monitor_interval_jiffies(void)
{
    unsigned int timeout_ms = READ_ONCE(vctx_stall_warn_ms);
    unsigned int interval_ms = timeout_ms ?
        clamp_t(unsigned int, timeout_ms / 4, 100, 1000) : 1000;

    return max_t(unsigned long, 1, msecs_to_jiffies(interval_ms));
}

static bool vctx_channel_has_ongoing_transfer(
    const struct hailo_ongoing_transfers_list *transfers)
{
    /* The common VDMA circular-list macros are private to vdma_common.c.
     * The monitor only needs the empty/non-empty state, for which head != tail
     * is sufficient while channel_context->lock serializes list updates. */
    return transfers->head != transfers->tail;
}

static void hailo_vdma_vctx_stall_monitor_work(struct work_struct *work)
{
    struct delayed_work *delayed_work =
        container_of(work, struct delayed_work, work);
    struct hailo_vdma_controller *controller =
        container_of(delayed_work, struct hailo_vdma_controller, stall_monitor_work);
    unsigned int timeout_ms = READ_ONCE(vctx_stall_warn_ms);
    unsigned long now = jiffies;
    u8 engine_index;
    u8 channel_index;

    if (READ_ONCE(controller->stall_monitor_stopped)) {
        return;
    }

    if (timeout_ms && READ_ONCE(vctx_trace_enabled)) {
        for (engine_index = 0; engine_index < controller->vdma_engines_count;
            engine_index++) {
            for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE;
                channel_index++) {
                struct hailo_vdma_channel_context *channel_context =
                    &controller->channel_contexts[engine_index][channel_index];
                struct hailo_vdma_channel *channel =
                    &controller->vdma_engines[engine_index].channels[channel_index];
                struct hailo_ongoing_transfer *ongoing;
                struct hailo_vdma_transfer *transfer;
                u16 hw_num_avail;
                u16 hw_num_proc;
                u32 first_desc_status;
                u32 last_desc_status;

                mutex_lock(&channel_context->lock);
                if (!channel_context->enabled ||
                    !vctx_channel_has_ongoing_transfer(
                        &channel->ongoing_transfers)) {
                    mutex_unlock(&channel_context->lock);
                    continue;
                }

                ongoing = &channel->ongoing_transfers.transfers[
                    channel->ongoing_transfers.tail];
                transfer = ongoing->opaque;
                if (!transfer) {
                    mutex_unlock(&channel_context->lock);
                    continue;
                }

                hw_num_avail = hailo_vdma_get_num_avail(channel->host_regs);
                hw_num_proc = hailo_vdma_get_num_proc(channel->host_regs);
                first_desc_status = READ_ONCE(transfer->descriptors->desc_list
                    .desc_list[transfer->physical_starting_desc].RemainingPageSize_Status);
                last_desc_status = READ_ONCE(transfer->descriptors->desc_list
                    .desc_list[transfer->physical_last_desc].RemainingPageSize_Status);
                if (channel->state.desc_count_mask != U32_MAX) {
                    hw_num_proc &= (u16)channel->state.desc_count_mask;
                }

                if (!transfer->cursor_progress_valid ||
                    transfer->last_hw_num_proc != hw_num_proc) {
                    transfer->last_hw_num_proc = hw_num_proc;
                    transfer->cursor_progress_jiffies = now;
                    transfer->cursor_progress_valid = true;
                    transfer->stall_reported = false;
                } else if (!transfer->stall_reported &&
                    time_after_eq(now, transfer->cursor_progress_jiffies +
                        msecs_to_jiffies(timeout_ms))) {
                    transfer->stall_reported = true;
                    VCTX_TRACE("TRANSFER_STALL_WARN vctx=%llu gen=%llu seq=%llu engine=%u channel=%u logical_start=%u logical_last=%u descriptors=%u logical_ring_wrap=%u physical_start=%u physical_last=%u physical_ring_wrap=%u age_ms=%u no_progress_ms=%u logical_avail=%u logical_proc=%u physical_avail=%u physical_proc=%u hw_avail=%u hw_proc=%u first_status=0x%x last_status=0x%x ongoing=%d device_ongoing=%d action=diagnostic-only\n",
                        (unsigned long long)transfer->vctx->vctx_id,
                        (unsigned long long)transfer->generation,
                        (unsigned long long)transfer->sequence,
                        (unsigned int)engine_index,
                        (unsigned int)channel_index,
                        transfer->starting_desc,
                        transfer->last_desc,
                        transfer->programmed_descs,
                        (unsigned int)transfer->ring_wrapped,
                        transfer->physical_starting_desc,
                        transfer->physical_last_desc,
                        (unsigned int)transfer->physical_ring_wrapped,
                        jiffies_to_msecs(now - transfer->committed_jiffies),
                        jiffies_to_msecs(now - transfer->cursor_progress_jiffies),
                        (unsigned int)READ_ONCE(transfer->vctx->channel_states
                            [engine_index][channel_index].num_avail),
                        (unsigned int)READ_ONCE(transfer->vctx->channel_states
                            [engine_index][channel_index].num_proc),
                        (unsigned int)channel->state.num_avail,
                        (unsigned int)channel->state.num_proc,
                        (unsigned int)hw_num_avail,
                        (unsigned int)hw_num_proc,
                        first_desc_status,
                        last_desc_status,
                        atomic_read(&channel_context->ongoing_count),
                        atomic_read(&controller->total_ongoing_count));
                }
                mutex_unlock(&channel_context->lock);
            }
        }
    }

    if (!READ_ONCE(controller->stall_monitor_stopped)) {
        schedule_delayed_work(&controller->stall_monitor_work,
            vctx_stall_monitor_interval_jiffies());
    }
}

void hailo_vdma_vctx_stall_monitor_init(struct hailo_vdma_controller *controller)
{
    WRITE_ONCE(controller->stall_monitor_stopped, true);
    INIT_DELAYED_WORK(&controller->stall_monitor_work,
        hailo_vdma_vctx_stall_monitor_work);
}

void hailo_vdma_vctx_stall_monitor_start(struct hailo_vdma_controller *controller)
{
    WRITE_ONCE(controller->stall_monitor_stopped, false);
    schedule_delayed_work(&controller->stall_monitor_work,
        vctx_stall_monitor_interval_jiffies());
}

void hailo_vdma_vctx_stall_monitor_stop(struct hailo_vdma_controller *controller)
{
    WRITE_ONCE(controller->stall_monitor_stopped, true);
    cancel_delayed_work_sync(&controller->stall_monitor_work);
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
    enum hailo_vdma_admission_wake_scope wake_scope;
    unsigned long flags;
    u8 engine_index;
    u8 channel_index;

    spin_lock_irqsave(&context->vctx.lock, flags);
    if (context->vctx.state != HAILO_VDMA_VCTX_ACTIVE) {
        spin_unlock_irqrestore(&context->vctx.lock, flags);
        return;
    }
    context->vctx.state = HAILO_VDMA_VCTX_CLOSING;
    context->vctx.generation++;
    spin_unlock_irqrestore(&context->vctx.lock, flags);
    spin_lock_irqsave(&controller->dispatch_vctxs_lock, flags);
    if (!list_empty(&context->vctx.dispatch_node)) {
        list_del_init(&context->vctx.dispatch_node);
    }
    spin_unlock_irqrestore(&controller->dispatch_vctxs_lock, flags);
    (void)vctx_dispatch_cancel_request(&context->vctx);
    hailo_vdma_vctx_fw_state_changed(&context->vctx);
    VCTX_TRACE("VCTX_CLOSE_BEGIN vctx=%llu gen=%llu\n",
        (unsigned long long)context->vctx.vctx_id,
        (unsigned long long)context->vctx.generation);
    wake_up_interruptible_all(&context->vctx.events_wq);

    hailo_vdma_vctx_reset_channels(context, controller, true);

    for (engine_index = 0; engine_index < MAX_VDMA_ENGINES; engine_index++) {
        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            wake_scope = purge_completed_channel(&context->vctx,
                engine_index, channel_index);
            apply_admission_wake_scope(controller,
                &controller->channel_contexts[engine_index][channel_index],
                wake_scope);
        }
    }
    spin_lock_irqsave(&context->vctx.lock, flags);
    WARN_ON_ONCE(!list_empty(&context->vctx.queued_transfers));
    WARN_ON_ONCE(!list_empty(&context->vctx.ongoing_transfers));
    spin_unlock_irqrestore(&context->vctx.lock, flags);
    WARN_ON_ONCE(atomic_read(&context->vctx.pending_transfer_count) != 0);
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
    atomic64_set(&controller->dispatch_epoch, 0);
    atomic64_set(&controller->notification_vctx_id, 0);
    WRITE_ONCE(controller->dispatch_started_jiffies, jiffies);
    atomic_set(&controller->dispatch_commit_count, 0);
    atomic64_set(&controller->dispatch_request_vctx_id, 0);
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
