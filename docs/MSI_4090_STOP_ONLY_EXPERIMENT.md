# MSI B850 RTX 4090 — one Stop call experiment

## Situation

The original NVIDIA firmware driver was initially bound on both GPUs.
The latest physical `STOP-4090-0006/0007.TXT` confirms that, with
**one monitor on AMD iGPU only**, the RTX4090 and RTX3080 have no
firmware graphics-output paths. The **single original RTX4090 Stop
returned success and removed its PCI driver owner**, with AMD and
RTX3080 owners unchanged. The old observer then halted deliberately.
Do not run a Stop experiment on the RTX3080.

The stock NVIDIA RTX 4090 Stop callback can write GPU registers and
wait in a loop without a verified time limit. The owner explicitly
accepts this risk and has designated a complete physical power-off
and power restoration as the attempted recovery method. This method
is **not verified** to recover every possible fault.

## Background

The normal release function, `GuMsiRelease()`, continues to reject
AD102 and GA102. The experimental program is **separate** from
the standard unlock program and passive GPU diagnostic.

The experiment calls only the original RTX 4090's
`DisconnectController(target, original_nvidia_driver, NULL)`.
Firmware may invoke the original NVIDIA Stop callback. The program
does not attempt firmware unlock, flash, GPU reset, a second card,
device rebinding. The default build still halts after Stop; a
**separate, explicitly opted-in executable** can prepare the original
Windows Boot Manager from a pinned Windows EFI System Partition (ESP)
and start it only when all post-Stop checks pass. This path has
**not been tested on physical hardware**.

Before the call, the program must verify:

- Three display devices with the exact original PCI identities:
  RTX 4090 `0000:01:00.0` (`10DE:2684 / 1043:88E6`),
  RTX 3080 `0000:04:00.0` (`10DE:220A / 19DA:B612`), and
  AMD iGPU `0000:0F:00.0` (`1002:13C0 / 1462:7E56`)
- The SHA-256 of all three PCI device paths matches report `0029`
- The RTX 4090 PCI option-ROM buffer is present and has 150,016 bytes
- Each device has one original `BY_DRIVER` owner
- The RTX 4090 has no firmware graphics-output path or child owner
- The AMD iGPU has one graphics-output path; the RTX 3080 may have zero
  (AMD-only display configuration) or one (previous mixed-console setup)
- The stored `ConOut` and `ErrOut` paths refer only to AMD with one
  AMD GOP, or to exactly AMD and RTX 3080 when both GOPs exist.
  Neither may refer to the RTX 4090 or an unidentified controller.
- The original RTX 4090 loaded driver image is 162,016 bytes and its
  SHA-256 equals
  `0fbd44f3455d4df5ade4bc738dbbc73fbd213487a7456a953af9e8ccbc14c2ba`
- The original Driver Binding Stop pointer matches that exact image
- A new intent log on the application's removable volume is flushed
  and **closed** before the call. A separate result log is created only
  if Stop returns

There is also a second explicit key confirmation. Only `Y` can
start Stop. Any failed guard must refuse before Stop.

## Assessment

The test can report that the original Stop returned, failed, or
did not return before manual power removal. It cannot establish
full GPU recovery or the Windows 617.42 secure Booter acceptance.

A post-call result may report the PCI owner, RTX 3080 and AMD
driver owners, GOP paths, and console variables. These checks are
read-only. Neither executable returns to the UEFI Shell after a Stop
attempt. The default one-card executable requires a manual power-off.
The optional Windows-handoff executable tries the pinned Windows
Boot Manager only after Stop succeeds and the PCI/console checks pass.
If it refuses or returns, the program halts and requires a manual
power-off. A completed Windows boot is not proof of a safe unlock.

An intent log containing `STOP_ATTEMPT=IMMINENT` without a separate
`STOP_RETURNED=1` result log is consistent with a hang **or a file-close
error before the call**. The intent marker alone does not prove that
Stop was entered, or identify any instruction that caused a hang. A log containing
`STOP_ATTEMPT=REFUSED` means the code did not call Stop.

The current build and source checks do not prove that physical
execution will be safe. Successful execution is also **not**
permission to run unlock or flash code.

## Current hardware result — original RTX4090 Stop physically succeeded

The owner used an ordinary restart, one monitor on the AMD iGPU,
and no display or dummy plug on either NVIDIA card. Complete
original evidence:

- `STOP-4090-0006.TXT`: 5,264 bytes, SHA-256
  `eb46cf91f82602f7724be84d1fae33420820f1d685701104140dbf3e0e64756a`
- `STOP-4090-0007.TXT`: 1,891 bytes, SHA-256
  `d7ef21d895906afb954f369e9188f6f6c9ad347db24ce87f49490a3b15579f14`

Before Stop, the RTX4090 original driver-image SHA-256 and Stop
pointer match; the exact GPU identities, one original PCI owner per
GPU, no target child, one AMD GOP, and only AMD `ConOut/ErrOut` all
pass. The pre-call result file records `STOP_ATTEMPT=IMMINENT` and
`STOP_SERVICE=DisconnectController DRIVER=PINNED_ORIGINAL IMAGE=SHA_MATCH`.

The second, separately written file establishes
`STOP_RETURNED=1 EFI_STATUS=0`, a target `BY_DRIVER` count of
zero (one remaining ordinary protocol reference), unchanged original
RTX3080 and AMD `BY_DRIVER` owners, AMD GOP alone, and exact
post-Stop `ConOut/ErrOut` success. It ends with
`NO_WINDOWS_HANDOFF=1 POWER_OFF_REQUIRED=1 DO_NOT_RETRY=1`.
This last record is a **deliberate programmed halt**, not a failed
NVIDIA Stop callback. Following manual power-cycle restart, Windows
`nvidia-smi -L` showed **both original NVIDIA GPU identities**.
This is evidence of basic cold-start Windows recognition, not a
complete post-Stop same-boot handoff, DMA safety, signed Windows
firmware-loader acceptance or repeatable recovery guarantee.

## Experimental automatic Windows handoff — separate opt-in build

The implementation reuses the repository's existing pinned Windows
Boot Manager loader; it does **not** change `BootNext`, `BootOrder`,
device driver packages, the original GPU ROM, or any production
release gate. The new executable is named
`MSI-RTX4090-STOP-WINDOWS-HANDOFF-EXPERIMENT.EFI` so it cannot be
mistaken for the original stop-and-halt image.

The build requires both `-AcceptUnboundedStopRisk` and
`-AcceptWindowsHandoffRisk`, together with `-WindowsEspGuid`
set to the **current Windows EFI System Partition unique GPT
partition identifier**. The owner must independently verify the
current identifier. The previously reviewed candidate is
`a005858c-4487-4ee2-976e-1710ff484c31`; do not assume a Windows
reinstall or disk change preserved it. Use the previously approved
signed USB execution route and keep this binary off the default
firmware boot list.

After deliberate `Y` confirmation, the application uses the
original firmware `LoadImage` path to prepare the pinned
`\\EFI\\Microsoft\\Boot\\bootmgfw.efi` **before Stop**, without
running it. A missing/ambiguous ESP, USB match, invalid image or
failed platform image-authentication check **refuses before Stop**.
The code then repeats all original target, peer, GOP, console and
original NVIDIA driver checks, saves/closes the intent log, and
calls original `DisconnectController` **once** for RTX4090 only.

After Stop, the application admits handoff only if: the original
Stop returned `EFI_SUCCESS`; the target PCI protocol has **no**
`BY_DRIVER`, child-controller, exclusive or unknown opener; the
unchanged original RTX3080 and AMD driver owners remain; the AMD
GOP/console checks pass; and the second result log is flushed and
closed. Only then does it call `StartImage` for the already prepared
Windows Boot Manager. New records are
`WINDOWS_BOOT_MANAGER_PREPARED`,
`POST_STOP_TARGET_PCI_UNBOUND`,
`WINDOWS_HANDOFF_POST_STOP_ELIGIBLE`, and
`WINDOWS_HANDOFF=STARTING_PINNED_BOOT_MANAGER`.

**Verified build status:** The [final GitHub Actions run 43662849749](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/43662849749)
completed with host **PASS** and Windows UEFI compilation **PASS**.
The host checks invoke the real pinned Windows loader functions and
the actual post-Stop PCI-unbound C function in 15 positive/negative
cases, including ordinary opens, duplicate/malformed owners,
child-controller and exclusive records, and failed firmware services.
The workflow compiles the separate opt-in image with an example GPT
identifier but neither executes it on firmware nor publishes it as
a signed physical diagnostic. **Same-boot Windows handoff remains
untested on this hardware.**

**Important limits:** `StartImage` may not return if Windows boots.
The implementation does not prove that firmware, Boot Manager or
Windows will leave the RTX4090 in precisely its post-Stop state.
A Windows boot can fail, trigger recovery, or require a full hard
power-off. If the Boot Manager returns, the observer persists its
returned status when possible and **halts without retrying, connecting
a GPU driver or returning to the Shell**. This remains a user-approved
one-card hardware experiment, not production Windows handoff or
unlock enablement. Have physical AC disconnect, Windows recovery
media and any applicable BitLocker recovery key ready before using it.

## Recommendation — Owner procedure: optional automatic Windows handoff

Use the original signed EFI execution route that was approved for the
previous diagnostic. Do not change BIOS security, BootOrder, GPU drivers,
display cables, or the default boot device. Do not make the experimental
EFI program a permanent boot option. Keep one monitor on the AMD iGPU.

1. On Windows, identify the actual Windows EFI System Partition (ESP)
   and its **unique GPT partition GUID** (not its partition type GUID).
   The following command lists ESP candidates:

   `Get-Partition | Where-Object { $_.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' } | Select-Object DiskNumber,PartitionNumber,Guid,Size`

   Identify the Windows system disk, not the removable USB. Previous
   approved evidence used `a005858c-4487-4ee2-976e-1710ff484c31`
   but this value must match the current installed Windows ESP.

2. From the `universal-gpu-unlock` repository root, update the source:

   `git pull --ff-only https://github.com/akashskypatel/universal-gpu-unlock.git main`

3. Build the separately named EFI program with the **verified** unique
   Windows ESP GUID and both explicit risk switches:

   `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\src\Build-Msi-Stop-Observation.ps1 -AcceptUnboundedStopRisk -WindowsEspGuid 'a005858c-4487-4ee2-976e-1710ff484c31' -AcceptWindowsHandoffRisk`

   If the current Windows ESP GUID is different, replace the above
   example value before building. Do not run an image built with a
   stale, ambiguous, USB or unknown ESP identifier.

4. Record the source commit and hash of the **new handoff executable**:

   `git rev-parse HEAD`

   `Get-FileHash -LiteralPath '.\src\build_stop_observation\MSI-RTX4090-STOP-WINDOWS-HANDOFF-EXPERIMENT.EFI' -Algorithm SHA256`

5. Sign and stage **only the handoff-named executable** by the already
   approved removable USB/UEFI method. Compare its staged SHA-256
   with the built SHA-256. Preserve all existing numbered USB logs.
   Confirm that the Windows recovery media, any BitLocker recovery
   key, physical power switch and removable AC power are accessible.

6. Launch the handoff-named EFI program from the trusted UEFI Shell.
   Before Stop, the program must show the exact original RTX4090 and
   AMD/peer identity checks, a prepared Windows Boot Manager, and
   the signed original NVIDIA driver match. A deliberate `Y` now
   authorizes **one RTX4090 firmware disconnect and, only if safe
   post-Stop checks pass, automatic `StartImage` for Windows**.
   A key other than `Y` cancels before Stop. Do not attempt any
   RTX3080 Stop or repeated Stop in the same boot.

7. If Windows starts from this same boot, record `nvidia-smi -L`,
   both NVIDIA device statuses, display operation and any driver
   errors, and preserve all new `STOP-4090-NNNN.TXT` files. A
   successful boot is not proof that the original signed firmware
   unlock, DMA safety or production recovery has succeeded.

8. If the original disconnect hangs, Windows Boot Manager refuses or
   returns, or firmware becomes unresponsive, hold physical power
   until OFF, disconnect AC, then restore power. Do not allow a
   retry in the same boot. Preserve the logs, staged image hash and
   exact observed failure. Recovery is still not guaranteed.

**Agent:** Assess physical `WINDOWS_BOOT_MANAGER_PREPARED`,
`POST_STOP_TARGET_PCI_UNBOUND`,
`WINDOWS_HANDOFF_POST_STOP_ELIGIBLE`, and the attempted
`WINDOWS_HANDOFF=STARTING_PINNED_BOOT_MANAGER` transition. The
previous stop-and-halt image is unchanged in behavior. Maintain
`CAN_DETACH=false`, `CAN_UNLOCK=false`, `CAN_FLASH=false`, and
`RECOVERY_VERIFIED=false` for the production system. An isolated
firmware PCI owner disconnect is not proof of a GPU unlock.
