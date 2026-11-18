# Production readiness inventory — read-only source review

**Review date:** 2026-11-18 · **Scope:** `src/`, `src/vendor/`, build scripts, CI,
`sim/`, tests · **Method:** static reading only; no source file was modified and
no hardware was touched.

**Fix pass in progress (2026-11-18):** the class-A items and the two coupled
hardware-refusal defects (B6, B7) are being closed in source. See §11 for the
per-item state; every fix there is **unverified on hardware** and is exercised
only by the host simulator (`ENVIRONMENT=HOST_SIMULATOR`, `ACTUATION_VERIFIED=0`)
and host contract tests.

**One-line verdict:** the repository is a *fail-closed research harness*. Every
component that would mutate a GPU is present and internally consistent, but the
product asked about — one deployable signed image that selects cards at runtime
and unlocks several of them on arbitrary supported machines — is blocked by
three separate classes of work: **per-machine build coupling** (selection,
ESP identity, stock profile), **unproven privileged operations** (vendor Stop,
DMA quiescence, firmware acceptance), and **release engineering** (signing,
CI coverage of the only actuating build, recovery). Roughly **10 blocking
architecture items, 10 hardware-precondition items, 10 unverified-operation
items, and 5 release items**.

---

## 0. What the source can do today

| Artifact | Built by | What it does | Mutates a GPU? |
| --- | --- | --- | --- |
| `GPU-TOPOLOGY-DIAGNOSTIC.EFI`, `MSI-B850-*.EFI`, Windows-handoff variants | `Build-Gpu-Topology-Diagnostic.ps1` | read-only inventory, ROM/fuse/GOP/policy observation | No — separate PE image, never links the unlock core |
| `GPU-ACTIVE-PREP-*.EFI`, `…-DXE-*.efi` | `Build-Experimental-MSVC.ps1`, `Build-Dxe-Driver.ps1` (default) | boots, discovers, validates cold profile, refuses before mutation (`GU_ACTIVE_SEQUENCE_PREPARE_ONLY`, `GU_PREFLIGHT_ONLY 1`) | No |
| `GPU-SEQUENCE-COMPILE-ONLY-*.EFI` | same builders, `-SyntheticCompileProfile` | compiles the ordered multi-target source; both entrypoints return `EFI_UNSUPPORTED` immediately | No |
| `GPU-{ONE-TARGET,SEQUENCE}-UNVERIFIED-ACTUATION-*.EFI` | same builders, `-AcceptUnverifiedActuation` | the real transaction, one card at a time, ordered | **Yes** — no CI build coverage, per-machine, `ACTUATION_VERIFIED=0` |
| `GPU-RUNTIME-SELECTION-{PREP,UNVERIFIED-ACTUATION}[-DXE].{EFI,efi}` | same builders, `-RuntimeSelection` | same profiles, but the target list comes from the `GuTargets` variable at boot instead of the build | only with `-AcceptUnverifiedActuation`, which still requires the waiver |

CI (`gpu-topology-diagnostic.yml`) compiles the diagnostic and the app/DXE
prepare and synthetic variants on `windows-latest`. It **never** builds an
admitted image, because that build requires the private
`src/local/boot_profile.h` and is not exercised anywhere
(`grep -n AcceptUnverifiedActuation .github/workflows/*.yml` → no hits).

---

## 1. The gates, exactly as they stand

| Gate | Location | Default | With `-AcceptUnverifiedActuation` |
| --- | --- | --- | --- |
| Build admission | `port.h:130-146` (`GU_ACTUATION_ADMITTED`) | 0 | 1 |
| Write/DMA/FLR helpers | `port.h:148-157` (`GU_PREFLIGHT_ONLY`) | 1 | 0 |
| Runtime multi-target refusal | `gpu_discovery.c:977-981` | refuses `selected_count != 1` | admits ordered list |
| Single-target compile gate | `gpu_discovery.c:26-28` | `#error` unless one address | lifted |
| Vendor Stop for 0x2684 / 0x220a | `msi_release.c:1055-1060` | `EFI_UNSUPPORTED` | logged admission |
| Turing actuation | `profile.c:268-271` | `TURING_PREPARE=REFUSED` | unchanged |
| PROM authenticity | `profile.c:78-99` | structural + digest log only | unchanged |

The gates fail closed and early, log a reason, and never retry. That design is
sound and should survive every item below.

---

## 2. Class A — architecture coupling to the build machine (P0)

These are the items that make "dynamic hardware selection" impossible today.

### A1. Target selection is a compile-time constant — **P0**
`gpu_discovery.c:19-31` includes a generated `gpu-selection.h`;
`GU_GPU_SELECTION_COUNT`/`GU_GPU_SELECTION_LOCATIONS` are baked in by
`Gpu-Selection-Build.ps1` from `-SelectionBdf(s)`. With no requested list, only
the `count == 1` case auto-selects (`gpu_discovery.c:283-286`). There is no
runtime inventory menu, no UEFI-variable policy, no "all supported cards"
default. A card added to a slot, or a BDF that shifts after a firmware update,
turns into `EFI_UNSUPPORTED` with no way to recover without a rebuild.
*Exit criteria:* runtime selector over the live inventory with the ordered
coordinator unchanged, plus a documented default policy and an operator override
that does not require recompilation.

**Status (2026-11-18): fixed in source — unverified on hardware.** `-RuntimeSelection`
builds carry no `gpu-selection.h`; the target list is the `GuTargets` UEFI
variable (`src/gpu_selection.c`), read once per boot, defaulting to the
single-card policy when unset and to every supported card for `ALL`. The
console path records a lone card automatically and offers a numbered choice on
a multi-card machine. See §11.1.

### A2. The Windows ESP is identified by a build-time partition GUID — **P0**
`boot_io.c:468-482` requires the ESP device path to match `WindowsPartition`;
`gpu_windows_path.c:21-40` compares the GPT GUID byte-for-byte;
`src/boot_profile.h` demands a private `local/boot_profile.h`, and
`boot_profile.example.h` says "Do not remove or widen those guards to make
another machine boot." One image therefore belongs to one Windows installation.
*Exit criteria:* runtime ESP identification (enumerate ESPs, require exactly one
with `\EFI\Microsoft\Boot\bootmgfw.efi`, cross-check the `BootOrder` entry that
names it), keeping the strict single-ESP refusal.

**Status (2026-11-18): fixed in source — unverified on hardware.** `-RuntimeEsp`
drops the compiled pin. A volume qualifies only if its device path carries
exactly one GPT partition with the EFI-system type (`GuWindowsEspPathCandidate`),
it is not USB and not this image's own volume, and it actually contains
`\EFI\Microsoft\Boot\bootmgfw.efi`; the existing "exactly one match"
refusal is unchanged, so a second ESP cannot be silently chosen. The
`BootOrder` cross-check in the original exit criteria is **not** implemented:
it needs live `BootOrder` parsing and a second machine to validate. See §11.6.

### A3. Multi-GPU execution exists but only behind the waiver — **P0**
`gpu_discovery.c:26-28` (compile) and `:977-981` (runtime) both refuse
`N > 1` unless admitted; `unlock_execution.c:148-190` is a correct sequential
loop, but nothing has ever executed two cards. Production requires *verified*
multi-target admission, which needs C1–C4 resolved first.
*Exit criteria:* two physical cards, ordered execution, per-card completion
records, peer snapshots unchanged, handover verified.

**Status (2026-11-18): build coupling removed; hardware evidence still absent.**
A runtime-selection image cannot know the target count at build time, so the
static "exactly one address" gate no longer applies to it; the runtime
admission (`selected_count != 1` without `GU_ACTUATION_ADMITTED`) is unchanged
and still refuses multi-card actuation without the waiver. The remaining work
is the two-card harness in C1–C4/C9, not code. See §11.2.

### A4. The stock profile is a compile-time selector — **P0**
`port.h:57-72` requires exactly one of `GU_ENTRY_LATCHED_SS1_FULL`,
`…_PRESERVE`, `GU_STOCK_PROFILE_ORIGINAL_03100062`,
`OBSERVED_03000062`; the expected post-write selectors are constants
(`GU_SS0_UNLOCK 0x88888888`, `GU_SS1_UNLOCK 0x8`,
`unlock_610.c:29-31`). A second card whose cold selectors differ, or a machine
whose stock values differ from the chosen variant, cannot use the same image.
*Exit criteria:* profile detection from the latched entry values, with the
per-variant expectations and the same latching discipline
(`profile.c:307-312`).

### A5. Architecture coverage is uneven and partly absent — **P0**
* Turing is refused in prepare (`profile.c:268-271`) *and* ships no payload:
  `TU102-ASSETS/` contains only a README, while `profile.c:27-35` pins hash
  expections for five TU102 files.
* Ampere is admitted (assets pinned, `profile.c:18-26`) but the transaction
  branches it takes are the generic ones, since `is_ad102()` is
  `gpu_is_ada_device()` (`unlock_610.c:353-356`).
* All Ada IDs share the "AD102" permission/readout/WPR offsets
  (`unlock_610.c:1385-1424`, `1715`, `1793`) while AD104/AD106/AD107 SKUs are
  assumed to have identical register geometry. Nothing verifies that per SKU.
*Exit criteria:* per-SKU register tables justified by captured ROM/register
dumps, Turing either implemented with payloads and a signed-loader contract or
refused at *build* time (not only at prepare).

### A6. Firmware assets must be re-read from removable media on every boot — **P0**
`GuBootOpen` refuses any image not launched from USB (`boot_io.c:168`) and
requires a writable `\AD102-UNLOCK` directory; `GuBootLoadAssets`
(`boot_io.c:370-425`) reads five files, including the 84,277,400-byte
`gsp_ga10x.bin`, from the USB root or a build-time-pinned ESP
(`ReadPinnedAssetFromConfiguredEsp`). Hash equality is checked
(`boot_io.c:403`) but no signature is. Operationally: the USB stick is part of
the runtime, not just the installer.
*Exit criteria:* an install step that places assets on the ESP with an
integrity check, or embedding, plus a defined story for the 84 MB payload.

### A7. Nothing persists across boots; the transaction is re-applied every boot — **P1**
`dxe_main.c:18-21`, `:112-125` registers a `ReadyToBoot` event and runs the whole
sequence each boot. There is no marker that the card was already processed, no
idempotency rule beyond the cold-state checks, and `driver_nvram.c` only
manages `Driver####`/`DriverOrder` entries. Whether the stock Windows driver
accepts a re-modified card on the Nth boot is unverified.
*Exit criteria:* a defined contract — either "always re-apply, and here is the
evidence it is safe", or a persisted state record with a documented reset path.

### A8. Recovery is physical only — **P1**
`unlock_610.c:846-857` retains all firmware-visible memory whenever the engine
did not stop; `app.c:198-210` and `dxe_main.c:69-80` halt or power down instead
of recovering. `GuBootOpen` disables the firmware watchdog
(`boot_io.c:214-217`), so a hung transaction has no automatic reset. There is
no rollback, no re-flash path, and no post-failure diagnostic except the USB
log.
*Exit criteria:* documented, tested recovery matrix (mid-transaction power
loss, Stop hang, FLR refusal, partial multi-GPU) with a software-visible state
that survives the failure.

---

## 3. Class B — hardware preconditions that refuse ordinary systems (P0/P1)

All fail closed, but each one is a machine class that cannot run the product.
These are verified from source, not inferred.

| # | Requirement | Evidence | Why it blocks | Pri |
| --- | --- | --- | --- | --- |
| B1 | An **AMD** display function must own the console | `msi_release.c:695` (`if(!amd_count||!display_count) s=EFI_ACCESS_DENIED`), plus `GuDevicePathAmdDisplayOwner` | Intel iGPU consoles and AMD-less systems are refused outright | P0 |
| B2 | Headless mode needs a surviving non-target GOP; non-headless needs exactly one target GOP | `msi_release.c:587-597` | no iGPU + single dGPU (the common gaming build) cannot be prepared | P0 |
| B3 | MSI must be disabled and the function in D0 | `checks.c:66-68` (MSI control bit 0 must be 0), `checks.c:99-102` (PM state must be D0) | firmware or a UEFI driver that left MSI enabled refuses forever | P0 |
| B4 | Unknown PCI capability IDs refused | `checks.c:87-89` (`default: return FALSE`) | a real card exposing any capability outside PM/MSI/MSI-X/PCIe/vendor is refused | P1 |
| B5 | PCIe endpoint v1/v2 only, FLR bit set, link-status bit clear | `checks.c:70-79`, `107-112` | PCIe v3 (or any other layout) refuses | P1 |
| ~~B6~~ | ~~BAR0 exactly 16 MiB at a base that fits the low 32 bits~~ | was `gpu_discovery.c:546-547`, `msi_release.c:773-775` | **fixed in source, unverified on hardware** — the firmware descriptor is authoritative: base decoded as 64-bit from config, any power-of-two window `>= 16 MiB`, config/attribute mismatch refused (§11.3) | ~~P0~~ done |
| ~~B7~~ | ~~Device DMA addresses below 4 GiB~~ | was `port.c:497` with `AllocateAnyPages` | **fixed in source, unverified on hardware** — `AllocateMaxAddress` asks for a sub-4 GiB buffer up front; the mapped-address ceiling check stays as the last line of defence and the refusal is modelled in the simulator (§11.4) | ~~P1~~ done |
| B8 | Bus Master off at admission (one narrow exception) | `checks.c:39-41`, `GuCheckConfigInitialMaster` in single-target mode | a firmware that leaves BME on changes which gate applies | P2 |
| B9 | Image launched from USB with a writable log directory | `boot_io.c:168`, `:176-180` | no fully internal deployment (ESP-resident app) is possible | P1 |
| B10 | Per-call DMA ≤ 128 MiB, 1 MiB ROM buffer, ~80 MB radix table per card | `port.c:469`, `profile.c:289`, `unlock_610.c:1117` | multi-GPU memory footprint is multiplicative (~165 MB/card) and untested | P1 |

---

## 4. Class C — privileged operations that have never been shown safe

| # | Operation | Current state | Evidence | Pri |
| --- | --- | --- | --- | --- |
| C1 | Calling the vendor UEFI driver's `Stop` (3-stage detach) | refused by default for 0x2684/0x220a; the callback may write BAR0 and poll without a proven bound | `msi_release.c:1080-1112`, `:1051-1060`, `Driver()` at `:429-455` | P0 |
| C2 | DMA quiescence after Stop | cleared and re-read, but no IOMMU/SMMU, no engine-queue readback, no bus trace | `msi_release.c:935-940`, `NormalizeMasterAfterStop` | P0 |
| C3 | Firmware acceptance (FWSEC descriptor, ROM) | structural parse + SHA-256 log; `SIGNATURE_AUTHENTICATED=0` | `vbios_610.c:177,213`, `profile.c:78-99` | P0 |
| C4 | Booter ↔ V67 ↔ ROM pairing | five files hash-pinned to 610.43.03; FWSEC image comes from the live ROM, V67 from the bundle; no signature chain ties them | `profile.c:10-40`, `unlock_610.c:1240-1270`, `:1871-1883` | P0 |
| C5 | Post-state verification | proves the registers it wrote (`SS0/SS1`, permissions, PLM `0xffffff8f`/`0x8b8f`, WPR2 closed) | `unlock_610.c:1360-1395`, `:1770-1800` | P1 (by design: it cannot prove the *unlock*) |
| C6 | Time budgets | 20 s boot/PLM (`unlock_610.c:28`), 1 s post-FLR BAR (`:1629-1645`), GFW budget (`:1656`) | on timeout buffers are retained and the box must be power-cycled | P1 |
| C7 | FLR assumptions | requires FLR bit; post-FLR expectations are board-observed values | `checks.c:107-112`, `unlock_610.c:1770-1800` | P1 |
| C8 | PCIe error/training, AER, ASPM, hot-plug, Thunderbolt/USB4 tunnels | unhandled | `checks.c:107-112` covers link status only | P2 |
| C9 | Multi-GPU interference | peer protection is 2× full config-space snapshot per target | `gpu_discovery.c:497-527` | P1 |
| C10 | S3/resume | boot-script writes of SS0/SS1 at resume, needs 16 MiB BAR, no resume verification | `unlock_execution.c:211-260` | P1 |

---

## 5. Class D — release engineering

| # | Gap | Evidence | Pri |
| --- | --- | --- | --- |
| D1 | Artifacts are unsigned; Secure Boot must be off or `AdaUnlock_SecureBoot.cer` enrolled by hand | builders link only (`Build-Experimental-MSVC.ps1:133-141`, `Build-Dxe-Driver.ps1:127-135`); no `signtool` anywhere | P0 |
| D2 | The only actuating build has no CI coverage | CI has no `-AcceptUnverifiedActuation` and no `local/boot_profile.h` | P0 |
| D3 | No provenance for the 84 MB payloads; `verify_reference_assets.py` hashes local copies only | `tools/verify_reference_assets.py` output: `NVIDIA_SIGNATURE_VERIFIED=false` | P1 |
| D4 | MSVC-only toolchain, no EDK II build, installer disabled | `Setup-AdaUnlock.ps1` refuses by design (`Setup.bat`, CI asserts `STATUS=REFUSED`) | P2 |
| D5 | No hardware-in-the-loop; CI proves compilation and host logic only | workflow jobs `host`, `efi` | P0 (for the product claim) |

---

## 6. Class E — evidence the product depends on and does not have

1. Stock Windows 617.42 driver accepts a modified card (E1, P0).
2. Tensor Core numerical correctness after modification (E2, P0).
3. Tensor Core throughput / rate-limiter effect (E3, P1).
4. Recovery matrix, including mid-transaction power loss (E4, P0).
5. Multi-GPU end-to-end on real hardware (E5, P0).
6. BDF stability across firmware updates and slot changes (E6, P1).
7. Driver-update coupling: assets are pinned to the 610.43.03 lineage (E7, P1).
8. AD104/AD106/AD107 and GA102 variants with non-0x2684/0x220a IDs (E8, P1).
9. Thermal/power/long-run stability (E9, P2).
10. Interaction with Secure Boot, BitLocker, anti-cheat (E10, P2).

---

## 7. Class F — what the simulator cannot answer

`docs/HOST_SIMULATOR.md` states the limits; for this inventory the relevant ones
are: one modelled card, no SMMU, no MSI-X vector allocation, no scheduled
interrupts, no real vendor Stop callback, no power/thermal model, no Windows.
The simulator's four scenarios (`verify`) therefore constrain *ordering and
cleanup in our own code*, not any of C1–C10.

---

## 8. Ordered plan to close the inventory

| Stage | Work | Exit criteria |
| --- | --- | --- |
| **S1 — make one machine repeatable** (weeks) | A2 runtime ESP identification; A6 asset install; D1 signing; D2 CI build of the waiver path with a synthetic GUID | one image booted twice on the same machine, both boots verified from the USB log; CI builds and diffs the admitted image |
| **S2 — make selection dynamic** (weeks) | A1 runtime selector + policy; A4 profile detection; B3/B6 relaxation with evidence (MSI state handling, ReBAR/BAR>4 GiB) | one image, two different machines with different BDFs and different stock profiles, each completes without a rebuild |
| **S3 — prove the privileged path** (the long pole) | C1 bounded Stop or a replacement detach; C2 DMA quiescence proof; C3/C4 authenticated firmware acceptance | a written harness that shows the Stop callback returns within a bound, DMA is provably idle, and firmware acceptance is decided by signature, not hash |
| **S4 — multi-GPU** (after S3) | A3 verified multi-target admission; C9 interference testing; memory budget (B10) | two cards, ordered, peers unchanged, handover verified, no retained buffers |
| **S5 — recovery and coverage** (parallel from S1) | A8 recovery matrix; C6 timeout policy; A5 SKU coverage; E1–E5 campaign | documented recovery for each failure class; per-SKU evidence; Windows-side acceptance measured |

S3 is the item that cannot be shortened by more code review: it needs hardware
and vendor-behaviour evidence. S1 and S2 are ordinary engineering and are the
only blockers that are fully solvable in this repository.

---

## 9. What is already sound (do not redo)

* Fail-closed, logged, no-retry gate structure (`port.h`, `gpu_discovery.c`).
* Ownership records: per-platform release state, asset tables, CPU-pool
  ownership, uncertain-release retention (`port.c`, `compat.c`).
* No PROM writes anywhere; the unlock patches a RAM copy (`sim` asserts
  `PROM_WRITES=0`).
* Ordered transaction with per-step completion records and peer config
  snapshots (`gpu_execution_state.c`, `gpu_discovery.c:497-540`).
* Inventory never truncates: `GU_INVENTORY_TOO_MANY` instead of a silent cap
  (`gpu_inventory.c:63-67`).
* The simulator's ordering assertions (`SS0`, `SS1` before `FLR`, WPR2 opened
  once and closed) are the strongest evidence in the repository about our own
  sequencing.

---

## 10. Reproducing this review

```sh
grep -n "GU_ACTUATION_ADMITTED\|GU_PREFLIGHT_ONLY" src/port.h        # gates
grep -n "MULTI_TARGET_WITHOUT_WAIVER" src/gpu_discovery.c           # A3
grep -n "amd_count" src/msi_release.c                               # B1
grep -n "GU_BAR_MIN_BYTES\|AddrRangeMax" src/msi_release.c          # B6 (fixed)
grep -n "AllocateMaxAddress" src/port.c                             # B7 (fixed)
grep -n "return FALSE" src/checks.c                                 # B3-B5
grep -n "AcceptUnverifiedActuation" .github/workflows/*.yml         # D2 (no hits)
python3 tools/run_simulation.py verify                              # F
```

No file under `src/` was modified by the review itself. The fix pass recorded
in §11 does modify `src/`; reproduce it with:

```sh
python3 tools/run_simulation.py verify                        # 9 scenarios, all pass
python3 -m unittest discover -s tests -p "test_*.py"          # host contracts
gcc -std=c11 -O1 -fsanitize=undefined -fno-sanitize-recover=all \
    -Wall -Wextra -Werror -pedantic -DGU_SELECTION_HOST_TEST \
    -Isrc -Itests tests/test_gpu_selection.c src/gpu_selection.c -o /tmp/tgs && /tmp/tgs
```

---

## 11. Fix log — what actually changed (2026-11-18)

Every entry below is **fixed in source, unverified on hardware**. "Verified"
means observed in the host simulator or a host contract test with
`ENVIRONMENT=HOST_SIMULATOR`, `ACTUATION_VERIFIED=0`,
`CAN_DETACH/CAN_UNLOCK/CAN_FLASH/RECOVERY_VERIFIED=0`. No card was touched, so
no entry claims hardware behaviour.

### 11.1 A1 — runtime target selection

| What | Where |
| --- | --- |
| New module: text parser, canonical formatter, variable read/write/delete | `src/gpu_selection.h`, `src/gpu_selection.c` |
| Variable name/GUID (fork-owned) | `GuTargets`, `5c1f2b40-8e21-4ef7-9d2b-7a113c5e8402` |
| Build switch; no `gpu-selection.h` is generated or read | `-RuntimeSelection` in both builders, `/DGU_RUNTIME_SELECTION` |
| Boot-time resolution, `ALL` policy, single-card default | `gpu_discovery.c` `ResolveRuntimeSelection`, `SelectPreparedRecords` |
| Console policy acquisition (auto single, numbered menu, `A`, `S`) | `app.c` `ResolveTargetsOnConsole` |
| Simulator models the variable | `sim/sim_world.c` `SimWorldSetTargets` |

Bounds: 128 targets, 4096-byte text, ASCII, `SSSS:BB:DD.F`, duplicates and
malformed forms refused with no partial list. A `GetVariable` that reports
`EFI_BUFFER_TOO_SMALL` is refused (`EFI_BAD_BUFFER_SIZE`), never truncated.

Evidence: sim scenarios `runtime-target` (binds `0000:01:00.0` chosen from the
variable with **no compiled list in the image**) and `runtime-absent`
(`0000:03:00.0` requested, no card there → refused, unbound platform, no BAR0
write, no FLR). Host unit `tests/test_gpu_selection.c` (80 checks: parser
matrix, 128/129 boundary, round-trip, variable store, oversized and malformed
stored text). Contract `tests/test_runtime_selection_contract.py` (16 tests,
including "a stale three-address `gpu-selection.h` in the include path is
ignored by a runtime-selection build").

Not closed by this: the *policy* still has no per-card record of which card was
already unlocked last boot (A7), and nothing yet proves two real cards.

### 11.2 A3 — multi-target admission

* `port.h`: `GU_RUNTIME_SELECTION` requires the ordered execution mode and is
  rejected next to `GU_SYNTHETIC_COMPILE_ONLY`/`GU_ACTIVE_SEQUENCE_PREPARE_ONLY`.
* `gpu_discovery.c`: the static `GU_GPU_SELECTION_COUNT != 1` gate now belongs
  to the compiled-list branch only. With runtime selection the count is unknown
  at build time, so the **runtime** gate decides: `selected_count != 1` without
  `GU_ACTUATION_ADMITTED` still refuses.
* No new admission path was added: the single switch
  (`GU_ACTUATION_ADMITTED`) and the waiver are untouched, and the synthetic
  profile is still structurally impossible to ship.

### 11.3 B6 — the BAR0 window is read, not assumed

`ReadBarWindow` (`gpu_discovery.c`) validates the firmware ACPI descriptor:
`Desc == 0x8a`, `Len == 43`, zero resource type and translation offset,
power-of-two `AddrLen >= 16 MiB`, base aligned to `AddrLen`, no
`AddrRangeMin + AddrLen` overflow, `AddrRangeMax == Min + Len - 1`. It releases
the descriptor through `FreePool` and refuses if the release cannot be proven
harmful — the release result is logged, never treated as a hardware fault.

`Gu.bar_size`/`bar_base` (from the descriptor, base cross-checked against a
64-bit decode of config `0x10`/`0x14`) are now authoritative in `profile.c`,
`msi_release.c`, `unlock_execution.c` and the S3 gate; the 16 MiB literal is
gone. A config/attribute disagreement is refused, so the change makes discovery
**stricter**, not looser.

Evidence: sim scenarios `rebar` (256 MiB window) and `bar-above-4g` (BAR0 at
`0x1_0000_0000`), both previously refused, now pass discovery and the full
transaction with the image reporting the assigned base/size.

### 11.4 B7 — device DMA under 4 GiB by request, not by luck

`GuDmaAllocate` now calls `AllocateBuffer(AllocateMaxAddress, ...)` with the
32-bit ceiling. The ownership rules are unchanged: any buffer the firmware
writes is retained if the call fails, retained if `Map` fails, and released
only when both `Unmap` and `FreeBuffer` succeed. The mapped-address check
(`dma > 0xffffffff`, alignment, truncation, `bytes-1 > 0xffffffff - dma`)
remains as the last line of defence.

Evidence: sim scenario `dma-high` (the model forces the 32-bit allocator to hand
out `0x1_0000_0000`): the transaction stops before any register write, no WPR2
window opens, no FLR, no fault is recorded, and no buffer is counted as mapped.
`tests/test_platform_bind_lifetime.py` re-executes the real allocator against
11 independent EFI error modes.

Honesty limit: the simulator cannot model the exact `AllocateMaxAddress`
ceiling; it models "32-bit unless the scenario forces 2^32". The firmware
contract that matters (never DMA above 4 GiB) is enforced in code twice.

### 11.6 A2 — runtime Windows-ESP identification

| What | Where |
| --- | --- |
| Structural predicate, no pin | `GuWindowsEspPathCandidate` (`gpu_windows_path.c/.h`) |
| Marker probe that does not latch the boot-file error state | `EspCarriesWindowsManager` (`boot_io.c`) |
| Both ESP sites select the runtime rule | asset reader `ReadPinnedAssetFromConfiguredEsp`, `GuBootLoadWindows` |
| Pin becomes conditional; the pinned default is unchanged | `boot_profile.h` (`GU_RUNTIME_ESP` branch, explicit comment) |
| Build switch | `-RuntimeEsp` → `/DGU_RUNTIME_ESP` in both builders |

The pinned path still demands `local/boot_profile.h` and an exact GUID match;
`GU_RUNTIME_ESP` + `GU_HOST_TEST_PROFILE` is a compile error, so a
synthetic-GUID image cannot be passed off as a runtime-ESP image.

Evidence: `tests/test_gpu_windows_path.c` (32 checks, now including 11 candidate
predicate cases), `tests/test_runtime_esp_contract.py` (12 tests: the pin is
still mandatory by default, the runtime branch carries no pin, both ESP sites
select it, the marker probe never latches `BootFileStatus`, the single-match
refusals survive, and the builders expose the switch). The UEFI sources compile
under `-DGU_RUNTIME_ESP` (checked by building the simulator with the define).

Coverage limit, stated plainly: the simulator does **not** exercise the runtime
ESP policy, because it models the asset volume and the Windows volume as two
directories, so no single modelled volume carries both. The runtime rule is
predicate-tested and compile-checked, not end-to-end tested, and has not been
run on hardware.

### 11.7 Items deliberately not changed

* **A4 (stock profile)** and **A5 (Turing payload)** need per-SKU cold-state
  evidence that this repository does not contain. Choosing a profile or
  emitting a Turing patch from the current constants would be a guess in the
  most destructive part of the path, so nothing was changed there.
* **A8 (watchdog)**: the transaction re-enables no timer today because nobody
  knows its worst-case duration. Arming a watchdog without that bound could
  reset a machine mid-transaction, which is worse than the current hang. Left
  as inventoried until C2/C6 produce the bound.
* **A6/A7 (asset install, persisted state/rollback)**: both require an
  off-line installer and a signature story (D1) that this repository does not
  have yet.

### 11.5 Not touched by this pass

A2 (build-time ESP GUID), A4 (compile-time stock profile), A5 (Turing payload),
A6 (asset install/hash pinning), A7 (persisted state/rollback), A8 (watchdog
recovery), and every class-C item that needs hardware remain exactly as
inventoried above.
