#!/bin/bash

set -Eeuo pipefail

readonly MODULE_NAME="hailo_pci"
readonly TRACE_PARAMETER="/sys/module/${MODULE_NAME}/parameters/vctx_trace"
readonly TRACE_PATTERN='vctx-(trace|fw)'
trace_use_sudo=0

usage()
{
    cat <<'EOF'
Usage: hailo_vctx_trace.sh [--authorize|--follow|--run]

  --authorize  Acquire/validate sudo credentials in the foreground, then exit.
  --follow     Never prompt; enable vctx_trace and stream new VCTX dmesg events.
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

follow_dmesg()
{
    if sudo_is_required; then
        sudo -n -- dmesg --follow-new
    else
        dmesg --follow-new
    fi
}

follow_trace()
{
    local original_trace_value

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
    }
    trap restore_trace_value EXIT

    write_trace_value 1
    echo "Enabled ${MODULE_NAME} vctx_trace; restoring vctx_trace=${original_trace_value} on exit." >&2

    # --follow-new excludes the existing kernel ring buffer so each case only
    # contains messages emitted after its trace process starts.
    follow_dmesg | grep -E --line-buffered "${TRACE_PATTERN}"
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
