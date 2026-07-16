// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2025 Hailo Technologies Ltd. All rights reserved.
 **/
/**
 * Hailo vdma engine definitions
 */

#ifndef _HAILO_VDMA_VDMA_H_
#define _HAILO_VDMA_VDMA_H_

#include "hailo_ioctl_common.h"
#include "hailo_resource.h"
#include "vdma_common.h"

#include <linux/dma-mapping.h>
#include <linux/types.h>
#include <linux/semaphore.h>
#include <linux/dma-buf.h>
#include <linux/version.h>
#include <linux/completion.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>

#define VDMA_CHANNEL_CONTROL_REG_OFFSET(channel_index, direction) (((direction) == DMA_TO_DEVICE) ? \
            (((channel_index) << 5) + 0x0) : (((channel_index) << 5) + 0x10))
#define VDMA_CHANNEL_CONTROL_REG_ADDRESS(vdma_registers, channel_index, direction) \
    ((u8*)((vdma_registers)->address) + VDMA_CHANNEL_CONTROL_REG_OFFSET(channel_index, direction))

#define VDMA_CHANNEL_NUM_PROC_OFFSET(channel_index, direction) (((direction) == DMA_TO_DEVICE) ? \
            (((channel_index) << 5) + 0x4) : (((channel_index) << 5) + 0x14))
#define VDMA_CHANNEL_NUM_PROC_ADDRESS(vdma_registers, channel_index, direction) \
    ((u8*)((vdma_registers)->address) + VDMA_CHANNEL_NUM_PROC_OFFSET(channel_index, direction))

#define DMA_DIRECTION_EQUALS(a, b) (a == DMA_BIDIRECTIONAL || b == DMA_BIDIRECTIONAL || a == b)

struct hailo_vdma_file_context;
struct hailo_vdma_low_memory_buffer;

// dmabuf is supported from linux kernel version 3.3
#if LINUX_VERSION_CODE < KERNEL_VERSION( 3, 3, 0 )
// Make dummy struct with one byte (C standards does not allow empty struct) - in order to not have to ifdef everywhere
struct hailo_dmabuf_info {
    uint8_t dummy;
};
#else
// dmabuf_sg_table is needed because in dma_buf_unmap_attachment() the sg_table's address has to match the
// The one returned from dma_buf_map_attachment() - otherwise we would need to malloc each time
struct hailo_dmabuf_info {
    struct dma_buf *dmabuf;
    struct dma_buf_attachment *dmabuf_attachment;
    struct sg_table *dmabuf_sg_table;
};
#endif // LINUX_VERSION_CODE < KERNEL_VERSION( 3, 3, 0 )

struct hailo_vdma_buffer {
    struct list_head            mapped_user_buffer_list;
    size_t                      handle;

    struct kref                 kref;
    struct device               *device;
    struct hailo_vdma_file_context *owner;
    struct hailo_vdma_low_memory_buffer *low_memory_buffer;

    enum hailo_dma_buffer_type  buffer_type;
    u32                         size;
    uintptr_t                   addr_or_fd;
    enum dma_data_direction     data_direction;

    struct sg_table             sg_table;

    // If this flag is set, the buffer pointed by sg_table is not backed by
    // 'struct page' (only by pure pfn). On this case, accessing to the page,
    // or calling APIs that access the page (e.g. dma_sync_sg_for_cpu) is not
    // allowed.
    bool                        is_mmio;

    // Relevant paramaters that need to be saved in case of dmabuf - otherwise struct pointers will be NULL
    struct hailo_dmabuf_info    dmabuf_info;
};

// Continuous buffer that holds a descriptor list.
struct hailo_descriptors_list_buffer {
    struct list_head                   descriptors_buffer_list;
    uintptr_t                          handle;
    void                               *kernel_address;
    dma_addr_t                         dma_address;
    u32                                buffer_size;
    struct hailo_vdma_descriptors_list desc_list;
    struct kref                        kref;
    struct device                      *device;
    struct hailo_vdma_file_context     *owner;
    bool                               release_struct;
};

struct hailo_vdma_low_memory_buffer {
    struct list_head                    vdma_low_memory_buffer_list;
    struct kref                         kref;
    struct hailo_vdma_file_context      *owner;
    uintptr_t                           handle;
    size_t                              pages_count;
    void                                **pages_address;
};

struct hailo_vdma_continuous_buffer {
    struct list_head    continuous_buffer_list;
    uintptr_t           handle;
    struct hailo_vdma_file_context *owner;
    void                *kernel_address;
    dma_addr_t          dma_address;
    size_t              size;
};

struct hailo_vdma_controller;

enum hailo_vdma_vctx_state {
    HAILO_VDMA_VCTX_ACTIVE = 0,
    HAILO_VDMA_VCTX_CLOSING,
    HAILO_VDMA_VCTX_DEAD,
};

enum hailo_vdma_vctx_fw_state {
    HAILO_VDMA_VCTX_FW_UNCONFIGURED = 0,
    HAILO_VDMA_VCTX_FW_CONFIGURING,
    HAILO_VDMA_VCTX_FW_CONFIGURED,
    HAILO_VDMA_VCTX_FW_RUNNABLE,
    HAILO_VDMA_VCTX_FW_QUIESCING,
    HAILO_VDMA_VCTX_FW_ERROR,
};

struct hailo_vdma_vctx_event {
    u32 completed_count;
    bool channel_error;
    bool channel_inactive;
    bool disable_wakeup;
};

struct hailo_vdma_vctx {
    u64 vctx_id;
    u64 generation;
    enum hailo_vdma_vctx_state state;
    struct kref refcount;
    struct completion refs_zero;
    spinlock_t lock;
    wait_queue_head_t events_wq;
    atomic64_t next_transfer_sequence;
    atomic_t transfer_count;
    u32 transfer_quota;
    bool cancel_requested;
    bool resource_registered;
    bool enable_timestamps_measure;
    enum hailo_vdma_vctx_fw_state fw_state;
    u64 fw_epoch;
    u32 logical_channels_bitmap[MAX_VDMA_ENGINES];
    struct hailo_vdma_channel_state channel_states[MAX_VDMA_ENGINES][MAX_VDMA_CHANNELS_PER_ENGINE];
    bool channel_state_valid[MAX_VDMA_ENGINES][MAX_VDMA_CHANNELS_PER_ENGINE];
    u8 application_count;
    u8 application_map[CONTROL_PROTOCOL__MAX_CONTEXT_SWITCH_APPLICATIONS];
    bool activation_valid;
    u32 activation_request_len;
    CONTROL_PROTOCOL__request_t activation_request;
    struct list_head queued_transfers;
    struct list_head ongoing_transfers;
    struct list_head completed_transfers[MAX_VDMA_ENGINES][MAX_VDMA_CHANNELS_PER_ENGINE];
    struct hailo_vdma_vctx_event events[MAX_VDMA_ENGINES][MAX_VDMA_CHANNELS_PER_ENGINE];
    struct hailo_vdma_controller *controller;
};

struct hailo_vdma_channel_context {
    struct mutex lock;
    spinlock_t admission_lock;
    wait_queue_head_t admission_wq;
    struct list_head admission_queue;
    struct hailo_vdma_vctx *owner;
    u64 owner_generation;
    enum hailo_vdma_vctx_fw_state owner_fw_state;
    bool owner_resource_registered;
    bool owner_active;
    struct hailo_descriptors_list_buffer *bound_descriptors;
    atomic_t ongoing_count;
    u32 logical_users;
    u64 dispatch_sequence;
    u64 last_dispatched_vctx_id;
    bool enabled;
    bool shutting_down;
};

struct hailo_vdma_controller_ops {
    void (*update_channel_interrupts)(struct hailo_vdma_controller *controller, size_t engine_index,
        u32 channels_bitmap);
    int (*activate_vctx)(struct hailo_vdma_controller *controller,
        struct hailo_vdma_vctx *vctx);
};

struct hailo_vdma_controller {
    struct hailo_vdma_hw *hw;
    struct hailo_vdma_controller_ops *ops;
    struct device *dev;

    size_t vdma_engines_count;
    struct hailo_vdma_engine *vdma_engines;

    spinlock_t interrupts_lock;
    wait_queue_head_t interrupts_wq;

    atomic64_t last_vctx_id;
    struct work_struct completion_work;
    bool completion_stopped;
    struct delayed_work stall_monitor_work;
    bool stall_monitor_stopped;
    struct hailo_vdma_channel_context channel_contexts[MAX_VDMA_ENGINES][MAX_VDMA_CHANNELS_PER_ENGINE];
    struct mutex dispatch_lock;
    atomic_t total_ongoing_count;
    atomic64_t dispatched_vctx_id;
    atomic64_t dispatched_generation;
    atomic64_t notification_vctx_id;

    atomic_t registered_vctx_count;

    // Putting big IOCTL structures here to avoid stack allocation.
    struct hailo_vdma_interrupts_read_timestamp_params read_interrupt_timestamps_params;
};

#define for_each_vdma_engine(controller, engine, engine_index)                          \
    _for_each_element_array(controller->vdma_engines, controller->vdma_engines_count,   \
        engine, engine_index)

struct hailo_vdma_file_context {
    struct hailo_vdma_vctx vctx;
    atomic_t last_vdma_user_buffer_handle;
    struct list_head mapped_user_buffer_list;

    // Last_vdma_handle works as a handle for vdma decriptor list and for the vdma buffer -
    // there will be no collisions between the two
    atomic_t last_vdma_handle;
    struct list_head descriptors_buffer_list;
    struct list_head vdma_low_memory_buffer_list;
    struct list_head continuous_buffer_list;
    u32 enabled_channels_bitmap[MAX_VDMA_ENGINES];
};


int hailo_vdma_controller_init(struct hailo_vdma_controller *controller,
    struct device *dev, struct hailo_vdma_hw *vdma_hw,
    struct hailo_vdma_controller_ops *ops,
    struct hailo_resource *channel_registers_per_engine, size_t engines_count);
void hailo_vdma_controller_cleanup(struct hailo_vdma_controller *controller);
void hailo_vdma_controller_reset(struct hailo_vdma_controller *controller);

void hailo_vdma_update_interrupts_mask(struct hailo_vdma_controller *controller,
    size_t engine_index);

void hailo_vdma_file_context_init(struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller);
void hailo_vdma_file_context_finalize(struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller, struct file *filp);

void hailo_vdma_wakeup_interrupts(struct hailo_vdma_controller *controller, struct hailo_vdma_engine *engine, 
    u32 channels_bitmap);
void hailo_vdma_irq_handler(struct hailo_vdma_controller *controller, size_t engine_index,
    u32 channels_bitmap);

void hailo_vdma_vctx_get(struct hailo_vdma_vctx *vctx);
void hailo_vdma_vctx_put(struct hailo_vdma_vctx *vctx);
bool hailo_vdma_vctx_unregister(struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller);

// TODO: reduce params count
long hailo_vdma_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller,
    unsigned int cmd, unsigned long arg, struct file *filp, struct semaphore *mutex, bool *should_up_board_mutex);

int hailo_vdma_mmap(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller,
    struct vm_area_struct *vma, uintptr_t vdma_handle);

enum dma_data_direction get_dma_direction(enum hailo_dma_data_direction hailo_direction);
void hailo_vdma_disable_vdma_channels(struct hailo_vdma_controller *controller, const bool should_close_channels);

#endif /* _HAILO_VDMA_VDMA_H_ */
