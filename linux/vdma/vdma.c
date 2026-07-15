// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2025 Hailo Technologies Ltd. All rights reserved.
 **/

#define pr_fmt(fmt) "hailo: " fmt

#include "vdma.h"
#include "memory.h"
#include "ioctl.h"
#include "vctx.h"
#include "utils/logs.h"

#include <linux/sched.h>
#include <linux/string.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
#include <linux/dma-map-ops.h>
#else
#include <linux/dma-mapping.h>
#endif


static struct hailo_vdma_engine* init_vdma_engines(struct device *dev,
    struct hailo_resource *channel_registers_per_engine, size_t engines_count, u32 src_channels_bitmask)
{
    struct hailo_vdma_engine *engines = NULL;
    u8 i = 0;

    engines = devm_kmalloc_array(dev, engines_count, sizeof(*engines), GFP_KERNEL);
    if (NULL == engines) {
        dev_err(dev, "Failed allocating vdma engines\n");
        return ERR_PTR(-ENOMEM);
    }

    for (i = 0; i < engines_count; i++) {
        hailo_vdma_engine_init(&engines[i], i, &channel_registers_per_engine[i], src_channels_bitmask);
    }

    return engines;
}

static int hailo_set_dma_mask(struct device *dev)
{
    int err = -EINVAL;
    /* Check and configure DMA length */
    if (!(err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64)))) {
        dev_notice(dev, "Probing: Enabled 64 bit dma\n");
    } else if (!(err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(48)))) {
        dev_notice(dev, "Probing: Enabled 48 bit dma\n");
    } else if (!(err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(40)))) {
        dev_notice(dev, "Probing: Enabled 40 bit dma\n");
    } else if (!(err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(36)))) {
        dev_notice(dev, "Probing: Enabled 36 bit dma\n");
    } else if (!(err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32)))) {
        dev_notice(dev, "Probing: Enabled 32 bit dma\n");
    } else {
        dev_err(dev, "Probing: Error enabling dma %d\n", err);
        return err;
    }

    return 0;
}

int hailo_vdma_controller_init(struct hailo_vdma_controller *controller,
    struct device *dev, struct hailo_vdma_hw *vdma_hw,
    struct hailo_vdma_controller_ops *ops,
    struct hailo_resource *channel_registers_per_engine, size_t engines_count)
{
    int err = 0;
    u8 engine_index;
    u8 channel_index;

    if (engines_count == 0 || engines_count > MAX_VDMA_ENGINES) {
        return -EINVAL;
    }
    controller->hw = vdma_hw;
    controller->ops = ops;
    controller->dev = dev;

    controller->vdma_engines_count = engines_count;
    controller->vdma_engines = init_vdma_engines(dev, channel_registers_per_engine, engines_count,
        vdma_hw->src_channels_bitmask);
    if (IS_ERR(controller->vdma_engines)) {
        dev_err(dev, "Failed initialized vdma engines\n");
        return PTR_ERR(controller->vdma_engines);
    }

    controller->used_by_filp = NULL;
    spin_lock_init(&controller->interrupts_lock);
    init_waitqueue_head(&controller->interrupts_wq);
    atomic64_set(&controller->last_vctx_id, 0);
    controller->completion_stopped = false;
    INIT_WORK(&controller->completion_work, hailo_vdma_vctx_completion_work);
    for (engine_index = 0; engine_index < MAX_VDMA_ENGINES; engine_index++) {
        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            struct hailo_vdma_channel_context *channel_context =
                &controller->channel_contexts[engine_index][channel_index];

            mutex_init(&channel_context->lock);
            spin_lock_init(&channel_context->admission_lock);
            init_waitqueue_head(&channel_context->admission_wq);
            INIT_LIST_HEAD(&channel_context->admission_queue);
            channel_context->owner = NULL;
            channel_context->owner_generation = 0;
            channel_context->bound_descriptors = NULL;
            atomic_set(&channel_context->ongoing_count, 0);
            channel_context->enabled = false;
            channel_context->shutting_down = false;
        }
    }

    /* Check and configure DMA length */
    err = hailo_set_dma_mask(dev);
    if (0 > err) {
        return err;
    }

    if (get_dma_ops(controller->dev)) {
        hailo_dev_notice(controller->dev, "Probing: Using specialized dma_ops=%ps", get_dma_ops(controller->dev));
    }

    return 0;
}

void hailo_vdma_controller_cleanup(struct hailo_vdma_controller *controller)
{
    hailo_vdma_vctx_controller_quiesce(controller);
}

void hailo_vdma_controller_reset(struct hailo_vdma_controller *controller)
{
    hailo_vdma_vctx_controller_reset(controller);
}

void hailo_vdma_file_context_init(struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller)
{
    u8 engine_index;
    u8 channel_index;

    context->vctx.vctx_id = atomic64_inc_return(&controller->last_vctx_id);
    context->vctx.generation = 1;
    context->vctx.state = HAILO_VDMA_VCTX_ACTIVE;
    kref_init(&context->vctx.refcount);
    init_completion(&context->vctx.refs_zero);
    spin_lock_init(&context->vctx.lock);
    init_waitqueue_head(&context->vctx.events_wq);
    atomic64_set(&context->vctx.next_transfer_sequence, 0);
    atomic_set(&context->vctx.transfer_count, 0);
    context->vctx.transfer_quota = controller->vdma_engines_count *
        MAX_VDMA_CHANNELS_PER_ENGINE * (HAILO_VDMA_MAX_ONGOING_TRANSFERS - 1);
    context->vctx.cancel_requested = false;
    context->vctx.controller = controller;
    memset(context->vctx.events, 0, sizeof(context->vctx.events));
    INIT_LIST_HEAD(&context->vctx.queued_transfers);
    INIT_LIST_HEAD(&context->vctx.ongoing_transfers);
    for (engine_index = 0; engine_index < MAX_VDMA_ENGINES; engine_index++) {
        context->enabled_channels_bitmap[engine_index] = 0;
        for (channel_index = 0; channel_index < MAX_VDMA_CHANNELS_PER_ENGINE; channel_index++) {
            INIT_LIST_HEAD(&context->vctx.completed_transfers[engine_index][channel_index]);
        }
    }

    atomic_set(&context->last_vdma_user_buffer_handle, 0);
    INIT_LIST_HEAD(&context->mapped_user_buffer_list);

    atomic_set(&context->last_vdma_handle, 0);
    INIT_LIST_HEAD(&context->descriptors_buffer_list);
    INIT_LIST_HEAD(&context->vdma_low_memory_buffer_list);
    INIT_LIST_HEAD(&context->continuous_buffer_list);

    BUILD_BUG_ON_MSG(MAX_VDMA_CHANNELS_PER_ENGINE > sizeof(context->enabled_channels_bitmap[0]) * BITS_IN_BYTE,
        "Unexpected amount of VDMA channels per engine");
}

void hailo_vdma_update_interrupts_mask(struct hailo_vdma_controller *controller,
    size_t engine_index)
{
    struct hailo_vdma_engine *engine = &controller->vdma_engines[engine_index];
    controller->ops->update_channel_interrupts(controller, engine_index, engine->enabled_channels);
}

void hailo_vdma_file_context_finalize(struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller, struct file *filp)
{
    hailo_vdma_vctx_finalize(context, controller);

    hailo_vdma_clear_mapped_user_buffer_list(context, controller);
    hailo_vdma_clear_descriptors_buffer_list(context, controller);
    hailo_vdma_clear_low_memory_buffer_list(context);
    hailo_vdma_clear_continuous_buffer_list(context, controller);

    if (filp == controller->used_by_filp) {
        controller->used_by_filp = NULL;
    }
}

void hailo_vdma_wakeup_interrupts(struct hailo_vdma_controller *controller, struct hailo_vdma_engine *engine,
    u32 channels_bitmap)
{
    unsigned long irq_saved_flags = 0;

    spin_lock_irqsave(&controller->interrupts_lock, irq_saved_flags);
    hailo_vdma_engine_set_channel_interrupts(engine, channels_bitmap);
    spin_unlock_irqrestore(&controller->interrupts_lock, irq_saved_flags);

    if (!READ_ONCE(controller->completion_stopped)) {
        schedule_work(&controller->completion_work);
    }
}

void hailo_vdma_irq_handler(struct hailo_vdma_controller *controller,
    size_t engine_index, u32 channels_bitmap)
{
    struct hailo_vdma_engine *engine = NULL;

    BUG_ON(engine_index >= controller->vdma_engines_count);
    engine = &controller->vdma_engines[engine_index];

    hailo_vdma_wakeup_interrupts(controller, engine, channels_bitmap);
}

long hailo_vdma_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller,
    unsigned int cmd, unsigned long arg, struct file *filp, struct semaphore *mutex, bool *should_up_board_mutex)
{
    switch (cmd) {
    case HAILO_VDMA_ENABLE_CHANNELS:
        return hailo_vdma_vctx_enable_channels(controller, arg, context);
    case HAILO_VDMA_DISABLE_CHANNELS:
        return hailo_vdma_vctx_disable_channels(controller, arg, context);
    case HAILO_VDMA_INTERRUPTS_WAIT:
        return hailo_vdma_vctx_wait(context, controller, arg, mutex, should_up_board_mutex);
    case HAILO_VDMA_INTERRUPTS_READ_TIMESTAMPS:
        return hailo_vdma_interrupts_read_timestamps_ioctl(context, controller, arg);
    case HAILO_VDMA_BUFFER_MAP:
        return hailo_vdma_buffer_map_ioctl(context, controller, arg);
    case HAILO_VDMA_BUFFER_UNMAP:
        return hailo_vdma_buffer_unmap_ioctl(context, controller, arg);
    case HAILO_VDMA_BUFFER_SYNC:
        return hailo_vdma_buffer_sync_ioctl(context, controller, arg);
    case HAILO_DESC_LIST_CREATE:
        return hailo_desc_list_create_ioctl(context, controller, arg);
    case HAILO_DESC_LIST_RELEASE:
        return hailo_desc_list_release_ioctl(context, controller, arg);
    case HAILO_DESC_LIST_PROGRAM:
        return hailo_desc_list_program_ioctl(context, controller, arg);
    case HAILO_VDMA_LOW_MEMORY_BUFFER_ALLOC:
        return hailo_vdma_low_memory_buffer_alloc_ioctl(context, controller, arg);
    case HAILO_VDMA_LOW_MEMORY_BUFFER_FREE:
        return hailo_vdma_low_memory_buffer_free_ioctl(context, controller, arg);
    case HAILO_MARK_AS_IN_USE:
        return hailo_mark_as_in_use(controller, arg, filp);
    case HAILO_VDMA_CONTINUOUS_BUFFER_ALLOC:
        return hailo_vdma_continuous_buffer_alloc_ioctl(context, controller, arg);
    case HAILO_VDMA_CONTINUOUS_BUFFER_FREE:
        return hailo_vdma_continuous_buffer_free_ioctl(context, controller, arg);
    case HAILO_VDMA_LAUNCH_TRANSFER:
        return hailo_vdma_vctx_launch(context, controller, arg, mutex, should_up_board_mutex);
    default:
        hailo_dev_err(controller->dev, "Invalid vDMA ioctl code 0x%x (nr: %d)\n", cmd, _IOC_NR(cmd));
        return -ENOTTY;
    }
}

static int low_memory_buffer_mmap(struct hailo_vdma_controller *controller,
    struct hailo_vdma_low_memory_buffer *vdma_buffer, struct vm_area_struct *vma)
{
    int err     = 0;
    size_t i    = 0;
    unsigned long vsize         = vma->vm_end - vma->vm_start;
    unsigned long orig_vm_start = vma->vm_start;
    unsigned long orig_vm_end   = vma->vm_end;
    unsigned long page_fn       = 0;

    if (vsize != vdma_buffer->pages_count * PAGE_SIZE) {
        hailo_dev_err(controller->dev, "mmap size should be %lu (given %lu)\n",
            vdma_buffer->pages_count * PAGE_SIZE, vsize);
        return -EINVAL;
    }

    for (i = 0 ; i < vdma_buffer->pages_count ; i++) {
        if (i > 0) {
            vma->vm_start = vma->vm_end;
        }
        vma->vm_end = vma->vm_start + PAGE_SIZE;

        page_fn = virt_to_phys(vdma_buffer->pages_address[i]) >> PAGE_SHIFT ;
        err = remap_pfn_range(vma, vma->vm_start, page_fn, PAGE_SIZE, vma->vm_page_prot);

        if (err != 0) {
            hailo_dev_err(controller->dev, " fops_mmap failed mapping kernel page %d\n", err);
            return err;
        }
    }

    vma->vm_start = orig_vm_start;
    vma->vm_end = orig_vm_end;

    return 0;
}

static int continuous_buffer_mmap(struct hailo_vdma_controller *controller,
    struct hailo_vdma_continuous_buffer *buffer, struct vm_area_struct *vma)
{
    int err = 0;
    const unsigned long vsize = vma->vm_end - vma->vm_start;

    if (vsize > buffer->size) {
        hailo_dev_err(controller->dev, "mmap size should be less than %zu (given %lu)\n",
            buffer->size, vsize);
        return -EINVAL;
    }

    err = dma_mmap_coherent(controller->dev, vma, buffer->kernel_address,
        buffer->dma_address, vsize);
    if (err < 0) {
        hailo_dev_err(controller->dev, " vdma_mmap failed dma_mmap_coherent %d\n", err);
        return err;
    }

    return 0;
}

int hailo_vdma_mmap(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller,
    struct vm_area_struct *vma, uintptr_t vdma_handle)
{
    struct hailo_vdma_low_memory_buffer *low_memory_buffer = NULL;
    struct hailo_vdma_continuous_buffer *continuous_buffer = NULL;

    hailo_dev_info(controller->dev, "Map vdma_handle %llu\n", (u64)vdma_handle);
    if (NULL != (low_memory_buffer = hailo_vdma_find_low_memory_buffer(context, vdma_handle)) &&
        low_memory_buffer->owner == context) {
        return low_memory_buffer_mmap(controller, low_memory_buffer, vma);
    }
    else if (NULL != (continuous_buffer = hailo_vdma_find_continuous_buffer(context, vdma_handle)) &&
        continuous_buffer->owner == context) {
        return continuous_buffer_mmap(controller, continuous_buffer, vma);
    }
    else {
        hailo_dev_err(controller->dev, "Can't mmap vdma handle: %llu (not existing)\n", (u64)vdma_handle);
        return -EINVAL;
    }
}

enum dma_data_direction get_dma_direction(enum hailo_dma_data_direction hailo_direction)
{
    switch (hailo_direction) {
    case HAILO_DMA_BIDIRECTIONAL:
        return DMA_BIDIRECTIONAL;
    case HAILO_DMA_TO_DEVICE:
        return DMA_TO_DEVICE;
    case HAILO_DMA_FROM_DEVICE:
        return DMA_FROM_DEVICE;
    default:
        pr_err("Invalid hailo direction %d\n", hailo_direction);
        return DMA_NONE;
    }
}
