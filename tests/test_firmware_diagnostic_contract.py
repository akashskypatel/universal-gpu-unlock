"""Firmware diagnostic and menu contract checks."""
from pathlib import Path
import re
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
        # The new evidence is the full LoadImage DevicePath, not just the
        # relative LoadedImage.FilePath previously captured by the probe.
        self.assertIn("LoadedImagePathGuid=EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID",source)
        self.assertIn("bs->HandleProtocol(binding->ImageHandle,&LoadedImagePathGuid",parent)
        self.assertIn("SOURCE=LOADED_IMAGE_DEVICE_PATH READ_ONLY=1",parent)
        self.assertIn("loaded_image_path&&PathCopy(loaded_image_path,&path)",parent)
        self.assertIn('SayPath("MSI_B850_LOAD_DEVICE_PATH",i,&path)',parent)
        self.assertNotIn("bs->OpenProtocol(",parent)
        self.assertNotIn("DisconnectController(",parent)
        self.assertNotIn("Stop(",parent)
        self.assertNotIn("StartImage(",parent)
        self.assertNotIn("SetVariable(",parent)

    def test_msi_policy_pointer_read_requires_original_image_and_map(self):
        source=(P/"gpu_topology_efi.c").read_text(encoding="utf-8")
        report=source.split("static void ReportMsiB850RomVariables(",1)[1].split(
            "\n#endif",1)[0]
        self.assertLess(report.index(
            "if(!pci_ok || !GuMsiB850KnownRomDiagnosticHardware(&Snapshot))"),
            report.index("ReportMsiB850PolicyPointers(table->BootServices);"))
        probe=source.split("static void ReportMsiB850PolicyPointers(",1)[1].split(
            "static void ReportMsiB850RomVariables(",1)[0]
        self.assertIn("child->ParentHandle",probe)
        self.assertIn("child->DeviceHandle!=(EFI_HANDLE)node->identity.handle",probe)
        self.assertIn("GuSha256(",probe)
        self.assertIn("memcmp(digest,MsiPciBusTextDigest",probe)
        self.assertIn("bs->GetMemoryMap(",probe)
        self.assertIn("MsiPolicyMapped(map,map_size,desc_size,pci,object_span)",probe)
        self.assertIn("config_id!=node->identity.pci_id",probe)
        self.assertIn("STATE=OBSERVED PCI_OBJECT=%p KIND=%u ENTRY=%p MODE=%u RECORD_OWNER=%p",probe)
        self.assertIn("kind!=expected_kind",probe)
        self.assertIn("obj[0xefU]!=(UINT8)(node->identity.class_code>>8)",probe)
        self.assertLess(probe.index("MsiPolicyMapped(map,map_size,desc_size,pci,object_span)"),
                        probe.index("memcpy(&modeptr,obj+0x3c0U,sizeof(modeptr))"))
        self.assertLess(probe.index("if(method<text_start || method>=text_end"),
                        probe.index("memcpy(&modeptr,obj+0x3c0U,sizeof(modeptr))"))
        self.assertLess(probe.index("MsiPolicyMapped(map,map_size,desc_size,modeptr,0x20U)"),
                        probe.index("mode=entry[0x18U]"))
        for unsafe in ("DisconnectController(", "Stop(", "StartImage(",
                       "SetVariable(", "OpenProtocol(", "Pci.Write(",
                       "Pci.Read(", "Mem.Write(", "Mem.Read("):
            self.assertNotIn(unsafe,probe)

    def test_msi_bds_policy_snapshot_requires_exact_original_image(self):
        source=(P/"gpu_topology_efi.c").read_text(encoding="utf-8")
        report=source.split("static void ReportMsiB850RomVariables(",1)[1].split(
            "\\n#endif",1)[0]
        gate="if(!pci_ok || !GuMsiB850KnownRomDiagnosticHardware(&Snapshot))"
        call="ReportMsiB850BdsPolicyState(table->BootServices);"
        self.assertLess(report.index(gate),report.index("return;"))
        self.assertLess(report.index("return;"),report.index(call))
        probe=source.split("static void ReportMsiB850BdsPolicyState(",1)[1].split(
            "static void ReportMsiB850RomVariables(",1)[0]
        self.assertIn("MsiBdsTextDigest[32]",source)
        self.assertIn("expected_size=113568U,expected_rva=704U,expected_text=89076U;",probe)
        digest_initializer=source.split("static const UINT8 MsiBdsTextDigest[32]={",1)[1].split("};",1)[0]
        digest=bytes(int(word,16) for word in re.findall(r"0x([0-9a-fA-F]{2})\\b",digest_initializer))
        self.assertEqual(digest.hex(),"b17dc40833da35c094671ff1da25b1e7ac367daf77637eebc65b7d6cf5f00151")
        self.assertIn("MsiBdsExecutableMapped(map,map_bytes,desc_size,li->ImageBase,expected_size)",probe)
        self.assertIn("GuLoadedPeTextRange(",probe)
        self.assertIn("memcmp(digest,MsiBdsTextDigest,sizeof(digest))",probe)
        self.assertIn("policy!=(VOID *)(base+0x16330U)",probe)
        self.assertIn("revision!=0x10000U",probe)
        self.assertIn("method_set!=(VOID *)(base+0x33e4U)",probe)
        self.assertIn("head=(const UINT8 *)policy+0x70U;",probe)
        self.assertLess(probe.index("memcmp(digest,MsiBdsTextDigest,sizeof(digest))"),
                        probe.index("memcpy(&head_forward,head,sizeof(head_forward))"))
        self.assertLess(probe.index("INTERFACE_METHODS_UNVERIFIED"),
                        probe.index("memcpy(&head_forward,head,sizeof(head_forward))"))
        for action in ("DisconnectController(", "ConnectController(",
                       "Stop(", "StartImage(", "SetVariable(",
                       "OpenProtocol(", "Pci.Write(", "Pci.Read(",
                       "Mem.Write(", "Mem.Read(", "method_get(",
                       "method_set(", "method_class("):
            self.assertNotIn(action,probe)
        self.assertIn("NO_VENDOR_CALLBACK=1 NO_LIST_TRAVERSAL=1",probe)

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
