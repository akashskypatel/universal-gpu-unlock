"""Contract tests for per-card firmware acceptance (multi-GPU step 2).

`prom_identity.h` answers "is this a structurally valid VBIOS naming this PCI
device", which is not authentication and has always logged
`SIGNATURE_AUTHENTICATED=0`. `prom_reference.[ch]` is the authenticated half that
can be settled offline: an explicit allowlist of captured
(device, subsystem, ROM digest, loader version) tuples, exact match only,
fail-closed on a miss.

These tests pin the shape of that contract:

  1. the allowlist is data, not code: nothing can register, extend or mutate a
     reference at runtime;
  2. the shipped table is empty, and an empty table admits nothing;
  3. the requirement is a compile-time build choice, checked at preparation and
     in the transaction - never in the read-only diagnostic, which exists to
     capture the very reference a refusal demands;
  4. a miss is `EFI_UNSUPPORTED` (not this image's card), not a hardware fault
     and not `EFI_COMPROMISED_DATA`;
  5. neither decision path claims authentication it does not have;
  6. the synthetic entry is test-only and cannot reach a firmware image.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
HEADER = (SRC / "prom_reference.h").read_text(encoding="utf-8")
MODULE = (SRC / "prom_reference.c").read_text(encoding="utf-8")
PROFILE = (SRC / "profile.c").read_text(encoding="utf-8")
PROFILE_H = (SRC / "profile.h").read_text(encoding="utf-8")
VENDOR = (SRC / "vendor" / "unlock_610.c").read_text(encoding="utf-8")
CI = (ROOT / ".github" / "workflows" / "gpu-topology-diagnostic.yml").read_text(
    encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    end = text.index("\n}\n", start)
    return text[start:end + 3]


class AllowlistIsData(unittest.TestCase):
    def test_nothing_can_register_a_reference_at_runtime(self):
        for banned in ("GuPromReferenceAdd", "GuPromReferenceRegister",
                       "GuPromReferenceSet", "GuPromReferenceLoad",
                       "GuPromReferenceInstall", "GuPromReferenceClear"):
            self.assertNotIn(banned, HEADER)
            self.assertNotIn(banned, MODULE)
        # The table is a static const array, so the only way in is a rebuild.
        self.assertIn("static const GU_PROM_REFERENCE References[]", MODULE)
        self.assertEqual(MODULE.count("static const GU_PROM_REFERENCE References[]"), 2,
                         "one table per branch, both static const")

    def test_the_shipped_table_is_empty_and_admits_nothing(self):
        self.assertIn("#define GU_PROM_REFERENCE_TABLE_COUNT 0U", MODULE)
        empty = MODULE[MODULE.index("#else"):]
        self.assertIn("GuPromReferenceCount(void)", MODULE)
        # Lookups iterate the count, so the placeholder row is unreachable.
        body = function_body(MODULE, "const GU_PROM_REFERENCE *GuPromReferenceFind(")
        self.assertIn("GuPromReferenceCount()", body)

    def test_the_match_is_exact_on_all_four_fields(self):
        body = function_body(MODULE, "GU_PROM_REF_DECISION GuPromReferenceDecide(")
        for field in ("DeviceId", "SubsystemId", "RomSha256", "LoaderVersion"):
            self.assertIn(field, body + function_body(
                MODULE, "const GU_PROM_REFERENCE *GuPromReferenceFind("))
        self.assertIn("GU_PROM_REF_UNKNOWN_CARD", body)
        self.assertIn("GU_PROM_REF_DIGEST_MISMATCH", body)
        self.assertIn("GU_PROM_REF_LOADER_MISMATCH", body)
        self.assertNotIn("strncmp", MODULE, "a prefix match would not be exact")
        self.assertNotIn("strstr", MODULE.replace(
            "strstr(Digest", "\0"))  # only the test may search text


class WhereTheRequirementApplies(unittest.TestCase):
    def test_preparation_requires_and_the_diagnostic_only_reports(self):
        self.assertEqual(PROFILE.count("GuAd104PromMatches("), 3,
                         "declaration plus two call sites")
        self.assertIn("GuAd104PromMatches(owner,prom,1048576U,FALSE)", PROFILE)
        self.assertIn("GuAd104PromMatches(&Gu,prom,1048576,TRUE)", PROFILE)
        diagnostic = PROFILE.index("DIAGNOSTIC PROM=")
        self.assertLess(PROFILE.index("GuAd104PromMatches(owner,prom,1048576U,FALSE)"),
                        diagnostic,
                        "the diagnostic reads the PROM before it reports")
        self.assertIn("GuAd104PromMatches(owner,rom,mapped,TRUE)", VENDOR,
                      "the transaction must hold the same requirement")

    def test_the_gate_is_a_compile_time_choice(self):
        self.assertIn("#if defined(GU_PROM_REFERENCE_REQUIRED)", PROFILE)
        gate = PROFILE.index("#if defined(GU_PROM_REFERENCE_REQUIRED)")
        refusal = PROFILE[gate:gate + 400]
        self.assertIn("PromReferenceMismatch=TRUE", refusal)
        self.assertIn("return FALSE", refusal)

    def test_a_miss_is_an_unsupported_card_not_a_fault(self):
        self.assertIn("if(!valid) return PromReferenceMismatch?EFI_UNSUPPORTED:EFI_COMPROMISED_DATA;",
                      PROFILE)
        self.assertIn("PromReferenceMismatch=FALSE;", PROFILE,
                      "the flag must be cleared at the start of every preparation")
        body = function_body(PROFILE, "EFI_STATUS GuAd104Prepare(VOID) {")
        self.assertLess(body.index("PromReferenceMismatch=FALSE;"),
                        body.index("PromReferenceMismatch?EFI_UNSUPPORTED"),
                        "cleared before it is read")

    def test_the_diagnostic_stays_open(self):
        self.assertIn("DIAGNOSTIC_ALLOWED=1", PROFILE)
        gate = PROFILE.index("#if defined(GU_PROM_REFERENCE_REQUIRED)")
        refusal = PROFILE[gate:gate + 400]
        self.assertNotIn("GuAd104Diagnostic", refusal)


class MessagesDoNotOverclaim(unittest.TestCase):
    def test_both_decisions_keep_signature_authentication_at_zero(self):
        verified = PROFILE.index("PROM_IDENTITY=VERIFIED")
        refused = PROFILE.index("PROM_IDENTITY=%s")
        self.assertIn("SIGNATURE_AUTHENTICATED=0", PROFILE[verified:verified + 300])
        self.assertIn("MODE=STRUCTURAL AUTHENTICATION=0", PROFILE)
        self.assertNotIn("SIGNATURE_AUTHENTICATED=1", PROFILE)

    def test_the_refusal_names_everything_an_operator_needs(self):
        refused = PROFILE[PROFILE.index("PROM_IDENTITY=%s"):
                          PROFILE.index("PROM_IDENTITY=%s") + 400]
        for field in ("DEVICE=%04x", "SUBSYS=%08x", "SHA256=%s", "REASON=%s",
                      "REFS=%u", "DIAGNOSTIC_ALLOWED=1"):
            self.assertIn(field, refused)
        self.assertIn('RequireReference?"REFUSED":"UNVERIFIED"', refused)

    def test_the_reason_tokens_are_the_documented_ones(self):
        for token in ('"ACCEPTED"', '"UNKNOWN_CARD"', '"DIGEST_MISMATCH"',
                      '"LOADER_MISMATCH"', '"MALFORMED"'):
            self.assertIn(token, MODULE)


class SyntheticIsTestOnly(unittest.TestCase):
    def test_the_synthetic_entry_cannot_reach_a_firmware_image(self):
        self.assertIn("#error The synthetic PROM reference is test-only", MODULE)
        self.assertIn("#error The synthetic PROM reference must not be part of a firmware image",
                      MODULE)
        guard = MODULE[MODULE.index("GU_PROM_REFERENCE_SYNTHETIC"):
                       MODULE.index("static const GU_PROM_REFERENCE References[]")]
        self.assertIn("GU_SIMULATED_ACTUATION", guard)
        self.assertIn("__STDC_HOSTED__ != 1", guard)

    def test_it_says_what_it_is(self):
        synthetic = MODULE[MODULE.index("#ifdef GU_PROM_REFERENCE_SYNTHETIC"):
                           MODULE.index("#else")]
        self.assertIn("SYNTHETIC: simulator model ROM, not a captured card", synthetic)

    def test_exactly_one_digest_literal_and_it_is_synthetic(self):
        literals = re.findall(r'"([0-9a-f]{64})"', MODULE)
        self.assertEqual(len(literals), 1)
        synthetic = MODULE[MODULE.index("#ifdef GU_PROM_REFERENCE_SYNTHETIC"):
                           MODULE.index("#else")]
        self.assertIn(literals[0], synthetic)


class OneVersionLiteral(unittest.TestCase):
    def test_the_loader_version_has_one_source(self):
        self.assertIn('#define GU_ASSET_BUNDLE_VERSION_CHAR8 "610.43.03"', PROFILE_H)
        self.assertIn("#define GU_ASSET_BUNDLE_VERSION GU_WIDEN_LITERAL(", PROFILE_H)
        # The reference comparison uses the same literal the manifests name.
        self.assertIn("GU_ASSET_BUNDLE_VERSION_CHAR8", PROFILE)
        self.assertIn('"610.43.03"', MODULE,
                      "the synthetic reference names the pinned bundle")

    def test_the_digest_length_is_the_sha256_one(self):
        self.assertIn("#define GU_PROM_REFERENCE_DIGEST_CHARS 64U", HEADER)


class BuildWiring(unittest.TestCase):
    def test_ci_compiles_both_table_variants(self):
        self.assertIn("tests/test_prom_reference.c", CI)
        self.assertIn("src/prom_reference.c", CI)
        self.assertIn("-DGU_PROM_REFERENCE_HOST_TEST", CI)
        self.assertIn("-DGU_PROM_REFERENCE_SYNTHETIC", CI)
        self.assertIn("test_prom_reference_contract.py", CI)

    def test_the_simulator_links_the_module(self):
        runner = (ROOT / "tools" / "run_simulation.py").read_text(encoding="utf-8")
        self.assertIn('"prom_reference.c"', runner)
        self.assertIn('"prom-refused"', runner)
        self.assertIn('"prom-admitted"', runner)


if __name__ == "__main__":
    unittest.main()
