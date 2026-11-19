# Host simulator for the active unlock image

**State: HOST SIMULATION ONLY. No physical GPU was touched.**

`sim/` builds the real application objects (`src/`, compiled with `-D__KERNEL__`
against the vendored EFI headers) and links them against a host-side model of
the firmware environment. The image therefore executes its own code paths —
discovery, cold capture, FWSEC/GFW transaction, MSI release, handover — with no
hardware present. Everything the model reports describes the model.

## What it demonstrates

* The active image reaches `GuExecuteUnlock` and completes one selected target:
  `SELECTED_GPU_STEP=COMPLETE NEXT=1 PEERS_VERIFIED=1`.
* The access window it opens is exactly the one it models: one WPR2 open by
  FWSEC, no PROM writes, selector registers written in order **before** the
  function-level reset, and WPR2 closed (`high == 0`) by that reset.
* The post-transaction invariants hold: armed, no fault, no retained buffers,
  no live DMA, one recorded FLR, handover ready, stock driver released.
* The image's own self-report agrees with the model: the harness asserts the
  captured kernel log says `Selected GPU result=0 fault=0 safe=1`,
  `SELECTED_GPU_STEP=COMPLETE`, `Selected preboot transaction verified`,
  `MSI pre-close silicon verification: valid=1` and
  `restore PCI command: actual=0x2`. A passing run is not just "no check fired";
  the two independent records have to match.
* Refusals are refusals: a card that presents an enabled MSI vector is rejected
  before any register write, and a card that drops the FLR makes the image fail
  at the firmware transaction while leaving the platform context faulted and
  retained, so firmware-visible memory is not reclaimed and the next selection
  is blocked.

## What it does not demonstrate

The simulator cannot establish that the sequence is sufficient on silicon — only
that it is self-consistent. Output is labelled `ENVIRONMENT=HOST_SIMULATOR` and
`ACTUATION_VERIFIED=0`; `CAN_DETACH`, `CAN_UNLOCK`, `CAN_FLASH` and
`RECOVERY_VERIFIED` remain false. The register map, the FWSEC command record in
DMEM, the falcon DMA/IMEM/DMEM layout and the FRTS page arithmetic are models
written from the vendored code and public documentation, not measurements.

## Running it

```sh
python3 tools/run_simulation.py verify          # all scenarios, exit code = result
python3 tools/run_simulation.py prepare         # boot, discovery, cold capture
python3 tools/run_simulation.py transaction     # the ordered unlock
python3 tools/run_simulation.py prepare msi-enabled
python3 tools/run_simulation.py transaction flr-refused
```

`SIM_CFLAGS` adds compiler flags; `SIM_DEBUG=1` enables the host file-system
diagnostics in `sim/sim_world.c`. The stage arguments are
`<prepare|transaction> [variant]`; `verify` ignores them.

`verify` runs every scenario in the table below and prints
`SIM_VERIFY=PASS SCENARIOS=18 FAILED=0`.

| Scenario | Card | Expected |
| --- | --- | --- |
| `prepare` | nominal | discovery and cold capture succeed |
| `transaction` | nominal | unlock completes, WPR2 opened once then closed |
| `prepare msi-enabled` | MSI enable bit set | refused at discovery, zero BAR0 writes, zero FLRs |
| `transaction flr-refused` | DEVCTL FLR bit clear | fails at the firmware transaction, WPR2 stays open, buffers and device reference retained |
| `transaction dma-high` | allocator ignores the ceiling | refuses to place DMA above the card's limit |
| `transaction rebar` | 256 MiB BAR0 above 4 GiB | addresses registers through the firmware-assigned window |
| `transaction bar-above-4g` | BAR0 at 4 GiB | works without truncating a 32-bit BAR address |
| `prepare runtime-target` | operator selection names the card | binds the requested BDF from NVRAM |
| `prepare runtime-absent` | operator selection names a missing card | refuses, binds nothing, writes no BAR0 |
| `prepare ad104-sku` | `10de:2786` (RTX 4070) | refused at prepare: no captured register table, zero BAR0 writes |
| `prepare install-assets` | nominal | verifies the bundle from removable media, stages it on the ESP, records it as CURRENT, touches no BAR0 |
| `prepare stock-match` | card presents `0x03100062` | the `original03100062` build accepts it, logs `STOCK_PROFILE=MATCH` |
| `prepare stock-mismatch` | card presents `0x03000062` | the same build refuses it before any write: `EFI_UNSUPPORTED`, zero BAR0 writes, zero FLRs, `REASON=CARD_DOES_NOT_MATCH_BUILD` |
| `transaction watchdog` | nominal, `GU_WATCHDOG_SECONDS=600` | the timer is armed once with that bound, the transaction still completes, and the cancel removes it |
| `transaction preserve` | nominal, `entryLatchedSs1Preserve` build | writes SS0 only, never SS1, and completes - the policy preserves the selector it must not touch |
| `transaction untracked` | card presents `0x03100062`, `original03100062` build | writes both selectors in stock order with no phase tracking, and says so |
| `prepare prom-refused` | nominal, `GU_PROM_REFERENCE_REQUIRED` build | refused at prepare: no captured reference for this card, `EFI_UNSUPPORTED`, zero BAR0 writes, diagnostic still allowed |
| `prepare prom-admitted` | nominal, same build plus the model's pinned reference | admitted, `PROM_IDENTITY=VERIFIED`, preparation completes |

Rows that name a build run a separate binary of the same sources: `stock-match`,
`stock-mismatch` and `transaction untracked` use
`-DGU_STOCK_PROFILE_ORIGINAL_03100062`, `transaction preserve` uses
`-DGU_ENTRY_LATCHED_SS1_PRESERVE` (each replaces the default entry-latched
policy, because exactly one profile is allowed), and `transaction watchdog` adds
`-DGU_WATCHDOG_SECONDS=600`. Every other row uses the default build, whose
profile carries no selector expectation and whose watchdog threshold is `0` —
those rows assert that nothing was armed, so the default stays inert.

`SIM_MODEL_PROM_SHA256` prints the digest of the model's ROM, so a scenario can
pin it: the synthetic reference is exactly that digest, and if `sim_gpu.c` builds
a different ROM the `prom-admitted` row fails instead of quietly admitting a
different card.

`python3 tools/run_simulation.py matrix` builds and links every profile without
running anything and prints `SIM_MATRIX=PASS PROFILES=5`. It exists because the
profile matrix was never built: three of the four policies compiled and then
failed to link, and nobody noticed until a host test tried to link them.

## The model

`sim/sim_gpu.c` is an AD102-class PCI function: `10de:2684`, subsystem
`a140:10de`, class `030000`, BAR0 `0x80000000` (16 MiB), NV_PROM at `0x300000`,
and a structurally valid 1 MiB ROM image so the profile and FRTS checks pass.

* **Config space** — power management, MSI and PCIe capability lists. The
  DEVCTL FLR bit drives `SimGpuFunctionReset`, which clears WPR2
  (`0x001fa824/28`), halts both falcons and drops the engine buffers, while
  leaving the PLM and link-speed registers alone (the driver's post-reset
  contract depends on that).
* **Falcon engines** — SEC2 (`0x00840000`) and GSP/FWSEC (`0x00110000`) with
  demand-zeroed IMEM (256 KiB) and DMEM (64 KiB), a 256-byte DMA block, IMEM
  destination and DMEM offset accumulating separately, and a start that reads
  the FWSEC command record from DMEM to find the FRTS page and open WPR2 over
  it. There is no out-of-band shortcut: an empty DMEM yields no page and an
  `-EIO` from `vbios_610.c`.
* **Trace** — a 512-entry detail ring plus per-kind totals and first/last
  ordinals, and a watched-register table. The run generates ~21,400 events, so
  ring capacity is not the record: ordinals answer "did X happen, and in what
  order relative to Y" even after the detail entries are overwritten.
* **Switches** — `SimGpuEnableMsi`, `SimGpuSetFlrSupported`, used by the
  scenario matrix above.

`sim/sim_world.c` supplies boot services, a directory-backed file system on two
volumes, the console, DMA allocation, and the firmware graph the release path
inspects: a bound stock driver, a headless target, and an AMD console owner.
The machine has no display attached to the NVIDIA card, which is what the image
requires.

## Reading the output

The image's `dev_info`/`dev_err` output is captured rather than echoed
(`SIM_CAPTURED_LOG_BYTES=`), and its claims are asserted as described above.
What the harness prints for a nominal `transaction` run:

```
SIM_ENVIRONMENT=HOST_SIMULATOR PROFILE=GU_SIMULATED_ACTUATION ACTUATION_VERIFIED=0 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLASH=0 RECOVERY_VERIFIED=0
SIM_DEVICE BDF=0000:01:00.0 ID=10de:2684 SUBSYS=a14010de CLASS=000300
SIM_MODEL PROM_BYTES=1048576 BAR_BYTES=16777216 STOCK_DRIVER_BOUND=1
SIM_STAGE=GuExecuteUnlock
SIM_RESULT=GuExecuteUnlock STATUS=0 STAGE=selected-gpu-state-finish
SIM_TX TRACE=21368 BAR_WRITES=739 PROM_WRITES=0 CONFIG_WRITES=13 FLR=1 FW_STARTS=2 WPR2_OPENS=1
SIM_TX ORDER FLR=21353 SS0=21350 SS1=21351 SEL_WRITES=1:1
SIM_TX STATE ARMED=1 FAULT=0 RETAINED=0 FLRS=1 DMA=0 STARTED=1 READY_FOR_WINDOWS=1 WPR2=00000000:00000000
SIM_TX BAR0_DELTA=31
SIM_CAPTURED_LOG_BYTES=12795
SIM_RESULT=PASS CHECKS_FAILED=0
```

`SIM_TX ORDER` is the ordering evidence: the selector writes are ordinals
21350 and 21351, the function-level reset is 21353. `SIM_TX BAR0_CHANGED[n]`
lists every register the run left different from the cold state, which is what
`BAR0_DELTA` counts.

The `flr-refused` scenario instead ends:

```
SIM_RESULT=GuExecuteUnlock STATUS=8000000000000007 STAGE=firmware-transaction
SIM_TX TRACE=21368 BAR_WRITES=739 PROM_WRITES=0 CONFIG_WRITES=13 FLR=0 FW_STARTS=2 WPR2_OPENS=1
SIM_TX ORDER FLR=-1 SS0=21350 SS1=21351 SEL_WRITES=1:1
SIM_TX STATE ARMED=1 FAULT=1 RETAINED=1 FLRS=1 DMA=20612 STARTED=1 READY_FOR_WINDOWS=0 WPR2=05ffe000:05ffe400
SIM_ABORT=function-reset REFUSED=1 WPR2=05ffe000:05ffe400 STAGE=firmware-transaction STATUS=8000000000000007 RETAINED=1 DMA=20612 FAULT=1
SIM_RESULT=PASS CHECKS_FAILED=0
```

`SIM_RESULT=PASS` there means the expectation held: the image failed, reported
`FLR=-1` (no reset happened at all), and left the window and the buffers it
could no longer prove idle alone.

## Limits worth keeping in mind

The model is a single card. A two-GPU arrangement, a shadowed VGA legacy window,
a real MSI/MSI-X allocation, an SMMU, and a card whose ROM differs from the
one captured under `AD102-ASSETS/` are all untested by it. Any conclusion drawn
from a passing run is a statement about ordering and cleanup in the image, not
about a 4090 accepting the result.
