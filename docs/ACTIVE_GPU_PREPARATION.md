# Dynamic GPU targeting for active-source EFI preparation

**State: SOURCE/HOST/EFI-COMPILE VERIFIED; GPU ACTUATION BLOCKED.**
The read-only prepared-sequence refresh is current through
[CI `42855085195`](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42855085195);
the independent ErrOut classifier's invalid-graph refusal is current through
[CI `42857858767`](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42857858767).
Neither is a new hardware approval.

This document applies to the *non-diagnostic* interactive EFI application
(`src/app.c`) and DXE entrypoint (`src/dxe_main.c`). Both still invoke the
shared `src/gpu_discovery.c::GuDiscoverSingleGpu`. An operator-selected build
prepares an ordered list of GPU identities, but **cannot run an unlock**.
Do not deploy or register the preparation-only DXE driver as a startup driver.

## Dynamic selection and read-only preparation

`GuPrepareGpuSequence` enumerates current-boot EFI PCI_IO handles, collecting
up to 16 NVIDIA PCI display-class `03xx` functions from at most 4096 handles.
It records full segment:bus:device.function, vendor/device, subsystem, class,
and opaque current-boot EFI handle/protocol pointers. The source does not
pin a motherboard model, a subsystem, PCI slot or architecture. Known
legacy device IDs are reported separately from identity discovery; being
discoverable does **not** qualify an unsupported model for mutation.

The operator may supply one BDF (`-SelectionBdf`) or 2–16 distinct BDFs
(`-SelectionBdfs`, comma-delimited) in execution-intent order. The same
`src/Gpu-Selection-Build.ps1` validation and generated `gpu-selection.h`
are shared with the read-only topology EFI, not duplicated per builder.
`GuFindGpuSequence` atomically resolves all requested locations against the
observed inventory. Duplicate, nonexistent or invalid locations fail with
no partially selected indices. With no explicit selection, a lone NVIDIA
device may be identified by the legacy code, but **multiple devices are
never implicitly selected**.

Each prepared record is **observational only**. Handles and PCI ownership
are boot-local and cannot be assumed to survive driver Stop, FLR, firmware
execution, reconnect, or handoff. `GuRefreshPreparedGpu` validates only
one target's saved **current-boot handle, PCI_IO instance, BDF,
vendor/device, class and subsystem**, returning a 256-byte PCI snapshot
only after full identity match. That single-target helper does **not**
validate the rest of the NVIDIA inventory.

**For a selected active-preparation image**, the existing
`GuRefreshPreparedSequence` now also performs a fresh UEFI census of the
complete NVIDIA display inventory. It rejects missing, added or rebound
devices, aliased saved BDFs/handles/PCI_IO instances, read failures of
unselected NVIDIA cards, and any mismatch in two sequential full
256-byte PCI snapshots on **all** inventoried NVIDIA devices. Only the
requested targets are returned in operator order after successful
validation. A failed preflight returns no partial selected results.
[Universal host and Windows EFI verification
`42855085195`](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42855085195)
**PASS**. The scans are **not atomic with hardware** and do not verify
GOP/ConOut/ErrOut/driver ownership or safe release. A future active
transaction still requires fresh ownership evidence and independent
source-specific post-mutation and recovery checks; no GPU mutation
is authorized.

## Compile a preparation-only active EFI

Run from the `universal-gpu-unlock` repository root on Windows with MSVC.
The example PCI addresses below are **synthetic**; replace them with actual
currently observed addresses when preparing a target-specific image.

Interactive application source, ordered two-target preparation:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\src\Build-Experimental-MSVC.ps1 -SelectionBdfs '0000:09:02.0,0001:2A:03.1'
```

DXE driver source, same ordered preparation:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\src\Build-Dxe-Driver.ps1 -SelectionBdfs '0000:09:02.0,0001:2A:03.1'
```

The selected build outputs are named `GPU-ACTIVE-PREP-*` and
`GPU-ACTIVE-PREP-DXE-*`. They are **unsigned**, use a deliberately synthetic
non-deployable Windows ESP test profile (`GU_HOST_TEST_PROFILE`), and do not
prove compatibility with the machine identified by the supplied BDFs.

Build-time `GU_ACTIVE_SEQUENCE_PREPARE_ONLY` makes
`GuDiscoverSingleGpu` return `EFI_UNSUPPORTED` before `GuInit` for **any**
selected build, even when exactly one NVIDIA GPU is present. Both the app
and DXE entrypoints add an independent refusal immediately after discovery.
`GU_PREFLIGHT_ONLY=1` also disables the port's guarded GPU MMIO/PCI write,
DMA and FLR helpers in these builds. Full legacy source dependencies are
compiled for compatibility checking, not exercised.

An active-preparation image may still perform its normal pre-discovery
boot/log initialization, including opening or writing a log file. This is
**not a universally side-effect-free EFI image**. For strictly passive
PCI/GOP/ConOut capture use
[the isolated topology diagnostic](READ_ONLY_GPU_TOPOLOGY_DIAGNOSTIC.md).

**Important:** Both repository MSVC builders now **refuse an omitted selection**
before attempting to compile. Providing a BDF produces preparation-only
artifacts; they refuse before `GuInit` and all hardware mutation stages.
The legacy single-GPU transaction code still exists in the repository and
may be compiled separately, but **`GuAd104PromMatches` now refuses any ROM
without an independently authenticated per-card digest allowlist** (none is
pinned), preventing ROM-length-only approval. This is a fail-closed source
guard, **not** physical recovery proof or production firmware compatibility.

## Known legacy blockers (not generalized or waived)

| Component | Current limitation | Required direction |
|---|---|---|
| `src/port.c::GuTargetConfig` and `src/checks.c::GuCheckConfig` | Legacy active preflight assumes PCI segment 0 and other narrow PCI geometry/capability requirements | Use validated per-target device contracts; do not globally relax constraints merely to admit more boards |
| `src/msi_release.c` | Static Z790-specific PCI/GOP/device paths, USB/input, NVIDIA driver bindings, exactly staged proprietary `Stop`, singleton static `Plan` | Replace with fresh observed per-target controller/driver graph and independently proven Stop/partial-failure/recovery contracts |
| `src/port.c` | One mutable `GU_PLATFORM Gu`, shared DMA, BAR, FLR, selector and containment state | Isolated per-target contexts; never use the current `Gu` instance for more than one simultaneous/serial GPU transaction |
| `src/boot_io.c`, `src/profile.c`, `src/vendor/*` | Fixed firmware asset names, sizes, hashes and 610 reference lineage | Per-architecture and exact-device authenticated assets, loader compatibility and observed signed Windows execution; no cross-card substitution |
| Display/console | Passive graph can be dynamic; active release has not been proven with NVIDIA, AMD, Intel, shared root, or headless arrangements | Validate same-boot GOP/ConOut owner graph and independently demonstrate console survival following Stop |
| Windows and recovery | Stock NVIDIA 617.42 acceptance, post-transaction device health and rollback remain unproven | Preserve fail-closed stages and demonstrate safe physical recovery before any separately authorized active execution |

**Required future sequencing:** validate the complete ordered target list
without mutation, capture per-target identity and non-target exclusions,
validate independently authenticated assets and fresh owner/console graph
for the *next* target, prove safe release/rollback, execute **at most one**
approved target, verify cleanup/FLR/stock Windows readiness, re-inventory
the remaining targets, and abort on the first unexpected state. The
current code does **not** implement that active mutation sequence.

## Verification and evidence boundary

Host tests compile the actual shared UEFI discovery adapter against mocked
EFI PCI protocols. The ordinary build tests conservative one-card behavior;
separate `GU_ACTIVE_SEQUENCE_PREPARE_ONLY` test builds assert that selected
firmware never calls `GuInit`. GCC/Clang and GCC UBSan cover the shared
inventory, ordered target selection and topology. Windows MSVC compiles the
real interactive app and DXE source trees as selected preparation-only EFI
images, as well as the separate passive diagnostic variants.

**Verified CI:** [Run 42525760457](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42525760457) completed both `host` and `efi` jobs successfully at source commit `4e69a7c8cdc46023fbd21d6de7855f914a10cfd5`. Inventory/ordered-selection tests: **73 checks** under GCC, Clang and GCC UBSan. Topology tests: **72 checks** under GCC, Clang and GCC UBSan. Legacy UEFI discovery adapter: **39 checks** each under GCC and Clang; selected-entrypoint refusal: **4 checks per configuration** under GCC and Clang for both one- and two-target generated configurations, with zero `GuInit` calls. Windows MSVC built the default diagnostic, ordered single/two/three-target diagnostics, and genuine app and DXE selected preparation-only binaries. [Active app/DXE preparation build artifacts](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42525760457/artifacts/13635198013); [passive selected EFIs](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42525760457/artifacts/13635163109). The workflow deliberately does not execute these EFI binaries.

None of these tests actually execute an EFI image on the physical host.
Neither PCI device ownership, GPU detachment, signed firmware acceptance,
real GPU mutation, Windows recovery nor performance improvement is proven.

**Current authorization state:** `CAN_DETACH=false`, `CAN_UNLOCK=false`,
`CAN_FLASH=false`, `RECOVERY_VERIFIED=false`. No source build, selected
binary, host test or passive diagnostic changes those gates.
