#!/usr/bin/env bash

usage () {
    echo "Load the Hailo PCIe driver"
    echo ""
    echo "Usage: $0 [-d] [-s] [-h]"
    echo " - -h: display this help."
    echo ""
    echo " - -d: load the driver in debug mode (more messages in dmesg)."
    echo ""
    echo " - -s: load the driver activating SR-IOV feature."
    echo ""
    exit 1
}


MIN_DEBUG=5
MAX_DEBUG=7

DEBUG=$MIN_DEBUG

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE="${SCRIPT_DIR}/../hailo_pci.ko"

while getopts "hdp" arg; do
    case $arg in
    h)
        usage
        ;;
    d)
        DEBUG=$MAX_DEBUG
        ;;
    *)
        usage
        ;;
    esac
done

# load driver
if lsmod | awk '{print $1}' | grep -qx "hailo_pci"; then
    echo driver already loaded, unloading first
    sudo rmmod hailo_pci || exit 1
fi

sudo insmod "$MODULE" o_dbg=$DEBUG || exit 1
echo driver loaded successfully
