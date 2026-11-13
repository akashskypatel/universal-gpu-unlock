"""Firmware diagnostic and menu contract checks."""
from pathlib import Path
import unittest
P=Path(__file__).resolve().parents[1]/"src"
C=(P/"vendor/vbios_610.c").read_text(encoding="utf-8")
B=(P/"boot_io.c").read_text(encoding="utf-8")
A=(P/"app.c").read_text(encoding="utf-8")
V=(P/"version.h").read_text(encoding="utf-8")
class DiagnosticContract(unittest.TestCase):
    def test_bounded_fwsec_raw_read(self):
        descriptor=C.split("static int descriptor(",1)[1].split("int gpu_unlock_vbios_fwsec(",1)[0]
        self.assertLess(descriptor.index("range(length, off, 44)"),descriptor.index("raw descriptor[0:16]"))
        self.assertLess(descriptor.index("range(length, off, size)"),descriptor.index("raw descriptor[0:16]"))
        self.assertIn("SIGNATURE_AUTHENTICATED=0",descriptor)
    def test_msi_parent_image_observation_stays_behind_fingerprint_gate(self):
        source=(P/"gpu_topology_efi.c").read_text(encoding="utf-8")
        report=source.split("static void ReportMsiB850RomVariables(",1)[1].split("\n#endif",1)[0]
        gate="if(!pci_ok || !GuMsiB850KnownRomDiagnosticHardware(&Snapshot))"
        call="ReportMsiB850ImageParents(table->BootServices);"
        self.assertLess(report.index(gate),report.index("return;"))
        self.assertLess(report.index("return;"),report.index(call))
        parent=source.split("static void ReportMsiB850ImageParents(",1)[1].split(
            "static void ReportMsiB850RomVariables(",1)[0]
        self.assertIn("GuMsiB850IsOwnerNvidia(&node->identity)",parent)
        self.assertIn("node->by_driver!=1",parent)
        self.assertIn("child->ParentHandle",parent)
        self.assertIn("bs->HandleProtocol(child->ParentHandle,&LoadedGuid",parent)
        self.assertIn("PARENT_TEXT_SHA256=",parent)
        self.assertNotIn("DisconnectController(",parent)
        self.assertNotIn("Stop(",parent)
        self.assertNotIn("StartImage(",parent)
        self.assertNotIn("SetVariable(",parent)

    def test_version_does_not_claim_parent_release(self):
        self.assertIn("GPU-Unlock fork: experimental",V)
        self.assertIn("GU_FORK_VERSION_WIDE",B)
        self.assertIn("GU_FORK_VERSION",A)
        self.assertNotIn("proven rock-solid",B)
        self.assertNotIn("UNIVERSAL UNLOCK v1.7.5",A)
    def test_stock_windows_countdown_keeps_default(self):
        self.assertIn("*choice=L'S';",B)
        self.assertIn("i<300",B)
        self.assertIn("Stall(100000)",B)
        self.assertIn("if(i%10==0)",B)
        self.assertIn("seconds=(300-i+9)/10",B)
if __name__=="__main__":unittest.main()
