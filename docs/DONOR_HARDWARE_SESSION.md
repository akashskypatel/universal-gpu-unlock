# Donor-hardware session: the gates a run has to pass

The donor card, the programmer and the known-good image are on the bench. This
document is what turns that into a session with gates rather than an experiment
with hope. It is checked against the code and against a captured log, and it
**claims nothing**: no run described here has happened, `HARDWARE_VERIFIED=0` is
printed by every check, and the list at the end is everything a log can never
establish.

`docs/hardware_session.json` is the source of truth; this file is rendered from
it. `tools/verify_run_log.py` scores a captured log against the same manifest,
so the plan and the verdict cannot disagree.

## How a run is scored

```bash
# read-only contact: the diagnostic's own log
python3 tools/verify_run_log.py /media/stick/GPU-TOPOLOGY-DIAGNOSTIC-0001.TXT --stage readonly

# preparation only: the app's numbered log
python3 tools/verify_run_log.py /media/stick/AD102-UNLOCK/RUN0001.TXT --stage prepare

# waived actuation: the run's log and the next boot's, in that order
python3 tools/verify_run_log.py /media/stick/AD102-UNLOCK/RUN0001.TXT \
                                /media/stick/AD102-UNLOCK/RUN0002.TXT --stage actuate
```

Each stage is its own boot with its own log, so only the requested stage's gates
are evaluated: earlier stages are reported as `PRIOR_STAGE`, never as passes.
A log carrying the fixture marker is refused (exit 3) unless `--self-test` is
given, and a self-test verdict can never read `PASS`.

Exit codes: `0` every gate in the stage passed, `1` a gate failed, `2` a listed
log does not exist, `3` the log set is synthetic, `4` reserved for too few logs
(reported inline as a failing gate today).

Before the boot, `tools/session_preflight.py` checks the bench the same way:
that the medium carries no earlier `RUNnnnn.TXT` (a stale one is
indistinguishable from a live one), that the ESP profile holds a reviewed GUID
and not the committed placeholder, and that the image is a non-empty PE with its
SHA256 recorded:

```bash
python3 tools/session_preflight.py --stick /media/stick --image out/<the built>.EFI
```

It prints the same `MANUAL` gates and the same `HARDWARE_VERIFIED=0`. A clean
bench means nothing is obviously wrong before the run; it says nothing about a
card.

## The stages

### `readonly` — Read-only contact

*Costs:* nothing: the diagnostic never writes a GPU register, resets a Falcon, calls the vendor Stop or loads firmware

*Exit criteria:* the card is enumerated, its BDF and PCI identity are in the log, and the eligibility flags are printed as false

| Gate | What it checks | Requires | Must not contain |
| --- | --- | --- | --- |
| `R1_IMAGE_RAN_READ_ONLY` | the read-only image ran | `GPU TOPOLOGY DIAGNOSTIC ONLY - no GPU hardware writes or unlock calls` | `ACTIVE_EXECUTION=WAIVED_UNVERIFIED SELECTED=%zu`<br>`STOP_CONTRACT device=%04x stage=%zu` |
| `R2_ENUMERATION_COMPLETED` | the inventory finished without a vendor callback | `PCI_TOPOLOGY_SCOPE=DISPLAY_AND_PCI_BRIDGES INCLUDED=%zu EXCLUDED=%zu`<br>`NVIDIA_DRIVER_OWNER_INVENTORY=READ_ONLY IMAGE_PROVENANCE_UNVERIFIED=1`<br>`PCI_OBSERVATION_ONLY=1 NOT_TOPOLOGY=1 CAN_DETACH=0 CAN_UNLOCK=0` | `PCI_ENUM_FAILURE STAGE=device-read INDEX=%zu` |
| `R3_CARD_IDENTIFIED` | at least one NVIDIA card is named | `NVIDIA_DRIVER_OWNER[%zu] BDF=%04x:%02x:%02x.%u BY_DRIVER=%u` *(one of)*<br>`ACTIVE_GPU[%zu] BDF=%04x:%02x:%02x.%u ID=%04x:%04x` *(one of)* | — |

**`R1_IMAGE_RAN_READ_ONLY`** — proves: the read-only diagnostic started and wrote its own log. Does not prove: that the enumeration below finished, or anything about the card.

* `GPU TOPOLOGY DIAGNOSTIC ONLY - no GPU hardware writes or unlock calls` (src/gpu_topology_efi.c) — the banner is printed before any observation, so its presence is the cheapest proof that this build and not another one ran.
* must not contain `ACTIVE_EXECUTION=WAIVED_UNVERIFIED SELECTED=%zu` (src/gpu_discovery.c): a read-only session must not contain the waiver admission, which only the actuating build prints
* must not contain `STOP_CONTRACT device=%04x stage=%zu` (src/msi_release.c): the Stop contract is captured inside the release path; its presence means a Stop was attempted, which no read-only run does

**`R2_ENUMERATION_COMPLETED`** — proves: the scan reached its end and reported counts, with eligibility explicitly false. Does not prove: that the card is in a state any later stage could use.

* `PCI_TOPOLOGY_SCOPE=DISPLAY_AND_PCI_BRIDGES INCLUDED=%zu EXCLUDED=%zu` (src/gpu_topology_efi.c) — printed once the display and bridge scan has counted its handles, with the count in the line.
* `NVIDIA_DRIVER_OWNER_INVENTORY=READ_ONLY IMAGE_PROVENANCE_UNVERIFIED=1` (src/gpu_topology_efi.c) — the driver-owner inventory is the part that says which image owns the card, and it is explicitly not provenance.
* `PCI_OBSERVATION_ONLY=1 NOT_TOPOLOGY=1 CAN_DETACH=0 CAN_UNLOCK=0` (src/gpu_topology_efi.c) — the eligibility flags are printed as false, so the log itself states the session could not detach or unlock.
* must not contain `PCI_ENUM_FAILURE STAGE=device-read INDEX=%zu` (src/gpu_topology_efi.c): a partial enumeration is not an inventory; the failure line means some handle could not be read

**`R3_CARD_IDENTIFIED`** — proves: the run is about a real card with an address and an identity, not an empty machine. Does not prove: that the identity matches the donor card the operator recorded by hand.

* `NVIDIA_DRIVER_OWNER[%zu] BDF=%04x:%02x:%02x.%u BY_DRIVER=%u` (src/gpu_topology_efi.c) — the driver-owner line carries BDF, driver binding and opener state for one card.
* `ACTIVE_GPU[%zu] BDF=%04x:%02x:%02x.%u ID=%04x:%04x` (src/gpu_discovery.c) — the active-app inventory line carries BDF and PCI identity for one card.

### `prepare` — Preparation only

*Costs:* the active app runs its preflight and preparation, logs a refusal before any mutation, and hands back to Windows

*Exit criteria:* preparation completes with MUTATION_AUTHORIZED=0 and no Stop, no waiver and no step line

| Gate | What it checks | Requires | Must not contain |
| --- | --- | --- | --- |
| `P1_PREPARATION_REFUSED_BEFORE_MUTATION` | the app refused to actuate and said so first | `ACTIVE_APP=PREPARATION_ONLY ACTIVE_EXECUTION=REFUSED BEFORE_MUTATION=1`<br>`ACTIVE_PREPARE=COMPLETE SELECTED=%zu NVIDIA_COUNT=%zu MUTATION_AUTHORIZED=0` | `SELECTED_GPU_STEP=ACTIVE ORDER=%zu TARGET=%zu PEERS=%zu`<br>`STOP_CONTRACT device=%04x stage=%zu` |
| `P2_BOOT_RECORD_READ` | the persistent record was read before anything else | `BOOT_STATE PRESENT=1 PHASE=%u BOOT_COUNT=%u STATUS=%#llx DEVICE=%04x` *(one of)*<br>`BOOT_STATE PRESENT=0` *(one of)* | — |

**`P1_PREPARATION_REFUSED_BEFORE_MUTATION`** — proves: the entrypoint refused actuation and the preflight completed. Does not prove: that the preflight's conclusions are right; the counts say what was selected, not what was safe.

* `ACTIVE_APP=PREPARATION_ONLY ACTIVE_EXECUTION=REFUSED BEFORE_MUTATION=1` (src/app.c) — printed by the preparation-only entrypoint before any transaction, so the session is bounded by the entrypoint and not by luck.
* `ACTIVE_PREPARE=COMPLETE SELECTED=%zu NVIDIA_COUNT=%zu MUTATION_AUTHORIZED=0` (src/gpu_discovery.c) — preparation reached its end with mutation explicitly unauthorized.
* must not contain `SELECTED_GPU_STEP=ACTIVE ORDER=%zu TARGET=%zu PEERS=%zu` (src/gpu_discovery.c): a step that may write must not appear in a preparation session
* must not contain `STOP_CONTRACT device=%04x stage=%zu` (src/msi_release.c): no Stop may be attempted in a preparation session

**`P2_BOOT_RECORD_READ`** — proves: the record path works on this machine, in one of its two honest states. Does not prove: that the record can be written, or that it survives a power loss; that needs a run that is interrupted.

* `BOOT_STATE PRESENT=1 PHASE=%u BOOT_COUNT=%u STATUS=%#llx DEVICE=%04x` (src/boot_io.c) — a record from an earlier boot was found and decoded.
* `BOOT_STATE PRESENT=0` (src/boot_io.c) — no record was found, which is the correct state for the first boot on a wiped machine.

### `actuate` — Waived actuation

*Costs:* the one admission that may write: a single card, ordered, no retry, with the firmware timer bounded or explicitly off

*Exit criteria:* the waiver is recorded once, the pre-Stop contract is captured, the step reaches a terminal record, and the next boot reads the record it left

| Gate | What it checks | Requires | Must not contain |
| --- | --- | --- | --- |
| `A1_WAIVER_RECORDED_ONCE` | the waiver is in this boot's log, exactly once | `ACTIVE_EXECUTION=WAIVED_UNVERIFIED SELECTED=%zu ORDERED_SEQUENTIAL=1`<br>`WAIVER=GU_ACTIVE_UNVERIFIED_ACTUATION` | `ACTIVE_EXECUTION=SIMULATED SELECTED=%zu` |
| `A2_STOP_CONTRACT_CAPTURED` | the pre-Stop capture exists and names what it is missing | `STOP_CONTRACT device=%04x stage=%zu OWNER=%d COLD=%d CONSOLE=%d PEERS=%d`<br>`DECISION=%s MISSING=%s` | — |
| `A3_ONE_ORDERED_STEP_STARTED` | the step that may write began, in order | `SELECTED_GPU_STEP=ACTIVE ORDER=%zu TARGET=%zu PEERS=%zu` | — |
| `A4_STEP_TERMINATED` | the step reached a terminal record instead of the log ending | `SELECTED_GPU_STEP=COMPLETE NEXT=%zu PEERS_VERIFIED=%zu` *(one of)*<br>`SELECTED_GPU_STEP=HALTED NEXT=%zu MUTATION_AUTHORIZED=0` *(one of)*<br>`STOP: stage=%s status=%#llx mutated=%u retained=%u fault=%u` *(one of)* | — |
| `A5_NEXT_BOOT_READ_THE_RECORD` | a later boot read the record this run left | `BOOT_STATE PRESENT=1 PHASE=%u BOOT_COUNT=%u STATUS=%#llx DEVICE=%04x`<br>at least 2 logs | — |

**`A1_WAIVER_RECORDED_ONCE`** — proves: an actuating image reached its admission exactly once. Does not prove: that the admission was safe; the waiver is the reason it is not.

* `ACTIVE_EXECUTION=WAIVED_UNVERIFIED SELECTED=%zu ORDERED_SEQUENTIAL=1` (src/gpu_discovery.c) — the waiver is a boot-level decision: more than one occurrence means a retry or a second entrypoint, which the plan forbids. At most 1 per log.
* `WAIVER=GU_ACTIVE_UNVERIFIED_ACTUATION` (src/gpu_discovery.c) — names the waiver the diff admits, so the log cannot be mistaken for a verified build.
* must not contain `ACTIVE_EXECUTION=SIMULATED SELECTED=%zu` (src/gpu_discovery.c): a simulator run prints SIMULATED instead of WAIVED_UNVERIFIED; if both appear the log is not a clean hardware record

**`A2_STOP_CONTRACT_CAPTURED`** — proves: the image captured its pre-Stop state and stated which requirements it cannot establish. Does not prove: that any of the six unobservable facts hold - by construction they are the ones this image cannot see.

* `STOP_CONTRACT device=%04x stage=%zu OWNER=%d COLD=%d CONSOLE=%d PEERS=%d` (src/msi_release.c) — the capture names the device, the stage and the four observations that just held; the rest of the line is the requirements.
* `DECISION=%s MISSING=%s` (src/msi_release.c) — the same line ends with the decision and the missing set, so a capture that found nothing missing cannot be confused with one that did.

**`A3_ONE_ORDERED_STEP_STARTED`** — proves: an ordered step was admitted after every preflight. Does not prove: how the step ended; that is the next gate.

* `SELECTED_GPU_STEP=ACTIVE ORDER=%zu TARGET=%zu PEERS=%zu` (src/gpu_discovery.c) — the only line printed immediately before a card may be written.

**`A4_STEP_TERMINATED`** — proves: the log ends at a decision the image recorded itself, so truncation is distinguishable from an ending. Does not prove: that the ending was a good one; HALTED passes this gate on purpose.

* `SELECTED_GPU_STEP=COMPLETE NEXT=%zu PEERS_VERIFIED=%zu` (src/gpu_discovery.c) — the step completed and peer state was re-observed.
* `SELECTED_GPU_STEP=HALTED NEXT=%zu MUTATION_AUTHORIZED=0` (src/gpu_discovery.c) — the coordinator halted deliberately and promised no successor; this is a legitimate ending, not a failure.
* `STOP: stage=%s status=%#llx mutated=%u retained=%u fault=%u` (src/app.c) — the application recorded the transaction result, including whether the card was mutated, retained or faulted.

**`A5_NEXT_BOOT_READ_THE_RECORD`** — proves: the record survived whatever the actuation did, and a later boot could read it. Does not prove: that the record was correct, or that the machine came back warm rather than through a power cycle.

* `BOOT_STATE PRESENT=1 PHASE=%u BOOT_COUNT=%u STATUS=%#llx DEVICE=%04x` (src/boot_io.c) — the state that is supposed to survive a failed boot is only proved by a boot that comes after it.

## Manual gates

These cannot be checked from a log, so the verifier prints them as `MANUAL` on every run: they are remembered or they are skipped.

| Gate | What it means | Why it is not optional | What to record |
| --- | --- | --- | --- |
| `M1_RECOVERY_TOOLKIT_READY` | the physical recovery path is on the bench before the first run | an external programmer and a known-good firmware image are the only way back from a card that no longer answers; they cannot be fetched afterwards | the programmer model, the image digest, and the SPI flash part number, noted in the session log |
| `M2_DONOR_CARD_RECORDED` | the donor card's identity is written down before the run | the log will name a BDF and a device ID; without the card's serial and the slot it was in, those cannot be tied to a piece of silicon afterwards | serial number, slot, board model, the VBIOS version the vendor tool reports |
| `M3_LOG_MEDIUM_EMPTY` | the log medium starts empty | RUN0001.TXT must be this boot, or a stale file is indistinguishable from a live one and the gate verdicts describe the wrong run | a directory listing of the stick before the boot, photographed or copied |
| `M4_ONE_CARD_ONLY` | exactly one NVIDIA card, no peers | the waived path admits ordered single steps; a second card changes which gates apply and has never been exercised | the R2 inventory line, which must show one NVIDIA driver owner |
| `M5_HALT_STOPS_THE_SESSION` | a HALTED step ends the day, not the loop | no retry is a property of the image; re-running after a halt is the operator reintroducing the risk the image refused | the halt line, and the decision to stop, in the session log |
| `M6_STOCK_BOOT_AFTER_EVERY_RUN` | Windows is started normally with the card enumerated after every run | the log cannot see Windows; device start, driver binding and a display output have to be observed by a person | the Windows build, the driver version, and the device status in Device Manager |

## Advisories

Printed as `NOTE` lines. An absent advisory is a finding to explain, not a failure: these lines exist in some entrypoints and not others.

* `WATCHDOG_REPORTED` — `WATCHDOG BOUND=%u SECONDS ACTIVE=1` (or `WATCHDOG BOUND=0 SECONDS ACTIVE=0 REASON=NO_BUILD_THRESHOLD`) (src/app.c): the application prints whether the firmware timer is armed or deliberately off. The DXE entrypoint does not print this, so it is a note rather than a gate
* `LAUNCH_SOURCE_LOGGED` — `LAUNCH_SOURCE DECISION=%s REASON=%s` (src/boot_io.c): every boot now records where it started from and, when it refuses, which gate failed
* `ASSET_INSTALL_REPORTED` — `ASSET_INSTALL PRESENT=1 FILES=%u` (or `ASSET_INSTALL PRESENT=0`) (src/boot_io.c): says whether a bundle was ever staged on the stick, before discovery assumes anything about assets

## Never established by a log

* `WINDOWS_61742_SIGNED_BOOTER` — no code in this tree observes the Windows 617.42 Booter; the comparison is external evidence that has to be produced separately and has never been produced
* `TARGET_FIRMWARE_GOP_GONE` — the Stop contract's TARGET_GOP requirement is never observed - the image does not read the target's firmware GOP ownership
* `LOG_DIRECTORY_CLOSED` — the contract's LOG requirement needs the log closed before the Stop; the image cannot close the file it is writing to
* `RECOVERY_EXECUTED` — no recovery procedure has been executed on this card, so the requirement stays false no matter what the log shows
* `STOP_BOUND_MEASURED` — the vendor Stop callback has no measured worst-case duration; a log line that shows it returned says nothing about how long it could take
* `DMA_IDLE_PROVEN` — the contract's DMA requirement is not observable here; bus-master state is checked, engine idleness is not
* `WINDOWS_DEVICE_START` — the log ends at the handover; whether Windows started the device is only visible in Windows
* `CUDA_THROUGHPUT` — a benchmark number is a measurement on a machine, not a property of this source
* `COLD_STATE_AFTER_POWER_LOSS` — the state after cutting power mid-transaction has never been observed, and a clean run says nothing about it

## What every one of these checks refuses to claim

The last section of the manifest is printed by the verifier as `NOT_PROVEN` on
every run, pass or fail. It is the honest half of the document: these are the
things a log cannot show, and each one is why the readiness tracker still counts
hardware evidence as 0 of 43.

The 617.42 Booter comparison is the one to keep in view. It is external evidence
that has never existed in this repository, no code here observes it, and a donor
run does not produce it. A green session is a session that behaved as the plan
says; it is not a statement that the card is safe, that Windows started it, or
that the firmware accepted anything.
