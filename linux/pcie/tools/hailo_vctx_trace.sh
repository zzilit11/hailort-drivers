#!/bin/bash

set -Eeuo pipefail

readonly MODULE_NAME="hailo_pci"
readonly TRACE_PARAMETER="/sys/module/${MODULE_NAME}/parameters/vctx_trace"
readonly TRACE_PATTERN='vctx-(trace|fw)'
readonly TRACE_SESSION_UID="$(id -u)"
readonly TRACE_STATE_FILE="${HAILO_VCTX_TRACE_STATE_FILE:-/tmp/hailo-vctx-trace-${TRACE_SESSION_UID}.state}"
readonly TRACE_SHARED_LOG="${HAILO_VCTX_TRACE_LOG:-/tmp/hailo-vctx-trace-${TRACE_SESSION_UID}.log}"
readonly TRACE_ERROR_LOG="${HAILO_VCTX_TRACE_ERROR_LOG:-/tmp/hailo-vctx-trace-${TRACE_SESSION_UID}.errors.log}"
readonly TRACE_TERMINAL_ECHO="${HAILO_VCTX_TRACE_ECHO:-0}"
trace_use_sudo=0
trace_producer_pid=""
trace_monitor_pid=""

usage()
{
    cat <<'EOF'
Usage: hailo_vctx_trace.sh [--authorize|--follow|--run]

  --authorize  Acquire/validate sudo credentials in the foreground, then exit.
  --follow     Never prompt; trace in the same login session as --authorize.
  --run        Authorize and follow (default; intended for direct terminal use).

The inference application itself must remain a normal-user process. Only the
sysfs write and restricted dmesg read are elevated when required.

While following, the helper publishes an external trace session for the
experiment matrix:
  state: /tmp/hailo-vctx-trace-UID.state
  log:   /tmp/hailo-vctx-trace-UID.log (unfiltered dmesg stream)
  error: /tmp/hailo-vctx-trace-UID.errors.log

The kernel log is written directly to a file to avoid losing high-rate VCTX
events. Set HAILO_VCTX_TRACE_ECHO=1 only when live terminal output is needed;
terminal echo is disabled by default and never sits in the capture path.
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
    if ! command -v stdbuf >/dev/null 2>&1; then
        echo "ERROR: stdbuf is required for line-buffered dmesg capture." >&2
        exit 1
    fi
    if [[ "${TRACE_TERMINAL_ECHO}" != "0" && "${TRACE_TERMINAL_ECHO}" != "1" ]]; then
        echo "ERROR: HAILO_VCTX_TRACE_ECHO must be 0 or 1." >&2
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

read_state_pid()
{
    local state_file=$1

    sed -n 's/^pid=//p' "${state_file}" 2>/dev/null | head -n 1
}

prepare_external_session()
{
    local existing_pid=""

    if [[ -e "${TRACE_STATE_FILE}" ]]; then
        existing_pid="$(read_state_pid "${TRACE_STATE_FILE}")"
        if [[ "${existing_pid}" =~ ^[1-9][0-9]*$ ]] &&
           kill -0 "${existing_pid}" 2>/dev/null; then
            echo "ERROR: another VCTX trace session is active: pid=${existing_pid}" >&2
            echo "State file: ${TRACE_STATE_FILE}" >&2
            return 1
        fi
        rm -f -- "${TRACE_STATE_FILE}"
    fi

    : >"${TRACE_SHARED_LOG}"
    : >"${TRACE_ERROR_LOG}"
}

publish_external_session()
{
    local state_tmp="${TRACE_STATE_FILE}.tmp.$$"

    {
        printf 'format_version=2\n'
        printf 'pid=%s\n' "$$"
        printf 'producer_pid=%s\n' "${trace_producer_pid}"
        printf 'log=%s\n' "${TRACE_SHARED_LOG}"
        printf 'error_log=%s\n' "${TRACE_ERROR_LOG}"
        printf 'log_format=raw-dmesg\n'
        printf 'terminal_echo=%s\n' "${TRACE_TERMINAL_ECHO}"
        printf 'started_unix=%s\n' "$(date +%s)"
    } >"${state_tmp}"
    mv -f -- "${state_tmp}" "${TRACE_STATE_FILE}"
}

remove_external_session()
{
    local published_pid=""

    if [[ -r "${TRACE_STATE_FILE}" ]]; then
        published_pid="$(read_state_pid "${TRACE_STATE_FILE}")"
    fi
    if [[ "${published_pid}" == "$$" ]]; then
        rm -f -- "${TRACE_STATE_FILE}"
    fi
}

monitor_trace_stream()
{
    local line

    while IFS= read -r line; do
        if [[ "${line}" =~ ${TRACE_PATTERN} ]]; then
            printf '%s\n' "${line}"
        fi
    done
}

follow_trace()
{
    local original_trace_value
    local producer_status=0

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
        if [[ -n "${trace_monitor_pid}" ]] &&
           kill -0 "${trace_monitor_pid}" 2>/dev/null; then
            kill -TERM "${trace_monitor_pid}" 2>/dev/null || true
        fi
        [[ -n "${trace_producer_pid}" ]] &&
            wait "${trace_producer_pid}" 2>/dev/null || true
        [[ -n "${trace_monitor_pid}" ]] &&
            wait "${trace_monitor_pid}" 2>/dev/null || true
        restore_trace_value
        remove_external_session
        exit "${exit_code}"
    }
    trap cleanup_trace EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM

    write_trace_value 1
    echo "Enabled ${MODULE_NAME} vctx_trace; restoring vctx_trace=${original_trace_value} on exit." >&2

    # --follow-new excludes the existing kernel ring buffer. Write dmesg
    # directly to the shared file: no shell regex loop, terminal, FIFO, or tee
    # is allowed to apply backpressure to the privileged kernel-log reader.
    prepare_external_session
    if sudo_is_required; then
        sudo -n -- stdbuf -oL -eL dmesg --follow-new \
            >>"${TRACE_SHARED_LOG}" 2>>"${TRACE_ERROR_LOG}" &
    else
        stdbuf -oL -eL dmesg --follow-new \
            >>"${TRACE_SHARED_LOG}" 2>>"${TRACE_ERROR_LOG}" &
    fi
    trace_producer_pid=$!
    sleep 0.05
    if ! kill -0 "${trace_producer_pid}" 2>/dev/null; then
        if wait "${trace_producer_pid}"; then
            producer_status=0
        else
            producer_status=$?
        fi
        trace_producer_pid=""
        echo "ERROR: dmesg follow process failed to start (status=${producer_status})." >&2
        [[ -s "${TRACE_ERROR_LOG}" ]] && cat "${TRACE_ERROR_LOG}" >&2
        return 1
    fi
    publish_external_session

    if [[ "${TRACE_TERMINAL_ECHO}" == "1" ]]; then
        tail --pid="$$" -n 0 -F "${TRACE_SHARED_LOG}" 2>/dev/null |
            monitor_trace_stream &
        trace_monitor_pid=$!
    fi
    echo "External VCTX trace ready: state=${TRACE_STATE_FILE} log=${TRACE_SHARED_LOG} errors=${TRACE_ERROR_LOG} echo=${TRACE_TERMINAL_ECHO}" >&2

    if wait "${trace_producer_pid}"; then
        producer_status=0
    else
        producer_status=$?
    fi
    trace_producer_pid=""
    if (( producer_status != 0 )); then
        echo "ERROR: dmesg follow process exited with status ${producer_status}." >&2
        [[ -s "${TRACE_ERROR_LOG}" ]] && cat "${TRACE_ERROR_LOG}" >&2
        return "${producer_status}"
    fi
    return 0
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
