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

struct hailo_notification_wait {
    struct list_head    notification_wait_list;
    int                 tgid;
    struct file*        filp;
    struct completion 	notification_completion;
    spinlock_t          notification_lock;
    struct hailo_d2h_notification notification;
    u64                 vctx_id;
    bool                has_notification;
    bool                is_disabled;
};

#endif /* _HAILO_LINUX_COMMON_H_ */
