# Universal GPU Unlock — research fork

This fork investigates the safety and feasibility of a preboot Tensor Core
FP32-accumulation issue-rate change on a dual-GPU Windows workstation.
**It does not currently provide a verified working unlock for this machine.**

**Development scope:** Implement and test the unlock on the research setup
(RTX 4090 AD102 + RTX 3080 12 GB GA102, Windows NVIDIA 617.42, MSI B850,
AMD iGPU). Preserve the parent project's dynamic recognition of other GPUs,
multi-target selection, and shared safety checks. Do not investigate or fix
defects limited to other hardware if they are also present in the parent
repository, unless a demonstrated shared-code dependency affects this setup.
Turing-only signature and loader work is deferred. This scope does not reduce
the parent's device support list or authorize an active GPU operation.

**Current physical research setup (not a support allowlist):** RTX 4090
(AD102, ASUS `1043:88e6`) and RTX 3080 12 GB (GA102, ZOTAC
`19da:b612`), Windows NVIDIA KMD 617.42, MSI B850 Gaming Plus WiFi
and an AMD iGPU. This setup supplies physical evidence, not permanent PCI
addresses or a restriction on other supported NVIDIA cards.

**Parent device compatibility:** The fork uses the upstream device registry:
11 Ada, 24 Ampere and 20 Turing NVIDIA device IDs. Passive discovery and
selected preparation do not require either of the owner's GPUs. A detected
GPU family selects the matching SHA-256-pinned **reference** firmware
manifest. Availability and format of each image, driver ownership and
verified recovery remain separate conditions before any active operation.
The Turing FWSEC v2 descriptor has a bounded, structural-only parser, but
its signed-loader execution is not verified. Recognizing a Turing GPU does
not prove it can be unlocked by this build. Further Turing work is out of
scope unless it affects the research setup. Hardware identities
must be revalidated within the current boot.

## Current evidence and limitations

- The read-only EFI diagnostic has observed both NVIDIA GPUs, distinct PCI
  ancestry, and an AMD iGPU. The physical three-GPU `0016` report established
  that all three GOP/TextOut branches share GraphicsConsole, ConSplitter and
  an aggregate console handle in the captured mixed-display configuration.
  The separately captured AMD-iGPU-only `0018` report instead has zero
  NVIDIA configured-console matches, one independent AMD GOP/ConOut/ErrOut
  path, and both NVIDIA firmware drivers still bound. This establishes
  passive console isolation for that boot **only**; the strict topology
  verdict remains `INCOMPLETE` with zero NVIDIA GOPs, and no selected
  vendor Stop, AMD continuity after Stop or cold recovery was tested.
- Dynamic PCI discovery and ordered target selection work in passive
  diagnostics and host verification. Selecting a GPU does **not** authorize
  its controller to be stopped or its registers to be changed.
- The non-diagnostic selected application and DXE builds are preparation-only:
  they refuse before `GuInit` and guarded hardware-write, DMA and FLR stages.
  Both MSVC builders now **reject omitted target selections** instead of
  emitting legacy mutating EFI binaries.
  Selected preparation also performs an all-or-none, read-only refresh of
  every requested GPU's PCI identity before the existing refusal; it does
  not create independent per-card mutable transaction state or a
  simultaneous hardware snapshot. Commit `da4051c4` passed host and
  MSVC EFI CI in [run 42739479432](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42739479432).
  Dedicated host negatives now exercise duplicate/aliased handles, BDFs,
  PCI protocols, failed later-target reads, zeroed partial outputs and
  first-target BAR drift while a later target is inspected. The source
  remains read-only, and active firmware ownership/recovery is unverified.
- The 1 MiB PROM structural identity parser checks the current GPU
  device ID and ROM layout. A matching ROM structure is **not** a verified
  manufacturer signature or proof that a specific firmware image is safe.
  The checks do not pin the source to the two research GPUs.
- The original Windows-GPU-UNLOCK `internal-boot/boot/probe.c` already
  implements `ProbeBar`/`DumpProm`: a bounded 1 MiB BAR0
  `0x300000` read followed by `PROM.BIN` export. This is **not** a 2 MiB
  full-card NVGI v3 ROM capture. Its reader, target-bound EFI adapter and
  non-overwrite-intent file save have been ported to
  `src/gpu_prom_capture*`. GCC/Clang host tests and MSVC compile-only
  integration pass in [CI 42743917070](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42743917070).
  **The adapter is not linked or invoked** in the passive topology EFI,
  selected active prep app or DXE: the two-card BAR0 read semantics are not
  independently approved. It cannot be used as proof of firmware extraction,
  GPU mutation safety or recovery. The create-if-absent precheck is not an
  atomic exclusive-create guarantee.
- The owner's existing `AD102.rom` and `GA102.rom` were already examined
  in the Windows investigation v99; the uploaded archive matches the
  recorded SHA-256 and both 2,048,000-byte NVGI v3 structures. The owner
  reports they were captured using Windows-GPU-UNLOCK. Their **structural**
  PCI/IFR gate is complete; the exact capture method, manufacturer
  authenticity and full physical SPI coverage are not independently proven.
- **Linux is a reference, not a Windows asset synonym.** NVIDIA's public
  open-module README requires release-matched GSP firmware/userspace, and
  NVIDIA's Nouveau extraction notes say the GSP-RM ABI is not stable across
  versions. In the original Windows-GPU-UNLOCK comparison, Linux 610.43.03
  and Linux 615.78.08 share four exact GSP boot image/descriptor files, but
  the AD102 and GA102 production Booter bundles differ. Windows 617.42 was
  previously observed with a separately paired `gsp_ga10x.bin` (27,043,992 B)
  and `ucodes_ga10x.bin` (58,215,936 B), running GSP 615.78.05, **not
  proven identical to either Linux release**. V67 files are generated
  experimental payloads, not NVIDIA stock signed Windows firmware.
  A direct independent Windows-versus-Linux version-pinned component comparison,
  plus genuine Windows Booter producer/loader tracing, remains mandatory.
- The exact nine committed `AD102-ASSETS`/`GA102-ASSETS` 610.43.03
  reference binaries are now checked offline by
  `python3 tools/verify_reference_assets.py --root .`. All nine
  SHA-256/size pins matched in
  [CI 42840199095](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/42840199095);
  three negative verifier tests and both host and MSVC EFI jobs passed.
  These findings establish **610 reference file identity only**, not
  NVIDIA signature authentication, Windows 617.42 ABI, or hardware readiness.
  The original Windows-GPU-UNLOCK
  `research/firmware/prepare_windows_firmware.py` already reconstructs
  these assets from pinned source and distribution packages; no duplicate
  extraction or GPU ROM capture is necessary.
- Genuine Windows 617.42 signed Booter IMAGE/HEADER/SIG/PATCH origins,
  Falcon loader invocation/descriptor, per-card secure acceptance, and
  controlled recovery are unverified. Older 610.43.03 assets and Linux
  comparison sources are **not** validated substitutes.
- Windows `Check-GPU-Unlock.bat` produces an observational FP16 GEMM sample
  for **each CUDA ordinal**. It deliberately reports
  `UNLOCK_VERIFIED=UNKNOWN`: a throughput threshold is not proof of a
  changed firmware issue-rate selector or even of a particular PCI BDF.

**All active eligibility gates remain closed:**

```text
CAN_DETACH=false
CAN_UNLOCK=false
CAN_FLASH=false
RECOVERY_VERIFIED=false
```

The original legacy `Setup-AdaUnlock.ps1` is disabled in this research fork
before administrative elevation, USB formatting or ESP/NVRAM changes.
Do **not** use historical signed binaries, unsigned CI artifacts, installer
instructions, persistent DXE/NVRAM setup, or arbitrary firmware assets as a
shortcut around the research gates. No claims of 100% safety, BitLocker
compatibility, anti-cheat safety, or guaranteed speedup are established.

## Authoritative project documents

- [Dual-GPU Windows 617.42 remediation plan](docs/DUAL_GPU_61742_REMEDIATION_PLAN.md)
- [Read-only GPU topology diagnostic](docs/READ_ONLY_GPU_TOPOLOGY_DIAGNOSTIC.md)
- [Preparation-only active source builds](docs/ACTIVE_GPU_PREPARATION.md)
- [Windows-GPU-UNLOCK authoritative execution map](https://github.com/akashskypatel/Windows-GPU-UNLOCK/blob/main/research/nvidia_unlock_evidence_to_execution_map.md)
- [Windows 617.42 cumulative reverse engineering notes](https://github.com/akashskypatel/Windows-GPU-UNLOCK/blob/main/research/nvidia-tensor-core-unlock-investigation.md)

For host-only tests and compile evidence, see
[the GitHub Actions workflow](.github/workflows/gpu-topology-diagnostic.yml).
Successful CI proves the selected source compiles and the tested host logic
behaves as asserted; it does not prove proprietary firmware signatures, UEFI
driver ownership release, physical recovery or post-Windows throughput.

## Attribution

This repository is a research derivative of
[BrightRaider/universal-gpu-unlock](https://github.com/BrightRaider/universal-gpu-unlock).
The upstream quick-start, marketing performance claims and operational
guarantees are **not** verified claims about this fork, its installed cards,
or stock Windows 617.42. Respect the repository's existing license and
the provenance/licensing of any firmware investigated separately.
