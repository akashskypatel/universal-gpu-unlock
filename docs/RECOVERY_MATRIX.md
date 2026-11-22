# Recovery matrix

What to do when a boot did not end the way it should, and what proves you are
back. This is a plan checked against the code. The recovery routes are covered by
source/host contracts but have **not** been verified on hardware. The user reported
full recovery after a 60-second cold boot with USB removed; that does not validate
the new firmware recovery routes. Eligibility gates still read
`CAN_DETACH=false`, `CAN_UNLOCK=false`, `CAN_FLASH=false`, `RECOVERY_VERIFIED=false`.

Every class below is grounded in something this repository prints or stores:

* the **symptom** is a log line the source actually emits (the token is the
  format string, so it is verifiable by grep);
* the **record** is what survives in NVRAM (`GuBootState`, the A7 record) - the
  only state that outlives a failed boot;
* the **simulator row** is the scenario that exercises the software half, when one
  exists. It shows the image behaves as described; it says nothing about silicon.

`docs/recovery_matrix.json` is the source of truth and this file is rendered from
it by `tools/render_recovery_matrix.py`; `tests/test_recovery_matrix.py` fails if
they differ, if a symptom line no longer exists, or if any class starts claiming a
verified recovery.

Recovery is per card and per class. There is no rollback or re-flash path. A
failed/interrupted boot record now blocks another DXE transaction and selects a
no-GPU recovery path; the code does not attempt to repair GPU firmware state.

---

### `power-loss-mid-transaction`

**Power lost during the transaction**

| | |
| --- | --- |
| Symptom | `BOOT_STATE WARNING=previous boot stopped during a transaction` (src/boot_io.c) |
| Record | `Phase`, `BootCount`, `DeviceId`, `Armed` |
| Record state | Phase == INTENT (1) with no OUTCOME: the record was written before the first mutation and the sequence never returned. |
| Card state | Unknown. The selection write, the WPR2 window, or an engine reset may have been applied; the card may still be executing or may be halted with reset released. |
| Machine state | Unknown. The host may have been mid-DMA; firmware state is whatever it was when power dropped. |
| Simulator | none |

Steps:

1. Do not warm-reboot: a warm reset can leave a falcon running with host buffers already freed.
2. Remove power at the wall for 30 s so the card's own firmware state is discarded.
3. Next boot, read the .LOG file on the removable media for BOOT_STATE lines and for the last stage before the sequence.
4. Confirm the card enumerates with its stock identity in the read-only diagnostic; do not run a transaction on that boot.
5. If the diagnostic cannot read configuration space, treat the card as faulty and stop.

Recovered when:

* BOOT_STATE PRESENT=1 with PHASE=2 (OUTCOME) on a later boot
* the read-only diagnostic completes with STABLE=1
* a subsequent normal boot reaches Windows with the card present

Not verified: No hardware run has ever lost power mid-transaction; the software-visible record is exercised on the host only.

---

### `dma-buffers-retained`

**Firmware did not stop, so its buffers were retained**

| | |
| --- | --- |
| Symptom | `firmware did not stop; keeping its DMA buffers until power off` (src/vendor/unlock_610.c) |
| Record | `Phase`, `Status`, `Armed` |
| Record state | OUTCOME with a failure Status and Armed == 0: the sequence returned, but the engine may still read the buffers. |
| Card state | Firmware may still be running. The unlock was not verified and the stock driver must not bind while it is active. |
| Machine state | The image retains the DMA buffers and the device reference on purpose; freeing them would be a use-after-free from the card's point of view. |
| Simulator | `transaction flr-refused` |

Steps:

1. Do not start Windows on this boot. The image already refuses to hand over in this state.
2. Remove power at the wall; the retained buffers die with the boot.
3. Next boot, check BOOT_STATE PRESENT=1 PHASE=2 STATUS for the recorded failure.
4. Run the read-only diagnostic only; it does not touch firmware state.

Recovered when:

* the next boot shows no INTENT record
* the diagnostic reads a valid PMC, SS0/SS1 and PROM digest

Not verified: The simulator shows the image retaining buffers and refusing handover; no card has been observed continuing to DMA.

---

### `function-reset-refused`

**The card refused the function-level reset**

| | |
| --- | --- |
| Symptom | `PCIe FLR returned: result=` (src/vendor/unlock_610.c) |
| Record | `Phase`, `Status`, `Armed` |
| Record state | OUTCOME with a failure Status and Armed == 0. |
| Card state | The permission window is still open: the selectors were written and the reset that closes it did not happen. |
| Machine state | Bus master is clear and the image keeps the buffers it allocated. |
| Simulator | `transaction flr-refused` |

Steps:

1. Do not boot into Windows: the stock driver expects a reset GSP and an open permission window is not a state it recovers from.
2. Power off at the wall, wait, and power on.
3. Next boot, confirm BOOT_STATE shows the failure and run the read-only diagnostic to confirm the selectors are back to their cold values.

Recovered when:

* the diagnostic reports the cold selector values, not the unlock value
* a later transaction is not attempted automatically

Not verified: Modelled, never observed on silicon.

---

### `wpr2-left-open`

**The WPR2 window is still open after the reset**

| | |
| --- | --- |
| Symptom | `post-FLR WPR2 remains enabled` (src/vendor/unlock_610.c) |
| Record | `Phase`, `Status` |
| Record state | OUTCOME with a busy or error Status. |
| Card state | FWSEC did not close its window. The stock driver rejects a preexisting WPR2 on this path, so it will not bind. |
| Machine state | The image stops before any further write and retains its buffers. |
| Simulator | none |

Steps:

1. Power off at the wall; the window is firmware state and does not survive a cold boot.
2. Next boot, run the read-only diagnostic and confirm WPR2 reads zero.
3. If WPR2 is still nonzero after a cold boot, treat the card as faulty.

Recovered when:

* WPR2 low/high both read zero in the diagnostic
* the stock driver binds without a WPR2 complaint

Not verified: The simulator never leaves WPR2 open; this is a hardware-only failure mode.

---

### `stop-hang`

**The vendor driver Stop call never returns**

| | |
| --- | --- |
| Symptom | `MSI release refused` (src/msi_release.c)<br>`WATCHDOG=ARMED` (src/boot_io.c) |
| Record | `Phase`, `BootCount` |
| Record state | Phase == INTENT: the record is written before the detach, so a hang inside the vendor callback leaves INTENT behind. |
| Card state | Whatever the vendor Stop callback was doing. The image has no proof that it is safe to continue. |
| Machine state | The firmware watchdog is the only thing that ends the hang, and only in a build that arms it (GU_WATCHDOG_SECONDS). |
| Simulator | `transaction watchdog` |

Steps:

1. Wait for the machine to reset itself if the bounded build is in use; do not power off during a vendor callback.
2. If no reset arrives, power off at the wall.
3. On the next boot, expect the same BOOT_STATE WARNING as a mid-transaction power loss and follow that procedure.

Recovered when:

* the machine resets without operator action, or powers off cleanly
* the next boot reports the INTENT record from the hung boot

Not verified: No Stop bound has been measured, which is why the timer is opt-in and the AD102/GA102 Stop path is refused.

---

### `card-refused-before-any-write`

**The image refuses this card before writing anything**

| | |
| --- | --- |
| Symptom | `STOCK_PROFILE=REFUSED` (src/profile.c)<br>`SKU_GEOMETRY=REFUSED` (src/profile.c)<br>`PROM_IDENTITY=%s` (src/profile.c) |
| Record | `DeviceId`, `Status` |
| Record state | No INTENT record is written: the refusal happens before the first mutation, so nothing was in flight. |
| Card state | Untouched. Zero BAR0 writes, zero FLRs, WPR2 not opened. |
| Machine state | Windows boots normally, or the diagnostic runs. Nothing to recover. |
| Simulator | `prepare stock-mismatch` |

Steps:

1. Read the refusal: it names the compiled profile, the chip, or the observed digest and the reason.
2. For a stock-profile refusal, this image was built for a different card; use the build that matches, or capture the card's values with the diagnostic.
3. For a SKU refusal, no register table exists for that chip; the diagnostic still runs and is the way to capture one.
4. For a reference refusal, capture the digest the log prints and add a reference to a reviewed build.

Recovered when:

* the diagnostic completes with STABLE=1 and the digest printed
* no BOOT_STATE INTENT record exists for the boot

Not verified: The refusal semantics are simulated; the digests and cold values of real cards are not captured.

---

### `asset-install-incomplete`

**The staged bundle on the ESP is stale or incomplete**

| | |
| --- | --- |
| Symptom | `ASSET_INSTALL PRESENT=1` (src/boot_io.c) |
| Record | `BootCount` |
| Record state | The install record is a separate NVRAM variable; it names the boot that staged it and the bundle digest. |
| Card state | Untouched: the install build never prepares or drives the card. |
| Machine state | Nothing to recover; the next boot either re-stages from removable media or reports the mismatch. |
| Simulator | `prepare install-assets` |

Steps:

1. Compare the recorded bundle digest with the pinned version in the image.
2. Re-run the install build from removable media; it is resumable and does not overwrite a verified file.
3. If the record reports CORRUPT, delete the variable and re-stage.

Recovered when:

* ASSET_INSTALL PRESENT=1 with the pinned bundle digest and STATE=CURRENT
* a boot without removable media loads the staged bundle

Not verified: Install and resume are simulated; no ESP has been written by this image.

---

### `partial-multi-target`

**A multi-target sequence stopped between cards**

| | |
| --- | --- |
| Symptom | `SELECTED_GPU_STEP=` (src/gpu_discovery.c) |
| Record | `Phase`, `Status`, `DeviceId`, `Segment`, `Bus`, `Device`, `Function` |
| Record state | OUTCOME names the card that was being driven; the steps logged before it name the cards that already completed. |
| Card state | The completed cards are in their post-transaction state; the next card was never touched. |
| Machine state | Peers are unchanged, because the coordinator verifies them before and after each card. |
| Simulator | none |

Steps:

1. Read the SELECTED_GPU_STEP lines in order: COMPLETE for finished cards, then the failing step.
2. Recovery is per card, using the class that matches the failure, not for the sequence as a whole.
3. Re-run only after the failing card's class procedure has been followed to its proof.

Recovered when:

* each card's own class proof holds
* the next boot reports no INTENT record

Not verified: Only one card has ever been driven, and not on hardware; the two-target run in the plan has not happened.

---

### `dxe-failure-recovery-latch`

**DXE transaction failed and the next boot must not repeat it**

| | |
| --- | --- |
| Symptom | `DXE_RECOVERY=FAILED_OR_INTERRUPTED` (src/dxe_main.c)<br>`USB_RECOVERY=FAILED_OR_INTERRUPTED` (src/app.c) |
| Record | `Phase`, `Status`, `BootCount`, `DeviceId`, `Segment`, `Bus`, `Device`, `Function` |
| Record state | An unmatched INTENT or an OUTCOME whose Status is not EFI_SUCCESS is treated as a persistent recovery latch. |
| Card state | Unknown until separately diagnosed. Recovery changes only AdaUnlock boot registration; it does not reset or repair GPU state. |
| Machine state | The next DXE invocation performs no GPU discovery or mutation and removes the owned option from DriverOrder. The USB recovery route removes the owned Driver#### entry entirely before handing off to Windows. |
| Simulator | none |

Steps:

1. Without USB, allow the next boot to reach the DXE recovery check; it skips GPU work, disables the owned DriverOrder entry, and continues normal boot.
2. With USB, launch the app; it detects the failed/interrupted record before GPU discovery, removes the owned DXE entry, and boots Windows without touching PCI.
3. If the NVRAM operation fails, stop and use the firmware boot picker to choose Windows Boot Manager; do not select an unlock/transaction mode.
4. After recovery, use only the read-only diagnostic until GPU state is independently confirmed.

Recovered when:

* the recovery log reports DXE_RECOVERY or USB_RECOVERY and no transaction is attempted
* the owned AdaUnlock option is absent from DriverOrder (no-USB route) or removed from Driver#### and DriverOrder (USB route)
* the system reaches Windows without another DXE transaction

Not verified: Source contract tests cover ordering and ownership of the recovery actions; these new firmware paths have not been tested on hardware. The user separately reported full recovery after a 60-second cold boot with USB removed.

---

