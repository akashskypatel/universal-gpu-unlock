# Read-only PCI/GOP topology diagnostic (AD102 + GA102)

This is a standalone, **diagnostic-only EFI application**, not a GPU unlocker.
It extends the shared `gpu_inventory` selector already used by both legacy EFI
entrypoints. It checks the exact pinned B850 cards:

| Card | PCI ID | Subsystem ID | Segment:BDF |
|---|---|---|---|
| AD102 RTX 4090 | `10de:2684` | `1043:88e6` | `0000:01:00.0` |
| GA102 RTX 3080 | `10de:220a` | `19da:b612` | `0000:04:00.0` |

These are observed identities and **not generic model claims**. The result
remains `PARTIAL` or `ERROR` if PCI handles, locations, path prefixes, bridge
bus ranges, root-port types, GOP paths, or `ConOut` device paths are missing,
changed or contradictory. In particular, a Windows device number does not prove
its pre-boot PCI bus will remain the same across firmware configuration changes.

## Diagnostic observations and limitations

- Enumerates all EFI `PCI_IO` endpoints using bounded `LocateHandleBuffer`,
  `HandleProtocol`, `GetLocation`, 256-byte PCI config reads and device paths.
  Re-resolves each PCI protocol and re-reads its entire config and device path
  for stability. A read/FreePool failure makes the capture incomplete.
- Identifies exact GPUs through **the same** `GuCollectGpuInventory` and
  `GuFindPinnedGpuPair` routine as the existing source, not by array position.
- Validates nearest PCI bridge parents using complete path-prefix ancestry;
  bridge bus windows must contain the GPU bus. Each terminal PCI path node must agree with its BDF device/function. A PCIe Root Port requires PCIe
  capability Port Type 4. Roots must be distinct. Missing handles cannot be
  invented from PCI bus numbers.
- Enumerates `EFI_GRAPHICS_OUTPUT_PROTOCOL` handles and associates each GOP
  device path with its longest matching display-controller PCI path. Both
  NVIDIA GOP associations and successful PCI owner queries are required for
  a complete topology observation. A missing or multiple BY_DRIVER owner record
  for an NVIDIA GPU is not accepted as a complete owner observation. Logs
  `OpenProtocolInformation` BY_DRIVER counts and first agent handle token; an owner count is not proof of
  a particular NVIDIA driver's private GOP context or of a safe Stop operation.
- Reads `ConOut` device path(s) from the UEFI global variable **without
  writing it**; demands an AMD display GOP association and no NVIDIA ConOut
  path before declaring *observed console-path separation*. That still does
  **not** prove which physical cable/monitor is lit, GOP destructor safety,
  firmware ownership teardown, or NVIDIA hardware recovery.
- `TOPOLOGY=0` / `STATUS=COMPLETE` means only the limited **read-only
  observations** were consistent. It is **never actuation eligibility**.
  The EFI output always says `CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLR=0`.

No vendor unlock sources, `app.c`, `dxe_main.c`, `msi_release.c`, DMA, BAR0
MMIO or firmware transactions are linked. The default image only outputs
verbose Unicode text through `ConOut` and does not write a disk or NVRAM.
**Optional** `-SaveLog` creates a new, numbered
`GPU-TOPOLOGY-DIAGNOSTIC-0000.TXT` (then `-0001.TXT`, etc.) on the
application's EFI volume, refusing overwrites and file-operation failures.
This log file is the only intended persistent mutation by the diagnostic.

## Build, inspect and run

From a Windows Developer PowerShell with Visual Studio C++ Build Tools installed:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\src\Build-Gpu-Topology-Diagnostic.ps1 -SaveLog
```

Without the optional disk write, omit `-SaveLog`. The output is
`src\build_diagnostic\GPU-TOPOLOGY-DIAGNOSTIC.EFI` and a printed SHA-256.
The generated PE/COFF binary is **unsigned**. It is deliberately **not** the
repository's prebuilt `bin\BOOTX64.EFI`, is not a DXE driver, cannot be used
as an active unlock, and is not automatically installed. Verify the image's
EFI application subsystem, imports, hash, and Secure Boot policy before copying
it to a removable EFI filesystem and running it manually in UEFI Shell.
Do not enroll new Secure Boot keys merely to run a diagnostic.

On removable media with UEFI Shell, run the image by its explicitly chosen
path, e.g. `fs0:\GPU-TOPOLOGY-DIAGNOSTIC.EFI`. Filesystem mappings vary.
With `-SaveLog`, each run chooses the next unused numbered diagnostic log
(up to 10,000), preserving preceding captures. Full media, failed file
operations or log-closure failures are reported rather than ignored.
Capture all `PCI[n]`, `PCI PATH_OFFSET`, `GPU[n]`, `GOP[n]`, `CONOUT[n]`
path hex chunks and final `STATUS` lines,
and retain the SHA-256 and build arguments. Never interpret `STATUS=COMPLETE`
as permission to detach a driver or proceed to a firmware transaction.


## Optional, pinned Windows Boot Manager handoff

The normal diagnostic image **does not automatically start Windows**. A
separately built diagnostic variant can run the same read-only capture,
save/flush/close its numbered log, then invoke UEFI `LoadImage` and
`StartImage` on precisely `\EFI\\Microsoft\\Boot\\bootmgfw.efi` on a
**build-time pinned GPT Windows EFI System Partition**:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\src\Build-Gpu-Topology-Diagnostic.ps1 -SaveLog -WindowsEspGuid "<verified-Windows-ESP-unique-partition-GUID>"
```

The GUID is the selected **Windows ESP's unique GPT partition GUID**, not
the volume label, disk GUID or USB partition. Obtain and verify it locally,
for example by inspecting Windows system and ESP partitions with
`Get-Partition`. A GUID is required; there is **no** auto-select-first-disk
fallback. The header containing the selected GUID is generated under the
ignored `src/build_diagnostic` directory and **must not be committed**.

The diagnostic searches EFI Simple File System handles, rejects absent or
duplicate matching GPT partition paths, excludes its own boot volume,
validates the complete device path and the returned Windows image's
`DeviceHandle`, and makes a single direct `StartImage` attempt. It does
not write `BootOrder`, `BootNext`, NVRAM, firmware or GPU state. A passive
`STATUS=PARTIAL` or `STATUS=ERROR` does not prevent normal Windows startup
*if the diagnostic log was durably saved and closed*: these results are
observations, not evidence of altered GPU hardware.

On log save failure, ambiguous Windows ESP, image-load failure or returned
`StartImage` error, the application **does not reset or retry**. EFI
firmware controls what happens after the image returns. Recovery must remain
available via independent Windows Boot Manager boot selection. This variant
is still **unsigned** as built; any local signing, drive replacement, key
handling and USB deployment occur **outside this repository**. The
repository intentionally includes **no** build-and-sign/deploy script,
machine-specific Windows ESP GUID or signing credential.

The host-only negative checks are `tests/test_gpu_windows_path.c` and
`tests/test_gpu_windows_handoff.c`. They cover wrong/duplicate GPT paths,
malformed nodes, invalid child image binding, rejected load and pool-free
failures. These tests do not validate actual firmware Windows handoff,
vendor GOP teardown or Windows 617.42 operation.

### Physical B850 capture status and next passive test (2026-11-07)

The second supplied log, `GPU-TOPOLOGY-DIAGNOSTIC-0001.TXT` (SHA-256
`1acc874082ace67413f57b180c571969e58d1e05bbca164c3c97c78009d7b4ef`),
locates **50 PCI protocol handles**, below the strict limit of 128. The
first handle's two 256-byte configuration reads differ:
`PCI_READ_FAILURE STAGE=pci-refresh-config-changed`,
`INDEX=0`, `EFI_COMPROMISED_DATA`. Until the changed DWORDs are identified,
do not skip that handle or relax the full-read stability check.

The supplementary observer completes independently with **579** firmware
handles, **50** PCI protocols, **19** detailed display/bridge records,
and **zero errors**. It identifies the exact RTX 4090 (`01:00.0`,
`10de:2684 1043:88e6`), RTX 3080 (`04:00.0`,
`10de:220a 19da:b612`) and AMD iGPU (`0f:00.0`, `1002:13c0`).
Both captured GOP paths extend the respective NVIDIA GPU PCI paths,
and both `ConOut` paths match those GOP paths; **neither matches AMD**.
The user confirmed that no monitor was attached to the AMD motherboard
output in this run. This may explain the current NVIDIA preboot console
paths, but does not prove that attaching an AMD display will change them.

The next *read-only* build reports bounded `PCI_CONFIG_DRIFT_DWORD` records
(`OFFSET`, `FIRST`, `SECOND`, `XOR`, maximum 12 displayed) and the
full changed-DWORD count, while still returning `STATUS=ERROR` on any drift.
The user subsequently connected a display to the AMD motherboard output
and observed BIOS setup rendering on the iGPU. This validates the BIOS
setup display path, but not the UEFI application's `ConOut` variable.
Keep the NVIDIA monitor cables unchanged, **cold boot** the updated
read-only EFI, and compare console paths and config drift. A different monitor setup is a documented changed condition,
not a controlled replication of the previous configuration.

`WINDOWS_HANDOFF=REQUESTED` and a saved log do **not** by themselves prove
the handoff succeeded. A successful passive report would not establish safe
NVIDIA DriverBinding.Stop, recovery, signed 617.42 Booter or SEC2
compatibility. Active unlocking remains blocked.

### Independent passive PCI evidence on strict-collector failure

The original strict topology assessment still requires a complete bounded set of at most **128 PCI records**. Earlier USB evidence gave `PCI_CAPTURE=0 PCI_NODES=0`, so failure could not be attributed to missing GPUs, a count limit, or a first-handle error.

The supplementary, diagnostic-only observer borrows the **read-only reporting method**, not the GPU mutation/teardown code, from [Windows-GPU-UNLOCK's passive handle collector](https://github.com/akashskypatel/Windows-GPU-UNLOCK/blob/main/internal-boot/boot/diagnostic.c). When the strict capture fails, it independently snapshots up to **4,096 EFI handles** using `AllHandles`, probes for `EFI_PCI_IO_PROTOCOL` with `HandleProtocol`, reads the first 64 PCI configuration bytes, obtains PCI location, captures a bounded device path, and inspects BY_DRIVER open records. Each allocated firmware pool is released. No GPU MMIO, PCI configuration writes, ROM reads, driver detachment, reset or UEFI variable writes are introduced.

The log marks the secondary report `PCI_OBSERVATION_ONLY=1 NOT_TOPOLOGY=1 CAN_DETACH=0 CAN_UNLOCK=0`, followed by `PCI_OBS_SUMMARY`, `PCI_OBS_STATUS`, and selected `PCI_OBS[]` display/bridge identity/path/ownership records. It records at most 96 relevant endpoints and explicitly marks truncation or a failed UEFI call. Paths or owner identities that could not be observed remain unknown. The report never changes `PCI_CAPTURE`, `EXACT_PAIR`, or the topology result and cannot authorize active GPU firmware operations. It is independent evidence useful for *diagnosing* a failed strict collector.

[GitHub Actions run 42425559424](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42425559424) **PASS**: `tests/test_gpu_pci_observation.c` contains **30 assertions**, run with GCC, Clang and GCC UBSan; existing inventory/topology/handoff host checks also pass, and MSVC builds the diagnostic EFI with/without Windows handoff. The CI handoff variant uses a synthetic, non-deployable ESP GUID. These are host mock and build results, **not a successful physical B850 inventory**, real signed USB execution, or proof of driver release.

## Host verification and fault boundaries

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic -Isrc tests/test_gpu_topology.c src/gpu_topology.c src/gpu_inventory.c -o /tmp/test_gpu_topology && /tmp/test_gpu_topology
```

The host tests validate reversed PCI enumeration, exact card pins, wrong PCI
IDs/BDF/subsystem, duplicated handles/BDF/protocols, absent or shared root
ports, bridge windows, broken parent paths, malformed GOP and ConOut paths,
NVIDIA ConOut ownership, missing AMD GOP and console observations, and bounds.
They cannot prove genuine firmware methods, GOP driver safety or display
ownership. See `Windows-GPU-UNLOCK`'s read-only `research/pci-decode/efi-capture.*`,
`internal-boot/boot/probe-parent.c` and `research/gop/nvgop_stop_model.py`
for the basis of the ancestry observations and for modeled Stop-related
failure boundaries. The latter model is host-only and must **not** be used as
proof that `DisconnectController()` is safe.

## Current status

- Pure topology host checks are implemented. EFI source and isolated build
  recipe are provided; physical execution remains **not tested**.
- [GitHub Actions run 42422191256](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42422191256) passed the 21-path and 17-handoff negative checks under GCC/Clang (handoff also GCC UBSan); Windows MSVC compiled the opt-in handoff variant using a deliberately **synthetic, non-deployable Windows ESP GUID**, then rebuilt the console-only variant for artifact upload. No signing, USB boot or real Windows handoff was exercised.
- No changes are made to the legacy active entrypoint's strict multi-GPU
  refusal. No dual-GPU write-enabled path is introduced.
- Revalidate all build and host results against a specific commit and hardware
  capture before treating any output as an implementation qualification.
