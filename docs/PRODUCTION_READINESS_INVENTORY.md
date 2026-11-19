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

**Status (2026-11-18): partly fixed — the build choice is now enforced.** An
image built for one captured stock value refuses a card presenting another,
before any write (§11.11). The compile-time *selection* remains compile-time:
the profile chooses between code paths guarded by
`#ifdef GU_ENTRY_LATCHED_SS1_FULL`/`_PRESERVE` across `port.c` (`:565`, `:570`,
`:761`), not just a value, and the two entry-latched policies are
indistinguishable before the write, so no probe can choose between them. Turning
that into a runtime choice is a behavioural change in the most destructive part
of the path with no cold-state evidence behind it; guessing here would be exactly
the failure mode the repository's own gates exist to prevent. See §11.11 and
§11.7.

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

**Status (2026-11-18): partly fixed — the unsafe half is now refused, and
Turing is refused when the image is built.** The actuation paths refuse every Ada
id whose register geometry was never captured
(`src/gpu_sku_geometry.[ch]`), so an AD104/106/107 card can no longer be driven
with AD102 offsets; the read-only diagnostic deliberately still runs, because
capturing that geometry is the work needed to add a row. Turing is now refused at
*build* time as well as at prepare: `src/turing_assets.h` fails any build that
defines `GU_TURING_ACTUATION`, because this tree pins five TU102 hashes and
ships no payload bytes, and it pins the reviewed manifest row count so the table
cannot be trimmed while Turing stays admitted (§11.9). What remains open is the data: a captured
table per SKU, and a Turing payload plus a signed-loader contract. See §11.9.

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

**Status (2026-11-18): partly fixed — the install step exists.** An explicit
`GU_ASSET_INSTALL_ONLY` build verifies the pinned bundle from removable media,
copies it to the configured Windows ESP, reads every file back and re-verifies
it, and only then records the bundle in NVRAM (`asset_install.[ch]`,
`GuAssetInstallStage`). The launch rule is unchanged: the image must still be
started from USB, and the record authorizes nothing — it reports
`CURRENT`/`STALE`/`ABSENT`/`CORRUPT` at load. Signature acceptance (C3/C4/D1) is
still open, and so is the 84 MB embed-versus-stream decision. See §11.10.

### A7. Nothing persists across boots; the transaction is re-applied every boot — **P1**
`dxe_main.c:18-21`, `:112-125` registers a `ReadyToBoot` event and runs the whole
sequence each boot. There is no marker that the card was already processed, no
idempotency rule beyond the cold-state checks, and `driver_nvram.c` only
manages `Driver####`/`DriverOrder` entries. Whether the stock Windows driver
accepts a re-modified card on the Nth boot is unverified.
*Exit criteria:* a defined contract — either "always re-apply, and here is the
evidence it is safe", or a persisted state record with a documented reset path.

**Status (2026-11-18): partially fixed in source — the record exists, the
contract does not.** Both entrypoints now write a fixed-size NVRAM record in two
phases (`INTENT` before the first mutation, `OUTCOME` after) and report it at
the next boot, so an interrupted transaction is visible instead of invisible.
The record is deliberately **inert**: nothing branches on it, because whether
re-application is safe is unproven. "Always re-apply" therefore remains the
behaviour, and this item stays open until C1–C4 produce the evidence. See §11.8.

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

**Status (2026-11-18): the mechanism exists; no bound is claimed.** The
software-visible state from A7 (above) now survives a failure, and a build can
now bound the firmware timer: it is armed once in `GuBootOpen` for a build that
sets `GU_WATCHDOG_SECONDS`, and cancelled before the handover, so a hang inside
the sequence resets the machine instead of freezing it with a half-written card
(§11.12). The default build arms nothing, because no worst-case duration has been
measured (C2/C6) — a timer shorter than the transaction would reset a machine
mid-write, which is strictly worse than the current hang. Recovery itself remains
physical.

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
| C4 | Booter ↔ V67 ↔ ROM pairing | five files hash-pinned to 610.43.03; FWSEC image comes from the live ROM, V67 from the bundle; no signature chain ties them. A per-card allowlist now exists for the ROM half (§11.13) but carries no references, and it is a digest, not a signature | `profile.c`, `prom_reference.c`, `unlock_610.c` | P0 |
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
| **S1 — make one machine repeatable** (weeks) | A2 runtime ESP identification (done, §11.6); A6 asset install (done, §11.10); D1 signing (open); D2 CI build of the waiver path (**diff done, §11.14**; synthetic-GUID build open) | one image booted twice on the same machine, both boots verified from the USB log; CI builds and diffs the admitted image |
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

### 11.8 A7 — a record that survives a failed boot

| What | Where |
| --- | --- |
| Fixed-size validated record, two phases | `src/boot_state.h`, `src/boot_state.c` |
| Variable `GuBootState`, fork GUID `2a7d54c1-5b3e-4a19-8f60-9d21c4b7e803`, NV+BS+RT | `boot_state.c` |
| Reported once per boot for every entrypoint | `boot_io.c` (`GuBootOpen`) |
| Intent before the first mutation, outcome after the sequence | `app.c`, `dxe_main.c` |
| Reset path | `GuBootStateDelete` (delete the variable) |

Validation: exact size, magic, known phase, PCI field ranges, zero reserved
bytes. A short, long, or corrupt record is `EFI_COMPROMISED_DATA`, logged as
`BOOT_STATE PRESENT=0 READ_STATUS=... IGNORED=1`, and never repaired during
boot. Write failures are cast to void at both call sites, so a record problem
can never change a transaction's result.

Evidence: `tests/test_boot_state.c` (41 checks: validation matrix, phase
transitions, count advances only on outcome, write failure is inert, delete),
`tests/test_boot_state_contract.py` (10 tests: no control transfer depends on
the record, intent precedes the sequence, one read covers every entrypoint), and
the simulator runs all 9 scenarios with the record linked and reported.

What this does *not* do: it does not skip a card that was already processed, it
does not roll anything back, and it does not make re-application safe. Those
need the C1–C4 evidence.

### 11.9 A5 — one chip's offsets are not every chip's offsets

| What | Where |
| --- | --- |
| Captured-geometry table, one row per admitted Ada id | `src/gpu_sku_geometry.c` |
| `Refuses()` — the only predicate the gates use | `gpu_sku_geometry.h` |
| Refused at prepare, before any BAR access | `profile.c`, `GuAd104Prepare` |
| Refused in the vendor core before the one-shot attempt is consumed | `unlock_610.c`, `gpu_unlock_610_unlock` |
| AD102-only branches require a captured table | `unlock_610.c`, `is_ad102` |

The table classifies all 11 Ada ids the image admits. Only the reference AD102
pair (`0x2684`, `0x2685`) is marked captured; every AD103/104/106/107 id is
refused, and the device is not driven with offsets taken from another chip. The
refusal is reported as
`SKU_GEOMETRY=REFUSED DEVICE=2786 CHIP=AD104 SKU=RTX 4070 REASON=NO_CAPTURED_REGISTER_TABLE DIAGNOSTIC_ALLOWED=1`,
returns `EFI_UNSUPPORTED`, and leaves the platform untouched
(`PROFILE_VERIFIED=0 PROM_VERIFIED=0`, zero BAR writes, zero FLR, WPR2 unopened).
The read-only diagnostic does not consult the table: it is the mechanism by which
the missing capture is obtained.

Evidence: `tests/test_sku_geometry.c` (330 checks: table covers exactly
`GU_ADA_DEVICE_IDS`, complements hold, non-Ada ids are not answered by the gate,
no duplicate rows), `tests/test_sku_geometry_contract.py` (21 tests: refusal
precedes every hardware read and the state check, the vendor path refuses before
consuming the attempt, the diagnostic stays open, build wiring), the simulator
scenario `prepare ad104-sku` (`SIM_VERIFY=PASS SCENARIOS=10`), and host case 13 in
`tests/test_gpu_asset_lifetime.py` (the refusal costs zero reads).

Turing is refused twice over, which is the second half of the A5 exit
criterion:

| Where | What it refuses |
| --- | --- |
| `src/turing_assets.h` (compile time) | a build defining `GU_TURING_ACTUATION` — there is no payload to stage or hash, so such an image cannot exist |
| `src/turing_assets.h` (compile time) | a Turing manifest whose row count drifts from the reviewed five, so trimming the table cannot leave Turing cards admitted |
| `profile.c`, `RegistersFor` | a Turing card whose cold state does not match the passive predicate (`TURING_COLD=REFUSED`, `ACTIVE_ALLOWED=0`) |
| `profile.c`, `GuAd104Prepare` | the actuation path for any Turing card |

Evidence: `tests/test_turing_build_gate.py` (5 tests that compile a translation
unit including the header and require the compiler to fail with the explanation,
require it to succeed without the flag, and require a trimmed manifest to fail —
the tripwire is demonstrated, not asserted from source text).

What this does *not* do: it does not make any captured table a verified
transaction. `ACTUATION_VERIFIED=0` still holds for the reference AD102, and the
AD104/106/107 rows stay empty until a dump exists. Ampere keeps its documented
generic path, unchanged by this gate. And the build gate says nothing about
whether the pinned Turing hashes are right — only that a Turing actuation image
cannot be produced from this tree.

### 11.10 A6 — the bundle can be staged once, and says so

| What | Where |
| --- | --- |
| Fixed-size validated record, `GuAssetInstall` variable | `src/asset_install.[ch]` |
| Bundle digest over names, sizes and pinned hashes | `GuAssetInstallBundleDigest` |
| `ABSENT`/`CURRENT`/`STALE`/`CORRUPT` decision | `GuAssetInstallState` |
| Staging build: verify → copy → read back → record | `boot_io.c`, `GuAssetInstallStage` |
| Entrypoint, never reaches prepare or the sequence | `app.c` (`GU_ASSET_INSTALL_ONLY`) |
| Report at open and at load | `boot_io.c` (`ASSET_INSTALL PRESENT=…`, `STATE=…`) |

Every file is verified against its pinned SHA-256 before it is written, written
in ≤1 MiB pieces, flushed, closed, and then read back and hashed again; the
record is written only after all five succeed. An existing file is accepted only
when it already holds the pinned bytes — a mismatch is refused
(`REASON=… EXISTING_MISMATCH=1`), never overwritten, so an interrupted install is
resumable and a foreign file is not silently replaced. The installer refuses
when the image was not launched from removable media, or when any GPU state is
non-idle, and it touches no BAR: zero writes, zero FLRs.

Evidence: `tests/test_asset_install.c` (112 checks: digest determinism and
sensitivity, validation matrix, CURRENT/STALE/CORRUPT, NVRAM round trip, refused
write is inert), `tests/test_asset_install_contract.py` (24 tests: the launch
rule is not relaxed, only the install build stages, every file is verified twice,
no overwrite and no delete, the record is the commit point, the pinned version is
singular), the simulator scenario `prepare install-assets` (stages 5 files,
reports `CURRENT`, repeated install is idempotent, zero BAR writes), and
`tests/test_gpu_asset_lifetime.py` (host load path over the staged report).

What this does *not* do: it does not make the ESP a place the image may be
launched from (that guard is the only thing preventing unattended re-application,
whose safety is unproven), and it does not authenticate the vendor signature — a
digest proves these bytes, not who signed them. Both remain C3/C4/D1.

### 11.11 A4 — a card the image was not built for is refused

The compile-time profile is still compile-time. What changed is that the choice
is now a *precondition* rather than a comment: `GU_STOCK_PROFILE_SS0` was a
constant the image carried and never compared against anything, so a build for
one card would run its destructive path on another card presenting a different
cold value.

| Piece | Where | What it does |
| --- | --- | --- |
| Predicate | `src/stock_profile.h` | `GuStockProfileAccepts(Observed, HasExpected, Expected)` — a build with no expectation accepts, a build with one compares |
| Gate | `src/profile.c`, `RegistersFor` | after the selector sanity rules, before anything else; sets `StockProfileMismatch` and logs, never faults |
| Refusal | `GuAd104Prepare` | returns `EFI_UNSUPPORTED` with the card untouched |
| Message | log | `STOCK_PROFILE=REFUSED COMPILED=… EXPECTED_SS0=… OBSERVED_SS0=… REASON=CARD_DOES_NOT_MATCH_BUILD`; a match logs `STOCK_PROFILE=MATCH` |
| Linkability | `src/port.c`, `src/port.h` | the entry-latch API is declared for every profile, with an explicit no-op branch for the policies that keep no latch |
| Selector step | `src/vendor/unlock_610.c` | the four-phase selector machine runs only in `_FULL`; every other policy refuses the step instead of writing selectors the port layer would fault on |

That last row is a finding, not a nicety: before this pass three of the four
profiles **did not link**. The latch API and the selector phase were
unconditional call sites with `#ifdef`-guarded definitions, so
`entryLatchedSs1Preserve`, `original03100062` and `observed03000062` compiled and
then failed at link time — the "one image, four profiles" matrix was really one
image, and nothing in CI ever built the other three.

Linking them exposed a second half of the same defect: the *transaction* existed
only in the full policy. The other three linked and then refused the selector
step, so they could not perform an unlock at all. Each policy now has its own
selector contract, and the checks that follow the write compare against that
contract instead of a constant:

| Profile | Selectors | Follow-up checks | Log |
| --- | --- | --- | --- |
| `entryLatchedSs1Full` | SS0, then one SS1 write, authorised by the phase machine | SS1 must be `0x8`, phase must be `TARGET` | `SELECTOR_PATH=PHASED` |
| `entryLatchedSs1Preserve` | SS0 only; SS1 is refused by the port layer and left as found | SS1 must equal the latched entry value | `SELECTOR_PATH=PRESERVE` |
| `original03100062`, `observed03000062` | both, in stock order, no phase | SS1 must be `0x8`; no phase to check | `SELECTOR_PATH=UNTRACKED` |

Three call sites had to become policy-aware for that to be true: the pre-FLR
comparison, the post-FLR comparison, and the pre-close silicon check in
`msi_release.c` — each expected `SS1==0x8`, so a preserve build faulted after its
own reset. The non-latched policies still have no protection against a repeated
SS1 write; that guard is full-policy only, and the build says so rather than
implying otherwise.

Evidence: `tests/test_stock_profile.c` (12 checks, gcc and clang under UBSan:
mismatch refuses, match accepts, no expectation accepts, no fault, message
fields), `tests/test_stock_profile_contract.py` (14 tests: placement after the
sanity rules, read-only, `EFI_UNSUPPORTED`, message fields, profile table, no
runtime NVRAM selector), `tests/test_stock_profile_matrix.py` (8 tests: the
matrix builds and links every profile, and each policy keeps its own selector
contract), and four simulator rows — `prepare stock-match` and `prepare
stock-mismatch` differ only in the card (the mismatch row reports
`STATUS=8000000000000003 BAR_WRITES=0 FLR=0` and names the build), and
`transaction preserve` / `transaction untracked` run a full transaction under
those policies: preserve reports `SEL_WRITES=1:0` (one SS0 write, no SS1 write)
and still completes, untracked reports `SEL_WRITES=1:1` in stock order.

What this does *not* do: it does not choose a profile at runtime. The two
entry-latched policies are indistinguishable before the write — that is what
"entry-latched" means — so no probe can pick between them, and the expectations
for the non-latched policies do not exist in this repository. Operator selection
with the choice recorded in the A7 record (§11.8) is still open.

### 11.12 A8 — the firmware timer can now be bounded

`GuBootOpen` used to disable the firmware timer and never re-arm it, so a hang
inside the sequence left the machine frozen with a half-written card. The timer
is now programmed in exactly one place and cancelled at one place:

| Piece | Where | What it does |
| --- | --- | --- |
| Threshold | `GU_WATCHDOG_SECONDS` | build constant, default `0` = arms nothing, disables the timer exactly as before |
| Arm | `WatchdogProgram`, called from `GuBootOpen` | `seconds × 10,000,000` in the firmware's 100 ns unit, reset code `0` |
| Sanity | `GU_WATCHDOG_MAX_SECONDS` (3600) | a longer value is refused with `REASON=IMPLAUSIBLE_THRESHOLD` rather than armed |
| Cancel | `GuBootWatchdogCancel`, called from `app.c` | after the outcome is recorded, before `GuBootWindows`; logs `WATCHDOG=CANCELLED SECONDS=…` |
| Record | log | `WATCHDOG BOUND=… ACTIVE=1` or `REASON=NO_BUILD_THRESHOLD`, before the first mutation |

The default build is unchanged on purpose: no worst-case duration has been
measured, and a timer shorter than the transaction resets a machine mid-write,
which is worse than the hang it replaces. A prepare-only build returns before the
arm site, so it still touches neither the timer nor any boot state.

Evidence: `tests/test_watchdog_contract.py` (10 tests: two programming sites,
arm inside `GuBootOpen` after the prepare-only return, sanity check before
programming, 100 ns conversion, report helper is read-only, cancel reports and
clears, cancel after the outcome and before Windows, log states whether a bound
was in force), and the simulator row `transaction watchdog`, which builds with
`GU_WATCHDOG_SECONDS=600` and reports `SIM_WATCHDOG ARMS=1 BOUND=600
CANCELLED=1` after a transaction that still completed. The default build is
pinned too: the other transaction rows assert that nothing was armed.

What this does *not* do: it does not choose a threshold. That needs the measured
budgets from C2/C6, and it does not attempt recovery — the timer's only job is to
turn a hang into a reboot that the A7 record then explains.

### 11.13 Multi-GPU step 2 — per-card firmware acceptance

The structural PROM check answers a weak question: is this 1 MiB readout a valid
VBIOS naming this PCI device. Any card of the right family can present one, and
the log has always said `SIGNATURE_AUTHENTICATED=0`. Step 2 of the multi-GPU plan
asks for an authenticated per-device reference instead, which is the part that can
be settled without hardware.

| Piece | Where | What it does |
| --- | --- | --- |
| Allowlist | `src/prom_reference.c` | static const table of (device id, subsystem id, ROM SHA-256, loader version); **empty in the shipped image** |
| Decision | `GuPromReferenceDecide` | exact hit on all four, or one of `UNKNOWN_CARD`, `DIGEST_MISMATCH`, `LOADER_MISMATCH`, `MALFORMED` |
| Requirement | `GU_PROM_REFERENCE_REQUIRED` | a miss becomes a refusal; a build without it keeps the structural check and says so |
| Preparation | `profile.c`, `GuAd104Prepare` | passes `TRUE`: with the requirement, a miss is `EFI_UNSUPPORTED` and nothing is written |
| Transaction | `unlock_610.c`, FWSEC step | passes `TRUE` as well: the same requirement before the first write |
| Diagnostic | `profile.c`, `GuDiagnosticScan` | passes `FALSE`: it reports the digest, device and subsystem, which is how a reference is captured in the first place |
| Message | log | `PROM_IDENTITY=VERIFIED …` or `PROM_IDENTITY=REFUSED DEVICE=… SUBSYS=… SHA256=… REASON=… REFS=… DIAGNOSTIC_ALLOWED=1` |

**No reference is committed, and that is the finding.** This repository contains
no card dumps, so the allowlist is empty and a build with the requirement admits
nothing. That is the fail-closed default doing its job rather than a defect: a
digest invented to make the admitted path look green would be worse than no
reference at all, because it would assert provenance nobody observed.

The one entry that exists is synthetic and cannot ship: `GU_PROM_REFERENCE_SYNTHETIC`
pins the *simulator's* model ROM, is rejected with `#error` in any unhosted build
or next to `GU_SIMULATED_ACTUATION`, and names itself in its `Origin` field. It
exists so the admitted path is executed rather than merely compiled.

What this does not do: it does not authenticate a signature. Both decision paths
keep `SIGNATURE_AUTHENTICATED=0`, and the Windows 617.42 Booter provenance
comparison is still mandatory evidence that nothing here provides. It also does
not replace the per-target admission gates; it narrows what counts as "the right
card" inside them.

Evidence: `tests/test_prom_reference.c` (two builds of the same module: the
synthetic table accepts the exact tuple and refuses wrong device, wrong subsystem,
wrong digest, wrong loader, short/long/uppercase/non-hex digests; the shipped
table admits nothing including the synthetic identity), `tests/test_prom_reference_contract.py`
(17 tests: no runtime registration, empty table, exact matching, requirement is
compile-time and applied at preparation and in the transaction but not in the
diagnostic, `EFI_UNSUPPORTED` mapping, message fields, no authentication claim,
synthetic cannot reach firmware, one version literal), and two simulator rows:
`prepare prom-refused` (`STATUS=8000000000000003 BAR_WRITES=0 FLR=0`,
`REASON=UNKNOWN_CARD REFS=0`) and `prepare prom-admitted`
(`PROM_IDENTITY=VERIFIED`).

### 11.14 S1/D2 — the admitted image is built and diffed

S1's exit criteria end with "CI builds and diffs the admitted image", and D2 asks
for a CI build of the waiver path. Neither had a mechanism: the workflow built
preparation-only images, and `GU_ACTIVE_UNVERIFIED_ACTUATION` - the single switch
that turns "refuses to actuate" into "may attempt the unverified transaction" -
was described only in a runbook.

`tools/waiver_build_diff.py` builds every source in the image twice from the same
tree and diffs each translation unit:

| Compared | Expected |
| --- | --- |
| Preprocessor defines | exactly two deltas: `GU_ACTIVE_UNVERIFIED_ACTUATION` appears, `GU_ACTUATION_ADMITTED` flips `0 -> 1`; anything else is an unreviewed change and fails |
| Defined object symbols | none, in either direction: the waiver flips a gate inside the same program, it does not compile a different one |
| Reach | `GU_ACTUATION_ADMITTED` must flip in at least one unit, so a waiver that admits nothing cannot pass as evidence |

Current result: `WAIVER_DIFF=PASS SOURCES=26 MACRO_DELTAS=41 ADMITTED_UNITS=15
SYMBOL_DELTAS=0`. The 41 deltas are the flag reaching all 26 units plus the
admission constant in the 15 that include `port.h`.

What this does not do: it is a hosted GCC build of the same sources with the
simulator's define shim, so it witnesses source shape, not that MSVC/EDK II emit
the same image. It also does not build with a synthetic GUID, the second half of
D2 - that is a build-script change in `src/Build-Experimental-MSVC.ps1`, which
cannot be executed here and is left open rather than claimed.

Evidence: the tool itself (CI runs it), `tests/test_waiver_build_diff.py` (9
tests: the diff passes, a missing compiler is a skip rather than a pass, the pin
is exactly the reviewed pair, no unit may gain or lose code, the profile matches
the runbook, every compiled source is covered).

### 11.7 Items deliberately not changed

* **A4 (stock profile)** and **A5 (Turing payload, per-SKU tables)** need
  per-SKU cold-state evidence that this repository does not contain; A5 now
  refuses what it cannot drive (§11.9). Choosing a profile or
  emitting a Turing patch from the current constants would be a guess in the
  most destructive part of the path, so nothing was changed there.
* **A8 (watchdog)**: the arm/cancel mechanism exists but no threshold is
  chosen (§11.12). Nobody knows the worst-case duration, and arming a timer
  shorter than it would reset a machine mid-transaction, which is worse than the
  current hang. The default build still arms nothing until C2/C6 produce the
  bound.
* **A6/A7 (asset install, persisted state/rollback)**: the install step and the
  boot record now exist (§11.8, §11.10); what remains is the signature story
  (D1) and any behaviour that would skip or roll back a transaction.

### 11.5 Not touched by this pass

A2 (build-time ESP GUID), A5 (Turing payload), A6 (asset install/hash
pinning), A7 (persisted state/rollback), A8 (watchdog recovery), and every
class-C item that needs hardware remain exactly as inventoried above, except
where §11.11 and §11.12 record what did change.
