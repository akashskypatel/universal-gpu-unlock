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

Nothing below has been executed against this tree. No log described here exists, every tool in the chain prints `HARDWARE_VERIFIED=0`, and no step changes a row in `docs/readiness_status.json`: a captured log is something a reviewer reads, not a flag this repository flips.

**The name.** There is no script called `build-sign-stage` in this repository. Building, signing and staging one image is a single command with two switches: `-Only <id>` chooses the image from `docs/image_variants.json`, and `-StageDrive <letter>` copies the signed `EFI\BOOT\BOOTX64.EFI` onto the stick after a verified backup of whatever was there. `src/Build-Sign-Deploy-UniversalGPU.ps1` is that command, and `docs/BUILD_SIGN_DEPLOY.md` is its user document.

### Before the first boot

* `M1_RECOVERY_TOOLKIT_READY` — the programmer, the known-good firmware image and the SPI part number are on the bench before the first boot; none of the images below writes the GPU, but the bench rule does not depend on that
* `M2_DONOR_CARD_RECORDED` — serial, slot, board model and the vendor tool's VBIOS version, so the BDF and device ID in the log can be tied to a piece of silicon afterwards
* `M3_LOG_MEDIUM_EMPTY` — no `GPU-TOPOLOGY-DIAGNOSTIC-*.TXT` and no `AD102-UNLOCK\RUN*.TXT` on the stick; a stale file is indistinguishable from a live one and `session_preflight.py` refuses a medium that carries one
* `M4_ONE_CARD_ONLY` — recorded for the waived stages; a read-only candidate may run with one card or two, and the log must be read against the cabling you actually had connected
* `M6_STOCK_BOOT_AFTER_EVERY_RUN` — after each candidate, boot Windows once and record the build, driver version and device status: the read-only images promise the card is untouched, and this is the observation that would notice if it were not
* `SIGNING_KEY` — the signing certificate lives in WSL (`$HOME/secureboot-signing/uefi-db.key` and `.crt`) and the same certificate must be enrolled in the BIOS `db`; the script refuses to sign when it is not, unless you pass `-SkipDbCheck` and accept a receipt that reads `DB_ENROLLED=UNKNOWN`
* `REPOSITORY_ROOT` — every command is written from the repository root, with the signed outputs landing in `artifacts\signed`

### The steps

**Step 1 — confirm the catalogue accepts the candidate**

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -List
```

*Expect:* one `CATALOGUE ... variants=29 families=4` line, then a `VARIANT <id> SAFETY=<class> BINARY=<name>` line per image; `NOT_BUILDABLE` lines say what this tree cannot produce and why. The candidate must be listed with the safety class in the table below.

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

*Expect:* `STICK FREE_OF_OLD_LOGS=1`, `IMAGE ... BYTES=... SHA256=...` for the file that will actually boot, the six `MANUAL` gates, and `PREFLIGHT_VERDICT=PASS ... CHECKS=2 HARDWARE_VERIFIED=0`. `--skip-profile` is right for every candidate here: the reviewed ESP profile belongs to the waived stages.

*Look for:*

* `PREFLIGHT_VERDICT={verdict}{failed} CHECKS={checked} HARDWARE_VERIFIED=0` — printed by `tools/session_preflight.py`
* `STICK FREE_OF_OLD_LOGS=1` — printed by `tools/session_preflight.py`

*If it does not:* `STALE_LOGS=` with an `ACTION=` line means the medium is not empty: archive and wipe it (M3) rather than rebooting over an old log. `IMAGE EMPTY=1` or `PE_HEADER=0` means step 2 did not produce the file you think it did.

**Step 4 — boot the stick once, from the firmware boot menu**

*Expect:* choose the USB entry once and let the image run to its own end. Do not retry inside the same session: the file number is the boot order, and a second boot writes a second file. The read-only candidates print `GPU hardware untouched` and return, and the handoff candidates start Windows after the log is closed.

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

*Expect:* the file named by the log's own `LOG_FILE=` line is the newest number on the stick; copy it off before wiping anything, and record its SHA-256, byte count and line count next to the candidate id. Wipe the stick afterwards so the next candidate starts from an empty medium.

*Look for:*

* `LOG_FILE=\\GPU-TOPOLOGY-DIAGNOSTIC-%04u.TXT` — printed by `src/gpu_topology_efi.c`

*If it does not:* an empty stick with no log means the image never reached its save step. The screen said why: the last line before it stopped is the finding, and there is nothing to score.

**Step 6 — score the log against the gates**

```bash
python3 tools/verify_run_log.py ~/donor-evidence/<stamp>/GPU-TOPOLOGY-DIAGNOSTIC-0011.TXT --stage readonly
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
| `prep-selected` | refuses-before-mutation | `prepare` | \AD102-UNLOCK\RUNnnnn.TXT, one file per boot, never overwritten | `D5` `B9` `A7` `A4` `E8` `M4` |
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

#### `prep-selected` — preparation for explicit targets

The active application, built without the waiver: it runs the real inventory and selection, prepares its state, and refuses before `GuInit`. This is the only image here that executes the app's own entrypoint on hardware, which is why it is the one the P gates score.

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only prep-selected -SelectionBdf '0000:01:00.0' -StageDrive G
```

*Log:* \AD102-UNLOCK\RUNnnnn.TXT, one file per boot, never overwritten

*Score:* `python3 tools/verify_run_log.py <log> --stage prepare`

* `D5` — `ACTIVE_PREPARE=COMPLETE SELECTED=%zu NVIDIA_COUNT=%zu MUTATION_AUTHORIZED=0` (`src/gpu_discovery.c`): the app's inventory, selection and preparation completed on real firmware and the log says mutation was not authorised: the manual loop D5 says does not exist, run once, by hand
* `B9` — `LAUNCH_SOURCE DECISION=%s REASON=%s REMOVABLE=%u PARTITION=%u` (`src/boot_io.c`): the app's own launch decision, printed before anything else: the USB-launch requirement observed rather than inferred
* `A7` — `BOOT_STATE PRESENT=1 PHASE=%u BOOT_COUNT=%u STATUS=%#llx DEVICE=%04x` (`src/boot_io.c`): the persistent record is read at boot on real firmware, so a later boot's `PRESENT=1` is a record this tree wrote, not a fixture
* `A4` — `ACTIVE_PREPARE=SELECTED_UNSUPPORTED_CARD BDF=%04x:%02x:%02x.%u ID=%04x:%04x` (`src/gpu_discovery.c`): building for one BDF and booting with a different card produces this refusal: the negative case of the image-refuses-a-card-it-was-not-built-for rule
* `E8` — `ACTIVE_GPU[%zu] BDF=%04x:%02x:%02x.%u ID=%04x:%04x SUB=%04x:%04x CLASS=%04x LEGACY_UNLOCK_DEVICE_ID=%u` (`src/gpu_discovery.c`): the identity the app itself resolved, with the device id it would act on: the first row of a per-SKU entry
* `M4` — `ACTIVE_PREPARE=INVENTORIED NVIDIA_COUNT=%zu REQUESTED=%zu READ_ONLY=1` (`src/gpu_discovery.c`): how many NVIDIA cards the app saw, which is the number M4's one-card rule has to be recorded against

*Does not prove:* that any admission gate passed on the card: the preparation image stops before `GuCheckConfig` and before the link check, so a preparation log cannot speak to B3-B8 or C8 at all. It proves the entrypoint ran and refused, nothing about the card's state.

#### `dxe-prep-selected` — DXE preparation for explicit targets

The same preparation as a boot-service driver, loaded from a `Driver####` entry instead of `EFI\BOOT\BOOTX64.EFI`. It answers one question the application cannot: whether the same preflight runs under a driver loaded at `ReadyToBoot`.

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only dxe-prep-selected -SelectionBdf '0000:01:00.0' -SignOnly
```

*Log:* \AD102-UNLOCK\RUNnnnn.TXT, one file per boot, never overwritten

*Score:* `python3 tools/verify_run_log.py <log> --stage prepare`

* `D5` — `ACTIVE_APP=PREPARATION_ONLY ACTIVE_EXECUTION=REFUSED BEFORE_MUTATION=1` (`src/app.c`): the DXE entrypoint reaches the same refusal line as the application, which is what a resident-driver design has to be built on

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

## Advancing between stages: `donor_gate.py`

One stage's gates passing is not permission to run the next stage. This tool composes the stage verdicts into a single go/no-go answer and is the only thing in the repository that does so.

```powershell
python3 tools/donor_gate.py --readonly media/GPU-TOPOLOGY-DIAGNOSTIC-0001.TXT --prepare media/AD102-UNLOCK/RUN0001.TXT --ack M1_RECOVERY_TOOLKIT_READY,M2_DONOR_CARD_RECORDED,M3_LOG_MEDIUM_EMPTY,M4_ONE_CARD_ONLY,M5_HALT_STOPS_THE_SESSION,M6_STOCK_BOOT_AFTER_EVERY_RUN
```

It refuses to advance unless:

* the read-only stage must pass before preparation counts as done, and preparation must pass before actuation may be attempted at all
* every stage must name the same single BDF, so a card swap or a second card cannot slip between stages; a stage that names no PCI id at all is refused rather than assumed
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

*Build:*

```powershell
.\src\Build-Experimental-MSVC.ps1 -SelectionBdf '0000:01:00.0'
```

Note: a selection without -EnableSingleTargetExecution and without the waiver compiles the preparation-only image (GPU-ACTIVE-PREP-*), which refuses to actuate before it starts.

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

*Build:*

```powershell
.\src\Build-Experimental-MSVC.ps1 -SelectionBdf '0000:01:00.0' -EnableSingleTargetExecution -AcceptUnverifiedActuation
```

Note: the only waiver switch; it also needs a reviewed local/boot_profile.h or -RuntimeEsp, and the matched assets on the ESP.

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
