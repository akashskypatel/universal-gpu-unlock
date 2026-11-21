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
python3 tools/verify_run_log.py /media/stick/GPU-TOPOLOGY-DIAGNOSTIC-0000.TXT --stage readonly

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

The images each stage needs are in [`image_variants.json`](image_variants.json):
`src/Build-Sign-Deploy-UniversalGPU.ps1 -Only <id>` builds and signs them, and
[`BUILD_SIGN_DEPLOY.md`](BUILD_SIGN_DEPLOY.md) is the user document for that
script. The session stages map to `diag-topology-log`, `prep-selected` and
`actuate-one-target`.

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

## Read-only hardware execution guide, step by step

The sections after this one describe the gates. This one is the sequence that produces a log to score, for every image in the catalogue that writes nothing to the GPU, and names the fact in each log that a hardware-dependent item is waiting for.

Every read-only and preparation run stored under `captured_runs` below was executed on this bench; **no actuating image has ever run**. Every tool in the chain prints `HARDWARE_VERIFIED=0`, and no captured log changes a row in `docs/readiness_status.json` by itself: a log is something a reviewer reads, not a flag this repository flips.

**The name.** There is no script called `build-sign-stage` in this repository. Building, signing and staging one image is a single command with two switches: `-Only <id>` chooses the image from `docs/image_variants.json`, and `-StageDrive <letter>` copies the signed `EFI\BOOT\BOOTX64.EFI` onto the stick after a verified backup of whatever was there. `src/Build-Sign-Deploy-UniversalGPU.ps1` is that command, and `docs/BUILD_SIGN_DEPLOY.md` is its user document.

### Before the first boot

* `M1_RECOVERY_TOOLKIT_READY` — the programmer, the known-good firmware image and the SPI part number are on the bench before the first boot; none of the images below writes the GPU, but the bench rule does not depend on that
* `M2_DONOR_CARD_RECORDED` — serial, slot, board model and the vendor tool's VBIOS version, so the BDF and device ID in the log can be tied to a piece of silicon afterwards
* `M3_LOG_MEDIUM_EMPTY` — no `GPU-TOPOLOGY-DIAGNOSTIC-*.TXT` and no `AD102-UNLOCK\RUN*.TXT` on the stick; a stale file is indistinguishable from a live one and `session_preflight.py` refuses a medium that carries one
* `M4_DONOR_CARDS_RECORDED` — for every stage: the serial and slot of every card in the machine, plus the ordered BDF list the image is built with. This fork runs an ordered list with one card active at a time, so a two-card machine is the ordinary case and the list, not the card count, is what has to be written down
* `M6_STOCK_BOOT_AFTER_EVERY_RUN` — after each candidate, boot Windows once and record the build, driver version and device status: the read-only images promise the card is untouched, and this is the observation that would notice if it were not
* `SIGNING_KEY` — the signing certificate lives in WSL (`$HOME/secureboot-signing/uefi-db.key` and `.crt`) and the same certificate must be enrolled in the BIOS `db`; the script refuses to sign when it is not, unless you pass `-SkipDbCheck` and accept a receipt that reads `DB_ENROLLED=UNKNOWN`
* `REPOSITORY_ROOT` — every command is written from the repository root, with the signed outputs landing in `artifacts\signed`

### The steps

**Step 1 — confirm the catalogue accepts the candidate**

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -List
```

*Expect:* one `CATALOGUE ... variants=31 families=4` line, then a `VARIANT <id> SAFETY=<class> BINARY=<name>` line per image; `NOT_BUILDABLE` lines say what this tree cannot produce and why. The candidate must be listed with the safety class in the table below.

*Look for:*

* `CATALOGUE {0} variants={1} families={2}` — printed by `src/Build-Sign-Deploy-UniversalGPU.ps1`
* `VARIANT {0} SAFETY={1} BINARY={2}` — printed by `src/Build-Sign-Deploy-UniversalGPU.ps1`

*If it does not:* a missing id is a catalogue problem, not a bench problem: fix the tree before touching the stick.

**Step 2 — build, sign and stage in one command**

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diag-topology-log -StageDrive G
```

*Expect:* `VARIANT_BEGIN` for the one image, `SIGNING_CERTIFICATE ... DB_ENROLLED=TRUE`, the verified signed SHA-256, `STAGED diag-topology-log PATH=... DRIVE=G:`, and a final `STATUS=SIGNED=1 FAILED=0 ... HARDWARE_VERIFIED=0`. A stick holds one `BOOTX64.EFI`, so this is one candidate per stick: the previous image is backed up locally before it is replaced.

*Look for:*

* `VARIANT_BEGIN [{0}/{1}] id={2} family={3} safety={4} builder={5}` — printed by `src/Build-Sign-Deploy-UniversalGPU.ps1`
* `SIGNING_CERTIFICATE path={0} SHA256={1} DB_ENROLLED={2}` — printed by `src/Build-Sign-Deploy-UniversalGPU.ps1`
* `STAGED {0} PATH={1} DRIVE={2}:` — printed by `src/Build-Sign-Deploy-UniversalGPU.ps1`
* `STATUS={0} SIGNED={1} FAILED={2} SKIPPED={3} DB_ENROLLED={4} HARDWARE_VERIFIED=0 MATRIX={5}` — printed by `src/Build-Sign-Deploy-UniversalGPU.ps1`

*If it does not:* `Refusing to stage` names the safety class and the missing switch. `-AcceptActuatingDeploy` exists for images that write the GPU; no read-only candidate in this guide needs it. `-SignOnly` builds and signs without touching a stick.

**Step 3 — check the bench before the boot**

```bash
python3 tools/session_preflight.py --stick /media/stick \
    --image /media/stick/EFI/BOOT/BOOTX64.EFI --skip-profile
```

*Expect:* `STICK FREE_OF_OLD_LOGS=1`, `IMAGE ... BYTES=... SHA256=...` for the file that will actually boot, the six `MANUAL` gates, and `PREFLIGHT_VERDICT=PASS ... CHECKS=2 HARDWARE_VERIFIED=0`. `--skip-profile` is right for every candidate here: the reviewed ESP profile belongs to the waived stages. Add `--assets AD102,GA102` once the run is meant to get past `EFI_CRC_ERROR`: the image reads the five pinned files from the stick's root and hashes each before use, and a 3080 boot reads `\AD102-ASSETS\gsp_ga10x.bin` through its manifest's alternate path, so the check names that dependency instead of leaving it to a later `Pinned asset unavailable` line.

*Look for:*

* `PREFLIGHT_VERDICT={verdict}{failed} CHECKS={checked} HARDWARE_VERIFIED=0` — printed by `tools/session_preflight.py`
* `STICK FREE_OF_OLD_LOGS=1` — printed by `tools/session_preflight.py`

*If it does not:* `STALE_LOGS=` with an `ACTION=` line means the medium is not empty: archive and wipe it (M3) rather than rebooting over an old log. `IMAGE EMPTY=1` or `PE_HEADER=0` means step 2 did not produce the file you think it did.

**Step 4 — boot the stick once, from the firmware boot menu**

*Expect:* Choose the USB entry once and let the image run to its own end. Do not retry inside the same session: the image takes the first free number on the medium, starting at 0000, so a second boot writes a second file and the two are only distinguishable by their numbers and their contents. The read-only candidates print `GPU hardware untouched` and return, and the handoff candidates start Windows after the log is closed.

*Look for:*

* `DIAGNOSTIC FINISHED; GPU hardware untouched.` — printed by `src/gpu_topology_efi.c`

*If it does not:* a hang is a finding: power-cycle, keep the stick as it is, and record what the screen last showed. Nothing in a read-only session writes the card, so a hang here is a firmware or console fault, not a damaged GPU.

**Step 5 — capture the log, then empty the medium**

```powershell
$evidence = Join-Path $env:USERPROFILE ('donor-evidence\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $evidence | Out-Null
$log = Get-ChildItem G:\ -Filter 'GPU-TOPOLOGY-DIAGNOSTIC-*.TXT' | Sort-Object Name | Select-Object -Last 1
Copy-Item -LiteralPath $log.FullName -Destination $evidence
Get-FileHash -Algorithm SHA256 (Join-Path $evidence $log.Name)
(Get-Content -LiteralPath $log.FullName | Measure-Object -Line).Lines
```

*Expect:* The file named by the log's own `LOG_FILE=` line is the one to copy (the numbers start at 0000 and count up; on a clean medium the first run is `-0000.TXT`). Copy it off before wiping anything, and record its SHA-256, byte count and line count next to the candidate id. Wipe the stick afterwards so the next candidate starts from an empty medium.

*Look for:*

* `LOG_FILE=\\GPU-TOPOLOGY-DIAGNOSTIC-%04u.TXT` — printed by `src/gpu_topology_efi.c`

*If it does not:* an empty stick with no log means the image never reached its save step. The screen said why: the last line before it stopped is the finding, and there is nothing to score.

**Step 6 — score the log against the gates**

```bash
python3 tools/verify_run_log.py ~/donor-evidence/<stamp>/GPU-TOPOLOGY-DIAGNOSTIC-0000.TXT --stage readonly
```

*Expect:* one `GATE <id> STAGE=<stage> RESULT=PASS` line per gate in the stage, the `NOTE` advisories, the six `MANUAL` gates, the `NOT_PROVEN` list, and `RUN_LOG_VERDICT=PASS ... HARDWARE_VERIFIED=0 GATES=3 GATES_FAILED=0`. Exit code `0` means every gate in that stage passed; `1` names the first failure in `FIRST_FAILED=`.

*Look for:*

* `HARDWARE_SESSION stage={stage} logs={len(logs)} ` — printed by `tools/verify_run_log.py`
* `RUN_LOG_VERDICT={verdict}{first} ` — printed by `tools/verify_run_log.py`
* `GATES_FAILED={len(failed)}` — printed by `tools/verify_run_log.py`

*If it does not:* exit `2` is a missing log, exit `3` is a log carrying the fixture marker (a synthetic log can never score as hardware evidence), and exit `1` is a real gate failure: `FIRST_FAILED=` names it, and the gate's own paragraph above says what it does not prove. For `prep-selected` and `dxe-prep-selected` the same command takes `--stage prepare` and the app's numbered log.

That is the loop: build-sign-stage, boot once, capture, score. The verdict is written into the session record beside the candidate id and the goal it was collected for. It does not change `hardware_verified` for anything, and the next section says why: a green read-only log is a statement about one image on one machine, not about a card.

### The read-only candidates

Every image in the catalogue whose safety class writes nothing to the GPU. One candidate per stick, one boot per candidate, one scored log per boot.

| Candidate | Safety | Stage | Log | Facts it puts on paper |
| --- | --- | --- | --- | --- |
| `diag-topology-log` | read-only | `readonly` | \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself | `B1` `B2` `B9` `E8` |
| `diag-topology-handoff` | read-only | `readonly` | \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself | `A2` `B9` |
| `diag-selection` | read-only | `readonly` | \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself | `A1` `C9` |
| `diag-msi-rom` | read-only | `readonly` | \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself | `B1` |
| `diag-msi-rom-handoff` | read-only | `readonly` | \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself | `A2` `B9` |
| `diag-msi-fuse` | read-only | `readonly` | \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself | `C4` `C3` `E7` |
| `diag-msi-fuse-handoff` | read-only | `readonly` | \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself | `A2` `B9` |
| `diag-msi-sec2` | read-only | `readonly` | \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself | `C4` |
| `diag-msi-sec2-handoff` | read-only | `readonly` | \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself | `A2` `B9` |
| `diag-vram-blocks` | read-only | `readonly` | \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself | `C5` `C3` |
| `diag-vram-blocks-handoff` | read-only | `readonly` | \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself | `A2` `B9` |
| `prep-selected` | refuses-before-mutation | `prepare` | \AD102-UNLOCK\RUNnnnn.TXT, one file per boot, never overwritten | `D5` `B9` `A7` `A4` `E8` `M4` `C9` `B10` |
| `dxe-prep-selected` | refuses-before-mutation | `prepare` | \AD102-UNLOCK\RUNnnnn.TXT, one file per boot, never overwritten | `D5` |

#### `diag-topology-log` — topology with a numbered USB log

The baseline read-only contact: PCI tree, GPU identities, GOP and console ownership, one numbered log. This is the image the R1-R3 gates were written for.

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diag-topology-log -StageDrive G
```

*Log:* \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself

*Score:* `python3 tools/verify_run_log.py <log> --stage readonly`

* `B1` — `TOPOLOGY_VERDICT=%s NVIDIA_CONOUT_BLOCKER=%u` (`src/gpu_topology_efi.c`): names the configured-console finding on the machine in front of you: whether an AMD display function owns the console, or an NVIDIA path is configured and blocks it
* `B1` — `ERROR_CONSOLE_ISOLATION=%s` (`src/gpu_topology_efi.c`): the error-console half of the same question: an independent AMD `ErrOut` observation is what a console-handover refactor would rest on
* `B2` — `GPU[%zu] BDF=%04x:%02x:%02x.%u ID=%04x:%04x SUB=%04x:%04x PCI_INDEX=%zu PARENT=%zu ROOT_PORT=%zu DEPTH=%u ANCESTRY=%u ROOT_VALID=%u GOP_COUNT=%u` (`src/gpu_topology_efi.c`): one line per GPU with its GOP count, so the surviving-non-target-GOP question B2 is about is answered for this cabling instead of assumed
* `B9` — `LOG_FILE=\\GPU-TOPOLOGY-DIAGNOSTIC-%04u.TXT` (`src/gpu_topology_efi.c`): a numbered log written by an image launched from USB is the writable-medium half of B9 observed rather than assumed
* `E8` — `PCI[%zu] %04x:%02x:%02x.%u %04x:%04x SUB=%04x:%04x CLASS=%04x ROOTPORT=%u PRIMARY=%02x BUSWINDOW=%02x-%02x BY_DRIVER=%u KNOWN=%u AGENT=%p PATH=%zu` (`src/gpu_topology_efi.c`): device and subsystem id of every PCI node: the identity row a per-SKU entry (AD104/AD106/AD107, other GA102 ids) starts from

*Does not prove:* that the card can be detached, unlocked or flashed: every eligibility flag in the log is zero by construction, and the report ends with `GPU hardware untouched`.

#### `diag-topology-handoff` — topology with a log and automatic Windows handoff

The same report, then the explicitly pinned Windows Boot Manager. The added value is not the topology: it is the observation that the reviewed ESP GUID boots Windows on this machine, after the log is closed.

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diag-topology-handoff -WindowsEspGuid '<reviewed GUID>' -StageDrive G
```

*Log:* \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself

*Score:* `python3 tools/verify_run_log.py <log> --stage readonly`

*Also collects* everything `diag-topology-log` collects, plus:

* `A2` — `WINDOWS_HANDOFF=REQUESTED; pinned Windows ESP, no reset/BootOrder write` (`src/gpu_topology_efi.c`): the handoff is pinned to one ESP GUID, and the image states it neither resets nor rewrites BootOrder
* `A2` — `Starting the explicitly pinned Windows Boot Manager directly.` (`src/gpu_topology_efi.c`): the line that must be followed by a Windows boot: the reviewed GUID is right only if Windows actually starts
* `B9` — `SAVE_LOG_STATUS=%#llx ROTATING_FILE=GPU-TOPOLOGY-DIAGNOSTIC-NNNN.TXT` (`src/gpu_topology_efi.c`): the log is durably written before the handoff begins, so a failed handoff still leaves the evidence

*Does not prove:* that the GUID names the only Windows loader, or that BitLocker, Secure Boot policy or anti-cheat accept that path (E10); it shows one pinned partition started Windows once.

#### `diag-selection` — selected-GPU observation with a log

The passive diagnostic with an ordered selection: one snapshot, every requested BDF resolved before any report, then per-target identity and a summary. This is the read-only image that says something about two cards.

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diag-selection -SelectionBdfs '0000:01:00.0,0000:04:00.0' -StageDrive G
```

*Log:* \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself

*Score:* `python3 tools/verify_run_log.py <log> --stage readonly`

* `A1` — `PASSIVE_SELECTION=GPU_IDENTIFIED TARGET=%04x:%02x:%02x.%u ID=%04x:%04x SUB=%04x:%04x CLASS=%04x` (`src/gpu_topology_efi.c`): the requested BDF resolved to a device and subsystem id from the live snapshot: the runtime selector's positive case on real handles
* `A1` — `SELECTION_SEQUENCE=REFUSED CODE=%d COMPLETED=0 REQUESTED=%u TOPOLOGY_VERDICT=%d` (`src/gpu_topology_efi.c`): a wrong or absent BDF refuses the whole sequence with no partial selection: the negative half of the same feature
* `C9` — `SELECTION_STEP[%zu]=PASSIVE_IDENTIFIED ID=%04x:%04x SUB=%04x:%04x CLASS=%04x ROOT_INDEX=%zu DEPTH=%u GOP_COUNT=%u BY_DRIVER=%u OPENERS_KNOWN=%u` (`src/gpu_topology_efi.c`): one numbered report per selected card in order, with its own GOP count and driver-owner status: the per-card data a multi-GPU refactor has to be written against
* `C9` — `SELECTION_SEQUENCE=PASSIVE_IDENTIFIED COMPLETED=%u NON_SELECTED_NVIDIA_COUNT=%zu TOPOLOGY_VERDICT=%d CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLR=0` (`src/gpu_topology_efi.c`): the aggregate says how many NVIDIA cards were seen but not selected, which is the peer set the interference question is about

*Does not prove:* that two cards can be actuated in order: the sequence is CPU-side reporting over one immutable snapshot, and B10's memory cost per additional card is untouched by it.

#### `diag-msi-rom` — MSI B850 ROM variables

Reads the MS-7E56 firmware's `ROM_CMN`/`CSM_CMN` variables and dumps them raw, on the board fingerprint the earlier console analysis was done on. This is the only passive route to the firmware's own display-selection policy; it changes nothing.

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diag-msi-rom -StageDrive G
```

*Log:* \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself

*Score:* `python3 tools/verify_run_log.py <log> --stage readonly`

* `B1` — `MSI_B850_ROM_VARIABLES PLATFORM_FINGERPRINT=MATCH SMBIOS_VERIFIED=0 BIOS_VERSION_VERIFIED=0 GPU_MUTATION=NONE` (`src/gpu_topology_efi.c`): the probe only runs on the exact board and GPU fingerprint, and it says in the same line that SMBIOS and the BIOS version are not verified
* `B1` — `MSI_B850_VARIABLE NAME=%s EFI_STATUS=%#llx BYTES=%zu ATTRS=%#x CAP=%u RAW_ONLY=1 READ_ONLY=1` (`src/gpu_topology_efi.c`): the raw bytes of the variable, with `RAW_ONLY=1`: the firmware's display policy is in there, and the log does not pretend to interpret it
* `B1` — `MSI_B850_BDS_POLICY_END NO_VENDOR_CALLBACK=1 NO_LIST_TRAVERSAL=1 NO_GPU_ACTION=1 POOL_UNCERTAIN=%u` (`src/gpu_topology_efi.c`): the line that bounds the probe: no vendor callback, no list traversal, no GPU action, so the dump cannot have changed the console state it describes

*Does not prove:* which byte is `Initiate Graphic Adapter`, or that clearing it in firmware would give an AMD-exclusive `ConOut`/`ErrOut`: the dump is raw, there is no schema, and nothing here authorises an NVRAM rewrite.

#### `diag-msi-rom-handoff` — MSI B850 ROM variables with handoff

The ROM-variable dump plus the pinned Windows handoff, so a passive capture of the firmware's display policy does not end with the operator hunting for a boot entry.

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diag-msi-rom-handoff -WindowsEspGuid '<reviewed GUID>' -StageDrive G
```

*Log:* \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself

*Score:* `python3 tools/verify_run_log.py <log> --stage readonly`

*Also collects* everything `diag-msi-rom` collects, plus:

* `A2` — `WINDOWS_HANDOFF=REQUESTED; pinned Windows ESP, no reset/BootOrder write` (`src/gpu_topology_efi.c`): the handoff is pinned to one ESP GUID and does not touch BootOrder
* `B9` — `SAVE_LOG_STATUS=%#llx ROTATING_FILE=GPU-TOPOLOGY-DIAGNOSTIC-NNNN.TXT` (`src/gpu_topology_efi.c`): the dump is written before the handoff, so the evidence survives a handoff that fails

*Does not prove:* anything the base ROM-variable probe does not prove, plus the handoff's own limit: one pinned partition started Windows once.

#### `diag-msi-fuse` — MSI B850 fuse version

One 32-bit read of the GSP fuse register per identity-verified card, plus one of the SEC2 register, and nothing else. The only image that reports which firmware signature slot a physical card's fuse selects.

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diag-msi-fuse -StageDrive G
```

*Log:* \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself

*Score:* `python3 tools/verify_run_log.py <log> --stage readonly`

* `C4` — `MSI_B850_FWSEC_FUSE GPU=%s STATUS=OBSERVED BAR0_OFFSET=0x8241e0 RAW=%#x LOW16=%#x FUSE_VERSION=%u ARCHIVED_ROM_SIG_MASK=%#x CANDIDATE_SLOT=%u ARCHIVED_SLOT_NONZERO=%u ACCEPTANCE=UNVERIFIED` (`src/gpu_topology_efi.c`): the fuse word and the slot it implies for each card: the per-card half of the Booter/V67/ROM pairing question, and the line ends `ACCEPTANCE=UNVERIFIED`
* `C3` — `MSI_B850_FWSEC_FUSE_BEGIN SOURCE=NOVA_CORE_GA102_PLUS READ_ONLY=1 CAN_DETACH=0 CAN_UNLOCK=0 NO_STOP=1 NO_DMA=1 NO_RESET=1` (`src/gpu_topology_efi.c`): the first line of the probe says it reads only, so it can never establish firmware acceptance: C3 stays where it is whatever the value is
* `E7` — `MSI_B850_SEC2_FUSE_BEGIN SOURCE=NOVA_CORE_GA102_PLUS REFERENCE_BOOTER=LINUX_610_43_03 ENGINE_MASK=0x1 UCODE_ID=3 WINDOWS_61742_DESCRIPTOR=UNKNOWN READ_ONLY=1` (`src/gpu_topology_efi.c`): the probe names the reference it decoded against (the Linux 610.43.03 header) and states that the Windows 617.42 descriptor is unknown: the asset-coupling claim, bounded in the log

*Does not prove:* that the slot this fuse names is the slot Windows 617.42 selects; the same line prints `WINDOWS_61742_SIGNATURE_SLOT=UNKNOWN`, and no signature is authenticated anywhere in this path.

#### `diag-msi-fuse-handoff` — MSI B850 fuse version with handoff

The fuse read plus the pinned Windows handoff: a card that is left alone still has to end the session in Windows, and this is the variant that says so in one boot.

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diag-msi-fuse-handoff -WindowsEspGuid '<reviewed GUID>' -StageDrive G
```

*Log:* \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself

*Score:* `python3 tools/verify_run_log.py <log> --stage readonly`

*Also collects* everything `diag-msi-fuse` collects, plus:

* `A2` — `WINDOWS_HANDOFF=REQUESTED; pinned Windows ESP, no reset/BootOrder write` (`src/gpu_topology_efi.c`): the handoff is pinned to one ESP GUID and does not touch BootOrder
* `B9` — `SAVE_LOG_STATUS=%#llx ROTATING_FILE=GPU-TOPOLOGY-DIAGNOSTIC-NNNN.TXT` (`src/gpu_topology_efi.c`): the fuse reads are written before the handoff begins

*Does not prove:* anything the base fuse probe does not prove, plus the handoff's own limit: one pinned partition started Windows once.

#### `diag-msi-sec2` — MSI B850 SEC2 ucode version

One read of the SEC2 version register at BAR0+0x824140 per card, named as the Windows 617.42 register by the Linux 610.43.03 header. It reports a version and refuses to infer a signature from it.

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diag-msi-sec2 -StageDrive G
```

*Log:* \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself

*Score:* `python3 tools/verify_run_log.py <log> --stage readonly`

* `C4` — `MSI_B850_SEC2_UCODE1 GPU=%s STATUS=OBSERVED BDF=0000:%02x:00.0 BAR0_OFFSET=0x824140 RAW=%#x LOW16=%#x FUSE_VERSION=%u ENGINE_ID=1 UCODE_ID=1 SELECTED_SIGNATURE=UNVERIFIED SECURE_ACCEPTANCE=UNVERIFIED READ_ONLY=1` (`src/gpu_topology_efi.c`): the SEC2 version per card, with the selected signature and the secure-acceptance result stated as unverified in the same line
* `C4` — `MSI_B850_SEC2_UCODE1_END READS_SUCCEEDED=%u EXPECTED=2 NO_GPU_WRITES=1 SECURE_ACCEPTANCE=UNVERIFIED` (`src/gpu_topology_efi.c`): the completion line says how many of the expected reads landed and that no GPU write happened: a failed read is visible as a count, not as a silent zero

*Does not prove:* that this version is the one Windows 617.42 uses. The register is named by a Linux header, the log prints `SELECTED_SIGNATURE=UNVERIFIED`, and the Windows signature slot stays `UNKNOWN`.

#### `diag-msi-sec2-handoff` — MSI B850 SEC2 ucode version with handoff

The SEC2 read plus the pinned Windows handoff, for a bench where the read-only image is expected to return to Windows by itself.

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diag-msi-sec2-handoff -WindowsEspGuid '<reviewed GUID>' -StageDrive G
```

*Log:* \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself

*Score:* `python3 tools/verify_run_log.py <log> --stage readonly`

*Also collects* everything `diag-msi-sec2` collects, plus:

* `A2` — `WINDOWS_HANDOFF=REQUESTED; pinned Windows ESP, no reset/BootOrder write` (`src/gpu_topology_efi.c`): the handoff is pinned to one ESP GUID and does not touch BootOrder
* `B9` — `SAVE_LOG_STATUS=%#llx ROTATING_FILE=GPU-TOPOLOGY-DIAGNOSTIC-NNNN.TXT` (`src/gpu_topology_efi.c`): the read is written before the handoff begins

*Does not prove:* anything the base SEC2 probe does not prove, plus the handoff's own limit: one pinned partition started Windows once.

#### `diag-vram-blocks` — read-only probe of the actuating write set

Forty-two BAR0 reads per verified card, at the addresses the waived transaction writes, plus the twenty-four words it reads without writing - the two HwCfg words `falcon_reset()` polls, the RISC-V halt word, the GFW and WPR2 bookkeeping words, and the rest of the set whose refused read stops the transaction. It is the interlock for the first actuation: run the same image before and after, and any block that moved shows up as a different digest. Nothing is written. It also copies the one-megabyte ROM window at BAR0 + 0x300000 and applies the same structural check the transaction applies before it refuses a board.

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diag-vram-blocks -StageDrive G
```

*Log:* \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself

*Score:* `python3 tools/verify_run_log.py <log> --stage readonly`

* `C5` — `VRAM_BLOCK_PROBE_BEGIN SOURCE=UNLOCK_610_WRITE_SET BLOCKS=%u CONTEXT=%u MODE=READ_ONLY READ_ONLY=1 NO_STOP=1 NO_DMA=1 NO_RESET=1 NO_UNLOCK=1 NO_FLR=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLASH=0 NO_GPU_WRITES=1` (`src/gpu_topology_efi.c`): the probe declares what it is before it reads anything: a read-only boundary and the size of its own address list, so a run that reads fewer blocks than the table has is visible in the first line rather than by counting the middle.
* `C5` — `VRAM_BLOCK_PROBE GPU=%s BDF=0000:%02x:00.0 BLOCK=%s BAR0_OFFSET=%#x ROLE=WRITE STATUS=OBSERVED RAW=%#010x ACCESS=%s READ_ONLY=1 ACCEPTANCE=UNVERIFIED` (`src/gpu_topology_efi.c`): one line per word the actuating sequence writes: block name, the absolute BAR0 address, and the value there before any write exists. Forty-two of them (twenty per engine plus both selectors) is the evidence that this image addresses the same blocks as the transaction. It does not move C5: a readable address identifies the target, and there is no post-state yet to verify.
* `C3` — `VRAM_BLOCK_PROBE_CARD GPU=%s READ=%u FAILED=%u OBSERVED=%u NOT_ACCESSIBLE=%u WRITE_SET_SHA256=%s DIGEST_CV=CHANGE_DETECTOR_ONLY ACCEPTANCE=UNVERIFIED` (`src/gpu_topology_efi.c`): the per-card digest is labelled a change detector in the line itself, and the line carries the failed-read count beside it: a digest over a partial read set would otherwise look complete. A digest over register values is not firmware acceptance, so C3 stays where it is.
* `C5` — `VRAM_BLOCK_PROBE_END CARDS=%u BLOCKS=%u CONTEXT=%u EXPECTED_READS=%u READS_SUCCEEDED=%u READS_FAILED=%u DIGESTS=%u NO_GPU_WRITES=1 SECURITY_ACCEPTANCE=UNVERIFIED OBSERVED=%u NOT_ACCESSIBLE=%u` (`src/gpu_topology_efi.c`): the totals: cards probed, expected reads, reads that landed and reads that did not. A block that does not read back is a number here instead of a silent gap, which is the whole point of probing before the first write.

*Does not prove:* that these are the right addresses for the transaction - only that this image and `src/vendor/unlock_610.c` name the same ones, checked by `tests/test_vram_block_probe.py`, not by the log. A successful read proves the word is reachable through PCI I/O; it does not prove an engine mask, a signature selector or a speed-selector write is permitted on this board, and it cannot say what any value means. The post-actuation half of the comparison has never run, because the actuation has never run.

#### `diag-vram-blocks-handoff` — read-only probe of the actuating write set, with handoff

The block probe plus the pinned Windows handoff, for a bench where the read-only image has to return to Windows by itself. The end-of-session digest is in the log either way.

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diag-vram-blocks-handoff -WindowsEspGuid '<reviewed GUID>' -StageDrive G
```

*Log:* \GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT at the root of the stick, written by the image itself

*Score:* `python3 tools/verify_run_log.py <log> --stage readonly`

*Also collects* everything `diag-vram-blocks` collects, plus:

* `A2` — `WINDOWS_HANDOFF=REQUESTED; pinned Windows ESP, no reset/BootOrder write` (`src/gpu_topology_efi.c`): the handoff is pinned to one ESP GUID and does not touch BootOrder
* `B9` — `SAVE_LOG_STATUS=%#llx ROTATING_FILE=GPU-TOPOLOGY-DIAGNOSTIC-NNNN.TXT` (`src/gpu_topology_efi.c`): the block lines and the digest are written before the handoff begins, so the evidence survives a handoff that does not

*Does not prove:* anything the plain block probe does not prove, plus the handoff's own limit: one pinned partition started Windows once.

#### `prep-selected` — preparation for explicit targets, one or both cards

The active application, built without the waiver: it runs the real inventory and selection, prepares its state, and refuses before `GuInit`. This is the only image here that executes the app's own entrypoint on hardware, which is why it is the one the P gates score. Given both BDFs it does the same thing for two cards in one boot: `GU_ACTIVE_SEQUENCE_PREPARE_ONLY` returns before the `MULTI_TARGET_WITHOUT_WAIVER` check, so preparing an ordered list needs no waiver - executing one does.

*one card* — `GPU-ACTIVE-PREP-0000-01-00-0.EFI`

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only prep-selected -SelectionBdf '0000:01:00.0' -StageDrive G
```

*both cards, in this order* — `GPU-ACTIVE-PREP-0000-01-00-0--0000-04-00-0.EFI`

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only prep-selected -SelectionBdfs '0000:01:00.0,0000:04:00.0' -StageDrive G
```

*Log:* \AD102-UNLOCK\RUNnnnn.TXT, one file per boot, never overwritten

*Score:* `python3 tools/verify_run_log.py <log> --stage prepare`

* `D5` — `ACTIVE_PREPARE=COMPLETE SELECTED=%zu NVIDIA_COUNT=%zu MUTATION_AUTHORIZED=0` (`src/gpu_discovery.c`): the app's inventory, selection and preparation completed on real firmware and the log says mutation was not authorised: the manual loop D5 says does not exist, run once, by hand
* `B9` — `LAUNCH_SOURCE DECISION=%s REASON=%s REMOVABLE=%u PARTITION=%u` (`src/boot_io.c`): the app's own launch decision, printed before anything else: the USB-launch requirement observed rather than inferred
* `A7` — `BOOT_STATE PRESENT=1 PHASE=%u BOOT_COUNT=%u STATUS=%#llx DEVICE=%04x` (`src/boot_io.c`): the persistent record is read at boot on real firmware, so a later boot's `PRESENT=1` is a record this tree wrote, not a fixture
* `A4` — `ACTIVE_PREPARE=SELECTED_UNSUPPORTED_CARD BDF=%04x:%02x:%02x.%u ID=%04x:%04x` (`src/gpu_discovery.c`): building for one BDF and booting with a different card produces this refusal: the negative case of the image-refuses-a-card-it-was-not-built-for rule
* `E8` — `ACTIVE_GPU[%zu] BDF=%04x:%02x:%02x.%u ID=%04x:%04x SUB=%04x:%04x CLASS=%04x LEGACY_UNLOCK_DEVICE_ID=%u` (`src/gpu_discovery.c`): the identity the app itself resolved, with the device id it would act on: the first row of a per-SKU entry
* `M4` — `ACTIVE_PREPARE=INVENTORIED NVIDIA_COUNT=%zu REQUESTED=%zu READ_ONLY=1` (`src/gpu_discovery.c`): how many NVIDIA cards the app saw, and how many of them the build asked for: the two numbers M4's record has to agree with before the ordered list is trusted
* `C9` — `ACTIVE_PREPARED_STEP[%zu] BDF=%04x:%02x:%02x.%u ID=%04x:%04x SUB=%04x:%04x LEGACY_UNLOCK_DEVICE_ID=%u READ_ONLY=1` (`src/gpu_discovery.c`): one line per selected card, in the order the selection names them: two of them is what a two-card session looks like, and their IDs are what ties each step to silicon
* `C9` — `ACTIVE_CONTEXT[%zu]=PREPARED CPU_ONLY=1 MUTATION_AUTHORIZED=0` (`src/gpu_discovery.c`): `PreparePlatformContexts` allocates one platform context per selected card and refuses an alias to the active context or a duplicate pointer, so two of these lines are evidence that two distinct per-card contexts existed at the same time in a read-only build
* `B10` — `ACTIVE_REFRESH=PASSIVE_REVALIDATED TARGETS=%zu MUTATION_AUTHORIZED=0` (`src/gpu_discovery.c`): the cards were re-read after preparation and the revalidation passed: the log shows the second observation, not only the first

*Does not prove:* that the ordered list may execute. Preparation and execution are different admissions: executing more than one target requires `-AcceptUnverifiedActuation` (`MULTI_TARGET_WITHOUT_WAIVER` otherwise), and nothing in a preparation log exercises a write, a Stop, a detach, a reset or a FLR. Two prepared contexts prove a list was built, not that a peer card is undisturbed during a mutation - there has never been a mutation.

#### `dxe-prep-selected` — DXE preparation for explicit targets

The same preparation as a boot-service driver, loaded from a `Driver####` entry instead of `EFI\BOOT\BOOTX64.EFI`. It answers one question the application cannot: whether the same preflight runs under a driver loaded at `ReadyToBoot`.

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only dxe-prep-selected -SelectionBdf '0000:01:00.0' -SignOnly
```

*Log:* \AD102-UNLOCK\RUNnnnn.TXT, one file per boot, never overwritten

*Score:* `python3 tools/verify_run_log.py <log> --stage prepare`

* `D5` — `ACTIVE_EXECUTION=REFUSED REASON=SELECTION_PREPARATION_ONLY BEFORE_GU_INIT=1` (`src/gpu_discovery.c`): both entrypoints call `GuDiscoverSingleGpu`, which refuses there; that shared boundary is what a resident-driver design has to be built on. Neither entrypoint's own banner can print in this build - `ACTIVE_APP=` in `src/app.c` and `ACTIVE_DXE=` in `src/dxe_main.c` both sit behind the `GuDiscoverSingleGpu` success check that refuses

*Does not prove:* that the driver is safe to leave installed, or that `ReadyToBoot` timing is compatible with anything: the catalogue refuses to stage a DXE driver as `BOOTX64.EFI`, so the only path is a `Driver####` entry you add and remove by hand, and that is a firmware change, not a read-only act.

### Candidates that produce no scoreable log, and why

* `diag-topology` — no `-SaveLog`: it prints to the console and writes nothing. A photograph of a screen is not a log, and `verify_run_log.py` scores files: use `diag-topology-log`, which is the same report with the save compiled in.
* `compile-one-target` — compile-only (`may_be_staged` is false): the image returns before any firmware or GPU operation, and CI already builds it. A boot would add nothing a compile does not already prove.
* `compile-sequence` — compile-only, as above: it exists to prove the ordered sources compile together.
* `dxe-compile-one-target` — compile-only DXE: nothing runs, so there is no log to score.
* `dxe-compile-sequence` — compile-only DXE, as above.
* `actuate-one-target` — writes the GPU: this is the `actuate` stage, needs `-AcceptActuatingDeploy`, and is outside a read-only session by definition.
* `actuate-sequence` — writes the GPU, ordered over several cards: `actuate` stage, and it needs the selection to be exactly what the catalogue says.
* `actuate-one-target-resident` — writes the GPU from the ESP: `actuate` stage, and it is the only image that could observe B9's ESP-resident half.
* `actuate-runtime-selection` — writes the GPU with the target chosen at boot: `actuate` stage.
* `actuate-runtime-selection-resident` — writes the GPU, resident, target at boot: `actuate` stage.
* `dxe-actuate-one-target` — writes the GPU from a driver: `actuate` stage.
* `dxe-actuate-sequence` — writes the GPU, ordered, from a driver: `actuate` stage.
* `dxe-actuate-runtime-selection` — writes the GPU from a driver with the target chosen at boot: `actuate` stage.
* `stop-4090-once` — calls the vendor `Stop` on the 4090: `stop-only`, writes the GPU, needs `-AcceptActuatingDeploy` and the recovery toolkit. It is the image C1 is about, and it is not read-only.
* `stop-4090-handoff` — vendor `Stop` then Windows: `stop-only`, writes the GPU.
* `stop-3080-once` — vendor `Stop` on the 3080: `stop-only`, writes the GPU.
* `stop-3080-handoff` — vendor `Stop` on the 3080 then Windows: `stop-only`, writes the GPU.
* `stop-dual-handoff` — vendor `Stop` on both cards then Windows: `stop-only`, writes the GPU, and it is the only image that would say anything about C9's interference on real silicon.

### What the read-only stage cannot advance

The ceiling of the loop above, item by item. This is the table to read before promising that a clean session means anything beyond what it says.

| Item | After every candidate above has been scored | Why it does not go further |
| --- | --- | --- |
| `B1` | observed on one board, one cabling and one BIOS revision | the policy still refuses every other configuration, and the firmware-policy question stays uninterpreted until someone maps the raw dump |
| `B2` | the GOP inventory and console ownership of this machine are recorded per card | a headless build's surviving non-target GOP is a different cabling; it needs its own capture |
| `B9` | a USB launch with a writable numbered log, observed | the ESP-resident half is printed only by an `-EspResident` actuation build, not by anything in this guide |
| `A1` | the selector resolved and refused real BDFs on real handles | two machines with different BDFs, which is A1's own exit criterion, is a second session |
| `A2` | one reviewed ESP GUID started Windows from the passive image | the app's own ESP identification and BootOrder cross-check is exercised by the preparation image only when it runs from the ESP |
| `A4` | the app refused a card it was not built for | the refusal is one observation; the profile-detection half still has no per-SKU table |
| `A7` | the boot record was read on real firmware | whether re-applying on the Nth boot is safe is exactly what the record is not allowed to decide |
| `C3` | nothing | firmware acceptance needs the vendor path; the read-only probes print `ACCEPTANCE=UNVERIFIED` by construction |
| `C4` | one card's fuse words and the reference the probe decoded them against | no signature chain ties Booter, V67 and ROM; the Windows 617.42 slot stays `UNKNOWN` |
| `C8` | nothing | `PCI_LINK=` is printed only by the waived admission path, after the preparation image has already returned |
| `C1` | nothing | the vendor `Stop` is the `stop-only` image; C2, C5, C6 and C7 go with it |
| `C9` | two cards enumerated, selected and reported in one snapshot | interference is about what happens when one of them is actuated; that is the waived stage, and B10's memory cost per card is unmeasured |
| `C10` | nothing | resume needs a suspend and a resume, and nothing of ours runs during either |
| `D5` | the loop above was executed by hand, once per candidate | no machine runs a stage automatically; a GO from `donor_gate.py` is still paperwork plus captured evidence |
| `E6` | the same BDF recorded across firmware updates | one capture is one firmware revision; stability is a series |
| `E7` | the reference each probe decoded against is in the log | that an asset bundle matches the card is a signature question, not an observation |
| `E8` | the identity rows for the cards actually on the bench | a row per SKU needs the geometry capture, and Turing still ships no payload |
| `E10` | nothing | Secure Boot, BitLocker and anti-cheat interaction is a Windows-side observation after an actuation, which is E1's campaign |

Every candidate above ends at `tools/verify_run_log.py`, and every verdict it prints carries `HARDWARE_VERIFIED=0`. When all of them have been scored, `tools/donor_gate.py --readonly ... --ack M1_...` says whether the session may enter preparation at all; it composes the same verdicts and adds the requirement that every stage saw the same single card. Neither tool, and nothing in this document, turns a captured log into a verified card.

## Captured runs, scored

Physical runs, stored as files and scored by the same tool an operator runs. Each entry pins the digest of the stored log, the facts quoted from it, and the verdict line the verifier prints for it, and `tests/test_hardware_session.py` re-scores the file on every run: the record cannot drift from the evidence, and a gate that no healthy machine can satisfy fails the suite instead of the operator. The read-only runs and the preparation runs are scored per stage; `tools/donor_gate.py` is what composes them into a go/no-go answer.

| Run | Image | Stage | Lines | SHA-256 of the stored log | Verdict |
| --- | --- | --- | --- | --- | --- |
| `physical-readonly-0000` | `diag-topology-log` | `readonly` | 151 | `fff77af4aa87291be8e6e715b69c8a621648c4acd1e099e5a431bc0658a40a03` | `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=3 GATES_FAILED=0` |
| `physical-readonly-0001` | `diag-topology-log` | `readonly` | 151 | `522dacb7a2638f3a537f723c1678bb0bb11da870b13353e4f11f99a27788a017` | `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=3 GATES_FAILED=0` |
| `physical-readonly-0002` | `diag-selection` | `readonly` | 161 | `e26a23c09dec972a30137e5195bace33d1c4f5221fc57778d0bafd7ef36ea3ed` | `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=3 GATES_FAILED=0` |
| `physical-readonly-0003` | `diag-msi-rom` | `readonly` | 224 | `139756cfa4a8944a9496af7e8a48f3d53b646861def382e3c362f3b5fda317e0` | `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=3 GATES_FAILED=0` |
| `physical-readonly-0004` | `diag-msi-fuse` | `readonly` | 159 | `a0611e2afbc9e42fe8798171d8a5454f10f9456b328b382b0fa35f3be9ad0c07` | `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=3 GATES_FAILED=0` |
| `physical-readonly-0005` | `diag-msi-sec2` | `readonly` | 155 | `eef276f7db4bf8c0a8a288f8cc256f47c5f8912886155bb32634392394e02a1f` | `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=3 GATES_FAILED=0` |
| `physical-prepare-0001` | `prep-selected -SelectionBdf '0000:01:00.0'` | `prepare` | 12 | `341c91aeec538bcf8e43f8194c164fadc041ed76a3255cfd3b7c38cef59f1e00` | `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=2 GATES_FAILED=0` |
| `physical-prepare-0002` | `prep-selected -SelectionBdf '0000:04:00.0'` | `prepare` | 12 | `f8f7208516cdd76b68ffb7153267874cd30df3368111ab527d925d2fe27cad15` | `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=2 GATES_FAILED=0` |
| `physical-readonly-0006` | `diag-vram-blocks` | `readonly` | 243 | `b101089391a189fc5882fcb79549950b14b9b111f5b9cacbeb14d6335667d50e` | `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=4 GATES_FAILED=0` |
| `physical-prepare-0003` | `prep-selected, both BDFs in one boot` | `prepare` | 14 | `cfe29f56032fdd826acba3cd38cb8bd16211f65f57ba72a943d8666f2584eed3` | `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=2 GATES_FAILED=0` |
| `physical-readonly-0007` | `diag-vram-blocks` | `readonly` | 289 | `0f3b6ac6fc5b948edbb4c4cc5f31177c375b8164ae0841a4c1d9d49ac3709251` | `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=4 GATES_FAILED=0` |
| `physical-readonly-0008` | `diag-vram-blocks` | `readonly` | 291 | `7ecffa0eaec2cf5b68e19ff2217f30e3964167048bb57a7e636a7ec9d81e88b1` | `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=5 GATES_FAILED=0` |
| `physical-readonly-0009` | `diag-vram-blocks` | `readonly` | 293 | `cd7920fcacf517280379f7ded17c87200278e4bc2eb847626bf4d6b2a8a780c5` | `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=6 GATES_FAILED=0` |
| `physical-readonly-0010` | `diag-vram-fuse` | `readonly` | 327 | `f0030317273565c8afa376751dd2821aaaa01828dde34777198452423ad5168f` | `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=7 GATES_FAILED=0` |
| `physical-prepare-0004` | `prep-selected (built with -CapturedPromReference)` | `prepare` | 15 | `a454aa546d8db3be4e3e8e9b99488bc4f6ff29388449f96ab3c08be4ef80b96c` | `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=2 GATES_FAILED=0` |

### `physical-readonly-0000`

*File:* `tests/data/hardware_session/physical-readonly-0000.txt` — 13140 bytes stored, 13291 as captured, 151 lines. CRLF as captured; stored with LF so a checkout cannot change the digest.

*Captured:* 2026-11-20. *Bench:* one display on the AMD iGPU, no cable on either NVIDIA card; Windows started after the image returned, with no error and no hang.

*Score:*

```bash
python3 tools/verify_run_log.py tests/data/hardware_session/physical-readonly-0000.txt --stage readonly
```

It prints `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=3 GATES_FAILED=0`.

*What this log puts on paper:*

* `GPU TOPOLOGY DIAGNOSTIC ONLY - no GPU hardware writes or unlock calls` — the image's own banner, so the file cannot be mistaken for an activating build's log
* `STATUS=PARTIAL TOPOLOGY=1 DISCOVERED_NVIDIA_GPUS=2 HARDWARE_TESTED=0 PHYSICAL_DISPLAY_OWNER=UNKNOWN` — the report's own summary: two NVIDIA cards found, `HARDWARE_TESTED=0` because nothing mutated a GPU, and no claim about which connector a human was looking at
* `TOPOLOGY_VERDICT=INCOMPLETE NVIDIA_CONOUT_BLOCKER=0` — B1/B2: the console is AMD-only and no NVIDIA path is configured, and the topology is still INCOMPLETE because neither NVIDIA card exposes a GOP - the exact configuration B2's policy has to decide about
* `ERROR_CONSOLE_ISOLATION=INDEPENDENT_OBSERVED` — B1: the error console resolves to the AMD GOP with nothing NVIDIA in it
* `GPU[0] BDF=0000:01:00.0 ID=10de:2684 SUB=1043:88e6` — E8/A5: the first card's BDF, device and subsystem id, as the image resolved them
* `GPU[1] BDF=0000:04:00.0 ID=10de:220a SUB=19da:b612` — E8/A5: the second card's identity, and that both cards were seen with `GOP_COUNT=0` on this cabling
* `NVIDIA_DRIVER_OWNER[1] MAPPED_TEXT_SHA256=77632902f0ceb0ba2848200f396ab61eacd7910e9cce7c535f65d6cdbe2b6c32` — the 4090's loaded driver text digest, identical to the previously pinned capture - the same driver image, not merely the same filename
* `NVIDIA_DRIVER_OWNER[5] MAPPED_TEXT_SHA256=615b37a708b1894e18d7fa60cfcdacaa4d8b598863a10b8103d231a595847b2b` — the 3080's loaded driver text digest, also identical to its pinned capture. `SIGNATURE_NOT_VERIFIED=1` rides on both lines: a digest agrees, it does not authenticate
* `LOG_FILE=\GPU-TOPOLOGY-DIAGNOSTIC-0000.TXT` — B9: a USB-launched image wrote a numbered log at the root of an empty medium - the writable-medium half of B9, observed
* `DIAGNOSTIC_LOG_COMPLETENESS=COMPLETE DROPPED=0 OVERFLOW=0` — the report finished with no withheld lines, so an absent line above is absent from the run, not from the buffer

*History:* First scored against gate R2 as it then stood: `FAIL FIRST_FAILED=R2_ENUMERATION_COMPLETED`, because R2 required `PCI_OBSERVATION_ONLY=1 ...`, a line the strict collector prints only after it has already failed. The gate was wrong, not the run (§11.29). R2 now requires the strict walk's scope line, the owner-inventory line that only its success branch prints, and the report's final line; the suite re-scores this file on every run, so a gate that no healthy machine can satisfy fails the tests instead of the operator.

*What it does not show:* that the card is safe, that anything was unlocked, or that Windows accepted the device: no Stop was called, every eligibility flag in the log is zero, both driver-image signatures are unverified, and `hardware_verified` stays false for every item in the inventory.

### `physical-readonly-0001`

*File:* `tests/data/hardware_session/physical-readonly-0001.txt` — 13140 bytes stored, 13291 as captured, 151 lines. CRLF as captured; stored with LF so a checkout cannot change the digest.

*Captured:* 2026-11-20. *Bench:* one display on the AMD iGPU, no cable on either NVIDIA card, driven from a USB stick; Windows started after each image returned, with no error and no hang.

*Score:*

```bash
python3 tools/verify_run_log.py tests/data/hardware_session/physical-readonly-0001.txt --stage readonly
```

It prints `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=3 GATES_FAILED=0`.

*What this log puts on paper:*

* `GPU[0] BDF=0000:01:00.0 ID=10de:2684 SUB=1043:88e6` — the same 4090, at the same address, in the same slot as `-0000`
* `GPU[1] BDF=0000:04:00.0 ID=10de:220a SUB=19da:b612` — the same 3080, at the same address: the topology did not change between the two boots
* `NVIDIA_DRIVER_OWNER[1] MAPPED_TEXT_SHA256=77632902f0ceb0ba2848200f396ab61eacd7910e9cce7c535f65d6cdbe2b6c32` — the 4090's loaded driver text digest, unchanged from `-0000`: the same driver image was loaded, not merely the same file name
* `NVIDIA_DRIVER_OWNER[1] MAPPED_IMAGE_SHA256=42adb7a86aa5241cdb418217b398e6de8f441347dba140ab32b08d56320d55e9 RELOCATED_POSSIBLE=1 SIGNATURE_NOT_VERIFIED=1` — the whole-image digest *did* change between the two boots (`a513a554...` in `-0000`), which is what `RELOCATED_POSSIBLE=1` warns about: the image is relocated at load, so only the text digest is a stable identity. Nothing in the repository may pin a driver by the whole-image digest
* `ERROR_CONSOLE_ISOLATION=INDEPENDENT_OBSERVED` — B1 reproduced on a second boot: the error console resolves to the AMD GOP with nothing NVIDIA in it
* `NVIDIA_GPU_COUNT=2 INDEPENDENT_GOP_COUNT=1 INDEPENDENT_CONOUT_MATCH=1 NVIDIA_CONOUT_MATCH=0 GOP_UNMATCHED=0 INDEPENDENT_CONSOLE_ASSOCIATION=1 CONOUT_WITHOUT_GOP=0 CONOUT_UNMATCHED=0 CONOUT_AMBIGUOUS=0` — two NVIDIA cards, one independent (AMD) GOP that does match the console, and no NVIDIA console match - the same B2 configuration as `-0000`, with the independent path positively matched
* `DIAGNOSTIC_LOG_COMPLETENESS=COMPLETE DROPPED=0 OVERFLOW=0` — the report finished with nothing withheld, so a line missing from this file is missing from the run
* `LOG_FILE=\GPU-TOPOLOGY-DIAGNOSTIC-0001.TXT` — the image numbered its own log `0001`: an existing log on the medium is never overwritten

*History:* Run to test whether `-0000` was reproducible or lucky. It is reproducible: all 151 lines carry the same topology, the same console isolation and the same driver text digests; only firmware pointer values and the relocated whole-image digests differ. That is the strongest statement these two captures support - two boots, one configuration - not a claim that the machine is stable under load or across a power cycle.

*What it does not show:* that two boots are a trend, that the configuration survives a firmware setting change, or that anything was unlocked: the driver images are still `SIGNATURE_NOT_VERIFIED=1` and every eligibility flag is zero.

### `physical-readonly-0002`

*File:* `tests/data/hardware_session/physical-readonly-0002.txt` — 13888 bytes stored, 14049 as captured, 161 lines. CRLF as captured; stored with LF so a checkout cannot change the digest.

*Captured:* 2026-11-20. *Bench:* one display on the AMD iGPU, no cable on either NVIDIA card, driven from a USB stick; Windows started after each image returned, with no error and no hang.

*Score:*

```bash
python3 tools/verify_run_log.py tests/data/hardware_session/physical-readonly-0002.txt --stage readonly
```

It prints `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=3 GATES_FAILED=0`.

*What this log puts on paper:*

* `SELECTION_SEQUENCE=BEGIN REQUESTED=2 READ_ONLY=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLR=0` — the ordered path really was asked for two targets, and it declared itself read-only before touching either
* `SELECTION_REQUEST[0] BDF=0000:01:00.0` — the 4090 was requested by the address the operator typed, not by index
* `SELECTION_REQUEST[1] BDF=0000:04:00.0` — the 3080 likewise: this is the first capture where two cards were named in one session
* `SELECTION_STEP[0]=PASSIVE_IDENTIFIED ID=10de:2684 SUB=1043:88e6 CLASS=0300 ROOT_INDEX=0 DEPTH=1 GOP_COUNT=0 BY_DRIVER=1 OPENERS_KNOWN=1` — the first target resolved passively to the expected device, behind a driver, with no GOP
* `SELECTION_STEP[0]=END STATUS=OBSERVED READ_ONLY=1` — the first step ended observed and read-only; no transaction was opened for it
* `SELECTION_STEP[1]=PASSIVE_IDENTIFIED ID=10de:220a SUB=19da:b612 CLASS=0300 ROOT_INDEX=2 DEPTH=3 GOP_COUNT=0 BY_DRIVER=1 OPENERS_KNOWN=1` — the second target resolved at a different root index and depth, so the two cards are not behind one bridge
* `SELECTION_STEP[1]=END STATUS=OBSERVED READ_ONLY=1` — the second step ended the same way: the ordered sequence can identify several cards without acting on any
* `SELECTION_SEQUENCE=PASSIVE_IDENTIFIED COMPLETED=2 NON_SELECTED_NVIDIA_COUNT=0 TOPOLOGY_VERDICT=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLR=0` — every requested step completed, no NVIDIA card on the machine escaped the request, and the sequence still claims no capability to detach, unlock or reset
* `GRAPH_ONLY_TOPOLOGY_VERDICT=INCOMPLETE ERROUT_INCLUDED_IN_FINAL=1` — the graph-only verdict is still INCOMPLETE, and the error console is included in it rather than dropped: the selection did not improve the topology verdict
* `DIAGNOSTIC_LOG_COMPLETENESS=COMPLETE DROPPED=0 OVERFLOW=0` — nothing was withheld, so `NON_SELECTED_NVIDIA_COUNT=0` is a count over a complete enumeration

*History:* The first capture of the ordered two-target selection, and the input that showed `tools/donor_gate.py` had to tell a census from a choice: this log names two cards on purpose, and the gate's old "one BDF per stage" rule called that a second donor. The gate now reads the `SELECTION_REQUEST`/`SELECTION_STEP` lines as the selection and the census as what it is.

*What it does not show:* that a selected card can be handed over or reset, or that the ordered sequence would behave the same with a card behind a different bridge: both targets here are root-complex children with `ROOTPORT=0`.

### `physical-readonly-0003`

*File:* `tests/data/hardware_session/physical-readonly-0003.txt` — 19754 bytes stored, 19978 as captured, 224 lines. CRLF as captured; stored with LF so a checkout cannot change the digest.

*Captured:* 2026-11-20. *Bench:* one display on the AMD iGPU, no cable on either NVIDIA card, driven from a USB stick; Windows started after each image returned, with no error and no hang.

*Score:*

```bash
python3 tools/verify_run_log.py tests/data/hardware_session/physical-readonly-0003.txt --stage readonly
```

It prints `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=3 GATES_FAILED=0`.

*What this log puts on paper:*

* `MSI_B850_ROM_VARIABLES MODE=READ_ONLY PLATFORM=MS-7E56 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLASH=0` — the board is the one this probe was written for, and the probe declares no capability to detach, unlock or flash
* `MSI_B850_ROM_VARIABLES PLATFORM_FINGERPRINT=MATCH SMBIOS_VERIFIED=0 BIOS_VERSION_VERIFIED=0 GPU_MUTATION=NONE` — the fingerprint matched while the SMBIOS and BIOS-version halves stayed unverified: a match on the product string is not a verified platform
* `MSI_B850_VARIABLE NAME=ROM_CMN EFI_STATUS=0 BYTES=327 ATTRS=0x7 CAP=1024 RAW_ONLY=1 READ_ONLY=1` — the first vendor variable the desk research named was present, 327 bytes, and was read raw without being interpreted
* `MSI_B850_VARIABLE NAME=CSM_CMN EFI_STATUS=0 BYTES=5 ATTRS=0x2 CAP=1024 RAW_ONLY=1 READ_ONLY=1` — the second named variable exists but is five bytes - too small to hold a policy table, which is a finding against the desk research rather than for it
* `MSI_B850_PRIVATE_BEGIN SCOPE=EXACT_TWO_NVIDIA READ_ONLY=1 NO_VENDOR_CALLBACK=1` — the private-interface probe refused to run outside a two-NVIDIA machine and used no vendor callback
* `MSI_B850_PRIVATE GPU=4090 BDF=0000:01:00.0 PRIVATE_STATUS=0 PRESENT=1 PCI_BY_DRIVER=1 OWNER_AGENT=0x76b74698 ROM_IMAGE_PRESENT=1 ROM_BYTES=150016 CALLS=ZERO` — the 4090's ROM image is present in memory at 150016 bytes, its PCI handle is held by a driver, and the probe made zero calls
* `MSI_B850_PRIVATE_END NO_PRIVATE_METHOD=1 NO_ROM_DEREFERENCE=1 NO_RELEASE=1 POOL_UNCERTAIN=0` — the probe found no private method to call and did not dereference the ROM image or release anybody's handle
* `MSI_B850_IMAGE_PARENT GPU=4090 PARENT_TEXT_SHA256=02a92fd4f6dcef415608649beb58a851a536cc1426e305d34a3361ee76a80ea5` — both NVIDIA drivers were loaded from the same parent image text; the 3080 line carries the identical digest
* `MSI_B850_BDS_POLICY STATE=SKIPPED ORIGINAL_BDS_MISMATCH=1 MATCHES=0` — the BDS policy probe located the structure but refused to use it because it did not match the expected original: zero matches, so it skipped rather than guessed
* `MSI_B850_ROM_VARIABLES STATUS=OBSERVED_OR_REFUSED ORIGINAL_ROM_DISPATCH_UNVERIFIED=1` — the report's own summary of the whole probe: observed where it could, refused where it could not, and the original ROM dispatch path remains unverified
* `MSI_B850_POLICY_PTR STATE=SKIPPED PCIBUS_IMAGE_LAYOUT_UNVERIFIED=1` — the policy-pointer walk was skipped for the same reason: the image layout it would walk is unverified
* `DIAGNOSTIC_LOG_COMPLETENESS=COMPLETE DROPPED=0 OVERFLOW=0` — nothing was withheld, so `MATCHES=0` and `STATE=SKIPPED` are the probe's findings and not a truncated log

*History:* First physical run of the MSI ROM probe. The desk research that motivated it assumed a ROM_CMN policy table and a private interface worth calling; the machine says ROM_CMN is real (327 bytes), CSM_CMN is five bytes, the BDS policy structure does not match the expected original, and no private method was found. `tests/test_hardware_session.py` re-scores this file on every run.

*What it does not show:* that the ROM variables can be written back, that any of them influences which GPU gets posted, or that the board lacks a vendor path this probe did not look for. It also cannot say the probe is harmless to run twice: it made no calls, but nothing here was measured twice.

### `physical-readonly-0004`

*File:* `tests/data/hardware_session/physical-readonly-0004.txt` — 14406 bytes stored, 14565 as captured, 159 lines. CRLF as captured; stored with LF so a checkout cannot change the digest.

*Captured:* 2026-11-20. *Bench:* one display on the AMD iGPU, no cable on either NVIDIA card, driven from a USB stick; Windows started after each image returned, with no error and no hang.

*Score:*

```bash
python3 tools/verify_run_log.py tests/data/hardware_session/physical-readonly-0004.txt --stage readonly
```

It prints `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=3 GATES_FAILED=0`.

*What this log puts on paper:*

* `MSI_B850_FWSEC_FUSE_BEGIN SOURCE=NOVA_CORE_GA102_PLUS READ_ONLY=1 CAN_DETACH=0 CAN_UNLOCK=0 NO_STOP=1 NO_DMA=1 NO_RESET=1` — the fuse probe declares four separate refusals - no Stop, no DMA, no reset - before the first read
* `MSI_B850_FWSEC_FUSE GPU=4090 STATUS=OBSERVED BAR0_OFFSET=0x8241e0 RAW=0x1 LOW16=0x1 FUSE_VERSION=1 ARCHIVED_ROM_SIG_MASK=0x3 CANDIDATE_SLOT=1 ARCHIVED_SLOT_NONZERO=1 ACCEPTANCE=UNVERIFIED` — the 4090's fuse word decoded as version 1 with candidate slot 1 non-zero, and the probe marked its own reading unverified
* `MSI_B850_FWSEC_FUSE GPU=3080 STATUS=OBSERVED BAR0_OFFSET=0x8241e0 RAW=0x3 LOW16=0x3 FUSE_VERSION=2 ARCHIVED_ROM_SIG_MASK=0x7 CANDIDATE_SLOT=2 ARCHIVED_SLOT_NONZERO=1 ACCEPTANCE=UNVERIFIED` — the 3080 differs: raw 0x3, version 2, slot 2. The two cards do not share a fuse state, so a rule calibrated on one cannot be asserted for the other
* `MSI_B850_SEC2_FUSE GPU=4090 STATUS=OBSERVED BAR0_OFFSET=0x824148 RAW=0x1 LOW16=0x1 FUSE_VERSION=1 REFERENCE_ENGINE=SEC2 REFERENCE_UCODE_ID=3 WINDOWS_61742_SIGNATURE_SLOT=UNKNOWN ACCEPTANCE=UNVERIFIED` — the SEC2 fuse reads the same version 1, and the Windows 61742 descriptor that would corroborate it is still UNKNOWN
* `MSI_B850_FWSEC_FUSE_END READS_SUCCEEDED=2 SECURITY_ACCEPTANCE=UNVERIFIED NO_GPU_WRITES=1` — two reads succeeded, no write was issued, and the acceptance stays unverified
* `MSI_B850_SEC2_FUSE_END READS_SUCCEEDED=2 WINDOWS_61742_DESCRIPTOR=UNKNOWN SECURITY_ACCEPTANCE=UNVERIFIED NO_GPU_WRITES=1` — the same statement for the SEC2 engine, with the missing reference named in the line itself
* `DIAGNOSTIC_LOG_COMPLETENESS=COMPLETE DROPPED=0 OVERFLOW=0` — nothing was withheld, so both cards' fuse lines are the whole set the probe printed

*History:* First physical run of the fuse probe. It establishes that the fuse words at these two BAR0 offsets are readable on a machine with the driver still resident, that the two cards disagree, and that no write was made. It does not establish what the values mean: every line carries `ACCEPTANCE=UNVERIFIED` because the archive the probe would compare against (`WINDOWS_61742_DESCRIPTOR`) is still unknown.

*What it does not show:* that a fuse reading predicts whether a card can be unlocked - that mapping is exactly what is missing - nor that the read is side-effect free at the firmware level; it shows no write was *issued*, which is what the image controls.

### `physical-readonly-0005`

*File:* `tests/data/hardware_session/physical-readonly-0005.txt` — 13817 bytes stored, 13972 as captured, 155 lines. CRLF as captured; stored with LF so a checkout cannot change the digest.

*Captured:* 2026-11-20. *Bench:* one display on the AMD iGPU, no cable on either NVIDIA card, driven from a USB stick; Windows started after each image returned, with no error and no hang.

*Score:*

```bash
python3 tools/verify_run_log.py tests/data/hardware_session/physical-readonly-0005.txt --stage readonly
```

It prints `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=3 GATES_FAILED=0`.

*What this log puts on paper:*

* `MSI_B850_SEC2_UCODE1_BEGIN SOURCE=WINDOWS_61742_SEC2_VERSION_REGISTER UCODE_ID=1 BAR0_OFFSET=0x824140 READ_ONLY=1 NO_STOP=1 NO_DMA=1 NO_RESET=1 NO_UNLOCK=1` — the probe names the reference it came from, its single read offset, and five refusals before reading anything
* `MSI_B850_SEC2_UCODE1 GPU=4090 STATUS=OBSERVED BDF=0000:01:00.0 BAR0_OFFSET=0x824140 RAW=0x1 LOW16=0x1 FUSE_VERSION=1 ENGINE_ID=1 UCODE_ID=1 SELECTED_SIGNATURE=UNVERIFIED SECURE_ACCEPTANCE=UNVERIFIED READ_ONLY=1` — the 4090's version register reads 1, and the line attributes the reading to the card by BDF rather than by index
* `MSI_B850_SEC2_UCODE1 GPU=3080 STATUS=OBSERVED BDF=0000:04:00.0 BAR0_OFFSET=0x824140 RAW=0x3 LOW16=0x3 FUSE_VERSION=2 ENGINE_ID=1 UCODE_ID=1 SELECTED_SIGNATURE=UNVERIFIED SECURE_ACCEPTANCE=UNVERIFIED READ_ONLY=1` — the 3080 reads 3: the same disagreement `-0004` saw at the neighbouring offset, now reproduced at this one on the same boot order
* `MSI_B850_SEC2_UCODE1_END READS_SUCCEEDED=2 EXPECTED=2 NO_GPU_WRITES=1 SECURE_ACCEPTANCE=UNVERIFIED` — both expected reads succeeded, no write was issued, and the security acceptance is still explicitly unverified
* `DIAGNOSTIC_LOG_COMPLETENESS=COMPLETE DROPPED=0 OVERFLOW=0` — nothing was withheld, so `READS_SUCCEEDED=2 EXPECTED=2` is not an artefact of a full buffer

*History:* First physical run of the SEC2 version-register probe. Its value is that it is the same reading as `-0004`'s SEC2 fuse taken through a different named register: the two agree per card (1 for the 4090, 3 for the 3080), which is the first cross-check of the fuse work that did not come from the same source document.

*What it does not show:* which signature the engine would accept, or that the register's low bits mean what the desk research says. `SELECTED_SIGNATURE=UNVERIFIED` on both lines is the honest state of that question.

### `physical-prepare-0001`

*File:* `tests/data/hardware_session/physical-prepare-0001.txt` — 881 bytes stored, 881 as captured, 12 lines. LF as captured (written by the image, never re-encoded); the digest is the captured bytes.

*Captured:* 2026-11-20. *Bench:* one display on the AMD iGPU, no cable on either NVIDIA card, driven from a USB stick; Windows started after each image returned, with no error and no hang.

*Score:*

```bash
python3 tools/verify_run_log.py tests/data/hardware_session/physical-prepare-0001.txt --stage prepare
```

It prints `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=2 GATES_FAILED=0`.

*What this log puts on paper:*

* `ASSET_INSTALL PRESENT=0` — no bundle was ever staged on this stick, and the log says so before discovery assumes anything about assets
* `BOOT_STATE PRESENT=0` — no earlier record was found: P2's honest first-boot state, not a missing feature
* `ACTIVE_PREPARE=INVENTORIED NVIDIA_COUNT=2 REQUESTED=1 READ_ONLY=1` — the preparation image saw both cards, was asked for one, and declared itself read-only before preparing anything
* `ACTIVE_GPU[0] BDF=0000:01:00.0 ID=10de:2684 SUB=1043:88e6 CLASS=0300 LEGACY_UNLOCK_DEVICE_ID=1` — the 4090's identity as the preparation path resolved it, including that it is a device the legacy unlock table would have matched
* `ACTIVE_GPU[1] BDF=0000:04:00.0 ID=10de:220a SUB=19da:b612 CLASS=0300 LEGACY_UNLOCK_DEVICE_ID=1` — the 3080 is named too: the image enumerated the whole machine, so the one-card boundary is a rule it applied, not a fact it assumed
* `ACTIVE_PREPARED_STEP[0] BDF=0000:01:00.0 ID=10de:2684 SUB=1043:88e6 LEGACY_UNLOCK_DEVICE_ID=1 READ_ONLY=1` — exactly one step was prepared, for the card that was asked for, and the step itself is marked read-only
* `ACTIVE_PREPARE=COMPLETE SELECTED=1 NVIDIA_COUNT=2 MUTATION_AUTHORIZED=0` — preparation reached its end with mutation explicitly unauthorized
* `ACTIVE_SEQUENCE=PREPARED_NOT_STARTED SELECTED=1 TRACKED=2 NEXT=0 MUTATION_AUTHORIZED=0` — the sequence is prepared and not started, and the authorization flag is still zero at the last place it is checked
* `ACTIVE_EXECUTION=REFUSED REASON=SELECTION_PREPARATION_ONLY BEFORE_GU_INIT=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLR=0` — the entrypoint refused execution before any GU init, naming the reason: this build exists to prepare and to stop
* `STOP: stage=discover status=0x8000000000000003 mutated=0 retained=0 fault=0` — the app left through its failure path with `mutated=0 retained=0 fault=0` - the halt marker after a refusal, not a Stop contract

*History:* The first physical preparation run, and the run that found the second unreachable-token gate: P1 required `ACTIVE_APP=PREPARATION_ONLY`, which `src/app.c` prints only after `GuDiscoverSingleGpu` returns success, the very call that refuses here. The gate was wrong, not the image (§11.30); P1 now requires the refusal line discovery prints. With the identity rule in `tools/donor_gate.py` reading the selected card rather than the census, `-0001` and this file compose into the first `DONOR_GATE VERDICT=GO` this workbench has produced from real logs.

*What it does not show:* that the card can be actuated, or that preparation's conclusions are right: it shows the entrypoint refused, the preflight completed, and nothing was mutated. No Stop, no detach, no FLR, no firmware write appears in these 12 lines.

### `physical-prepare-0002`

*File:* `tests/data/hardware_session/physical-prepare-0002.txt` — 881 bytes stored, 881 as captured, 12 lines. LF as captured (written by the image, never re-encoded); the digest is the captured bytes.

*Captured:* 2026-11-20. *Bench:* one display on the AMD iGPU, no cable on either NVIDIA card, driven from a USB stick; Windows started after each image returned, with no error and no hang.

*Score:*

```bash
python3 tools/verify_run_log.py tests/data/hardware_session/physical-prepare-0002.txt --stage prepare
```

It prints `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=2 GATES_FAILED=0`.

*What this log puts on paper:*

* `ASSET_INSTALL PRESENT=0` — the same medium state as the previous boot: nothing staged, nothing assumed
* `BOOT_STATE PRESENT=0` — still no record, which is consistent with a preparation image that writes none
* `ACTIVE_PREPARE=INVENTORIED NVIDIA_COUNT=2 REQUESTED=1 READ_ONLY=1` — the same inventory count and the same one-request read-only declaration as the 4090 run
* `ACTIVE_GPU[0] BDF=0000:01:00.0 ID=10de:2684 SUB=1043:88e6 CLASS=0300 LEGACY_UNLOCK_DEVICE_ID=1` — the 4090 is still first in enumeration even though it is not the target this time
* `ACTIVE_GPU[1] BDF=0000:04:00.0 ID=10de:220a SUB=19da:b612 CLASS=0300 LEGACY_UNLOCK_DEVICE_ID=1` — the 3080's identity, unchanged from the previous boot and from the read-only stage
* `ACTIVE_PREPARED_STEP[0] BDF=0000:04:00.0 ID=10de:220a SUB=19da:b612 LEGACY_UNLOCK_DEVICE_ID=1 READ_ONLY=1` — the only line that differs from `physical-prepare-0001`: the prepared step is the 3080, so the target really is chosen by the operator's BDF and not by enumeration order
* `ACTIVE_PREPARE=COMPLETE SELECTED=1 NVIDIA_COUNT=2 MUTATION_AUTHORIZED=0` — preparation completed for the second card with mutation unauthorized
* `ACTIVE_SEQUENCE=PREPARED_NOT_STARTED SELECTED=1 TRACKED=2 NEXT=0 MUTATION_AUTHORIZED=0` — prepared, not started, nothing authorized
* `ACTIVE_EXECUTION=REFUSED REASON=SELECTION_PREPARATION_ONLY BEFORE_GU_INIT=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLR=0` — the same refusal, on the other card: the boundary is a property of the image, not of the target
* `STOP: stage=discover status=0x8000000000000003 mutated=0 retained=0 fault=0` — the same clean exit with nothing mutated

*History:* Prepared the second card to test that the selection is honoured per target rather than by index. Diffing the two preparation logs shows exactly one changed line - the prepared step - which is what makes the pair useful: the preparation path behaves identically for a 4090 and a 3080 up to the refusal. This pair is also what showed that a gate keyed on a stage's *census* cannot describe a one-target session.

*What it does not show:* that the 3080's preparation conclusions hold for the 4090 or vice versa: each log speaks only for its own target, and neither says anything about actuation.

### `physical-readonly-0006`

*File:* `tests/data/hardware_session/physical-readonly-0006.txt` — 27929 bytes stored, 28172 as captured, 243 lines. CRLF as captured; stored with LF so a checkout cannot change the digest.

*Captured:* 2026-11-18. *Bench:* one display on the AMD iGPU, no cable on either NVIDIA card; the NVIDIA UEFI driver bound both cards (BY_DRIVER=1) in this boot; Windows started after the image returned.

*Score:*

```bash
python3 tools/verify_run_log.py tests/data/hardware_session/physical-readonly-0006.txt --stage readonly
```

It prints `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=4 GATES_FAILED=0`.

*What this log puts on paper:*

* `VRAM_BLOCK_PROBE_BEGIN SOURCE=UNLOCK_610_WRITE_SET BLOCKS=42 CONTEXT=2 MODE=READ_ONLY READ_ONLY=1 NO_STOP=1 NO_DMA=1 NO_RESET=1 NO_UNLOCK=1 NO_FLR=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLASH=0 NO_GPU_WRITES=1` — the first image in this tree that reads the transaction's own address list, and it says how many words that is before it reads any
* `VRAM_BLOCK_PROBE_END CARDS=2 BLOCKS=42 CONTEXT=2 EXPECTED_READS=88 READS_SUCCEEDED=88 READS_FAILED=0 DIGESTS=2 NO_GPU_WRITES=1 SECURITY_ACCEPTANCE=UNVERIFIED` — 88 of 88 reads landed on two cards: the whole write set is addressable through PCI I/O on this machine
* `VRAM_BLOCK_PROBE GPU=3080 BDF=0000:04:00.0 BLOCK=GSP.UCODE_ID BAR0_OFFSET=0x111198 ROLE=WRITE STATUS=OBSERVED RAW=0x00000009` — GSP ucode ID 9 on the 3080, the value Linux nova-core GA102+ uses to select BAR0+0x8241e0: this is a real engine register, not a decode artifact
* `VRAM_BLOCK_PROBE GPU=4090 BDF=0000:01:00.0 BLOCK=SEC2.CPUCTL BAR0_OFFSET=0x840100 ROLE=WRITE STATUS=OBSERVED RAW=0xbadf5620` — the other half of the same run: a word in the write set that reads as the transaction's own inaccessible() shape, which is why section 11.33 exists

*History:* First run of the block probe, and the first read-only image here that reads the transaction's own address list. It answers the question it was built for: 88 of 88 reads landed on both cards, so the forty-two words `src/vendor/unlock_610.c` writes are all addressable through PCI I/O on this machine. It also found something the plan did not have. On both cards the five falcon-bridge words read as state, while the rest of the SEC2 window - and, on the 4090, nearly all of the GSP window as well - returns `0xbadf5620`, which is the shape `inaccessible()` treats as a refused read: 45 of the 84 write-set words across the two cards. The probe as built printed those as `STATUS=OBSERVED`, so section 11.33 teaches it to classify them, adds the two `HwCfg` words `falcon_reset()` polls before its first write, and records the finding here.

*What it does not show:* that the actuating image will see the same values. It reads through the same `Mem.Read` call with the same width and bar index (`src/port.c`'s `GuRead32For`), so the values should reproduce, but that image has never run. It does not say why the engine refuses those words, or how a run would lift the refusal; and `0x8400f4`, the word `falcon_reset()` polls first, was not in the probed set, so whether the transaction stops at that poll or gets as far as its first write is still open.

### `physical-prepare-0003`

*File:* `tests/data/hardware_session/physical-prepare-0003.txt` — 1047 bytes stored, 1047 as captured, 14 lines. LF as captured; stored unchanged.

*Captured:* 2026-11-18. *Bench:* same boot and same two cards as physical-readonly-0006; built with -SelectionBdfs '0000:01:00.0,0000:04:00.0'.

*Score:*

```bash
python3 tools/verify_run_log.py tests/data/hardware_session/physical-prepare-0003.txt --stage prepare
```

It prints `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=2 GATES_FAILED=0`.

*What this log puts on paper:*

* `ACTIVE_PREPARE=INVENTORIED NVIDIA_COUNT=2 REQUESTED=2 READ_ONLY=1` — the inventory saw both cards and the build asked for both: the first time that number is 2 rather than 1
* `ACTIVE_PREPARE=COMPLETE SELECTED=2 NVIDIA_COUNT=2 MUTATION_AUTHORIZED=0` — preparation completed for two targets and states that no mutation was authorised
* `ACTIVE_CONTEXT[1]=PREPARED CPU_ONLY=1 MUTATION_AUTHORIZED=0` — a second per-card platform context existed at the same time as the first - what PreparePlatformContexts' alias refusal makes meaningful
* `ACTIVE_SEQUENCE=PREPARED_NOT_STARTED SELECTED=2 TRACKED=2 NEXT=0 MUTATION_AUTHORIZED=0` — the ordered two-target sequence was built and left unstarted: the list the waiver would later execute
* `ACTIVE_EXECUTION=REFUSED REASON=SELECTION_PREPARATION_ONLY BEFORE_GU_INIT=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLR=0` — execution refused at the shared discovery boundary, before GuInit, with detach/unlock/FLR all zero

*History:* The two-card preparation section 11.31 listed as next and unmeasured: one boot, `-SelectionBdfs '0000:01:00.0,0000:04:00.0'`, built without the waiver. It prepares both cards in the order the selection names them, holds two per-card contexts at the same time, and refuses execution before `GuInit`.

*What it does not show:* that the ordered list may execute - execution needs `-AcceptUnverifiedActuation`, which this build does not have - or anything about a second card while the first is being written. No write, Stop, detach, reset or FLR was called, and `MUTATION_AUTHORIZED=0` is printed on every line that could be mistaken for one.

### `physical-readonly-0007`

*File:* `tests/data/hardware_session/physical-readonly-0007.txt` — 37731 bytes stored, 38020 as captured, 289 lines. CRLF as captured; stored with LF so a checkout cannot change the digest.

*Captured:* 2026-11-18. *Bench:* same bench and cabling as -0006; this boot ran the read-set build (CONTEXT=24), so it is the first run that reads every word the transaction reads as well as every word it writes.

*Score:*

```bash
python3 tools/verify_run_log.py tests/data/hardware_session/physical-readonly-0007.txt --stage readonly
```

It prints `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=4 GATES_FAILED=0`.

*What this log puts on paper:*

* `VRAM_BLOCK_PROBE_BEGIN SOURCE=UNLOCK_610_WRITE_SET BLOCKS=42 CONTEXT=24 MODE=READ_ONLY READ_ONLY=1 NO_STOP=1 NO_DMA=1 NO_RESET=1 NO_UNLOCK=1 NO_FLR=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLASH=0 NO_GPU_WRITES=1` — the read-set build ran: the probe states its own list size (42 written, 24 read) before it reads any of them
* `VRAM_BLOCK_PROBE_END CARDS=2 BLOCKS=42 CONTEXT=24 EXPECTED_READS=132 READS_SUCCEEDED=132 READS_FAILED=0 DIGESTS=2 NO_GPU_WRITES=1 SECURITY_ACCEPTANCE=UNVERIFIED OBSERVED=84 NOT_ACCESSIBLE=48` — 132 of 132 reads landed on two cards, 48 of them returning the refused shape: no address in either list is unreachable, so every remaining question is about values
* `VRAM_BLOCK_PROBE GPU=4090 BDF=0000:01:00.0 BLOCK=SEC2.HWCFG BAR0_OFFSET=0x8400f4 ROLE=READ_CONTEXT STATUS=OBSERVED RAW=0x000067b7 ACCESS=STATE` — the word falcon_reset() polls before its first write reads as state on the 4090, which is the question section 11.33 left open: bit 31 clear (the pre-reset wait is a tolerated timeout) and bit 12 clear (the post-reset release wait is satisfied by the value that was already there)
* `VRAM_BLOCK_PROBE GPU=3080 BDF=0000:04:00.0 BLOCK=GSP.HWCFG BAR0_OFFSET=0x1100f4 ROLE=READ_CONTEXT STATUS=OBSERVED RAW=0x000047f7 ACCESS=STATE` — the same word on the other card, also state: the two GPUs differ in one bit here, so the first poll is not what separates them
* `VRAM_BLOCK_PROBE GPU=4090 BDF=0000:01:00.0 BLOCK=GSP.BCR BAR0_OFFSET=0x111668 ROLE=WRITE STATUS=OBSERVED RAW=0xbadf5620 ACCESS=NOT_ACCESSIBLE` — falcon_reset() reads this word whenever HwCfg bit 10 is set, which it is on both cards: on the 4090 the GSP engine's BCR is refused, so the first falcon reset in the sequence stops here after two writes
* `VRAM_BLOCK_PROBE GPU=3080 BDF=0000:04:00.0 BLOCK=SEC2.FBIF_TRANS_24 BAR0_OFFSET=0x840624 ROLE=WRITE STATUS=OBSERVED RAW=0xbadf5620 ACCESS=NOT_ACCESSIBLE` — falcon_hs() reads this word before it programs any transfer, and the 3080's SEC2 window refuses it: the card whose GSP block is fully readable is the one that would get furthest before stopping
* `VRAM_BLOCK_PROBE GPU=4090 BDF=0000:01:00.0 BLOCK=GSP.RISCV BAR0_OFFSET=0x111388 ROLE=READ_CONTEXT STATUS=OBSERVED RAW=0xbadf5620 ACCESS=NOT_ACCESSIBLE` — the RISC-V halt word is refused on the 4090's GSP block; the transaction does not consult it for the GSP engine, so this is an observation about the block rather than a stop
* `VRAM_BLOCK_PROBE GPU=3080 BDF=0000:04:00.0 BLOCK=FB.WPR2_HI BAR0_OFFSET=0x1fa828 ROLE=READ_CONTEXT STATUS=OBSERVED RAW=0000000000 ACCESS=STATE` — WPR2 high reads zero on both cards, which is the value the transaction's entry gate requires: a non-zero HI.VAL means a driver already used the card this power cycle

*History:* The read-set build of the probe. -0006 answered whether the words the transaction writes are addressable; this run answers whether the words it reads are readable, and with it the question -0006 left open: falcon_reset()'s first poll, 0x8400f4, reads 0x67b7 on the 4090 and 0x47f7 on the 3080, both as state. With bit 31 clear the pre-reset wait is the tolerated timeout the code's own comment describes, and with bit 12 clear the post-reset release wait is satisfied by the value that was already in the register, so it does not demonstrate that a reset happened. Two of the twenty-four new words are refused: SEC2.IRQSTAT on both cards, which only log_falcon_state() reads, and the 4090's GSP.RISCV halt word. Everything the layout and entry gates demand by exact value is present on both cards - PLM 0xffffff8f, GFW.PLM 0x8b8f, GFW.BOOT 0x3ff, GSP.CPUCTL 0x10, WPR2 high 0 - and the sixty-six write-set words are unchanged from -0006, digest included.

*What it does not show:* that either card can be actuated. The probe reads and never resets, so the two refused words have no cause attached, and whether a refused word is refused again after an engine reset is exactly what only an actuating run can say. Nothing here touches the ROM window at BAR0+0x300000, which is the first content gate the transaction passes through, nor the profile and entry-selector bookkeeping the entry gate also requires. `ACCEPTANCE=UNVERIFIED` stands: a word that reads as state is not a signature check.

### `physical-readonly-0008`

*File:* `tests/data/hardware_session/physical-readonly-0008.txt` — 38282 bytes stored, 38573 as captured, 291 lines. CRLF as captured; stored with LF so a checkout cannot change the digest.

*Captured:* 2026-11-18. *Bench:* same bench and cabling as -0006 and -0007; first boot of the ROM-window build, so it is the only run that reads BAR0 + 0x300000.

*Score:*

```bash
python3 tools/verify_run_log.py tests/data/hardware_session/physical-readonly-0008.txt --stage readonly
```

It prints `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=5 GATES_FAILED=0`.

*What this log puts on paper:*

* `VRAM_BLOCK_PROBE_ROM GPU=4090 BAR0_OFFSET=0x300000 BYTES=1048576 SIGNATURE=0xaa55 PCIR_OFFSET=0x0170 PCIR_VENDOR=0x10de PCIR_DEVICE=0x2684 LIVE_DEVICE=0x10de MATCH=0` — the megabyte at BAR0 + 0x300000 read on the 4090, and its own PCIR structure names 10de:2684 - the card the GPU line in the same log reports. MATCH=0 is the probe's error, not the ROM's: this build was handed the vendor half of pci_id, so LIVE_DEVICE printed 0x10de
* `VRAM_BLOCK_PROBE_ROM GPU=3080 BAR0_OFFSET=0x300000 BYTES=1048576 SIGNATURE=0xaa55 PCIR_OFFSET=0x0170 PCIR_VENDOR=0x10de PCIR_DEVICE=0x220a LIVE_DEVICE=0x10de MATCH=0` — the same read on the 3080, naming 10de:220a. Its SHA256 is the digest of that card's ROM window, which is the value a PROM reference would be captured from
* `VRAM_BLOCK_PROBE_END CARDS=2 BLOCKS=42 CONTEXT=24 EXPECTED_READS=132 READS_SUCCEEDED=132 READS_FAILED=0 DIGESTS=2 NO_GPU_WRITES=1 SECURITY_ACCEPTANCE=UNVERIFIED OBSERVED=84 NOT_ACCESSIBLE=48 ROMS=2` — both ROM windows were copied, and the word tables are unchanged from -0007 to the value: the run added a reading, not a reinterpretation

*History:* The first boot of the ROM-window build, and the run that caught this probe's own defect. Both cards returned a ROM header (0xaa55) and a PCIR structure at 0x0170 carrying NVIDIA's vendor id and a device id naming the card - 10de:2684 on the 4090, 10de:220a on the 3080, both matching the GPU lines in the same log - so the structural check the transaction applies is satisfied on this bench. The probe still reported MATCH=0 because it passed pci_id & 0xffff, the vendor, as the live device id. The ROM was never the problem; the comparison was handed the wrong half of the identity. Fixed to pci_id>>16, with a test that pins the split against the PCI printer's own %04x:%04x encoding.

*What it does not show:* that a fixed build reads MATCH=1 on these cards - that needs the re-run. Nor does it show that the ROM carries the FWSEC-FRTS descriptor preparation parses from the same megabyte, or that GuAd104Prepare accepts the board on its other terms: assets, selector stability, and the reference policy that is compiled out. MATCH says the structure names the device and nothing more; no signature is authenticated here.

### `physical-readonly-0009`

*File:* `tests/data/hardware_session/physical-readonly-0009.txt` — 38696 bytes stored, 38989 as captured, 293 lines. CRLF as captured; stored with LF so a checkout cannot change the digest.

*Captured:* 2026-11-18. *Bench:* same bench and cabling as -0006 to -0008; first boot of the capture build, which writes the megabyte it read instead of only hashing it.

*Score:*

```bash
python3 tools/verify_run_log.py tests/data/hardware_session/physical-readonly-0009.txt --stage readonly
```

It prints `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=6 GATES_FAILED=0`.

*What this log puts on paper:*

* `VRAM_BLOCK_PROBE_ROM GPU=4090 BAR0_OFFSET=0x300000 BYTES=1048576 SIGNATURE=0xaa55 PCIR_OFFSET=0x0170 PCIR_VENDOR=0x10de PCIR_DEVICE=0x2684 LIVE_DEVICE=0x2684 MATCH=1` — the megabyte at BAR0 + 0x300000 on the 4090, compared against the device half of the identity. MATCH=1 is the fix §11.37 made, confirmed on the card it was written for: the earlier build printed the vendor half and read 0
* `VRAM_BLOCK_PROBE_ROM GPU=3080 BAR0_OFFSET=0x300000 BYTES=1048576 SIGNATURE=0xaa55 PCIR_OFFSET=0x0170 PCIR_VENDOR=0x10de PCIR_DEVICE=0x220a LIVE_DEVICE=0x220a MATCH=1` — the same on the 3080. Two structurally valid expansion ROMs, each naming the card the GPU line in this log reports
* `VRAM_BLOCK_PROBE_ROM_FILE GPU=4090 BYTES=1048576 FILE=\PROM-0000-01-00-0-0000.BIN SHA256=422ea53f6b3393720f991fa9e189efefeefce98d1f54486acbfa0ecb96a570ad` — the 4090's window left the machine. The digest is the one -0008 printed a boot earlier, before any capture existed, so the file is the same bytes that were already hashed rather than a second reading that might differ
* `VRAM_BLOCK_PROBE_ROM_FILE GPU=3080 BYTES=1048576 FILE=\PROM-0000-04-00-0-0000.BIN SHA256=742549cf1a4b2ce6e1e854af35655e90b89a9a205603f03be2c096077dac9165` — the 3080's window, under the digest -0008 printed for that card. Two files, each named after the card it came from
* `VRAM_BLOCK_PROBE_END CARDS=2 BLOCKS=42 CONTEXT=24 EXPECTED_READS=132 READS_SUCCEEDED=132 READS_FAILED=0 DIGESTS=2 NO_GPU_WRITES=1 SECURITY_ACCEPTANCE=UNVERIFIED OBSERVED=84 NOT_ACCESSIBLE=48 ROMS=2` — every word read again, with the same totals as -0008; the run added a file write and no GPU write
* `VRAM_BLOCK_PROBE_CARD GPU=4090 READ=66 FAILED=0 OBSERVED=34 NOT_ACCESSIBLE=32 WRITE_SET_SHA256=79c7e4b70f94dca1b3c4a0c7483ba7cd6c4a7b9e4cf9948e0768f4f3a95247db` — the 4090's write set is byte-for-byte the state -0008 saw, so nothing moved between the two boots
* `VRAM_BLOCK_PROBE_CARD GPU=3080 READ=66 FAILED=0 OBSERVED=50 NOT_ACCESSIBLE=16 WRITE_SET_SHA256=c643a097fb677fce6c44bfceb3099bb022b0088f5d8b0ba85cb97c1bc064c5c4` — the same for the 3080. A read-only run that changed a digest would be a finding; this one did not

*History:* The defect §11.37 found is fixed here and the boot was repeated, so this run answers two questions at once: MATCH=1 on both cards, and the two PROM files whose digests match the lines that describe them. The images themselves were parsed off the bench by tests/test_fwsec_v2.c and the result is recorded in `prom_captures`: both carry a version-3 FWSEC-FRTS descriptor with a non-zero ucode id and 384 bytes per signature, which is what GuAd104FwsecMatches() requires.

*What it does not show:* that the files are authentic firmware (SIGNATURE_AUTHENTICATED=0 on every line), or that the signature slot the fuse word selects exists for either card: the transaction reads BAR0 0x8241e0 for ucode 9 and that word has never been read on this bench. The 1 MiB images are NVIDIA firmware and are not stored here, only their digests and the fields the parser reported.

### `physical-readonly-0010`

*File:* `tests/data/hardware_session/physical-readonly-0010.txt` — 43149 bytes stored, 43149 as captured, 327 lines. LF as captured.

*Captured:* 2026-11-18. *Bench:* same bench and cabling as -0006 to -0009; first boot of the build that reads the fuse window.

*Score:*

```bash
python3 tools/verify_run_log.py tests/data/hardware_session/physical-readonly-0010.txt --stage readonly
```

It prints `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=7 GATES_FAILED=0`.

*What this log puts on paper:*

* `VRAM_BLOCK_PROBE_FUSE_END GPU=4090 BDF=0000:01:00.0 BASE=0x8241c0 WORDS=16 READ=16 FAILED=0 UCODE9_OFFSET=0x8241e0 UCODE9_RAW=0x00000001 UCODE9_VERSION=1 UCODE9_ACCESS=STATE UCODE9_UNREAD=0` — the word ucode 9 addresses on the 4090, read at last. It says 0x00000001, which is version 1, and the captured ROM's mask 0x3 contains bit 1, so unlock_transaction_fwsec() would copy the second of its two packed signatures and not stop with -ERANGE
* `VRAM_BLOCK_PROBE_FUSE_END GPU=3080 BDF=0000:04:00.0 BASE=0x8241c0 WORDS=16 READ=16 FAILED=0 UCODE9_OFFSET=0x8241e0 UCODE9_RAW=0x00000003 UCODE9_VERSION=2 UCODE9_ACCESS=STATE UCODE9_UNREAD=0` — the same word on the 3080 reads 0x00000003 - version 2 - and its mask 0x7 contains bit 2, so it would copy the third of three. Both cards are selectable, which is the question this boot existed to answer
* `VRAM_BLOCK_PROBE_FUSE GPU=4090 BDF=0000:01:00.0 OFFSET=0x8241fc UCODE_ID=16 RAW=0x00007fff VERSION=15` — the last word of the 4090's window is 0x00007fff, which is version 15 and selectable by no mask here. A probe that read one word and assumed the window was uniform would have been wrong about this card had the ROM named ucode 16; reading all sixteen is what makes the answer a measurement
* `VRAM_BLOCK_PROBE_FUSE GPU=3080 BDF=0000:04:00.0 OFFSET=0x8241fc UCODE_ID=16 RAW=0x00000007 VERSION=3` — the 3080's last word is version 3, and its mask 0x7 has no bit 3. Its id-15 word is 0x00007fff as well. Three words in that window would have refused; the one the id names does not
* `VRAM_BLOCK_PROBE_END CARDS=2 BLOCKS=42 CONTEXT=24 EXPECTED_READS=132 READS_SUCCEEDED=132 READS_FAILED=0 DIGESTS=2 NO_GPU_WRITES=1 SECURITY_ACCEPTANCE=UNVERIFIED OBSERVED=84 NOT_ACCESSIBLE=48 ROMS=2` — the write set and the read context are unchanged to the value: 132 of 132 reads landed, and the sixteen fuse words are not counted here because they are neither a write nor a context row
* `VRAM_BLOCK_PROBE_CARD GPU=4090 READ=66 FAILED=0 OBSERVED=34 NOT_ACCESSIBLE=32 WRITE_SET_SHA256=79c7e4b70f94dca1b3c4a0c7483ba7cd6c4a7b9e4cf9948e0768f4f3a95247db` — the 4090's write set has the same digest for the third boot running, so the new reads changed nothing
* `VRAM_BLOCK_PROBE_ROM GPU=4090 BAR0_OFFSET=0x300000 BYTES=1048576 SIGNATURE=0xaa55 PCIR_OFFSET=0x0170 PCIR_VENDOR=0x10de PCIR_DEVICE=0x2684 LIVE_DEVICE=0x2684 MATCH=1` — the ROM readout is unchanged and no capture file was written in this boot - the capture is its own switch - so the fuse build did not disturb the path -0009 took

*History:* The boot §11.40 asked for. Sixteen words per card at BAR0 0x8241c0, printed with the version gpu_unlock_fuse_version() computes, and gate R7 scores the read. The read set the probe already covered is unchanged - the fuse window is not part of it, which is why the totals are the same as -0008 and -0009 - and the sixteen extra reads are the only new ones.

*What it does not show:* that either fuse word reads the same when the transaction reads it, that the signature the version selects is the one the driver will accept, or that any signature authenticates (SIGNATURE_AUTHENTICATED=0 stands). It also does not answer the two questions that remain before a first write: which captured PROM reference the image trusts, and whether a staged asset bundle matches the pinned hashes.

### `physical-prepare-0004`

*File:* `tests/data/hardware_session/physical-prepare-0004.txt` — 1095 bytes stored, 1095 as captured, 15 lines. CRLF as captured; the recorded digest is over the LF-normalised file.

*Captured:* 2026-11-18. *Bench:* two NVIDIA cards, no display attached, driven from a USB stick; Windows started after the image returned with no error and no hang.

*Score:*

```bash
python3 tools/verify_run_log.py tests/data/hardware_session/physical-prepare-0004.txt --stage prepare
```

It prints `RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=2 GATES_FAILED=0`.

*What this log puts on paper:*

* `PROM_REFERENCE=CAPTURED ROWS=2 AUTHENTICATION=0` — the image names the table it was built with, and the token matches Switches.CapturedPromReference in the receipt; RUN0003 and RUN0004 could not be told apart because no such line existed
* `ACTIVE_PREPARE=INVENTORIED NVIDIA_COUNT=2 REQUESTED=2 READ_ONLY=1` — the announce line sits above it, so the table is named before any stage can refuse
* `ACTIVE_EXECUTION=REFUSED REASON=SELECTION_PREPARATION_ONLY BEFORE_GU_INIT=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLR=0` — carrying the captured table changed nothing about the refusal: preparation is still CPU-only and the transaction is still refused
* `ASSET_INSTALL PRESENT=0` — the NVRAM record is still absent because the installer was never run; it reports on the record, not on the files the loader hashes, so this is not the asset gate
* `STOP: stage=discover status=0x8000000000000003 mutated=0 retained=0 fault=0` — the run ended where a preparation image is designed to end, with nothing written, retained or faulted

*History:* the fourth preparation boot. `RUN0004.TXT` turned out to be byte-identical to `physical-prepare-0003`, so this is the first preparation log that can be told apart from the one before it.

*What it does not show:* nothing about the PROM read. The transaction is still refused before `GuInit`, so the captured table was never consulted, and `ASSET_INSTALL PRESENT=0` says only that the installer has never run.

## Advancing between stages: `donor_gate.py`

One stage's gates passing is not permission to run the next stage. This tool composes the stage verdicts into a single go/no-go answer and is the only thing in the repository that does so.

```powershell
python3 tools/donor_gate.py --readonly media/GPU-TOPOLOGY-DIAGNOSTIC-0000.TXT --prepare media/AD102-UNLOCK/RUN0001.TXT --ack M1_RECOVERY_TOOLKIT_READY,M2_DONOR_CARD_RECORDED,M3_LOG_MEDIUM_EMPTY,M4_DONOR_CARDS_RECORDED,M5_HALT_STOPS_THE_SESSION,M6_STOCK_BOOT_AFTER_EVERY_RUN
```

It refuses to advance unless:

* the read-only stage must pass before preparation counts as done, and preparation must pass before actuation may be attempted at all
* every stage must identify the same ordered target list: the list the first stage names (`SELECTION_REQUEST[n]`, `ACTIVE_PREPARED_STEP[n]`, `PASSIVE_SELECTION=`) must be the list every later stage names, in the same order, every card in it must appear in the read-only census, no card may appear twice, and the PCI id printed beside each BDF must agree across stages. A session where no stage names a target and the machine has more than one card is refused rather than guessed at: an ordered list of two cards is a normal session in this fork, an unnamed one is not
* every manual gate must be acknowledged by its exact id; an unknown id is refused, so a typo cannot silently stand in for M6
* a log carrying the fixture marker is refused (exit 3) unless --self-test is given, and a GO from a self-test still prints SYNTHETIC=1

It prints `DONOR_GATE VERDICT=GO NEXT=...` with every line carrying `HARDWARE_VERIFIED=0`; anything else prints `VERDICT=NO_GO` and one `BLOCKED` line per reason.

Exit codes: `0 GO`, `1 NO_GO`, `2 a listed log is missing`, `3 the log set is synthetic and --self-test was not given`.

What a GO does not prove: that any stage behaved as its gates say, or that the card is safe: a GO repeats evidence the operator supplied, it does not create any.

## The stages

### `readonly` — Read-only contact

*Build:*

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\src\Build-Gpu-Topology-Diagnostic.ps1 -SaveLog
```

Note: the read-only image always saves its numbered log; add -WindowsEspGuid '<reviewed GUID>' only if you want the automatic Windows handover.

*Costs:* nothing: the diagnostic never writes a GPU register, resets a Falcon, calls the vendor Stop or loads firmware

*Exit criteria:* the card is enumerated, its BDF and PCI identity are in the log, and the eligibility flags are printed as false

| Gate | What it checks | Requires | Must not contain |
| --- | --- | --- | --- |
| `R1_IMAGE_RAN_READ_ONLY` | the read-only image ran | `GPU TOPOLOGY DIAGNOSTIC ONLY - no GPU hardware writes or unlock calls` | `ACTIVE_EXECUTION=WAIVED_UNVERIFIED SELECTED=%zu`<br>`STOP_CONTRACT device=%04x stage=%zu` |
| `R2_ENUMERATION_COMPLETED` | the inventory finished, and the collector said so | `PCI_TOPOLOGY_SCOPE=DISPLAY_AND_PCI_BRIDGES INCLUDED=%zu EXCLUDED=%zu`<br>`NVIDIA_DRIVER_OWNER_INVENTORY=READ_ONLY IMAGE_PROVENANCE_UNVERIFIED=1`<br>`DIAGNOSTIC FINISHED; GPU hardware untouched.` | `PCI_ENUM_FAILURE STAGE=`<br>`NVIDIA_GOP_GRAPH=UNAVAILABLE GOP_CAPTURE_FAILED=1` |
| `R3_CARD_IDENTIFIED` | at least one NVIDIA card is named | `NVIDIA_DRIVER_OWNER[%zu] BDF=%04x:%02x:%02x.%u BY_DRIVER=%u` *(one of)*<br>`ACTIVE_GPU[%zu] BDF=%04x:%02x:%02x.%u ID=%04x:%04x` *(one of)* | — |
| `R4_WRITE_SET_READ_BEFORE_ACTUATION` | the block probe read every word the transaction writes | `VRAM_BLOCK_PROBE_BEGIN SOURCE=UNLOCK_610_WRITE_SET BLOCKS=%u CONTEXT=%u MODE=READ_ONLY READ_ONLY=1 NO_STOP=1 NO_DMA=1 NO_RESET=1 NO_UNLOCK=1 NO_FLR=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLASH=0 NO_GPU_WRITES=1`<br>`VRAM_BLOCK_PROBE GPU=%s BDF=0000:%02x:00.0 BLOCK=%s BAR0_OFFSET=%#x ROLE=WRITE STATUS=OBSERVED RAW=%#010x`<br>`VRAM_BLOCK_PROBE_END CARDS=%u BLOCKS=%u CONTEXT=%u EXPECTED_READS=%u READS_SUCCEEDED=%u READS_FAILED=%u DIGESTS=%u NO_GPU_WRITES=1 SECURITY_ACCEPTANCE=UNVERIFIED` | `VRAM_BLOCK_PROBE GPU=%s BDF=0000:%02x:00.0 BLOCK=%s BAR0_OFFSET=%#x ROLE=WRITE STATUS=UNAVAILABLE`<br>`INCOMPLETE_READ_SET=1` |
| `R5_ROM_WINDOW_STRUCTURE` | the probe read the ROM window the transaction refuses a board on | `VRAM_BLOCK_PROBE_ROM GPU=%s BAR0_OFFSET=0x300000 BYTES=%u SIGNATURE=0x%04x PCIR_OFFSET=0x%04x PCIR_VENDOR=0x%04x PCIR_DEVICE=0x%04x LIVE_DEVICE=0x%04x MATCH=%u SHA256=%s READ_ONLY=1 ACCEPTANCE=UNVERIFIED` | `ROM_WINDOW_UNREADABLE GPU=%s BAR0_OFFSET=0x300000 SIGNATURE=0x%04x` |
| `R6_ROM_WINDOW_CAPTURED` | the ROM window the probe read is in a file a later build can parse | `VRAM_BLOCK_PROBE_ROM_FILE GPU=%s BYTES=%u FILE=%s SHA256=%s ROLE=PARSE_OFFLINE READ_ONLY=1 ACCEPTANCE=UNVERIFIED` | `VRAM_BLOCK_PROBE_ROM_FILE=UNAVAILABLE GPU=%s BYTES=%u REASON=FILE_NOT_WRITTEN` |
| `R7_FUSE_VERSION_READ` | the fuse word that selects a packed signature was read | `VRAM_BLOCK_PROBE_FUSE_END GPU=%s BDF=0000:%02x:00.0 BASE=%#x WORDS=%u READ=%u FAILED=%u UCODE9_OFFSET=%#x UCODE9_RAW=%#010x UCODE9_VERSION=%u UCODE9_ACCESS=%s UCODE9_UNREAD=0 READ_ONLY=1 NO_GPU_WRITES=1` | `VRAM_BLOCK_PROBE_FUSE_WINDOW_UNREADABLE GPU=%s BDF=0000:%02x:00.0 BASE=%#x WORDS=%u FAILED=%u READ_ONLY=1`<br>`STATUS=UNAVAILABLE EFI_STATUS=%#llx NO_FALLBACK=1`<br>`UCODE9_UNREAD=1 READ_ONLY=1 NO_GPU_WRITES=1` |

**`R1_IMAGE_RAN_READ_ONLY`** — proves: the read-only diagnostic started and wrote its own log. Does not prove: that the enumeration below finished, or anything about the card.

* `GPU TOPOLOGY DIAGNOSTIC ONLY - no GPU hardware writes or unlock calls` (src/gpu_topology_efi.c) — the banner is printed before any observation, so its presence is the cheapest proof that this build and not another one ran.
* must not contain `ACTIVE_EXECUTION=WAIVED_UNVERIFIED SELECTED=%zu` (src/gpu_discovery.c): a read-only session must not contain the waiver admission, which only the actuating build prints
* must not contain `STOP_CONTRACT device=%04x stage=%zu` (src/msi_release.c): the Stop contract is captured inside the release path; its presence means a Stop was attempted, which no read-only run does

**`R2_ENUMERATION_COMPLETED`** — proves: the strict PCI walk completed and reported its scope, the NVIDIA owner inventory ran on its success branch, the GOP census did not fail, and the report reached its last line. Does not prove: that every PCI node was read correctly or that the topology is usable: a handle that was excluded is counted rather than described, and the supplementary observation that would cross-check it exists only on the failure path.

* `PCI_TOPOLOGY_SCOPE=DISPLAY_AND_PCI_BRIDGES INCLUDED=%zu EXCLUDED=%zu` (src/gpu_topology_efi.c) — the strict collector reports how many protocol handles it included and excluded, which is the shape of the enumeration it actually performed.
* `NVIDIA_DRIVER_OWNER_INVENTORY=READ_ONLY IMAGE_PROVENANCE_UNVERIFIED=1` (src/gpu_topology_efi.c) — the NVIDIA owner inventory ran, and it runs only on the strict collector's success branch (`if(pci_ok)`): the failure branch prints `NVIDIA_DRIVER_OWNER_INVENTORY=UNAVAILABLE PCI_CAPTURE_FAILED=1` instead. This line therefore proves the walk completed without a vendor callback, and that the driver images' provenance is unverified.
* `DIAGNOSTIC FINISHED; GPU hardware untouched.` (src/gpu_topology_efi.c) — the report reached its own last line. A log that stops earlier - a hang, a reset, an aborted save - cannot satisfy R2, which is the case a truncated capture fails on.
* must not contain `PCI_ENUM_FAILURE STAGE=` (src/gpu_topology_efi.c): any enumeration failure, at any stage, invalidates the inventory even when later lines look complete
* must not contain `NVIDIA_GOP_GRAPH=UNAVAILABLE GOP_CAPTURE_FAILED=1` (src/gpu_topology_efi.c): the GOP census is part of the inventory; a failed capture makes every per-GPU GOP count in the log meaningless

**`R3_CARD_IDENTIFIED`** — proves: the run is about a real card with an address and an identity, not an empty machine. Does not prove: that the identity matches the donor card the operator recorded by hand.

* `NVIDIA_DRIVER_OWNER[%zu] BDF=%04x:%02x:%02x.%u BY_DRIVER=%u` (src/gpu_topology_efi.c) — the driver-owner line carries BDF, driver binding and opener state for one card.
* `ACTIVE_GPU[%zu] BDF=%04x:%02x:%02x.%u ID=%04x:%04x` (src/gpu_discovery.c) — the active-app inventory line carries BDF and PCI identity for one card.

**`R4_WRITE_SET_READ_BEFORE_ACTUATION`** — proves: that a read-only image reached every BAR0 word the actuating sequence writes, on each verified card, before anything wrote to them. That is the interlock: the address list is not a surprise at actuation time. Does not prove: that the values are right, that a selector write will be accepted, or that the transaction will unlock anything. Reading a register before writing it identifies the block and nothing more; the post-actuation half of the comparison needs an actuation, and none has run.

* `VRAM_BLOCK_PROBE_BEGIN SOURCE=UNLOCK_610_WRITE_SET BLOCKS=%u CONTEXT=%u MODE=READ_ONLY READ_ONLY=1 NO_STOP=1 NO_DMA=1 NO_RESET=1 NO_UNLOCK=1 NO_FLR=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLASH=0 NO_GPU_WRITES=1` (src/gpu_topology_efi.c) — the probe says what it is once per boot and states the size of its own address list: a second copy would mean two cards' worth of output in one bracket. At most 1 per log.
* `VRAM_BLOCK_PROBE GPU=%s BDF=0000:%02x:00.0 BLOCK=%s BAR0_OFFSET=%#x ROLE=WRITE STATUS=OBSERVED RAW=%#010x` (src/gpu_topology_efi.c) — at least forty-two observed write-set words: twenty per engine plus both selectors, which is the whole table. Fewer means a block did not read back and the interlock is incomplete.
* `VRAM_BLOCK_PROBE_END CARDS=%u BLOCKS=%u CONTEXT=%u EXPECTED_READS=%u READS_SUCCEEDED=%u READS_FAILED=%u DIGESTS=%u NO_GPU_WRITES=1 SECURITY_ACCEPTANCE=UNVERIFIED` (src/gpu_topology_efi.c) — the closing line carries the arithmetic: cards, blocks, expected reads, reads that landed and reads that did not. `READS_FAILED=0` and `READS_SUCCEEDED=EXPECTED_READS` are what make the block list complete rather than partial. At most 1 per log.
* must not contain `VRAM_BLOCK_PROBE GPU=%s BDF=0000:%02x:00.0 BLOCK=%s BAR0_OFFSET=%#x ROLE=WRITE STATUS=UNAVAILABLE` (src/gpu_topology_efi.c): a word the probe could not read is a hole in the interlock: the address was never confirmed on this silicon, so the stage does not pass
* must not contain `INCOMPLETE_READ_SET=1` (src/gpu_topology_efi.c): the probe withholds its digest when any read failed; a log that says so has no before-and-after comparison to offer

**`R5_ROM_WINDOW_STRUCTURE`** — proves: that a read-only image copied the megabyte at BAR0 + 0x300000 the transaction copies before its first write, and that the image's own signature and PCIR identity name the card it was read from. Does not prove: that the ROM is authentic, that it carries the FWSEC-FRTS descriptor the transaction looks for, or that preparation will accept it. A MATCH=1 says the structure names this device and nothing more; SIGNATURE_AUTHENTICATED is not printed because nothing here authenticates a signature.

* `VRAM_BLOCK_PROBE_ROM GPU=%s BAR0_OFFSET=0x300000 BYTES=%u SIGNATURE=0x%04x PCIR_OFFSET=0x%04x PCIR_VENDOR=0x%04x PCIR_DEVICE=0x%04x LIVE_DEVICE=0x%04x MATCH=%u SHA256=%s READ_ONLY=1 ACCEPTANCE=UNVERIFIED` (src/gpu_topology_efi.c) — one structural readout per verified card: the ROM's own signature, its PCIR offset and identity, the live device id and whether they name the same card. Two cards are the bench, so two lines.
* must not contain `ROM_WINDOW_UNREADABLE GPU=%s BAR0_OFFSET=0x300000 SIGNATURE=0x%04x` (src/gpu_topology_efi.c): a window the probe could not copy is the exact read the transaction makes before it writes, and it did not happen: the stage cannot claim the ROM is a candidate for this board

**`R6_ROM_WINDOW_CAPTURED`** — proves: that a read-only image put the megabyte it copied from BAR0 + 0x300000 into a file on its own volume, named after the card it came from and the run that captured it, with the digest printed on the same line as the file name. Does not prove: that the file holds any particular descriptor, that the ROM is authentic, or that the FWSEC-FRTS descriptor the transaction looks for is present and acceptable. Nothing in the image parses a descriptor; the file is the input to that question, not an answer to it.

* `VRAM_BLOCK_PROBE_ROM_FILE GPU=%s BYTES=%u FILE=%s SHA256=%s ROLE=PARSE_OFFLINE READ_ONLY=1 ACCEPTANCE=UNVERIFIED` (src/gpu_topology_efi.c) — one file per verified card, each line carrying the digest of the megabyte that was copied, so the bytes that produced the digest left the machine and a parser can be run against them off the bench.
* must not contain `VRAM_BLOCK_PROBE_ROM_FILE=UNAVAILABLE GPU=%s BYTES=%u REASON=FILE_NOT_WRITTEN` (src/gpu_topology_efi.c): the window was read and hashed but no file was written, and a digest on its own cannot answer the FWSEC question the transaction asks before its first write

**`R7_FUSE_VERSION_READ`** — proves: that a read-only image read the sixteen-word fuse window at BAR0 + 0x8241c0 and, for each card it verified, printed the word ucode id 9 addresses together with the version gpu_unlock_fuse_version() computes from it - the number unlock_transaction_fwsec() turns into a packed-signature slot before pci_set_master() and before its first write. Does not prove: that the word will read the same at transaction time, that the version selects a packed signature that is present in the ROM, or that the selected signature authenticates. A fuse word is a value read once through PCI I/O; nothing here verifies a signature, and the slot question stays open until the version is compared with each ROM's sig_versions mask - 0x3 for the 4090 and 0x7 for the 3080 in the captured set, both of which include version zero.

* `VRAM_BLOCK_PROBE_FUSE_END GPU=%s BDF=0000:%02x:00.0 BASE=%#x WORDS=%u READ=%u FAILED=%u UCODE9_OFFSET=%#x UCODE9_RAW=%#010x UCODE9_VERSION=%u UCODE9_ACCESS=%s UCODE9_UNREAD=0 READ_ONLY=1 NO_GPU_WRITES=1` (src/gpu_topology_efi.c) — one closing line per verified card, each naming the word ucode id 9 addresses, its value, and the version that value implies: two cards are the bench, so two lines, and a card whose word is missing says UCODE9_UNREAD=1 instead of being absent.
* must not contain `VRAM_BLOCK_PROBE_FUSE_WINDOW_UNREADABLE GPU=%s BDF=0000:%02x:00.0 BASE=%#x WORDS=%u FAILED=%u READ_ONLY=1` (src/gpu_topology_efi.c): the window was addressed and not one of its words came back: the read that would have answered the slot question did not happen, so the stage cannot claim it did
* must not contain `STATUS=UNAVAILABLE EFI_STATUS=%#llx NO_FALLBACK=1` (src/gpu_topology_efi.c): a word inside the window refused the PCI read - and that word may be the one the transaction's ucode id addresses. A partial window is a partial answer
* must not contain `UCODE9_UNREAD=1 READ_ONLY=1 NO_GPU_WRITES=1` (src/gpu_topology_efi.c): the closing line reports that the word this run was after did not come back, so the version printed beside it is a placeholder and not what the transaction would compute

### `prepare` — Preparation only

*Build:*

```powershell
.\src\Build-Experimental-MSVC.ps1 -SelectionBdf '0000:01:00.0'
```

Note: a selection without -EnableSingleTargetExecution and without the waiver compiles the preparation-only image (GPU-ACTIVE-PREP-*), which refuses to actuate before it starts.

*Costs:* the active app runs its preflight and preparation, logs a refusal before any mutation, and hands back to Windows

*Exit criteria:* preparation completes with MUTATION_AUTHORIZED=0 and no Stop, no waiver and no step line

| Gate | What it checks | Requires | Must not contain |
| --- | --- | --- | --- |
| `P1_PREPARATION_REFUSED_BEFORE_MUTATION` | the app refused to actuate and said so first | `ACTIVE_EXECUTION=REFUSED REASON=SELECTION_PREPARATION_ONLY BEFORE_GU_INIT=1`<br>`ACTIVE_PREPARE=COMPLETE SELECTED=%zu NVIDIA_COUNT=%zu MUTATION_AUTHORIZED=0` | `SELECTED_GPU_STEP=ACTIVE ORDER=%zu TARGET=%zu PEERS=%zu`<br>`STOP_CONTRACT device=%04x stage=%zu` |
| `P2_BOOT_RECORD_READ` | the persistent record was read before anything else | `BOOT_STATE PRESENT=1 PHASE=%u BOOT_COUNT=%u STATUS=%#llx DEVICE=%04x` *(one of)*<br>`BOOT_STATE PRESENT=0` *(one of)* | — |

**`P1_PREPARATION_REFUSED_BEFORE_MUTATION`** — proves: the image refused to actuate at the discovery boundary, before any GU init, and the preflight ran to completion with mutation still unauthorized. Does not prove: that the refusal is the right one for these cards, or that preparing a list means the list may execute: it says the entrypoint is a preparation entrypoint. The counts say what was selected, not what was safe.

* `ACTIVE_EXECUTION=REFUSED REASON=SELECTION_PREPARATION_ONLY BEFORE_GU_INIT=1` (src/gpu_discovery.c) — the refusal is printed inside `GuDiscoverSingleGpu` before any GU init, and it is the line that bounds the session. The application's own `ACTIVE_APP=PREPARATION_ONLY` banner sits *after* that call returns success, and the preparation image is refused *by* that call, so the banner is unreachable in exactly the build it describes; P1 required it until the first two physical preparation runs showed no preparation image can print it. The same holds for the DXE entrypoint's `ACTIVE_DXE=PREPARATION_ONLY` banner.
* `ACTIVE_PREPARE=COMPLETE SELECTED=%zu NVIDIA_COUNT=%zu MUTATION_AUTHORIZED=0` (src/gpu_discovery.c) — preparation reached its end with mutation explicitly unauthorized.
* must not contain `SELECTED_GPU_STEP=ACTIVE ORDER=%zu TARGET=%zu PEERS=%zu` (src/gpu_discovery.c): a step that may write must not appear in a preparation session
* must not contain `STOP_CONTRACT device=%04x stage=%zu` (src/msi_release.c): no Stop may be attempted in a preparation session

**`P2_BOOT_RECORD_READ`** — proves: the record path works on this machine, in one of its two honest states. Does not prove: that the record can be written, or that it survives a power loss; that needs a run that is interrupted.

* `BOOT_STATE PRESENT=1 PHASE=%u BOOT_COUNT=%u STATUS=%#llx DEVICE=%04x` (src/boot_io.c) — a record from an earlier boot was found and decoded.
* `BOOT_STATE PRESENT=0` (src/boot_io.c) — no record was found, which is the correct state for the first boot on a wiped machine.

### `actuate` — Waived actuation

*Build:*

```powershell
.\src\Build-Experimental-MSVC.ps1 -SelectionBdf '0000:01:00.0' -EnableSingleTargetExecution -AcceptUnverifiedActuation
```

Note: the only waiver switch; it also needs a reviewed local/boot_profile.h or -RuntimeEsp, and the matched assets on the ESP. `-SelectionBdfs 'a,b'` builds the ordered two-card list, and the step lines then carry `ORDER=`/`TARGET=` indexes into it.

*Costs:* the one admission that may write: an ordered list executed one card at a time, no retry, with the firmware timer bounded or explicitly off; more than one target is admitted by the same waiver (`MULTI_TARGET_WITHOUT_WAIVER` otherwise)

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
| `M4_DONOR_CARDS_RECORDED` | every card in the machine is recorded, and the ordered target list is written down before the image is built | this fork executes an ordered list rather than a single card: one card is active at a time, each step is gated on its predecessor's completion record, and the cards that are not the current target are tracked for drift rather than ignored. A log names BDFs and device IDs; only the operator can tie those to serials and slots, and only the operator can say which card the session is meant to reach and in what order | for every NVIDIA card in the machine: serial, slot, board model and the VBIOS version the vendor tool reports; then the ordered BDF list the image was built with, in the order the session intends to execute |
| `M5_HALT_STOPS_THE_SESSION` | a HALTED step ends the day, not the loop | no retry is a property of the image; re-running after a halt is the operator reintroducing the risk the image refused | the halt line, and the decision to stop, in the session log |
| `M6_STOCK_BOOT_AFTER_EVERY_RUN` | Windows is started normally with the card enumerated after every run | the log cannot see Windows; device start, driver binding and a display output have to be observed by a person | the Windows build, the driver version, and the device status in Device Manager |

## Advisories

Printed as `NOTE` lines. An absent advisory is a finding to explain, not a failure: these lines exist in some entrypoints and not others.

* `WATCHDOG_REPORTED` — `WATCHDOG BOUND=%u SECONDS ACTIVE=1` (or `WATCHDOG BOUND=0 SECONDS ACTIVE=0 REASON=NO_BUILD_THRESHOLD`) (src/app.c): the application prints whether the firmware timer is armed or deliberately off. The DXE entrypoint does not print this, so it is a note rather than a gate
* `LAUNCH_SOURCE_LOGGED` — `LAUNCH_SOURCE DECISION=%s REASON=%s` (src/boot_io.c): every boot now records where it started from and, when it refuses, which gate failed
* `ASSET_INSTALL_REPORTED` — `ASSET_INSTALL PRESENT=1 FILES=%u` (or `ASSET_INSTALL PRESENT=0`) (src/boot_io.c): says whether a bundle was ever staged on the stick, before discovery assumes anything about assets
* `WRITE_SET_NOT_READABLE` — `VRAM_BLOCK_PROBE_REFUSED GPU=%s WORDS=%u OF=%u SIGNATURE=0xbadf0000 ACCESS=NOT_ACCESSIBLE` (src/gpu_topology_efi.c): the probe met words inside the actuating write set that the engine returns its own inaccessible() shape for: the read succeeded at the PCI I/O level and the value is not device state. Printed once per card that has any, so a run cannot look fully observed while part of the write set was refused
* `SUPPLEMENTARY_PCI_OBSERVATION` — `PCI_OBSERVATION_ONLY=1 NOT_TOPOLOGY=1 CAN_DETACH=0 CAN_UNLOCK=0` (src/gpu_topology_efi.c): printed by `ReportPciEvidence`, which runs only when the strict collector failed (`if(!pci_ok)` in `GuTopoDiagnosticMain`). A healthy run does not print it, so its presence in a log is a finding to explain - and gate R2 required it until the first physical run showed that no healthy machine can satisfy that
* `ROM_WINDOW_UNREADABLE` — `ROM_WINDOW_UNREADABLE GPU=%s BAR0_OFFSET=0x300000 SIGNATURE=0x%04x` (src/gpu_topology_efi.c): the probe's ROM read did not produce a structural readout: the header word was not a ROM signature, or the megabyte copy failed. Printed with a REASON= field, once per card, so an unreadable window is a line in the log instead of a missing one

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
