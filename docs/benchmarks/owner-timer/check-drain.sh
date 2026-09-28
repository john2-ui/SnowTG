#!/bin/bash
set -euo pipefail
lab=/home/lca/work/snowtg-owner-timer-20260928
if pgrep -x traffic-gen; then exit 2; fi
test "$(basename "$(readlink /sys/bus/pci/devices/0000:64:00.0/driver)")" = igc
mkdir "$lab/drain-counters"
cat /proc/sys/vm/nr_hugepages > "$lab/drain-counters/huge.before"
cmp "$lab/drain-counters/huge.before" "$lab/live-r3/huge.before"
trap 'bash "$lab/live-r3/restore.sh"; systemctl stop snowtg-owner-timer-counters.timer; chown -R lca:lca "$lab/drain-counters"' EXIT
systemd-run --unit=snowtg-owner-timer-counters --on-active=30m /bin/bash "$lab/live-r3/restore.sh"
echo 1024 > /proc/sys/vm/nr_hugepages
modprobe vfio-pci
ip link set enp100s0 down
/opt/dpdk-26.07-rc3/bin/dpdk-devbind.py --force -b vfio-pci 0000:64:00.0
export LD_LIBRARY_PATH=/opt/dpdk-26.07-rc3/lib
for backend in rte wheel; do
  gdb --batch -x "$lab/drain-counters.gdb" --args "$lab/$backend/traffic-gen/build/traffic-gen" \
    -l 8,0,2 --main-lcore 8 -a 0000:64:00.0 -m 1024 --file-prefix owner-timer-counters -- \
    --workers 2 --local-ip 192.168.10.86 --rx-mode worker --tx-mode worker \
    --stats-csv "$lab/drain-counters/$backend.csv" "$lab/drain-counters.json" \
    > "$lab/drain-counters/$backend.log" 2>&1
  grep DRAIN_COUNTER "$lab/drain-counters/$backend.log"
done
