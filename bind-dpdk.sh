#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: ./bind-dpdk.sh [--status]
       ./bind-dpdk.sh [OPTIONS] <interface | PCI address>

  --driver NAME   Target module (default: vfio-pci). UIO alternatives:
                  uio_pci_generic / igb_uio (must be installed separately).
                  Use the original driver, e.g. igc/vmxnet3, to restore it.
  --devbind PATH  Path to dpdk-devbind.py; also accepts DPDK_DEVBIND.
  --force         Allow detaching an addressed/routed dedicated test NIC.
                  Never overrides protection of the current SSH route.
  --dry-run       Print the plan only; no sudo, module loading or binding.
  -h, --help      Show this help.

Discovery: DPDK_DEVBIND, PATH, DPDK_DIR/usertools, ../dpdk/usertools.
Run as your login user; sudo is used only for module loading and binding.
Binding restores only the driver, not IP addresses, routes or NIC settings.
No arguments only show status. No traffic generator is started.
EOF
}

die() { echo "Error: $*" >&2; exit 1; }

driver=vfio-pci
devbind=${DPDK_DEVBIND:-}
device=
status=0
force=0
dry_run=0
while (($#)); do
    case "$1" in
        --driver|--devbind)
            (($# >= 2)) && [[ -n $2 && $2 != -* ]] || die "Missing value for $1"
            if [[ $1 == --driver ]]; then driver=$2; else devbind=$2; fi
            shift 2 ;;
        --status) status=1; shift ;;
        --force) force=1; shift ;;
        --dry-run) dry_run=1; shift ;;
        -h|--help) usage; exit 0 ;;
        -*) die "Unknown option: $1" ;;
        *) [[ -z $device ]] || die "Specify exactly one device"
           device=$1; shift ;;
    esac
done
[[ $driver =~ ^[a-zA-Z0-9_-]+$ ]] || die "Invalid driver: $driver"
[[ $status == 0 || -z $device ]] || die "--status does not accept a device"

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ -z $devbind ]]; then
    for candidate in "$(command -v dpdk-devbind.py || true)" \
                     "$(command -v dpdk-devbind || true)" \
                     "${DPDK_DIR:-}/usertools/dpdk-devbind.py" \
                     "$script_dir/../dpdk/usertools/dpdk-devbind.py"; do
        if [[ -f $candidate ]]; then devbind=$candidate; break; fi
    done
fi
[[ -f $devbind ]] || die "Cannot find dpdk-devbind.py; set --devbind PATH or DPDK_DIR"
if [[ -z $device ]]; then
    ((force == 0 && dry_run == 0)) || die "--force/--dry-run requires a device"
    exec python3 "$devbind" --status
fi

if [[ -e /sys/class/net/$device/device ]]; then
    pci=$(basename -- "$(readlink -f -- "/sys/class/net/$device/device")")
elif [[ $device =~ ^([[:xdigit:]]{4}:)?[[:xdigit:]]{2}:[[:xdigit:]]{2}\.[0-7]$ ]]; then
    pci=${device,,}
    [[ $pci =~ ^[[:xdigit:]]{4}: ]] || pci=0000:$pci
else
    die "Not a PCI network interface or PCI address: $device"
fi
sysdev=/sys/bus/pci/devices/$pci
[[ -d $sysdev && $(cat "$sysdev/class") == 0x02* ]] || die "Not a PCI network device: $pci"
current=unbound
if [[ -L $sysdev/driver ]]; then
    current=$(basename -- "$(readlink -f -- "$sysdev/driver")")
fi
echo "$pci: $current -> $driver"
if [[ $current == "$driver" ]]; then
    echo "Already bound; no changes."
    exit 0
fi
if [[ $current != unbound ]]; then
    printf 'Restore driver: %q --driver %q %q\n' "$0" "$current" "$pci"
fi

privileged=()
((EUID == 0)) || privileged=(sudo)
bind=(python3 "$devbind" "--bind=$driver")
((force == 0)) || bind+=(--force)
bind+=("$pci")
if ((dry_run)); then
    printf '%q ' "${privileged[@]}" modprobe "$driver"; printf '\n'
    printf '%q ' "${privileged[@]}" "${bind[@]}"; printf '\n'
    echo "Preview only; actual binding checks SSH routing, active interfaces and IOMMU."
    exit 0
fi

# Keep the SSH source address: policy routing can give it a different return path.
ssh_interface=
if [[ -n ${SSH_CONNECTION:-} ]]; then
    read -r ssh_peer _ ssh_local _ <<< "$SSH_CONNECTION"
    ssh_route=$(ip -o route get "$ssh_peer" from "$ssh_local") || die "Cannot verify SSH return route"
    ssh_interface=$(awk '{for (i=1; i<NF; i++) if ($i == "dev") {print $(i+1); exit}}' <<< "$ssh_route")
    [[ -n $ssh_interface ]] || die "Cannot identify SSH return interface"
fi
# Include physical ports underneath a bridge, bond or VLAN used by SSH.
protect_ssh_path() {
    local link
    [[ $(readlink -f -- "/sys/class/net/$1/device") != "$sysdev" ]] || die "$1 carries the current SSH return route; use independent management or a local console"
    for link in /sys/class/net/"$1"/lower_*; do
        [[ -e $link ]] || continue
        protect_ssh_path "${link##*/lower_}"
    done
}
[[ -z $ssh_interface ]] || protect_ssh_path "$ssh_interface"
for netpath in "$sysdev"/net/*; do
    [[ -e $netpath ]] || continue
    interface=${netpath##*/}
    if ((force == 0)); then
        addresses=$(ip -o addr show dev "$interface" scope global)
        routes=$(ip -o route show dev "$interface")
        routes6=$(ip -o -6 route show dev "$interface")
        [[ -z $addresses && -z $routes && -z $routes6 ]] || die "$interface has addresses/routes; reserve it for testing before using --force"
    fi
done
if [[ $driver == vfio-pci && ! -d $sysdev/iommu_group ]]; then
    die "$pci has no IOMMU group; enable IOMMU, or explicitly select a supported lab UIO driver"
fi

"${privileged[@]}" modprobe "$driver"
"${privileged[@]}" "${bind[@]}"
[[ -L $sysdev/driver && $(basename -- "$(readlink -f -- "$sysdev/driver")") == "$driver" ]] || die "Binding did not complete; inspect dpdk-devbind status"
python3 "$devbind" --status
