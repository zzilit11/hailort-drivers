// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2025 Hailo Technologies Ltd. All rights reserved.
 **/
/**
 * @file type_utils.h
 * @brief Defines common types.
**/

#ifndef __TYPE_UTILS_H__
#define __TYPE_UTILS_H__

#ifdef __KERNEL__ /* Common Types for Linux Kernel */

#include <linux/types.h>
#include <linux/limits.h>
#include <linux/string.h>

#define UINT8_MAX U8_MAX

#else /* Common Types for Linux Userspace */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limits.h>
#include <string.h>

#endif

#include "stdfloat.h"

#endif /* __TYPE_UTILS_H__ */
