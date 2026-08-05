#!/usr/bin/env bash
set -euo pipefail

DPDK_DEVBIND="/home/snow/dpdk/usertools/dpdk-devbind.py"
DPDK_HELLOWORLD="/home/snow/dpdk/build/examples/dpdk-helloworld"
INTERFACE="ens160"
PCI_ADDRESS="0000:03:00.0"
DPDK_DRIVER="uio_pci_generic"

echo "Binding $INTERFACE to $DPDK_DRIVER for lab-only DPDK testing."
echo "UIO does not provide VFIO IOMMU isolation."
sudo modprobe uio_pci_generic
sudo python3 "$DPDK_DEVBIND" --status

if [[ -e "/sys/class/net/$INTERFACE/device" ]]; then
    PCI_ADDRESS="$(basename "$(readlink -f "/sys/class/net/$INTERFACE/device")")"
fi

if [[ -L "/sys/bus/pci/devices/$PCI_ADDRESS/driver" ]] &&
   [[ "$(basename "$(readlink -f "/sys/bus/pci/devices/$PCI_ADDRESS/driver")")" == "$DPDK_DRIVER" ]]; then
    echo "$PCI_ADDRESS is already bound to $DPDK_DRIVER; skipping."
else
    sudo python3 "$DPDK_DEVBIND" --bind="$DPDK_DRIVER" "$PCI_ADDRESS"
fi

sudo python3 "$DPDK_DEVBIND" --status
sudo "$DPDK_HELLOWORLD" -l 0-3 -n 4
