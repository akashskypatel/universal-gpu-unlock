# Unverified actuation runbook — `-AcceptUnverifiedActuation`

This runbook is for the one build configuration in this repository that is
allowed to attempt the GPU firmware transaction. It exists because the owner
reviewed the gate map in
[`MULTI_GPU_REFACTOR_REVIEW.md` §4.1](MULTI_GPU_REFACTOR_REVIEW.md#41-gate-map)
and chose to open the admission gates anyway.

**Nothing here is verified.** No test, CI job or script in this repository
observes a GPU, a firmware transaction or a Windows boot after one. A waived
image is *permitted to try*; it is not *shown to work*. The eligibility flags
stay where they were:

```text
CAN_DETACH=false
CAN_UNLOCK=false
CAN_FLASH=false
RECOVERY_VERIFIED=false
```

If you are looking for a build that cannot write to hardware, use
`-SyntheticCompileProfile` or the plain selected build. Do not "just run" this
one.

---

## 1. Preconditions

| Requirement | How to check | If it fails |
|---|---|---|
| A reviewed `local/boot_profile.h` with your ESP GUID | File exists, `GU_WINDOWS_PARTITION_INITIALIZER` defined | Compile stops: `Configure the reviewed ESP GUID in local/boot_profile.h before building.` A waived build **cannot** use the synthetic host-test GUID. |
| Matched firmware payloads on the target ESP | `python3 tools/verify_reference_assets.py --root .` then copy `AD102-ASSETS/` (or `GA102-ASSETS/`) to the ESP root | Runtime: `Pinned asset unavailable from USB/configured ESP: <name> status=...` — the step stops before any MMIO write. |
| A writable ESP directory `\AD102-UNLOCK\` for the ordered log | Create it before booting | The run aborts; without a durable log there is no evidence of what happened. |
| The exact target BDF, read from *this* boot | Passive topology diagnostic (`READ_ONLY_GPU_TOPOLOGY_DIAGNOSTIC.md`) | A wrong BDF is refused only if PCI identity revalidation fails; a valid-but-wrong card is a real wrong-target risk. |
| Power-removal recovery within reach | Physical, not software | There is no software recovery. Do not run this remotely or headless. |
| An independent console | Serial or a second display that the NVIDIA firmware does not own | Without it the ordered log may be the only output, and it lives on the ESP you may need to remove. |

CI compiles the same waiver configuration on a Windows runner, with
`-RuntimeEsp` instead of a reviewable GUID and a synthetic BDF
(`tests/test_waiver_ci.py` pins that wiring). That job proves the sources build
with MSVC and the interlock refusals still fire; it does not run, boot or publish
anything. An operator building for a real machine still needs the reviewed
`local/boot_profile.h` below, or an explicit `-RuntimeEsp` build.

## 2. Build

```powershell
# First run: exactly one target.
.\src\Build-Experimental-MSVC.ps1 -SelectionBdf '0000:01:00.0' `
    -EnableSingleTargetExecution -AcceptUnverifiedActuation

# Ordered list: one card active at a time, each step gated on its predecessor.
.\src\Build-Experimental-MSVC.ps1 -SelectionBdfs '0000:01:00.0,0000:02:00.0' `
    -EnableSingleTargetExecution -AcceptUnverifiedActuation

# DXE driver entrypoint, same switches.
.\src\Build-Dxe-Driver.ps1 -SelectionBdf '0000:01:00.0' `
    -EnableSingleTargetExecution -AcceptUnverifiedActuation
```

Expected build status:

```text
STATUS=COMPLETE PATH=...\GPU-ONE-TARGET-UNVERIFIED-ACTUATION-*.EFI SHA256=...
SELECTED_COUNT=1 ... MULTI_GPU_MUTATION_ALLOWED=1 SYNTHETIC_PROFILE=0
UNVERIFIED_ACTUATION=1 ACTUATION_VERIFIED=0 CAN_DETACH=0 CAN_UNLOCK=0
CAN_FLASH=0 RECOVERY_VERIFIED=0 WAIVER=GU_ACTIVE_UNVERIFIED_ACTUATION
NOT_FOR_DEPLOYMENT=1
```

The artifact name contains `UNVERIFIED-ACTUATION`. If a builder produced a
`COMPILE-ONLY` or `ACTIVE-PREP` name instead, the switches did not reach it —
stop and re-read the parameters. The scripts refuse
`-AcceptUnverifiedActuation` without `-EnableSingleTargetExecution`, and refuse
it together with `-SyntheticCompileProfile`.

Compile-time guarantees you can rely on (enforced in `src/port.h`):

* the waiver requires `GU_AD104_TRANSACTION` and `GU_ACTIVE_SINGLE_TARGET`;
* it is an `#error` next to `GU_ACTIVE_SEQUENCE_PREPARE_ONLY`,
  `GU_SYNTHETIC_COMPILE_ONLY` and `GU_HOST_TEST_PROFILE`;
* it forces `GU_PREFLIGHT_ONLY 0`, so the guarded MMIO/PCI-write, DMA and FLR
  helpers are the only thing between this image and silicon.

## 3. Read the ordered log

The log is written to the ESP as `\AD102-UNLOCK\RUNnnnn.TXT`. Markers, in the
order a healthy run emits them:

| Marker | Meaning |
|---|---|
| `ACTIVE_GPU[i] BDF=... ID=... CLASS=...` | discovered identity of every NVIDIA GPU, including untouched peers |
| `LAUNCH_SOURCE DECISION=REFUSE REASON=... REMOVABLE=0 ...` | the image refused to start from this volume, and named the gate. In any build without `GU_ESP_RESIDENT` this is expected off the stick (`REASON=BUILD_REFUSES_ESP_RESIDENT`); any other reason means a resident image could not prove one of its gates. Record it: the reason is the finding. |
| `LAUNCH_SOURCE=ESP_RESIDENT_WINDOWS_ESP ALLOWED_BY=GU_ESP_RESIDENT INSTALL_STILL_REQUIRES_USB=1 ACTUATION_GATES_UNCHANGED=1` | a resident build started from the machine's own ESP. It says nothing about the run being safe: every actuation gate above still applies, and installing the bundle still needs the stick. |
| `ACTIVE_EXECUTION=WAIVED_UNVERIFIED SELECTED=<n> ORDERED_SEQUENTIAL=1 MUTATION_AUTHORIZED=1 ...` | the waiver is in force for this boot; `CAN_*` are still `0` |
| `MSI preflight: status=... graph=<n>` | owner graph captured |
| `MSI release WAIVED for original AD102/GA102 ... VENDOR_STOP_BOUND=UNVERIFIED` | the `0x2684`/`0x220a` refusal was replaced by an admission, not by a proof |
| `STOP_CONTRACT device=... DECISION=REFUSE MISSING=...` | the pre-Stop capture lists the facts this image cannot establish - target GOP, closed log, executed recovery, 617.42 Booter comparison, Stop bound, DMA idleness. Record this line with the run: it is the checklist a hardware session has to fill. |
| `SELECTED_GPU_STEP=ACTIVE ORDER=0 TARGET=0 PEERS=<n>` | the step that may write has begun |
| `SELECTED_GPU_STEP=COMPLETE NEXT=1 PEERS_VERIFIED=<n>` | the step finished and peer state was re-observed |
| `SELECTED_GPU_STEP=BOUND ORDER=1 TARGET=1` | only for multi-target runs: the successor context was published |
| `SELECTED_GPU_STEP=HALTED NEXT=<n> MUTATION_AUTHORIZED=0` | the shared coordinator stopped; no successor will start |
| `ACTIVE_EXECUTION=WAIVED_UNVERIFIED` appears **once per boot**, not per card | the waiver is a boot-level decision |

## 4. Stop conditions

Treat any of these as "stop, do not retry, power-cycle":

* `SELECTED_GPU_STEP=HALTED`
* `ACTIVE_SEQUENCE=REFUSED`, `ACTIVE_CONTEXTS=REFUSED`, `ACTIVE_PEER=DRIFT`
* `MSI finish refused`, `MSI release check failed`
* `TURING_PREPARE=REFUSED REASON=SIGNED_LOADER_NOT_VERIFIED` (Turing targets
  are still refused by design; the transaction code is AD104/GA10x-specific)
* any `FWSEC` refusal — the firmware descriptor did not match this card's ROM

Do not re-run in the same boot. The ordered coordinator refuses a repeated
step by design (`EFI_ALREADY_STARTED`), and a partially completed transaction
is not a state to experiment in.

## 4a. Score the captured log before you believe a word of it

[`DONOR_HARDWARE_SESSION.md`](DONOR_HARDWARE_SESSION.md) is the session plan as
gates, and `tools/verify_run_log.py` is the check:

```bash
python3 tools/verify_run_log.py \\media\\stick\\AD102-UNLOCK\\RUN0001.TXT \
                                \\media\\stick\\AD102-UNLOCK\\RUN0002.TXT --stage actuate
```

It evaluates the gates in `docs/hardware_session.json` against the format
strings the source actually prints, prints `MANUAL` for the six things only the
operator can confirm (the recovery toolkit on the bench, the donor card recorded,
the medium empty, one card only, a halt ending the session, Windows started
afterwards), and prints `NOT_PROVEN` for the nine things no log can establish -
starting with the Windows 617.42 Booter comparison. `HARDWARE_VERIFIED=0` is on
every line it prints, by construction.

A `PASS` here means the run did what this document says it does. It is not a
verdict on the card.

## 5. Capture afterwards, or the run proves nothing

1. The whole `RUNnnnn.TXT`.
2. SHA256 of the EFI image you actually booted.
3. Whether Windows enumerated the card, with which driver and which device
   status, plus `nvidia-smi -q` and a Tensor Core / CUDA smoke test result.
4. Whether a cold power cycle returns the card to stock behaviour.

Only items 3 and 4, recorded on the physical machine, can change any row of
[`MULTI_GPU_REFACTOR_REVIEW.md` §4.4](MULTI_GPU_REFACTOR_REVIEW.md#44-what-still-cannot-be-established-from-source).
A green log is not that evidence: it says the code reached a point, not that
the result is correct.

## 6. What this runbook cannot tell you

* Whether the transaction succeeds on this card at all.
* Whether the vendor Stop callback is bounded.
* Whether Windows 617.42 accepts the resulting firmware.
* Whether recovery works if the transaction fails mid-way.

Those are physical measurements. This document exists so that when you make
them, you make them deliberately, once, with the log on the ESP and the power
switch in reach.
