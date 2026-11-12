"""Prevent post-close GPU register reads and weakened PCI drift checks.

This is an explicit source-order test. It does not emulate EFI firmware.
"""
from pathlib import Path
import re
import unittest

SOURCE = (Path(__file__).resolve().parents[1] / "src/msi_release.c").read_text(encoding="utf-8")


def function_body(name):
    match = re.search(
        r"\b(?:static\s+)?(?:BOOLEAN|EFI_STATUS)\s+" + re.escape(name) +
        r"\s*\([^)]*\)\s*\{",
        SOURCE,
    )
    if match is None:
        raise AssertionError("missing function: " + name)
    at, depth, start = match.end(), 1, match.end()
    while at < len(SOURCE) and depth:
        depth += (SOURCE[at] == "{") - (SOURCE[at] == "}")
        at += 1
    if depth:
        raise AssertionError("unterminated function: " + name)
    return SOURCE[start:at - 1]


class HandoverOrderTests(unittest.TestCase):
    def test_preclose_reads_require_valid_claim_and_full_register_checks(self):
        pre = function_body("GuMsiHardwareVerifiedBeforeClose")
        self.assertEqual(pre.count("GuRead32("), 7)
        self.assertIn("!Plan.claimed", pre)
        self.assertIn("!Gu.owned", pre)
        self.assertIn("Gu.fault", pre)
        for assertion in (
            "architecture==0x06U", "architecture==0x07U",
            "architecture==0x09U", "(cpu&0x10)", "!(hi>>4)",
            "ss0==0x88888888", "ss1==8",
            "!(readout&0x07ffffff)", "plm==0xffffffff",
        ):
            with self.subTest(assertion=assertion):
                self.assertIn(assertion, pre)

    def test_finish_checks_hardware_before_closing_pci(self):
        finish = function_body("GuMsiFinish")
        check = finish.index("GuMsiHardwareVerifiedBeforeClose()")
        close = finish.index("CloseProtocol(")
        self.assertLess(check, close)
        self.assertIn("return EFI_DEVICE_ERROR;", finish[check:close])
        self.assertIn("Plan.hardware_verified=TRUE;", finish[check:close])
        self.assertIn("Plan.hardware_verified=FALSE;", finish[close:])
        self.assertIn("Snapshot(&Current)", finish[close:])
        self.assertIn("Expected(3,FALSE)", finish[close:])
        # Post-close success must include a fresh survivor path comparison
        # and independent AMD/peer ownership. A later handover check alone
        # must not hide a successful finish with a changed GOP path.
        after = finish[close:]
        path_check = after.index("Gops(FALSE)")
        amd_check = after.index("ReadOnlyAmdConsoleOwnership()")
        peer_check = after.index("GuVerifyActivePeers(Plan.table)")
        self.assertLess(after.index("Expected(3,FALSE)"), path_check)
        self.assertLess(path_check, amd_check)
        self.assertLess(amd_check, peer_check)
        self.assertLess(peer_check, after.rindex("return s;"))
        self.assertIn("Plan.hardware_verified=FALSE;", after)
        self.assertNotIn("GuRead32(", after)

    def test_handover_retains_ownership_checks_without_postclose_mmio(self):
        handover = function_body("GuMsiReadyForWindows")
        for requirement in (
            "!Plan.hardware_verified", "!GuSafeToReturn()",
            "Alive()", "Gops(FALSE)", "ReadConfig(c)", "ConfigStable(c)",
        ):
            with self.subTest(requirement=requirement):
                self.assertIn(requirement, handover)
        self.assertNotIn("GuRead32(", handover)
        self.assertNotIn("Snapshot(", handover)

    def test_bus_master_exception_rejects_set_or_unrelated_changes(self):
        stable = function_body("ConfigStable")
        for required in (
            "Gu.flrs<=1", "i==1", "drift==4u",
            "GuPciMasterClearOnly(Plan.config[i],c[i])",
            "if(!allowed)",
        ):
            with self.subTest(required=required):
                self.assertIn(required, stable)
        self.assertIn("Gu.flrs==1", stable)
        self.assertIn("drift&~0x00090000u", stable)


    def test_stop_then_bus_master_clear_then_exclusive_claim(self):
        release = function_body("GuMsiRelease")
        self.assertLess(release.index("DisconnectController("),
                        release.index("NormalizeMasterAfterStop()"))
        self.assertLess(release.index("NormalizeMasterAfterStop()"),
                        release.index("ClaimRestore()"))
        normalize = function_body("NormalizeMasterAfterStop")
        self.assertIn("GuVerifyActivePeers",normalize)
        self.assertIn("EfiPciIoAttributeOperationDisable",normalize)
        self.assertIn("Gu.retained=TRUE",normalize)
        self.assertNotIn("GuConfigWrite(",normalize)
        self.assertIn("!Plan.master_normalized",function_body("GuMsiReleased"))

    def test_amd_owner_rechecked_after_each_stop_and_before_handover(self):
        # A post-Stop controller identity change must not be accepted solely
        # because its previously captured GOP and console edges still exist.
        release = function_body("GuMsiRelease")
        stop = release.index("DisconnectController(")
        self.assertIn("ReadOnlyAmdConsoleOwnership()", release[:stop])
        self.assertIn("ReadOnlyAmdConsoleOwnership()", release[stop:])
        self.assertLess(release.index("ReadOnlyAmdConsoleOwnership()", stop),
                        release.index("NormalizeMasterAfterStop()"))
        released = function_body("GuMsiReleased")
        self.assertIn('ReadOnlyAmdConsoleOwnership()!=EFI_SUCCESS', released)
        self.assertLess(released.index("ReadOnlyAmdConsoleOwnership()"),
                        released.index("Snapshot(&Current)"))
        finish = function_body("GuMsiFinish")
        after_close = finish.index("CloseProtocol(")
        self.assertIn("ReadOnlyAmdConsoleOwnership()", finish[after_close:])
        self.assertIn("Plan.hardware_verified=FALSE;", finish[after_close:])
        handover = function_body("GuMsiReadyForWindows")
        self.assertIn("ReadOnlyAmdConsoleOwnership()==EFI_SUCCESS", handover)
        self.assertNotIn("Snapshot(", handover)
        self.assertNotIn("GuRead32(", handover)

    def test_gop_survivor_path_is_pinned_before_vendor_stop(self):
        gops = function_body("Gops")
        release = function_body("GuMsiRelease")
        self.assertIn("GuDevicePathLength(", gops)
        self.assertIn("Plan.survivor_path_bytes[k]=path_bytes;", gops)
        self.assertIn("memcpy(Plan.survivor_paths[k],live_path,path_bytes);", gops)
        self.assertIn("GuDevicePathUnchanged(", gops)
        self.assertLess(gops.index("if(first) {", gops.index("EFI_DEVICE_PATH_PROTOCOL *live_path")),
                        gops.index("GuDevicePathUnchanged("))
        self.assertLess(release.index("Gops(FALSE)"),release.index("DisconnectController("))
        self.assertIn("Gops(FALSE)",release[release.index("DisconnectController("):])
        self.assertIn("Gops(FALSE)",function_body("GuMsiReadyForWindows"))

    def test_amd_owner_validation_is_read_only(self):
        owner = function_body("ReadOnlyAmdConsoleOwnership")
        paths = function_body("AmdConsoleVariable")
        graph = function_body("ReadOnlyAmdConsoleGraph")
        self.assertIn("EfiPciIoWidthUint32", owner)
        self.assertIn("GuDevicePathAmdDisplayOwner(", owner)
        self.assertIn('AmdConsoleVariable(L"ConOut"', owner)
        self.assertIn('AmdConsoleVariable(L"ErrOut"', owner)
        self.assertIn("GetVariable(", paths)
        self.assertIn("matching_gops!=1", paths)
        self.assertIn("EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER", graph)
        for text in (owner, paths, graph):
            for forbidden in ("DisconnectController(", "Pci.Write(",
                              "GuWrite32(", "EfiPciIoAttributeOperationEnable",
                              "SetVariable(", "ResetSystem("):
                self.assertNotIn(forbidden, text)

    def test_full_non_target_protocol_ownership_is_preserved(self):
        self.assertIn("GuIsPreparedPeerHandle(h)",function_body("ProtectedHandle"))

if __name__ == "__main__":
    unittest.main()
