# Dynamic multi-GPU refactor review and active-execution gate analysis

**Status of this document:** review record for the refactor inventory. It
distinguishes *completed source work with host evidence* from *remaining
review items* and from *hardware qualification*, which no source change here
establishes.

**Scope of the code changed in this pass:** resource-ownership and lifetime
contracts in the passive diagnostic and CPU-inventory layers, diagnostic
completeness reporting, benchmark result attribution, and — after the owner
reviewed §4.1 and asked for it — one explicit, non-default operator waiver
that opens the admission gates G1–G4 in §4.1.

**What the waiver is not:** a verification result. Nothing in this repository
observes a GPU, so a waived image is *allowed to attempt* the firmware
transaction and is still *unproven*. Eligibility remains `CAN_DETACH=false`,
`CAN_UNLOCK=false`, `CAN_FLASH=false`, `RECOVERY_VERIFIED=false`, and every
waived artifact and log line says so.

---

## 1. What changed, and the evidence for it

| # | Refactor item | Source change | Host evidence |
|---|---------------|---------------|---------------|
| 1 | `gpu_pci_observation.c` used an error-only status check for borrowed protocol references and observation results | Every result is now classified as *exact `EFI_SUCCESS`*, *warning* (`EFI_ERROR == 0 && status != EFI_SUCCESS`) or *error*. A warning is never accepted as observed evidence; it is counted in `warnings`, with `first_warning_stage/index/status`, and still fails the capture closed. | `tests/test_gpu_pci_observation.c`: warning cases for `Pci.Read`, device-path `HandleProtocol`, `OpenProtocolInformation` and `FreePool` (68 checks) |
| 2 | `gpu_pci_observation.c` did not reject all duplicate references to live buffers | `PoolProblem()` now rejects a reference that overlaps the caller's live capture (`uncertain_alias`) or that repeats an already-uncertain address (`uncertain_duplicate`). Such a reference is never published, dereferenced, freed or retried. The handle array is checked the same way before it is walked. | Same test: capture-alias negative, shared-buffer duplicate negative, and a two-distinct-allocation positive control |
| 3 | `gpu_inventory.c` selection interface documented non-overlap but never established it | `GuFindGpuRecordSequence()` validates record/location/index ranges with overflow-aware half-open comparison before writing any output, returning the new `GU_INVENTORY_OVERLAP` code. `GuCollectGpuRecords()` refuses a count output that lies inside the record storage. | `tests/test_gpu_inventory.c`: three overlap negatives, a count-in-records negative, adjacent-disjoint positive control (157 checks) |
| 4 | `gpu_inventory.h`/`gpu_discovery.h` legacy fixed-capacity interface review | The 16-record structure is documented as a bounded-caller compatibility view; boot discovery is asserted to use runtime-sized pool storage. The bound itself is *not* removed. | `tests/test_legacy_inventory_boundary.py` (5 tests) and the unchanged 18-target host builds |
| 5 | `gpu_topology.c` assessment used caller report storage initialized after validation | `GuTopoAssess()` now rejects report/storage/capture ranges that overlap, before the first output byte is written, so a partially initialized report cannot be mistaken for an assessment. | `tests/test_gpu_topology.c`: storage-overlaps-report, storage-overlaps-capture, report-overlaps-capture negatives and a populated positive control (243 checks) |
| 6 | `gpu_topology_efi.c` could omit an invalid or oversized diagnostic line without saying so | `Say()` refuses the whole line (never a partial line), counts it in `LogDropped`, and publishes one bounded `DIAGNOSTIC_LINE_DROPPED=1 CONTENT_WITHHELD=1` marker. A trailing `DIAGNOSTIC_LOG_COMPLETENESS=COMPLETE\|INCOMPLETE DROPPED=<n> OVERFLOW=<0\|1>` line is emitted after every other line and before the durable write. The supplementary PCI report also prints `PCI_OBS_CLASSIFICATION` (warnings, aliases, duplicates) and one `PCI_OBS_POOL_UNCERTAIN[...]` line per retained buffer, so item 1's new categories reach the durable log. | `tests/test_diagnostic_log_completeness.py`: the extracted `Say`/`SayDropped`/`SayCompleteness` block compiled with and without `GU_TOPO_SAVE_LOG` (4 tests); plus a `-fshort-wchar` syntax build of the real `gpu_topology_efi.c` and `gpu_pci_observation.c` against the vendored EDK II headers in both log variants |
| 7 | `gpu_prom_capture.c` depended on caller output memory and callback lifetime | The capture refuses a reader record or caller context that lies inside the 1 MiB output window, before the first write, leaving the caller's storage untouched. Invalid arguments no longer publish or modify the output. | `tests/test_gpu_prom_capture.c`: reader-inside-window, context-inside-window, same-context-outside-window positive control (29 checks) |
| 8 | Active-execution admission gates G1–G4 (see §4.1) | New `GU_ACTIVE_UNVERIFIED_ACTUATION`, emitted only by `-AcceptUnverifiedActuation`. It forces `GU_PREFLIGHT_ONLY 0`, admits 1..N ordered targets instead of exactly one, and replaces the `0x2684`/`0x220a` MSI-release refusal with a recorded admission. `port.h` `#error`s it without `GU_AD104_TRANSACTION` + `GU_ACTIVE_SINGLE_TARGET`, and with any preparation-only, synthetic or host-test profile. | `tests/test_unverified_actuation_waiver.py` (23 tests): header matrix through the real compiler, the three-target compile gate, branch-exclusivity after preprocessing, builder status/name contracts, and a repository-wide check that no source claims eligibility |
| 9 | `benchmark_4090.py` silently measured CUDA device zero | The script is explicitly legacy (`LEGACY_SINGLE_DEVICE_SCRIPT=1`, `LEGACY_SINGLE_DEVICE=1`), takes an explicit `--device` ordinal, refuses invalid/out-of-range ordinals instead of falling back, and prints `CUDA_INDEX`, `NAME`, `PCI_BUS_ID` and `ATTRIBUTION=MEASURED_NOT_VERIFIED`. `tools/benchmark_all_gpus.py` additionally reports PCI bus id and UUID per device. | `tests/test_benchmark_attribution.py` (9 tests) |

All of the above compile clean under `-std=c11 -Wall -Wextra -Werror
-pedantic` and execute; the modified C is also exercised under
`-fsanitize=undefined -fno-sanitize-recover=all`. New tests are wired into
`.github/workflows/gpu-topology-diagnostic.yml`.

---

## 2. Remaining refactor inventory — current status

Status meanings: **Done** = source contract implemented and host-verified;
**Review** = inspected in this pass, no code defect found, residual risk
recorded; **Open** = still needs work before any active claim.

### A. Dynamic discovery and CPU records

| Item | Status | Note |
|------|--------|------|
| Dynamic PCI discovery | Done | Runtime-sized pool storage partitioned by a union quantum; single allocation per census. |
| Independent GPU identity records | Done | BDF + vendor/device + subsystem + class + handle + protocol reference. |
| Ordered GPU selection | Done | `GuFindGpuRecordSequence` + `GuGpuExecInit/CheckNext`. |
| Runtime-sized preparation storage | Done | Capacity from observed handle count, bounded at 4096 handles. |
| Duplicate device identity rejection | Done | Location, handle and PCI-interface aliases rejected. |
| Passive workspace isolation | Done | Record/location/index overlap is now enforced in the shared selector, not assumed. |
| Legacy fixed-capacity interface | Done (bounded, retained) | 16-record API kept for bounded callers only; boot entrypoint asserted not to use it. |
| Independent record disposal verification | Review | Pool ownership transfer (`BindPreparedSequence`) and refusal-path disposal are covered by `test_gpu_discovery.c`; retention on uncertain release is exercised. No new defect found. |

### B. CPU resource ownership

| Item | Status | Note |
|------|--------|------|
| Independent platform allocation records | Done | `GU_PLATFORM_ALLOCATION` registry; one entry per PCI protocol. |
| Owner-specific CPU allocation interfaces | Done | `GuAllocatePlatform` / `GuBindAllocatedPlatform` / `GuDiscardUnboundPlatform`. |
| Compatibility device record isolation | Done | `compat.c` reserve/open/owner checks against the active platform. |
| Passive allocation registry integrity | Review | `RecordPlatformRegistryFailure` preserves the first ambiguous pool status/address; publication is irreversible per boot image. No new defect found; a registry address is never reused. |
| Stale CPU reference rejection | Review | Every guarded helper re-compares `owner == GuPlatformActive` after each EFI call. |
| Original-owner failure attribution | Review | `GuCompatOwner()` identity is checked before a firmware transaction continues. |
| Temporary diagnostic buffer lifetime | Review | `profile.c` PROM/temporary buffers are allocated, copied, compared and released inside one function on every path. |
| Independent asset buffer ownership | Review | `boot_io.c`/`profile.c` keep per-platform asset tables; fixed names/sizes remain a *content* limitation (Section 4), not a lifetime defect. |
| Callback lifetime verification | Done (passive) | `gpu_prom_capture_efi.c` descriptor ownership and `gpu_topology_efi.c` opener-buffer release are confirmed-reference-only paths. |

### C. Passive topology and display observation

| Item | Status | Note |
|------|--------|------|
| Runtime-sized graphics inventory | Done | GOP storage allocated from the observed GOP count with an explicit capacity. |
| Independent topology report storage | Done | Report storage is separately allocated and released after the verdict is printed. |
| Exact duplicate buffer rejection | Done | GOP/report alias checks plus the new `GuTopoAssess` range checks. |
| Supplementary observation cleanup | Done | Warning vs error classification; single uncertain reference per buffer; capture aliasing rejected. |
| Remaining overlapping memory-range checks | Done | `GuTopoAssess`, `GuFindGpuRecordSequence`, `GuCollectGpuRecords`, `GuPromCapture`. |
| Borrowed protocol status consistency | Done (observation), Review (EFI) | The EFI capture paths already require `status == EFI_SUCCESS`; the collector now matches them. |
| Incomplete observation reporting | Done | Dropped lines are counted and reported; truncation and pool uncertainty already had explicit lines. |
| Passive display resource lifetime | Review | No passive diagnostic acquires a display resource it must release; `msi_release.c` release lifetime is an *active* concern (Section 4). |

### D. Boot-wide resource ownership

| Item | Status | Note |
|------|--------|------|
| Confirmed diagnostic file references | Done | `LogOpenResult`/`CloseLogFile` confirm ownership before use; uncertain handles are retained. |
| Diagnostic file uncertainty retention | Done | Uncertain close/open results set a fault and stop further file work. |
| Isolated descriptor resource ownership | Done | PROM EFI adapter allocates its own descriptor and never returns a borrowed handle. |
| Shared filesystem resource review | Review | `boot_io.c` closes the volume, file and pinned Windows image exactly once per path; refusal paths close before returning. |
| Boot image resource lifetime review | Review | `gpu_windows_handoff.c` uses a pinned ESP GUID and does not rewrite `BootOrder`; no ownership regression found. |
| Boot event reference lifetime | Review | `dxe_main.c` signals once, closes the event before execution, and on close uncertainty keeps the reference and permanently refuses the callback. |
| Application-level failure containment | Review | `app.c` and `dxe_main.c` separate *refusal before mutation* (log, close, return) from *containment after mutation* (clear master, halt/shutdown). No retry, no fallback. |

### E. Independent verification

| Item | Status |
|------|--------|
| Simulated same-model GPU identity isolation | Done |
| Simulated arbitrary GPU selection | Done |
| Simulated 18-target preparation | Done |
| Independent allocation failure tests | Done |
| Successful but overlapping memory references | Done (new, both inventory and observation layers) |
| Diagnostic callback lifetime tests | Done |
| Independent diagnostic result completeness | Done (new) |
| Benchmark device identity attribution | Done (new) |
| Host test workflow | Passes locally with GCC; CI runs GCC and Clang |
| Windows firmware compilation workflow | Unchanged by this pass; requires the MSVC CI job |

---

## 3. What "review" did *not* change

The following were inspected and deliberately left as-is:

- **The 4096-handle and 96-record resource bounds.** They are resource
  bounds, not device-count policy. Nothing in this pass suggested they are
  reached on supported hardware.
- **The 16-record compatibility inventory.** It is still used by bounded
  callers and host fixtures. Removing it would be an interface break with no
  evidence of benefit; it is now documented and asserted to be off the boot
  path.
- **The 1 MiB PROM window and the `PROM.BIN` reader semantics.** Unchanged
  and still not linked into any EFI image.
- **Every fail-closed refusal in the active path.** See Section 4.

---

## 4. Full active hardware execution — gate analysis

The repository currently has **one** verified physical actuation path: the
isolated MSI Stop-to-Windows experiment, which calls the *original* NVIDIA
driver's `DisconnectController` for the RTX 4090 under an explicit risk
acceptance, with a pinned Windows handoff (`docs/MSI_4090_STOP_ONLY_EXPERIMENT.md`).
That is **not** the unlock path. The unlock path is gated at six independent
places. At the owner's explicit request, four of those refusals can now be
lifted by **one** non-default build switch, `-AcceptUnverifiedActuation`
(`GU_ACTIVE_UNVERIFIED_ACTUATION`). The switch is an *acceptance*, not a
verification result: it changes what the image is allowed to attempt, and
changes nothing about what has been proven.

### 4.1 Gate map

| Gate | Location | What it prevents | State by default | State with the waiver |
|------|----------|------------------|------------------|------------------------|
| G1 | `gpu_discovery.c::GuDiscoverSingleGpu` (`GU_ACTIVE_SEQUENCE_PREPARE_ONLY`) | One or more selected GPUs reaching `GuInit` | Preparation-only builds revalidate every PCI identity read-only, then return `EFI_UNSUPPORTED` before `GuInit`. The whole block is compiled out of an active build. | Open. The ordered coordinator admits 1..`GU_INVENTORY_MAX_HANDLES` selected targets; each step requires its predecessor's completion record. |
| G2 | `port.h` `GU_PREFLIGHT_ONLY` (1 for prepare-only/synthetic builds) | GPU MMIO writes, DMA mappings, FLR and PCI config writes in those builds | Write/DMA/FLR helpers compile to refuse unless the build is a real transaction mode. | Open: the waiver forces `GU_PREFLIGHT_ONLY 0`. The waiver itself is rejected next to prepare-only, synthetic and host-test profiles. |
| G3 | `gpu_discovery.c` `GU_ACTIVE_SINGLE_TARGET` + `GU_SYNTHETIC_COMPILE_ONLY` | A non-synthetic real execution image | Builders emit `GU_ACTIVE_SINGLE_TARGET` only with `GU_SYNTHETIC_COMPILE_ONLY`, and both entrypoints return `EFI_UNSUPPORTED` before device work. | Open: `-AcceptUnverifiedActuation` emits `/DGU_ACTIVE_SINGLE_TARGET /DGU_ACTIVE_UNVERIFIED_ACTUATION` and no synthetic or host-test define. |
| G4 | `msi_release.c::GuMsiRelease` (`device_id == 0x2684 || 0x220a`) | Vendor Stop for the exact research cards, RTX 4090 (AD102) and RTX 3080 12 GB (GA102) | Refusal retained because the stock Stop callback can write BAR0 and poll without a proven local bound. | Open, logged as `MSI release WAIVED ... VENDOR_STOP_BOUND=UNVERIFIED SIGNED_BOOTER=UNVERIFIED RECOVERY=UNVERIFIED`. |
| G5 | `profile.c::GuAd104PromMatches` | Accepting a PROM image as an authenticated firmware reference | Structural validation plus a digest log; `SIGNATURE_AUTHENTICATED=0`. | Unchanged. There is no authenticated per-card allowlist to switch to; the check never claimed to authenticate, so nothing is lifted. |
| G6 | `profile.c::GuAd104Prepare` (Turing) and asset manifests | Turing signed-loader execution; cross-architecture asset substitution | Turing returns `TURING_PREPARE=REFUSED REASON=SIGNED_LOADER_NOT_VERIFIED`; assets are 610.43.03 Linux-lineage. | Unchanged. The transaction code is AD104/GA10x-specific; opening the Turing refusal would push a Turing card into an AD10x register script, which is a wrong-write, not an unlock. |

Without the switch each gate fails *closed and early*: before the mutation it
protects, with an explicit log line, and without retry or fallback. With the
switch, G1–G4 admit the mutation and G5–G6 still refuse.

### 4.2 The operator waiver: `GU_ACTIVE_UNVERIFIED_ACTUATION`

This section replaces the earlier "why the gates were not opened" rationale.
The owner reviewed the gate map and chose to open G1–G4 anyway, so the
decision is now recorded in the build instead of argued in prose.

Why an earlier pass declined is still true and still matters:

1. **It is not verifiable here.** There is no NVIDIA GPU, no MSI B850, no
   Windows 617.42 Booter image and no MSVC/EDK II toolchain in the
   development environment. A change that removes G1–G4 cannot be executed,
   observed, or shown to fail safely.
2. **The failure mode is not recoverable by software.** A wrong BAR0 write or
   a Stop callback without a bound leaves the GPU in an unknown state. The
   documented recovery is physical power removal, and "healthy-stock cold-power
   recovery" is the only recovery item ever marked passed.
3. **The remaining work is evidence acquisition, not source edits.**
   Authenticated per-card firmware acceptance, DMA inactivity proof and
   post-modification Windows behaviour are measurements on the physical
   machine.

The waiver does not pretend otherwise. It makes the acceptance explicit,
single, non-default and loud:

| Property | Implementation |
|---|---|
| Single switch | `GU_ACTIVE_UNVERIFIED_ACTUATION`, emitted only by `-AcceptUnverifiedActuation` in `src/Build-Experimental-MSVC.ps1` and `src/Build-Dxe-Driver.ps1`. |
| Never a default | No script, workflow or default define sets it. `tests/test_unverified_actuation_waiver.py` asserts there is exactly one `/DGU_ACTIVE_UNVERIFIED_ACTUATION` per builder and that the fallback `GU_PREFLIGHT_ONLY` branch still refuses. |
| Only in a real transaction build | `port.h` `#error`s unless `GU_AD104_TRANSACTION` and `GU_ACTIVE_SINGLE_TARGET` are also defined. |
| Never combined with a passive profile | `port.h` `#error`s with `GU_ACTIVE_SEQUENCE_PREPARE_ONLY`, `GU_SYNTHETIC_COMPILE_ONLY` or `GU_HOST_TEST_PROFILE`; the builders refuse `-AcceptUnverifiedActuation` with `-SyntheticCompileProfile`. |
| Required to be deliberate | `-AcceptUnverifiedActuation` requires `-EnableSingleTargetExecution`, so an operator cannot drift into it while producing a preparation image. |
| Still ordered, never simultaneous | Multi-target is admitted as an ordered list; the coordinator runs one transaction at a time and gates each step on its predecessor's completion record. `MULTI_GPU_REFACTOR_REVIEW.md` §4.3 step 6 still forbids simultaneous ownership. |
| Loud at build time | Artifact names contain `UNVERIFIED-ACTUATION`; the `STATUS=COMPLETE` line carries `UNVERIFIED_ACTUATION=1 ACTUATION_VERIFIED=0 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLASH=0 RECOVERY_VERIFIED=0 WAIVER=GU_ACTIVE_UNVERIFIED_ACTUATION NOT_FOR_DEPLOYMENT=1`. |
| Loud at run time | `ACTIVE_EXECUTION=WAIVED_UNVERIFIED ... MUTATION_AUTHORIZED=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLASH=0 RECOVERY_VERIFIED=0`, plus `MSI release WAIVED ... VENDOR_STOP_BOUND=UNVERIFIED SIGNED_BOOTER=UNVERIFIED RECOVERY=UNVERIFIED`. |
| Honest about scope | Eligibility strings stay `false`. A waived image is *allowed to try*; it is not *shown to work*, and no status line says otherwise. |

Two refusals deliberately stay closed (G5, G6). They are not policy refusals
that a waiver can lift:

* `GuAd104PromMatches` never claimed authentication, so there is nothing to
  switch off; the digest it logs is evidence, not a verdict.
* the Turing refusal protects a different register script. The transaction
  code in this fork is AD104/GA10x-specific, so admitting a Turing card would
  produce a wrong write rather than an unlock.

### 4.2.1 Building and reading the result

```powershell
# One target, explicitly unverified actuation
.\src\Build-Experimental-MSVC.ps1 -SelectionBdf '0000:01:00.0' `
    -EnableSingleTargetExecution -AcceptUnverifiedActuation

# Ordered list: one card active at a time, each step gated on its predecessor
.\src\Build-Experimental-MSVC.ps1 -SelectionBdfs '0000:01:00.0,0000:02:00.0' `
    -EnableSingleTargetExecution -AcceptUnverifiedActuation

# Same for the DXE driver entrypoint
.\src\Build-Dxe-Driver.ps1 -SelectionBdf '0000:01:00.0' `
    -EnableSingleTargetExecution -AcceptUnverifiedActuation
```

Expected status from either script:

```text
STATUS=COMPLETE ... MULTI_GPU_MUTATION_ALLOWED=1 UNVERIFIED_ACTUATION=1
ACTUATION_VERIFIED=0 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLASH=0 RECOVERY_VERIFIED=0
WAIVER=GU_ACTIVE_UNVERIFIED_ACTUATION NOT_FOR_DEPLOYMENT=1
```

Step-by-step build, expected markers, stop conditions and the evidence that
must be captured afterwards are in
[`UNVERIFIED_ACTUATION_RUNBOOK.md`](UNVERIFIED_ACTUATION_RUNBOOK.md).

What is still required from the operator, unchanged by the waiver: the pinned
Windows 617.42 handoff and the reviewed `local/boot_profile.h` ESP GUID
(a waived build cannot use the synthetic host-test GUID), the matched
`AD102-ASSETS`/`GA102-ASSETS` payloads (`tools/verify_reference_assets.py`),
an independent console for the ordered log, and physical power-removal
recovery within reach for the first run.

### 4.3 Implementation plan for full hardware execution

Sequenced so that each step is independently testable and the fail-closed
default is never widened:

1. **Transaction mode for 2..N targets (source work, host-testable).**
   Introduced as `GU_ACTIVE_SINGLE_TARGET` + `GU_ACTIVE_UNVERIFIED_ACTUATION`
   rather than as a separate `GU_ACTIVE_SEQUENCE_TRANSACTION` name: the
   ordered coordinator already existed, and the missing piece was an admission
   that was explicit instead of implicit. Host tests prove what they can —
   a successor never starts before the predecessor's completion record is
   verified, a mid-sequence refusal retains completed contexts, no target is
   retried (`tests/test_gpu_discovery.c`, 210 checks; `tests/test_unlock_execution_contract.py`;
   `tests/test_unverified_actuation_waiver.py`). What they cannot prove is
   that the transaction succeeds on real silicon.
   *This is the only step that could be finished without hardware, and it is
   the only step finished here.*
2. **Per-card firmware acceptance contract (source + offline evidence).**
   Replace the structural PROM check with an authenticated per-device
   reference: exact device ID, subsystem, signed image digest and loader
   version, stored as an explicit allowlist that fails closed on a miss.
   Offline verification can be completed with committed digests; the Windows
   617.42 Booter provenance comparison remains mandatory evidence.
   *Status (2026-11-18): **the contract is implemented; it has no references
   yet.*** `src/prom_reference.[ch]` is the allowlist - a reference is
   (device id, subsystem id, ROM SHA-256, loader version) and a card is admitted
   only by an exact hit on all four. `GU_PROM_REFERENCE_REQUIRED` makes a miss a
   refusal (`EFI_UNSUPPORTED`, nothing written); the default build keeps the
   structural check and logs `PROM_IDENTITY MODE=STRUCTURAL AUTHENTICATION=0`, so
   no log implies otherwise. **Zero references are committed**, because this
   repository contains no card dumps: with the requirement enabled, the image
   admits nothing, which is the correct fail-closed answer. The read-only
   diagnostic deliberately still runs and prints the digest, device and
   subsystem to capture one. Signature authentication is *not* part of this -
   both decision paths keep `SIGNATURE_AUTHENTICATED=0`. Evidence:
   `tests/test_prom_reference.c` (both table variants), 17 contract tests,
   simulator rows `prepare prom-refused` and `prepare prom-admitted`; inventory
   §11.13.
3. **Vendor Stop with a bounded poll (hardware-qualified change).** Add a
   documented bound and a pre-Stop state capture for the AD102/GA102 Stop
   callback, then replace the wholesale `device_id` refusal with a
   per-target contract that requires: single verified `BY_DRIVER` owner, no
   firmware GOP for that GPU, an independent console, a flushed-and-closed
   intent log, and an independently verified recovery procedure. Until the
   bound and recovery are demonstrated on hardware, G4 stays closed for
   `0x2684`/`0x220a`.
4. **DMA and FLR quiescence proof.** Demonstrate DMA inactivity *after* the
   Stop and before any MMIO write, per target.
5. **Physical qualification on one card at a time.** Single target, stock
   Windows driver, Tensor Core numerical correctness and throughput, then a
   failed-transaction and cold-power recovery run. Only after that, a
   sequential two-target run.
6. **Only then** consider admitting multiple simultaneous targets. Physical
   isolation between the GPUs is currently unverified, so "dynamic multi-GPU"
   means *ordered sequential* targets, not simultaneous ownership.

#### CI builds the admitted image (2026-11-18)

Step 1's last exit criterion - "CI builds and diffs the admitted image" - is met,
and it is the only one in this section that CI can meet:

* the **host** job builds all 26 sources twice, checked and admitted, and diffs
  the define and symbol sets per translation unit
  (`tools/waiver_build_diff.py`): `WAIVER_DIFF=PASS SOURCES=26 MACRO_DELTAS=41
  ADMITTED_UNITS=15 SYMBOL_DELTAS=0`. The waiver flips
  `GU_ACTUATION_ADMITTED` and adds no code to any module;
* the **Windows** job compiles the admitted image with MSVC - application and DXE,
  one target and an ordered two-target sequence - and asserts the exact binary
  each selection implies. A hosted runner has no reviewed partition GUID, and a
  waived image may not use the synthetic host-test GUID, so these builds pass
  `-RuntimeEsp`. Nothing is uploaded.

The admitted artifact stays out of the artifact set by construction: the upload
globs name `GPU-ACTIVE-PREP-*`/`GPU-ACTIVE-PREP-DXE-*`, and
`tests/test_waiver_ci.py` fails if a glob could ever match an
`*UNVERIFIED-ACTUATION*` name.

What this is not: a statement about behaviour. Compiling proves the instrument is
buildable from this tree; it says nothing about the Stop bound, DMA quiescence,
firmware acceptance or recovery, which is why steps 3-6 remain untouched. It also
found three defects that the host suite had been hiding - a missing
`prom_reference.c` in both MSVC source lists, two test pins that only execute
where clang exists, and a mock scanning past its own array - each now covered by a
test. Inventory §11.14.

### 4.4 What still cannot be established from source

| Requirement | Remains |
|---|---|
| Production hardware ownership | Unverified |
| Physical isolation between GPUs | Unverified |
| Direct memory access inactivity | Unverified |
| Authenticated RTX 3080 firmware acceptance | Unverified |
| Authenticated RTX 4090 firmware acceptance | Unverified |
| Production active hardware execution | Untested |
| Stock Windows driver operation after modification | Untested |
| Tensor Core numerical correctness after modification | Untested |
| Tensor Core performance after modification | Untested |
| Failed-transaction recovery | Unverified |

The eligibility gates in `README.md` are unchanged:

```text
CAN_DETACH=false
CAN_UNLOCK=false
CAN_FLASH=false
RECOVERY_VERIFIED=false
```

Every row above is unchanged by `GU_ACTIVE_UNVERIFIED_ACTUATION`. The waiver
moves one item — production active hardware execution — from *cannot be
attempted* to *may be attempted and is still untested*. It moves nothing from
Unverified to Verified, and it cannot, because no test in this repository
observes a GPU.

---

## 5. Reproducing the verification in this pass

```bash
# EFI-only passive sources: syntax check against the vendored EDK II headers
for define in "" "-DGU_TOPO_SAVE_LOG"; do
  for src in src/gpu_topology_efi.c src/gpu_pci_observation.c; do
    gcc -std=c11 -fshort-wchar -fsyntax-only -Wall -Wextra \
        -DGU_GPU_DIAGNOSTIC_ONLY $define -Isrc \
        -Isrc/vendor/edk2/Include -Isrc/vendor/edk2/Include/X64 "$src"
  done
done

# The waiver: only the complete define set compiles, and only with the waiver
# does an ordered three-target source admit a non-synthetic image.
gcc -std=c11 -fsyntax-only -DGU_ACTIVE_SINGLE_TARGET \
    -DGU_ACTIVE_UNVERIFIED_ACTUATION -DGU_AD104_TRANSACTION \
    -DGU_ENTRY_LATCHED_SS1_FULL -I/tmp/active-selected -Isrc \
    tests/test_gpu_discovery.c
python3 -m unittest tests.test_unverified_actuation_waiver

# Shared host logic
gcc -std=c11 -O2 -Wall -Wextra -Werror -pedantic -Isrc \
    tests/test_gpu_inventory.c src/gpu_inventory.c -o /tmp/inv && /tmp/inv
gcc -std=c11 -O2 -Wall -Wextra -Werror -pedantic -Isrc \
    tests/test_gpu_topology.c src/gpu_topology.c src/gpu_inventory.c -o /tmp/topo && /tmp/topo
gcc -std=c11 -O2 -Wall -Wextra -Werror -pedantic -Isrc \
    tests/test_gpu_prom_capture.c src/gpu_prom_capture.c -o /tmp/prom && /tmp/prom
gcc -std=c11 -O2 -Wall -Wextra -Werror -pedantic -DGU_GPU_DIAGNOSTIC_ONLY \
    -DGU_PCI_OBSERVATION_HOST_TEST -Isrc \
    tests/test_gpu_pci_observation.c src/gpu_pci_observation.c -o /tmp/obs && /tmp/obs

# Extracted-source and attribution contracts
python3 -m unittest discover -s tests -p test_diagnostic_log_completeness.py -v
python3 -m unittest discover -s tests -p test_legacy_inventory_boundary.py -v
python3 -m unittest discover -s tests -p test_benchmark_attribution.py -v
```

The EFI sources touched here (`gpu_topology_efi.c`) are still only compiled,
never executed, in CI. The extracted-source test establishes the logging
behaviour; it does not establish firmware behaviour.
