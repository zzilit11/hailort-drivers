// SPDX-License-Identifier: MIT
/**
 * Copyright (c) 2019-2026 Hailo Technologies Ltd. All rights reserved.
 **/

#ifndef _HAILO_COMMON_LOGS_H_
#define _HAILO_COMMON_LOGS_H_

#include <linux/kern_levels.h>
#include <linux/device.h>

// This variable is defined by each Linux driver and controls the log level.
extern int o_dbg;

#define hailo_printk(level, dev, fmt, ...)              \
    do {                                                \
        if ((level) <= o_dbg) {                         \
            const char *kern_level = ((level) <= LOGLEVEL_CRIT)    ? KERN_CRIT : \
                                     ((level) == LOGLEVEL_ERR)     ? KERN_ERR  : \
                                     ((level) == LOGLEVEL_WARNING) ? KERN_WARNING : KERN_INFO; \
            dev_printk(kern_level, dev, fmt, ##__VA_ARGS__); \
        }                                               \
    } while (0)

#define hailo_emerg(board, fmt, ...)    hailo_printk(LOGLEVEL_EMERG, &(board)->pdev->dev, fmt, ##__VA_ARGS__)
#define hailo_alert(board, fmt, ...)    hailo_printk(LOGLEVEL_ALERT, &(board)->pdev->dev, fmt, ##__VA_ARGS__)
#define hailo_crit(board, fmt, ...)     hailo_printk(LOGLEVEL_CRIT, &(board)->pdev->dev, fmt, ##__VA_ARGS__)
#define hailo_err(board, fmt, ...)      hailo_printk(LOGLEVEL_ERR, &(board)->pdev->dev, fmt, ##__VA_ARGS__)
#define hailo_warn(board, fmt, ...)     hailo_printk(LOGLEVEL_WARNING, &(board)->pdev->dev, fmt, ##__VA_ARGS__)
#define hailo_notice(board, fmt, ...)   hailo_printk(LOGLEVEL_NOTICE, &(board)->pdev->dev, fmt, ##__VA_ARGS__)
#define hailo_info(board, fmt, ...)     hailo_printk(LOGLEVEL_INFO, &(board)->pdev->dev, fmt, ##__VA_ARGS__)
#define hailo_dbg(board, fmt, ...)      hailo_printk(LOGLEVEL_DEBUG, &(board)->pdev->dev, fmt, ##__VA_ARGS__)

#define hailo_dev_emerg(dev, fmt, ...)  hailo_printk(LOGLEVEL_EMERG, dev, fmt, ##__VA_ARGS__)
#define hailo_dev_alert(dev, fmt, ...)  hailo_printk(LOGLEVEL_ALERT, dev, fmt, ##__VA_ARGS__)
#define hailo_dev_crit(dev, fmt, ...)   hailo_printk(LOGLEVEL_CRIT, dev, fmt, ##__VA_ARGS__)
#define hailo_dev_err(dev, fmt, ...)    hailo_printk(LOGLEVEL_ERR, dev, fmt, ##__VA_ARGS__)
#define hailo_dev_warn(dev, fmt, ...)   hailo_printk(LOGLEVEL_WARNING, dev, fmt, ##__VA_ARGS__)
#define hailo_dev_notice(dev, fmt, ...) hailo_printk(LOGLEVEL_NOTICE, dev, fmt, ##__VA_ARGS__)
#define hailo_dev_info(dev, fmt, ...)   hailo_printk(LOGLEVEL_INFO, dev, fmt, ##__VA_ARGS__)
#define hailo_dev_dbg(dev, fmt, ...)    hailo_printk(LOGLEVEL_DEBUG, dev, fmt, ##__VA_ARGS__)

#endif //_HAILO_COMMON_LOGS_H_
