# Dynamic GPU targeting for active-source EFI preparation

**State: DYNAMIC ORDERED SOURCE AND HOST TESTS VERIFIED; PHYSICAL GPU ACTUATION BLOCKED.**

The resource-ownership and diagnostic-completeness refactor items are tracked
in [the refactor review](MULTI_GPU_REFACTOR_REVIEW.md), including a gate-by-gate
analysis of every source-level block on active execution. That review changed
no admission gate: this document's refusals, and the `GU_PREFLIGHT_ONLY` write,
DMA and FLR refusals below, are unchanged.

The interactive UEFI application and the Driver Execution Environment (DXE) driver use the same ordered execution coordinator. Neither production builder permits live GPU actuation.

## Dynamic selection and read-only preparation

A PCI location has the format segment:bus:device.function (BDF).
Specify one location with `-SelectionBdf` or 2–128 distinct locations with
`-SelectionBdfs`. The shared parser preserves the requested order and rejects
invalid or duplicate locations. It does not select multiple devices implicitly.

The boot discovery path allocates its inventory from the observed PCI handle
count, with a 4,096-handle resource bound. It retains independent saved
records, live records, selected indices and PCI snapshots. The 16-record
compatibility interface remains for bounded callers, not boot discovery.
A device identity includes the BDF, vendor/device, subsystem, class and
current-boot handle and PCI protocol references. Identity does not approve
firmware execution.

The shared collector reads the complete NVIDIA inventory. The refresh
operation rejects added, removed, rebound or aliased records. It reads all
PCI configurations twice, including unselected devices. A failed read or
configuration change refuses the sequence. These reads are not atomic with
hardware activity.

Each selected GPU has a distinct retained platform context, firmware assets,
release state and protocol graph. The shared coordinator executes one
transaction at a time. It checks the previous context before it selects a
successor. It stops on a mandatory failure, retains completed contexts and
checks the final combined handover. Standby persistence uses the existing
per-target policy. The source does not grant hardware admission.

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
The synthetic compile-only profile also permits selected sequences. It
uses `-SyntheticCompileProfile` with an explicit selection. The resulting
files have COMPILE-ONLY names and `NOT_FOR_DEPLOYMENT=1`. Both entrypoints
refuse before device work. Do not deploy these files. The live builders
and non-synthetic multiple-target source still refuse execution.
`GuAd104PromMatches` requires an independently authenticated per-card digest
allowlist. No source or compile result proves physical recovery.

## Known legacy blockers (not generalized or waived)

| Component | Current limitation | Required direction |
|---|---|---|
| `src/port.c::GuTargetConfig` and `src/checks.c::GuCheckConfig` | Legacy active preflight assumes PCI segment 0 and other narrow PCI geometry/capability requirements | Use validated per-target device contracts; do not globally relax constraints merely to admit more boards |
| `src/msi_release.c` | Independent release state and dynamic graphs exist. Physical release and recovery contracts remain target-specific. | Verify the real owner graph and device-specific contract without relaxing admission. |
| `src/port.c` | Independent registered contexts and retained completion state exist. The active CPU pointer is serialized. | Preserve context identity and test the complete physical sequence only when qualified. |
| `src/boot_io.c`, `src/profile.c`, `src/vendor/*` | Fixed firmware asset names, sizes, hashes and 610 reference lineage | Per-architecture and exact-device authenticated assets, loader compatibility and observed signed Windows execution; no cross-card substitution |
| Display/console | Passive graph can be dynamic; active release has not been proven with NVIDIA, AMD, Intel, shared root, or headless arrangements | Validate same-boot GOP/ConOut owner graph and independently demonstrate console survival following Stop |
| Windows and recovery | Stock NVIDIA 617.42 acceptance, post-transaction device health and rollback remain unproven | Preserve fail-closed stages and demonstrate safe physical recovery before any separately authorized active execution |

**Remaining qualification:** The source coordinator and per-target context
lifetime checks exist. Physical release, protected firmware acceptance,
post-modification Windows operation and failed-transaction recovery remain
unverified. The host tests do not remove these restrictions.

## Verification and evidence boundary

Host tests compile the actual shared UEFI discovery adapter against mocked
EFI PCI protocols. The ordinary build tests conservative one-card behavior;
separate `GU_ACTIVE_SEQUENCE_PREPARE_ONLY` test builds assert that selected
firmware never calls `GuInit`. GCC/Clang and GCC UBSan cover the shared
inventory, ordered target selection and topology. Windows MSVC compiles the
real interactive app and DXE source trees as selected preparation-only EFI
images, as well as the separate passive diagnostic variants.

**Verified CI:** [Run 42525760457](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42525760457) completed both `host` and `efi` jobs successfully at source commit `4e69a7c8cdc46023fbd21d6de7855f914a10cfd5`. Inventory/ordered-selection tests: **73 checks** under GCC, Clang and GCC UBSan. Topology tests: **72 checks** under GCC, Clang and GCC UBSan. Legacy UEFI discovery adapter: **39 checks** each under GCC and Clang; selected-entrypoint refusal: **4 checks per configuration** under GCC and Clang for both one- and two-target generated configurations, with zero `GuInit` calls. Windows MSVC built the default diagnostic, ordered single/two/three-target diagnostics, and genuine app and DXE selected preparation-only binaries. [Active app/DXE preparation build artifacts](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42525760457/artifacts/13635198013); [passive selected EFIs](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42525760457/artifacts/13635163109). The workflow deliberately does not execute these EFI binaries.

**Current architecture verification:** [CI 44198436908](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/44198436908)
passed host and Windows EFI compilation with runtime-sized discovery.
[CI 44199844525](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/44199844525)
passed the host tests with 18 selected GPUs and two protected peers.
The actual discovery adapter completed 695 checks per compiler. The tests
include late allocation failure, retained ownership and peer drift.
Read-only preparation completed 25 checks per compiler and never called
`GuInit`. Refer to the linked run for the Windows EFI result.

None of these tests actually execute an EFI image on the physical host.
Neither PCI device ownership, GPU detachment, signed firmware acceptance,
real GPU mutation, Windows recovery nor performance improvement is proven.

**Current authorization state:** `CAN_DETACH=false`, `CAN_UNLOCK=false`,
`CAN_FLASH=false`, `RECOVERY_VERIFIED=false`. No source build, selected
binary, host test or passive diagnostic changes those gates.
