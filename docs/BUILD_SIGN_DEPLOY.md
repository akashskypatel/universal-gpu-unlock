# Build, sign and deploy GPU profiles

### Provision a dedicated staging partition

`-StageDrive G` continues to stage to an existing eligible FAT32 USB volume. `-StageDisk 2` selects a non-system, writable, online GPT disk with enough contiguous unallocated space. Review its ID, model, serial and partition layout, then type the exact `CREATE <disk> <calculated MiB>` shown. The partition defaults to twice the verified staged firmware asset bytes rounded up to a whole MiB, subject to the FAT32 minimum. The current 84,585,936-byte nine-file bundle needs 162 MiB on a 512-byte logical-sector disk (272 MiB minimum on a 4 KiB logical-sector disk). `-PartitionSizeMB` may explicitly increase the size but cannot fall below the computed requirement. The script creates a new EFI partition, checks that the old partitions are unchanged, formats only the new partition as FAT32, signs and stages one explicit profile. No erase, resize, initialize, existing ESP reuse, or boot-order changes. Internal NVMe/SATA/ATA/SAS targets initially support only the combined `firmware-transaction -Operation UnlockReattach` application; the signed image has the new partition GUID compiled in, separate from the Windows ESP. USB GPT provisioning supports ordinary stageable applications. Do not select Windows boot/system disks; keep separate recovery media. A provisioning/build/stage error may leave the new partition; inspect it rather than automatically retry or delete it. Firmware boot-menu visibility and physical launch compatibility are unverified.

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only firmware-transaction -Operation UnlockReattach -StageDisk 2 -AcceptActuatingDeploy
```

The combined application checks its launch volume's free space at startup, before GPU work. At 90% utilization or above, it deletes previous regular RUNnnnn.TXT logs from its own AD102-UNLOCK directory in slot order until utilization falls below 90%, or no eligible logs remain. It preserves the current log, unrelated names, directories, and all logs when the recovery state is unresolved or cannot be read. Below 90%, or if volume usage is unavailable, it deletes nothing. No cleanup occurs during hardware transactions or on the Windows EFI partition. Copy any historical logs you want to preserve. Other profiles keep their existing logging policy.

Omit the disk number to list physical disks and request a selection: `-StageDisk`. Omit the drive letter to list disks with their assigned drive letters and request a USB volume: `-StageDrive`. Explicit `-StageDisk 2` and `-StageDrive G` still work without the selection prompt. The inventory shows all disks, including ineligible ones; selection never bypasses the existing safety checks or the separate partition-creation confirmation. Blank input cancels staging.

Use the actual disk number. Boot manually using the firmware boot menu or the firmware's file selector for the dedicated partition's `EFI\BOOT\BOOTX64.EFI`. Do not modify Windows Boot Manager or boot order. Do not deploy before the pinned build and provisioning regression CI passes.


`src/Build-Sign-Deploy-UniversalGPU.ps1` builds every EFI image this repository
can produce, signs each one inside WSL with your personal Secure Boot certificate
(the one already enrolled in the BIOS `db`), and - only when you name a single
image and a stick - stages it as `EFI\BOOT\BOOTX64.EFI`.

When you stage the `firmware-transaction` profile, the script also stages the required reference firmware files in `AD102-ASSETS` and `GA102-ASSETS` at the USB root. It checks the source and destination file sizes and SHA-256 digests against `docs/asset_provenance.json` before it replaces the boot image. Both architectures share the existing `AD102-ASSETS/gsp_ga10x.bin` file. If an older Ampere copy exists, the script checks that copy too. It refuses a mismatch and preserves existing files. A failed copy can leave verified new asset files on the USB, but does not replace the boot image. The `FirmwareAssets` receipt field records each verified destination. `-SignOnly`, `-ResolveOnly`, diagnostics, driver transitions and maintenance do not stage assets. This procedure does not authenticate NVIDIA firmware or prove hardware compatibility.

The list of images is data: [`image_variants.json`](image_variants.json). The
script executes that catalogue, so "all combinations" is not a hand-maintained
list that rots, and this page's matrix is rendered from the same file.

**Signing is an authenticity statement, not a safety statement.** A signed image
is the one you built and the firmware will load it. It says nothing about whether
the card survives, whether Windows starts the device, or whether the 617.42
Booter would accept anything. Every receipt the script writes contains
`HardwareVerified: false` and `Windows61742BooterProven: false`, and the closing
status line contains `HARDWARE_VERIFIED=0`.

## 1. What you need on the bench

| Piece | Where it lives | How the script uses it |
| --- | --- | --- |
| Private key | WSL: `~/secureboot-signing/uefi-db.key` | `sbsign --key` inside WSL; never copied to Windows |
| Signing certificate | WSL: `~/secureboot-signing/uefi-db.crt` | `sbsign --cert`, then `sbverify --cert` |
| The same certificate, DER | Windows: `src\uefi-db.cer`, or `-PublicCertificate` | Proves the WSL cert is byte-identical to the one you enrolled |
| Enrollment in `db` | The machine's own BIOS | `Get-SecureBootUEFI -Name db`; the script refuses to sign if absent |
| WSL tools | Inside the distribution | `base64 wslpath sha256sum openssl cmp sbsign sbverify` |
| MSVC x64 | `vswhere` finds it, or `-VcVars` | The repository builders need it |

Install the WSL side once:

```bash
sudo apt update && sudo apt install -y sbsign openssl
mkdir -p ~/secureboot-signing
# put uefi-db.key and uefi-db.crt here, mode 600 on the key
```

Enroll the certificate in the BIOS once (this is what makes the signature count):

1. Export the certificate as DER: `openssl x509 -in ~/secureboot-signing/uefi-db.crt -outform DER -out uefi-db.cer`.
2. Copy `uefi-db.cer` beside the script (`src\uefi-db.cer`) so the script can prove the two are the same certificate.
3. In the firmware setup, enroll it into `db` - "Enroll EFI image", "Append", or your board's equivalent for a DER certificate. Turn Secure Boot back on afterwards if you turned it off.
4. Confirm from Windows: the script's `DB_ENROLLED=TRUE` line at the top of a run is the proof, and the receipt records it per image.

If the firmware has no way to enroll a personal certificate, you are running with
Secure Boot off, and the signature is then only an integrity record - say so: use
`-SkipDbCheck`, which records `DB_ENROLLED=UNKNOWN` rather than pretending.

For `firmware-transaction -Operation UnlockReattach`, `-WindowsEspGuid` is optional. Supply the unique GPT
partition GUID of the current Windows EFI System Partition, not a disk GUID or
a partition type GUID, when more than one internal volume contains Windows Boot
Manager. The same exact GUID constrains firmware asset fallback and Windows
image staging. The builder rejects an invalid or zero GUID and the wrapper
checks the `WINDOWS_ESP_GUID` receipt before signing. Without this option, one candidate is selected automatically. If there are
multiple candidates, the application asks you to select one. The image does not change either
partition or the firmware boot order.

The fixed U (`app-unlock`), I (`app-install-driver`), P
(`app-unlock-with-resume`), and combined (`app-firmware-reattach`) applications
resolve the Windows boot destination before firmware asset preparation. With no
explicit partition pin, one matching Windows Boot Manager is selected automatically;
multiple candidates display their GPT partition GUIDs and require a fresh numeric
choice. ESC or timeout cancels without a GPU transaction or driver installation.
The chosen partition is held in memory for staging and fallback reads; no boot
order or partition selection variable is written. I still registers the existing
companion DXE driver and halts; P still requests the existing S3 resume policy.
The explicit combined `-WindowsEspGuid` option bypasses the menu and requires one
exact match. DXE, R, and other profiles retain their existing admission policy.

## Canonical profiles and arguments

Use `-Only <profile>` with the operation-specific arguments below. The five
public profiles resolve to 41 supported build configurations.
`-List` displays the public profiles. `-ListConfigurations` displays the complete build matrix. Only the five profiles are valid `-Only` selectors. Configuration IDs identify resolved builds and historical evidence; they are not command selectors.
`-ResolveOnly` reports the selected configuration without a build, signature
or deployment.

### Diagnostic

Default: `-Probe Topology`, with a file log.

| Probe | Optional arguments | Restrictions |
| --- | --- | --- |
| `Topology` | `-SaveLog:$false`, `-SelectionBdf`, `-SelectionBdfs`, `-WindowsEspGuid` | Console-only output cannot include selection or handoff. Selection and handoff cannot combine. |
| `GopTextCycle` | File log (default) | No compiled selection or Windows handoff. GOP = Graphics Output Protocol. |
| `MsiRom`, `MsiFuse`, `MsiSec2`, `VramBlocks` | `-WindowsEspGuid` for pinned Windows handoff | No compiled selection. Requires a file log. |
| `Preflight` | `-ImageKind Application/Dxe`, `-CapturedPromReference` (application only) | Requires explicit GPU addresses and a file log. No Windows handoff. DXE cannot stage as BOOTX64.EFI. |

Preflight checks preparation and refuses before GPU mutation. A prior latched
state refuses before discovery without changing boot registration.

### Driver transition

Choose an explicit `-Operation`. These modes do not perform the firmware
transaction.

| Operation | Arguments | Restrictions |
| --- | --- | --- |
| `StopReattach` | `-ImageKind Application/Dxe`, `-SelectionBdf` or `-SelectionBdfs`, `-Handoff Return/WindowsAfterSuccess` | Without selection, uses live supported GPU enumeration. Default handoff is Return. WindowsAfterSuccess requires the live-all application. No explicit ESP pin. The DXE form needs the private ESP profile. |
| `ObserveStop` | `-ObservationTarget RTX4090/RTX3080/BothNvidia`, `-Handoff Halt/WindowsAfterSuccess`, `-WindowsEspGuid` | Application only. No compiled BDF selection. Single-card default without ESP pin is Halt; with pin, Windows handoff. BothNvidia requires pinned handoff. |

ObserveStop is not a reattachment test. StopReattach retains the
existing driver release, reattachment and final device graph checks.

### Firmware transaction

Choose an explicit `-Operation`. RAM-only and resume configurations are
distinct. ESP = EFI System Partition. DXE = Driver Execution Environment.

| Operation | Arguments | Restrictions |
| --- | --- | --- |
| `UnlockReattach` | Optional `-WindowsEspGuid`, `-EspPolicy Runtime` | Runtime application selection only. No compiled BDF list. Combined transaction and original-driver reattachment. No resume writes. |
| `Unlock` | `-EspPolicy Runtime` | Runtime application selection only. No compiled BDF list or ESP pin. RAM-only. |
| `UnlockWithResume` | `-EspPolicy Runtime` | Runtime application selection only. No compiled BDF list or ESP pin. Requests the existing S3 resume policy. Physical resume verification is pending. |
| `Menu` | `-SelectionBdf` or `-SelectionBdfs`, `-EspPolicy Pinned/Resident` | Application only. Omit selection for runtime selection. Pinned is default and needs private ESP profile. Resident supports zero or one compiled target, not a sequence. No explicit ESP pin. |
| `DxeTransaction` | `-ImageKind Dxe`, `-SelectionBdf` or `-SelectionBdfs`, `-EspPolicy Pinned` | Pinned private ESP profile required. No explicit ESP pin or captured reference. Cannot stage as BOOTX64.EFI. |

The combined operation has a historical physical PASS on the RTX 4090 and
RTX 3080 with displays connected. Manual cold-power recovery after a failed
transaction is owner-confirmed. Numerical accuracy and sleep-resume
persistence are pending physical tests. A new signed image receipt is not
proof of a new physical test.

### Maintenance

Choose `-Operation InstallDriver`, `RemoveDriver` or `ReviewBootState`.
All three use an application. Compiled GPU selection, ESP pin, captured
reference and companion staging are not applicable. InstallDriver and
RemoveDriver retain their existing preparation and recovery behavior.
ReviewBootState remains a separate image with its existing isolation,
record review and confirmation checks.

### Compile validation

`compile-check` requires `-SelectionBdf` or `-SelectionBdfs`.
`-ImageKind Application` is the default. Use `-ImageKind Dxe` for a
boot-service driver build. This profile cannot stage or execute. No ESP or
captured reference options apply.

### Common arguments

| Argument | Function |
| --- | --- |
| `-Only <profile>` | Select one canonical profile with its arguments. |
| `-List`, `-ListConfigurations` | List public profiles or build configurations. |
| `-ResolveOnly` | Inspect one configuration. Does not build or stage. |
| `-SignOnly` | Build and sign. Do not stage. |
| `-StageDrive <letter>` | Stage one EFI application to the verified USB volume. Cannot combine with SignOnly or All. |
| `-AcceptActuatingDeploy` | Explicit consent required for deployment of driver-transition, firmware-transaction and maintenance images. |
| `-All`, `-Family <builder-family>` | Build matrix controls. No canonical operation options with a matrix request. |
| `-SelectionBdf`, `-SelectionBdfs` | One compiled PCI bus/device/function address (BDF), or a comma-separated ordered list, where supported. Do not combine them. |
| `-WindowsEspGuid` | Unique GPT partition GUID, where supported. Not the disk GUID or ESP type GUID. |
| `-CapturedPromReference` | Include existing captured references for supported application-builder configurations. Not a signature-authentication claim. |
| `-StageDxeCompanion` | Existing paired one-target menu application and DXE driver deployment only. |
| `-VcVars` | Explicit path to the Visual Studio x64 environment script. All five builders use shared environment discovery when omitted. |
| `-RepoRoot`, `-OutputDirectory` | Repository path and local signed output path. |
| `-PublicCertificate`, `-WslDistribution`, `-WslUser` | Select signing certificate and Windows Subsystem for Linux (WSL) signing environment. |
| `-SkipDbCheck` | Sign without confirming certificate enrollment. Records UNKNOWN. |

The default image kind is Application. The supported options are combinations,
not independent switches that can always combine. The resolver rejects
unsupported canonical combinations before compilation.

Inspect a configuration without compiling, signing or staging:

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diagnostic -Probe MsiFuse -ResolveOnly
```

`-ResolveOnly` reports the canonical profile, configuration ID, request
origin, builder, switches, safety class, PE subsystem, predicted unsigned
filename, validated compiled GPU selection, missing private-profile prerequisite
and expected attestation templates. The ResolvedAttestations field contains the
actual expected build values after selection and partition substitution. It
uses the same resolver as the build's independent status-line check. It
validates selection addresses and
applicable partition GUIDs without writing a selection header. It does not
check the compiler, signing certificate, physical
hardware or firmware execution. The signed build receipt records actual
verified attestations separately from the expected templates.

See [the migration tracker](EFI_IMAGE_PROFILE_CONSOLIDATION_PLAN.md).

## 2. The catalogue

<!-- IMAGE-VARIANTS:BEGIN (generated by tools/render_image_variants.py from docs/image_variants.json) -->5 canonical profiles. 41 build configurations across 5 builders.

| Canonical profile | Configurations |
| --- | ---: |
| `diagnostic` | 15 |
| `driver-transition` | 8 |
| `firmware-transaction` | 11 |
| `maintenance` | 3 |
| `compile-check` | 4 |

Build configuration matrix (also available with `-ListConfigurations`). Configuration IDs identify resolved builds and evidence; only profiles are selectors. `Safety` is what the image may do to the system, not what it has been shown to do.

### Read-only diagnostic — `src/Build-Gpu-Topology-Diagnostic.ps1` (PE subsystem 10, `STATUS=COMPLETE`)

| Configuration ID | Output | Safety | Needs | What it is |
| --- | --- | --- | --- | --- |
| `diag-topology` | `GPU-TOPOLOGY-DIAGNOSTIC.EFI` | `read-only` | nothing | the smallest read-only image: it prints to the console and writes nothing |
| `diag-topology-log` | `GPU-TOPOLOGY-DIAGNOSTIC.EFI` | `read-only` | nothing | stage 1 of the donor session: the log this build writes is what tools/verify_run_log.py scores |
| `diag-topology-gop-text-cycle` | `GPU-TOPOLOGY-GOP-TEXT-CYCLE-DIAGNOSTIC.EFI` | `read-only` | nothing | Explicit visual diagnostic: logs per-GOP SimpleTextOut and GOP driver image; writes only through already installed text interfaces and pauses two seconds each. Does not Stop, reconnect, set display modes, change global ConOut, or write NVRAM. Successful text API return is not proof of visible pixels. |
| `diag-topology-handoff` | `GPU-TOPOLOGY-WINDOWS-HANDOFF-DIAGNOSTIC.EFI` | `read-only` | reviewed ESP GUID | starts the pinned Windows Boot Manager after the log is saved; needs a reviewed ESP GUID |
| `diag-selection` | `GPU-SELECTION-<label>-DIAGNOSTIC.EFI` | `read-only` | one or more BDFs | the builder refuses a selected diagnostic with Windows handoff, so this one always returns to the launch environment |
| `diag-msi-rom` | `MSI-B850-ROM-VARIABLES-DIAGNOSTIC.EFI` | `read-only` | nothing | reads the MS-7E56 UEFI variables; cannot be combined with a GPU selection |
| `diag-msi-rom-handoff` | `MSI-B850-ROM-VARIABLES-WINDOWS-HANDOFF-DIAGNOSTIC.EFI` | `read-only` | reviewed ESP GUID | as above, then hands off to Windows |
| `diag-msi-fuse` | `MSI-B850-FUSE-VERSION-DIAGNOSTIC.EFI` | `read-only` | nothing | reads the FWSEC/GSP signature-slot fuse on each verified NVIDIA GPU |
| `diag-msi-fuse-handoff` | `MSI-B850-FUSE-VERSION-WINDOWS-HANDOFF-DIAGNOSTIC.EFI` | `read-only` | reviewed ESP GUID | the fuse read that fixes the signature slot; still not the Windows 617.42 provenance |
| `diag-msi-sec2` | `MSI-B850-SEC2-UCODE1-DIAGNOSTIC.EFI` | `read-only` | nothing | reads BAR0 0x824140 only, and prints WINDOWS_61742_SIGNATURE_SLOT=UNKNOWN |
| `diag-msi-sec2-handoff` | `MSI-B850-SEC2-UCODE1-WINDOWS-HANDOFF-DIAGNOSTIC.EFI` | `read-only` | reviewed ESP GUID | as above, then hands off to Windows |
| `diag-vram-blocks` | `VRAM-BLOCK-PROBE-DIAGNOSTIC.EFI` | `read-only` | nothing | reads the 42 BAR0 words the waived transaction writes, once per verified card, and writes nothing; the safety interlock before the first actuation |
| `diag-vram-blocks-handoff` | `VRAM-BLOCK-PROBE-WINDOWS-HANDOFF-DIAGNOSTIC.EFI` | `read-only` | reviewed ESP GUID | as above, then hands off to Windows; the probe's digest is in the log either way |

### Active preparation (application) — `src/Build-Experimental-MSVC.ps1` (PE subsystem 10, `STATUS=COMPLETE`)

| Configuration ID | Output | Safety | Needs | What it is |
| --- | --- | --- | --- | --- |
| `prep-selected` | `GPU-ACTIVE-PREP-<label>.EFI` | `refuses-before-mutation` | one or more BDFs | stage 2 of the donor session: runs the whole preflight, then refuses before GuInit |
| `compile-one-target` | `GPU-ONE-TARGET-COMPILE-ONLY-<label>.EFI` | `compile-only` | one BDF | the CI-equivalent compile check for an operator's own selection |
| `compile-sequence` | `GPU-SEQUENCE-COMPILE-ONLY-<label>.EFI` | `compile-only` | two or more BDFs | two or more targets, one active at a time |
| `actuate-one-target` | `GPU-ONE-TARGET-UNVERIFIED-ACTUATION-<label>.EFI` | `actuates` | one BDF, private `src/local/boot_profile.h` | stage 3 of the donor session; the only image that may write, and its own status line says it is unverified |
| `actuate-sequence` | `GPU-SEQUENCE-UNVERIFIED-ACTUATION-<label>.EFI` | `actuates` | two or more BDFs, private `src/local/boot_profile.h` | two or more targets, each step gated on its predecessor, no retry |
| `app-stop-release-sequence-bench` | `GPU-SEQUENCE-STOP-RELEASE-BENCH-RUNTIME.EFI` | `actuates` | nothing | Bootable EFI application. At each boot it enumerates current PCI devices and selects every currently detected supported NVIDIA GPU in firmware enumeration order, without a compiled BDF list or persistent GuTargets write. Waives ASPM admission. Console policy is topology-dependent: before Stop, the image makes a best-effort attempt to display the warning through configured ConOut, including when the target is the only GOP; this attempt is not a console admission requirement. If that target is the only GOP, release diagnostics use the authoritative file log, then after ConnectController and reattachment graph verification the image makes a best-effort completion display through current ConOut; console-output failure does not invalidate the verified sequence. Otherwise ConOut, ErrOut and the live TextOut graph must resolve to a verified non-target survivor GOP, including an unselected NVIDIA peer. It runs msi_release.c Stop/release, ConnectController reattachment and protocol/GOP graph checks for the live selection, then returns to firmware without launching another EFI program. The pre-Stop warning lists each selected device's live BDF, PCI ID and subsystem ID; successful or unavailable console display is recorded in the file log. vendor/unlock_610.c is excluded and no firmware unlock transaction is called. Displays on listed GPUs may go dark, disconnect or flicker; physical display routing, reattachment, recovery and hardware behavior are unverified. No firmware asset files are loaded and PROM/signature checks are not run. File logging and the live device warning are retained; routine screen output is suppressed. For a fixed ordered subset instead, pass -SelectionBdfs with one or more BDFs when building this stop-only variant; the build embeds and reports that exact order. |
| `app-stop-release-sequence-handoff` | `GPU-SEQUENCE-STOP-RELEASE-BENCH-RUNTIME-WINDOWS-HANDOFF.EFI` | `actuates` | nothing | Uses the same runtime GPU selection, Stop/release and verified reattachment sequence as the Stop-only application. After each verified reattachment, the application makes a best-effort attempt to replace the warning on its original peer display with a success message and target address. After all targets complete, loads the Windows Boot Manager from exactly one detected non-USB EFI system partition. Rechecks the complete GPU sequence, closes the USB log, then starts Windows without a reset. A failed check blocks explicit handover. No fixed BDFs, firmware unlock, flash or NVRAM writes. Windows boot and physical recovery remain unverified. |
| `actuate-one-target-resident` | `GPU-ONE-TARGET-UNVERIFIED-ACTUATION-<label>.EFI` | `actuates` | one BDF | the B9 relaxation combined with the waiver: the riskiest image this repository can build |
| `actuate-runtime-selection` | `GPU-RUNTIME-SELECTION-UNVERIFIED-ACTUATION.EFI` | `actuates` | private `src/local/boot_profile.h` | no compiled BDF, so the operator picks the target at the machine instead of at the build |
| `actuate-runtime-selection-resident` | `GPU-RUNTIME-SELECTION-UNVERIFIED-ACTUATION.EFI` | `actuates` | nothing | no compiled BDF and no compiled ESP GUID: both are resolved at boot |
| `app-firmware-reattach` | `GPU-RUNTIME-FIRMWARE-REATTACH.EFI` | `actuates` | nothing | Combined firmware transaction and driver reattachment application. Uses runtime target selection and existing firmware admission. Requires a verified non-target NVIDIA display warning before each Stop. Verifies the existing firmware transaction and resource cleanup before closing exclusive ownership. Reconnects the original driver and verifies the restored device, child, display and peer graph. Stops the sequence on failure. Starts Windows only after all targets pass and the log closes. Retains the one-use restart record. CUDA verification is manual after Windows starts. No resume writes. Explicitly waives ASPM admission only; retains link training, negotiated width/speed and uncorrected error refusals. Historical physical PASS: RUN0002(1).TXT on RTX 4090 and RTX 3080 with connected displays, followed by Windows driver 617.42 and CUDA execution. Manual failed-transaction cold-power recovery is owner-confirmed. Numerical accuracy, sleep-resume persistence and other configurations remain unverified. |
| `app-unlock` | `GPU-RUNTIME-SELECTION-UNVERIFIED-ACTUATION-UNLOCK.EFI` | `actuates` | nothing | Existing U handler selected by the image, without the operation menu. Existing target selection, preparation, recovery latch and admission checks remain in force. RAM-only; no new S3 script requested. Not a combined reattachment/persistence profile. |
| `app-unlock-with-resume` | `GPU-RUNTIME-SELECTION-UNVERIFIED-ACTUATION-UNLOCK-WITH-RESUME.EFI` | `actuates` | nothing | Existing P handler selected by the image, without the operation menu. Existing target selection, preparation, recovery latch and admission checks remain in force. Existing S3 resume policy is requested. Not a combined reattachment/persistence profile. |
| `app-install-driver` | `GPU-RUNTIME-SELECTION-UNVERIFIED-ACTUATION-INSTALL-DRIVER.EFI` | `actuates` | nothing | Existing I handler selected by the image, without the operation menu. Existing target selection, preparation, recovery latch and admission checks remain in force. Registers only the existing companion driver path; the correctly reviewed companion must already be present. Stops after the operation instead of rebooting into itself. Not a combined reattachment/persistence profile. |
| `app-remove-driver` | `GPU-RUNTIME-SELECTION-UNVERIFIED-ACTUATION-REMOVE-DRIVER.EFI` | `actuates` | nothing | Existing R handler selected by the image, without the operation menu. Existing target selection, preparation, recovery latch and admission checks remain in force. Removes owned driver entries; does not clear the restart barrier. Stops after the operation instead of rebooting into itself. Not a combined reattachment/persistence profile. |

### Active preparation (DXE driver) — `src/Build-Dxe-Driver.ps1` (PE subsystem 11, `STATUS=COMPLETE`)

| Configuration ID | Output | Safety | Needs | What it is |
| --- | --- | --- | --- | --- |
| `dxe-prep-selected` | `GPU-ACTIVE-PREP-DXE-<label>.efi` | `refuses-before-mutation` | one or more BDFs | the same preparation as a boot-service driver; it cannot be staged as BOOTX64.EFI |
| `dxe-compile-one-target` | `GPU-ONE-TARGET-COMPILE-ONLY-DXE-<label>.efi` | `compile-only` | one BDF | proves the DXE entrypoint links for this selection |
| `dxe-compile-sequence` | `GPU-SEQUENCE-COMPILE-ONLY-DXE-<label>.efi` | `compile-only` | two or more BDFs | two or more targets, compile-checked only |
| `dxe-actuate-one-target` | `GPU-ONE-TARGET-UNVERIFIED-ACTUATION-DXE-<label>.efi` | `actuates` | one BDF, private `src/local/boot_profile.h` | NVRAM-installed driver form; -StageDxeCompanion stages this exact paired build at EFI\Drivers\AdaUnlockDxe.efi, never as BOOTX64.EFI |
| `dxe-actuate-sequence` | `GPU-SEQUENCE-UNVERIFIED-ACTUATION-DXE-<label>.efi` | `actuates` | two or more BDFs, private `src/local/boot_profile.h` | NVRAM-installed driver form, ordered |
| `dxe-stop-release-sequence-bench` | `GPU-SEQUENCE-STOP-RELEASE-BENCH-RUNTIME-DXE.efi` | `actuates` | private `src/local/boot_profile.h` | Dynamic DXE test that selects all currently detected supported NVIDIA GPUs in firmware enumeration order, without compiled BDFs or persistent GuTargets writes. Waives ASPM admission. Console policy is topology-dependent: before Stop, the image makes a best-effort attempt to display the warning through configured ConOut, including when the target is the only GOP; this attempt is not a console admission requirement. If that target is the only GOP, release diagnostics use the authoritative file log, then after ConnectController and reattachment graph verification the image makes a best-effort completion display through current ConOut; console-output failure does not invalidate the verified sequence. Otherwise ConOut, ErrOut and the live TextOut graph must resolve to a verified non-target survivor GOP, including an unselected NVIDIA peer. Each selected GPU runs msi_release.c driver Stop/release, then ConnectController reattachment and protocol/GOP graph verification. The pre-Stop warning lists each selected device's live BDF, PCI ID and subsystem ID; successful or unavailable console display is recorded in the file log. The vendor gpu_unlock_610 firmware transaction is excluded from the image; UNLOCK=0, FLASH=0, RESET=0. Displays on selected GPUs may go dark, disconnect, or flicker; physical reattachment, recovery and hardware behavior are unverified. No firmware asset files are loaded and PROM/signature checks are not run. File logging and the live device warning are retained; routine screen output is suppressed. The DXE driver returns to firmware after the release sequence. For a fixed ordered subset instead, pass -SelectionBdfs with one or more BDFs when building this stop-only variant; the build embeds and reports that exact order. |
| `dxe-actuate-runtime-selection` | `GPU-RUNTIME-SELECTION-UNVERIFIED-ACTUATION-DXE.efi` | `actuates` | private `src/local/boot_profile.h` | NVRAM-installed driver, target at boot |

### Original vendor Stop experiment — `src/Build-Msi-Stop-Observation.ps1` (PE subsystem 10, `STATUS=BUILT`)

| Configuration ID | Output | Safety | Needs | What it is |
| --- | --- | --- | --- | --- |
| `stop-4090-once` | `MSI-RTX4090-STOP-ONCE-EXPERIMENT.EFI` | `stop-only` | nothing | halts after the Stop; the vendor callback can hang without a timeout |
| `stop-4090-handoff` | `MSI-RTX4090-STOP-WINDOWS-HANDOFF-EXPERIMENT.EFI` | `stop-only` | reviewed ESP GUID | starts the pinned Windows Boot Manager after the Stop returns |
| `stop-3080-once` | `MSI-RTX3080-STOP-ONCE-EXPERIMENT.EFI` | `stop-only` | nothing | GA102 variant: requires the original 148,992-byte ROM and the original GOP driver |
| `stop-3080-handoff` | `MSI-RTX3080-STOP-WINDOWS-HANDOFF-EXPERIMENT.EFI` | `stop-only` | reviewed ESP GUID | GA102 variant, then hands off to Windows |
| `stop-dual-handoff` | `MSI-DUAL-NVIDIA-STOP-WINDOWS-HANDOFF-EXPERIMENT.EFI` | `stop-only` | reviewed ESP GUID | one boot, two Stops, AMD left attached; the builder refuses this without pinned handoff |

### Boot-state maintenance — `src/Build-Boot-State-Maintenance.ps1` (PE subsystem 10, `STATUS=BUILT`)

| Configuration ID | Output | Safety | Needs | What it is |
| --- | --- | --- | --- | --- |
| `boot-state-maintenance` | `BOOT-STATE-MAINTENANCE.EFI` | `maintenance` | nothing | Independent maintenance application. Requires explicit deployment consent and isolated USB media. Displays the exact record, requires D/d confirmation, refuses any present or unreadable DriverOrder, checks the record again and verifies absence after deletion. No GPU action, boot entry change or reset. Physical execution is unverified. |

### Not buildable, and why

* `legacy-experimental` — the unselected mutating Experimental image is deliberately disabled: its mutation path is not generally hardware-safe, and the builder throws if asked
* `stop-dual-without-handoff` — the dual Stop requires a pinned Windows handoff, so a no-handoff dual image has no legal build
* `dxe-as-bootx64` — a DXE boot-service driver is PE subsystem 11; firmware will not run it as BOOTX64.EFI, and the script refuses to stage one
* `esp-resident-dxe` — GU_ESP_RESIDENT gates the launch of an application image; a Driver#### entry is not launched from a volume, so the combination would advertise a gate that cannot run
* `prep-runtime-selection` — a preparation-only image cannot also read its targets at boot: src/port.h requires the ordered selected-target mode (GU_ACTIVE_SINGLE_TARGET) for GU_RUNTIME_SELECTION, and src/gpu_discovery.c refuses that mode with a preflight-only profile, so no define set satisfies both - the source needs a preparation profile for runtime target selection first
* `prep-runtime-selection-resident` — a preparation-only image cannot also read its targets at boot: src/port.h requires the ordered selected-target mode (GU_ACTIVE_SINGLE_TARGET) for GU_RUNTIME_SELECTION, and src/gpu_discovery.c refuses that mode with a preflight-only profile, so no define set satisfies both - the source needs a preparation profile for runtime target selection first
* `dxe-prep-runtime-selection` — a preparation-only image cannot also read its targets at boot: src/port.h requires the ordered selected-target mode (GU_ACTIVE_SINGLE_TARGET) for GU_RUNTIME_SELECTION, and src/gpu_discovery.c refuses that mode with a preflight-only profile, so no define set satisfies both - the source needs a preparation profile for runtime target selection first

<!-- IMAGE-VARIANTS:END -->

**Fixed application operations:** The four `app-unlock`, `app-unlock-with-resume`, `app-install-driver` and `app-remove-driver` variants select the existing U, P, I and R handlers at build time. The normal menu image remains unchanged. These variants use runtime target selection and runtime Windows ESP discovery; an absent target policy can still require target input. The application builder also accepts `-ApplicationOperation` with an explicitly selected target list. The fixed I/R profiles stop after the administrative result and checked log close, with no automatic reset; power off and select the next boot manually. Installation requires the existing reviewed companion driver at the expected path; removal does not clear the experiment record. All four variants retain the explicit actuating deployment classification and the existing preparation, recovery, ownership and verification requirements. They are not combined firmware/reattach profiles and do not qualify firmware persistence.

**Sequential unlock progress:** The combined firmware/reattach application shows the existing preparation trace for each successor through the direct peer text interface verified in the preceding transaction. It verifies the live interface before each output call. It clears the route before the next driver release. It never restores global console output after actuation. If no verified text route is available, the file log remains authoritative and the existing peer warning remains unchanged. Legacy console traces and explicitly file-only images keep their existing policies. Physical display verification of the new trace route is pending.

**One-use active experiment:** Explicitly waived firmware transaction images retain the readback-verified `GuBootState` INTENT through successful preboot completion and Windows handover. The next boot refuses another GPU transaction while this record remains. A successful preboot result is not owner acceptance. No automatic record deletion or rearm is provided. The Stop-only profiles remain separate and do not create an active transaction record.

**Administrative maintenance image:** The separate `src/Build-Boot-State-Maintenance.ps1` still builds only `src/build_boot_state_maintenance/BOOT-STATE-MAINTENANCE.EFI`. The `maintenance -Operation ReviewBootState` profile signs it through the existing workflow. Staging requires `-Only maintenance -Operation ReviewBootState -StageDrive <letter> -AcceptActuatingDeploy`. The USB drive must contain no other EFI executable apart from the boot image that the script replaces after a verified backup. Use isolated maintenance media and disconnect other experimental boot media before booting. The script never registers the image. It links only the boot-record, console and CPU formatting/runtime source. The image displays the exact record and requires an explicit typed confirmation. It checks the reviewed record again, refuses any existing or unreadable `DriverOrder` (including unrelated entries), and verifies absence after removing the owned record. It cannot accept a GPU result or verify stock recovery. Owner review and separately authorized next experiment are prerequisites; no automatic reactivation. Do not remove unrelated firmware boot entries to satisfy this restriction. The utility stops after a deletion attempt, including uncertain readback, and does not start an image or request a reset. Physical qualification remains outstanding. Run it only for a separately authorized experiment.

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only maintenance -Operation ReviewBootState -SignOnly
```

For an explicitly authorized maintenance deployment, replace `G` with the isolated USB drive letter:

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only maintenance -Operation ReviewBootState -StageDrive G -AcceptActuatingDeploy
```

Before confirmation, photograph the reviewed record. Press `D` or `d` only if the prerequisites are met. No Enter is needed. Any other key cancels. On success, record `REACTIVATION=RECORD_REMOVAL_VERIFIED`. Power off and remove the maintenance media. Do not remove unrelated DriverOrder entries or retry after uncertain readback.

`<label>` is the repository's own selection label from
`src/Gpu-Selection-Build.ps1`: `0000:01:00.0` becomes `0000-01-00-0`, two targets
are joined with `--`, and more than two become `N<count>-<12 hex>`. The script
probes that helper with your selection, so the expected filename and the built
filename come from the same code - if they disagree the run fails instead of
renaming an image.

### Carrying the captured firmware references

`src/prom_reference.c` holds an empty table by default: an image built without
one can only answer `UNKNOWN_CARD` when the transaction asks
`GuAd104PromMatches(…, TRUE)`, and with `GU_PROM_REFERENCE_REQUIRED` that is a
refusal. Two cards have been captured on this bench, and `-CapturedPromReference`
compiles their two rows into the image that asks for them:

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diagnostic -Probe Preflight -SelectionBdfs '0000:01:00.0,0000:04:00.0' -CapturedPromReference -SignOnly
```

It is a switch rather than a catalogue variant on purpose. A variant that
differed only by a define would write the same filename as the variant it
differs from, and two entries writing one path is a run that signs an image it
cannot name. It is also restricted to the `prepare` family: nothing else links a
PROM read behind that table. The receipt records it under `Switches`, the run
prints `PROM_REFERENCE=CAPTURED ROWS=2 … AUTHENTICATION=0`, and the image still
logs `SIGNATURE_AUTHENTICATED=0` — a digest of what was read is not a signature of
what will run.

The image says the same thing about itself, because a receipt is not evidence
about a boot. `src/app.c` prints `PROM_REFERENCE=<NONE|CAPTURED|SYNTHETIC>
ROWS=<n> AUTHENTICATION=0` at open, before any stage can refuse, so a preparation
run that never reaches a PROM read still names the table it was built with. Two
boots of the two builds used to be byte-identical files; the line is what makes
them distinguishable, and it is the first line to compare when a log and a
receipt disagree.

### When a variant asks for the reviewed ESP profile

`VARIANT_SKIPPED id=actuate-sequence REASON=NEEDS_LOCAL_BOOT_PROFILE` — and, under
`-Only`, the `VARIANT_FAILED` that repeats it — is a setup step, not a catalogue
defect. The `prepare` family compiles `src/boot_profile.h`, which includes
`src/local/boot_profile.h` and stops with an `#error` when that file is absent.
That is deliberate: an image allowed to write is pinned to one partition an
operator read out and reviewed, and a compiled-in GUID is the only way the image
can tell that partition from any other.

`-WindowsEspGuid` is not a substitute for the private profile in pinned configurations.
The combined UnlockReattach application accepts an explicit pin without that
private file. The other pinned configurations retain their existing build policy.

Read the current identifier, then write the file:

```powershell
# 1. The unique partition GUID of the Windows ESP. GptType is the ESP *type*
#    (the same on every machine); Guid is this partition.
Get-Partition | Where-Object GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' |
  Select-Object DiskNumber, PartitionNumber, DriveLetter, Guid
# If that column is missing on this Windows build, ask the storage provider:
# Get-CimInstance -Namespace root\Microsoft\Windows\Storage -Class MSFT_Partition |
#   Where-Object GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' |
#   Select-Object DiskNumber, PartitionNumber, DriveLetter, Guid
```

```powershell
# 2. Write it in the initializer form the build expects.
$guid = [guid]'00000000-0000-0000-0000-000000000000'   # <- the Guid printed above
$n    = $guid.ToString('N')
$d4   = (8..15 | ForEach-Object { '0x{0:x2}' -f $guid.ToByteArray()[$_] }) -join ','
$init = '{0x' + $n.Substring(0,8) + ',0x' + $n.Substring(8,4) + ',0x' + $n.Substring(12,4) + ',{' + $d4 + '}}'
New-Item -ItemType Directory -Force src\local | Out-Null
@"
/* SPDX-License-Identifier: GPL-2.0-only */
/* Reviewed ESP profile for this machine. src/local/ is gitignored: this names
 * one partition on one bench and must never be committed. */
#pragma once
#define GU_WINDOWS_PARTITION_INITIALIZER $init
"@ | Set-Content -Encoding ASCII src\local\boot_profile.h

# 3. The check the build and the preflight both rely on.
python3 tools\session_preflight.py --profile src\local\boot_profile.h
```

Expect `PROFILE path=src\local\boot_profile.h PRESENT=1 GUID=REVIEWED`. The other
three answers are each a different mistake: `PRESENT=0` is the wrong path (it is
`src\local`, not the repository root), `GUID=SYNTHETIC` is the placeholder
`0x11111111` still being in the file, and `GUID=UNDEFINED` is a renamed or
misspelled macro.

A GUID read in an earlier session is not evidence about this one: a Windows
reinstall or a re-partition changes it, and the image would be pinned to a
partition that no longer exists. Read it again before rebuilding, and record the
value in the session log next to `M1_RECOVERY_TOOLKIT_READY`.

## 3. Commands

Run these commands from the repository root. Each example is independent.
Use `-ResolveOnly` first to inspect any supported configuration. Replace the
example addresses and GUID with those for the applicable hardware.

**List the profiles and the build configurations:**

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -List
```

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -ListConfigurations
```

**Diagnostic, file log (sign only):**

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diagnostic -SignOnly
```

**Preparation check with explicit selection (sign only):**

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diagnostic -Probe Preflight -SelectionBdfs '0000:01:00.0,0000:04:00.0' -SignOnly
```

**Compile check (sign only):**

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only compile-check -SelectionBdf '0000:01:00.0' -SignOnly
```

**Inspect driver-transition configurations:**

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only driver-transition -Operation StopReattach -ResolveOnly
```

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only driver-transition -Operation StopReattach -ImageKind Dxe -SelectionBdfs '0000:01:00.0,0000:04:00.0' -ResolveOnly
```

**Inspect the combined firmware transaction and resume configuration:**

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only firmware-transaction -Operation UnlockReattach -ResolveOnly
```

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only firmware-transaction -Operation UnlockWithResume -ResolveOnly
```

**Inspect the separate maintenance configuration:**

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only maintenance -Operation ReviewBootState -ResolveOnly
```

**Stage a read-only diagnostic to the intended USB drive (G in this example):**

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diagnostic -StageDrive G
```

**Build matrix (sign only):**

```powershell
.\src\Build-Sign-Deploy-UniversalGPU.ps1 -All -SelectionBdfs '0000:01:00.0,0000:04:00.0' -WindowsEspGuid 'a005858c-4487-4ee2-976e-1710ff484c31' -SignOnly
```

Build and sign runs report WSL_SIGNING_READY before the first build. A failed
signing preflight stops the run. A successful ResolveOnly operation does not
run signing preflight.

`-All` never stages, and it is bounded: 41 build configurations, one `VARIANT_BEGIN` and one
`SIGNED`/`FAILED`/`SKIPPED` line each - it is not a retry loop, so a long scroll
is expected and a failing variant does not stop the rest.

A selection has a shape, and `-All` respects it instead of failing. With two
BDFs the single-target variants are skipped (`REASON=NEEDS_EXACTLY_ONE_BDF`) and
the sequence ones build; with one BDF it is the other way round
(`REASON=NEEDS_TWO_OR_MORE_BDFS`). Run `-All` twice with both shapes, or use
`-Only` for the images you actually need.

Staging takes two things, and neither is a duplicate of the other:
`-StageDrive <letter>` selects the USB drive. `-Only <profile>` and its
arguments select exactly one image because the drive holds one `BOOTX64.EFI`. `-SignOnly` is
exclusive with `-StageDrive`, and `-All` can never stage. The ordinary staging run replaces only `EFI\BOOT\BOOTX64.EFI`. For the waived
one-target image, `-StageDxeCompanion` is an explicit paired build: it builds and
signs the matching `dxe-actuate-one-target` variant first, stages that image at
`EFI\Drivers\AdaUnlockDxe.efi` (the path the I/NVRAM installer opens), then
stages the application at `EFI\BOOT\BOOTX64.EFI`. Both artifacts are hash
verified and replacements receive local backups. It is restricted to
the application configuration `-Only firmware-transaction -Operation Menu` with the pinned ESP policy (configuration ID `actuate-one-target`), a single BDF, `-StageDrive`, and
`-AcceptActuatingDeploy`; the driver is never renamed to BOOTX64.EFI. The output
includes `STAGED_DXE_COMPANION` and `STAGED actuate-one-target` receipts. The
bench-override sequence is a standalone DXE image, not a paired one-target
companion; `-StageDrive` deliberately refuses it. After signing, install it only
at the existing `Driver####` file path (`EFI\\Drivers\\AdaUnlockDxe.efi`) on the
volume that option references, after backing up the prior file and verifying
both hashes. The image itself logs on that volume and emits no screen messages.

Staging requires one explicit canonical `-Only` request with its operation arguments. The resolved configuration selects the image; there is no second deployment selector.

The seven waived images that compile a real ESP identity also need the private
`src\local\boot_profile.h`. When it is absent `-All` skips them with
`REASON=NEEDS_LOCAL_BOOT_PROFILE` and prints one `SETUP_HINT` naming the file -
while `-Only` on one of them is an error, because a named image that silently
does not build is worse than a failed command.

## 4. The gates, in the order they bite

| Gate | What it stops |
| --- | --- |
| `#Requires -Version 5.1`, strict mode | PowerShell 7-only syntax cannot creep in unnoticed; CI parses the file |
| Catalogue lookup | An unknown `-Only` profile, or a family without a builder |
| Builder preflight | The builders' own refusals (unselected mutating image, waiver without a single target, `-EspResident` without `-RuntimeEsp`, MSI diagnostics with a selection) |
| `STATUS=<token> PATH=` parse | A builder that reports success in another shape, or reports twice |
| Filename check against the catalogue | A stale or renamed artifact passing as the image you asked for |
| Attestation check on the status line | A build whose reported flags contradict the variant (`READ_ONLY=1`, `PREPARATION_ONLY=1`, `UNVERIFIED_ACTUATION=1`, …) |
| PE subsystem check | An EFI application where a boot-service driver was expected, and the reverse |
| Unsigned SHA-256 | A file that changed between build and signing |
| Signing preflight inside WSL | A distribution, user or home that has no `sbsign`, no key, or a key that does not match the `.cer` - it runs once, before the first build, and prints `WSL_SIGNING_READY distro=… user=… home=…` |
| `db` enrollment | Signing with a certificate the firmware does not trust (`-SkipDbCheck` records `UNKNOWN`) |
| Cert/key/certificate match inside WSL | A key that is not the one enrolled, an expired certificate, or a `.cer` that is a different certificate |
| `sbverify` | A signature the tool cannot verify |
| Signed ≠ unsigned | A "signing" step that did nothing |
| Staging preflight | A non-USB, non-FAT32, read-only, system or boot volume; a drive letter whose identity changed |
| Backup and rollback | Replacing `BOOTX64.EFI` without a verified local copy, or leaving a half-written one |
| `-AcceptActuatingDeploy` | Staging a `stop-only` or `actuates` image by habit |
| Compile-only refusal | Staging an image that returns before it observes anything |
| DXE refusal | Staging a PE subsystem 11 driver as `BOOTX64.EFI` |

## 5. Receipts

Each signed image gets `<signed>.efi.json` and the run gets
`artifacts\signed\matrix-<utc>.json`:

```json
{
  "VariantId": "diag-topology-log",
  "CanonicalProfile": "diagnostic",
  "RequestedCanonicalProfile": "diagnostic",
  "Family": "diagnostic",
  "Switches": {"SaveLog": true},
  "ExpectedAttestations": ["READ_ONLY=1","CAN_DETACH=0","CAN_UNLOCK=0","SAVE_LOG=1","WINDOWS_HANDOFF=0"],
  "AttestationsVerified": ["READ_ONLY=1","CAN_DETACH=0","CAN_UNLOCK=0","SAVE_LOG=1","WINDOWS_HANDOFF=0"],
  "Safety": "read-only",
  "WritesGpu": false,
  "SourceSha256": "…",
  "SignedSha256": "…",
  "CertificateSha256": "…",
  "DbEnrolled": "TRUE",
  "VerifiedBySbverify": true,
  "Destination": null,
  "HardwareVerified": false,
  "Windows61742BooterProven": false
}
```

Keep the matrix file with the run. It is the evidence that the image on the stick
is the one the receipt names, and it is the only record of which variants were
skipped and why.

## 6. When something fails

| Symptom | Meaning | What to do |
| --- | --- | --- |
| `STATUS=ERROR DETAIL=… DB_ENROLLED=FALSE` | The certificate is not in `db` | Enroll it, or re-run with `-SkipDbCheck` and accept `DB_ENROLLED=UNKNOWN` |
| `DB_ENROLLED=UNKNOWN` | `Get-SecureBootUEFI` could not read `db` - not elevated, or Secure Boot off | Run the script from an elevated prompt; if Secure Boot is off, the signature is only an integrity record |
| `WSL_SIGNING_MISSING tool=…` | The preflight ran in a distribution or user without that tool, and it stops the run before any build | `apt install sbsign openssl` in that distribution, or pass `-WslDistribution`/`-WslUser` to name the one that has it |
| `WSL_SIGNING_MISSING key=…` or `cert=…` | `$HOME` of the signing user does not hold `secureboot-signing/uefi-db.key` and `.crt` | Copy both into that home (mode 600 on the key), or select the user whose home already holds them with `-WslUser`. The preflight prints the `home=` it used |
| `WSL_SIGNING_MISMATCH cer=…`, `key=…` or `cert=…` | The `.cer` you enrolled is not the DER form of the WSL certificate, the key does not match it, or it has expired | Re-export `uefi-db.cer` from the same certificate, and check `openssl x509 -noout -dates` |
| Exit 2 before anything is built | Same as `WSL_SIGNING_MISSING tool=` - the preflight is the only thing that ran | Fix the WSL side; nothing was built, signed or staged |
| WSL exit 3 | `wslpath` could not map a Windows path | Check `[automount] enabled = true` in `/etc/wsl.conf`, and that the repository is not on an unmounted drive |
| WSL exit 4 | Key, certificate or `.cer` disagree, or the certificate is expired | Re-export the `.cer` from the same certificate; check `openssl x509 -noout -dates` |
| WSL exit 5 | The unsigned image changed, or the signed file already existed | Re-run; if it repeats, something else is writing the build directory |
| `-StageDrive stages exactly one image, but N variants are selected` | `-StageDrive` was given with a selection that names more than one image (or none) | Select one configuration, for example `-Only diagnostic -StageDrive G` |
| `VARIANT_SKIPPED … REASON=NEEDS_SELECTION` | `-All` was run without a BDF | Pass `-SelectionBdf` or `-SelectionBdfs` |
| `REASON=NEEDS_EXACTLY_ONE_BDF` | Two or more BDFs given, a single-target variant selected | Re-run with `-SelectionBdf '0000:01:00.0'` |
| `REASON=NEEDS_TWO_OR_MORE_BDFS` | One BDF given, a sequence variant selected | Re-run with `-SelectionBdfs 'a,b'` |
| `REASON=NEEDS_LOCAL_BOOT_PROFILE` | The waived builders compile a real ESP identity, and the private `src/local/boot_profile.h` is missing (`boot_profile.h` stops with `Configure the reviewed ESP GUID in local/boot_profile.h`) | Copy `src\boot_profile.example.h` to `src\local\boot_profile.h` and put this machine's EFI system partition GUID in it, or build the `-RuntimeEsp` variant (`actuate-one-target-resident`, `actuate-runtime-selection-resident`). `-All` skips these and prints one `SETUP_HINT`; `-Only` on one of them is an error |
| `Builder failed for … Last builder line: …` | The builder threw, and the line after the message is what it printed last - usually the compiler's own error | Fix the compile error, or the missing setup file the line names; the builder's output is no longer swallowed |
| `Internal: the signing call returned N separate values` | The WSL call leaked output into the exit code | Upgrade the script: it captures the output and returns the code alone |
| Every variant fails with `exit Signature verification OK … 0` | The same leak, in a version before this fix | Upgrade; the signature itself succeeded |
| Every variant fails with `DETAIL=Signing Unsigned original image` | `sbsign` writes that banner on stderr, and Windows PowerShell 5.1 turned it into a terminating `NativeCommandError` because `$ErrorActionPreference` is `Stop` | Upgrade: the signing call runs with `Continue` locally and reports the exit code alone |
| The log's lines splice together or start mid-column after the first variant | `Write-Host` writes through the host's stream, which is a different writer from the redirected stdout the builders use | Upgrade: status lines go to the process stdout, so `> build.log` keeps one line per line. `*> build.log` captures every PowerShell stream if you are on an older copy |
| `STATUS=ERROR DETAIL=Argument types do not match` | Windows PowerShell 5.1 throws from `@()` wrapped around a `List[object]`; the error is reported against the enclosing hashtable, so it looked like the matrix JSON failed | Upgrade: the matrix is written from `$results.ToArray()` |
| `VARIANT_FAILED … expects` | The builder produced a filename the catalogue does not predict | Fix `docs/image_variants.json`; the script will not rename an image |
| Staged, but the machine ignores it | Secure Boot refused the image, or the firmware is booting another entry | Confirm `DB_ENROLLED=TRUE`, then check the boot order and the file hash on the stick |
| Staged, boots, black screen | A read-only diagnostic that writes nothing is not supposed to be quiet for long | Re-read the log on the stick; the console output is duplicated there when `-SaveLog` was used |

## 7. How this maps to the donor session

[`DONOR_HARDWARE_SESSION.md`](DONOR_HARDWARE_SESSION.md) defines three stages and
scores the logs they produce. The variants that belong to each:

| Session stage | Variant | Why that one |
| --- | --- | --- |
| 1, read-only contact | `diag-topology-log` | saves `GPU-TOPOLOGY-DIAGNOSTIC-nnnn.TXT`, never writes a GPU register |
| 2, preparation only | `prep-selected` | links the active source and refuses before `GuInit`. The runtime-selection form of this image does not exist yet: `src/port.h` refuses `GU_RUNTIME_SELECTION` outside the ordered selected-target mode, and `src/gpu_discovery.c` refuses that mode with a preflight-only profile |
| 3, waived actuation | `actuate-one-target` | the only admission that may write, one card, no retry |

Score the log before you believe the run:

```bash
python3 tools/verify_run_log.py /media/stick/AD102-UNLOCK/RUN0001.TXT --stage prepare
python3 tools/session_preflight.py --stick /media/stick --image out/<signed>.efi
python3 tools/donor_gate.py --readonly /media/stick/GPU-TOPOLOGY-DIAGNOSTIC-0001.TXT \
    --prepare /media/stick/AD102-UNLOCK/RUN0001.TXT \
    --ack M1_RECOVERY_TOOLKIT_READY,M2_DONOR_CARD_RECORDED,M3_LOG_MEDIUM_EMPTY,\
M4_DONOR_CARDS_RECORDED,M5_HALT_STOPS_THE_SESSION,M6_STOCK_BOOT_AFTER_EVERY_RUN
```

The last one is the go/no-go gate: a stage passing its own gates is not permission
to run the next stage. It refuses unless both earlier stages passed, every log
names the same ordered target list on cards the read-only stage enumerated, and
every manual gate is acknowledged by id. Two cards in the machine is the ordinary
case for this fork; two *different* lists between stages is not.

## 8. What this document does not claim

The combined firmware transaction and reattachment have passed physical testing
on the two recorded GPUs. The owner also confirmed recovery from a failed
transaction after a 60-second power-off. These results are specific to the tested
configuration. Signing a new image does not prove its physical operation,
numerical accuracy, resume behavior, every failure class, or Windows Booter
provenance. See the [execution map](https://github.com/akashskypatel/Windows-GPU-UNLOCK/blob/main/research/nvidia_unlock_evidence_to_execution_map.md) for current physical evidence.
