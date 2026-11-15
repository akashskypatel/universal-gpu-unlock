"""Checks for a deliberately isolated, single-card Stop experiment.

These static guards are not proof of firmware recovery or successful Stop.
MSVC compile and a real physical log are independent verification steps.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT/"src/msi_stop_observation.c").read_text(encoding="utf-8")
BUILD = (ROOT/"src/Build-Msi-Stop-Observation.ps1").read_text(encoding="utf-8")
PASSIVE = (ROOT/"src/Build-Gpu-Topology-Diagnostic.ps1").read_text(encoding="utf-8")


class StopOnlyObservation(unittest.TestCase):
    def test_one_vendor_stop_to_exact_original_driver_after_durable_log(self):
        # Actual direct Boot Services call count, not substring occurrences in comments.
        calls = re.findall(r"->DisconnectController\s*\(", SOURCE)
        self.assertEqual(len(calls), 1)
        self.assertIn("DisconnectController(Cards[0].handle,", SOURCE)
        self.assertIn("expected_agent,NULL)", SOURCE)
        self.assertIn("GuHashMatches(image->ImageBase", SOURCE)
        self.assertIn("binding->Stop!=(EFI_DRIVER_BINDING_STOP)", SOURCE)
        self.assertIn("image->ImageSize!=162016U", SOURCE)
        self.assertIn("Image4090Sha", SOURCE)
        self.assertIn("Cards[0].io->RomSize!=RomSize4090", SOURCE)
        self.assertIn("TargetHasNoChildren()",SOURCE)
        for pinned in ("8e2b676ed8eccbb582ca6a80d6bf39196dcc574a1c3c4cc73d35b53c2d4de312",
                       "b96b0411dcd378e6433c609f94f6828ea18d8713f5055e4d76ffe9c4901b2908",
                       "fcb89e38b347b79e6fd126ade23dab174a5c5bfb9bfe7686b1072835986038e0"):
            self.assertIn(pinned,SOURCE)
        self.assertIn("GuHashMatches(Cards[i].path,Cards[i].path_size,ExactPaths[i])",SOURCE)
        self.assertIn("if(s==EFI_SUCCESS)s=Owner(&Cards[1]);",SOURCE)
        self.assertIn("if(s==EFI_SUCCESS)s=Owner(&Cards[2]);",SOURCE)
        self.assertIn("POST_STOP_PEER_OWNER=",SOURCE)
        # A refused Inventory() result must identify its exact read-only gate.
        for record in ("STOP_PREFLIGHT_PCI_OPENS SLOT=",
                       "STOP_PREFLIGHT_PCI_OPEN SLOT=",
                       "STOP_PREFLIGHT_PCI_OWNER SLOT=",
                       "STOP_PREFLIGHT_OWNER_RESULT SLOT=",
                       "STOP_PREFLIGHT_TARGET_PROTOCOLS STATUS=",
                       "STOP_PREFLIGHT_TARGET_PROTOCOL INDEX=",
                       "STOP_PREFLIGHT_TARGET_OPEN_BLOCKER PROTOCOL=",
                       "STOP_PREFLIGHT_CHILD_RESULT STATUS="):
            self.assertIn(record, SOURCE)
        inventory=SOURCE.split("static EFI_STATUS Inventory(VOID) {",1)[1].split(
            "static EFI_STATUS Gops(VOID) {",1)[0]
        self.assertLess(inventory.index("s=Owner(&Cards[i]);"),
                        inventory.index("STOP_PREFLIGHT_OWNER_RESULT SLOT="))
        owner_checks=inventory.split("for(i=0;i<3;i++) {",1)[1]
        self.assertLess(owner_checks.index("STOP_PREFLIGHT_OWNER_RESULT SLOT="),
                        owner_checks.index("if(EFI_ERROR(s))return s;"))
        self.assertLess(inventory.index("s=TargetHasNoChildren();"),
                        inventory.index("STOP_PREFLIGHT_CHILD_RESULT STATUS="))
        self.assertIn("if(EFI_ERROR(s))return s;", inventory)


        self.assertIn("STOP_ATTEMPT=IMMINENT", SOURCE)
        before=SOURCE.index("s=Flush();\n  if(EFI_ERROR(s)||!Log)goto refuse;", SOURCE.index("STOP_ATTEMPT=IMMINENT"))
        attempt=SOURCE.index("s=table->BootServices->DisconnectController(")
        self.assertLess(before, attempt)
        self.assertLess(SOURCE.index("s=Log->Close(Log);",before),attempt)
        self.assertLess(SOURCE.index("s=Root->Close(Root);",before),attempt)
        self.assertGreater(SOURCE.index("EFI_STATUS postlog=StartLog(image);"),attempt)

        self.assertIn("if(EFI_ERROR(s)||!Log)goto refuse;",SOURCE)
        self.assertIn("s=KeyY();",SOURCE)
        self.assertIn("for(;;)(VOID)Sys->BootServices->Stall(1000000);",SOURCE)
        self.assertIn("NO_WINDOWS_HANDOFF=1 POWER_OFF_REQUIRED=1",SOURCE)

    def test_gpu_peer_and_console_guards_precede_stop(self):
        before = SOURCE[:SOURCE.index("STOP_ATTEMPT=IMMINENT")]
        for required in ('display!=3', 'found[0]!=1', 'found[1]!=1', 'found[2]!=1',
                         'each[0]==0 && each[1]==1 && each[2]==1',
                         'ConsoleVar(L"ConOut")','ConsoleVar(L"ErrOut")',
                         'Cards[0].owner!=expected_agent',
                         'ExactOriginalDriver()',
                         'EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER'):
            self.assertIn(required, before)
        for disallowed in ('SetVariable(', 'Pci.Write(', 'Mem.Write(',
                           'ResetSystem(', 'StartImage(', 'LoadImage('):
            self.assertNotIn(disallowed,SOURCE)

    def test_build_is_explicitly_opt_in_and_does_not_change_passive_image(self):
        self.assertIn("if (-not $AcceptUnboundedStopRisk)",BUILD)
        self.assertIn("'msi_stop_observation.c','sha256.c','format.c','runtime.c'",BUILD)
        self.assertIn("MSI-RTX4090-STOP-ONCE-EXPERIMENT.EFI",BUILD)
        for source in ("unlock_execution.c","msi_release.c","gpu_topology_efi.c",
                       "app.c","dxe_main.c","gpu_windows_handoff.c"):
            self.assertNotIn("'"+source+"'", BUILD)
        self.assertNotIn("GU_EXPERIMENTAL_STOP_OBSERVATION", PASSIVE)
        self.assertIn("GU_GPU_DIAGNOSTIC_ONLY", PASSIVE)


if __name__=="__main__":
    unittest.main()
