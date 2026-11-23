"""Compile and execute the real PCIe link policy from ``src/checks.c``.

C8 named four things this image never looked at: AER, ASPM, PCIe training and
hot-plug. Three of them are readable from the same 256-byte snapshot the
admission policy already parses; the fourth (AER) lives in extended config
space and is therefore reported as unobserved instead of assumed.

This module extracts the real ``ConfigByte``, ``ConfigWord``, ``GuCheckLinkState``
``GuLinkAccepted`` and ``GuLinkReason`` and drives crafted snapshots through
them: a healthy link, each refusal in the order it is reported, the width and
speed arithmetic (including a degraded x1 link, which is logged rather than
refused), and the offsets of a capability that does not sit at 0x50.

No PCI/GPU hardware is touched: these are crafted bytes.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "checks.c").read_text(encoding="utf-8")


def source_body(name):
    pattern = (r"\b(?:static\s+)?(?:BOOLEAN|UINT8|UINT16|UINT32|GU_LINK_VERDICT|"
               r"const\s+char\s+\*)\s*" + re.escape(name) + r"\s*\([^;]*?\)\s*\{")
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


HARNESS = r"""
#include <stdio.h>
#include <string.h>

typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;
typedef unsigned long      UINTN;
typedef int                BOOLEAN;

#define TRUE 1
#define FALSE 0

#define GU_CONFIG_BYTES 256U
#define GU_CAP_START 0x40U

__REAL_TYPES__

__ACTUAL_CODE__

static int Failures = 0;
static int Checks = 0;
#define EXPECT(cond) do { \
  ++Checks; \
  if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++Failures; } \
} while (0)

static void Set8(UINT32 c[64], UINTN off, UINT8 value) {
  UINTN index = off / 4u, shift = (off & 3u) * 8u;
  c[index] = (c[index] & ~(0xFFu << shift)) | ((UINT32)value << shift);
}
static void Set16(UINT32 c[64], UINTN off, UINT16 value) {
  Set8(c, off, (UINT8)(value & 0xffu));
  Set8(c, off + 1u, (UINT8)(value >> 8));
}

typedef struct {
  UINT8 width, max_width, speed, max_speed, aspm;
  UINT8 trained, up, errors, hotplug, surprise;
} CARD;

/* A PCIe capability at `pcie` describing one link state. Link Capabilities at
 * +0x0C carry the advertised width/speed; Link Status at +0x12 the negotiated
 * ones plus training and DLL-active; Slot Capabilities at +0x14 the hot-plug
 * bits. */
static void Card(UINT32 c[64], UINTN pcie, CARD card) {
  memset(c, 0, 64u * sizeof(UINT32));
  c[0] = 0x268410DEu;                 /* a supported display function */
  Set8(c, 0x0b, 0x03);                /* class: display */
  Set8(c, 0x0e, 0x00);                /* header: a normal device */
  Set8(c, 0x34, (UINT8)pcie);         /* capability list */
  Set8(c, pcie + 0x00u, 0x10);        /* PCIe capability id */
  Set8(c, pcie + 0x01u, 0x00);        /* next = none */
  Set16(c, pcie + 0x02u, 0x0002);     /* version 2, endpoint */
  Set16(c, pcie + 0x0au, (UINT16)card.errors);
  Set8(c, pcie + 0x0cu, (UINT8)((card.max_speed & 0x0fu) | ((card.max_width & 0x3fu) << 4)));
  Set8(c, pcie + 0x0du, (UINT8)((card.max_width & 0x3fu) >> 4));
  Set16(c, pcie + 0x10u, (UINT16)(card.aspm & 3u));
  Set16(c, pcie + 0x12u,
        (UINT16)((card.speed & 0x0fu) | ((card.width & 0x3fu) << 4) |
                 (card.trained ? 0u : (1u << 11u)) |
                 (card.up ? (1u << 13u) : 0u)));
  Set8(c, pcie + 0x14u, (UINT8)(((card.hotplug & 1u) << 6) | ((card.surprise & 1u) << 5)));
}

static CARD Healthy(void) {
  CARD card = {16, 16, 3, 4, 0, 1, 1, 0, 0, 0};
  return card;
}

int main(void) {
  UINT32 c[64];
  GU_LINK link;
  CARD card;

  /* 1. A healthy x16 Gen3 link with ASPM off is accepted, and every observed
   *    field is reported as read. */
  card = Healthy();
  Card(c, 0x50, card);
  memset(&link, 0xff, sizeof(link));
  EXPECT(GuCheckLinkState(c, 0x50, &link) == GU_LINK_OK);
  EXPECT(GuLinkAccepted(GU_LINK_OK));
  EXPECT(link.width == 16 && link.max_width == 16);
  EXPECT(link.speed == 3 && link.max_speed == 4);
  EXPECT(link.aspm == 0 && link.trained == 1 && link.up == 1 && link.errors == 0);
  EXPECT(strcmp(GuLinkReason(GU_LINK_OK), "OK") == 0);

  /* 2. A card that already logged a sticky PCIe error is refused first, before
   *    any other condition is even considered. */
  card = Healthy();
  card.errors = 0x02;      /* FER: a fatal error was already recorded */
  card.aspm = 1;           /* and ASPM is enabled, to prove the ordering */
  Card(c, 0x50, card);
  EXPECT(GuCheckLinkState(c, 0x50, &link) == GU_LINK_ERROR_LOGGED);
  EXPECT(!GuLinkAccepted(GU_LINK_ERROR_LOGGED));
  EXPECT(link.errors == 0x02 && link.aspm == 1);
  EXPECT(strcmp(GuLinkReason(GU_LINK_ERROR_LOGGED), "PCIE_ERROR_LOGGED") == 0);

  /* 3. Only the uncorrected bits refuse. A fatal or non-fatal error is an event
   *    the link did not recover from; a correctable error and an unsupported
   *    request are recorded and not weighed, because firmware enumeration sets
   *    them on this bench on every boot (RUN0008 and RUN0009: 0x9 on both cards,
   *    before any operating system ran, after a full AC removal). */
  for (UINT8 bit = 0; bit < 4; ++bit) {
    card = Healthy();
    card.errors = (UINT8)(1u << bit);
    Card(c, 0x50, card);
    GU_LINK_VERDICT expected = (bit == 1u || bit == 2u) ? GU_LINK_ERROR_LOGGED : GU_LINK_OK;
    EXPECT(GuCheckLinkState(c, 0x50, &link) == expected);
    EXPECT(link.errors == (UINT8)(1u << bit));
  }
  /* The state the bench actually presents: correctable error and unsupported
   * request together, on a trained x16 gen4 link. It is reported, and it is not
   * a refusal. */
  card = Healthy();
  card.errors = 0x09;
  Card(c, 0x50, card);
  EXPECT(GuCheckLinkState(c, 0x50, &link) == GU_LINK_OK);
  EXPECT(link.errors == 0x09 && GuLinkAccepted(GU_LINK_OK));

  /* 4. An ASPM request this image does not manage is refused, with the state
   *    reported so a log says which request it was. */
  card = Healthy();
  card.aspm = 3;
  Card(c, 0x50, card);
  EXPECT(GuCheckLinkState(c, 0x50, &link) == GU_LINK_ASPM_ENABLED);
  EXPECT(link.aspm == 3 && link.errors == 0);
#ifdef GU_UNVERIFIED_BENCH_CONDITIONS
  EXPECT(GuLinkAccepted(GU_LINK_ASPM_ENABLED));
#else
  EXPECT(!GuLinkAccepted(GU_LINK_ASPM_ENABLED));
#endif
  EXPECT(strcmp(GuLinkReason(GU_LINK_ASPM_ENABLED), "ASPM_ENABLED") == 0);

  /* 5. An untrained link is refused. A trained link whose DLLLA bit reads 0 is
   *    not: the bit reads 0 on this bench over a link that is negotiating x16
   *    gen4 and answering the config read that produced this snapshot, so it is
   *    reported (link.up) and the verdict ignores it. */
  card = Healthy();
  card.trained = 0;
  card.up = 0;
  Card(c, 0x50, card);
  EXPECT(GuCheckLinkState(c, 0x50, &link) == GU_LINK_NOT_TRAINED);
  EXPECT(link.trained == 0);
  card = Healthy();
  card.up = 0;
  Card(c, 0x50, card);
  EXPECT(GuCheckLinkState(c, 0x50, &link) == GU_LINK_OK);
  EXPECT(link.trained == 1 && link.up == 0 && link.width == 16 && link.speed == 3);

  /* 6. Trained, active, and still useless: no negotiated width or no speed. */
  card = Healthy();
  card.width = 0;
  Card(c, 0x50, card);
  EXPECT(GuCheckLinkState(c, 0x50, &link) == GU_LINK_UNUSABLE);
  card = Healthy();
  card.speed = 0;
  Card(c, 0x50, card);
  EXPECT(GuCheckLinkState(c, 0x50, &link) == GU_LINK_UNUSABLE);
  card = Healthy();
  card.speed = 0x0f;       /* reserved encoding */
  Card(c, 0x50, card);
  EXPECT(GuCheckLinkState(c, 0x50, &link) == GU_LINK_UNUSABLE);

  /* ASPM alone is the only admitted exception. When the same real PCI
   * snapshot includes a missing/unsafe link, return that stronger refusal
   * before the ASPM result that an experimental build may waive. */
  card = Healthy();
  card.aspm = 2;
  card.trained = 0;
  Card(c, 0x50, card);
  EXPECT(GuCheckLinkState(c, 0x50, &link) == GU_LINK_NOT_TRAINED);
  EXPECT(!GuLinkAccepted(GU_LINK_NOT_TRAINED));
  card = Healthy();
  card.aspm = 2;
  card.width = 0;
  Card(c, 0x50, card);
  EXPECT(GuCheckLinkState(c, 0x50, &link) == GU_LINK_UNUSABLE);
  EXPECT(!GuLinkAccepted(GU_LINK_UNUSABLE));
  card = Healthy();
  card.aspm = 2;
  card.speed = 0;
  Card(c, 0x50, card);
  EXPECT(GuCheckLinkState(c, 0x50, &link) == GU_LINK_UNUSABLE);
  card = Healthy();
  card.aspm = 2;
  card.speed = 0x0f;
  Card(c, 0x50, card);
  EXPECT(GuCheckLinkState(c, 0x50, &link) == GU_LINK_UNUSABLE);
  card = Healthy();
  card.aspm = 2;
  card.errors = 0x04;
  Card(c, 0x50, card);
  EXPECT(GuCheckLinkState(c, 0x50, &link) == GU_LINK_ERROR_LOGGED);
  EXPECT(!GuLinkAccepted(GU_LINK_ERROR_LOGGED));

  /* 7. A degraded link is a fact to log, not a refusal: x1 Gen1 still trains,
   *    still reports its advertised maximum, and is accepted. */
  card = Healthy();
  card.width = 1;
  card.speed = 1;
  Card(c, 0x50, card);
  EXPECT(GuCheckLinkState(c, 0x50, &link) == GU_LINK_OK);
  EXPECT(link.width == 1 && link.max_width == 16 && link.speed == 1 && link.max_speed == 4);

  /* 8. Hot-plug capability is observed and never silently dropped. */
  card = Healthy();
  card.hotplug = 1;
  card.surprise = 1;
  Card(c, 0x50, card);
  EXPECT(GuCheckLinkState(c, 0x50, &link) == GU_LINK_OK);
  EXPECT(link.hotplug == 1 && link.surprise == 1);

  /* 9. The offsets come from the capability, not from a fixed 0x50. */
  card = Healthy();
  Card(c, 0x70, card);
  EXPECT(GuCheckLinkState(c, 0x70, &link) == GU_LINK_OK);
  EXPECT(link.width == 16);
  /* Reading it at the wrong offset must not report the same link. */
  EXPECT(GuCheckLinkState(c, 0x50, &link) != GU_LINK_OK);

  /* 10. Anything that is not a usable PCIe capability offset is ABSENT, and
   *     the output is cleared rather than left holding the previous card. */
  card = Healthy();
  Card(c, 0x50, card);
  EXPECT(GuCheckLinkState(c, 0x00, &link) == GU_LINK_ABSENT);
  EXPECT(link.width == 0 && link.up == 0);
  EXPECT(GuCheckLinkState(c, 0x51, &link) == GU_LINK_ABSENT);   /* unaligned */
  EXPECT(GuCheckLinkState(c, 0x30, &link) == GU_LINK_ABSENT);   /* below 0x40 */
  EXPECT(GuCheckLinkState(c, 0xf0, &link) == GU_LINK_ABSENT);   /* runs past 256 */
  EXPECT(GuCheckLinkState(NULL, 0x50, &link) == GU_LINK_ABSENT);
  EXPECT(GuCheckLinkState(c, 0x50, NULL) == GU_LINK_ABSENT);
  EXPECT(strcmp(GuLinkReason(GU_LINK_ABSENT), "PCIE_CAPABILITY_ABSENT") == 0);

  /* 11. Every verdict has a reason token, and no two share one. */
  {
    const GU_LINK_VERDICT all[] = {GU_LINK_OK, GU_LINK_ABSENT, GU_LINK_ERROR_LOGGED,
                                   GU_LINK_ASPM_ENABLED, GU_LINK_NOT_TRAINED,
                                   GU_LINK_DOWN, GU_LINK_UNUSABLE};
    for (UINTN i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
      EXPECT(GuLinkReason(all[i]) != NULL && GuLinkReason(all[i])[0]);
#ifdef GU_UNVERIFIED_BENCH_CONDITIONS
      EXPECT(GuLinkAccepted(all[i]) == (all[i] == GU_LINK_OK || all[i] == GU_LINK_ASPM_ENABLED));
#else
      EXPECT(GuLinkAccepted(all[i]) == (all[i] == GU_LINK_OK));
#endif
      for (UINTN j = i + 1; j < sizeof(all) / sizeof(all[0]); ++j)
        EXPECT(strcmp(GuLinkReason(all[i]), GuLinkReason(all[j])) != 0);
    }
  }

  if (Failures) { fprintf(stderr, "FAILURES=%d\n", Failures); return 1; }
  printf("STATUS=COMPLETE PASS=%d PCI_LINK_STATE_CASES\n", Checks);
  return 0;
}
"""


def harness() -> str:
    types = source_type_block()
    return HARNESS.replace("__REAL_TYPES__", types).replace(
        "__ACTUAL_CODE__",
        "\n\n".join(source_body(n) for n in ("ConfigByte", "ConfigWord", "GuCheckLinkState",
                                             "GuLinkAccepted", "GuLinkReason")))


def source_type_block() -> str:
    """The real GU_LINK struct and verdict enum, copied from ``src/checks.h``."""
    header = (ROOT / "src" / "checks.h").read_text(encoding="utf-8")
    start = header.index("typedef struct {", header.index("C8:"))
    end = header.index("BOOLEAN GuLinkAccepted", start)
    return header[start:end]


MOCK_HEADER = (ROOT / "tests" / "gpu_discovery_mock.h").read_text(encoding="utf-8")


def declaration(text, tail):
    """The compact token form of the ``typedef`` that ends with ``tail``."""
    end = text.index(tail) + len(tail)
    begin = text.rindex("typedef", 0, end)
    block = text[begin:end]
    block = re.sub(r"/\*.*?\*/", " ", block, flags=re.DOTALL)
    return " ".join(block.split())


class MockAgreesWithTheRealHeader(unittest.TestCase):
    def test_the_mock_carries_the_same_link_type(self):
        for tail in ("GU_LINK;", "GU_LINK_VERDICT;"):
            with self.subTest(type=tail):
                real = declaration((ROOT / "src" / "checks.h").read_text(encoding="utf-8"), tail)
                mock = declaration(MOCK_HEADER, tail)
                self.assertEqual(real, mock)

    def test_the_mock_can_refuse_a_link(self):
        source = (ROOT / "tests" / "test_gpu_discovery.c").read_text(encoding="utf-8")
        self.assertIn("static GU_LINK_VERDICT link_refusal;", source)
        self.assertIn("REASON=PCIE_LINK", source)
        # And the active path is checked end to end in that mock.
        self.assertIn("link_refusal=GU_LINK_ASPM_ENABLED;", source)
        self.assertIn("CHECK(init_calls==0 && link_refusals==1);", source)


class PciLinkState(unittest.TestCase):
    def test_the_link_is_read_before_anything_is_actuated(self):
        compiler = os.environ.get("CC", "gcc")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "link.c"
            binary = Path(directory) / "link"
            source.write_text(harness(), encoding="utf-8")
            built = subprocess.run(
                [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "src"), str(source), "-o", str(binary)],
                capture_output=True, text=True, timeout=60, check=False,
            )
            self.assertEqual(built.returncode, 0, built.stderr[-3000:])
            executed = subprocess.run([str(binary)], capture_output=True, text=True,
                                      timeout=30, check=False)
            self.assertEqual(executed.returncode, 0, executed.stderr[-3000:])
            # LOOPS run some expectations more than once, so the source count is
            # a floor and the runtime count is what the harness reports.
            declared = harness().count("EXPECT(") - 1
            self.assertGreaterEqual(declared, 30)
            match = re.search(r"STATUS=COMPLETE PASS=(\d+) PCI_LINK_STATE_CASES",
                              executed.stdout)
            self.assertIsNotNone(match, executed.stdout)
            self.assertGreaterEqual(int(match.group(1)), declared)

    def test_explicit_bench_override_admits_aspm_only(self):
        compiler = os.environ.get("CC", "gcc")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "link.c"
            binary = Path(directory) / "link"
            source.write_text(harness(), encoding="utf-8")
            built = subprocess.run(
                [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-DGU_UNVERIFIED_BENCH_CONDITIONS", "-I", str(ROOT / "src"),
                 str(source), "-o", str(binary)],
                capture_output=True, text=True, timeout=60, check=False,
            )
            self.assertEqual(built.returncode, 0, built.stderr[-3000:])
            executed = subprocess.run([str(binary)], capture_output=True, text=True,
                                      timeout=30, check=False)
            self.assertEqual(executed.returncode, 0, executed.stderr[-3000:])
            self.assertRegex(executed.stdout, r"STATUS=COMPLETE PASS=\d+ PCI_LINK_STATE_CASES")

    def test_the_active_path_observes_and_refuses(self):
        discovery = (ROOT / "src" / "gpu_discovery.c").read_text(encoding="utf-8")
        self.assertIn("GuCheckLinkState(config,caps.pcie,&link)", discovery)
        self.assertIn("PCI_LINK=%s WIDTH=%u", discovery)
        self.assertIn("REASON=PCIE_LINK LINK_REASON=%s", discovery)
        # The observation must sit after the admission gate it complements.
        self.assertLess(discovery.index("GuCheckConfigInitialMaster(config,selected.segment"),
                        discovery.index("GuCheckLinkState(config,caps.pcie"))
        # A refusal here must not claim retention: nothing was mutated.
        tail = discovery[discovery.index("REASON=PCIE_LINK LINK_REASON=%s"):
                         discovery.index("REASON=PCIE_LINK LINK_REASON=%s") + 400]
        self.assertNotIn("retained=TRUE", tail)

    def test_what_is_not_observable_is_named(self):
        discovery = (ROOT / "src" / "gpu_discovery.c").read_text(encoding="utf-8")
        self.assertIn("AER=UNOBSERVED", discovery)
        self.assertIn("UNHANDLED=HOTPLUG,AER", discovery)
        header = (ROOT / "src" / "checks.h").read_text(encoding="utf-8")
        self.assertIn("extended capability space", header)
        # The policy must not invent an AER field it cannot read.
        self.assertNotIn("aer", source_type_block())

    def test_the_verdicts_are_the_hazards_named_in_c8(self):
        header = (ROOT / "src" / "checks.h").read_text(encoding="utf-8")
        for verdict in ("GU_LINK_ERROR_LOGGED", "GU_LINK_ASPM_ENABLED",
                        "GU_LINK_NOT_TRAINED", "GU_LINK_DOWN", "GU_LINK_UNUSABLE"):
            with self.subTest(verdict=verdict):
                self.assertIn(verdict, header)
        # And the names in the log are the ones the docs quote.
        source = SOURCE
        for token in ("PCIE_ERROR_LOGGED", "ASPM_ENABLED", "LINK_NOT_TRAINED",
                      "LINK_DOWN", "LINK_UNUSABLE"):
            with self.subTest(token=token):
                self.assertIn('"%s"' % token, source)


if __name__ == "__main__":
    unittest.main()
