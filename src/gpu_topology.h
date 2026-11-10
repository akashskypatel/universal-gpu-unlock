/* SPDX-License-Identifier: GPL-2.0-only */
/* Pure read-only PCI/GOP topology assessment: never authorizes actuation. */
#pragma once
#include "gpu_inventory.h"

#define GU_TOPO_MAX_PCI 128U
#define GU_TOPO_MAX_HANDLES 4096U
#define GU_TOPO_MAX_GOPS 32U
#define GU_TOPO_MAX_CONSOLES 8U
#define GU_TOPO_PATH_BYTES 192U

typedef struct {
  uint8_t bytes[GU_TOPO_PATH_BYTES];
  size_t length; /* Includes the four-byte END_ENTIRE node. */
} GU_TOPO_PATH;

typedef struct {
  GU_GPU_RECORD identity;
  GU_TOPO_PATH path;
  uint8_t root_port;  /* PCIe capability Port Type == 4; not inferred from name. */
  uint8_t bridge;     /* Class 06:04 */
  uint8_t secondary, subordinate; /* PCI bridge bus-number window. */
  unsigned by_driver; /* OpenProtocolInformation BY_DRIVER observers, if available. */
  void *first_owner_agent; /* Handle token only, NOT a verified driver identity. */
  uint8_t openers_known;
} GU_TOPO_PCI;

typedef struct {
  GU_TOPO_PATH path;
  /* Boot-local read-only association, never a vendor Stop capability. */
  void *handle;
  void *protocol;
} GU_TOPO_GOP;

typedef struct {
  size_t pci_count, gop_count, console_count;
  GU_TOPO_PCI pci[GU_TOPO_MAX_PCI];
  GU_TOPO_GOP gop[GU_TOPO_MAX_GOPS];
  GU_TOPO_PATH console[GU_TOPO_MAX_CONSOLES];
  uint8_t console_available;
} GU_TOPO_CAPTURE;

/* Arbitrary 1..N NVIDIA endpoints; AMD/Intel/other display controllers are
 * independent console candidates. Topology completeness is observational,
 * never an authorization to disconnect or write hardware. */
typedef struct {
  size_t gpu_count;
  size_t gpu_index[GU_INVENTORY_MAX_NVIDIA];
  size_t parent_index[GU_INVENTORY_MAX_NVIDIA];
  size_t root_index[GU_INVENTORY_MAX_NVIDIA];
  unsigned depth[GU_INVENTORY_MAX_NVIDIA];
  unsigned gop_count[GU_INVENTORY_MAX_NVIDIA];
  uint8_t ancestry_verified[GU_INVENTORY_MAX_NVIDIA];
  uint8_t root_port_verified[GU_INVENTORY_MAX_NVIDIA];
  unsigned nvidia_console_count;
  unsigned independent_gop_count, independent_console_count, unmatched_gops;
  unsigned independent_console_without_gop;
  unsigned unmatched_console_count, ambiguous_console_count;
  uint8_t independent_console_observed, gop_relationships_observed;
  int status;
} GU_TOPO_REPORT;

enum { GU_TOPO_COMPLETE=0, GU_TOPO_INCOMPLETE=1, GU_TOPO_CONFLICT=2, GU_TOPO_INVALID=3 };

/* Observational device-path ownership only; never qualifies vendor Stop. */
typedef struct {
  unsigned nvidia, independent, unmatched, ambiguous;
  unsigned independent_without_gop;
} GU_TOPO_CONSOLE_OWNERS;

/* Combine graph/ConOut with separately classified ErrOut.
 * Unknown/NVIDIA-owned or independently owned without same-PCI GOP is never
 * COMPLETE. This cannot prove proprietary vendor Stop or recovery. */
int GuTopoCombinedConsoleVerdict(int graph_status, int errout_classified,
                                 const GU_TOPO_CONSOLE_OWNERS *errout);

/* Reject malformed/overlong or multi-instance paths. PCI paths must be
 * unique; any unknown/ambiguous fact is explicitly incomplete. */
/* A topology graph needs display endpoints and PCI-to-PCI bridges, not
 * unrelated host, audio, network or USB functions. All protocol handles are
 * still visited; excluded classes never participate in GuTopoAssess. */
int GuTopoClassRelevant(uint16_t class_code);

/* Compare the only initial header words used to classify a handle: PCI
 * vendor/device identity and class/revision. Command/status is deliberately
 * not a classification field. Selected topology devices still get two full
 * 256-byte reads and a strict bytewise comparison. */
int GuTopoClassifierStable(const uint32_t first[3], const uint32_t second[3]);
int GuTopoPathValid(const GU_TOPO_PATH *p);
/* Compare bounded configured-console path captures; no atomicity claim. */
int GuTopoConsolePathsEqual(const GU_TOPO_PATH *first, size_t first_count,
                             const GU_TOPO_PATH *second, size_t second_count);
/* Compare complete GOP census by handle/protocol/path, order independent. */
int GuTopoGopSnapshotsEqual(const GU_TOPO_GOP *before, size_t before_count,
                            const GU_TOPO_GOP *after, size_t after_count);
/* Complete PCI device-path prefix only; no root-port-only ownership inference. */
int GuTopoPathOnController(const GU_TOPO_PATH *controller,
                           const GU_TOPO_PATH *candidate);
int GuTopoAssess(const GU_TOPO_CAPTURE *capture, GU_TOPO_REPORT *report);
/* Classify ConOut/ErrOut paths against a structurally valid, already
 * captured PCI/GOP/console graph; refuse INVALID topology and clear output on
 * every failure. INCOMPLETE/CONFLICT remain observational categories only.
 * Returns 0 on invalid input, never an actuation verdict. */
int GuTopoConsoleOwners(const GU_TOPO_CAPTURE *capture, const GU_TOPO_PATH *paths,
                        size_t count, GU_TOPO_CONSOLE_OWNERS *owners);
