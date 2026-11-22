# Bench notes: the four link conditions and where they are set

The admission policy refuses an unlock on a PCIe link that has logged an
*uncorrected* error, that asks for ASPM, that is not trained, or that negotiated no
width or speed (`GuCheckLinkState`, `src/checks.c`). `RUN0006` and `RUN0007` are the
first two logs where that policy met the bench, and both cards failed three of the
five things it reads:

```
ACTIVE_LINK[0] BDF=0000:01:00.0 PCI_LINK=PCIE_ERROR_LOGGED WIDTH=16 MAX_WIDTH=16
  SPEED=4 MAX_SPEED=4 ASPM=2 TRAINED=1 UP=0 ERRORS=9 ERROR_CE=1 ERROR_UR=1 ADMISSION=REFUSE
ACTIVE_LINK[1] BDF=0000:04:00.0 PCI_LINK=PCIE_ERROR_LOGGED WIDTH=4 MAX_WIDTH=16
  SPEED=4 MAX_SPEED=4 ASPM=2 TRAINED=1 UP=0 ERRORS=9 ERROR_CE=1 ERROR_UR=1 ADMISSION=REFUSE
```

This page is about the bench, not the image: what each condition is, where it is
set on this machine, and what evidence says it moved.

## The board's ASPM knob, as photographed on 2026-11-21

||||
| --- | --- | --- |
| Path | `Advanced \ PCIe Subsystem Settings` | the only ASPM entry the BIOS search returns (`1 results found`) |
| Setting | `ASPM Control for CPU PCIe` | options: `Disabled`, `L0s Entry`, `L1 Entry`, `L0s And L1 Entry`, `Auto` |
| Was | `Auto` | which is how the observed `ASPM=2` (Link Control L1) can exist |

**`Auto` is not `Disabled`.** `Auto` leaves the choice to firmware and the OS, and
the snapshot shows the result: Link Control requests L1 on both cards. `Disabled`
is the value that stops the request being programmed.

**One knob covers CPU-attached links.** `0000:01:00.0` (the 4090) is on the CPU's
x16 lanes. `0000:04:00.0` (the 3080) is on a bus number in the chipset range, so
this setting may not govern it at all; boards that split the control expose a
second entry (`ASPM Control for PCH PCIe`, or under `AMD CBS \ NBIO \ PCIe ASPM`).
If this board has only the one and the 3080 still reads `ASPM=2` with it disabled,
the request is coming from somewhere this page does not name, and that is a finding
to record rather than a second setting to guess at.

## What each condition needs, and what proves it moved

| condition | `OK_` flag | where it comes from | what changes it | proof |
| --- | --- | --- | --- | --- |
| uncorrected error bits (FER/NFER) | `OK_ERRORS` | Device Status (config 0x06): bits 2 and 1 | a **Fundamental Reset** — full AC removal, not a reboot. The image will not write Device Status: that is a config write, and the admission path makes none | `OK_ERRORS=1`, and `ERROR_FATAL=0 ERROR_NONFATAL=0` |
| corrected / unsupported bits (CE/UR) | none: recorded as `ERROR_CE`, `ERROR_UR` | Device Status bits 0 and 3 | firmware enumeration sets them on this machine on every boot (RUN0008, RUN0009), so there is nothing to change; a correctable error is corrected by definition | they are in the log and do not refuse |
| ASPM request | `OK_ASPM` | Link Control (config 0x10), programmed by firmware or the OS | the BIOS knob above; for an OS-programmed value, the Windows power plan's *PCI Express → Link State Power Management* | `ASPM=0` |
| training | `OK_TRAINED` | Link Status bit 11 | seating, slot, riser, the card itself | `TRAINED=1` |
| negotiated width and speed | `OK_LINK` | Link Status bits 3:0 and 9:4 | training, the slot's lane wiring, the card | `WIDTH` and `SPEED` non-zero, `SPEED` not the reserved `0xf` |
| data-link active | reported as `UP`, **not weighed** (`UP_WEIGHED=0`) | Link Status bit 13 | nothing: RUN0008 and RUN0009 read `UP=0` on the 4090 with `ASPM=0`, x16 gen4 trained, while answering the very config read that produced the snapshot | `UP` is in the log; the verdict does not use it |

`UP=0` was *assumed* to be a consequence of the L1 request. RUN0008 disproved
that: with the CPU-PCIe ASPM request disabled, the 4090 read `ASPM=0 TRAINED=1
UP=0` while a x16 gen4 link was answering the read. The bit is therefore recorded
and not weighed - see the corrections below.

## The order that produces a clean reading

1. Change the BIOS setting, save, and let the machine power off (`shutdown /s`, not
   restart).
2. Remove AC power for 60 s. A warm reboot does not clear the sticky bits; only a
   Fundamental Reset does.
3. Boot the stick **from the firmware boot menu**, with no Windows boot in between:
   whatever is in Link Control at that moment was programmed by firmware, and that
   is the value the policy will judge.
4. Score the log and read the two `ACTIVE_LINK[n]` lines. `ADMISSION=ALLOW` on both
   is the precondition for a waived boot; `ADMISSION=REFUSE` means the waived image
   would refuse the same way, so no waived boot is needed to find out.

```powershell
python3 tools\verify_run_log.py G:\AD102-UNLOCK\RUN0008.TXT --stage prepare
```

## Record what was changed

`M3_LOG_MEDIUM_EMPTY` and `M4_DONOR_CARDS_RECORDED` are recorded per run. A bench
change belongs next to them: the setting, its old and new value, the cold-boot
protocol, and the two `ACTIVE_LINK` lines that followed. A link state is not a
property of the card; it is a property of what the machine did before the image
ran, which is why it goes in the log rather than in the code.

## What RUN0008 and RUN0009 settled

Two boots of the preparation image, the second after a full AC removal with the
CPU-PCIe ASPM request set to `Disabled`:

| | 4090 `0000:01:00.0` | 3080 `0000:04:00.0` |
| --- | --- | --- |
| RUN0008 (before the power cycle) | `ASPM=0 ERRORS=9 UP=0` | `ASPM=2 ERRORS=9 UP=0` |
| RUN0009 (after 60 s without AC) | `ASPM=0 ERRORS=9 UP=0` | `ASPM=2 ERRORS=9 UP=0` |

1. **The BIOS knob works and is scoped exactly as the label says.** The
   CPU-attached 4090 went from `ASPM=2` to `ASPM=0`; the chipset-attached 3080 did
   not move, so its L1 request comes from somewhere this board does not expose as
   an "ASPM" entry.
2. **A full AC removal does not clear the sticky error bits.** Both cards read
   `ERRORS=9` on the first boot after a Fundamental Reset, before any operating
   system ran. Firmware enumeration sets them, on every boot, on this machine.
3. **`UP=0` is not caused by ASPM** - the 4090 keeps it with the request disabled.

Consequences, taken deliberately:

* The admission policy now refuses on the **uncorrected** bits only (`FER`,
  `NFER`). `CE` and `UR` are recorded (`ERROR_CE`, `ERROR_UR`) and not weighed: a
  correctable error is corrected by definition, and an unsupported request is what
  a device answers when firmware enumerates something it does not implement.
* `DLLLA` is reported (`UP`) and not weighed (`UP_WEIGHED=0`), because a snapshot
  read over the link is evidence that the link carried it.

What still refuses on this bench: the 3080's `ASPM=2`. That is one condition, on
one card, and it is not something the image may fix by writing Link Control.

## The boot that admits a card, and the boot that writes

RUN0010 (`physical-prepare-0008`) is the first log on this bench where a card
reads `ADMISSION=ALLOW`:

```
ACTIVE_LINK[0] BDF=0000:01:00.0 PCI_LINK=OK WIDTH=16 ... ASPM=0 TRAINED=1 UP=0
  ERRORS=9 ERROR_CE=1 ERROR_UR=1 OK_ERRORS=1 OK_ASPM=1 OK_TRAINED=1 OK_LINK=1
  UP_WEIGHED=0 ADMISSION=ALLOW
ACTIVE_LINK[1] BDF=0000:04:00.0 PCI_LINK=ASPM_ENABLED WIDTH=4 ... ASPM=2 ...
  OK_ERRORS=1 OK_ASPM=0 OK_TRAINED=1 OK_LINK=1 UP_WEIGHED=0 ADMISSION=REFUSE
```

The 3080 now refuses on `ASPM` alone. The preparation image refuses either way -
`ACTIVE_EXECUTION=REFUSED REASON=SELECTION_PREPARATION_ONLY BEFORE_GU_INIT=1` -
so a green reading here is a passed gate, not a transaction.

**The waived image asks for a key.** `GuBootChoose` (`src/boot_io.c`) prints a
menu and waits **30 seconds**, then defaults to `S`:

```
  [U] Experimental selected GPU transaction (RAM-only)
  [P] Experimental GPU transaction with S3 resume script
  [I] Install DXE Driver to UEFI NVRAM (Auto-boot / S3)
  [R] Remove DXE Driver from UEFI NVRAM
  [S] Stock Windows (Skip unlock, normal boot)
```

If nothing is pressed the log says `Operator skipped: no GPU transaction, no
driver release; stock Windows` and the machine boots Windows. That is the only
outcome of a run that looks like nothing happened, and it is a person at the
console, not a setting. `U` runs the transaction without the S3 resume script;
`P` registers it; `I`/`R` only touch NVRAM.

**What the waived boot adds that no preparation boot can see:** the watchdog line
(`WATCHDOG BOUND=<n> SECONDS ACTIVE=1`, or `BOUND=0 ... REASON=NO_BUILD_THRESHOLD`),
the BAR0 attribute read compared against config, the driver release, DMA
quiescence, the boot-state record written *before* the first mutation so a power
loss still names the card, and one ordered step per target with no automatic
retry. All of those come after `BEFORE_GU_INIT=1`, so RUN0010 says nothing about
them.
