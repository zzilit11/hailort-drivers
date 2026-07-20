#!/bin/sh

set -eu

module="hailo_pci"

# Unload the driver.
sudo rmmod "$module"

echo "driver is unloaded"
