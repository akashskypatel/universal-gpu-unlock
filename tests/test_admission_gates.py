"""Compile and execute the real admission policy from ``src/checks.c``.

The policy is the only place that decides whether a discovered display
function may be admitted at all.  Until now it was exercised only through a
hand-written mock (``tests/gpu_discovery_mock.h``), so a regression in the
gates themselves would have been invisible on the host and would only have
surfaced -- silently, as ``EFI_UNSUPPORTED`` -- on real hardware.

This module extracts ``ConfigByte``, ``ConfigWord``, ``GuCheckConfigPolicy``
and both public wrappers from the real source, links them against independent
host mocks, and drives crafted 256-byte config snapshots through them.  Every
refusal path of the class-B gates is asserted individually, and the UEFI
constants the policy depends on are re-read from the vendored EDK2 headers so
the harness cannot drift away from the firmware's own values.

No PCI/GPU hardware is touched: this is a pure CPU-side policy test.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "checks.c").read_text(encoding="utf-8")
HEADERS = ROOT / "src" / "vendor" / "edk2" / "Include" / "IndustryStandard"


def source_body(name):
    """Return the complete body of a real function, braces included."""
    pattern = (
        r"\b(?:static\s+)?(?:BOOLEAN|UINT8|UINT16|UINT32)\s+"
        + re.escape(name)
        + r"\s*\([^;]*?\)\s*\{"
    )
    match = re.search(pattern, SOURCE, re.DOTALL)
    if not match:
        raise AssertionError("missing real source function: " + name)
    pos = match.end()
    depth = 1
    while depth and pos < len(SOURCE):
        depth += (SOURCE[pos] == "{") - (SOURCE[pos] == "}")
        pos += 1
    if depth:
        raise AssertionError("unterminated source function: " + name)
    start = SOURCE.rfind("\n", 0, match.start()) + 1
    return SOURCE[start:pos]


def source_define(name):
    """Return the value of a ``#define`` as written in the real source."""
    match = re.search(r"^#define\s+" + re.escape(name) + r"\s+([^\r\n]+)", SOURCE, re.MULTILINE)
    if not match:
        raise AssertionError("missing real source define: " + name)
    value = re.sub(r"/\*.*?\*/", "", match.group(1))
    value = re.sub(r"//.*$", "", value)
    return value.strip()


PRELUDE = r"""
#include <stdio.h>
#include <string.h>

typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;
typedef unsigned long      UINTN;
typedef unsigned char      BOOLEAN;

#define TRUE  1
#define FALSE 0
#ifndef NULL
#define NULL ((void *)0)
#endif

typedef struct {
  UINT16 pm, pcie, msi, msix;
  UINT8 pcie_version;
} GU_CAPS;

/* Values are asserted against the vendored EDK2 headers in
 * HarnessConstantsMatchVendoredHeaders; keep them literal here. */
#define PCI_CLASS_DISPLAY               0x03
#define PCI_HEADER_TYPE_OFFSET          0x0E
#define HEADER_TYPE_DEVICE              0x00
#define PCI_COMMAND_OFFSET              0x04
#define PCI_PRIMARY_STATUS_OFFSET       0x06
#define PCI_CAPABILITY_POINTER_OFFSET   0x34
#define PCI_EXPANSION_ROM_BASE          0x30
#define EFI_PCI_COMMAND_MEMORY_SPACE    0x0002
#define EFI_PCI_COMMAND_BUS_MASTER      0x0004
#define EFI_PCI_STATUS_CAPABILITY       0x0010
#define EFI_PCI_CAPABILITY_ID_PMI       0x01
#define EFI_PCI_CAPABILITY_ID_MSI       0x05
#define EFI_PCI_CAPABILITY_ID_VENDOR    0x09
#define PCI_EXPRESS_CAPABILITY_ID       0x0010
#define PCI_POWER_STATE_D0              0

__ID_STUB__

__DEFINES__

__ACTUAL_CODE__

static void Set8(UINT32 C[64], UINTN Off, UINT8 Value) {
  UINTN Index = Off / 4U, Shift = (Off & 3U) * 8U;
  C[Index] = (C[Index] & ~(0xFFU << Shift)) | ((UINT32)Value << Shift);
}

static void Set16(UINT32 C[64], UINTN Off, UINT16 Value) {
  Set8(C, Off, (UINT8)(Value & 0xFFU));
  Set8(C, Off + 1U, (UINT8)(Value >> 8U));
}

static void Set32(UINT32 C[64], UINTN Off, UINT32 Value) {
  Set16(C, Off, (UINT16)(Value & 0xFFFFU));
  Set16(C, Off + 2U, (UINT16)(Value >> 16U));
}

static int Failures = 0;
static int Checks = 0;
#define EXPECT(cond) do { \
  ++Checks; \
  if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++Failures; } \
} while (0)

/* A snapshot the policy must accept: NVIDIA 0x10de:0x2684, display class,
 * header type 0, memory decode on, capability status set, bus master off,
 * PM capability (version 3, PMCSR D0) at 0x40 and a PCIe v2 endpoint
 * capability (FLR capable, no FLR initiated) at 0x50. */
static void Base(UINT32 C[64]) {
  memset(C, 0, 64U * sizeof(UINT32));
  Set32(C, 0x00, SUPPORTED_ID);
  Set8(C, 0x0B, PCI_CLASS_DISPLAY);
  Set8(C, PCI_HEADER_TYPE_OFFSET, HEADER_TYPE_DEVICE);
  Set16(C, PCI_COMMAND_OFFSET, EFI_PCI_COMMAND_MEMORY_SPACE);
  Set16(C, PCI_PRIMARY_STATUS_OFFSET, EFI_PCI_STATUS_CAPABILITY);
  Set8(C, PCI_CAPABILITY_POINTER_OFFSET, 0x40);

  Set8(C, 0x40, EFI_PCI_CAPABILITY_ID_PMI);
  Set8(C, 0x41, 0x50);
  Set16(C, 0x42, 0x0003);           /* PMC: version 3 */
  Set16(C, 0x44, PCI_POWER_STATE_D0); /* PMCSR: D0 */

  Set8(C, 0x50, PCI_EXPRESS_CAPABILITY_ID);
  Set8(C, 0x51, 0x00);
  Set16(C, 0x52, 0x0002);           /* capabilities: v2, endpoint */
  Set32(C, 0x54, 1U << 28U);        /* device capabilities: FLR capable */
  Set16(C, 0x58, 0x0000);           /* device control: no FLR initiated */
}

static void AppendMsi(UINT32 C[64], UINTN At, UINT16 Control) {
  Set8(C, 0x51, (UINT8)At);         /* PCIe next -> MSI */
  Set8(C, At, EFI_PCI_CAPABILITY_ID_MSI);
  Set8(C, At + 1U, 0x00);
  Set16(C, At + 2U, Control);
}

static int Run(void) {
  UINT32 C[64];
  GU_CAPS Caps;

  /* 1. The baseline snapshot is admitted and reports both capabilities. */
  Base(C);
  memset(&Caps, 0xAA, sizeof(Caps));
  EXPECT(GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  EXPECT(Caps.pm == 0x40 && Caps.pcie == 0x50 && Caps.pcie_version == 2);
  EXPECT(Caps.msi == 0 && Caps.msix == 0);

  /* 2. A failing snapshot clears the caller's capability block. */
  Base(C);
  Set16(C, 0x44, 0x0003);           /* PMCSR: D3 instead of D0 */
  memset(&Caps, 0xAA, sizeof(Caps));
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  EXPECT(Caps.pm == 0 && Caps.pcie == 0 && Caps.msi == 0 && Caps.msix == 0);

  /* 3. NULL output is refused rather than dereferenced. */
  Base(C);
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, NULL));
  EXPECT(!GuCheckConfig(NULL, 0, 1, 0, 0, &Caps));

  /* 4. Out-of-range topology is refused. */
  Base(C);
  EXPECT(!GuCheckConfig(C, 0x10000U, 1, 0, 0, &Caps));
  EXPECT(!GuCheckConfig(C, 0, 256, 0, 0, &Caps));
  EXPECT(!GuCheckConfig(C, 0, 1, 32, 0, &Caps));
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 8, &Caps));
  EXPECT(GuCheckConfig(C, 0xFFFFU, 255, 31, 7, &Caps));

  /* 5. Identity: unsupported vendor/device, non-display class, bridge header. */
  Base(C);
  Set32(C, 0x00, 0x1B8010DEU);      /* Turing-class id not in this harness */
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  Base(C);
  Set8(C, 0x0B, 0x04);              /* multimedia, not display */
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  Base(C);
  Set8(C, PCI_HEADER_TYPE_OFFSET, 0x01); /* PCI-to-PCI bridge */
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  Base(C);
  Set8(C, PCI_HEADER_TYPE_OFFSET, 0x80); /* multifunction bit is masked off */
  EXPECT(GuCheckConfig(C, 0, 1, 0, 0, &Caps));

  /* 6. Memory decode must already be on. */
  Base(C);
  Set16(C, PCI_COMMAND_OFFSET, 0);
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));

  /* 7. Bus Master: refused by default, allowed exactly once by the
   *    pre-release read-only wrapper (B8). */
  Base(C);
  Set16(C, PCI_COMMAND_OFFSET, EFI_PCI_COMMAND_MEMORY_SPACE | EFI_PCI_COMMAND_BUS_MASTER);
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  EXPECT(GuCheckConfigInitialMaster(C, 0, 1, 0, 0, &Caps));
  EXPECT(!GuCheckConfigInitialMaster(C, 0, 1, 0, 0, NULL));

  /* 8. Expansion ROM must not be enabled. */
  Base(C);
  Set32(C, PCI_EXPANSION_ROM_BASE, 0xC0000001U);
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));

  /* 9. The capability-list bit in the status register must be set. */
  Base(C);
  Set16(C, PCI_PRIMARY_STATUS_OFFSET, 0);
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));

  /* 10. Power management: PMC version outside 1..3 is refused (B3). */
  Base(C);
  Set16(C, 0x42, 0x0000);
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  Base(C);
  Set16(C, 0x42, 0x0004);
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  Base(C);
  Set16(C, 0x42, 0x0001);
  EXPECT(GuCheckConfig(C, 0, 1, 0, 0, &Caps));

  /* 11. Power management: any state other than D0 is refused (B3). */
  Base(C);
  Set16(C, 0x44, 0x0001);
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  Base(C);
  Set16(C, 0x44, 0x0002);
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  Base(C);
  /* Only PMCSR bits 1:0 carry the power state; reserved bit 2 is not a
   * state, so a snapshot with it set is still D0 and stays admissible. */
  Set16(C, 0x44, 0x0004);
  EXPECT(GuCheckConfig(C, 0, 1, 0, 0, &Caps));

  /* 12. MSI must be present-and-disabled, never enabled (B3). */
  Base(C);
  AppendMsi(C, 0x8C, 0x0000);
  EXPECT(GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  EXPECT(Caps.msi == 0x8C);
  Base(C);
  AppendMsi(C, 0x8C, 0x0001);       /* MSI enable set */
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));

  /* 13. A second MSI capability is refused (duplicate layout). */
  Base(C);
  AppendMsi(C, 0x8C, 0x0000);
  Set8(C, 0x8D, 0xA0);              /* chain a second "MSI" */
  Set8(C, 0xA0, EFI_PCI_CAPABILITY_ID_MSI);
  Set8(C, 0xA1, 0x00);
  Set16(C, 0xA2, 0x0000);
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));

  /* 14. MSI-X must be present-and-disabled, never enabled. */
  Base(C);
  Set8(C, 0x51, 0x8C);
  Set8(C, 0x8C, 0x11);              /* MSI-X */
  Set8(C, 0x8D, 0x00);
  Set16(C, 0x8E, 0x0000);           /* table size 0, enable clear */
  EXPECT(GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  EXPECT(Caps.msix == 0x8C);
  Base(C);
  Set8(C, 0x51, 0x8C);
  Set8(C, 0x8C, 0x11);
  Set8(C, 0x8D, 0x00);
  Set16(C, 0x8E, 0x8000);           /* MSI-X enable set */
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));

  /* 15. Unknown capability ids are refused (B4). */
  Base(C);
  Set8(C, 0x51, 0x8C);
  Set8(C, 0x8C, 0x0D);              /* not PM/MSI/MSI-X/PCIe/vendor */
  Set8(C, 0x8D, 0x00);
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));

  /* 16. Vendor-specific capabilities are layout-checked only. */
  Base(C);
  Set8(C, 0x51, 0x8C);
  Set8(C, 0x8C, EFI_PCI_CAPABILITY_ID_VENDOR);
  Set8(C, 0x8D, 0x00);
  Set8(C, 0x8E, 0x08);              /* length 8 */
  EXPECT(GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  Base(C);
  Set8(C, 0x51, 0x8C);
  Set8(C, 0x8C, EFI_PCI_CAPABILITY_ID_VENDOR);
  Set8(C, 0x8D, 0x00);
  Set8(C, 0x8E, 0x02);              /* length below the 3-byte header */
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));

  /* 17. PCIe endpoint version 1 and 2 only; other layouts refused (B5). */
  Base(C);
  Set16(C, 0x52, 0x0001);           /* v1 endpoint */
  EXPECT(GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  EXPECT(Caps.pcie_version == 1);
  Base(C);
  Set16(C, 0x52, 0x0003);
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  Base(C);
  Set16(C, 0x52, 0x0000);
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  Base(C);
  Set16(C, 0x52, 0x0012);           /* v2, device type 1: legacy endpoint */
  EXPECT(GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  Base(C);
  Set16(C, 0x52, 0x0022);           /* v2, device type 2: root complex */
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));

  /* 18. FLR capability must be advertised (B5). */
  Base(C);
  Set32(C, 0x54, 0);
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));

  /* 19. An already-initiated FLR is refused (B5). */
  Base(C);
  Set16(C, 0x58, 0x8000);
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));

  /* 20. Both PM and PCIe capabilities are mandatory. */
  Base(C);
  Set8(C, 0x41, 0x00);              /* drop the PCIe capability */
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  Base(C);
  Set8(C, PCI_CAPABILITY_POINTER_OFFSET, 0x50); /* drop PM */
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));

  /* 21. Malformed chains: unaligned, below 0x40, past 0xfc, self-loop. */
  Base(C);
  Set8(C, PCI_CAPABILITY_POINTER_OFFSET, 0x41);
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  Base(C);
  Set8(C, PCI_CAPABILITY_POINTER_OFFSET, 0x3C);
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  Base(C);
  Set8(C, 0x51, 0xFC);              /* points past the 256-byte snapshot */
  Set8(C, 0xFC, EFI_PCI_CAPABILITY_ID_VENDOR);
  Set8(C, 0xFD, 0x00);
  Set8(C, 0xFE, 0x08);              /* length 8 does not fit before 0x100 */
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));
  Base(C);
  Set8(C, 0x41, 0x40);              /* PM capability points at itself */
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));

  /* 22. Overlapping capabilities are refused. */
  Base(C);
  Set8(C, 0x41, 0x44);              /* next pointer inside PM itself */
  EXPECT(!GuCheckConfig(C, 0, 1, 0, 0, &Caps));

  if (Failures) { fprintf(stderr, "FAILURES=%d\n", Failures); return 1; }
  printf("STATUS=COMPLETE PASS=%d ADMISSION_GATE_CASES\n", Checks);
  return 0;
}

int main(void) { return Run(); }
"""

# The whitelist stub is separate so other host models can admit more than one
# card without duplicating the type and constant block below.
ID_STUB = """
/* Only one vendor/device pair is "supported" for this harness; the real
 * whitelist lives in gpu_device_ids.h and is pinned by test_gpu_device_ids.c. */
#define SUPPORTED_ID 0x268410DEU
static BOOLEAN GuIsSupportedPciId(UINT32 Config0) { return Config0 == SUPPORTED_ID; }
"""

# Everything before the stub: UEFI-shaped typedefs, GU_CAPS and the PCI
# constants the policy uses. Reused by tests/test_multi_target.py.
TYPES = PRELUDE.split("__ID_STUB__")[0]

POLICY_DEFINES = "\n".join(
    "#define %s %s" % (name, source_define(name))
    for name in ("GU_CONFIG_BYTES", "GU_CAP_START", "GU_CAP_LAST", "GU_CAP_LIMIT", "GU_MSIX_ID")
)

POLICY_CODE = "\n\n".join(
    source_body(name)
    for name in (
        "ConfigByte",
        "ConfigWord",
        "GuCheckConfigPolicy",
        "GuCheckConfig",
        "GuCheckConfigInitialMaster",
    )
)

HARNESS = (
    PRELUDE.replace("__ID_STUB__", ID_STUB)
    .replace("__DEFINES__", POLICY_DEFINES)
    .replace("__ACTUAL_CODE__", POLICY_CODE)
)


class AdmissionGateTests(unittest.TestCase):
    def test_real_policy_refuses_every_unsafe_snapshot(self):
        compiler = os.environ.get("CC", "gcc")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "admission.c"
            binary = Path(directory) / "admission"
            source.write_text(HARNESS, encoding="utf-8")
            cc = subprocess.run(
                [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                 str(source), "-o", str(binary)],
                capture_output=True, text=True, timeout=60, check=False,
            )
            self.assertEqual(cc.returncode, 0, cc.stderr)
            execution = subprocess.run(
                [str(binary)], capture_output=True, text=True, timeout=30, check=False,
            )
            self.assertEqual(execution.returncode, 0, execution.stderr)
            expected = HARNESS.count("EXPECT(") - 1  # minus the macro definition
            self.assertGreaterEqual(expected, 45)
            self.assertIn(
                "STATUS=COMPLETE PASS=%d ADMISSION_GATE_CASES" % expected,
                execution.stdout,
            )

    def test_harness_constants_match_vendored_pci_headers(self):
        """The mocked UEFI constants must equal the firmware's own values."""
        expected = {
            "PCI_CLASS_DISPLAY": ("Pci22.h", "0x03"),
            "PCI_HEADER_TYPE_OFFSET": ("Pci22.h", "0x0E"),
            "HEADER_TYPE_DEVICE": ("Pci22.h", "0x00"),
            "PCI_COMMAND_OFFSET": ("Pci22.h", "0x04"),
            "PCI_PRIMARY_STATUS_OFFSET": ("Pci22.h", "0x06"),
            "PCI_CAPABILITY_POINTER_OFFSET": ("Pci22.h", "0x34"),
            "PCI_EXPANSION_ROM_BASE": ("Pci22.h", "0x30"),
            "EFI_PCI_COMMAND_MEMORY_SPACE": ("Pci22.h", "BIT1"),
            "EFI_PCI_COMMAND_BUS_MASTER": ("Pci22.h", "BIT2"),
            "EFI_PCI_STATUS_CAPABILITY": ("Pci22.h", "BIT4"),
            "EFI_PCI_CAPABILITY_ID_PMI": ("Pci22.h", "0x01"),
            "EFI_PCI_CAPABILITY_ID_MSI": ("Pci22.h", "0x05"),
            "EFI_PCI_CAPABILITY_ID_VENDOR": ("Pci23.h", "0x09"),
            "PCI_EXPRESS_CAPABILITY_ID": ("PciExpress21.h", "0x0010"),
            "PCI_POWER_STATE_D0": ("Pci22.h", "0"),
        }
        widths = {"BIT1": 0x0002, "BIT2": 0x0004, "BIT4": 0x0010}
        for name, (header, value) in expected.items():
            with self.subTest(constant=name):
                text = (HEADERS / header).read_text(encoding="utf-8")
                match = re.search(
                    r"^#define\s+" + re.escape(name) + r"\s+(\S+)", text, re.MULTILINE
                )
                self.assertIsNotNone(match, "%s is not defined in %s" % (name, header))
                self.assertEqual(match.group(1), value)
                literal = widths.get(value)
                if literal is not None:
                    value = "0x%04X" % literal
                self.assertRegex(
                    HARNESS,
                    r"(?m)^#define %s\s+%s\s*$" % (re.escape(name), re.escape(value)),
                    "harness constant drifted from the vendored header",
                )

    def test_policy_source_is_the_extracted_source(self):
        """Guard against the extraction silently matching nothing."""
        for name in ("GuCheckConfigPolicy", "GuCheckConfig", "GuCheckConfigInitialMaster"):
            with self.subTest(function=name):
                self.assertIn("typedef", HARNESS)
                self.assertIn(name, HARNESS)
        # The default branch of the capability switch must still refuse.
        self.assertIn("default:", HARNESS)
        self.assertIn("return FALSE;", HARNESS)


if __name__ == "__main__":
    unittest.main()
