// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2025 Hailo Technologies Ltd. All rights reserved.
 **/
/**
 * A Hailo PCIe NNC device is a device contains a NNC (neural network core) and some basic FW.
 * The device supports sending controls, receiving notification and reading the FW log.
 */

#include "nnc.h"
#include "hailo_ioctl_common.h"

#include "utils/logs.h"
#include "utils/compact.h"
#include "vdma/memory.h"
#include "vdma/vctx.h"

#include <linux/uaccess.h>

#if !defined(HAILO_EMULATOR)
#define DEFAULT_SHUTDOWN_TIMEOUT_MS (5)
#else /* !defined(HAILO_EMULATOR) */
#define DEFAULT_SHUTDOWN_TIMEOUT_MS (1000)
#endif /* !defined(HAILO_EMULATOR) */

void hailo_nnc_init(struct hailo_pcie_nnc *nnc)
{
    sema_init(&nnc->fw_control.mutex, 1);
    init_waitqueue_head(&nnc->fw_control.owner_wq);
    spin_lock_init(&nnc->notification_read_spinlock);
    init_completion(&nnc->fw_control.completion);
    nnc->fw_control.device_state = HAILO_NNC_DEVICE_COLD;
    nnc->fw_control.initialization_owner = NULL;
    nnc->fw_control.initialization_generation = 0;
    nnc->fw_control.configuration_owner = NULL;
    nnc->fw_control.configuration_generation = 0;
    nnc->fw_control.active_owner = NULL;
    nnc->fw_control.active_generation = 0;
    nnc->fw_control.active_global_application = 0xff;
    nnc->fw_control.active_epoch = 0;
    nnc->fw_control.expected_contexts = 0;
    nnc->fw_control.completed_contexts = 0;
    nnc->fw_control.context_chunk_open = false;
    nnc->fw_control.pending_local_application = 0;
    nnc->fw_control.pending_global_application = 0;
    nnc->fw_control.next_global_application = 0;
    nnc->fw_control.pending_application_reserved = false;
    nnc->fw_control.configuration_epoch = 0;
    INIT_LIST_HEAD(&nnc->notification_wait_list);
    memset(&nnc->notification_cache, 0, sizeof(nnc->notification_cache));
}

struct hailo_fw_vctx_operation {
    u32 opcode;
    bool suppress;
    bool initialization_reset;
    bool clear_configured_apps;
    bool configuration_header;
    bool configuration_context_info;
    bool context_first_chunk;
    bool context_last_chunk;
    bool runtime_reset;
    bool mapped_context_switch_status;
    bool mapped_hw_infer_status;
    bool deferred_activation;
};

static int hailo_nnc_pause_active_vctx_locked(struct hailo_pcie_board *board);

/*
 * fw_control.mutex protects both the shared command buffer and this KMD-only
 * firmware namespace. Configuration ownership survives across ioctls until
 * every context chunk described by the application header has arrived.
 */

static bool hailo_fw_vctx_owner_conflicts(struct hailo_fw_control_info *fw_control,
    struct hailo_vdma_vctx *vctx)
{
    return (fw_control->initialization_owner && fw_control->initialization_owner != vctx) ||
        (fw_control->configuration_owner && fw_control->configuration_owner != vctx);
}

static bool hailo_fw_vctx_owner_available(struct hailo_fw_control_info *fw_control,
    struct hailo_vdma_vctx *vctx)
{
    return !READ_ONCE(fw_control->initialization_owner) ||
        READ_ONCE(fw_control->initialization_owner) == vctx;
}

static bool hailo_fw_vctx_configuration_available(struct hailo_fw_control_info *fw_control,
    struct hailo_vdma_vctx *vctx)
{
    return !READ_ONCE(fw_control->configuration_owner) ||
        READ_ONCE(fw_control->configuration_owner) == vctx;
}

static bool hailo_fw_vctx_wait_condition(struct hailo_fw_control_info *fw_control,
    struct hailo_vdma_vctx *vctx)
{
    return hailo_fw_vctx_owner_available(fw_control, vctx) &&
        hailo_fw_vctx_configuration_available(fw_control, vctx);
}

static void hailo_fw_vctx_set_state(struct hailo_vdma_vctx *vctx,
    enum hailo_vdma_vctx_fw_state state)
{
    unsigned long flags;

    spin_lock_irqsave(&vctx->lock, flags);
    vctx->fw_state = state;
    vctx->fw_epoch++;
    spin_unlock_irqrestore(&vctx->lock, flags);
    hailo_vdma_vctx_fw_state_changed(vctx);
}

static int hailo_fw_vctx_get_application(struct hailo_vdma_vctx *vctx,
    u32 local_application, u8 *global_application)
{
    unsigned long flags;
    int err = 0;

    spin_lock_irqsave(&vctx->lock, flags);
    if (local_application >= vctx->application_count ||
        vctx->application_map[local_application] == 0xff) {
        err = -EINVAL;
    } else {
        *global_application = vctx->application_map[local_application];
    }
    spin_unlock_irqrestore(&vctx->lock, flags);
    return err;
}

static u8 hailo_fw_vctx_application_count(struct hailo_vdma_vctx *vctx)
{
    unsigned long flags;
    u8 count;

    spin_lock_irqsave(&vctx->lock, flags);
    count = vctx->application_count;
    spin_unlock_irqrestore(&vctx->lock, flags);
    return count;
}

static void hailo_fw_vctx_clear_applications(struct hailo_vdma_vctx *vctx)
{
    unsigned long flags;

    spin_lock_irqsave(&vctx->lock, flags);
    vctx->application_count = 0;
    memset(vctx->application_map, 0xff, sizeof(vctx->application_map));
    vctx->activation_valid = false;
    vctx->activation_request_len = 0;
    vctx->fw_state = HAILO_VDMA_VCTX_FW_UNCONFIGURED;
    vctx->fw_epoch++;
    spin_unlock_irqrestore(&vctx->lock, flags);
    hailo_vdma_vctx_fw_state_changed(vctx);
}

static void hailo_fw_vctx_clear_activation(struct hailo_vdma_vctx *vctx)
{
    unsigned long flags;

    spin_lock_irqsave(&vctx->lock, flags);
    vctx->activation_valid = false;
    vctx->activation_request_len = 0;
    spin_unlock_irqrestore(&vctx->lock, flags);
}

static void hailo_fw_vctx_release_active_owner(struct hailo_pcie_board *board)
{
    struct hailo_fw_control_info *fw_control = &board->nnc.fw_control;
    struct hailo_vdma_vctx *owner = fw_control->active_owner;

    fw_control->active_owner = NULL;
    fw_control->active_generation = 0;
    fw_control->active_global_application = 0xff;
    fw_control->active_epoch++;
    atomic64_set(&board->vdma.dispatched_vctx_id, 0);
    atomic64_set(&board->vdma.dispatched_generation, 0);
    atomic64_set(&board->vdma.notification_vctx_id, 0);
    if (owner) {
        hailo_vdma_vctx_put(owner);
    }
    hailo_vdma_vctx_wake_all_admission(&board->vdma);
}

static void hailo_fw_vctx_release_initialization_owner(struct hailo_fw_control_info *fw_control)
{
    struct hailo_vdma_vctx *owner = fw_control->initialization_owner;

    fw_control->initialization_owner = NULL;
    fw_control->initialization_generation = 0;
    if (owner) {
        hailo_vdma_vctx_put(owner);
    }
    wake_up_interruptible_all(&fw_control->owner_wq);
}

static void hailo_fw_vctx_release_configuration_owner(struct hailo_fw_control_info *fw_control)
{
    struct hailo_vdma_vctx *owner = fw_control->configuration_owner;

    fw_control->configuration_owner = NULL;
    fw_control->configuration_generation = 0;
    fw_control->expected_contexts = 0;
    fw_control->completed_contexts = 0;
    fw_control->context_chunk_open = false;
    fw_control->pending_application_reserved = false;
    if (owner) {
        hailo_vdma_vctx_put(owner);
    }
    wake_up_interruptible_all(&fw_control->owner_wq);
}

static int hailo_fw_vctx_claim_initialization(struct hailo_fw_control_info *fw_control,
    struct hailo_vdma_vctx *vctx)
{
    if (fw_control->device_state == HAILO_NNC_DEVICE_ERROR) {
        return -EIO;
    }
    if (fw_control->device_state == HAILO_NNC_DEVICE_READY) {
        return 1;
    }
    if (fw_control->initialization_owner && fw_control->initialization_owner != vctx) {
        return -EAGAIN;
    }
    if (fw_control->initialization_owner == vctx &&
        fw_control->initialization_generation != vctx->generation) {
        return -ECANCELED;
    }
    if (!fw_control->initialization_owner) {
        fw_control->initialization_owner = vctx;
        fw_control->initialization_generation = vctx->generation;
        fw_control->device_state = HAILO_NNC_DEVICE_INITIALIZING;
        hailo_vdma_vctx_get(vctx);
    }
    return 0;
}

static int hailo_fw_vctx_prepare(struct hailo_file_context *context,
    struct hailo_pcie_board *board, struct hailo_fw_control *command,
    struct hailo_fw_vctx_operation *operation)
{
    struct hailo_fw_control_info *fw_control = &board->nnc.fw_control;
    struct hailo_vdma_vctx *vctx = &context->vdma_context.vctx;
    CONTROL_PROTOCOL__request_t *request = &command->request;
    u8 global_application;
    u32 local_application;
    int claim_result;

    memset(operation, 0, sizeof(*operation));
    if (command->request_len < CONTROL_PROTOCOL__REQUEST_BASE_SIZE ||
        command->request_len > sizeof(command->request)) {
        return -EINVAL;
    }
    operation->opcode = BYTE_ORDER__dtohl(request->opcode);

    switch (operation->opcode) {
    case HAILO_CONTROL_OPCODE_CONTEXT_SWITCH_CLEAR_CONFIGURED_APPS:
        if (fw_control->configuration_owner) {
            return -EBUSY;
        }
        if (hailo_fw_vctx_application_count(vctx) != 0) {
            if (fw_control->initialization_owner) {
                return -EBUSY;
            }
            if (fw_control->active_owner == vctx &&
                atomic_read(&board->vdma.total_ongoing_count) != 0) {
                return -EBUSY;
            }
            if (fw_control->active_owner == vctx) {
                claim_result = hailo_nnc_pause_active_vctx_locked(board);
                if (claim_result) {
                    return claim_result;
                }
            }
            operation->clear_configured_apps = true;
            if (atomic_read(&board->vdma.registered_vctx_count) > 1) {
                operation->suppress = true;
                return 0;
            }
            if (fw_control->device_state == HAILO_NNC_DEVICE_ERROR) {
                return -EIO;
            }
            if (!fw_control->initialization_owner) {
                fw_control->initialization_owner = vctx;
                fw_control->initialization_generation = vctx->generation;
                hailo_vdma_vctx_get(vctx);
            }
            fw_control->device_state = HAILO_NNC_DEVICE_INITIALIZING;
            return 0;
        }
        claim_result = hailo_fw_vctx_claim_initialization(fw_control, vctx);
        if (claim_result < 0) {
            return claim_result;
        }
        operation->clear_configured_apps = true;
        operation->suppress = claim_result > 0;
        return 0;

    case HAILO_CONTROL_OPCODE_CHANGE_CONTEXT_SWITCH_STATUS:
        local_application = request->parameters.change_context_switch_status_request.application_index;
        if (request->parameters.change_context_switch_status_request.state_machine_status ==
            CONTROL_PROTOCOL__CONTEXT_SWITCH_STATUS_RESET &&
            hailo_fw_vctx_application_count(vctx) == 0) {
            claim_result = hailo_fw_vctx_claim_initialization(fw_control, vctx);
            if (claim_result < 0) {
                return claim_result;
            }
            operation->initialization_reset = true;
            operation->suppress = claim_result > 0;
            return 0;
        }
        if (request->parameters.change_context_switch_status_request.state_machine_status ==
            CONTROL_PROTOCOL__CONTEXT_SWITCH_STATUS_RESET && local_application == 0xff) {
            /* 0xff resets the global state machine; do not forward it over another VCTX. */
            if (fw_control->active_owner == vctx &&
                atomic_read(&board->vdma.total_ongoing_count) != 0) {
                return -EBUSY;
            }
            if (fw_control->active_owner == vctx) {
                claim_result = hailo_nnc_pause_active_vctx_locked(board);
                if (claim_result) {
                    return claim_result;
                }
            }
            operation->runtime_reset = true;
            operation->suppress = atomic_read(&board->vdma.registered_vctx_count) > 1;
            return 0;
        }
        if (hailo_fw_vctx_get_application(vctx, local_application, &global_application)) {
            return -EINVAL;
        }
        request->parameters.change_context_switch_status_request.application_index = global_application;
        operation->mapped_context_switch_status = true;
        if (request->parameters.change_context_switch_status_request.state_machine_status ==
            CONTROL_PROTOCOL__CONTEXT_SWITCH_STATUS_ENABLED) {
            operation->deferred_activation = true;
            operation->suppress = true;
        } else if (request->parameters.change_context_switch_status_request.state_machine_status ==
            CONTROL_PROTOCOL__CONTEXT_SWITCH_STATUS_PAUSED) {
            if (fw_control->active_owner == vctx &&
                atomic_read(&board->vdma.total_ongoing_count) != 0) {
                return -EBUSY;
            }
        }
        return 0;

    case HAILO_CONTROL_OPCODE_CHANGE_HW_INFER_STATUS:
        if (atomic_read(&board->vdma.registered_vctx_count) > 1 &&
            request->parameters.change_hw_infer_status_request.hw_infer_state ==
                CONTROL_PROTOCOL__HW_INFER_STATE_START) {
            return -EOPNOTSUPP;
        }
        local_application = request->parameters.change_hw_infer_status_request.application_index;
        if (hailo_fw_vctx_get_application(vctx, local_application, &global_application)) {
            return -EINVAL;
        }
        request->parameters.change_hw_infer_status_request.application_index = global_application;
        operation->mapped_hw_infer_status = true;
        if (request->parameters.change_hw_infer_status_request.hw_infer_state ==
            CONTROL_PROTOCOL__HW_INFER_STATE_START) {
            operation->deferred_activation = true;
            operation->suppress = true;
        } else if (fw_control->active_owner == vctx &&
            atomic_read(&board->vdma.total_ongoing_count) != 0) {
            return -EBUSY;
        }
        return 0;

    case HAILO_CONTROL_OPCODE_DOWNLOAD_CONTEXT_ACTION_LIST:
        local_application = BYTE_ORDER__dtohl(
            request->parameters.download_context_action_list_request.network_group_id);
        if (hailo_fw_vctx_get_application(vctx, local_application, &global_application)) {
            return -EINVAL;
        }
        request->parameters.download_context_action_list_request.network_group_id =
            BYTE_ORDER__htodl(global_application);
        return 0;

    case HAILO_CONTROL_OPCODE_CONFIG_CONTEXT_SWITCH_BREAKPOINT:
        if (atomic_read(&board->vdma.registered_vctx_count) > 1) {
            return -EOPNOTSUPP;
        }
        if (!request->parameters.config_context_switch_breakpoint_request
                .breakpoint_data.break_at_any_application_index) {
            local_application = request->parameters.config_context_switch_breakpoint_request
                .breakpoint_data.application_index;
            if (hailo_fw_vctx_get_application(vctx, local_application, &global_application)) {
                return -EINVAL;
            }
            request->parameters.config_context_switch_breakpoint_request
                .breakpoint_data.application_index = global_application;
        }
        return 0;

    case HAILO_CONTROL_OPCODE_GET_CONTEXT_SWITCH_BREAKPOINT_STATUS:
    case HAILO_CONTROL_OPCODE_GET_CONTEXT_SWITCH_MAIN_HEADER:
    case HAILO_CONTROL_OPCODE_CONFIG_CONTEXT_SWITCH_TIMESTAMP:
        if (atomic_read(&board->vdma.registered_vctx_count) > 1) {
            return -EOPNOTSUPP;
        }
        return 0;

    case HAILO_CONTROL_OPCODE_CONTEXT_SWITCH_SET_NETWORK_GROUP_HEADER:
        if (fw_control->device_state != HAILO_NNC_DEVICE_READY) {
            return -EAGAIN;
        }
        if (fw_control->configuration_owner) {
            return -EBUSY;
        }
        if (fw_control->next_global_application >= CONTROL_PROTOCOL__MAX_CONTEXT_SWITCH_APPLICATIONS ||
            hailo_fw_vctx_application_count(vctx) >= CONTROL_PROTOCOL__MAX_CONTEXT_SWITCH_APPLICATIONS) {
            return -ENOSPC;
        }
        fw_control->configuration_owner = vctx;
        fw_control->configuration_generation = vctx->generation;
        fw_control->expected_contexts = BYTE_ORDER__dtohs(
            request->parameters.context_switch_set_network_group_header_request
                .application_header.dynamic_contexts_count) + 3;
        /* HailoRT adds activation, batch-switching and preliminary contexts. */
        fw_control->completed_contexts = 0;
        fw_control->context_chunk_open = false;
        fw_control->pending_local_application = hailo_fw_vctx_application_count(vctx);
        fw_control->pending_global_application = fw_control->next_global_application;
        fw_control->pending_application_reserved = false;
        fw_control->configuration_epoch++;
        hailo_vdma_vctx_get(vctx);
        hailo_fw_vctx_set_state(vctx, HAILO_VDMA_VCTX_FW_CONFIGURING);
        operation->configuration_header = true;
        return 0;

    case HAILO_CONTROL_OPCODE_CONTEXT_SWITCH_SET_CONTEXT_INFO:
        if (fw_control->configuration_owner != vctx ||
            fw_control->configuration_generation != vctx->generation ||
            !fw_control->pending_application_reserved ||
            fw_control->completed_contexts >= fw_control->expected_contexts) {
            return -EINVAL;
        }
        operation->configuration_context_info = true;
        operation->context_first_chunk =
            request->parameters.context_switch_set_context_info_request.is_first_chunk_per_context;
        operation->context_last_chunk =
            request->parameters.context_switch_set_context_info_request.is_last_chunk_per_context;
        if ((operation->context_first_chunk && fw_control->context_chunk_open) ||
            (!operation->context_first_chunk && !fw_control->context_chunk_open)) {
            return -EINVAL;
        }
        return 0;

    default:
        return 0;
    }
}

static bool hailo_fw_control_succeeded(const struct hailo_fw_control *command)
{
    return BYTE_ORDER__dtohl(command->response.status.major_status) == 0;
}

static void hailo_fw_vctx_complete(struct hailo_file_context *context,
    struct hailo_pcie_board *board, struct hailo_fw_control *command,
    const struct hailo_fw_vctx_operation *operation)
{
    struct hailo_fw_control_info *fw_control = &board->nnc.fw_control;
    struct hailo_vdma_vctx *vctx = &context->vdma_context.vctx;
    unsigned long flags;

    if (operation->configuration_header) {
        fw_control->pending_application_reserved = true;
        fw_control->next_global_application++;
        hailo_notice(board,
            "vctx-fw: reserve vctx=%llu local_app=%u global_app=%u contexts=%u epoch=%llu\n",
            (unsigned long long)vctx->vctx_id,
            fw_control->pending_local_application,
            fw_control->pending_global_application,
            fw_control->expected_contexts,
            (unsigned long long)fw_control->configuration_epoch);
    }
    if (operation->configuration_context_info) {
        if (operation->context_first_chunk) {
            fw_control->context_chunk_open = true;
        }
        if (operation->context_last_chunk) {
            fw_control->context_chunk_open = false;
            fw_control->completed_contexts++;
        }
        if (fw_control->completed_contexts == fw_control->expected_contexts &&
            !fw_control->context_chunk_open) {
            spin_lock_irqsave(&vctx->lock, flags);
            vctx->application_map[fw_control->pending_local_application] =
                fw_control->pending_global_application;
            vctx->application_count++;
            vctx->fw_state = HAILO_VDMA_VCTX_FW_CONFIGURED;
            vctx->fw_epoch++;
            spin_unlock_irqrestore(&vctx->lock, flags);
            hailo_vdma_vctx_fw_state_changed(vctx);
            hailo_notice(board,
                "vctx-fw: configured vctx=%llu local_app=%u global_app=%u epoch=%llu\n",
                (unsigned long long)vctx->vctx_id,
                fw_control->pending_local_application,
                fw_control->pending_global_application,
                (unsigned long long)fw_control->configuration_epoch);
            hailo_fw_vctx_release_configuration_owner(fw_control);
        }
    }
    if (operation->initialization_reset) {
        hailo_fw_vctx_set_state(vctx, HAILO_VDMA_VCTX_FW_UNCONFIGURED);
        if (operation->suppress) {
            hailo_notice(board, "vctx-fw: suppress repeated global reset vctx=%llu\n",
                (unsigned long long)vctx->vctx_id);
        }
    }
    if (operation->runtime_reset) {
        hailo_fw_vctx_clear_activation(vctx);
        if (fw_control->active_owner == vctx) {
            hailo_fw_vctx_release_active_owner(board);
        }
        hailo_fw_vctx_set_state(vctx, HAILO_VDMA_VCTX_FW_CONFIGURED);
        if (operation->suppress) {
            hailo_notice(board, "vctx-fw: suppress global runtime reset vctx=%llu registered=%d\n",
                (unsigned long long)vctx->vctx_id,
                atomic_read(&board->vdma.registered_vctx_count));
        }
    }
    if (operation->clear_configured_apps) {
        if (fw_control->active_owner == vctx) {
            hailo_fw_vctx_release_active_owner(board);
        }
        hailo_fw_vctx_clear_applications(vctx);
        if (!operation->suppress) {
            fw_control->device_state = HAILO_NNC_DEVICE_READY;
            fw_control->next_global_application = 0;
            hailo_notice(board, "vctx-fw: global initialization complete owner=%llu\n",
                (unsigned long long)vctx->vctx_id);
            hailo_fw_vctx_release_initialization_owner(fw_control);
        } else {
            hailo_notice(board, "vctx-fw: suppress repeated global clear vctx=%llu\n",
                (unsigned long long)vctx->vctx_id);
        }
    }
    if (operation->mapped_context_switch_status) {
        u8 status = command->request.parameters.change_context_switch_status_request.state_machine_status;

        if (status == CONTROL_PROTOCOL__CONTEXT_SWITCH_STATUS_ENABLED) {
            unsigned long flags;

            if (operation->deferred_activation) {
                spin_lock_irqsave(&vctx->lock, flags);
                memcpy(&vctx->activation_request, &command->request,
                    sizeof(vctx->activation_request));
                vctx->activation_request_len = command->request_len;
                vctx->activation_valid = true;
                spin_unlock_irqrestore(&vctx->lock, flags);
            }
            hailo_fw_vctx_set_state(vctx, HAILO_VDMA_VCTX_FW_RUNNABLE);
        } else if (status == CONTROL_PROTOCOL__CONTEXT_SWITCH_STATUS_PAUSED) {
            hailo_fw_vctx_clear_activation(vctx);
            if (fw_control->active_owner == vctx) {
                hailo_fw_vctx_release_active_owner(board);
            }
            hailo_fw_vctx_set_state(vctx, HAILO_VDMA_VCTX_FW_QUIESCING);
        } else {
            hailo_fw_vctx_set_state(vctx, HAILO_VDMA_VCTX_FW_CONFIGURED);
        }
    }
    if (operation->mapped_hw_infer_status) {
        u8 status = command->request.parameters.change_hw_infer_status_request.hw_infer_state;

        if (status == CONTROL_PROTOCOL__HW_INFER_STATE_START && operation->deferred_activation) {
            unsigned long flags;

            spin_lock_irqsave(&vctx->lock, flags);
            memcpy(&vctx->activation_request, &command->request,
                sizeof(vctx->activation_request));
            vctx->activation_request_len = command->request_len;
            vctx->activation_valid = true;
            spin_unlock_irqrestore(&vctx->lock, flags);
        } else if (status != CONTROL_PROTOCOL__HW_INFER_STATE_START) {
            hailo_fw_vctx_clear_activation(vctx);
            if (fw_control->active_owner == vctx) {
                hailo_fw_vctx_release_active_owner(board);
            }
        }
        hailo_fw_vctx_set_state(vctx,
            status == CONTROL_PROTOCOL__HW_INFER_STATE_START ?
            HAILO_VDMA_VCTX_FW_RUNNABLE : HAILO_VDMA_VCTX_FW_CONFIGURED);
    }
}

static void hailo_fw_vctx_fail(struct hailo_file_context *context,
    struct hailo_pcie_board *board, const struct hailo_fw_vctx_operation *operation)
{
    struct hailo_fw_control_info *fw_control = &board->nnc.fw_control;
    struct hailo_vdma_vctx *vctx = &context->vdma_context.vctx;

    if (operation->initialization_reset || operation->clear_configured_apps) {
        fw_control->device_state = HAILO_NNC_DEVICE_ERROR;
        hailo_fw_vctx_clear_activation(vctx);
        hailo_fw_vctx_set_state(vctx, HAILO_VDMA_VCTX_FW_ERROR);
        hailo_fw_vctx_release_initialization_owner(fw_control);
    }
    if (operation->runtime_reset) {
        hailo_fw_vctx_clear_activation(vctx);
        hailo_fw_vctx_set_state(vctx, HAILO_VDMA_VCTX_FW_ERROR);
    }
    if (operation->configuration_header || operation->configuration_context_info) {
        hailo_fw_vctx_set_state(vctx, HAILO_VDMA_VCTX_FW_ERROR);
        hailo_fw_vctx_release_configuration_owner(fw_control);
    }
}

static void hailo_fw_control_set_success(struct hailo_fw_control *command)
{
    memset(&command->response, 0, sizeof(command->response));
    command->response_len = sizeof(CONTROL_PROTOCOL__status_t);
}

void hailo_nnc_finalize(struct hailo_pcie_nnc *nnc)
{
    struct hailo_notification_wait *cursor = NULL;
    unsigned long flags;

    // Lock rcu_read_lock and send notification_completion to wake anyone waiting on the notification_wait_list when removed
    rcu_read_lock();
    list_for_each_entry_rcu(cursor, &nnc->notification_wait_list, notification_wait_list) {
        spin_lock_irqsave(&cursor->notification_lock, flags);
        cursor->is_disabled = true;
        complete(&cursor->notification_completion);
        spin_unlock_irqrestore(&cursor->notification_lock, flags);
    }
    rcu_read_unlock();
}

static void hailo_nnc_reset_virtualization_state_locked(struct hailo_pcie_board *board)
{
    struct hailo_fw_control_info *fw_control = &board->nnc.fw_control;

    if (fw_control->active_owner) {
        hailo_fw_vctx_release_active_owner(board);
    }
    if (fw_control->initialization_owner) {
        hailo_fw_vctx_release_initialization_owner(fw_control);
    }
    if (fw_control->configuration_owner) {
        hailo_fw_vctx_release_configuration_owner(fw_control);
    }
    fw_control->device_state = HAILO_NNC_DEVICE_COLD;
    fw_control->next_global_application = 0;
    fw_control->expected_contexts = 0;
    fw_control->completed_contexts = 0;
    fw_control->context_chunk_open = false;
    fw_control->pending_local_application = 0;
    fw_control->pending_global_application = 0;
    fw_control->pending_application_reserved = false;
    fw_control->configuration_epoch++;
    wake_up_interruptible_all(&fw_control->owner_wq);
}

void hailo_nnc_reset_virtualization_state(struct hailo_pcie_board *board)
{
    struct hailo_file_context *context;

    down(&board->nnc.fw_control.mutex);
    hailo_nnc_reset_virtualization_state_locked(board);
    list_for_each_entry(context, &board->open_files_list, open_files_list) {
        struct hailo_vdma_vctx *vctx = &context->vdma_context.vctx;
        unsigned long flags;

        spin_lock_irqsave(&vctx->lock, flags);
        vctx->fw_state = HAILO_VDMA_VCTX_FW_UNCONFIGURED;
        vctx->fw_epoch++;
        vctx->application_count = 0;
        memset(vctx->application_map, 0xff, sizeof(vctx->application_map));
        vctx->activation_valid = false;
        vctx->activation_request_len = 0;
        spin_unlock_irqrestore(&vctx->lock, flags);
        hailo_vdma_vctx_fw_state_changed(vctx);
    }
    up(&board->nnc.fw_control.mutex);
}

/* This function has only one purpose: to populate the dma-address translation-table for fw-commands that
 * write the action list to the FW.
 * The translation table allows the FW to translate between desc-list-handles and their real dma-addresses. */
static int hailo_fw_control_preprocess(struct hailo_file_context *context, struct hailo_pcie_board *board,
    CONTROL_PROTOCOL__request_t *request)
{
    CONTROL_PROTOCOL__context_switch_dma_addr_translation_table_t *translation_table = NULL;
    struct hailo_descriptors_list_buffer *desc_list = NULL;
    uint64_t dma_addr_handle = 0;
    uint8_t i = 0;

    if (BYTE_ORDER__dtohl(request->opcode) != HAILO_CONTROL_OPCODE_CONTEXT_SWITCH_SET_CONTEXT_INFO) {
        return 0;
    }

    translation_table = &request->parameters.context_switch_set_context_info_request.translation_table;
    if (translation_table->handle_count > CONTROL_PROTOCOL__MAX_VDMA_CHANNELS_PER_ENGINE) {
        hailo_err(board, "hailo_fw_control: invalid translation-table length");
        return -EINVAL;
    }

    for (i = 0; i < translation_table->handle_count; i++) {
        dma_addr_handle = BYTE_ORDER__dtohll(translation_table->handles[i]);
        desc_list = hailo_vdma_find_descriptors_buffer(&context->vdma_context, dma_addr_handle);
        if (desc_list == NULL) {
            hailo_err(board, "hailo_fw_control: failed to find descriptor list for translation-table");
            return -EINVAL;
        }

        translation_table->dma_addrs[i] = BYTE_ORDER__htodll(desc_list->dma_address);
    }

    return 0;
}

static int hailo_fw_vctx_copy_activation(struct hailo_vdma_vctx *vctx,
    struct hailo_fw_control *command, bool require_runnable)
{
    unsigned long flags;
    int err = 0;

    spin_lock_irqsave(&vctx->lock, flags);
    if (vctx->state == HAILO_VDMA_VCTX_DEAD ||
        (require_runnable && (vctx->state != HAILO_VDMA_VCTX_ACTIVE ||
            vctx->fw_state != HAILO_VDMA_VCTX_FW_RUNNABLE)) ||
        !vctx->activation_valid ||
        vctx->activation_request_len < CONTROL_PROTOCOL__REQUEST_BASE_SIZE ||
        vctx->activation_request_len > sizeof(vctx->activation_request)) {
        err = -EINVAL;
        goto exit;
    }
    memcpy(&command->request, &vctx->activation_request, sizeof(command->request));
    command->request_len = vctx->activation_request_len;

exit:
    spin_unlock_irqrestore(&vctx->lock, flags);
    return err;
}

/* fw_control.mutex serializes the shared command and firmware response buffer. */
static int hailo_nnc_send_fw_control_locked(struct hailo_pcie_board *board,
    struct hailo_fw_control *command)
{
    struct hailo_fw_control_info *fw_control = &board->nnc.fw_control;
    long completion_result;
    int err;

    command->response_len = 0;
    memset(&command->response, 0, sizeof(command->response));
    reinit_completion(&fw_control->completion);

    err = hailo_pcie_write_firmware_control(&board->pcie_resources, command);
    if (err < 0) {
        return err;
    }
    completion_result = wait_for_completion_timeout(
        &fw_control->completion, msecs_to_jiffies(FW_CONTROL_DEFAULT_TIMEOUT_MS));
    if (completion_result == 0) {
        return -ETIMEDOUT;
    }
    err = hailo_pcie_read_firmware_control(&board->pcie_resources, command);
    if (err < 0) {
        return err;
    }
    if (command->response_len < sizeof(CONTROL_PROTOCOL__status_t)) {
        return -EIO;
    }
    return hailo_fw_control_succeeded(command) ? 0 : -EIO;
}

static int hailo_nnc_pause_active_vctx_locked(struct hailo_pcie_board *board)
{
    struct hailo_fw_control_info *fw_control = &board->nnc.fw_control;
    struct hailo_fw_control *command = &fw_control->dispatch_command;
    struct hailo_vdma_vctx *owner = fw_control->active_owner;
    u64 owner_id;
    u64 owner_generation;
    u32 opcode;
    int err;

    if (!owner) {
        return 0;
    }
    if (atomic_read(&board->vdma.total_ongoing_count) != 0) {
        return -EBUSY;
    }

    owner_id = owner->vctx_id;
    owner_generation = fw_control->active_generation;
    err = hailo_fw_vctx_copy_activation(owner, command, false);
    if (err) {
        goto release_owner;
    }
    opcode = BYTE_ORDER__dtohl(command->request.opcode);
    if (opcode == HAILO_CONTROL_OPCODE_CHANGE_CONTEXT_SWITCH_STATUS) {
        command->request.parameters.change_context_switch_status_request.state_machine_status =
            CONTROL_PROTOCOL__CONTEXT_SWITCH_STATUS_PAUSED;
    } else if (opcode == HAILO_CONTROL_OPCODE_CHANGE_HW_INFER_STATUS) {
        command->request.parameters.change_hw_infer_status_request.hw_infer_state =
            CONTROL_PROTOCOL__HW_INFER_STATE_STOP;
    } else {
        err = -EINVAL;
        goto release_owner;
    }

    err = hailo_nnc_send_fw_control_locked(board, command);
    if (!err) {
        hailo_notice(board, "vctx-fw: pause vctx=%llu generation=%llu global_app=%u\n",
            (unsigned long long)owner_id,
            (unsigned long long)owner_generation,
            fw_control->active_global_application);
    }

release_owner:
    /* A failed/timeout PAUSE leaves the physical state uncertain. Clearing the
     * dispatch owner forces the next transfer to send ENABLE again. */
    hailo_fw_vctx_release_active_owner(board);
    return err;
}

int hailo_nnc_activate_vctx(struct hailo_pcie_board *board, struct hailo_vdma_vctx *vctx)
{
    struct hailo_fw_control_info *fw_control = &board->nnc.fw_control;
    struct hailo_fw_control *command = &fw_control->dispatch_command;
    struct hailo_vdma_vctx *previous_owner;
    u32 opcode;
    int err = 0;

    if (down_interruptible(&fw_control->mutex)) {
        return -ERESTARTSYS;
    }

    if (atomic_read(&board->vdma.total_ongoing_count) != 0) {
        err = -EAGAIN;
        goto exit;
    }
    err = hailo_fw_vctx_copy_activation(vctx, command, true);
    if (err) {
        goto exit;
    }
    opcode = BYTE_ORDER__dtohl(command->request.opcode);
    if (opcode != HAILO_CONTROL_OPCODE_CHANGE_CONTEXT_SWITCH_STATUS &&
        opcode != HAILO_CONTROL_OPCODE_CHANGE_HW_INFER_STATUS) {
        err = -EINVAL;
        goto exit;
    }
    if ((opcode == HAILO_CONTROL_OPCODE_CHANGE_CONTEXT_SWITCH_STATUS &&
        command->request.parameters.change_context_switch_status_request.state_machine_status !=
            CONTROL_PROTOCOL__CONTEXT_SWITCH_STATUS_ENABLED) ||
        (opcode == HAILO_CONTROL_OPCODE_CHANGE_HW_INFER_STATUS &&
        command->request.parameters.change_hw_infer_status_request.hw_infer_state !=
            CONTROL_PROTOCOL__HW_INFER_STATE_START)) {
        err = -EINVAL;
        goto exit;
    }
    if (opcode == HAILO_CONTROL_OPCODE_CHANGE_HW_INFER_STATUS &&
        atomic_read(&board->vdma.registered_vctx_count) > 1) {
        err = -EOPNOTSUPP;
        goto exit;
    }
    if (fw_control->active_owner == vctx &&
        fw_control->active_generation == vctx->generation) {
        atomic64_set(&board->vdma.dispatched_vctx_id, vctx->vctx_id);
        atomic64_set(&board->vdma.dispatched_generation, vctx->generation);
        atomic64_set(&board->vdma.notification_vctx_id, vctx->vctx_id);
        goto exit;
    }
    if (fw_control->active_owner) {
        err = hailo_nnc_pause_active_vctx_locked(board);
        if (err) {
            goto exit;
        }
    }

    err = hailo_fw_vctx_copy_activation(vctx, command, true);
    if (err) {
        goto exit;
    }
    /* Activation can generate an application-less D2H event before its
     * control response, so publish a notification-only target first. */
    atomic64_set(&board->vdma.notification_vctx_id, vctx->vctx_id);
    err = hailo_nnc_send_fw_control_locked(board, command);
    if (err) {
        atomic64_set(&board->vdma.notification_vctx_id, 0);
        goto exit;
    }

    previous_owner = fw_control->active_owner;
    if (previous_owner != vctx) {
        hailo_vdma_vctx_get(vctx);
        fw_control->active_owner = vctx;
        if (previous_owner) {
            hailo_vdma_vctx_put(previous_owner);
        }
    }
    fw_control->active_generation = vctx->generation;
    if (opcode == HAILO_CONTROL_OPCODE_CHANGE_CONTEXT_SWITCH_STATUS) {
        fw_control->active_global_application =
            command->request.parameters.change_context_switch_status_request.application_index;
    } else if (opcode == HAILO_CONTROL_OPCODE_CHANGE_HW_INFER_STATUS) {
        fw_control->active_global_application =
            command->request.parameters.change_hw_infer_status_request.application_index;
    }
    fw_control->active_epoch++;
    atomic64_set(&board->vdma.dispatched_vctx_id, vctx->vctx_id);
    atomic64_set(&board->vdma.dispatched_generation, vctx->generation);
    atomic64_set(&board->vdma.notification_vctx_id, vctx->vctx_id);
    hailo_notice(board,
        "vctx-fw: activate vctx=%llu generation=%llu global_app=%u epoch=%llu\n",
        (unsigned long long)vctx->vctx_id,
        (unsigned long long)vctx->generation,
        fw_control->active_global_application,
        (unsigned long long)fw_control->active_epoch);

exit:
    up(&fw_control->mutex);
    return err;
}

static int hailo_fw_control(struct hailo_file_context *context, struct hailo_pcie_board *board, unsigned long arg,
    bool* should_up_board_mutex)
{
    struct hailo_fw_control *command = &board->nnc.fw_control.command;
    struct hailo_fw_vctx_operation operation = {0};
    struct hailo_fw_control_info *fw_control = &board->nnc.fw_control;
    struct hailo_vdma_vctx *vctx = &context->vdma_context.vctx;
    long completion_result = 0;
    int err = 0;

    up(&board->mutex);
    *should_up_board_mutex = false;

retry_owner:
    if (down_interruptible(&fw_control->mutex)) {
        hailo_info(board, "hailo_fw_control down_interruptible fail tgid:%d (process was interrupted or killed)\n",
            current->tgid);
        return -ERESTARTSYS;
    }
    if (!READ_ONCE(context->is_valid) || !READ_ONCE(board->pDev)) {
        err = -ENODEV;
        goto l_exit;
    }
    if (hailo_fw_vctx_owner_conflicts(fw_control, vctx)) {
        up(&fw_control->mutex);
        err = wait_event_interruptible(fw_control->owner_wq,
            hailo_fw_vctx_wait_condition(fw_control, vctx));
        if (err) {
            return err;
        }
        goto retry_owner;
    }

    if (copy_from_user(command, (void __user*)arg, sizeof(*command))) {
        hailo_err(board, "hailo_fw_control, copy_from_user fail\n");
        err = -EFAULT;
        goto l_exit;
    }

    err = hailo_fw_vctx_prepare(context, board, command, &operation);
    if (err < 0) {
        if (err == -EAGAIN && hailo_fw_vctx_owner_conflicts(fw_control, vctx)) {
            up(&fw_control->mutex);
            err = wait_event_interruptible(fw_control->owner_wq,
                hailo_fw_vctx_wait_condition(fw_control, vctx));
            if (err) {
                return err;
            }
            goto retry_owner;
        }
        hailo_err(board, "hailo_fw_control: rejected opcode %u for vctx %llu status %d\n",
            operation.opcode, (unsigned long long)vctx->vctx_id, err);
        goto l_exit;
    }

    if (operation.suppress) {
        hailo_fw_control_set_success(command);
        hailo_fw_vctx_complete(context, board, command, &operation);
        if (copy_to_user((void __user*)arg, command, sizeof(*command))) {
            err = -EFAULT;
        }
        goto l_exit;
    }

    err = hailo_fw_control_preprocess(context, board, &command->request);
    if (err < 0) {
        hailo_err(board, "hailo_fw_control: failed to preprocess command\n");
        goto l_exit;
    }

    reinit_completion(&board->nnc.fw_control.completion);

    err = hailo_pcie_write_firmware_control(&board->pcie_resources, command);
    if (err < 0) {
        hailo_err(board, "Failed writing fw control to pcie\n");
        goto l_exit;
    }

    // Wait for response
    completion_result = wait_for_completion_interruptible_timeout(
        &board->nnc.fw_control.completion, msecs_to_jiffies(FW_CONTROL_DEFAULT_TIMEOUT_MS));
    if (completion_result <= 0) {
        if (0 == completion_result) {
            hailo_err(board, "hailo_fw_control, timeout waiting for control (%d ms)\n", FW_CONTROL_DEFAULT_TIMEOUT_MS);
            err = -ETIMEDOUT;
        } else {
            hailo_info(board, "hailo_fw_control, wait for completion failed with err=%ld (process was interrupted or killed)\n", completion_result);
            err = -EINTR;
        }
        goto l_exit;
    }

    err = hailo_pcie_read_firmware_control(&board->pcie_resources, command);
    if (err < 0) {
        hailo_err(board, "Failed reading fw control from pcie\n");
        goto l_exit;
    }

    if (!hailo_fw_control_succeeded(command)) {
        hailo_fw_vctx_fail(context, board, &operation);
        goto copy_response;
    }

    hailo_fw_vctx_complete(context, board, command, &operation);

copy_response:
    if (copy_to_user((void __user*)arg, command, sizeof(*command))) {
        hailo_err(board, "hailo_fw_control, copy_to_user fail\n");
        err = -EFAULT;
        goto l_exit;
    }

l_exit:
    if (err < 0 && !operation.suppress &&
        (operation.initialization_reset || operation.clear_configured_apps ||
        operation.configuration_header || operation.configuration_context_info ||
        operation.runtime_reset)) {
        hailo_fw_vctx_fail(context, board, &operation);
    }
    up(&fw_control->mutex);
    return err;
}

static long hailo_get_notification_wait_thread(struct hailo_pcie_board *board, struct file *filp,
    struct hailo_notification_wait **current_waiting_thread)
{
    struct hailo_notification_wait *cursor = NULL;
    // note: safe to access without rcu because the notification_wait_list is closed only on file release
    list_for_each_entry(cursor, &board->nnc.notification_wait_list, notification_wait_list)
    {
        if (filp == cursor->filp) {
            *current_waiting_thread = cursor;
            return 0;
        }
    }

    return -EFAULT;
}

static long hailo_read_notification_ioctl(struct hailo_pcie_board *board, unsigned long arg, struct file *filp,
    bool* should_up_board_mutex)
{
    long err = 0;
    struct hailo_notification_wait *current_waiting_thread = NULL;
    struct hailo_d2h_notification *notification = &board->nnc.notification_to_user;
    unsigned long irq_saved_flags;

    err = hailo_get_notification_wait_thread(board, filp, &current_waiting_thread);
    if (0 != err) {
        goto l_exit;
    }
    up(&board->mutex);

    if (0 > (err = wait_for_completion_interruptible(&current_waiting_thread->notification_completion))) {
        hailo_info(board,
            "HAILO_READ_NOTIFICATION - wait_for_completion_interruptible error. err=%ld. tgid=%d (process was interrupted or killed)\n",
            err, current_waiting_thread->tgid);
        *should_up_board_mutex = false;
        goto l_exit;
    }

    if (down_interruptible(&board->mutex)) {
        hailo_info(board, "HAILO_READ_NOTIFICATION - down_interruptible error (process was interrupted or killed)\n");
        *should_up_board_mutex = false;
        err = -ERESTARTSYS;
        goto l_exit;
    }
    if (!READ_ONCE(board->pDev)) {
        err = -ENODEV;
        goto l_exit;
    }

    spin_lock_irqsave(&current_waiting_thread->notification_lock, irq_saved_flags);
    if (current_waiting_thread->is_disabled) {
        spin_unlock_irqrestore(&current_waiting_thread->notification_lock, irq_saved_flags);
        hailo_info(board, "HAILO_READ_NOTIFICATION - notification disabled for tgid=%d\n", current->tgid);
        err = -ECANCELED;
        goto l_exit;
    }
    if (current_waiting_thread->notification_count == 0) {
        spin_unlock_irqrestore(&current_waiting_thread->notification_lock, irq_saved_flags);
        err = -EAGAIN;
        goto l_exit;
    }
    memcpy(notification,
        &current_waiting_thread->notifications[current_waiting_thread->notification_read_index],
        sizeof(*notification));
    current_waiting_thread->notification_read_index =
        (current_waiting_thread->notification_read_index + 1) % HAILO_NOTIFICATION_QUEUE_DEPTH;
    current_waiting_thread->notification_count--;
    if (current_waiting_thread->notification_count > 0) {
        complete(&current_waiting_thread->notification_completion);
    } else {
        reinit_completion(&current_waiting_thread->notification_completion);
    }
    spin_unlock_irqrestore(&current_waiting_thread->notification_lock, irq_saved_flags);

    if (copy_to_user((void __user*)arg, notification, sizeof(*notification))) {
        bool was_empty;

        hailo_err(board, "HAILO_READ_NOTIFICATION copy_to_user fail\n");
        spin_lock_irqsave(&current_waiting_thread->notification_lock, irq_saved_flags);
        if (current_waiting_thread->notification_count < HAILO_NOTIFICATION_QUEUE_DEPTH) {
            was_empty = current_waiting_thread->notification_count == 0;
            current_waiting_thread->notification_read_index =
                (current_waiting_thread->notification_read_index +
                    HAILO_NOTIFICATION_QUEUE_DEPTH - 1) % HAILO_NOTIFICATION_QUEUE_DEPTH;
            memcpy(&current_waiting_thread->notifications[
                    current_waiting_thread->notification_read_index],
                notification, sizeof(*notification));
            current_waiting_thread->notification_count++;
            if (was_empty) {
                complete(&current_waiting_thread->notification_completion);
            }
        } else {
            current_waiting_thread->dropped_notifications++;
        }
        spin_unlock_irqrestore(&current_waiting_thread->notification_lock, irq_saved_flags);
        err = -EFAULT;
        goto l_exit;
    }

l_exit:
    return err;
}

static long hailo_disable_notification(struct hailo_pcie_board *board, struct file *filp)
{
    struct hailo_notification_wait *cursor = NULL;
    unsigned long flags;

    hailo_info(board, "HAILO_DISABLE_NOTIFICATION: disable notification");
    rcu_read_lock();
    list_for_each_entry_rcu(cursor, &board->nnc.notification_wait_list, notification_wait_list) {
        if (filp == cursor->filp) {
            spin_lock_irqsave(&cursor->notification_lock, flags);
            cursor->is_disabled = true;
            complete(&cursor->notification_completion);
            spin_unlock_irqrestore(&cursor->notification_lock, flags);
            break;
        }
    }
    rcu_read_unlock();

    return 0;
}

static long hailo_read_log_ioctl(struct hailo_pcie_board *board, unsigned long arg)
{
    long err = 0;
    struct hailo_read_log_params params;

    if (copy_from_user(&params, (void __user*)arg, sizeof(params))) {
        hailo_err(board, "HAILO_READ_LOG, copy_from_user fail\n");
        return -ENOMEM;
    }

    if (0 > (err = hailo_pcie_read_firmware_log(&board->pcie_resources.fw_access, &params))) {
        hailo_err(board, "HAILO_READ_LOG, reading from log failed with error: %ld \n", err);
        return err;
    }

    if (copy_to_user((void*)arg, &params, sizeof(params))) {
        return -ENOMEM;
    }

    return 0;
}

long hailo_nnc_ioctl(struct hailo_file_context *context, struct hailo_pcie_board *board, unsigned int cmd,
    unsigned long arg, struct file *filp, bool *should_up_board_mutex)
{
    switch (cmd) {
    case HAILO_FW_CONTROL:
        return hailo_fw_control(context, board, arg, should_up_board_mutex);
    case HAILO_READ_NOTIFICATION:
        return hailo_read_notification_ioctl(board, arg, filp, should_up_board_mutex);
    case HAILO_DISABLE_NOTIFICATION:
        return hailo_disable_notification(board, filp);
    case HAILO_READ_LOG:
        return hailo_read_log_ioctl(board, arg);
    default:
        hailo_err(board, "Invalid nnc ioctl code 0x%x (nr: %d)\n", cmd, _IOC_NR(cmd));
        return -ENOTTY;
    }
}


static int add_notification_wait(struct hailo_pcie_board *board,
    struct hailo_file_context *context)
{
    struct hailo_notification_wait *wait = kmalloc(sizeof(*wait), GFP_KERNEL);
    if (!wait) {
        hailo_err(board, "Failed to allocate notification wait structure.\n");
        return -ENOMEM;
    }
    wait->tgid = current->tgid;
    wait->filp = context->filp;
    wait->is_disabled = false;
    wait->notification_read_index = 0;
    wait->notification_write_index = 0;
    wait->notification_count = 0;
    wait->dropped_notifications = 0;
    wait->vctx = &context->vdma_context.vctx;
    wait->vctx_id = wait->vctx->vctx_id;
    memset(wait->notifications, 0, sizeof(wait->notifications));
    hailo_vdma_vctx_get(wait->vctx);
    spin_lock_init(&wait->notification_lock);
    init_completion(&wait->notification_completion);
    list_add_rcu(&wait->notification_wait_list, &board->nnc.notification_wait_list);
    return 0;
}

int hailo_nnc_file_context_init(struct hailo_pcie_board *board, struct hailo_file_context *context)
{
    int err = add_notification_wait(board, context);

    if (err) {
        return err;
    }
    return 0;
}

static void clear_notification_wait_list(struct hailo_pcie_board *board, struct file *filp)
{
    struct hailo_notification_wait *cur = NULL, *next = NULL;
    list_for_each_entry_safe(cur, next, &board->nnc.notification_wait_list, notification_wait_list) {
        if (cur->filp == filp) {
            list_del_rcu(&cur->notification_wait_list);
            synchronize_rcu();
            if (cur->dropped_notifications) {
                pr_warn("hailo: vctx-fw: notification queue dropped=%u vctx=%llu\n",
                    cur->dropped_notifications,
                    (unsigned long long)cur->vctx_id);
            }
            hailo_vdma_vctx_put(cur->vctx);
            kfree(cur);
        }
    }
}

int hailo_nnc_driver_down(struct hailo_pcie_board *board)
{
    long completion_result = 0;
    int err = 0;

    reinit_completion(&board->driver_down.reset_completed);

    hailo_pcie_write_firmware_driver_shutdown(&board->pcie_resources);

    // Wait for response
    completion_result =
        wait_for_completion_timeout(&board->driver_down.reset_completed, msecs_to_jiffies(DEFAULT_SHUTDOWN_TIMEOUT_MS));
    if (completion_result <= 0) {
        if (0 == completion_result) {
            hailo_err(board, "hailo_nnc_driver_down, timeout waiting for shutdown response (timeout_ms=%d)\n", DEFAULT_SHUTDOWN_TIMEOUT_MS);
            err = -ETIMEDOUT;
        } else {
            hailo_info(board, "hailo_nnc_driver_down, wait for completion failed with err=%ld (process was interrupted or killed)\n",
                completion_result);
            err = completion_result;
        }
        goto l_exit;
    }

l_exit:
    return err;
}

void hailo_nnc_file_context_finalize(struct hailo_pcie_board *board, struct hailo_file_context *context)
{
    struct hailo_fw_control_info *fw_control = &board->nnc.fw_control;
    struct hailo_vdma_vctx *vctx = &context->vdma_context.vctx;
    bool is_active_owner;
    int err;

    clear_notification_wait_list(board, context->filp);

    down(&fw_control->mutex);
    if (fw_control->initialization_owner == vctx) {
        fw_control->device_state = HAILO_NNC_DEVICE_ERROR;
        hailo_fw_vctx_release_initialization_owner(fw_control);
    }
    if (fw_control->configuration_owner == vctx) {
        hailo_fw_vctx_set_state(vctx, HAILO_VDMA_VCTX_FW_ERROR);
        hailo_fw_vctx_release_configuration_owner(fw_control);
    }
    is_active_owner = fw_control->active_owner == vctx;
    if (is_active_owner) {
        hailo_fw_vctx_set_state(vctx, HAILO_VDMA_VCTX_FW_QUIESCING);
    }
    up(&fw_control->mutex);

    /* PAUSE is device-global and is only safe after this VCTX has no DMA in flight. */
    if (is_active_owner) {
        hailo_vdma_vctx_reset_channels(&context->vdma_context, &board->vdma, true);
    }

    down(&fw_control->mutex);
    if (fw_control->active_owner == vctx) {
        err = hailo_nnc_pause_active_vctx_locked(board);
        if (err) {
            if (board->pDev) {
                hailo_warn(board, "vctx-fw: close pause failed vctx=%llu status=%d\n",
                    (unsigned long long)vctx->vctx_id, err);
            } else {
                pr_warn("hailo: vctx-fw: close pause failed vctx=%llu status=%d\n",
                    (unsigned long long)vctx->vctx_id, err);
            }
            if (fw_control->active_owner == vctx) {
                hailo_fw_vctx_release_active_owner(board);
            }
        }
    }
    hailo_fw_vctx_clear_activation(vctx);
    up(&fw_control->mutex);

    context->nnc_last_registered_vctx =
        hailo_vdma_vctx_unregister(&context->vdma_context, &board->vdma);
}

void hailo_nnc_file_context_finalize_post_vdma(struct hailo_pcie_board *board,
    struct hailo_file_context *context)
{
    if (context->nnc_last_registered_vctx) {
        if (board->pDev) {
            hailo_nnc_driver_down(board);
        }
        hailo_nnc_reset_virtualization_state(board);
    }
}
