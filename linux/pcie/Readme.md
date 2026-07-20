This project contains the source code of the Hailo PCIe driver for Linux.

## Build the driver.
Run the command `make all`. The driver will be compiled with the current running
kernel, in order to cross compile, the user needs to set the KERNEL_DIR environment variable.
 
## Install/Uninstall the driver
 Run the command `sudo make install` or `sudo make uninstall`

## Load the driver
Run the command `sudo modprobe hailo_pci` or `sudo modprobe -r hailo_pci`

## Set driver parameters
There are two options:
- Copy the `hailo_pci.conf` file to `/etc/modprobe.d` and edit the desired parameters.
- Pass the desired parameter in modprobe, for example:
  `sudo modprobe hailo_pci no_power_mode=Y`

## VCTX firmware dispatch

When multiple firmware-backed virtual contexts have pending DMA transfers, the
driver dispatches them with controller-wide round-robin scheduling. The first
eligible transfer may establish the initial owner. After that, the next owner
is the runnable VCTX with the next higher VCTX ID, wrapping to the lowest ID.
VCTXs without pending transfers are skipped and rejoin a later round when they
queue work. Selection is global across VDMA channels, so wake-up timing on an
individual channel cannot repeatedly bypass an older runnable VCTX.

The dispatch quantum closes when either configured threshold is reached:

- `vctx_dispatch_quantum_ms` is the ownership time threshold in milliseconds.
- `vctx_dispatch_quantum_transfers` is the committed-transfer threshold.
- Setting both parameters to zero restores immediate switching for diagnostics.

A threshold closes admission for the current owner; it does not interrupt DMA
already in flight. Firmware ownership changes only after the device-wide
ongoing-transfer count reaches zero. Consequently, an observed owner interval
can be longer than `vctx_dispatch_quantum_ms` by the time needed to drain DMA.

Enable `vctx_trace=1` to verify scheduling. `VCTX_QUANTUM_REQUEST` identifies
the VCTX selected by the round-robin policy, and `VCTX_QUANTUM_BEGIN` plus
`DEVICE_SWITCH` records the completed ownership change. With four continuously
runnable contexts, every group of four ownership changes must contain all four
VCTX IDs; the starting ID depends on the initial owner.
