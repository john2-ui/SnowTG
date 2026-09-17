#!/usr/bin/env python3
"""Exercise bind-dpdk safety gates using fake sysfs/tools; never touch a NIC."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class BindDpdkTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.pci = self.root / "sys/bus/pci/devices/0000:64:00.0"
        (self.pci / "net/eth-test").mkdir(parents=True)
        (self.pci / "iommu_group").mkdir()
        (self.pci / "class").write_text("0x020000\n")
        self.nic = self.root / "sys/class/net/eth-test"
        self.nic.mkdir(parents=True)
        (self.nic / "device").symlink_to(self.pci)
        self.drivers = self.root / "sys/bus/pci/drivers"
        for name in ("igc", "vfio-pci", "uio_pci_generic", "igb_uio"):
            (self.drivers / name).mkdir(parents=True)
        (self.pci / "driver").symlink_to(self.drivers / "igc")
        self.script = self.root / "bind-dpdk.sh"
        source = Path(__file__).resolve().parents[1] / "bind-dpdk.sh"
        self.script.write_text(source.read_text().replace("/sys/", str(self.root / "sys") + "/"))
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.log = self.root / "mutations.log"
        self.env = dict(os.environ, PATH=str(self.bin) + ":" + os.environ["PATH"],
                        SSH_CONNECTION="", MUTATIONS=str(self.log), PCI_FIXTURE=str(self.pci))
        self.tool("sudo", '#!/bin/sh\nexec "$@"\n')
        self.tool("modprobe", '#!/bin/sh\necho "modprobe $*" >> "$MUTATIONS"\n')
        self.tool("ip", '''#!/bin/sh
case "$*" in
  *"route get"*) echo "192.0.2.1 dev ${SSH_DEV:-mgmt0} src 192.0.2.2" ;;
  *) if [ "${ACTIVE_NIC:-0}" = 1 ]; then echo "192.0.2.0/24 dev eth-test"; fi ;;
esac
''')
        self.devbind = self.root / "dpdk tools/devbind.py"
        self.devbind.parent.mkdir()
        self.devbind.write_text('''import os, pathlib, sys
for arg in sys.argv[1:]:
    if arg.startswith("--bind="):
        with open(os.environ["MUTATIONS"], "a") as stream:
            stream.write(" ".join(sys.argv[1:]) + "\\n")
        if os.environ.get("BIND_NOOP") != "1":
            pci = pathlib.Path(os.environ["PCI_FIXTURE"])
            (pci / "driver").unlink()
            (pci / "driver").symlink_to(pci.parents[1] / "drivers" / arg.split("=", 1)[1])
''')
        self.env["DPDK_DEVBIND"] = str(self.devbind)

    def tool(self, name, content):
        path = self.bin / name
        path.write_text(content)
        path.chmod(0o755)

    def run_bind(self, *args, ok=True):
        result = subprocess.run(["bash", str(self.script), *args], env=self.env,
                                text=True, capture_output=True)
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def test_read_only_and_bad_arguments(self):
        self.run_bind()
        self.run_bind("--dry-run", "--driver", "uio_pci_generic", "eth-test")
        for args in (("--driver",), ("--force",), ("--unknown",),
                     ("eth-test", "64:00.0"), ("--status", "eth-test"), ("lo",)):
            self.run_bind(*args, ok=False)
        self.assertFalse(self.log.exists())

    def test_ssh_is_protected_even_with_force(self):
        self.env.update(SSH_CONNECTION="192.0.2.1 1234 192.0.2.2 22", SSH_DEV="eth-test")
        self.assertIn("SSH return route", self.run_bind("--force", "eth-test", ok=False))
        bridge = self.root / "sys/class/net/br-test"
        bridge.mkdir()
        (bridge / "lower_eth-test").symlink_to(self.nic)
        self.env["SSH_DEV"] = "br-test"
        self.assertIn("SSH return route", self.run_bind("--force", "eth-test", ok=False))
        self.assertFalse(self.log.exists())

    def test_active_nic_needs_explicit_force(self):
        self.env["ACTIVE_NIC"] = "1"
        self.assertIn("addresses/routes", self.run_bind("eth-test", ok=False))
        self.assertFalse(self.log.exists())
        self.run_bind("--force", "eth-test")
        self.assertIn("--force", self.log.read_text())

    def test_uio_without_iommu_and_restore(self):
        (self.pci / "iommu_group").rmdir()
        self.assertIn("no IOMMU group", self.run_bind("eth-test", ok=False))
        self.assertFalse(self.log.exists())
        for driver in ("uio_pci_generic", "igb_uio", "igc"):
            self.run_bind("--driver", driver, "64:00.0")
            self.assertEqual((self.pci / "driver").resolve().name, driver)
        mutations = self.log.read_text()
        self.run_bind("--driver", "igc", "eth-test")
        self.assertEqual(self.log.read_text(), mutations)

    def test_successful_devbind_exit_must_really_bind(self):
        self.env["BIND_NOOP"] = "1"
        self.assertIn("Binding did not complete", self.run_bind("eth-test", ok=False))


if __name__ == "__main__":
    unittest.main()
