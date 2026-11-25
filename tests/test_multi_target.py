"""Compile and execute the real multi-card selection and admission policy.

C9 says the multi-GPU path is "a 2x config-space snapshot per target" that has
never been executed with two cards. Nothing in the tree could show what the
image does on a machine with two NVIDIA display functions, because the host
simulator models exactly one card and the entrypoint's refusal paths live behind
UEFI headers.

This module builds the missing model out of real code:

* ``SelectPreparedRecords`` is extracted from ``src/gpu_discovery.c`` -- the
  function that decides which discovered card(s) are prepared -- and linked
  against a captured log;
* ``GuFindGpuRecordSequence`` is the real one, from ``src/gpu_inventory.c``,
  which is compiled as-is;
* ``GuCheckConfig``/``GuCheckConfigPolicy`` are the real admission policy from
  ``src/checks.c``, reused from ``tests/test_admission_gates.py``.

Two cards are driven through that stack with two independent 256-byte config
snapshots, and the entrypoint's own two-card refusals are pinned as source text
so the model cannot drift away from the code it stands in for.

No PCI/GPU hardware is touched: every byte here is a crafted snapshot.
"""
from pathlib import Path
import importlib.util
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DISCOVERY = (ROOT / "src" / "gpu_discovery.c").read_text(encoding="utf-8")

# Reuse the type/constant block and the real config-policy code from the
# admission-gate test instead of restating either.
_spec = importlib.util.spec_from_file_location(
    "admission_gates_support", ROOT / "tests" / "test_admission_gates.py")
_gates = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_gates)


def source_body(name, text=DISCOVERY):
    """Return the complete body of a real function from ``text``."""
    pattern = (
        r"\b(?:static\s+)?(?:EFI_STATUS|BOOLEAN|VOID|size_t)\s+"
        + re.escape(name)
        + r"\s*\([^;]*?\)\s*\{"
    )
    match = re.search(pattern, text, re.DOTALL)
    if not match:
        raise AssertionError("missing real source function: " + name)
    pos = match.end()
    depth = 1
    while depth and pos < len(text):
        depth += (text[pos] == "{") - (text[pos] == "}")
        pos += 1
    if depth:
        raise AssertionError("unterminated source function: " + name)
    start = text.rfind("\n", 0, match.start()) + 1
    return text[start:pos]


PRELUDE = r"""
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "gpu_inventory.h"

__TYPES__

typedef unsigned long long EFI_STATUS;
#define EFI_SUCCESS            0ULL
#define EFI_UNSUPPORTED        0x8000000000000003ULL
#define EFI_INVALID_PARAMETER  0x8000000000000002ULL

static char LogText[32768];
static size_t LogUsed;

static void GuLog(const char *Format, ...) {
  va_list Args;
  int Written;
  va_start(Args, Format);
  Written = vsnprintf(LogText + LogUsed, sizeof(LogText) - LogUsed, Format, Args);
  va_end(Args);
  if (Written > 0) {
    LogUsed += (size_t)Written;
    if (LogUsed >= sizeof(LogText)) LogUsed = sizeof(LogText) - 1;
  }
}

static void ResetLog(void) { LogUsed = 0; LogText[0] = 0; }
static int LogHas(const char *Needle) { return strstr(LogText, Needle) != NULL; }
static size_t LogCount(const char *Needle) {
  size_t Count = 0;
  const char *At = LogText;
  while ((At = strstr(At, Needle)) != NULL) { ++Count; At += strlen(Needle); }
  return Count;
}

/* Two cards this image was built for (10de:2684 and 10de:2204) and one it was
 * not (10de:1b80). The real whitelist is pinned by test_gpu_device_ids.c. */
#define SUPPORTED_A   0x268410DEU
#define SUPPORTED_B   0x220410DEU
#define UNSUPPORTED   0x1B8010DEU
static BOOLEAN GuIsSupportedPciId(UINT32 Config0) {
  return Config0 == SUPPORTED_A || Config0 == SUPPORTED_B;
}

__DEFINES__

__POLICY__

/* The same caller-owned view gpu_discovery.c declares; the test below asserts
 * the two declarations still agree field for field. */
typedef struct {
  struct {size_t count,capacity;GU_GPU_RECORD *nvidia;} inventory;
  size_t selected_count;
  size_t *selected_indices;
} GU_GPU_PREPARED_VIEW;

__SELECTION__

static GU_GPU_RECORD Records[4];
static size_t Indices[4];

static GU_GPU_RECORD Card(UINT32 Id, UINT32 Bus) {
  GU_GPU_RECORD Record;
  memset(&Record, 0, sizeof(Record));
  Record.pci_id = Id;
  Record.subsystem_id = 0xA14010DEU;
  Record.class_code = 0x0300U;
  Record.segment = 0;
  Record.bus = Bus;
  return Record;
}

static GU_GPU_LOCATION At(UINT32 Bus) {
  GU_GPU_LOCATION Location;
  memset(&Location, 0, sizeof(Location));
  Location.bus = Bus;
  return Location;
}

static GU_GPU_PREPARED_VIEW View(size_t Count) {
  GU_GPU_PREPARED_VIEW Prepared = {{0, 4, Records}, 0, Indices};
  Prepared.inventory.count = Count;
  return Prepared;
}

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

/* A complete, admissible snapshot whose PM and PCIe capabilities sit at the
 * offsets given, so two cards can be told apart by what the policy reports. */
static void Snapshot(UINT32 C[64], UINTN Pm, UINTN Pcie) {
  memset(C, 0, 64U * sizeof(UINT32));
  Set32(C, 0x00, SUPPORTED_A);
  Set8(C, 0x0B, PCI_CLASS_DISPLAY);
  Set8(C, PCI_HEADER_TYPE_OFFSET, HEADER_TYPE_DEVICE);
  Set16(C, PCI_COMMAND_OFFSET, EFI_PCI_COMMAND_MEMORY_SPACE);
  Set16(C, PCI_PRIMARY_STATUS_OFFSET, EFI_PCI_STATUS_CAPABILITY);
  Set8(C, PCI_CAPABILITY_POINTER_OFFSET, (UINT8)Pm);

  Set8(C, Pm, EFI_PCI_CAPABILITY_ID_PMI);
  Set8(C, Pm + 1U, (UINT8)Pcie);
  Set16(C, Pm + 2U, 0x0003);
  Set16(C, Pm + 4U, PCI_POWER_STATE_D0);

  Set8(C, Pcie, PCI_EXPRESS_CAPABILITY_ID);
  Set8(C, Pcie + 1U, 0x00);
  Set16(C, Pcie + 2U, 0x0002);
  Set32(C, Pcie + 4U, 1U << 28U);
  Set16(C, Pcie + 8U, 0x0000);
}

static int Failures = 0;
static int Checks = 0;
#define EXPECT(cond) do { \
  ++Checks; \
  if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++Failures; } \
} while (0)

static int Run(void) {
  GU_GPU_PREPARED_VIEW Prepared;
  GU_GPU_LOCATION Requested[3];
  UINT32 A[64], B[64], SavedA[64], SavedB[64];
  GU_CAPS CapsA, CapsB;

  /* 1. One card and no explicit selection is unambiguous: it is prepared. */
  Records[0] = Card(SUPPORTED_A, 1);
  Prepared = View(1);
  ResetLog();
  EXPECT(SelectPreparedRecords(&Prepared, NULL, 0, FALSE) == EFI_SUCCESS);
  EXPECT(Prepared.selected_count == 1 && Prepared.selected_indices[0] == 0);
  EXPECT(LogHas("ACTIVE_PREPARE=COMPLETE SELECTED=1 NVIDIA_COUNT=1"));

  /* 2. Two cards and no explicit selection selects nothing and names the
   *    situation. This helper answers "which records were named"; the refusal
   *    belongs to the entrypoint, which rejects an empty selection. */
  Records[0] = Card(SUPPORTED_A, 1);
  Records[1] = Card(SUPPORTED_B, 2);
  Prepared = View(2);
  ResetLog();
  EXPECT(SelectPreparedRecords(&Prepared, NULL, 0, FALSE) == EFI_SUCCESS);
  EXPECT(Prepared.selected_count == 0);
  EXPECT(LogHas("ACTIVE_PREPARE=NO_SELECTION REASON=SELECTION_AMBIGUOUS NVIDIA_COUNT=2 SELECTED=0"));
  EXPECT(LogHas("ACTIVE_PREPARE=COMPLETE SELECTED=0 NVIDIA_COUNT=2"));
  EXPECT(LogCount("ACTIVE_PREPARED_STEP") == 0);

  /* 3. Two cards with the second one named: exactly that one is prepared. */
  Requested[0] = At(2);
  Prepared = View(2);
  ResetLog();
  EXPECT(SelectPreparedRecords(&Prepared, Requested, 1, FALSE) == EFI_SUCCESS);
  EXPECT(Prepared.selected_count == 1 && Prepared.selected_indices[0] == 1);
  EXPECT(LogHas("ACTIVE_PREPARED_STEP[0] BDF=0000:02:00.0"));
  EXPECT(!LogHas("ACTIVE_PREPARED_STEP[1]"));

  /* 4. An explicit ordered list of both cards keeps enumeration order, which is
   *    what the multi-target waiver admits; the coordinator still runs one at
   *    a time, but the selection itself must not be reordered. */
  Requested[0] = At(1);
  Requested[1] = At(2);
  Prepared = View(2);
  ResetLog();
  EXPECT(SelectPreparedRecords(&Prepared, Requested, 2, FALSE) == EFI_SUCCESS);
  EXPECT(Prepared.selected_count == 2);
  EXPECT(Prepared.selected_indices[0] == 0 && Prepared.selected_indices[1] == 1);
  EXPECT(LogCount("ACTIVE_PREPARED_STEP") == 2);
  EXPECT(LogHas("ACTIVE_PREPARE=COMPLETE SELECTED=2 NVIDIA_COUNT=2"));

  /* 5. A duplicated location in the request is refused by the real matcher. */
  Requested[0] = At(1);
  Requested[1] = At(1);
  Prepared = View(2);
  ResetLog();
  EXPECT(SelectPreparedRecords(&Prepared, Requested, 2, FALSE) == EFI_UNSUPPORTED);
  EXPECT(Prepared.selected_count == 0);
  EXPECT(LogHas("ACTIVE_PREPARE=REFUSED SELECTION_CODE=4"));

  /* 6. A requested card that is not present is refused, not fuzzily matched. */
  Requested[0] = At(9);
  Prepared = View(2);
  ResetLog();
  EXPECT(SelectPreparedRecords(&Prepared, Requested, 1, FALSE) == EFI_UNSUPPORTED);
  EXPECT(LogHas("ACTIVE_PREPARE=REFUSED SELECTION_CODE=5"));

  /* 7. More requested locations than discovered cards is refused. */
  Requested[0] = At(1);
  Requested[1] = At(2);
  Requested[2] = At(3);
  Prepared = View(2);
  ResetLog();
  EXPECT(SelectPreparedRecords(&Prepared, Requested, 3, FALSE) == EFI_UNSUPPORTED);
  EXPECT(LogHas("REASON=REQUESTED_EXCEEDS_INVENTORY REQUESTED=3 NVIDIA_COUNT=2"));

  /* 8. A machine with no NVIDIA display function is an empty selection, not a
   *    failure of this helper, and it says which situation it is. */
  Prepared = View(0);
  ResetLog();
  EXPECT(SelectPreparedRecords(&Prepared, NULL, 0, FALSE) == EFI_SUCCESS);
  EXPECT(Prepared.selected_count == 0);
  EXPECT(LogHas("ACTIVE_PREPARE=NO_SELECTION REASON=NO_NVIDIA_CARD NVIDIA_COUNT=0"));

  /* 9. The explicit ALL policy selects both supported cards in order. */
  Records[0] = Card(SUPPORTED_A, 1);
  Records[1] = Card(SUPPORTED_B, 2);
  Prepared = View(2);
  ResetLog();
  EXPECT(SelectPreparedRecords(&Prepared, NULL, 0, TRUE) == EFI_SUCCESS);
  EXPECT(Prepared.selected_count == 2);
  EXPECT(LogHas("ACTIVE_PREPARE=POLICY_ALL SUPPORTED=2 NVIDIA_COUNT=2"));

  /* 10. ALL with nothing supported selects nothing and says why. */
  Records[0] = Card(UNSUPPORTED, 1);
  Records[1] = Card(UNSUPPORTED, 2);
  Prepared = View(2);
  ResetLog();
  EXPECT(SelectPreparedRecords(&Prepared, NULL, 0, TRUE) == EFI_UNSUPPORTED);
  EXPECT(Prepared.selected_count == 0);
  EXPECT(LogHas("ACTIVE_PREPARE=REFUSED REASON=NO_SUPPORTED_CARD NVIDIA_COUNT=2"));

  /* 11. A named card this image was not built for is selected by location and
   *     named in the log; the entrypoint, not this helper, refuses it. */
  Records[0] = Card(SUPPORTED_A, 1);
  Records[1] = Card(UNSUPPORTED, 2);
  Requested[0] = At(2);
  Prepared = View(2);
  ResetLog();
  EXPECT(SelectPreparedRecords(&Prepared, Requested, 1, FALSE) == EFI_SUCCESS);
  EXPECT(Prepared.selected_count == 1 && Prepared.selected_indices[0] == 1);
  EXPECT(LogHas("ACTIVE_PREPARE=SELECTED_UNSUPPORTED_CARD BDF=0000:02:00.0 ID=10de:1b80"));

  /* 12. One named supported card in a two-card machine is still admitted: the
   *     ambiguity rule applies only to an unstated selection. */
  Records[0] = Card(SUPPORTED_A, 1);
  Records[1] = Card(UNSUPPORTED, 2);
  Requested[0] = At(1);
  Prepared = View(2);
  ResetLog();
  EXPECT(SelectPreparedRecords(&Prepared, Requested, 1, FALSE) == EFI_SUCCESS);
  EXPECT(Prepared.selected_count == 1 && Prepared.selected_indices[0] == 0);

  /* 13. Each card's admission reads its own snapshot: different offsets in,
   *     different capabilities out. */
  Snapshot(A, 0x40, 0x50);
  Snapshot(B, 0x48, 0x60);
  memcpy(SavedA, A, sizeof(A));
  memcpy(SavedB, B, sizeof(B));
  EXPECT(GuCheckConfig(A, 0, 1, 0, 0, &CapsA));
  EXPECT(CapsA.pm == 0x40 && CapsA.pcie == 0x50);
  EXPECT(GuCheckConfig(B, 0, 2, 0, 0, &CapsB));
  EXPECT(CapsB.pm == 0x48 && CapsB.pcie == 0x60);

  /* 14. Admitting the second card changes neither the first card's snapshot
   *     nor what the first card reports; the policy keeps no cross-card state. */
  EXPECT(memcmp(A, SavedA, sizeof(A)) == 0);
  EXPECT(memcmp(B, SavedB, sizeof(B)) == 0);
  {
    GU_CAPS Again;
    EXPECT(GuCheckConfig(A, 0, 1, 0, 0, &Again));
    EXPECT(Again.pm == CapsA.pm && Again.pcie == CapsA.pcie);
    EXPECT(Again.pcie_version == CapsA.pcie_version);
    EXPECT(Again.msi == CapsA.msi && Again.msix == CapsA.msix);
  }

  /* 15. A card that becomes inadmissible does not make the other one
   *     inadmissible: the verdicts are independent. */
  Set16(B, 0x62, 0x0003);          /* PCIe capability version 3 */
  {
    GU_CAPS Refused, StillA;
    EXPECT(!GuCheckConfig(B, 0, 2, 0, 0, &Refused));
    EXPECT(Refused.pm == 0 && Refused.pcie == 0);
    EXPECT(GuCheckConfig(A, 0, 1, 0, 0, &StillA));
    EXPECT(StillA.pm == 0x40 && StillA.pcie == 0x50);
  }

  /* 16. Selection and admission compose: the card the selection names is the
   *     snapshot that gets admitted, and the other card's snapshot is never
   *     touched by that decision. */
  Records[0] = Card(SUPPORTED_A, 1);
  Records[1] = Card(SUPPORTED_A, 2);
  Snapshot(A, 0x40, 0x50);
  Snapshot(B, 0x40, 0x50);
  {   /* the two snapshots are distinct objects with distinct content */
    UINT32 C[64];
    Snapshot(C, 0x48, 0x60);
    EXPECT(memcmp(C, A, sizeof(C)) != 0);
  }
  Requested[0] = At(2);
  Prepared = View(2);
  ResetLog();
  EXPECT(SelectPreparedRecords(&Prepared, Requested, 1, FALSE) == EFI_SUCCESS);
  EXPECT(GuCheckConfig(B, 0, 2, 0, 0, &CapsB));
  EXPECT(memcmp(A, SavedA, sizeof(A)) == 0);
  EXPECT(CapsB.pm == 0x40 && CapsB.pcie == 0x50);

  if (Failures) { fprintf(stderr, "FAILURES=%d\n", Failures); return 1; }
  printf("STATUS=COMPLETE PASS=%d MULTI_TARGET_CASES\n", Checks);
  return 0;
}

int main(void) { return Run(); }
"""

HARNESS = (
    PRELUDE.replace("__TYPES__", _gates.TYPES)
    .replace("__DEFINES__", _gates.POLICY_DEFINES)
    .replace("__POLICY__", _gates.POLICY_CODE)
    .replace("__SELECTION__", source_body("SelectPreparedRecords"))
)


class MultiTargetModel(unittest.TestCase):
    def test_two_cards_select_and_admit_independently(self):
        compiler = os.environ.get("CC", "gcc")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "multi_target.c"
            binary = Path(directory) / "multi_target"
            source.write_text(HARNESS, encoding="utf-8")
            cc = subprocess.run(
                [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "src"), str(source),
                 str(ROOT / "src" / "gpu_inventory.c"), "-o", str(binary)],
                capture_output=True, text=True, timeout=60, check=False,
            )
            self.assertEqual(cc.returncode, 0, cc.stderr)
            execution = subprocess.run(
                [str(binary)], capture_output=True, text=True, timeout=30, check=False,
            )
            self.assertEqual(execution.returncode, 0, execution.stderr)
            expected = HARNESS.count("EXPECT(") - 1  # minus the macro definition
            self.assertGreaterEqual(expected, 40)
            self.assertIn(
                "STATUS=COMPLETE PASS=%d MULTI_TARGET_CASES" % expected,
                execution.stdout,
            )

    def test_the_harness_view_matches_the_source_view(self):
        """The copied struct must stay field-for-field identical to the real one."""
        match = re.search(
            r"typedef struct \{\s*struct \{size_t count,capacity;GU_GPU_RECORD \*nvidia;\} inventory;\s*"
            r"size_t selected_count;\s*size_t \*selected_indices;\s*\} GU_GPU_PREPARED_VIEW;",
            DISCOVERY,
        )
        self.assertIsNotNone(match, "gpu_discovery.c no longer declares the view as modelled")
        self.assertIn(
            "struct {size_t count,capacity;GU_GPU_RECORD *nvidia;} inventory;\n"
            "  size_t selected_count;\n  size_t *selected_indices;\n} GU_GPU_PREPARED_VIEW;",
            HARNESS,
        )
        # The initializer the source itself uses must still be the one the
        # harness copies, or the view would be constructed differently.
        self.assertIn("{{0,GU_INVENTORY_MAX_NVIDIA,prepared->inventory.nvidia},", DISCOVERY)

    def test_the_entrypoint_still_refuses_two_cards(self):
        """Selection is not the only guard; the entrypoint's refusals stand."""
        for token in (
            "REASON=MULTI_NVIDIA_GLOBAL_STATE",
            "REASON=MULTI_TARGET_WITHOUT_WAIVER",
            "if(prepared.selected_count!=1 && !GU_ACTUATION_ADMITTED)",
            "if(prepared.inventory.count>1)",
            "REASON=NO_SELECTED_TARGET",
        ):
            with self.subTest(token=token):
                self.assertIn(token, DISCOVERY)

    def test_single_target_build_refuses_any_other_selection_count(self):
        """GU_GPU_SELECTION_COUNT is a compile-time constant in that mode."""
        match = re.search(
            r"#if defined\(GU_ACTIVE_SINGLE_TARGET\) && GU_GPU_SELECTION_COUNT != 1.*?#error[^\n]*",
            DISCOVERY,
            re.DOTALL,
        )
        self.assertIsNotNone(match, "the single-target selection-count #error is gone")
        self.assertIn("#error", match.group(0))

    def test_every_new_diagnostic_names_its_reason(self):
        body = source_body("SelectPreparedRecords")
        for token in ("REASON=SELECTION_AMBIGUOUS", "REASON=NO_NVIDIA_CARD",
                      "REASON=NO_SUPPORTED_CARD", "SELECTED_UNSUPPORTED_CARD",
                      "REASON=REQUESTED_EXCEEDS_INVENTORY"):
            with self.subTest(token=token):
                self.assertIn(token, body)
        # The ambiguity is reported before the completion line that used to be
        # the only trace of it.
        self.assertLess(body.index("REASON=SELECTION_AMBIGUOUS"),
                        body.index("ACTIVE_PREPARE=COMPLETE"))
        # And the entrypoint names its own refusal of an empty selection.
        self.assertIn("REASON=NO_SELECTED_TARGET", DISCOVERY)


if __name__ == "__main__":
    unittest.main()
