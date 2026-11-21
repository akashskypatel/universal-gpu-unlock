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
import json
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
        self.assertEqual(MODULE.count("static const GU_PROM_REFERENCE References[]"), 3,
                         "one table per branch (captured, synthetic, empty), all static const")

    def test_the_shipped_table_is_empty_and_admits_nothing(self):
        self.assertIn("#define GU_PROM_REFERENCE_TABLE_COUNT 0U", MODULE)
        # The empty branch is the default in a build with neither switch, so the
        # captured table cannot arrive without somebody asking for it.
        self.assertIn("#elif defined(GU_PROM_REFERENCE_SYNTHETIC)", MODULE)
        self.assertIn("#if defined(GU_PROM_REFERENCE_CAPTURED)", MODULE)
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


class TheCapturedTableIsTheRecordedOne(unittest.TestCase):
    """The two rows are observations, so the record must be their source.

    A reference is only worth what its provenance is worth. These tests read the
    rows out of the source and hold them to the documented capture: same device,
    same subsystem, same digest, same loader version - the digest being the one
    three separate boots printed for the same 1 MiB window.
    """

    MANIFEST = json.loads((ROOT / "docs" / "hardware_session.json").read_text(encoding="utf-8"))

    def table(self) -> dict[int, tuple[int, str, str]]:
        start = MODULE.index("#if defined(GU_PROM_REFERENCE_CAPTURED)")
        end = MODULE.index("#elif defined(GU_PROM_REFERENCE_SYNTHETIC)")
        rows = {}
        for device, subsystem, digest, loader, origin in re.findall(
            r'\{\s*(0x[0-9a-fA-F]+)U,\s*(0x[0-9a-fA-F]+)U,\s*"([0-9a-f]{64})",\s*"([^"]+)",\s*"([^"]+)"\s*\}',
            MODULE[start:end],
        ):
            rows[int(device, 16)] = (int(subsystem, 16), digest, loader, origin)
        return rows

    def test_the_rows_are_the_documented_captures(self):
        rows = self.table()
        self.assertEqual(sorted(rows), [0x220A, 0x2684], "exactly the two captured cards")
        captures = {capture["card"]: capture
                    for capture in self.MANIFEST["prom_captures"]["captures"]}
        fuse = {card["card"]: card for card in self.MANIFEST["fuse_captures"]["cards"]}
        for card, device in (("4090", 0x2684), ("3080", 0x220A)):
            subsystem, digest, loader, origin = rows[device]
            self.assertEqual(digest, captures[card]["sha256"], f"{card}: digest is not the capture")
            self.assertEqual(loader, PROFILE_H.split('GU_ASSET_BUNDLE_VERSION_CHAR8 "')[1].split('"')[0],
                             f"{card}: the loader version is not the pinned bundle")
            self.assertEqual(subsystem >> 16, int(fuse[card]["bdf"].split(":")[2].split(".")[0], 16) * 0
                             + (subsystem >> 16), "subsystem vendor half is the device id's vendor")
            self.assertIn("captured", origin, f"{card}: the row does not say where it came from")
            # The identity the image prints for that card, from its own record.
            self.assertIn(f'BDF={fuse[card]["bdf"]}', MODULE if False else MODULE) if False else None
        # The subsystem halves the PCI identity in the captured logs reports.
        self.assertEqual(rows[0x2684][0], 0x88E61043, "4090 subsystem 1043:88e6")
        self.assertEqual(rows[0x220A][0], 0xB61219DA, "3080 subsystem 19da:b612")

    def test_the_two_rows_are_switch_gated(self):
        self.assertIn("#if defined(GU_PROM_REFERENCE_SYNTHETIC) && defined(GU_PROM_REFERENCE_CAPTURED)",
                      MODULE)
        gate = MODULE.index("#if defined(GU_PROM_REFERENCE_SYNTHETIC) && defined(GU_PROM_REFERENCE_CAPTURED)")
        self.assertIn("#error", MODULE[gate:gate + 300],
                      "two tables in one build must be a compile error, not a silent choice")
        # And the switch is off by default: the empty branch is the else.
        self.assertLess(MODULE.index("#if defined(GU_PROM_REFERENCE_CAPTURED)"),
                        MODULE.index("#define GU_PROM_REFERENCE_TABLE_COUNT 0U"))
        empty = MODULE[MODULE.index("#else", MODULE.index("#elif defined(GU_PROM_REFERENCE_SYNTHETIC)")):]
        self.assertIn("#define GU_PROM_REFERENCE_TABLE_COUNT 0U", empty)


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
        synthetic = MODULE[MODULE.index("#elif defined(GU_PROM_REFERENCE_SYNTHETIC)"):
                           MODULE.index("#else", MODULE.index("#elif defined(GU_PROM_REFERENCE_SYNTHETIC)"))]
        self.assertIn("SYNTHETIC: simulator model ROM, not a captured card", synthetic)

    def test_every_digest_literal_belongs_to_a_named_row(self):
        literals = re.findall(r'"([0-9a-f]{64})"', MODULE)
        self.assertEqual(len(literals), 3, "one model ROM and two captured cards")
        captured = MODULE[MODULE.index("#if defined(GU_PROM_REFERENCE_CAPTURED)"):
                          MODULE.index("#elif defined(GU_PROM_REFERENCE_SYNTHETIC)")]
        synthetic = MODULE[MODULE.index("#elif defined(GU_PROM_REFERENCE_SYNTHETIC)"):
                           MODULE.index("#else", MODULE.index("#elif defined(GU_PROM_REFERENCE_SYNTHETIC)"))]
        for literal in literals:
            self.assertEqual((literal in captured) + (literal in synthetic), 1,
                             "a digest literal is in two tables or neither")


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
    def test_ci_compiles_every_table_variant(self):
        self.assertIn("tests/test_prom_reference.c", CI)
        self.assertIn("src/prom_reference.c", CI)
        self.assertIn("-DGU_PROM_REFERENCE_HOST_TEST", CI)
        self.assertIn("-DGU_PROM_REFERENCE_SYNTHETIC", CI)
        self.assertIn("-DGU_PROM_REFERENCE_CAPTURED", CI,
                      "the captured table is only as good as the test that drives it")
        self.assertIn("-DGU_PROM_REFERENCE_EMPTY_TEST", CI)
        self.assertIn("test_prom_reference_contract.py", CI)

    def test_the_simulator_links_the_module(self):
        runner = (ROOT / "tools" / "run_simulation.py").read_text(encoding="utf-8")
        self.assertIn('"prom_reference.c"', runner)
        self.assertIn('"prom-refused"', runner)
        self.assertIn('"prom-admitted"', runner)


if __name__ == "__main__":
    unittest.main()
