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

## Reading a stalled waived boot

RUN0011 (`physical-actuate-0001`) was the first waived boot on this bench. It wrote
thirteen lines, went black, and sat for over a minute until it was power-cycled.
Nothing in the file said where it stopped, because the band it stopped in
(`GuPrepareBoundGpu`: five firmware files off removable media - one of them
84,277,400 bytes - then the PROM read) printed nothing until it finished.

The build now traces every phase, to the log and to the screen:

```
TRACE STEP=2 ELAPSED=3s PHASE=asset-read INDEX=2 NAME=gpu-unlock/610.43.03/gsp_ga10x.bin BYTES=84277400
TRACE STEP=2 PHASE=asset-read DONE=3 OF=5
```

Read the last three lines before the silence like this:

| Last thing in the log | What it means | What to do |
| --- | --- | --- |
| `PHASE=asset-read INDEX=<n> NAME=<f>` | it is reading that file from the stick, not hung | wait; `DONE=n OF=5` follows each file |
| `PHASE=assets` and nothing more | the media read did not return | stick out, reseat, try another port |
| `PHASE=diag-config` / `diag-registers` | it stopped at config space or the first BAR0 read | the trace now names the call; report the line |
| `PHASE=diag-prom-copy` with `DONE=` climbing | the 1 MiB PROM read is running | wait for `DONE=1048576 OF=1048576` |
| `PHASE=vendor-wait ACCUMULATED_US=` climbing | a firmware step is waiting, and it is alive | wait for the bound |
| `[gu] STOP stage=...` on screen | the run ended by itself | copy the line and the twenty lines above it |

Two properties are deliberate and worth keeping: the trace lines print **even
while the image is silent** (one line per phase, not a dump), and the
`WATCHDOG BOUND=` line now appears *before* the preparation, so a boot that stalls
before the operator menu still says whether it was bounded.

## RUN0012–RUN0014: detach drift and the DXE install failure

RUN0012 (`U`) and RUN0013 (`P`) stop identically at `driver-release`: the
vendor driver's final `Stop()` clears I/O and memory decode in PCI Command
(`0x100003` to `0x100000`). The refusal was the snapshot gate rejecting that
known detach side effect, not a flaky link. The code now restores the captured
Command word only for that narrowly constrained final detach case, checks the
readback, and then applies the original full snapshot rule unchanged.

RUN0014 (`I`) instead reports `EFI_NOT_FOUND` while looking for
`\\EFI\\Drivers\\AdaUnlockDxe.efi` on the boot image's volume. The
`actuate-one-target` package does not include the `dxe-*` driver variant. Stage
that variant beside the image before choosing I, or skip I; the diagnostic now
identifies the missing file and stage. RUN0012 plus RUN0014 pass the five log
verification gates as a pair; this is not a physical verification of the code
change (`HARDWARE_VERIFIED=0`).

The next permitted physical run still depends on the unresolved link-setting
choice for `04:00.0`: PCH-side ASPM control, a documented chipset-link Link
Control write, or single-target-only operation. No BIOS setting or link write
was changed based on these captures.

## Primary graphics adapter selection: documented BIOS control

The attached AMD 800-series BIOS user guide (p. 31) documents
`Advanced -> Integrated Graphics Configuration -> Initiate Graphic Adapter`,
with `IGD` (Integrated Graphics Display) and `PEG` (PCI-Express Graphics
Device) choices; it says this selects the primary boot graphics device. The
attached System Status screenshot identifies the bench board as MSI B850
Gaming Plus WiFi (MS-7E56), BIOS `E7E56AMSI.1AA3`, build date 2026-09-03.
The manual cautions that available options can vary by BIOS version, so confirm
that exact item is present before changing it.

This is a documented candidate for console-routing tests, not proof that IGD
selection removes NVIDIA from configured `ConOut`/`ErrOut` or works without an
iGPU monitor. Physical run `0015` (three reported displays on the 3080; none on
AMD/4090) captured only the 3080 GOP/console path. Run `0016` (one reported
display on AMD and two on the 3080; none on the 4090) captured both GOPs and
both console paths and remained `CONFLICT`.

In run `0017`, the operator set `Initiate Graphic Adapter=IGD` and kept one
display on AMD and two on the 3080, with the 4090 headless. The AMD GOP is now
listed first and reports 1920x1080; the 3080 GOP is still present. Both GPUs
remain in `ConOut` and `ErrOut`, so the verdict is still `CONFLICT`. The BIOS
setting may affect primary selection/order, but this capture does not show an
exclusive AMD console or prove the AMD GOP works headless.

**Next read-only case:** keep IGD selected, remove the monitor from AMD (move
it to the 3080 if needed to preserve the three-display day-to-day layout), keep
the 4090 headless, cold boot, and run the saved-log topology image. This tests
whether the AMD GOP/console path persists without an attached display. The
candidate console result remains AMD-only `ConOut` and `ErrOut` with no NVIDIA
matches. Do not alter ASPM or attempt a GPU action.

## Headless iGPU with IGD selected: run 0018

The operator kept `Initiate Graphic Adapter=IGD` and reported both the iGPU and
RTX 4090 headless. Run `0018` enumerates the AMD iGPU at `0000:0f:00.0` in PCI
topology, but discovers no AMD GOP. The sole GOP belongs to the 3080 at
`0000:04:00.0`; its active EDID identifies an attached LG display, while the
4090 remains enumerated with `GOP_COUNT=0`. Configured `ConOut` and `ErrOut`
contain the 3080 path only. The active firmware console interface matches the
system console, but the diagnostic cannot identify which physical display
received output. Its isolation classifier remains `CONFLICT` because no
independent GOP/console association exists, and the final verdict is
`CONFLICT NVIDIA_CONOUT_BLOCKER=1`.

Together with run `0017` (iGPU display attached, both AMD and 3080 GOP/console
paths present), this shows that IGD selection does not expose an AMD GOP in the
reported headless-iGPU layout. It does not prove whether firmware would route
text to the 3080 under every boot condition, nor does it make a display-active
3080 safe to release. The 3080 still reports `ASPM=2` at Gen4 x4; the iGPU root
function reports ASPM=0. No firmware source change is indicated by this
read-only capture.

The planned headless A/B check was run as `0021`: with only the BIOS selector
changed to `PEG`, and iGPU/4090 still headless, the GOP and configured console
paths remain 3080-only. See the next section for the result and next test.

## Headless iGPU with PEG selected: run 0021

The operator cold-booted after changing `Initiate Graphic Adapter` from `IGD`
to `PEG`, keeping the iGPU and RTX 4090 headless. As in `0018`, the AMD iGPU
(`0000:0f:00.0`) is still enumerated in PCI topology but has no GOP. The only GOP
and the sole configured `ConOut` and `ErrOut` path belong to the RTX 3080
(`0000:04:00.0`). The 4090 (`0000:01:00.0`) is still enumerated with no GOP.
Thus, in this headless-iGPU arrangement, changing IGD to PEG did not change the
observed GOP or configured console-path membership. The diagnostic retains
`ERROR_CONSOLE_ISOLATION=CONFLICT` and
`TOPOLOGY_VERDICT=CONFLICT NVIDIA_CONOUT_BLOCKER=1`; this is not proof of the
physical display receiving output or of exclusive firmware-console ownership.

The 3080 still reports `ASPM=2`, Gen4 x4. The read-only diagnostic also records
NVIDIA GOP/TextOut opener/driver information as observed-only; that is not
Stop-safety evidence and does not resolve the console blocker. No source change
was warranted.

**Next discriminating read-only case:** keep `PEG` selected and the 4090
headless, temporarily attach one display to the iGPU while leaving a display
on the 3080, then cold-boot and rerun the saved-log topology diagnostic. This
checks whether an attached iGPU display exposes an AMD GOP under PEG, and whether
it changes configured `ConOut`/`ErrOut`, compared with run `0017` (IGD with an
iGPU display attached). Do not change ASPM or run any Stop, preparation, or
actuation image. Even an AMD GOP or altered console-variable paths would not by
itself establish that display-active 3080 release is safe; the ASPM blocker
remains unresolved.

## PEG with attached iGPU and 3080 displays: run 0022

The operator reports `PEG` selected and the RTX 4090 headless. Run `0022`
finds GOPs for the RTX 3080 (`0000:04:00.0`) and AMD iGPU (`0000:0f:00.0`);
the 4090 (`0000:01:00.0`) is enumerated with no GOP. Each configured `ConOut`
and `ErrOut` variable contains both the NVIDIA and independent AMD paths.
Accordingly, the diagnostic reports `INDEPENDENT_CONSOLE_ASSOCIATION=0`,
`ERROR_CONSOLE_ISOLATION=CONFLICT`, and
`TOPOLOGY_VERDICT=CONFLICT NVIDIA_CONOUT_BLOCKER=1`. This is consistent with
run `0017` under IGD with an iGPU display attached: changing IGD to PEG did not
isolate the configured console paths when both GOPs are present. GOP/EDID
records describe firmware interfaces, not a physical monitor count or proof of
which panel received console text. The 3080 remains at `ASPM=2`, Gen4 x4.

### A screen is not the same thing as a log file or a safe release console

The installed DXE path can write its log without rendering those log lines on a
screen. In `src/boot_io.c`, `GuBootOpen` opens/creates
`\\AD102-UNLOCK\\RUNxxxx.TXT` on the volume containing the loaded image;
`GuLog` sends bytes through `EFI_FILE_PROTOCOL.Write` and flushes the file.
These operations are separate from `GuBootSay`/trace screen output through
`ConOut`. The installed DXE entry in `src/dxe_main.c` runs automatically at
ReadyToBoot and does not ask for the interactive app's U/P/I/R/S choice. Thus,
a writable file can hold the log for later inspection even if the screen is not
showing those messages; this still depends on the DXE image volume being
writable and the file writes succeeding.

That does **not** remove the release safety gate. `src/msi_release.c` requires
firmware console, error, and input handles/protocols, then verifies that
configured `ConOut` and `ErrOut` and the live TextOut relationship are owned by
independent AMD survivor GOPs before releasing the selected NVIDIA driver. A
log file is not a substitute for that ownership check. Run `0022` has both
NVIDIA and AMD in the configured variables, so the current fail-closed preflight
must refuse release even though an independent AMD GOP exists. The source does
not establish that a physical monitor must visibly illuminate for every such
protocol check; nor does the read-only diagnostic prove that text appeared on a
particular monitor. Do not bypass these console checks to make the screen-free
case run. No source change was indicated.

**Next read-only capture:** restore the preferred fixed layout—leave PEG
selected, disconnect the iGPU display, keep the 4090 headless, and connect the
intended displays to the 3080—then cold-boot and rerun the topology log. This
records the intended operating layout after the attached-iGPU comparison; it is
not an actuation approval. No ASPM changes, Stop, preparation, or actuation
image for this check.

## Headless iGPU/4090 with PEG selected: run 0023

Run `0023` follows the operator's PEG-selected layout, with the iGPU and 4090
reported headless. PCI inventory still sees the AMD iGPU (`0000:0f:00.0`) and
both NVIDIA GPUs. The 4090 (`0000:01:00.0`) has `GOP_COUNT=0`; the iGPU has no
GOP record. The sole GOP is the 3080 (`0000:04:00.0`), with a valid
2560-by-1440 mode descriptor and active LG EDID. Both configured `ConOut` and
`ErrOut` contain the 3080 path only. Despite the single path, the diagnostic
correctly reports `ERROR_CONSOLE_ISOLATION=CONFLICT`,
`INDEPENDENT_CONSOLE_ASSOCIATION=0`, and
`TOPOLOGY_VERDICT=CONFLICT NVIDIA_CONOUT_BLOCKER=1`: there is no independent
GOP/console association. The console handle's device path is unavailable, so
the report does not prove which physical screen received text. The 3080 and its
chipset links continue to report `ASPM=2`; the 3080 link is Gen4 x4 and records
9 device errors.

This does not authorize sequential Stop/release. Removing the ASPM admission
gate or treating an output file as a replacement for the console-ownership and
link-safety checks would turn unverified state into acceptance. No actuation
image or hardware mutation was produced from this capture.

For logging, the installed DXE implementation already separates `GuLog`
(`EFI_FILE_PROTOCOL.Write`/flush to the image volume's numbered run file) from
screen output. DXE has no interactive menu. The read-only topology image used
for this run saved its durable log; its capture reported `SCREEN=AVAILABLE`.

After the operator explicitly accepted the unverified bench risk, a separate
catalogued variant `dxe-actuate-sequence-bench-override` was added for exactly
the `0000:01:00.0` then `0000:04:00.0` order. Only that opt-in DXE build accepts
nonzero ASPM and bypasses the independent-AMD-console ownership checks; it
still checks link errors/training/width/speed, target identity, peer state,
transaction ownership, and containment/recovery logic. It writes `GuLog` to
the image volume and suppresses ordinary screen output. After read-only
preparation and immediately before mutation, it emits a mandatory warning
through firmware `SystemTable->ConOut` and requires a three-second stall; if the configured
console or stall service is unavailable, the image refuses GPU work. The
warning says NVIDIA-connected displays may go black, disconnect, or flicker,
and that reattachment is only attempted, not guaranteed. `ConOut` may be a
splitter or non-display console; the image cannot prove which physical screen
receives the warning. All normal variants retain their prior gates. This
variant has **not** been built, signed, staged, or hardware-tested in this Linux
workspace. Its catalogue attests the bypasses explicitly and keeps
`ACTUATION_VERIFIED=0` / `RECOVERY_VERIFIED=0`.

## The chipset-connected RTX 3080: what the attached topology does and does not say

The uploaded topology capture `GPU-TOPOLOGY-DIAGNOSTIC-0010.TXT` puts the
3080 at `04:00.0` behind this enumerated path:

```
00:02.1 -> 02:00.0 -> 03:00.0 -> 04:00.0 (RTX 3080)
```

The run's PCI records identify `00:02.1` as an AMD root-port function, followed
by AMD `1022:43f4` and `1022:43f5` bridge functions. This is distinct from the
4090 at `01:00.0`, directly below `00:01.1`. RUN0009 reports ASPM `0` for the
4090 but ASPM `2` for the 3080; the latter's link is Gen4 x4 against a reported
maximum x16. The BIOS option photographed as `ASPM Control for CPU PCIe` is
scoped to CPU PCIe, so it is not evidence of control over this chipset path.

**Recommendation:** do not try a speculative Link Control write or change the
CPU-side BIOS option for the 3080. The attachments establish topology and an
ASPM mismatch, not which chipset bridge's Link Control is authoritative, a
safe register-write policy, or a verified PCH ASPM setup control. Keep the next
active scope single-target only and defer an active 3080 attempt until a
read-only capture documents the applicable chipset-link capability/control and
its current state. This is a scope recommendation, not a claim that a
4090-only actuation is cleared by the other outstanding safety gates.

## Screen heartbeat

The silent-screen exception now advances a visible `.` / `..` / `...` cycle at
the same accumulated-delay interval as the `vendor-wait` log lines. Long asset
reads also show byte progress as `asset-read-bytes N of TOTAL` on the screen,
and each completed asset shows `asset-read N of 5`. A wait that returns to the
phase loop therefore no longer leaves only a static stage label. This cannot
animate inside a firmware call that never returns control; the line before a
blocking call and the bounded watchdog remain the evidence for that case.
