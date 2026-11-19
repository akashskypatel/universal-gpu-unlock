/* Modified 2026-10-19/20 for Windows GPU UNLOCK; changes documented in CHANGES.md. */ #pragma once

#include "port.h"

typedef struct {
  UINT16 pm, pcie, msi, msix;
  UINT8 pcie_version;
} GU_CAPS;

/* Config is a complete, CPU-owned 256-byte snapshot. Out is cleared on failure. */
BOOLEAN GuCheckConfig(const UINT32 Config[64], UINTN Segment, UINTN Bus,
                      UINTN Device, UINTN Function, GU_CAPS *Out);
/* Read-only selected-target cold observation. All other config checks remain. */
BOOLEAN GuCheckConfigInitialMaster(const UINT32 Config[64], UINTN Segment,
                                    UINTN Bus, UINTN Device, UINTN Function,
                                    GU_CAPS *Out);

/* C8: what the PCIe link itself says before anything is actuated.
 *
 * The admission policy above reads the capability chain; it never read the
 * link. A card can be a supported display function, in D0, with MSI off, and
 * still be attached over a link that is not trained, that firmware left in an
 * ASPM state this image does not manage, or that has already logged a PCIe
 * error. Those are the conditions an unlock must not start on.
 *
 * The extended capability space (where AER lives) starts at 0x100 and is not
 * part of the 256-byte snapshot, so AER is reported as unobservable rather
 * than guessed. Nothing here inspects a bus trace. */
typedef struct {
  UINT8 width, max_width;      /* negotiated and advertised link width */
  UINT8 speed, max_speed;      /* negotiated and advertised link speed */
  UINT8 aspm;                  /* Link Control ASPM request: 0 = disabled */
  UINT8 trained, up;           /* Link Training clear, DLL Link Active set */
  UINT8 errors;                /* Device Status CED/NFER/FER/URD, ORed */
  UINT8 hotplug, surprise;     /* Slot Capabilities bits 6 and 5 */
} GU_LINK;

typedef enum {
  GU_LINK_OK = 0,
  GU_LINK_ABSENT,       /* no usable PCIe capability offset was supplied */
  GU_LINK_ERROR_LOGGED, /* the card already recorded a PCIe error */
  GU_LINK_ASPM_ENABLED, /* firmware asked for L0s/L1 on a link we do not manage */
  GU_LINK_NOT_TRAINED,
  GU_LINK_DOWN,
  GU_LINK_UNUSABLE      /* trained and active, but no width or no speed */
} GU_LINK_VERDICT;

/* Out is cleared first, then filled as far as the snapshot allows. */
GU_LINK_VERDICT GuCheckLinkState(const UINT32 Config[64], UINT16 Pcie, GU_LINK *Out);
BOOLEAN GuLinkAccepted(GU_LINK_VERDICT Verdict);
/* A stable token for logs; never NULL. */
const char *GuLinkReason(GU_LINK_VERDICT Verdict);

/* Path must point to readable memory through its terminator, up to MaxBytes.
   MaxBytes is a read bound, not necessarily the exact allocation length.
   Returns 1 for the exact captured AMD GOP route/ADR, 2 for RTX, 0 otherwise.
   Route alone does not establish PCI identity: validate the GOP PCI parent. */
