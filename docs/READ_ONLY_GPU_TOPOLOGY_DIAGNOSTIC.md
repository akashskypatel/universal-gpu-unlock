# Dynamic read-only NVIDIA GPU and PCI/GOP topology diagnostic

## MSI B850 Gaming Plus WiFi (MS-7E56) ROM variable capture

The MSI-only passive diagnostic reads the original firmware variables
ROM_CMN and CSM_CMN before Windows starts, without a separate UEFI Shell.
From the repository root, build on Windows with MSVC:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\src\Build-Gpu-Topology-Diagnostic.ps1 -SaveLog -MsiB850RomVariables
```

Output: `src\build_diagnostic\MSI-B850-ROM-VARIABLES-DIAGNOSTIC.EFI`.
The generic diagnostic does not read these MSI variables. The MSI-only
build requires a saved log and forbids Windows handoff and GPU selection.

Before either variable read, the current passive PCI inventory must contain
exactly one AMD iGPU `1002:13C0`/subsystem `1462:7E56`, one ASUS
RTX 4090 `10DE:2684`/subsystem `1043:88E6`, and one ZOTAC RTX 3080
`10DE:220A`/subsystem `19DA:B612`. Missing, duplicate, extra
NVIDIA or mismatched platform identities refuse the variable reads.
These PCI identities are a system fingerprint, not a validated SMBIOS
motherboard model or a verified installed BIOS version.

The reader calls only UEFI `GetVariable` for `ROM_CMN` and `CSM_CMN` under
vendor GUID `97CDC6BE-7792-4782-BA6D-E6E3BB56ACD8`. It logs the
exact status, byte count, attributes and raw hexadecimal data, with a
1,024-byte bound. A missing or oversized value is reported without
guessing its contents. Original `ROM_CMN` size `0x147` (327) is checked
but no unverified RTX slot or mode interpretation is applied.

The same numbered `GPU-TOPOLOGY-DIAGNOSTIC-NNNN.TXT` USB log also
contains the normal PCI, GOP and console ownership report. Use only the
previously reviewed standalone passive boot procedure. Do not change
BootOrder, CSM, PEG/IGD, graphics detection, GPU ownership or firmware
variables. No `SetVariable`, PCI write, NVIDIA Stop, reset, flash
or unlock function is linked or called. This is not an active-release test.


This standalone, **non-mutating UEFI application** discovers hardware without a
hardcoded motherboard, NVIDIA GPU model, subsystem ID, BDF, or fixed two-card
layout. It inventories NVIDIA display/3D controllers, PCI-to-PCI bridges,
and independent non-NVIDIA display paths, including AMD and Intel consoles.

Dynamic topology supports 1–16 NVIDIA devices and multiple GPUs on the same
root port. The scan examines up to 4096 PCI handles and retains at most 128
display/bridge nodes; exceeding either limit fails closed. The generic
diagnostic requires no target-specific build setting.

## Physical B850 console ownership and GOP-driver mapping — latest `0011`/`0012`

**Corrected cable configurations:** Earlier `0009` and `0010` were incorrectly described as *all three GPUs connected*; the operator confirms that only AMD iGPU and RTX 4090 monitors were connected. Their missing 3080 GOP cannot be treated as an unexplained three-display firmware behavior. `0011` is the **AMD iGPU monitor only**; `0012` is the actual **AMD iGPU + RTX 4090 + RTX 3080 monitors** configuration.

| Physical passive observation | `0011`: AMD-only display | `0012`: all three displays |
|---|---|---|
| PCI scoped capture / selected target list | 50 total PCI, 19 relevant, both BDFs identified | Same |
| GOPs AMD / RTX 4090 / RTX 3080 | **1 / 0 / 0** | **1 / 1 / 1** |
| Configured `ConOut` AMD / NVIDIA paths | **1 / 0**, `NVIDIA_CONOUT_BLOCKER=0` | **1 / 2**, `NVIDIA_CONOUT_BLOCKER=1` |
| Configured `ErrOut` AMD / NVIDIA paths | **1 / 0**, `ERROR_CONSOLE_ISOLATION=INDEPENDENT_OBSERVED` | **1 / 2**, `ERROR_CONSOLE_ISOLATION=CONFLICT` |
| Strict topology | `INCOMPLETE` (missing NVIDIA GOPs) | `CONFLICT` (configured NVIDIA consoles) |
| NVIDIA PCI driver binding → loaded image | **Both linked** | **Both linked** |

**Raw artifact fingerprints:** `GPU-TOPOLOGY-DIAGNOSTIC-0011.TXT` = 9,548 bytes, 121 lines, SHA-256 `cda76fe9179aee1c1245bcc1968d0b1c3dc96710570d74c812ba5d7b627dfaa1`. `GPU-TOPOLOGY-DIAGNOSTIC-0012.TXT` = 11,025 bytes, 145 lines, SHA-256 `83e51f5f0bb60b7acbd403538aade4cc10c68815807b57c49656999f2aac6b76`. Both report `READ_ONLY=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLR=0`, `HARDWARE_TESTED=0` (no GPU **mutation**), `PHYSICAL_DISPLAY_OWNER=UNKNOWN`, and `SELECTION_SEQUENCE=PASSIVE_IDENTIFIED COMPLETED=2`. The cable descriptions are operator-supplied; the EFI confirms paths and GOP inventories, not physical connectors.

**New physically validated image associations, from both logs:** RTX 4090 `0000:01:00.0` `BY_DRIVER=1 OPENERS_KNOWN=1 BINDING_STATUS=0 LOADED_IMAGE_STATUS=0`, EFI FilePath `nvgop-ad1xx`, `ImageSize=162016` bytes; RTX 3080 `0000:04:00.0` has the same read-only owner/protocol status, EFI FilePath `nvgop-ga1xx`, `ImageSize=158432` bytes. `NVIDIA_OWNER_SOURCE` device paths agree with each card's PCI ancestry. This is actual physical passive exercise of the [driver-owner-reporting code](https://github.com/akashskypatel/universal-gpu-unlock/commit/e1fc407a2573611ffde5c3c29b61318bb800cd15), beyond its earlier [successful host/EFI CI](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42546463301). **Loaded Image properties do not authenticate original VBIOS ROM bytes, driver signing, code identity, child ownership or the safety of proprietary `DriverBinding.Stop`.**

**Decision:** `0011` **passes passive configured `ConOut` and `ErrOut` isolation** for AMD-only preboot cabling but fails the *independent* strict full-topology condition requiring NVIDIA GOPs. `0012` shows valid GOPs for all three GPUs but has a **real firmware-console configuration conflict** because both NVIDIA paths appear in `ConOut` and `ErrOut`. Neither represents a verified post-Stop console. Do not rewrite `ConOut`/`ErrOut` NVRAM, edit `DriverOrder`, detach GOPs or relax `GuTopoAssess` to make either label `COMPLETE`. Physical GPU mutation, firmware signing/acceptance and recovery are still unauthorized (`CAN_DETACH=false CAN_UNLOCK=false CAN_FLASH=false RECOVERY_VERIFIED=false`).

**Next engineering work, not a repeat cable test:** reconcile current-boot GOP handles, PCI protocol opener/child edges and **authenticated exact driver-image bytes** against older Windows DIAG07/DIAG10 private-context evidence, using stable BDF/subsystem/source fingerprints rather than reusing EFI handle tokens across boots. Then build and verify a fail-closed offline model of selective NVIDIA `Stop`, partial failures and nonreturning calls before any separately approved active test. If using all three displays during preboot remains necessary, a firmware policy that genuinely clears **both** configured NVIDIA `ConOut` and `ErrOut` paths would require separate passive verification; no such configuration has yet been demonstrated.

**Rebuild command for future source changes only** (run from repository root with MSVC installed; do not invoke active app or DXE):

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\src\Build-Gpu-Topology-Diagnostic.ps1 -SaveLog -SelectionBdfs '0000:01:00.0,0000:04:00.0'
```

## Physical `0015`: NVIDIA GOPs share GraphicsConsole and ConSplitter aggregate console

**Observed result; no repeat of the old capture needed:** With all three monitors connected, the standalone read-only EFI successfully resolves both NVIDIA PCI `BY_DRIVER` drivers, both GOP `BY_DRIVER` consumers (`GraphicsConsole`, FFS `43E7ABDD-E352-4CFB-A230-4CDC1D350E5C`, 18,464 B) and both **TextOut `BY_DRIVER` consumers** (`ConSplitter`, FFS `628A497D-2BF6-4264-8741-069DBD3399D6`, 28,160 B). Both NVIDIA TextOut handles have **one `BY_CHILD_CONTROLLER` opener by ConSplitter** to `0x806acc98`, exactly the reported live console/error handle. This is a shared graphical/text console structure, not two independent detach-safe output graphs. Driver Binding and Loaded Image queries pass, but no driver-image vendor signature is verified.

**Important interpretation:** `NVIDIA_GOP_GRAPH` still shows zero PCI `BY_CHILD_CONTROLLER` openers; `NVIDIA_TEXTOUT_EDGE` shows **one TextOut `BY_CHILD_CONTROLLER` relationship per NVIDIA path**. Both `SOURCE=GOP_OPEN_CONTROLLER` and `SOURCE=GOP_HANDLE` records in `0015` identify the SAME actual handle and report the same opener graph, so each pair is a duplicated observation, **not an extra child**. A `BY_DRIVER` record's `ControllerHandle` alone is not a child edge; check ATTR `0x8`. `ConOut` and `ErrOut` include both NVIDIA GPUs (`CONFLICT`) despite the independent AMD GOP. No physical post-Stop AMD-console survival is proven.

**Current binary comparison:** `0015` mapped NVIDIA GOP `.text` SHA-256s match previously pinned captures on 4090 (`77632902f0ceb0ba2848200f396ab61eacd7910e9cce7c535f65d6cdbe2b6c32`) and 3080 (`615b37a708b1894e18d7fa60cfcdacaa4d8b598863a10b8103d231a595847b2b`). Whole loaded-image hashes vary across boots, and original signed ROM/BIOS provenance remains unverified.

**Updated next evidence, after offline source reconciliation:** The Windows fork's `internal-boot/boot/prepare-console.c` has been statically reviewed: it classifies one AMD plus one RTX, pins historical AMD `1002:164e / 1043:8877` instead of the B850's observed `1002:13c0 / 1462:7e56`, and cannot safely manage both NVIDIA outputs' shared GraphicsConsole/ConSplitter lifetime. It remains **ineligible** for a dual-GPU detachment attempt. For a possible *firmware-only* resolution, inspect—not change—the current MSI B850 BIOS version and `Initiate Graphic Adapter` value and search any other console/display-selection options. MSI describes IGD as the **primary** boot adapter only; it is **not** proof of AMD-exclusive configured `ConOut`/`ErrOut`. Physical `0015` does not record the BIOS setting. No further identical EFI capture, `GuTopoAssess` change, `DisconnectController` or `DriverBinding.Stop` is authorized.

**Source verification:** [read-only TextOut collection `c684f93`](https://github.com/akashskypatel/universal-gpu-unlock/commit/c684f9344ce5d9c0d9de4d95287013e0ba7fa91a) passed [host and MSVC EFI CI 42554123054](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42554123054) and was **physically exercised in `0015`**. Raw `GPU-TOPOLOGY-DIAGNOSTIC-0015.TXT`: 18,468 bytes, 224 lines, SHA-256 `482721c373679054631bee69bf43c7cbae5bbcede63690927c9a9dae0cc443e6`. `CAN_DETACH=false CAN_UNLOCK=false CAN_FLASH=false RECOVERY_VERIFIED=false`.

## Optional read-only selection by PCI location

After observing the actual segment:BDF on a machine, an operator may build
a separately selected diagnostic. This does not constrain which *models*
are discoverable or qualify any firmware transactions. The BDF identifies
one GPU in the current snapshot; its observed PCI/device/subsystem identity
is recorded, and all other NVIDIA GPUs remain visible as non-targets.

Generic diagnostic build:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\src\Build-Gpu-Topology-Diagnostic.ps1 -SaveLog
```

Optional explicit BDF selection, with a **synthetic example address**:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\src\Build-Gpu-Topology-Diagnostic.ps1 -SaveLog -SelectionBdf '0000:09:02.0'
```

The example produces `src\build_diagnostic\GPU-SELECTION-0000-09-02-0-DIAGNOSTIC.EFI`.
An ordered, read-only **multi-target** diagnostic can be compiled with
`-SelectionBdfs` and 2–16 comma-separated PCI locations (the example below uses two):

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\src\Build-Gpu-Topology-Diagnostic.ps1 -SaveLog -SelectionBdfs '0000:09:02.0,0001:2A:03.1'
```

The example PCI locations are synthetic; substitute the *current* target
system's observed PCI addresses. A one- or two-target binary name contains
the locations in order; for three or more the name carries the target count
and a deterministic SHA-256 prefix to avoid excessively long filenames.
The ordered locations remain recorded in the selection log. Duplicate locations, invalid PCI syntax, or a missing
target refuse the sequence, with no partial selection. At runtime, the
diagnostic obtains **one** immutable PCI/GOP/ConOut snapshot, resolves both
requested NVIDIA devices before processing, and emits sequential
`SELECTION_STEP[n]` identity/ancestry/GOP/owner
reports followed by `SELECTION_SEQUENCE` summary. There is no repeated
hardware initialization or separate acquisition between steps. A topology
conflict is reported independently and still prevents an overall EFI success.
This is **not** sequential NVIDIA secure firmware execution, GPU driver Stop,
or unlock authorization. `GuDiscoverSingleGpu` still refuses the
write-enabled multi-NVIDIA entrypoints.

An absent or incorrect location gives `SELECTION_SEQUENCE=REFUSED`,
retained in the numbered log when `-SaveLog` is used. For the existing
single-target build, the legacy `SELECTION_VERDICT=REFUSED` or
`SELECTION_VERDICT=PASSIVE_IDENTIFIED`, `PASSIVE_SELECTION`, and
`NON_TARGET_NVIDIA_COUNT` fields remain available for log consumers.
A multi-target build uses ordered `SELECTION_STEP[n]` and the aggregate
`SELECTION_SEQUENCE`; it does not claim any GPU firmware was executed.
Any `TOPOLOGY_VERDICT=CONFLICT` remains a separate console/ownership finding.
The selection build rejects `-WindowsEspGuid`; neither the selection binary
nor the generic topology EFI links the GPU unlock/DMA/FLR/vendor Stop engine.

**Verification:** [Final CI run 42525760457](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42525760457) passed 72 generic topology checks (GCC/Clang/UBSan), 73 inventory/ordered-selection checks (GCC/Clang/UBSan), 39 active discovery adapter mocks (GCC/Clang) and selected-entrypoint hard-refusal mocks for one and two targets. MSVC compiled the untargeted diagnostic and one-, two- and three-target passive EFI images, as well as separately gated active app/DXE preparation-only binaries. The selected build CLI also rejects duplicate BDFs. [Unsigned selected diagnostic artifacts](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42525760457/artifacts/13635163109). These are source/host/build checks, **not physical runs** of the generalized EFI or evidence of safe driver Stop, firmware execution, or recovery. The unselected legacy active entrypoints still refuse any multi-NVIDIA configuration before `GuInit`.

## Shared active-preparation boundary

The same `src/Gpu-Selection-Build.ps1` validator is used by this passive
diagnostic and the separately built **non-diagnostic active app/DXE
preparation-only images**. It validates 1–16 ordered, distinct PCI segment:BDF
locations, independent of GPU model and motherboard. The active app/DXE
preparation images do **not** use this passive diagnostic's full PCI/GOP/ConOut
collector; their common UEFI discovery routine currently records bounded
PCI identity/inventory only and refuses any GPU actuation.

See [Active GPU preparation](ACTIVE_GPU_PREPARATION.md). This is a distinct
source/verification boundary from the previous passive diagnostic captures.

## Diagnostic observations and limitations

- Enumerates all EFI `PCI_IO` endpoints using bounded `LocateHandleBuffer`,
  `HandleProtocol`, `GetLocation`, 256-byte PCI config reads and device paths.
  Re-resolves each PCI protocol and re-reads its entire config and device path
  for stability. A read/FreePool failure makes the capture incomplete.
- Identifies exact GPUs through **the same** `GuCollectGpuInventory` and
  `GuFindGpuAtLocation` (on optional selection); GPU models and arbitrary
  array positions are not used as discovery criteria.
- Validates nearest PCI bridge parents using complete path-prefix ancestry.
  **Every immediate bridge-to-child bus edge**, including an intermediate
  bridge's own BDF bus inside its parent window, must be valid. The leaf GPU's
  bus must also fit all ancestor bridge windows; checking only that leaf bus
  previously permitted a false COMPLETE for a malformed nested chain.
  Every terminal PCI path node must agree with its BDF device/function.
  A PCIe Root Port requires PCIe capability Port Type 4; root ports may
  be shared. Missing handles cannot be invented from PCI bus numbers.
  [Host + Windows EFI CI `42875828284`](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42875828284)
  verified the valid two-bridge chain and two invalid parent-bus cases.
  This is passive PCI topology evidence, not verified GPU detachment.
- Enumerates `EFI_GRAPHICS_OUTPUT_PROTOCOL` handles and associates each GOP
  device path with its longest matching display-controller PCI path. The
  capture now records boot-local GOP handle and protocol-interface identity
  as well as its device path. After the configured console observations, the
  EFI application **re-enumerates every GOP** and requires the same complete
  handle/protocol/path set, independent of ordering. A missing/added/rebound
  handle, altered path, duplicate handle, failed protocol read or FreePool
  refuses `COMPLETE`; `GOP_CAPTURE_RECHECK` logs this evidence.
  This is a bounded **non-atomic** observation, not a guarantee the GOP or
  its backing NVIDIA driver will remain stable after the check.
  Each discovered NVIDIA GPU's GOP association and PCI owner observations
  are required for a complete topology report. A missing or multiple BY_DRIVER owner record
  for an NVIDIA GPU is not accepted as a complete owner observation. Logs
  `OpenProtocolInformation` BY_DRIVER counts and first agent handle token; an owner count is not proof of
  a particular NVIDIA driver's private GOP context or of a safe Stop operation.
- Reads **configured** `ConOut` and `ErrOut` paths independently from the
  UEFI global variables without writing them, then **re-reads both variables**
  after the other console queries. The complete ordered path sets must match,
  including all path bytes and the number and order of instances; unreadable,
  malformed, or changed rechecks fail closed. New diagnostic fields
  `CONOUT_RECHECK_VARIABLE`, `ERROUT_RECHECK_VARIABLE`, and
  `CONSOLE_PATHS_RECHECK CONOUT_STABLE=... ERROUT_STABLE=...` record the
  bounded check. An incomplete PCI/GOP/ConOut capture also precludes
  independent `ErrOut` classification. These are sequential **non-atomic**
  observations; configuration can still change after the last read.
  `ConOutDev` lists *possible* output paths, not installed console ownership. `GuTopoAssess` still
  evaluates the structurally validated PCI/GOP/`ConOut` graph, and the EFI
  logs its result as `GRAPH_ONLY_TOPOLOGY_VERDICT`. The final
  `TOPOLOGY_VERDICT` (and return status) now also applies
  `GuTopoCombinedConsoleVerdict`: a NVIDIA-owned or ambiguous `ErrOut`
  forces `CONFLICT`, while an unavailable/unclassified, unmatched, or
  same-controller-GOP-unproven `ErrOut` prevents `COMPLETE`.
  Each `ConOut` and `ErrOut` path classified as independent must have a
  GOP associated with **that exact PCI display controller**; a GOP on
  some other AMD/Intel card cannot satisfy the association. The diagnostic
  logs `CONOUT_WITHOUT_GOP` and `INDEPENDENT_WITHOUT_GOP` for `ErrOut`.
  **Every configured `ConOut` and every captured GOP must also resolve to
  an actual PCI display owner**: one valid AMD path cannot mask another
  unknown `ConOut`, nor can valid NVIDIA GOPs mask an unmatched extra GOP.
  `CONOUT_UNMATCHED` and `CONOUT_AMBIGUOUS` expose the former, while
  `GOP_UNMATCHED` exposes the latter. Unresolved ownership makes the strict
  graph `INCOMPLETE` (ambiguous ownership remains `CONFLICT`), never
  `COMPLETE`. Partial matches cannot imply full console-path separation.
  Independent `ConOut` and `ErrOut` observations, NVIDIA GOP associations
  and owner evidence remain distinct prerequisites, **never an authorization
  for vendor Stop or GPU mutation**. This does not prove the lit physical
  monitor/cable, destructor safety, firmware ownership teardown, or recovery.
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

### Physical B850 result from generic ordered two-target diagnostic 0008 — AMD-only console confirmed; NVIDIA GOPs absent (2026-11-08)

**Actual boot evidence:** uploaded `GPU-TOPOLOGY-DIAGNOSTIC-0008.TXT`, **7,284 bytes, 89 physical lines**, SHA-256 `549301ee28fb0b8ee286b3083375b1431ff7358c9e04f5e57243a5c53f7bca6a`. The operator reports **one monitor connected to the AMD iGPU, all NVIDIA display cables disconnected**; this is a different physical cable configuration from the mixed-monitor `0007`.

The same generalized ordered two-target EFI completed a strict, full 256-byte re-read PCI scan of **50 PCI handles**, retained **19** display/bridge nodes (31 excluded), and identified both NVIDIA GPUs in requested order: RTX 4090 `0000:01:00.0` (`10de:2684`, `1043:88e6`, root index 0, depth 1) and RTX 3080 `0000:04:00.0` (`10de:220a`, `19da:b612`, root index 2, depth 3). Both retained `ANCESTRY=1 ROOT_VALID=1 BY_DRIVER=1 OPENERS_KNOWN=1` and `SELECTION_STEP[n]=PASSIVE_IDENTIFIED`; aggregate `SELECTION_SEQUENCE=PASSIVE_IDENTIFIED COMPLETED=2`.

**Key same-source comparison:**

| Passive evidence | `0007` (mixed outputs) | `0008` (AMD-only connected) |
|---|---:|---:|
| Included PCI display/bridge nodes | 19 | 19 |
| NVIDIA GPU discovery and ordered selection | 2/2 | 2/2 |
| All GOP handles | 3 | 1 |
| Each NVIDIA GPU `GOP_COUNT` | 1 / 1 | **0 / 0** |
| Independent AMD GOP | 1 | 1 |
| `ConOut` device paths | 3 | 1 |
| NVIDIA `ConOut` matches | 2 | **0** |
| Independent `ConOut` match | 1 | 1 |
| `INDEPENDENT_CONSOLE_ASSOCIATION` | 0 | **1** |
| `NVIDIA_CONOUT_BLOCKER` | 1 | **0** |
| `TOPOLOGY_VERDICT` | `CONFLICT` (2) | `INCOMPLETE` (1) |

The **AMD-only firmware console policy is satisfied** in `0008`; there are no remaining NVIDIA `ConOut` paths. The overall `TOPOLOGY_VERDICT=INCOMPLETE` is **not a renewed console conflict**. Current `gpu_topology.c::GuTopoAssess` independently marks the assessment incomplete when *any identified NVIDIA GPU has zero observed GOP handles*. Both cards have `GOP_COUNT=0` here. This is consistent with the unplugged NVIDIA monitor configuration; the log does not establish the underlying reason GOP protocols were not produced.

The NVIDIA drivers remain bound at the PCI layer (`BY_DRIVER=1`). Absence of NVIDIA GOP handles must **not** be interpreted as completed proprietary `DriverBinding.Stop`, driver detachment, or a demonstrated safety/rollback path. The system's console handle still lacks a DevicePath and the log still says `PHYSICAL_DISPLAY_OWNER=UNKNOWN`; the physical iGPU monitor connection is **operator-reported**, independently consistent with the observed AMD-only GOP/`ConOut` snapshot. `HARDWARE_TESTED=0` and `READ_ONLY=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLR=0` retain their established *no mutation* meaning.

**Design consequence, not permission to unlock:** distinguish `PCI_TARGETS_IDENTIFIED`, `AMD_ONLY_CONSOLE_OBSERVED`, `NVIDIA_GOP_PRESENCE`, and `PROPRIETARY_STOP_READY` as separate read-only reasons. Do not globally relax or discard the missing-GOP `INCOMPLETE` flag to turn this report green: the real ownership, Stop and rollback protocols are still unresolved. Further AMD-only-cabling-only repeats are low value unless the current boot exposes new live driver-binding/ownership evidence.

### Physical B850 result from generic ordered two-target diagnostic 0007 — selection succeeds, console conflict (2026-11-08)

The uploaded `GPU-TOPOLOGY-DIAGNOSTIC-0007.TXT` is **8,407 bytes, 107 physical lines**, SHA-256 `67302542890272d2b45942b885e5ef054d9e3e8515551da79a8940c4a3770787`. This is the first supplied physical boot log **of the generalized ordered BDF selection implementation**; the earlier `0006` belongs to the fixed-pair implementation. The current generic diagnostic scanned 50 EFI PCI handles, retained 19 PCI display/bridge nodes, excluded 31, and captured 3 GOP paths and 3 `ConOut` paths. Both requested targets were observed **in order**: `SELECTION_STEP[0]=PASSIVE_IDENTIFIED` for `0000:01:00.0` (`10de:2684`, subsystem `1043:88e6`, root index 0, depth 1); `SELECTION_STEP[1]=PASSIVE_IDENTIFIED` for `0000:04:00.0` (`10de:220a`, subsystem `19da:b612`, root index 2, depth 3). Both have `GOP_COUNT=1 BY_DRIVER=1 OPENERS_KNOWN=1`. The aggregate reads `SELECTION_SEQUENCE=PASSIVE_IDENTIFIED COMPLETED=2 NON_SELECTED_NVIDIA_COUNT=0`.

The **separate topology/console verdict** remains `STATUS=PARTIAL TOPOLOGY=2` / `TOPOLOGY_VERDICT=CONFLICT NVIDIA_CONOUT_BLOCKER=1`: one independent AMD GOP and independent `ConOut` path exist, **but two NVIDIA `ConOut` paths remain**. `INDEPENDENT_CONSOLE_ASSOCIATION=0` follows directly from those two configured NVIDIA paths, not absent independent hardware. The console handle exposes no DevicePath, and `ConOutDev` paths remain `POSSIBLE_ONLY=1`; actual cable owner remains `PHYSICAL_DISPLAY_OWNER=UNKNOWN`. `HARDWARE_TESTED=0` means **no GPU actuation or firmware/driver release was tried**; actual passive EFI boot and PCI/GOP/console observations are established. The 0007 log does not separately attest the Secure Boot signature, backup or boot-manager handoff.

Historical Windows DIAG07 already observed AMD-only output with the NVIDIA boards installed and bound under a different monitor configuration. That optional same-version AMD-only passive confirmation has now been supplied as `0008`, which clears the NVIDIA console-path conflict but reports **zero NVIDIA GOP handles**, hence a separate `INCOMPLETE` verdict. It is not evidence of NVIDIA GOP Stop safety or an active unlock prerequisite being satisfied.

### Physical B850 result from diagnostic 0006 — strict graph succeeds, console conflict (2026-11-07)

The uploaded `GPU-TOPOLOGY-DIAGNOSTIC-0006.TXT` (7,525 bytes, 97
text lines; SHA-256 `9f21795f3fe2112eb7ea3131952c109f42627935d2df31c4f37c4f2a9fa2edc5`)
is the first supplied preboot capture in which the scoped strict
collector succeeds on the actual B850: `PCI_TOPOLOGY_SCOPE` shows
**50** PCI protocols visited, **19** display/PCI-bridge nodes
included and **31** excluded. Every included node retains strict
256-byte config rereads, BDF, device-path and ownership checks.
`PCI_CAPTURE=1 GOP_CAPTURE=1 CONOUT_CAPTURE=1`; the **older captured EFI**
printed `EXACT_PAIR=1`, a field removed from the new generic output.
The earlier AMD host-complex `1022:14d8` config offset `0xBC`
no longer blocks classification because this unrelated host bridge
does not belong to the GPU topology graph; the exact register meaning
remains unknown, and its changing value is not masked or called safe.

RTX 4090 `01:00.0` sits beneath root port `00:01.1` at
ancestry depth **1**. RTX 3080 `04:00.0` sits beneath root
port `00:02.1`, via bridges `02:00.0` and `03:00.0`, depth **3**.
Both have `ANCESTRY=1 ROOT_VALID=1 GOP_COUNT=1` and the roots
are distinct. The AMD iGPU `0f:00.0` also has a mapped GOP.

The remaining outcome is an explicit `GU_TOPO_CONFLICT`:
`DISTINCT_ROOTS=1 AMD_GOP_COUNT=1 AMD_CONOUT_MATCH=1
NVIDIA_CONOUT_MATCH=2 GOP_UNMATCHED=0 AMD_CONSOLE_ASSOCIATION=0`.
Both NVIDIA device paths remain configured in UEFI `ConOut` despite
the AMD path being present. The current source logs this as
`STATUS=PARTIAL TOPOLOGY=2`; **2 means conflict**, not incomplete
GPU inventory. Supplementary `TOPOLOGY_VERDICT=CONFLICT` reporting is committed without changing the legacy `STATUS` field. [GitHub Actions run `42433578045`](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42433578045) PASS: 58 topology host checks under GCC/Clang/UBSan and Windows EFI builds with/without handoff. The label and ordered selection are now present in the physically uploaded generic diagnostic `0007`.

`ConsoleOutHandle` matches `SystemTable.ConOut` SimpleTextOut but
does not expose a DevicePath, so the active console controller may be
virtual/aggregate and cannot be attributed to AMD alone. `ConOutDev`
continues to list all three **possible** output paths. User-observed
AMD BIOS video is evidence of AMD firmware output, not proof of
surviving a proprietary NVIDIA GOP release.

**Prior controlled AMD-only console test is already complete.**
`Windows-GPU-UNLOCK` historical DIAG07 (SHA-256
`16c4beb57abc9d2d695ab0b2e93bebafcdecc33021c4a07de522f89eb59a8872`)
was collected with the AMD motherboard monitor **as the only connected
monitor**, both NVIDIA boards still physically installed and their
UEFI GOP drivers bound through PCI `BY_DRIVER`. It contained only
one AMD GOP; `ConOut`, `ConOutDev`, `ErrOut`, `ErrOutDev` all
mapped exclusively to AMD. The original
[`verify-console-survivor.py`](https://github.com/akashskypatel/Windows-GPU-UNLOCK/blob/main/research/profile/verify-console-survivor.py)
accepted that capture as `observed_amd_only_console`. DIAG08 and
DIAG10 continued the AMD-only passive configuration, with DIAG10
capturing both NVIDIA loaded-driver contexts. Historical source:
[Windows investigation notes](https://github.com/akashskypatel/Windows-GPU-UNLOCK/blob/main/research/nvidia-tensor-core-unlock-investigation.md),
sections v77 and v93.

Therefore the new universal capture `0006`, with all three GPU displays
connected and `NVIDIA_CONOUT_MATCH=2`, is **not a contradiction**
or a new AMD-only-monitor test obligation. Different cabling leads to
different configured GOP/console device paths. A redundant cable-only
retest is not required to rediscover this fact. Reconcile original
NVIDIA driver ownership/Stop/rollback evidence with the verified
strict topology instead. AMD console **survival after a real proprietary
NVIDIA GOP Stop** is still untested; no active unlock is authorized.

### Independent passive PCI evidence on strict-collector failure

The original strict topology assessment still requires a complete bounded set of at most **128 PCI records**. Earlier USB evidence gave `PCI_CAPTURE=0 PCI_NODES=0`, so failure could not be attributed to missing GPUs, a count limit, or a first-handle error.

The supplementary, diagnostic-only observer borrows the **read-only reporting method**, not the GPU mutation/teardown code, from [Windows-GPU-UNLOCK's passive handle collector](https://github.com/akashskypatel/Windows-GPU-UNLOCK/blob/main/internal-boot/boot/diagnostic.c). When the strict capture fails, it independently snapshots up to **4,096 EFI handles** using `AllHandles`, probes for `EFI_PCI_IO_PROTOCOL` with `HandleProtocol`, reads the first 64 PCI configuration bytes, obtains PCI location, captures a bounded device path, and inspects BY_DRIVER open records. Each allocated firmware pool is released. No GPU MMIO, PCI configuration writes, ROM reads, driver detachment, reset or UEFI variable writes are introduced.

The log marks the secondary report `PCI_OBSERVATION_ONLY=1 NOT_TOPOLOGY=1 CAN_DETACH=0 CAN_UNLOCK=0`, followed by `PCI_OBS_SUMMARY`, `PCI_OBS_STATUS`, and selected `PCI_OBS[]` display/bridge identity/path/ownership records. It records at most 96 relevant endpoints and explicitly marks truncation or a failed UEFI call. Paths or owner identities that could not be observed remain unknown. The report never changes `PCI_CAPTURE`, `EXACT_PAIR`, or the topology result and cannot authorize active GPU firmware operations. It is independent evidence useful for *diagnosing* a failed strict collector.

[GitHub Actions run 42425559424](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42425559424) **PASS**: `tests/test_gpu_pci_observation.c` contains **30 assertions**, run with GCC, Clang and GCC UBSan; existing inventory/topology/handoff host checks also pass, and MSVC builds the diagnostic EFI with/without Windows handoff. The CI handoff variant uses a synthetic, non-deployable ESP GUID. These are host mock and build results, **not a successful physical B850 inventory**, real signed USB execution, or proof of driver release.

## Host verification and fault boundaries

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic -Isrc tests/test_gpu_topology.c src/gpu_topology.c src/gpu_inventory.c -o /tmp/test_gpu_topology && /tmp/test_gpu_topology
```

The host tests validate one/two/three NVIDIA devices, mixed models and
subsystems, reversed enumeration, shared roots, Intel/AMD console paths,
PCI-location lookup, duplicate handles/BDFs, bridge windows, malformed paths,
NVIDIA ConOut ownership and capacity bounds.
They cannot prove genuine firmware methods, GOP driver safety or display
ownership. See `Windows-GPU-UNLOCK`'s read-only `research/pci-decode/efi-capture.*`,
`internal-boot/boot/probe-parent.c` and `research/gop/nvgop_stop_model.py`
for the basis of the ancestry observations and for modeled Stop-related
failure boundaries. The latter model is host-only and must **not** be used as
proof that `DisconnectController()` is safe.

## Current status

- Generic host checks and isolated MSVC EFI builds are verified; physical
  execution of the new generic **ordered two-target passive diagnostic** is
  now documented in both B850 `0007` (mixed-monitor console conflict) and
  `0008` (AMD-only console PASS but NVIDIA GOPs 0/0 and overall topology INCOMPLETE);
  the historical `0006` used a fixed-pair image. None executes the active mutation path.
- [GitHub Actions run 42422191256](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42422191256) passed the 21-path and 17-handoff negative checks under GCC/Clang (handoff also GCC UBSan); Windows MSVC compiled the opt-in handoff variant using a deliberately **synthetic, non-deployable Windows ESP GUID**, then rebuilt the console-only variant for artifact upload. No signing, USB boot or real Windows handoff was exercised.
- No changes are made to the legacy active entrypoint's strict multi-GPU
  refusal. No dual-GPU write-enabled path is introduced.
- Revalidate all build and host results against a specific commit and hardware
  capture before treating any output as an implementation qualification.
