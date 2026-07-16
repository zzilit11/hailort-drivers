#!/bin/bash

set -euo pipefail

readonly MODULE_NAME="hailo_pci"
readonly TRACE_PARAMETER="/sys/module/${MODULE_NAME}/parameters/vctx_trace"
readonly TRACE_PATTERN='vctx-(trace|fw)'

if [[ ! -e "${TRACE_PARAMETER}" ]]; then
    echo "Error: ${TRACE_PARAMETER} does not exist." >&2
    echo "Load a hailo_pci module that provides the vctx_trace parameter first." >&2
    exit 1
fi

sudo -n -v

original_trace_value=$(<"${TRACE_PARAMETER}")

restore_trace_value()
{
    if [[ -e "${TRACE_PARAMETER}" ]]; then
        if ! printf '%s\n' "${original_trace_value}" | sudo tee "${TRACE_PARAMETER}" >/dev/null; then
            echo "Warning: failed to restore vctx_trace=${original_trace_value}." >&2
        fi
    fi
}

trap restore_trace_value EXIT

printf '1\n' | sudo tee "${TRACE_PARAMETER}" >/dev/null
echo "Enabled ${MODULE_NAME} vctx_trace; press Ctrl+C to stop and restore vctx_trace=${original_trace_value}." >&2

if ! dmesg --help 2>&1 | grep -q -- '--follow-new'; then
    echo "Error: this dmesg does not support --follow-new (-W)." >&2
    echo "The trace helper will not fall back to -w because that would mix old kernel messages into this experiment." >&2
    exit 1
fi

# --follow-new deliberately excludes the existing kernel ring buffer.  Each
# experiment log therefore contains only events emitted after tracing starts.
sudo dmesg --follow-new | grep -E --line-buffered "${TRACE_PATTERN}"
