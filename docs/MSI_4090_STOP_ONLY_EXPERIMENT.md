# MSI B850 RTX 4090 — one Stop call experiment

## Situation

The original NVIDIA UEFI driver owns both GPUs in hardware diagnostic
`GPU-TOPOLOGY-DIAGNOSTIC-0029.TXT`. The RTX 4090 at `0000:01:00.0`
has **no associated GOP or console path**. The RTX 3080 at
`0000:04:00.0` has a GOP and shares the active console with the
AMD integrated GPU. Do not run a Stop experiment on the RTX 3080.

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
device rebinding, or Windows handoff.

Before the call, the program must verify:

- Three display devices with the exact original PCI identities:
  RTX 4090 `0000:01:00.0` (`10DE:2684 / 1043:88E6`),
  RTX 3080 `0000:04:00.0` (`10DE:220A / 19DA:B612`), and
  AMD iGPU `0000:0F:00.0` (`1002:13C0 / 1462:7E56`)
- The SHA-256 of all three PCI device paths matches report `0029`
- The RTX 4090 PCI option-ROM buffer is present and has 150,016 bytes
- Each device has one original `BY_DRIVER` owner
- The RTX 4090 has no firmware graphics-output path or child owner
- The RTX 3080 and AMD iGPU each have one graphics-output path
- The stored `ConOut` and `ErrOut` paths refer to the RTX 3080
  and AMD iGPU and not the RTX 4090
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
read-only. The program does **not** return to the UEFI Shell after
a Stop attempt. The owner must physically power off the system.

An intent log containing `STOP_ATTEMPT=IMMINENT` without a separate
`STOP_RETURNED=1` result log is consistent with a hang **or a file-close
error before the call**. The intent marker alone does not prove that
Stop was entered, or identify any instruction that caused a hang. A log containing
`STOP_ATTEMPT=REFUSED` means the code did not call Stop.

The current build and source checks do not prove that physical
execution will be safe. Successful execution is also **not**
permission to run unlock or flash code.

## Recommendation — Owner procedure

Only use the reviewed, approved USB/UEFI Shell method that worked
for passive diagnostic `0029`. Keep the existing BIOS, Secure
Boot settings, display cables and both NVIDIA cards unchanged.
Do not bypass image-signing checks.

On Windows, from the `universal-gpu-unlock` repository root:

1. Update from `main`:

   `git pull --ff-only https://github.com/akashskypatel/universal-gpu-unlock.git main`

2. Build the standalone experiment with the explicit risk switch:

   `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\src\Build-Msi-Stop-Observation.ps1 -AcceptUnboundedStopRisk`

3. Record the resulting executable SHA-256:

   `Get-FileHash .\src\build_stop_observation\MSI-RTX4090-STOP-ONCE-EXPERIMENT.EFI -Algorithm SHA256`

4. Copy **only** this EFI executable to the same removable EFI Shell
   volume used for the previous read-only diagnostic. Keep older logs.
   Do not make this application the default boot option.
5. Confirm you can physically reach the power switch and remove
   wall power. If this is not possible, do not start the experiment.
6. Run this file once from the UEFI Shell. Check that the displayed
   target is RTX 4090 at `0000:01:00.0`, and that all guards pass.
   Press `Y` only if you intend to make one real Stop attempt.
   Press any other key to refuse. Do not attempt the RTX 3080.
7. After the Stop call returns **or** if firmware becomes unresponsive,
   hold the physical power button until the computer is off. Disconnect
   external AC power, then restore power for a full cold start. Do not
   try to boot Windows while the test is still running.
8. Confirm whether the BIOS, both GPUs, AMD console, and Windows can
   start normally. Preserve **all** new `STOP-4090-NNNN.TXT` files
   (the intent log and, if Stop returned, a second result log), the exact
   executed EFI SHA-256, and any physical symptom or recovery failure.
   Do not rerun the experiment in the same boot or after any
   unexpected recovery result.

**Agent:** Classify the original Stop return status and after-call
ownership only from all complete new logs. Keep
`CAN_DETACH=false`, `CAN_UNLOCK=false`, `CAN_FLASH=false`,
and `RECOVERY_VERIFIED=false`. An experimental Stop result never
opens the production unlock gate by itself.
