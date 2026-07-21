#!/usr/bin/env bash
set -euo pipefail

DPDK_DEVBIND="/home/snow/dpdk/usertools/dpdk-devbind.py"
DPDK_HELLOWORLD="/home/snow/dpdk/build/examples/dpdk-helloworld"
INTERFACE="ens33"
PCI_ADDRESS="0000:03:00.0"

sudo modprobe vfio-pci
sudo python3 "$DPDK_DEVBIND" --status

if [[ -e "/sys/class/net/$INTERFACE/device" ]]; then
    PCI_ADDRESS="$(basename "$(readlink -f "/sys/class/net/$INTERFACE/device")")"
fi

if [[ -L "/sys/bus/pci/devices/$PCI_ADDRESS/driver" ]] &&
   [[ "$(basename "$(readlink -f "/sys/bus/pci/devices/$PCI_ADDRESS/driver")")" == "vfio-pci" ]]; then
    echo "$PCI_ADDRESS 已绑定到 vfio-pci，跳过重复绑定。"
else
    sudo python3 "$DPDK_DEVBIND" --bind=vfio-pci "$PCI_ADDRESS"
fi

sudo python3 "$DPDK_DEVBIND" --status
sudo "$DPDK_HELLOWORLD" -l 0-3 -n 4
