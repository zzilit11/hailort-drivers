// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2025 Hailo Technologies Ltd. All rights reserved.
 **/

#ifndef _HAILO_LINUX_COMMON_H_
#define _HAILO_LINUX_COMMON_H_

#include "hailo_ioctl_common.h"

#include <linux/completion.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#define HAILO_NOTIFICATION_QUEUE_DEPTH (4)

struct hailo_vdma_vctx;

struct hailo_notification_wait {
    struct list_head    notification_wait_list;
    int                 tgid;
    struct file*        filp;
    struct completion 	notification_completion;
    spinlock_t          notification_lock;
    struct hailo_d2h_notification notifications[HAILO_NOTIFICATION_QUEUE_DEPTH];
    struct hailo_vdma_vctx *vctx;
    u64                 vctx_id;
    u16                 notification_read_index;
    u16                 notification_write_index;
    u16                 notification_count;
    u32                 dropped_notifications;
    bool                is_disabled;
};

#endif /* _HAILO_LINUX_COMMON_H_ */
