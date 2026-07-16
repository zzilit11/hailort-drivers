#!/bin/bash

set -Eeuo pipefail

readonly MODULE_NAME="hailo_pci"
readonly TRACE_PARAMETER="/sys/module/${MODULE_NAME}/parameters/vctx_trace"
readonly TRACE_PATTERN='vctx-(trace|fw)'
trace_use_sudo=0
trace_producer_pid=""
trace_consumer_pid=""
trace_runtime_dir=""
trace_fifo=""

usage()
{
    cat <<'EOF'
Usage: hailo_vctx_trace.sh [--authorize|--follow|--run]

  --authorize  Acquire/validate sudo credentials in the foreground, then exit.
  --follow     Never prompt; trace in the same login session as --authorize.
  --run        Authorize and follow (default; intended for direct terminal use).

The inference application itself must remain a normal-user process. Only the
sysfs write and restricted dmesg read are elevated when required.
EOF
}

check_environment()
{
    if [[ ! -e "${TRACE_PARAMETER}" ]]; then
        echo "ERROR: ${TRACE_PARAMETER} does not exist." >&2
        echo "Load a hailo_pci module that provides vctx_trace first." >&2
        exit 1
    fi
    if ! command -v dmesg >/dev/null 2>&1; then
        echo "ERROR: dmesg is required." >&2
        exit 1
    fi
    if ! dmesg --help 2>&1 | grep -q -- '--follow-new'; then
        echo "ERROR: this dmesg does not support --follow-new (-W)." >&2
        echo "Refusing to use -w because it would mix old messages into the experiment." >&2
        exit 1
    fi
}

can_trace_without_sudo()
{
    [[ -w "${TRACE_PARAMETER}" ]] && dmesg >/dev/null 2>&1
}

sudo_is_required()
{
    (( trace_use_sudo == 1 ))
}

select_access_mode()
{
    trace_use_sudo=0
    if (( EUID != 0 )) && ! can_trace_without_sudo; then
        trace_use_sudo=1
    fi
}

authorize_trace()
{
    check_environment
    select_access_mode
    if ! sudo_is_required; then
        echo "VCTX trace authorization: direct access is available; sudo is not required." >&2
        return
    fi

    if ! command -v sudo >/dev/null 2>&1; then
        echo "ERROR: sudo is required for ${TRACE_PARAMETER} and/or dmesg access." >&2
        exit 1
    fi

    # This is deliberately the only interactive operation. --follow is used in
    # the background and must never attempt to read a password.
    sudo -v
    if ! sudo -n -- test -r "${TRACE_PARAMETER}" ||
       ! sudo -n -- dmesg >/dev/null; then
        echo "ERROR: sudo was authorized but trace resources are not accessible." >&2
        exit 1
    fi
    echo "VCTX trace authorization: sudo credentials are ready." >&2
}

read_trace_value()
{
    if sudo_is_required; then
        sudo -n -- cat "${TRACE_PARAMETER}"
    else
        cat "${TRACE_PARAMETER}"
    fi
}

write_trace_value()
{
    local value=$1
    if sudo_is_required; then
        printf '%s\n' "${value}" | sudo -n -- tee "${TRACE_PARAMETER}" >/dev/null
    else
        printf '%s\n' "${value}" | tee "${TRACE_PARAMETER}" >/dev/null
    fi
}

follow_trace()
{
    local original_trace_value
    local producer_status=0
    local consumer_status=0

    check_environment
    select_access_mode
    if sudo_is_required; then
        if ! command -v sudo >/dev/null 2>&1 || ! sudo -n -v; then
            echo "ERROR: VCTX tracing needs sudo authorization." >&2
            echo "Run '$0 --authorize' in the foreground first; do not run the inference script with sudo." >&2
            exit 1
        fi
    fi

    original_trace_value="$(read_trace_value)"
    restore_trace_value()
    {
        if [[ -e "${TRACE_PARAMETER}" ]] &&
           ! write_trace_value "${original_trace_value}"; then
            echo "WARNING: failed to restore vctx_trace=${original_trace_value}." >&2
        fi
        return 0
    }

    cleanup_trace()
    {
        local exit_code=$?

        trap - EXIT INT TERM
        if [[ -n "${trace_producer_pid}" ]] &&
           kill -0 "${trace_producer_pid}" 2>/dev/null; then
            kill -TERM "${trace_producer_pid}" 2>/dev/null || true
        fi
        if [[ -n "${trace_consumer_pid}" ]] &&
           kill -0 "${trace_consumer_pid}" 2>/dev/null; then
            kill -TERM "${trace_consumer_pid}" 2>/dev/null || true
        fi
        [[ -n "${trace_producer_pid}" ]] &&
            wait "${trace_producer_pid}" 2>/dev/null || true
        [[ -n "${trace_consumer_pid}" ]] &&
            wait "${trace_consumer_pid}" 2>/dev/null || true
        restore_trace_value
        [[ -n "${trace_fifo}" && -p "${trace_fifo}" ]] &&
            rm -f -- "${trace_fifo}" || true
        [[ -n "${trace_runtime_dir}" && -d "${trace_runtime_dir}" ]] &&
            rmdir -- "${trace_runtime_dir}" 2>/dev/null || true
        exit "${exit_code}"
    }
    trap cleanup_trace EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM

    write_trace_value 1
    echo "Enabled ${MODULE_NAME} vctx_trace; restoring vctx_trace=${original_trace_value} on exit." >&2

    # --follow-new excludes the existing kernel ring buffer so each case only
    # contains messages emitted after its trace process starts. Keep producer
    # and consumer PIDs explicit so the normal-user runner need not create a
    # new session merely to clean up a privileged dmesg child.
    trace_runtime_dir="$(mktemp -d /tmp/hailo-vctx-trace.XXXXXX)"
    trace_fifo="${trace_runtime_dir}/dmesg.fifo"
    mkfifo -- "${trace_fifo}"
    if sudo_is_required; then
        sudo -n -- dmesg --follow-new >"${trace_fifo}" &
    else
        dmesg --follow-new >"${trace_fifo}" &
    fi
    trace_producer_pid=$!
    grep -E --line-buffered "${TRACE_PATTERN}" <"${trace_fifo}" &
    trace_consumer_pid=$!

    if wait "${trace_consumer_pid}"; then
        consumer_status=0
    else
        consumer_status=$?
    fi
    trace_consumer_pid=""
    if wait "${trace_producer_pid}"; then
        producer_status=0
    else
        producer_status=$?
    fi
    trace_producer_pid=""
    if (( producer_status != 0 )); then
        echo "ERROR: dmesg follow process exited with status ${producer_status}." >&2
        return "${producer_status}"
    fi
    return "${consumer_status}"
}

mode="${1:---run}"
if (( $# > 1 )); then
    usage >&2
    exit 2
fi

case "${mode}" in
--authorize)
    authorize_trace
    ;;
--follow)
    follow_trace
    ;;
--run)
    authorize_trace
    follow_trace
    ;;
-h|--help)
    usage
    ;;
*)
    echo "ERROR: unknown option: ${mode}" >&2
    usage >&2
    exit 2
    ;;
esac
