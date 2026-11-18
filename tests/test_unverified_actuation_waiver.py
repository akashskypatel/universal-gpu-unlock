"""Contract tests for the explicit GU_ACTIVE_UNVERIFIED_ACTUATION waiver.

The waiver is a documented operator acceptance, not a verification result.
Nothing in this repository observes a physical GPU, so these tests pin only
the shape of the decision:

  1. the define is absent by default and is rejected next to every
     preparation-only, synthetic or host-test profile;
  2. it is the only configuration in which an ordered multi-target list
     compiles and binds for a real (non-synthetic) active image;
  3. every artifact, log line and status line it produces states that the
     outcome is unverified, non-deployable and not eligible.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
PORT = (SRC / "port.h").read_text(encoding="utf-8")
DISCOVERY = (SRC / "gpu_discovery.c").read_text(encoding="utf-8")
RELEASE = (SRC / "msi_release.c").read_text(encoding="utf-8")
APP = (SRC / "Build-Experimental-MSVC.ps1").read_text(encoding="utf-8")
DXE = (SRC / "Build-Dxe-Driver.ps1").read_text(encoding="utf-8")
BUILDERS = (("Build-Experimental-MSVC.ps1", APP), ("Build-Dxe-Driver.ps1", DXE))

# Minimal waiver-complete define set: firmware transaction with one explicit
# stock profile plus the ordered selected-target execution mode.
COMPLETE = (
    "-DGU_ACTIVE_UNVERIFIED_ACTUATION",
    "-DGU_AD104_TRANSACTION",
    "-DGU_ENTRY_LATCHED_SS1_FULL",
    "-DGU_ACTIVE_SINGLE_TARGET",
)
PROBE = '#include "port.h"\nint gu_waiver_probe;\n'


def host_compiler():
    return shutil.which("clang") or shutil.which("gcc")


def run_cc(compiler, flags, source, include=(), hosted=False):
    """Compile a probe. Firmware probes are freestanding and carry the UEFI
    port marker; ``hosted`` drops both, which is the only way the simulator
    profile is allowed to exist."""
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "probe.c"
        path.write_text(source, encoding="utf-8")
        environment = [] if hosted else ["-ffreestanding", "-DGU_UEFI_PORT"]
        command = [compiler, "-std=c11", "-fsyntax-only", "-fshort-wchar",
                   *environment,
                   "-I", str(SRC),
                   "-I", str(SRC / "vendor" / "edk2" / "Include"),
                   "-I", str(SRC / "vendor" / "edk2" / "Include" / "X64")]
        command += list(include)
        command += list(flags) + [str(path)]
        done = subprocess.run(command, capture_output=True, text=True)
        return done.returncode, done.stdout + done.stderr


def selection_header(tmp_root, count):
    directory = Path(tmp_root) / "selected"
    directory.mkdir(parents=True, exist_ok=True)
    buses = (7, 9, 3, 5)[:count]
    entries = ", ".join("{ 0U,%dU,0U,0U }" % bus for bus in buses)
    header = "#define GU_GPU_SELECTION_COUNT %dU\n" % count
    header += "#define GU_GPU_SELECTION_LOCATIONS { %s }\n" % entries
    (directory / "gpu-selection.h").write_text(header, encoding="utf-8")
    return directory


class WaiverHeaderMatrix(unittest.TestCase):
    def setUp(self):
        self.compiler = host_compiler()
        if not self.compiler:
            self.skipTest("A host C compiler is required")

    def assertRefused(self, flags, reason):
        status, output = run_cc(self.compiler, flags, PROBE)
        self.assertNotEqual(status, 0, "waiver combination was admitted: %s" % (flags,))
        self.assertIn(reason, output)

    def test_waiver_is_not_part_of_any_default_profile(self):
        for name, text in BUILDERS:
            self.assertEqual(text.count("/DGU_ACTIVE_UNVERIFIED_ACTUATION"), 1, name)
        self.assertIn("#if GU_ACTUATION_ADMITTED\n#define GU_PREFLIGHT_ONLY 0", PORT)
        # Only two profiles may define GU_ACTUATION_ADMITTED: the operator
        # waiver and the hosted simulator. Nothing else admits I/O writes.
        self.assertEqual(PORT.count("#define GU_ACTUATION_ADMITTED 1"), 2)
        self.assertEqual(PORT.count("#define GU_ACTUATION_ADMITTED 0"), 1)
        # The last branch, taken by every unspecified build, still refuses I/O.
        tail = PORT.split("#define GU_PREFLIGHT_ONLY 0", 1)[1]
        self.assertIn("#else\n#define GU_PREFLIGHT_ONLY 1\n#endif", tail)

    def test_waiver_requires_transaction_and_ordered_mode(self):
        self.assertRefused(
            ("-DGU_ACTIVE_UNVERIFIED_ACTUATION",),
            "Unverified actuation requires the experimental firmware transaction profile")
        self.assertRefused(
            ("-DGU_ACTIVE_UNVERIFIED_ACTUATION", "-DGU_AD104_TRANSACTION",
             "-DGU_ENTRY_LATCHED_SS1_FULL"),
            "Unverified actuation requires the ordered selected-target execution mode")

    def test_waiver_rejects_preparation_synthetic_and_host_profiles(self):
        self.assertRefused(
            COMPLETE + ("-DGU_SYNTHETIC_COMPILE_ONLY",),
            "Unverified actuation cannot be combined with preparation-only or synthetic builds")
        self.assertRefused(
            COMPLETE + ("-DGU_ACTIVE_SEQUENCE_PREPARE_ONLY",),
            "Unverified actuation cannot be combined with preparation-only or synthetic builds")
        self.assertRefused(
            COMPLETE + ("-DGU_HOST_TEST_PROFILE",),
            "Unverified actuation requires the reviewed local boot profile")

    def test_complete_waiver_set_compiles(self):
        status, output = run_cc(self.compiler, COMPLETE, PROBE)
        self.assertEqual(status, 0, output)

    def test_default_probe_still_compiles(self):
        status, output = run_cc(self.compiler, (), PROBE)
        self.assertEqual(status, 0, output)


SIMULATED = (
    "-DGU_SIMULATED_ACTUATION",
    "-DGU_AD104_TRANSACTION",
    "-DGU_ENTRY_LATCHED_SS1_FULL",
    "-DGU_ACTIVE_SINGLE_TARGET",
)


class SimulatorProfile(unittest.TestCase):
    """The hosted simulator relaxes the same admission checks as the waiver.

    It must be impossible to ship: freestanding builds, the UEFI port marker,
    the waiver, and the preparation-only/synthetic profiles all refuse it.
    """

    def setUp(self):
        self.compiler = host_compiler()
        if not self.compiler:
            self.skipTest("A host C compiler is required")

    def assertRefused(self, flags, reason, hosted=False):
        status, output = run_cc(self.compiler, flags, PROBE, hosted=hosted)
        self.assertNotEqual(status, 0, "profile was admitted: %s" % (flags,))
        self.assertIn(reason, output)

    def test_simulator_is_refused_in_a_firmware_build(self):
        self.assertRefused(SIMULATED, "Simulated actuation is a hosted-only build")
        self.assertRefused(SIMULATED, "Simulated actuation must not use the UEFI port marker")

    def test_simulator_requires_transaction_and_ordered_mode(self):
        self.assertRefused(("-DGU_SIMULATED_ACTUATION",),
                           "Simulated actuation requires the experimental firmware transaction profile",
                           hosted=True)
        self.assertRefused(("-DGU_SIMULATED_ACTUATION", "-DGU_AD104_TRANSACTION"),
                           "Simulated actuation requires the ordered selected-target execution mode",
                           hosted=True)

    def test_simulator_rejects_preparation_synthetic_and_waiver_profiles(self):
        self.assertRefused(SIMULATED + ("-DGU_ACTIVE_SEQUENCE_PREPARE_ONLY",),
                           "Simulated actuation cannot be combined with preparation-only or synthetic builds",
                           hosted=True)
        self.assertRefused(SIMULATED + ("-DGU_SYNTHETIC_COMPILE_ONLY",),
                           "Simulated actuation cannot be combined with preparation-only or synthetic builds",
                           hosted=True)
        self.assertRefused(SIMULATED + ("-DGU_ACTIVE_UNVERIFIED_ACTUATION",),
                           "Simulated actuation and the hardware waiver are distinct profiles",
                           hosted=True)

    def test_simulator_profile_compiles_only_when_hosted(self):
        status, output = run_cc(self.compiler, SIMULATED, PROBE, hosted=True)
        self.assertEqual(status, 0, output)

    def test_no_builder_can_produce_a_simulated_image(self):
        for name, text in BUILDERS:
            self.assertNotIn("GU_SIMULATED_ACTUATION", text, name)
        runner = (ROOT / "tools" / "run_simulation.py").read_text(encoding="utf-8")
        self.assertIn("-DGU_SIMULATED_ACTUATION", runner)
        # The simulated card satisfies the same config policy as hardware, so
        # the synthetic ESP/host-test profile is never mixed in.
        self.assertNotIn("GU_HOST_TEST_PROFILE", runner)


class OrderedTargetAdmission(unittest.TestCase):
    def setUp(self):
        self.compiler = host_compiler()
        if not self.compiler:
            self.skipTest("A host C compiler is required")

    def compile_discovery(self, flags, count):
        with tempfile.TemporaryDirectory() as tmp:
            include = ("-I", str(selection_header(tmp, count)))
            command = [self.compiler, "-std=c11", "-fsyntax-only", "-I", str(SRC)]
            command += list(include) + list(flags) + [str(ROOT / "tests" / "test_gpu_discovery.c")]
            done = subprocess.run(command, capture_output=True, text=True)
            return done.returncode, done.stdout + done.stderr

    def test_multi_target_needs_the_waiver(self):
        status, output = self.compile_discovery(("-DGU_ACTIVE_SINGLE_TARGET",), 3)
        self.assertNotEqual(status, 0)
        self.assertIn("One-target active execution requires exactly one PCI address", output)

    def test_waiver_admits_an_ordered_multi_target_list(self):
        status, output = self.compile_discovery(COMPLETE, 3)
        self.assertEqual(status, 0, output)

    def test_waiver_also_serves_a_single_target(self):
        status, output = self.compile_discovery(COMPLETE, 1)
        self.assertEqual(status, 0, output)

    def test_runtime_refusal_survives_without_the_waiver(self):
        guard = DISCOVERY.split("if(prepared.selected_count!=1 && !GU_ACTUATION_ADMITTED){", 1)
        self.assertEqual(len(guard), 2)
        self.assertIn("ACTIVE_EXECUTION=REFUSED REASON=MULTI_TARGET_WITHOUT_WAIVER SELECTED=%zu", guard[1])
        self.assertIn("s=EFI_UNSUPPORTED;goto done;", guard[1].split("}", 1)[0])
        # Translation units that do not include port.h must still refuse.
        self.assertIn("#ifndef GU_ACTUATION_ADMITTED\n#define GU_ACTUATION_ADMITTED 0", DISCOVERY)

    def test_waiver_records_authorisation_without_claiming_eligibility(self):
        waived = DISCOVERY.split("ACTIVE_EXECUTION=WAIVED_UNVERIFIED", 1)[1]
        line = " ".join(waived.split(");", 1)[0].split())
        for token in ("ORDERED_SEQUENTIAL=1", "MUTATION_AUTHORIZED=1",
                      "CAN_DETACH=0", "CAN_UNLOCK=0", "CAN_FLASH=0",
                      "RECOVERY_VERIFIED=0", "SIGNED_BOOTER_UNVERIFIED=1",
                      "WAIVER=GU_ACTIVE_UNVERIFIED_ACTUATION"):
            self.assertIn(token, line)
        for claim in ("CAN_DETACH=1", "CAN_UNLOCK=1", "RECOVERY_VERIFIED=1"):
            self.assertNotIn(claim, DISCOVERY)


class ReleaseAdmission(unittest.TestCase):
    def test_original_boards_stay_refused_by_default(self):
        guard = RELEASE.split("#if !GU_ACTUATION_ADMITTED", 1)[1]
        guard = guard.split("#else", 1)[0]
        self.assertIn("if(Gu.device_id==0x2684U||Gu.device_id==0x220aU) {", guard)
        self.assertIn("MSI release refused for original AD102/GA102", guard)
        self.assertIn("return EFI_UNSUPPORTED;", guard)

    def test_waived_release_records_the_unverified_outcome(self):
        waived = RELEASE.split("#if GU_ACTUATION_SIMULATED", 1)[1]
        waived = waived.split("#else", 1)[1].split("#endif", 1)[0]
        self.assertIn("MSI release WAIVED for original AD102/GA102", waived)
        for token in ("VENDOR_STOP_BOUND=UNVERIFIED", "SIGNED_BOOTER=UNVERIFIED",
                      "RECOVERY=UNVERIFIED", "WAIVER=GU_ACTIVE_UNVERIFIED_ACTUATION"):
            self.assertIn(token, waived)
        self.assertNotIn("return EFI_UNSUPPORTED;", waived)

    def test_simulated_release_cannot_claim_verification(self):
        simulated = RELEASE.split("#if GU_ACTUATION_SIMULATED", 1)[1]
        simulated = simulated.split("#else", 1)[0]
        self.assertIn("MSI release SIMULATED for original AD102/GA102", simulated)
        self.assertIn("ENVIRONMENT=HOST_SIMULATOR", simulated)
        for token in ("VENDOR_STOP_BOUND=UNVERIFIED", "SIGNED_BOOTER=UNVERIFIED",
                      "RECOVERY=UNVERIFIED"):
            self.assertIn(token, simulated)
        # A simulated run must never borrow the hardware waiver's vocabulary.
        self.assertNotIn("WAIVER=", simulated)

    def test_other_devices_are_unaffected(self):
        # The refusal/admission pair is keyed on the two original boards only.
        self.assertEqual(RELEASE.count("Gu.device_id==0x2684U||Gu.device_id==0x220aU"), 2)


class BuilderOptIn(unittest.TestCase):
    def test_waiver_is_a_switch_on_both_entrypoints(self):
        for name, text in BUILDERS:
            self.assertIn("[switch]$AcceptUnverifiedActuation", text, name)

    def test_switch_requires_ordered_active_execution(self):
        for name, text in BUILDERS:
            self.assertIn("-AcceptUnverifiedActuation requires -EnableSingleTargetExecution", text, name)
            self.assertIn("-AcceptUnverifiedActuation cannot be combined with -SyntheticCompileProfile", text, name)

    def test_default_build_still_refuses_live_execution(self):
        for name, text in BUILDERS:
            self.assertIn(
                "if ($EnableSingleTargetExecution -and -not $SyntheticCompileProfile -and -not $AcceptUnverifiedActuation) {",
                text, name)
            self.assertIn("Live GPU execution build refused", text, name)

    def test_waived_build_emits_real_transaction_defines(self):
        for name, text in BUILDERS:
            self.assertIn(
                "$clFlags += ' /DGU_ACTIVE_SINGLE_TARGET /DGU_ACTIVE_UNVERIFIED_ACTUATION'",
                text, name)
            # Host-test and synthetic defines must never accompany the waiver.
            waived = text.split("if ($AcceptUnverifiedActuation) {", 1)[1].split("} elseif", 1)[0]
            self.assertNotIn("GU_HOST_TEST_PROFILE", waived, name)
            self.assertNotIn("GU_SYNTHETIC_COMPILE_ONLY", waived, name)

    def test_multi_target_selection_is_allowed_only_when_waived(self):
        for name, text in BUILDERS:
            self.assertIn(
                "if ($EnableSingleTargetExecution -and $selection.Count -ne 1 -and -not $AcceptUnverifiedActuation) {",
                text, name)
            self.assertIn("-AcceptUnverifiedActuation requires at least one explicit", text, name)

    def test_status_line_never_claims_verification(self):
        for name, text in BUILDERS:
            self.assertIn("UNVERIFIED_ACTUATION=$([int][bool]$AcceptUnverifiedActuation)", text, name)
            for token in ("ACTUATION_VERIFIED=0", "CAN_DETACH=0", "CAN_UNLOCK=0",
                          "CAN_FLASH=0", "RECOVERY_VERIFIED=0",
                          "WAIVER=GU_ACTIVE_UNVERIFIED_ACTUATION"):
                self.assertIn(token, text, name)
            self.assertIn(
                "NOT_FOR_DEPLOYMENT=$([int]($SyntheticCompileProfile -or $AcceptUnverifiedActuation))",
                text, name)

    def test_waived_artifacts_are_named_as_unverified(self):
        for name, text in BUILDERS:
            self.assertIn("'GPU-ONE-TARGET-UNVERIFIED-ACTUATION", text, name)
            self.assertIn("'GPU-SEQUENCE-UNVERIFIED-ACTUATION", text, name)


class PreprocessedBranchSelection(unittest.TestCase):
    """The two profiles must be textually exclusive, not merely both present."""

    def setUp(self):
        self.compiler = host_compiler()
        if not self.compiler:
            self.skipTest("A host C compiler is required")

    def preprocess(self, source, defines, extra_includes=()):
        with tempfile.TemporaryDirectory() as tmp:
            command = [self.compiler, "-E", "-P", "-DGU_UEFI_PORT",
                       "-I", str(SRC),
                       "-I", str(SRC / "vendor" / "edk2" / "Include"),
                       "-I", str(SRC / "vendor" / "edk2" / "Include" / "X64")]
            command += list(extra_includes) + list(defines) + [str(SRC / source)]
            done = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stderr[-2000:])
            return done.stdout

    def test_release_refusal_and_admission_are_exclusive(self):
        waived = self.preprocess("msi_release.c", COMPLETE)
        self.assertIn("MSI release WAIVED", waived)
        self.assertNotIn("MSI release refused", waived)
        plain = self.preprocess("msi_release.c", ())
        self.assertIn("MSI release refused", plain)
        self.assertNotIn("MSI release WAIVED", plain)

    def test_discovery_waiver_log_requires_the_waiver(self):
        with tempfile.TemporaryDirectory() as tmp:
            selection = selection_header(tmp, 3)
            waived = self.preprocess("gpu_discovery.c", COMPLETE, ("-I", str(selection)))
            self.assertIn("ACTIVE_EXECUTION=WAIVED_UNVERIFIED", waived)
        with tempfile.TemporaryDirectory() as tmp:
            # A one-target transaction build compiles without the waiver, which
            # is exactly the shape the run-time multi-target refusal lives in.
            selection = selection_header(tmp, 1)
            plain = self.preprocess("gpu_discovery.c",
                                    ("-DGU_ACTIVE_SINGLE_TARGET",
                                     "-DGU_AD104_TRANSACTION",
                                     "-DGU_ENTRY_LATCHED_SS1_FULL"),
                                    ("-I", str(selection)))
            self.assertIn("MULTI_TARGET_WITHOUT_WAIVER", plain)
            self.assertNotIn("ACTIVE_EXECUTION=WAIVED_UNVERIFIED", plain)


class OrderedStepLogHygiene(unittest.TestCase):
    def test_step_lines_are_newline_terminated(self):
        # A literal backslash-n glues the next line onto a HALTED marker, which
        # is exactly the line an operator reads first after a failed step.
        for marker in ("SELECTED_GPU_STEP=ACTIVE", "SELECTED_GPU_STEP=COMPLETE",
                       "SELECTED_GPU_STEP=BOUND", "SELECTED_GPU_STEP=HALTED"):
            line = DISCOVERY.split(marker, 1)[1].split("\n", 1)[0]
            self.assertNotIn("\\\\n", line, marker)
        self.assertIn('SELECTED_GPU_STEP=HALTED NEXT=%zu MUTATION_AUTHORIZED=0\\n"', DISCOVERY)


class WaivedBuildSourceSet(unittest.TestCase):
    """The waived define set must preprocess the whole image, not just a probe.

    This is a preprocessor check: it proves no conditional in the shared
    sources requires a preparation-only or synthetic define to stay
    well-formed. It is not a compile, and it is not a qualification.
    """

    SOURCES = (
        "app.c", "dxe_main.c", "driver_nvram.c", "boot_io.c", "msi_release.c",
        "unlock_execution.c", "port.c", "compat.c", "checks.c",
        "gpu_inventory.c", "gpu_discovery.c", "gpu_execution_state.c",
        "gpu_windows_path.c", "profile.c", "sha256.c", "runtime.c", "format.c",
        "vendor/unlock_610.c", "vendor/prep_610.c", "vendor/layout_610.c",
        "vendor/vbios_610.c", "vendor/gfw_eval.c",
    )
    DEFINES = (
        "-D__KERNEL__", "-DGU_UEFI_PORT", "-DGU_AD104_TRANSACTION",
        "-DGU_ENTRY_LATCHED_SS1_FULL", "-DGU_ACTIVE_SINGLE_TARGET",
        "-DGU_ACTIVE_UNVERIFIED_ACTUATION",
    )

    def setUp(self):
        self.compiler = host_compiler()
        if not self.compiler:
            self.skipTest("A host C compiler is required")

    def test_every_source_preprocesses_with_the_waiver(self):
        with tempfile.TemporaryDirectory() as tmp:
            local = Path(tmp) / "local"
            local.mkdir()
            (local / "boot_profile.h").write_text(
                "#pragma once\n"
                "#define GU_WINDOWS_PARTITION_INITIALIZER "
                "{0x11111111,0x2222,0x3333,{0x84,0x44,0x55,0x66,0x77,0x88,0x99,0xaa}}\n",
                encoding="utf-8")
            selection = selection_header(tmp, 1)
            includes = ["-I", tmp, "-I", str(SRC), "-I", str(SRC / "compat"),
                        "-I", str(SRC / "vendor"),
                        "-I", str(SRC / "vendor" / "edk2" / "Include"),
                        "-I", str(SRC / "vendor" / "edk2" / "Include" / "X64"),
                        "-I", str(selection)]
            command = [self.compiler, "-E", "-P"] + list(self.DEFINES) + includes
            for source in self.SOURCES:
                path = SRC / source
                if not path.is_file():
                    self.skipTest("source set changed: %s is missing" % source)
                done = subprocess.run(command + [str(path)], capture_output=True,
                                      text=True, cwd=str(SRC))
                self.assertEqual(done.returncode, 0,
                                 "%s: %s" % (source, done.stderr[-1500:]))


class EligibilityWording(unittest.TestCase):
    def test_no_source_claims_eligibility(self):
        for path in sorted(SRC.rglob("*.c")) + sorted(SRC.rglob("*.h")):
            text = path.read_text(encoding="utf-8", errors="ignore")
            for claim in ("CAN_DETACH=1", "CAN_UNLOCK=1", "CAN_FLASH=1",
                          "RECOVERY_VERIFIED=1", "SIGNED_LOADER_VERIFIED=1"):
                self.assertNotIn(claim, text, path.name)


if __name__ == "__main__":
    unittest.main()
