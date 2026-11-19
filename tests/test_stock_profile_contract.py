"""Contract tests for the compiled stock profile as a precondition (A4, partial).

Which stock selector policy an image implements is still a build-time choice.
What changed is that the choice is now *enforced*: before this pass the expected
cold SS0 was a constant defined in `port.h` and referenced nowhere, so an image
built for one card would run its destructive path on another presenting a
different value.

These tests pin the enforced half:

  1. the check happens after the selector sanity rules and before anything else
     in preparation, and it only reads;
  2. a contradiction is a clean refusal - no fault, no retry, no write;
  3. the refusal names the compiled profile, the expected value and the observed
     one, so an operator can tell which build they are holding;
  4. every profile branch still names itself, and only the two non-latched
     branches carry an SS0 expectation;
  5. the build-time selection rules are unchanged: exactly one profile, and no
     hand-written SS0.

Choosing the policy at runtime is *not* implemented: the two entry-latched
policies are indistinguishable before the write, so no probe can pick between
them. That remains an operator/build decision - see §11.11.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"

PORT_H = (SRC / "port.h").read_text(encoding="utf-8")
PROFILE = (SRC / "profile.c").read_text(encoding="utf-8")
HEADER = (SRC / "stock_profile.h").read_text(encoding="utf-8")

REGISTERS = PROFILE[PROFILE.index("static BOOLEAN RegistersFor("):
                    PROFILE.index("\nBOOLEAN GuAd104ColdEntryFor(")]
PREPARE = PROFILE[PROFILE.index("EFI_STATUS GuAd104Prepare(VOID) {"):]


class TheExpectationIsNotDeadText(unittest.TestCase):
    def test_constants_are_referenced_outside_port_h(self):
        # The finding this change fixes: the constants existed and were used by
        # nobody, so the compiled profile was documentation.
        for name in ("GU_STOCK_PROFILE_SS0", "GU_STOCK_PROFILE_NAME"):
            self.assertIn(name, PROFILE, name)
            uses = [line for line in PROFILE.splitlines() if name in line]
            self.assertTrue(uses, name)

    def test_each_profile_names_itself(self):
        branches = re.findall(r'#define GU_STOCK_PROFILE_NAME "([a-zA-Z0-9]+)"',
                              PORT_H)
        self.assertEqual(sorted(branches),
                         ["entryLatchedSs1Full", "entryLatchedSs1Preserve",
                          "observed03000062", "original03100062"])

    def test_only_non_latched_profiles_expect_a_value(self):
        values = re.findall(r"#define GU_STOCK_PROFILE_SS0 (0x[0-9a-fA-F]+)U", PORT_H)
        self.assertEqual(values, ["0x03100062", "0x03000062"])
        # The latched policies latch what the card presents: no expectation.
        latched = PORT_H[PORT_H.index("#elif defined(GU_AD104_TRANSACTION) && defined(GU_ENTRY_LATCHED_SS1_PRESERVE)"):]
        self.assertNotIn("GU_STOCK_PROFILE_SS0", latched)

    def test_selection_rules_are_unchanged(self):
        self.assertIn("#error Select exactly one stock profile", PORT_H)
        self.assertIn("Stock profile value and name must be derived from the "
                      "explicit selector", " ".join(PORT_H.split()))


class TheCheckReadsOnly(unittest.TestCase):
    def test_check_follows_the_sanity_rules(self):
        sanity = REGISTERS.index("Selectors invalid:")
        check = REGISTERS.index("STOCK_PROFILE=REFUSED")
        confirmed = REGISTERS.index("Registers: cold state verified")
        self.assertLess(sanity, check)
        self.assertLess(check, confirmed)

    def test_preparation_writes_nothing(self):
        for forbidden in ("GuWrite32", "GuWrite32For", "GuFlr", "GuMsi", "Dma"):
            self.assertNotIn(forbidden, REGISTERS, forbidden)

    def test_the_check_cannot_fault_the_card(self):
        block = REGISTERS[REGISTERS.index("STOCK_PROFILE=REFUSED") - 400:
                          REGISTERS.index("STOCK_PROFILE=REFUSED") + 400]
        self.assertNotIn("fault=TRUE", block)
        self.assertNotIn("retained=TRUE", block)


class AContradictionIsACleanRefusal(unittest.TestCase):
    def test_refusal_is_recorded_and_returns_false(self):
        block = REGISTERS[REGISTERS.index("#ifdef GU_STOCK_PROFILE_SS0"):]
        block = block[: block.index("#endif")]
        self.assertIn("StockProfileMismatch=TRUE", block)
        self.assertIn("return FALSE", block)

    def test_prepare_reports_unsupported_not_not_ready(self):
        self.assertIn("return StockProfileMismatch?EFI_UNSUPPORTED:EFI_NOT_READY;",
                      PROFILE)

    def test_flag_is_cleared_before_any_early_return(self):
        first = PREPARE.index("StockProfileMismatch=FALSE;")
        for gate in ("TURING_PREPARE=REFUSED", "SKU_GEOMETRY=REFUSED",
                     "state invalid"):
            self.assertLess(first, PREPARE.index(gate), gate)

    def test_refusal_message_is_actionable(self):
        start = REGISTERS.index("STOCK_PROFILE=REFUSED")
        message = REGISTERS[start:REGISTERS.index("return FALSE", start)]
        for field in ("COMPILED=%s", "EXPECTED_SS0=%#x", "OBSERVED_SS0=%#x",
                      "REASON=CARD_DOES_NOT_MATCH_BUILD"):
            self.assertIn(field, message, field)

    def test_a_match_is_also_reported(self):
        self.assertIn("STOCK_PROFILE=MATCH", REGISTERS)


class TheHeaderStatesItsLimit(unittest.TestCase):
    def test_predicate_is_total_and_narrowing_only(self):
        text = " ".join(HEADER.split())
        self.assertIn("never widens one", text)
        self.assertIn("no probe can tell them apart", text)

    def test_no_runtime_selector_pretends_to_exist(self):
        # Nothing may read an operator variable to pick a stock policy while the
        # two latched policies remain indistinguishable.
        for token in ("GetVariable", "SetVariable", "L\"GuStock"):
            self.assertNotIn(token, PROFILE, token)


if __name__ == "__main__":
    unittest.main()
