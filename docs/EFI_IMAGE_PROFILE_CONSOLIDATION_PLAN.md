# EFI image profile consolidation plan

## 1. Situation and objective

Status: AUDIT COMPLETE / MIGRATION NOT STARTED. Baseline [arena commit `2cbff8a6e5638555353288dda584bc44b3fc0731`](https://github.com/akashskypatel/universal-gpu-unlock/tree/2cbff8a6e5638555353288dda584bc44b3fc0731). Covers all 41 catalogue entries, five builders, application and driver entrypoints, signing wrapper and generated documents. Host and Windows EFI jobs for [run 45338664023](https://github.com/akashskypatel/universal-gpu-unlock/actions/runs/45338664023) passed for the existing maintenance integration. This is not migration verification.

Reduce 41 public variant IDs to five canonical profile IDs (87.8% reduction). Three describe GPU operations, two cover administration and compile validation. This is a reduction in public names, not a claim that all configurations produce identical binaries. Retain distinct execution modes and evidence. Use verified hardware tests as the authoritative experimental admission baseline. Remove restrictions that are stricter than the applicable verified hardware test, where the evidence establishes that the restriction is unnecessary. Do not add new precaution-only requirements, re-open closed leads, or require repeat hardware tests solely to reconfirm an unchanged path. This is a plan, not authorization to change hardware or production readiness.

Definitions: EFI = Extensible Firmware Interface; UEFI = Unified EFI; ESP = EFI System Partition; DXE = Driver Execution Environment; BDF = PCI bus/device/function address (with segment); GOP = Graphics Output Protocol; NVRAM = nonvolatile firmware variable storage; ASPM = PCI Express Active State Power Management; FLR = function-level reset; DMA = direct memory access; S3 = suspend-to-RAM; CI = continuous integration.

## 2. Background: complete catalogue audit

| Family | Count | Finding |
| --- | ---: | --- |
| diagnostic | 13 | One builder: logging, probe, BDF selection and handoff are parameters. |
| prepare | 15 | Application builder includes preflight, compile checks, stop/reattach, full firmware transactions and administrative operations. |
| prepare-dxe | 7 | Shared source, distinct entrypoint/event lifecycle. |
| stop-observation | 5 | Raw original vendor Stop experiments; fixed hardware identities and handoff. |
| maintenance | 1 | Separate reviewed record removal. |
| Total | 41 | 38 distinct unsigned filename patterns; no byte-equivalence claim. |

Functional counts: 13 diagnostics, 2 preparation, 4 compile-only, 3 Stop/reattach, 5 legacy Stop observation, 11 full transaction (including menus and DXE), 3 administration. Menu profiles can also select administrative operations; count each ID once.

Filename overlaps: diag-topology / diag-topology-log (SaveLog); actuate-one-target / actuate-one-target-resident (ESP policy); actuate-runtime-selection / actuate-runtime-selection-resident (ESP policy). The wrapper already isolates signed outputs by variant, run and digest prefix and verifies the complete hash. Preserve distinct effective configurations and artifact provenance; shared name is not proof of equal binary content.

### Source boundaries and findings

| Source | Consolidation boundary |
| --- | --- |
| [Diagnostic builder](../src/Build-Gpu-Topology-Diagnostic.ps1) | All 13 variants use existing switches. Probe flags are mutually exclusive; MSI probes reject BDF selection; GOP text cycle and selected diagnostics reject handoff; probes/handoff require logs. Keep those actual implementation restrictions until supported otherwise. |
| [Application builder](../src/Build-Experimental-MSVC.ps1) | Existing compiled/runtime selection, ESP policy, synthetic mode, StopReleaseOnlyBench, StopReleaseWindowsHandoff, FirmwareReattach and ApplicationOperation parameters. Stop-only omits vendor/unlock_610.c. Only combined runtime app currently accepts explicit WindowsEspGuid override. |
| [DXE builder](../src/Build-Dxe-Driver.ps1), [DXE entrypoint](../src/dxe_main.c) | Subsystem 11 and ReadyToBoot event registration/closure differ from subsystem 10 applications. Synthetic returns before registering events. DXE Stop-only omits unlock implementation. Keep separate entrypoints and timing. Never deploy DXE as BOOTX64.EFI. |
| [app.c](../src/app.c) and dxe_main.c | Classification defect: preparation-only is not globally read-only. App recovery at lines 233–282 can remove owned DXE options before preparation refusal at 329–335. DXE recovery at 49–83 can disable DriverOrder before discovery. Move preparation into diagnostic only after an end-to-end no-persistent-write path is proved; retain legacy recovery independently. |
| app.c | Fixed U/P/I/R dispatch follows recovery/preparation. U and P are not combined reattach. Fixed I/R halt, unlike legacy menu behavior. Preserve operation semantics and recovery precedence. |
| [Legacy Stop builder](../src/Build-Msi-Stop-Observation.ps1), [observer](../src/msi_stop_observation.c) | Raw vendor Stop has separate fixed hardware identity, may hang, does not prove restored graph, and dual mode requires pinned Windows handoff. Keep explicit legacy submode until evidence supports archival. Do not silently map it to StopReattach. |
| [Maintenance builder](../src/Build-Boot-State-Maintenance.ps1), [maintenance entrypoint](../src/boot_state_maintenance.c), [boot state](../src/boot_state.c) | Separate minimal link set. Exact reviewed record, typed confirmation, absent DriverOrder, checked deletion and halt. Never combine the removal image with another active operation. |
| [Wrapper](../src/Build-Sign-Deploy-UniversalGPU.ps1) | Named-variant branches in selection, argument mapping, output naming and attestation checks. Reuse signing, hash, certificate, PE subsystem, USB identity, backup and rollback. Retain independent builder attestations. |

## 3. Assessment: evidence-based admission policy

Authoritative physical evidence: [Windows-GPU-UNLOCK execution map](https://github.com/akashskypatel/Windows-GPU-UNLOCK/blob/main/research/nvidia_unlock_evidence_to_execution_map.md), current combined acceptance and repeatability sections. Do not substitute historical planning restrictions for physical findings.

| Evidence | Verified scope |
| --- | --- |
| RUN0002(1).TXT, 992 lines | RTX4090 then RTX3080, both NVIDIA displays connected; fresh ALL selection, interactive selection from two Windows ESPs, per-target peer warning, Stop, exclusive claim, firmware result, one FLR per card, verified cleanup and original driver/child/GOP restoration, final peer graph, retained INTENT and normal Windows handoff. GA102 ASPM=2 accepted. Original firmware signature provenance and independent DMA isolation not established. |
| Same-boot Windows 617.42 / CUDA | Both UUIDs match, PnP OK, TF32-allowed 8192 +69.1%/+82.2% in paired run; five repeated rounds sustain improvement. No independent numerical comparison or long-duration qualification. |
| RUN0043 | Owner-observed sole-GOP RTX3080 Stop and reattach, display returns. This is Stop-only evidence, not a sole-GOP firmware transaction. |
| Original Stop 4090/3080/dual captures | Separate raw Stop evidence; AMD-only console and pinned Windows handoff in those experiments. Do not infer reattachment proof from an unbound-device result. |
| RUN0003.TXT and post-removal measurements | Recovery path returns normalized removal success, preserves INTENT and does not retry; direct Windows load refused on two ESPs, owner reports normal firmware fallback boot. Stock-like repeated throughput and healthy devices. Not proof of actual entry deletion, dedicated R dispatch or failed-transaction recovery. |
| 45338664023 | Host and EFI CI pass; software verification, not physical maintenance test. |

### Gate disposition tracker

For every migrated gate, record original predicate and call site, actual verified run and applicable mode, proposed replacement/removal, negative cases still required, and CI result. Use the least restrictive predicate that preserves the verified behavior. A gate that accepted the verified run is not excessive merely because a different path used fewer checks. Untested hardware or paths do not inherit acceptance by name. Unknown values are not success.

| ID | Existing restriction / concern | Required migration decision | Status |
| --- | --- | --- | --- |
| G1 | Global AMD-only console or blanket refusal when displays are attached | Remove as a universal experimental prerequisite. Use the existing successful per-target live peer/console admission for combined mode; retain sole-GOP Stop-only exception from RUN0043. Do not require moving displays for the verified dual-display configuration. Raw legacy mode retains its tested scope. | READY TO MIGRATE |
| G2 | Mandatory ASPM=0 | Do not reintroduce it for the verified Stop/reattach and combined modes. Retain the already implemented, explicit mode-scoped exception. Link training, negotiated width/speed and uncorrected error checks remain. Do not extend the exception to detached U/P, S3 or DXE full actuation without applicable evidence. | READY TO MIGRATE |
| G3 | Fixed compile-time BDF list, one-target-only restriction | Remove blanket requirement for the verified runtime combined mode; reuse live supported inventory, ordered selection and per-target identity/peer checks. Derive one/many from the validated list rather than separate profiles. Keep fixed addresses only where required by the specific legacy test or synthetic compilation. | READY TO MIGRATE |
| G4 | Mandatory private ESP profile, single ESP or mandatory compiled GUID | Do not require a private pinned profile for the existing verified runtime combined app. Use the existing fresh explicit selection of one unique destination when multiple ESPs exist; exact pin remains an option. Preserve refusal on ambiguity, cancel, failed load or changed identity. Other modes use their existing supported resolver, including legacy pinned and DXE paths. | READY TO MIGRATE |
| G5 | Windows SEC2 Booter research, repeated unchanged identity audits, repeat healthy-stock recovery test, independent slot-power precaution | Do not make these prerequisites for the already verified experimental UEFI route. Keep unresolved production/provenance facts separate. Respect previous owner risk acceptance and do not repeat closed tests. | READY TO MIGRATE |
| G6 | Independent survivor and peer warning, console identity, claim/release proof, per-card result, cleanup, final restored graph, sequential ordering, log close, containment | KEEP for the applicable combined route. These checks participated in the successful run. Remove duplicate checks only if the same live predicate at the same lifecycle boundary is preserved; earlier observations do not substitute for post-transition verification. | RETAIN |
| G7 | One-use INTENT, explicit recovery/refusal, no automatic retry, separate reviewed removal | KEEP. RUN0002(1) retained INTENT; RUN0003 proves the protective path was used. No general record reset or bypass. Keep explicit deployment consent, maintenance isolation and unrelated-variable preservation. | RETAIN |
| G8 | Firmware provenance, independent DMA isolation, numeric accuracy, S3 persistence, actual failed-transaction recovery | Track separately as qualification gaps. Do not block the already verified experimental route with new production-only prerequisites; do not claim qualification or remove the existing live correctness/authentication/containment checks. | OPEN QUALIFICATION |
| G9 | Fixed I/R operations run active preparation and recovery before dispatch | Review exact dependencies. Remove unnecessary prerequisites only after source and tests prove the administrative path has equivalent ownership, refusal, logging and no GPU mutation. RUN0003 did not reach R dispatch; no physical equivalence claim. | INVESTIGATE |
| G10 | Maintenance DriverOrder absence, typed record review and independent removal readback | KEEP current requirements; maintenance physical execution not verified. No evidence supports relaxing them. | RETAIN |
| G11 | Legacy Stop vs reattach, detached U/P vs combined, DXE vs app, resume vs RAM-only | Do not propagate a verified exception across distinct untested lifecycle boundaries. Classify and preserve independently; retire redundant modes only after documented evidence equivalence. | INVESTIGATE |

Production CAN_DETACH=false, CAN_UNLOCK=false, CAN_FLASH=false, RECOVERY_VERIFIED=false remain unchanged. A completed experimental run is not production approval. No additional physical execution is necessary for the catalogue migration itself.

## 4. Proposed canonical interface (not implemented)

Keep -Only as the existing selector. Use existing builder arguments and a small typed profile resolver, not a second compiler or arbitrary switch passthrough. These are conceptual option names, not current runnable commands. Explicit action selection is required for any mutating profile. Invalid combinations fail before compile/sign/stage. Preserve the old profile IDs temporarily as exact compatibility aliases.

| ID | Role | Optional arguments and restrictions |
| --- | --- | --- |
| diagnostic | GPU/firmware observation only | Probe = Topology/GopTextCycle/MsiRom/MsiFuse/MsiSec2/VramBlocks; SaveLog; SelectionBdf(s); WindowsEspGuid where already supported. Preflight only after D2 verifies no persistent writes. Default topology with file evidence; alias preserves old console-only behavior. |
| driver-transition | Driver Stop/release/reconnect, no firmware transaction | Operation = StopReattach/LegacyStopObservation; ImageKind = Application/Dxe where supported; explicit BDFs or existing live-all policy; handoff choice; legacy hardware target and pinned ESP only for legacy observer. Requires existing mutating deployment consent. |
| firmware-transaction | Experimental full firmware transaction | Operation = UnlockReattach/Unlock/UnlockWithResume/Menu (explicit legacy menu); ImageKind = Application/Dxe (DXE existing operation policy, not app menu); compiled BDF list or existing runtime selection; EspPolicy = Pinned/Runtime/Resident where supported; optional WindowsEspGuid for combined runtime app; existing supported CapturedPromReference option. Prefer the verified combined application in documentation; no implicit firmware actuation. |
| maintenance | Administrative NVRAM changes, not read-only | Operation = InstallDriver/RemoveDriver/ReviewBootState. Keep I/R semantics until A1. ReviewBootState links separate maintenance binary, requires isolated media and existing interactive confirmation; reject target/ESP/companion options. |
| compile-check | Synthetic build validation | ImageKind = Application/Dxe; explicit BDF list. Derive count, never stage or execute; no active/ESP/resume options. |

Keep legacy aliases as exact argument mappings with a deprecation notice until downstream migration completes. No silent substitution of a different action, persistence policy, target order, handoff or safety class. Expose canonical profiles and supported options with -List; retain an explicit legacy listing during transition. -All signs an enumerated legal regression matrix, not an unbounded Cartesian product, and never stages. Preserve -SignOnly, -StageDrive and existing consent switches, with one resolved configuration per deployment. Keep direct builders available to existing CI. Do not add new runtime menu or configuration file solely to reduce profile names.

Receipts must identify canonical profile, old alias if supplied, resolved effective options, target selection source and order, ESP policy, actual builder switches, actual verified attestations, PE subsystem, full unsigned/signed SHA-256 and hardware verification false unless independent evidence exists. Avoid collisions for distinct configs; do not overwrite a prior signed receipt. Use actual effective values, not unresolved placeholders. Preserve existing certificate enrollment/verification, USB identity, backup and rollback. DXE pairing must be explicitly validated and cannot be staged as BOOTX64.

## 5. Migration task status tracker

Status values: NOT STARTED, IN PROGRESS, BLOCKED, IMPLEMENTED, VERIFIED, COMPLETE. COMPLETE requires evidence of migration acceptance and updated docs, not only a successful commit. Planning IDs are metadata, not code names.

| Task | Scope / acceptance | Depends on | Status | Evidence |
| --- | --- | --- | --- | --- |
| B0 | Enumerate all 41 entries; verify builders and current hardware baseline | None | COMPLETE | Baseline commit and physical evidence in sections 2–3; existing CI pass |
| G0 | Audit every affected admission predicate and classify using G1–G11. Remove duplicate or over-restrictive experimental conditions only within their proven scope; document unchanged negative cases. | B0 | NOT STARTED | Pending |
| R1 | Implement small profile resolver with typed options, exact alias mappings, strict illegal-option rejection and one effective configuration | G0 | NOT STARTED | Pending |
| D1 | Consolidate 13 diagnostics; preserve all existing evidence shapes, probe limits and handoff rules | R1 | NOT STARTED | Pending |
| D2 | Split passive preparation from recovery writes; only then migrate two preparation IDs to diagnostic Preflight. Retain DXE timing distinction and old compatibility until proved. | D1,G0 | NOT STARTED | Pending |
| C1 | Consolidate four compile-only IDs; validate count, app/DXE, non-stageable contract and no execution | R1 | NOT STARTED | Pending |
| T1 | Consolidate three Stop/reattach IDs, retaining actual warning, release, reattach, no firmware link, lifecycle and mode-specific handoff | R1,G0 | NOT STARTED | Pending |
| T2 | Consolidate five legacy raw Stop IDs as explicit legacy observer options; review necessity before any deletion | R1,G0 | NOT STARTED | Pending |
| F1 | Consolidate eight menu/legacy DXE transaction IDs; preserve compiled/runtime and ESP policies, event lifecycle and INTENT | R1,G0 | NOT STARTED | Pending |
| F2 | Consolidate three fixed U/P/combined IDs; preserve separate modes and verified combined default recommendation; never imply S3 qualification | F1,G0 | NOT STARTED | Pending |
| A1 | Consolidate three administrative IDs; preserve independent reviewed maintenance image, recovery path and owned entry semantics | R1,G0 | NOT STARTED | Pending |
| V1 | Compare all 41 old configurations with alias resolution; builder switch, image identity, attestations, safety, target order and negative cases. Run existing host and Windows EFI CI; verify actual outcomes. | D1,D2,C1,T1,T2,F1,F2,A1 | NOT STARTED | Pending |
| V2 | Update generated catalogue, user guide, donor guide, readiness/log scoring consumers, CI scripts and command references; prove coverage and receipt compatibility | V1 | NOT STARTED | Pending |
| V3 | Remove redundant public IDs from canonical catalogue only after alias migration; retain documented compatibility where needed. Archive legacy observer only if distinct evidence use is closed. | V2 | NOT STARTED | Pending |

### Per-variant tracker (41 of 41 mapped; 0 migrated)

The following mappings are proposed. Each row requires an independently verified equivalent configuration before marking COMPLETE.

| Existing ID | Task | Target profile | Effective options / semantics | Status |
| --- | --- | --- | --- | --- |
| `diag-topology` | D1 | diagnostic | Topology; console only | NOT STARTED |
| `diag-topology-log` | D1 | diagnostic | Topology; SaveLog | NOT STARTED |
| `diag-topology-gop-text-cycle` | D1 | diagnostic | GopTextCycle; SaveLog; no handoff | NOT STARTED |
| `diag-topology-handoff` | D1 | diagnostic | Topology; SaveLog; pinned WindowsEspGuid | NOT STARTED |
| `diag-selection` | D1 | diagnostic | Topology; SaveLog; SelectionBdf(s); no handoff | NOT STARTED |
| `diag-msi-rom` | D1 | diagnostic | MsiRom; SaveLog | NOT STARTED |
| `diag-msi-rom-handoff` | D1 | diagnostic | MsiRom; SaveLog; pinned WindowsEspGuid | NOT STARTED |
| `diag-msi-fuse` | D1 | diagnostic | MsiFuse; SaveLog | NOT STARTED |
| `diag-msi-fuse-handoff` | D1 | diagnostic | MsiFuse; SaveLog; pinned WindowsEspGuid | NOT STARTED |
| `diag-msi-sec2` | D1 | diagnostic | MsiSec2; SaveLog | NOT STARTED |
| `diag-msi-sec2-handoff` | D1 | diagnostic | MsiSec2; SaveLog; pinned WindowsEspGuid | NOT STARTED |
| `diag-vram-blocks` | D1 | diagnostic | VramBlocks; SaveLog | NOT STARTED |
| `diag-vram-blocks-handoff` | D1 | diagnostic | VramBlocks; SaveLog; pinned WindowsEspGuid | NOT STARTED |
| `prep-selected` | D2 | diagnostic (conditional) | Preflight; Application; explicit BDFs | NOT STARTED |
| `compile-one-target` | C1 | compile-check | Application; one BDF | NOT STARTED |
| `compile-sequence` | C1 | compile-check | Application; ordered BDFs | NOT STARTED |
| `actuate-one-target` | F1 | firmware-transaction | Menu; Application; one compiled BDF; Pinned | NOT STARTED |
| `actuate-sequence` | F1 | firmware-transaction | Menu; Application; ordered compiled BDFs; Pinned | NOT STARTED |
| `app-stop-release-sequence-bench` | T1 | driver-transition | StopReattach; Application; live-all or compiled BDFs; Return | NOT STARTED |
| `app-stop-release-sequence-handoff` | T1 | driver-transition | StopReattach; Application; live-all; WindowsAfterSuccess | NOT STARTED |
| `actuate-one-target-resident` | F1 | firmware-transaction | Menu; Application; one compiled BDF; Resident | NOT STARTED |
| `actuate-runtime-selection` | F1 | firmware-transaction | Menu; Application; Operator; Pinned | NOT STARTED |
| `actuate-runtime-selection-resident` | F1 | firmware-transaction | Menu; Application; Operator; Resident | NOT STARTED |
| `app-firmware-reattach` | F2 | firmware-transaction | UnlockReattach; Application; Operator; Runtime; optional WindowsEspGuid | NOT STARTED |
| `app-unlock` | F2 | firmware-transaction | Unlock; Application; Operator; Runtime | NOT STARTED |
| `app-unlock-with-resume` | F2 | firmware-transaction | UnlockWithResume; Application; Operator; Runtime | NOT STARTED |
| `app-install-driver` | A1 | maintenance | InstallDriver; Application; Operator; Runtime | NOT STARTED |
| `app-remove-driver` | A1 | maintenance | RemoveDriver; Application; Operator; Runtime | NOT STARTED |
| `dxe-prep-selected` | D2 | diagnostic (conditional) | Preflight; Dxe; explicit BDFs; event lifecycle retained | NOT STARTED |
| `dxe-compile-one-target` | C1 | compile-check | Dxe; one BDF | NOT STARTED |
| `dxe-compile-sequence` | C1 | compile-check | Dxe; ordered BDFs | NOT STARTED |
| `dxe-actuate-one-target` | F1 | firmware-transaction | Dxe; one compiled BDF; Pinned | NOT STARTED |
| `dxe-actuate-sequence` | F1 | firmware-transaction | Dxe; ordered compiled BDFs; Pinned | NOT STARTED |
| `dxe-stop-release-sequence-bench` | T1 | driver-transition | StopReattach; Dxe; live-all or compiled BDFs; existing local profile; Return | NOT STARTED |
| `dxe-actuate-runtime-selection` | F1 | firmware-transaction | Dxe; existing runtime selection policy; Pinned | NOT STARTED |
| `stop-4090-once` | T2 | driver-transition (legacy test) | LegacyStopObservation; RTX4090; Halt | NOT STARTED |
| `stop-4090-handoff` | T2 | driver-transition (legacy test) | LegacyStopObservation; RTX4090; pinned WindowsEspGuid | NOT STARTED |
| `stop-3080-once` | T2 | driver-transition (legacy test) | LegacyStopObservation; RTX3080; Halt | NOT STARTED |
| `stop-3080-handoff` | T2 | driver-transition (legacy test) | LegacyStopObservation; RTX3080; pinned WindowsEspGuid | NOT STARTED |
| `stop-dual-handoff` | T2 | driver-transition (legacy test) | LegacyStopObservation; BothNvidia; fixed order; pinned WindowsEspGuid | NOT STARTED |
| `boot-state-maintenance` | A1 | maintenance | ReviewBootState; separate isolated Application | NOT STARTED |

### Previously refused combinations

These are not additional variants. Preserve each actual refusal unless the evidence and source prove a supported alternative.

| Combination | Current reason | Migration disposition |
| --- | --- | --- |
| `legacy-experimental` | the unselected mutating Experimental image is deliberately disabled: its mutation path is not generally hardware-safe, and the builder throws if asked | Review against applicable hardware evidence; preserve refusal unless equivalence is established. |
| `stop-dual-without-handoff` | the dual Stop requires a pinned Windows handoff, so a no-handoff dual image has no legal build | Review against applicable hardware evidence; preserve refusal unless equivalence is established. |
| `dxe-as-bootx64` | a DXE boot-service driver is PE subsystem 11; firmware will not run it as BOOTX64.EFI, and the script refuses to stage one | Review against applicable hardware evidence; preserve refusal unless equivalence is established. |
| `esp-resident-dxe` | GU_ESP_RESIDENT gates the launch of an application image; a Driver#### entry is not launched from a volume, so the combination would advertise a gate that cannot run | Review against applicable hardware evidence; preserve refusal unless equivalence is established. |
| `prep-runtime-selection` | a preparation-only image cannot also read its targets at boot: src/port.h requires the ordered selected-target mode (GU_ACTIVE_SINGLE_TARGET) for GU_RUNTIME_SELECTION, and src/gpu_discovery.c refuses that mode with a preflight-only profile, so no define set satisfies both - the source needs a preparation profile for runtime target selection first | Review against applicable hardware evidence; preserve refusal unless equivalence is established. |
| `prep-runtime-selection-resident` | a preparation-only image cannot also read its targets at boot: src/port.h requires the ordered selected-target mode (GU_ACTIVE_SINGLE_TARGET) for GU_RUNTIME_SELECTION, and src/gpu_discovery.c refuses that mode with a preflight-only profile, so no define set satisfies both - the source needs a preparation profile for runtime target selection first | Review against applicable hardware evidence; preserve refusal unless equivalence is established. |
| `dxe-prep-runtime-selection` | a preparation-only image cannot also read its targets at boot: src/port.h requires the ordered selected-target mode (GU_ACTIVE_SINGLE_TARGET) for GU_RUNTIME_SELECTION, and src/gpu_discovery.c refuses that mode with a preflight-only profile, so no define set satisfies both - the source needs a preparation profile for runtime target selection first | Review against applicable hardware evidence; preserve refusal unless equivalence is established. |

## 6. Verification and acceptance

Extend existing tests, not a second framework. Compare all legacy inputs with canonical resolution, expected builder arguments, status token, actual filename and attestation fields. Include negative combinations: synthetic stage, DXE as boot application, incompatible probe/selection/handoff, missing explicit mutating consent, wrong target order, ambiguous ESP, invalid selection, incompatible resume/combined, wrong companion, maintenance with additional EFI files, unsupported options and stale INTENT. Preserve fail-closed errors and do not weaken assertions to obtain green status.

Test the resolved options through actual PowerShell entrypoints using controlled fixtures, existing host tests and Windows EFI builds. Verify that Stop-only excludes firmware-unlock source, synthetic builds cannot execute, diagnostics including new Preflight do not persistently mutate firmware, and maintenance cannot start or register another image. Compare normalized actual build evidence, not filename equality alone; compile/link changes may change binary hashes. Record CI run and results. Do not claim live hardware validation from CI.

For unchanged verified combined execution, preserve the recorded physical result as the reference; no repeated hardware operation needed for mechanical wrapper/alias consolidation. If a proposed source change alters a real admission or execution boundary beyond the proven scope, isolate it and require applicable evidence before claiming equivalent physical behavior. Numerical reference-output qualification remains a separate open task. Do not force a repeat transaction or clear INTENT to test catalogue changes.

## 7. Recommendation: execution order and owner / agent

Agent: complete G0 and R1 first, then diagnostic and compile migrations, followed by driver-transition, full firmware and maintenance. Keep changes small and retain working aliases until V1/V2 pass. Update status in place with commit and test evidence.

Owner: no hardware action for the audit or migration. Preserve existing logs, receipts and INTENT until a separately reviewed experiment. Use only presently implemented commands until migration is verified.

Production flags and qualification gaps remain separate from experimental profile classification.
