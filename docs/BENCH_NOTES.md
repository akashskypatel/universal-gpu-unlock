# Bench notes: the four link conditions and where they are set

The admission policy refuses an unlock on a PCIe link that has logged an error,
that asks for ASPM, that is not trained, or that does not report itself up
(`GuCheckLinkState`, `src/checks.c`). `RUN0006` and `RUN0007` are the first two
logs where that policy met the bench, and both cards failed three of the four:

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
| sticky error bits (CE/UR) | `OK_ERRORS` | Device Status (config 0x06), set by hardware and by firmware enumeration | a **Fundamental Reset** — full AC removal, not a reboot. The image will not write Device Status: that is a config write, and the admission path makes none | `ERRORS=0` and `ERROR_CE=0 ERROR_UR=0` |
| ASPM request | `OK_ASPM` | Link Control (config 0x10), programmed by firmware or the OS | the BIOS knob above; for an OS-programmed value, the Windows power plan's *PCI Express → Link State Power Management* | `ASPM=0` |
| training | `OK_TRAINED` | Link Status bit 11 | seating, slot, riser, the card itself | `TRAINED=1` |
| data-link active | `OK_UP` | Link Status bit 13 | usually a consequence of the two above: a link idling in L1 reports the data-link layer inactive | `UP=1` with `ASPM=0` |

`UP=0` next to `ASPM=2` is probably one fact, not two. If `UP` is still `0` with
`ASPM=0` and `TRAINED=1`, the link is not coming up and that is a hardware
finding.

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
