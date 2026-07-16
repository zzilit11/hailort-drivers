// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2025 Hailo Technologies Ltd. All rights reserved.
 **/

#ifndef _HAILO_VDMA_VCTX_H_
#define _HAILO_VDMA_VCTX_H_

#include "vdma.h"

struct semaphore;

long hailo_vdma_vctx_enable_channels(struct hailo_vdma_controller *controller,
    unsigned long arg, struct hailo_vdma_file_context *context);
long hailo_vdma_vctx_disable_channels(struct hailo_vdma_controller *controller,
    unsigned long arg, struct hailo_vdma_file_context *context);
long hailo_vdma_vctx_wait(struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller, unsigned long arg,
    struct semaphore *board_mutex, bool *should_up_board_mutex);
long hailo_vdma_vctx_launch(struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller, unsigned long arg,
    struct semaphore *board_mutex, bool *should_up_board_mutex);

void hailo_vdma_vctx_trace_created(struct hailo_vdma_vctx *vctx);
void hailo_vdma_vctx_completion_work(struct work_struct *work);
void hailo_vdma_vctx_fw_state_changed(struct hailo_vdma_vctx *vctx);
void hailo_vdma_vctx_finalize(struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller);
void hailo_vdma_vctx_controller_quiesce(struct hailo_vdma_controller *controller);
void hailo_vdma_vctx_controller_reset(struct hailo_vdma_controller *controller);

#endif /* _HAILO_VDMA_VCTX_H_ */
