/* Modified 2026-10-19/20 for Windows GPU UNLOCK; changes documented in CHANGES.md. */ #include "checks.h"
#include <IndustryStandard/Pci.h>

#define GU_CONFIG_BYTES 256U
#define GU_CAP_START 0x40U
#define GU_CAP_LAST 0xfcU
#define GU_CAP_LIMIT ((GU_CONFIG_BYTES - GU_CAP_START) / 4U)
#define GU_MSIX_ID 0x11U

static UINT8 ConfigByte(const UINT32 Config[64], UINTN Offset) {
  return (UINT8)(Config[Offset / 4U] >> ((Offset & 3U) * 8U));
}

static UINT16 ConfigWord(const UINT32 Config[64], UINTN Offset) {
  return (UINT16)(ConfigByte(Config, Offset) |
                  ((UINT16)ConfigByte(Config, Offset + 1U) << 8U));
}

static BOOLEAN GuCheckConfigPolicy(const UINT32 Config[64], UINTN Segment, UINTN Bus,
                                   UINTN Device, UINTN Function, GU_CAPS *Out,
                                   BOOLEAN allow_initial_master) {
  GU_CAPS Caps = {0, 0, 0, 0, 0};
  UINT8 Occupied[GU_CONFIG_BYTES - GU_CAP_START] = {0};
  UINT64 Seen = 0;
  UINTN Pointer, Steps = 0;
  UINT16 Command;

  if (Out == NULL) return FALSE;
  *Out = Caps;
  if (Config == NULL || Segment > 0xffffU || Bus > 255 || Device > 31 || Function > 7)
    return FALSE;
  if (!GuIsSupportedPciId(Config[0]) ||
      ConfigByte(Config, 0x0b) != PCI_CLASS_DISPLAY ||
      (ConfigByte(Config, PCI_HEADER_TYPE_OFFSET) & 0x7fU) != HEADER_TYPE_DEVICE)
    return FALSE;

  Command = ConfigWord(Config, PCI_COMMAND_OFFSET);
  if ((Command & EFI_PCI_COMMAND_MEMORY_SPACE) == 0 ||
      (!allow_initial_master && (Command & EFI_PCI_COMMAND_BUS_MASTER) != 0) ||
      (Config[PCI_EXPANSION_ROM_BASE / 4] & 1U) != 0 ||
      (ConfigWord(Config, PCI_PRIMARY_STATUS_OFFSET) & EFI_PCI_STATUS_CAPABILITY) == 0)
    return FALSE;

  Pointer = ConfigByte(Config, PCI_CAPABILITY_POINTER_OFFSET);
  while (Pointer != 0) {
    UINTN Length, I;
    UINT16 Control;
    UINT8 Id, Next;
    UINT64 Bit;
    if (++Steps > GU_CAP_LIMIT || Pointer < GU_CAP_START || Pointer > GU_CAP_LAST ||
        (Pointer & 3U) != 0)
      return FALSE;
    Bit = (UINT64)1U << (Pointer / 4U);
    if ((Seen & Bit) != 0) return FALSE;
    Seen |= Bit;

    Id = ConfigByte(Config, Pointer);
    Next = ConfigByte(Config, Pointer + 1U);
    Control = ConfigWord(Config, Pointer + 2U);
    switch (Id) {
      case EFI_PCI_CAPABILITY_ID_PMI:
        if (Caps.pm != 0 || (Control & 7U) == 0 || (Control & 7U) > 3)
          return FALSE;
        Length = 8;
        break;
      case EFI_PCI_CAPABILITY_ID_MSI:
        if (Caps.msi != 0 || (Control & 1U) != 0) return FALSE;
        /* Mask-capable layouts include two reserved bytes after Message Data. */
        Length = (Control & 0x80U) != 0 ? 14U : 10U;
        if ((Control & 0x100U) != 0) Length += 10U;
        break;
      case PCI_EXPRESS_CAPABILITY_ID:
        if (Caps.pcie != 0 || (Control & 0x0fU) < 1 ||
            (Control & 0x0fU) > 2 || ((Control >> 4U) & 0x0fU) > 1)
          return FALSE;
        /* Endpoint v1 reaches Link Status; reserve the entire v2 layout. */
        Length = (Control & 0x0fU) == 1 ? 0x14U : 0x3cU;
        break;
      case GU_MSIX_ID:
        if (Caps.msix != 0 || (Control & 0x8000U) != 0) return FALSE;
        Length = 12;
        break;
      case EFI_PCI_CAPABILITY_ID_VENDOR:
        Length = Control & 0xffU;
        if (Length < 3) return FALSE;
        break;
      default:
        /* A format we do not understand cannot be checked for overlap/length. */
        return FALSE;
    }

    if (Length > GU_CONFIG_BYTES - Pointer) return FALSE;
    for (I = Pointer; I < Pointer + Length; ++I) {
      if (Occupied[I - GU_CAP_START] != 0) return FALSE;
      Occupied[I - GU_CAP_START] = 1;
    }

    switch (Id) {
      case EFI_PCI_CAPABILITY_ID_PMI:
        if ((ConfigWord(Config, Pointer + 4U) & 3U) != PCI_POWER_STATE_D0)
          return FALSE;
        Caps.pm = (UINT16)Pointer;
        break;
      case EFI_PCI_CAPABILITY_ID_MSI:
        Caps.msi = (UINT16)Pointer;
        break;
      case PCI_EXPRESS_CAPABILITY_ID:
        if ((Config[(Pointer + 4U) / 4U] & (1U << 28U)) == 0 ||
            (ConfigWord(Config, Pointer + 8U) & 0x8000U) != 0)
          return FALSE;
        Caps.pcie = (UINT16)Pointer;
        Caps.pcie_version = (UINT8)(Control & 0x0fU);
        break;
      case GU_MSIX_ID:
        Caps.msix = (UINT16)Pointer;
        break;
      default:
        break;
    }
    Pointer = Next;
  }
  if (Caps.pm == 0 || Caps.pcie == 0) return FALSE;
  *Out = Caps;
  return TRUE;
}

/* Retain the original cold-state policy everywhere by default. */
BOOLEAN GuCheckConfig(const UINT32 Config[64], UINTN Segment, UINTN Bus,
                      UINTN Device, UINTN Function, GU_CAPS *Out) {
  return GuCheckConfigPolicy(Config, Segment, Bus, Device, Function, Out, FALSE);
}
/* A selected card may have bus mastering enabled while its original UEFI
 * driver owns the PCI controller. Only the pre-release, read-only inspection
 * may accept that state. Releasing the driver must precede any normalization. */
BOOLEAN GuCheckConfigInitialMaster(const UINT32 Config[64], UINTN Segment,
                                    UINTN Bus, UINTN Device, UINTN Function,
                                    GU_CAPS *Out) {
  return GuCheckConfigPolicy(Config, Segment, Bus, Device, Function, Out, TRUE);
}

/* C8: the link, observed from the same 256-byte snapshot.
 *
 * Extended capabilities begin at 0x100, so AER presence cannot be read here;
 * the caller logs it as unobserved rather than assuming a value. The verdicts
 * are ordered so the first hazard found is the one reported. */
GU_LINK_VERDICT GuCheckLinkState(const UINT32 Config[64], UINT16 Pcie, GU_LINK *Out) {
  GU_LINK Link = {0};
  UINTN Offset;
  UINT16 DeviceStatus, LinkControl, LinkStatus;
  UINT32 LinkCaps, SlotCaps;

  if (Out == NULL) return GU_LINK_ABSENT;
  *Out = Link;
  if (Config == NULL || Pcie < GU_CAP_START || (Pcie & 3U) != 0 ||
      (UINTN)Pcie + 0x18U > GU_CONFIG_BYTES)
    return GU_LINK_ABSENT;
  Offset = Pcie;

  DeviceStatus = ConfigWord(Config, Offset + 0x0aU);
  LinkControl = ConfigWord(Config, Offset + 0x10U);
  LinkStatus = ConfigWord(Config, Offset + 0x12U);
  LinkCaps = Config[(Offset + 0x0cU) / 4U];
  SlotCaps = Config[(Offset + 0x14U) / 4U];

  /* Device Status bits 3..0 are URD, FER, NFER and CED. Only the two that mean
   * an *uncorrected* error refuse: a fatal or non-fatal error is an event the
   * link did not recover from, and an unlock must not start on one.
   *
   * The other two are not refusals, and the bench settled that rather than an
   * argument. RUN0008 and RUN0009 are two boots of the preparation image on this
   * machine, the second after a full AC removal with the CPU-PCIe ASPM request
   * disabled: on both boots, both cards, before any operating system ran, Device
   * Status read 0x9 - correctable error and unsupported request - with a x16 gen4
   * link trained. Firmware enumeration generates them, a correctable error is
   * corrected by definition, and the sticky bits can only be cleared by writing
   * Device Status (a config write this path does not make) or by a Fundamental
   * Reset that had just been performed. Refusing them refused every card on this
   * bench on every boot. */
  Link.errors = (UINT8)(DeviceStatus & 0x0fU);
  Link.trained = (UINT8)((LinkStatus & (1U << 11U)) == 0);
  Link.up = (UINT8)((LinkStatus & (1U << 13U)) != 0);
  Link.speed = (UINT8)(LinkStatus & 0x0fU);
  Link.width = (UINT8)((LinkStatus >> 4U) & 0x3fU);
  Link.aspm = (UINT8)(LinkControl & 3U);
  Link.max_speed = (UINT8)(LinkCaps & 0x0fU);
  Link.max_width = (UINT8)((LinkCaps >> 4U) & 0x3fU);
  Link.hotplug = (UINT8)((SlotCaps >> 6U) & 1U);
  Link.surprise = (UINT8)((SlotCaps >> 5U) & 1U);
  *Out = Link;

  if ((Link.errors & 0x06U) != 0) return GU_LINK_ERROR_LOGGED;
  /* The experimental ASPM waiver is not a link-health waiver: classify
   * untrained and unusable links first, even when ASPM is enabled. */
  if (!Link.trained) return GU_LINK_NOT_TRAINED;
  /* Link Status DLLLA is reported, not weighed. It reads 0 on this bench on a
   * link that is trained, negotiating x16 gen4, and answering these very config
   * reads - one boot even observed the ASPM request change the firmware had just
   * made, which is only possible from a live snapshot. A zero here therefore does
   * not describe a link that is down, and refusing on it refused every card on
   * this machine (RUN0007 to RUN0009). GU_LINK_DOWN stays in the vocabulary so
   * logs and receipts from earlier builds still decode. */
  if (Link.width == 0 || Link.speed == 0 || Link.speed == 0x0fU) return GU_LINK_UNUSABLE;
  if (Link.aspm != 0) return GU_LINK_ASPM_ENABLED;
  return GU_LINK_OK;
}

BOOLEAN GuLinkAccepted(GU_LINK_VERDICT Verdict) {
#if defined(GU_UNVERIFIED_BENCH_CONDITIONS) || defined(GU_STOP_RELEASE_ONLY_BENCH) || defined(GU_FIRMWARE_REATTACH)
  /* Narrow, image-local bench waiver: ASPM is still observed and reported,
   * but these explicitly waived experiments do not refuse solely on ASPM state. */
  return Verdict == GU_LINK_OK || Verdict == GU_LINK_ASPM_ENABLED;
#else
  return Verdict == GU_LINK_OK;
#endif
}

const char *GuLinkReason(GU_LINK_VERDICT Verdict) {
  switch (Verdict) {
    case GU_LINK_OK: return "OK";
    case GU_LINK_ABSENT: return "PCIE_CAPABILITY_ABSENT";
    case GU_LINK_ERROR_LOGGED: return "PCIE_ERROR_LOGGED";
    case GU_LINK_ASPM_ENABLED: return "ASPM_ENABLED";
    case GU_LINK_NOT_TRAINED: return "LINK_NOT_TRAINED";
    case GU_LINK_DOWN: return "LINK_DOWN";
    case GU_LINK_UNUSABLE: return "LINK_UNUSABLE";
    default: return "LINK_STATE_UNKNOWN";
  }
}
