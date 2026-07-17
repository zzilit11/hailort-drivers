// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2025 Hailo Technologies Ltd. All rights reserved.
 **/

#include "ioctl.h"
#include "memory.h"
#include "vctx.h"
#include "utils/logs.h"
#include "utils.h"

#include <linux/slab.h>
#include <linux/uaccess.h>


static uintptr_t hailo_get_next_vdma_handle(struct hailo_vdma_file_context *context)
{
    // Note: The kernel code left-shifts the 'offset' param from the user-space call to mmap by PAGE_SHIFT bits and
    // stores the result in 'vm_area_struct.vm_pgoff'. We pass the desc_handle to mmap in the offset param. To
    // counter this, we right-shift the desc_handle. See also 'mmap function'.
    uintptr_t next_handle = 0;
    next_handle = atomic_inc_return(&context->last_vdma_handle);
    return (next_handle << PAGE_SHIFT);
}

long hailo_vdma_buffer_map_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller,
    unsigned long arg)
{
    struct hailo_vdma_buffer_map_params buf_info;
    struct hailo_vdma_buffer *mapped_buffer = NULL;
    enum dma_data_direction direction = DMA_NONE;
    struct hailo_vdma_low_memory_buffer *low_memory_buffer = NULL;

    if (copy_from_user(&buf_info, (void __user*)arg, sizeof(buf_info))) {
        hailo_dev_err(controller->dev, "copy from user fail\n");
        return -EFAULT;
    }

    hailo_dev_dbg(controller->dev, "address %lx tgid %d size: %zu\n",
        buf_info.user_address, current->tgid, buf_info.size);

    direction = get_dma_direction(buf_info.data_direction);
    if (DMA_NONE == direction) {
        hailo_dev_err(controller->dev, "invalid data direction %d\n", buf_info.data_direction);
        return -EINVAL;
    }

    low_memory_buffer = hailo_vdma_find_low_memory_buffer(context, buf_info.allocated_buffer_handle);
    if (low_memory_buffer && low_memory_buffer->owner != context) {
        return -EPERM;
    }

    mapped_buffer = hailo_vdma_buffer_map(controller->dev,
        buf_info.user_address, buf_info.size, direction, buf_info.buffer_type, low_memory_buffer);
    if (IS_ERR(mapped_buffer)) {
        hailo_dev_err(controller->dev, "failed map buffer %lx\n", buf_info.user_address);
        return PTR_ERR(mapped_buffer);
    }

    mapped_buffer->handle = atomic_inc_return(&context->last_vdma_user_buffer_handle);
    mapped_buffer->owner = context;
    buf_info.mapped_handle = mapped_buffer->handle;
    if (copy_to_user((void __user*)arg, &buf_info, sizeof(buf_info))) {
        hailo_dev_err(controller->dev, "copy_to_user fail\n");
        hailo_vdma_buffer_put(mapped_buffer);
        return -EFAULT;
    }

    list_add(&mapped_buffer->mapped_user_buffer_list, &context->mapped_user_buffer_list);
    hailo_dev_dbg(controller->dev, "buffer %lx (handle %zu) is mapped\n",
        buf_info.user_address, buf_info.mapped_handle);
    return 0;
}

long hailo_vdma_buffer_unmap_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller,
    unsigned long arg)
{
    struct hailo_vdma_buffer *mapped_buffer = NULL;
    struct hailo_vdma_buffer_unmap_params buffer_unmap_params;

    if (copy_from_user(&buffer_unmap_params, (void __user*)arg, sizeof(buffer_unmap_params))) {
        hailo_dev_err(controller->dev, "copy from user fail\n");
        return -EFAULT;
    }

    hailo_dev_dbg(controller->dev, "unmap user buffer handle %zu\n", buffer_unmap_params.mapped_handle);

    mapped_buffer = hailo_vdma_find_mapped_buffer_by_handle(context, buffer_unmap_params.mapped_handle);
    if (NULL == mapped_buffer || mapped_buffer->owner != context) {
        hailo_dev_warn(controller->dev, "buffer handle %zu not found\n", buffer_unmap_params.mapped_handle);
        return -EINVAL;
    }

    list_del(&mapped_buffer->mapped_user_buffer_list);
    hailo_vdma_buffer_put(mapped_buffer);
    return 0;
}

long hailo_vdma_buffer_sync_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller, unsigned long arg)
{
    struct hailo_vdma_buffer_sync_params sync_info = {};
    struct hailo_vdma_buffer *mapped_buffer = NULL;

    if (copy_from_user(&sync_info, (void __user*)arg, sizeof(sync_info))) {
        hailo_dev_err(controller->dev, "copy_from_user fail\n");
        return -EFAULT;
    }

    if (!(mapped_buffer = hailo_vdma_find_mapped_buffer_by_handle(context, sync_info.handle)) ||
        mapped_buffer->owner != context) {
        hailo_dev_err(controller->dev, "buffer handle %zu doesn't exist\n", sync_info.handle);
        return -EINVAL;
    }

    if ((sync_info.sync_type != HAILO_SYNC_FOR_CPU) && (sync_info.sync_type != HAILO_SYNC_FOR_DEVICE)) {
        hailo_dev_err(controller->dev, "Invalid sync_type given for vdma buffer sync.\n");
        return -EINVAL;
    }

    if (sync_info.offset + sync_info.count > mapped_buffer->size) {
        hailo_dev_err(controller->dev, "Invalid offset/count given for vdma buffer sync. offset %zu count %zu buffer size %u\n",
            sync_info.offset, sync_info.count, mapped_buffer->size);
        return -EINVAL;
    }

    hailo_vdma_buffer_sync(controller, mapped_buffer, sync_info.sync_type,
        sync_info.offset, sync_info.count);
    return 0;
}

long hailo_desc_list_create_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller,
    unsigned long arg)
{
    struct hailo_desc_list_create_params params;
    struct hailo_descriptors_list_buffer *descriptors_buffer = NULL;
    uintptr_t next_handle = 0;
    long err = -EINVAL;

    if (copy_from_user(&params, (void __user*)arg, sizeof(params))) {
        hailo_dev_err(controller->dev, "copy_from_user fail\n");
        return -EFAULT;
    }

    if (params.is_circular && !is_powerof2(params.desc_count)) {
        hailo_dev_err(controller->dev, "Invalid desc count given : %zu , circular descriptors count must be power of 2\n",
            params.desc_count);
        return -EINVAL;
    }

    if (!is_powerof2(params.desc_page_size)) {
        hailo_dev_err(controller->dev, "Invalid desc page size given : %u\n",
            params.desc_page_size);
        return -EINVAL;
    }

    hailo_dev_info(controller->dev,
        "Create desc list desc_count: %zu desc_page_size: %u\n",
        params.desc_count, params.desc_page_size);

    descriptors_buffer = kzalloc(sizeof(*descriptors_buffer), GFP_KERNEL);
    if (NULL == descriptors_buffer) {
        hailo_dev_err(controller->dev, "Failed to allocate buffer for descriptors list struct\n");
        return -ENOMEM;
    }

    next_handle = hailo_get_next_vdma_handle(context);

    err = hailo_desc_list_create(controller->dev, params.desc_count, params.desc_page_size, next_handle,
        params.is_circular, descriptors_buffer);
    if (err < 0) {
        hailo_dev_err(controller->dev, "failed to allocate descriptors buffer\n");
        kfree(descriptors_buffer);
        return err;
    }

    descriptors_buffer->owner = context;
    descriptors_buffer->release_struct = true;

    list_add(&descriptors_buffer->descriptors_buffer_list, &context->descriptors_buffer_list);

    params.desc_handle = descriptors_buffer->handle;

    if(copy_to_user((void __user*)arg, &params, sizeof(params))){
        hailo_dev_err(controller->dev, "copy_to_user fail\n");
        list_del(&descriptors_buffer->descriptors_buffer_list);
        hailo_desc_list_put(descriptors_buffer);
        return -EFAULT;
    }

    hailo_dev_info(controller->dev, "Created desc list, handle 0x%llu\n",
        (u64)params.desc_handle);
    return 0;
}

long hailo_desc_list_release_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller,
    unsigned long arg)
{
    struct hailo_desc_list_release_params params;
    struct hailo_descriptors_list_buffer *descriptors_buffer = NULL;

    if (copy_from_user(&params, (void __user*)arg, sizeof(params))) {
        hailo_dev_err(controller->dev, "copy_from_user fail\n");
        return -EFAULT;
    }

    descriptors_buffer = hailo_vdma_find_descriptors_buffer(context, params.desc_handle);
    if (descriptors_buffer == NULL || descriptors_buffer->owner != context) {
        hailo_dev_warn(controller->dev, "not found desc handle %llu\n", (unsigned long long)params.desc_handle);
        return -EINVAL;
    }

    list_del(&descriptors_buffer->descriptors_buffer_list);
    hailo_desc_list_put(descriptors_buffer);
    return 0;
}

long hailo_desc_list_program_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller,
    unsigned long arg)
{
    struct hailo_desc_list_program_params configure_info;
    struct hailo_vdma_buffer *mapped_buffer = NULL;
    struct hailo_descriptors_list_buffer *descriptors_buffer = NULL;
    struct hailo_vdma_mapped_transfer_buffer transfer_buffer = {0};

    if (copy_from_user(&configure_info, (void __user*)arg, sizeof(configure_info))) {
        hailo_dev_err(controller->dev, "copy from user fail\n");
        return -EFAULT;
    }
    hailo_dev_info(controller->dev, "config buffer_handle=%zu desc_handle=%llu starting_desc=%u\n",
        configure_info.buffer_handle, (u64)configure_info.desc_handle, configure_info.starting_desc);

    mapped_buffer = hailo_vdma_find_mapped_buffer_by_handle(context, configure_info.buffer_handle);
    descriptors_buffer = hailo_vdma_find_descriptors_buffer(context, configure_info.desc_handle);
    if (mapped_buffer == NULL || descriptors_buffer == NULL ||
        mapped_buffer->owner != context || descriptors_buffer->owner != context) {
        hailo_dev_err(controller->dev, "invalid user/descriptors buffer\n");
        return -EFAULT;
    }

    if (configure_info.buffer_size > mapped_buffer->size) {
        hailo_dev_err(controller->dev, "invalid buffer size. \n");
        return -EFAULT;
    }

    transfer_buffer.sg_table = &mapped_buffer->sg_table;
    transfer_buffer.size = configure_info.buffer_size;
    transfer_buffer.offset = configure_info.buffer_offset;

    return hailo_vdma_program_descriptors_list_batch(
        controller->hw,
        &descriptors_buffer->desc_list,
        configure_info.starting_desc,
        &mapped_buffer->sg_table,
        configure_info.buffer_offset,
        configure_info.buffer_size,
        configure_info.batch_size,
        configure_info.should_bind,
        configure_info.channel_index,
        configure_info.last_interrupts_domain,
        configure_info.is_debug,
        configure_info.stride
    );
}

long hailo_vdma_low_memory_buffer_alloc_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller,
    unsigned long arg)
{
    struct hailo_allocate_low_memory_buffer_params buf_info = {0};
    struct hailo_vdma_low_memory_buffer *low_memory_buffer = NULL;
    long err = -EINVAL;

    if (copy_from_user(&buf_info, (void __user*)arg, sizeof(buf_info))) {
        hailo_dev_err(controller->dev, "copy from user fail\n");
        return -EFAULT;
    }

    low_memory_buffer = kzalloc(sizeof(*low_memory_buffer), GFP_KERNEL);
    if (NULL == low_memory_buffer) {
        hailo_dev_err(controller->dev, "memory alloc failed\n");
        return -ENOMEM;
    }

    err = hailo_vdma_low_memory_buffer_alloc(buf_info.buffer_size, low_memory_buffer);
    if (err < 0) {
        kfree(low_memory_buffer);
        hailo_dev_err(controller->dev, "failed allocating buffer from driver\n");
        return err;
    }

    // Get handle for allocated buffer
    low_memory_buffer->handle = hailo_get_next_vdma_handle(context);
    low_memory_buffer->owner = context;

    list_add(&low_memory_buffer->vdma_low_memory_buffer_list, &context->vdma_low_memory_buffer_list);

    buf_info.buffer_handle = low_memory_buffer->handle;
    if (copy_to_user((void __user*)arg, &buf_info, sizeof(buf_info))) {
        hailo_dev_err(controller->dev, "copy_to_user fail\n");
        list_del(&low_memory_buffer->vdma_low_memory_buffer_list);
        hailo_vdma_low_memory_buffer_put(low_memory_buffer);
        return -EFAULT;
    }

    return 0;
}

long hailo_vdma_low_memory_buffer_free_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller,
    unsigned long arg)
{
    struct hailo_vdma_low_memory_buffer *low_memory_buffer = NULL;
    struct hailo_free_low_memory_buffer_params params = {0};

    if (copy_from_user(&params, (void __user*)arg, sizeof(params))) {
        hailo_dev_err(controller->dev, "copy from user fail\n");
        return -EFAULT;
    }

    low_memory_buffer = hailo_vdma_find_low_memory_buffer(context, params.buffer_handle);
    if (NULL == low_memory_buffer || low_memory_buffer->owner != context) {
        hailo_dev_warn(controller->dev, "vdma buffer handle %lx not found\n", params.buffer_handle);
        return -EINVAL;
    }

    list_del(&low_memory_buffer->vdma_low_memory_buffer_list);
    hailo_vdma_low_memory_buffer_put(low_memory_buffer);
    return 0;
}

long hailo_mark_as_in_use(struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller, unsigned long arg)
{
    struct hailo_mark_as_in_use_params params = {0};
    unsigned long flags;
    bool newly_registered = false;

    spin_lock_irqsave(&context->vctx.lock, flags);
    if (context->vctx.state != HAILO_VDMA_VCTX_ACTIVE ||
        context->vctx.fw_state == HAILO_VDMA_VCTX_FW_ERROR) {
        params.in_use = true;
    } else if (!context->vctx.resource_registered) {
        context->vctx.resource_registered = true;
        atomic_inc(&controller->registered_vctx_count);
        newly_registered = true;
        params.in_use = false;
    } else {
        params.in_use = false;
    }
    spin_unlock_irqrestore(&context->vctx.lock, flags);

    if (newly_registered) {
        hailo_dev_notice(controller->dev,
            "vctx-fw: register vctx=%llu generation=%llu registered=%d\n",
            (unsigned long long)context->vctx.vctx_id,
            (unsigned long long)context->vctx.generation,
            atomic_read(&controller->registered_vctx_count));
        hailo_vdma_vctx_fw_state_changed(&context->vctx);
    }

    if (copy_to_user((void __user*)arg, &params, sizeof(params))) {
        hailo_dev_err(controller->dev, "copy_to_user fail\n");
        if (newly_registered) {
            bool rollback_registration;

            spin_lock_irqsave(&context->vctx.lock, flags);
            rollback_registration = context->vctx.resource_registered;
            if (rollback_registration) {
                context->vctx.resource_registered = false;
            }
            spin_unlock_irqrestore(&context->vctx.lock, flags);
            if (rollback_registration) {
                atomic_dec(&controller->registered_vctx_count);
                hailo_vdma_vctx_fw_state_changed(&context->vctx);
            }
        }
        return -EFAULT;
    }

    return 0;
}

long hailo_vdma_continuous_buffer_alloc_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller, unsigned long arg)
{
    struct hailo_allocate_continuous_buffer_params buf_info = {0};
    struct hailo_vdma_continuous_buffer *continuous_buffer = NULL;
    long err = -EINVAL;
    size_t aligned_buffer_size = 0;

    if (copy_from_user(&buf_info, (void __user*)arg, sizeof(buf_info))) {
        hailo_dev_err(controller->dev, "copy from user fail\n");
        return -EFAULT;
    }

    continuous_buffer = kzalloc(sizeof(*continuous_buffer), GFP_KERNEL);
    if (NULL == continuous_buffer) {
        hailo_dev_err(controller->dev, "memory alloc failed\n");
        return -ENOMEM;
    }

    // We use PAGE_ALIGN to support mmap
    aligned_buffer_size = PAGE_ALIGN(buf_info.buffer_size);
    err = hailo_vdma_continuous_buffer_alloc(controller->dev, aligned_buffer_size, continuous_buffer);
    if (err < 0) {
        kfree(continuous_buffer);
        return err;
    }

    continuous_buffer->handle = hailo_get_next_vdma_handle(context);
    continuous_buffer->owner = context;
    list_add(&continuous_buffer->continuous_buffer_list, &context->continuous_buffer_list);

    buf_info.buffer_handle = continuous_buffer->handle;
    buf_info.dma_address = continuous_buffer->dma_address;
    if (copy_to_user((void __user*)arg, &buf_info, sizeof(buf_info))) {
        hailo_dev_err(controller->dev, "copy_to_user fail\n");
        list_del(&continuous_buffer->continuous_buffer_list);
        hailo_vdma_continuous_buffer_free(controller->dev, continuous_buffer);
        kfree(continuous_buffer);
        return -EFAULT;
    }

    return 0;
}

long hailo_vdma_continuous_buffer_free_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller, unsigned long arg)
{
    struct hailo_free_continuous_buffer_params params;
    struct hailo_vdma_continuous_buffer *continuous_buffer = NULL;

    if (copy_from_user(&params, (void __user*)arg, sizeof(params))) {
        hailo_dev_err(controller->dev, "copy from user fail\n");
        return -EFAULT;
    }

    continuous_buffer = hailo_vdma_find_continuous_buffer(context, params.buffer_handle);
    if (NULL == continuous_buffer || continuous_buffer->owner != context) {
        hailo_dev_warn(controller->dev, "vdma buffer handle %lx not found\n", params.buffer_handle);
        return -EINVAL;
    }

    list_del(&continuous_buffer->continuous_buffer_list);
    hailo_vdma_continuous_buffer_free(controller->dev, continuous_buffer);
    kfree(continuous_buffer);
    return 0;
}

long hailo_vdma_interrupts_read_timestamps_ioctl(struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller, unsigned long arg)
{
    struct hailo_vdma_interrupts_read_timestamp_params *params = &controller->read_interrupt_timestamps_params;
    struct hailo_vdma_engine *engine = NULL;
    struct hailo_vdma_channel_context *channel_context = NULL;
    int err = -EINVAL;

    hailo_dev_dbg(controller->dev, "Start read interrupt timestamps ioctl\n");

    if (copy_from_user(params, (void __user*)arg, sizeof(*params))) {
        hailo_dev_err(controller->dev, "copy_from_user fail\n");
        return -ENOMEM;
    }

    if (params->engine_index >= controller->vdma_engines_count) {
        hailo_dev_err(controller->dev, "Invalid engine %u", params->engine_index);
        return -EINVAL;
    }
    engine = &controller->vdma_engines[params->engine_index];
    if (params->channel_index >= MAX_VDMA_CHANNELS_PER_ENGINE) {
        return -EINVAL;
    }
    channel_context = &controller->channel_contexts[params->engine_index][params->channel_index];
    mutex_lock(&channel_context->lock);
    if (channel_context->owner != &context->vctx) {
        mutex_unlock(&channel_context->lock);
        return -EPERM;
    }

    err = hailo_vdma_engine_read_timestamps(engine, params);
    if (err < 0) {
        mutex_unlock(&channel_context->lock);
        hailo_dev_err(controller->dev, "Failed read engine interrupts for %u:%u",
            params->engine_index, params->channel_index);
        return err;
    }
    mutex_unlock(&channel_context->lock);

    if (copy_to_user((void __user*)arg, params, sizeof(*params))) {
        hailo_dev_err(controller->dev, "copy_to_user fail\n");
        return -ENOMEM;
    }

    return 0;
}
