/* SPDX-License-Identifier: GPL-2.0-only */
/* Host simulator world for the active transaction sources.
 *
 * The simulator is the "machine" the real image runs against: a UEFI-like
 * service table, a modelled PCI device with a 16 MiB BAR0 and a 1 MiB PROM
 * window, a directory-backed file system on two volumes, a console, and a
 * stock-driver binding that can be disconnected.
 *
 * Everything here is modelled state, not hardware. A simulator run is
 * evidence about code behaviour on the modelled device and nothing more.
 */
#pragma once

#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>

#include <stdint.h>
#include <stddef.h>

#define SIM_MAX_HANDLES     32
#define SIM_MAX_PROTOCOLS   8
#define SIM_BAR_BYTES       0x1000000u   /* 16 MiB BAR0 */
#define SIM_PROM_OFFSET     0x300000u    /* NV_PROM_DATA window inside BAR0 */
#define SIM_PROM_BYTES      0x100000u    /* 1 MiB readout */
#define SIM_CONFIG_BYTES    4096u
#define SIM_MAX_VOLUMES     4
#define SIM_MAX_OPEN_RECORDS 16
#define SIM_DMA_WINDOW      (512u << 20)   /* one GSP image fits */

typedef struct SIM_GPU SIM_GPU;

/* A directory-backed volume with a synthetic GPT device path. */
typedef struct {
  EFI_HANDLE                     handle;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL fs;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs_ptr;
  EFI_DEVICE_PATH_PROTOCOL       *path;
  UINT8                         *path_bytes;
  UINTN                          path_length;
  const char                    *host_root;   /* read/write directory */
  int                            writable;
  int                            is_usb;
} SIM_VOLUME;

typedef struct {
  EFI_GUID  guid;
  void     *iface;
} SIM_PROTOCOL;

typedef struct {
  EFI_HANDLE    handle;
  SIM_PROTOCOL  protocols[SIM_MAX_PROTOCOLS];
  unsigned      count;
  const char   *name;
} SIM_HANDLE;

/* One published open-protocol relationship (the UEFI driver model graph). */
typedef struct {
  EFI_HANDLE handle;
  EFI_GUID   guid;
  EFI_HANDLE agent;
  EFI_HANDLE controller;
  UINT32     attributes;
  UINT32     open_count;
} SIM_OPEN_RECORD;

/* Simulator statistics, used by the assertions and the report. */
typedef struct {
  unsigned allocations, frees, pool_failures;
  unsigned disconnect_calls, connect_calls;
  unsigned map_calls, unmap_calls, allocate_buffer_calls;
  unsigned locate_calls, handle_protocol_calls;
  unsigned stall_calls;
  UINT64   stalled_us;
  unsigned reset_calls;
  unsigned load_image_calls;
} SIM_STATS;

/* World lifecycle. */
EFI_STATUS SimWorldInit(const char *seed_dir, const char *esp_dir,
                        const char *windows_dir);
void       SimWorldShutdown(void);
EFI_SYSTEM_TABLE *SimSystemTable(void);
EFI_HANDLE        SimImageHandle(void);
SIM_GPU          *SimPrimaryGpu(void);
SIM_VOLUME       *SimEspVolume(void);
SIM_VOLUME       *SimWindowsVolume(void);
SIM_STATS        *SimStats(void);

/* Register a bare handle (used by the GPU model). */
EFI_HANDLE SimRegisterGpuHandle(const char *name);

/* GPU model construction happens in sim_gpu.c. */
EFI_STATUS SimGpuBuild(SIM_GPU **out);
void       SimGpuFree(SIM_GPU *gpu);

/* Register/BAR accessors for assertions (never used by the image). */
UINT32 SimGpuBarRead(SIM_GPU *gpu, UINT32 offset);
void   SimGpuBarWrite(SIM_GPU *gpu, UINT32 offset, UINT32 value);
const UINT8 *SimGpuProm(SIM_GPU *gpu);
UINTN  SimGpuFlrCount(SIM_GPU *gpu);
UINTN  SimGpuBarWrites(SIM_GPU *gpu);
UINTN  SimGpuPromWrites(SIM_GPU *gpu);
void   SimGpuStockDriverDisconnected(SIM_GPU *gpu);
EFI_HANDLE SimGpuHandle(SIM_GPU *gpu);
EFI_PCI_IO_PROTOCOL *SimGpuIo(SIM_GPU *gpu);
UINT32 SimGpuConfigRead(SIM_GPU *gpu, UINT32 offset);
int    SimGpuStockDriverBound(SIM_GPU *gpu);

/* Ordered operation trace: what the image did, in the order it did it. */
enum {
  SIM_TRACE_BAR_WRITE = 1,
  SIM_TRACE_PROM_WRITE,
  SIM_TRACE_CONFIG_WRITE,
  SIM_TRACE_FLR,
  SIM_TRACE_MAP,
};
void   SimTraceReset(void);
size_t SimTraceCount(void);
int    SimTraceOverflowed(void);
int    SimTraceKind(size_t index);
UINT32 SimTraceOffset(size_t index);
UINT32 SimTraceValue(size_t index);
size_t SimTraceCountKind(int kind);
long   SimTraceFirstOf(int kind);
long   SimTraceLastOf(int kind);

/* BAR0 snapshot helpers for pre/post comparison. */
void SimGpuSnapshot(SIM_GPU *gpu, UINT32 *out, size_t dwords);
size_t SimGpuSnapshotDwords(void);
size_t SimGpuDiff(const UINT32 *before, const UINT32 *after,
                  UINT32 *offsets, UINT32 *values, size_t limit);

/* Console/log capture. */
void SimSay(const char *text);
const char *SimLogText(void);
size_t SimLogLength(void);
int SimLogContains(const char *needle);
UINTN SimLogCount(const char *needle);
void SimConsoleKey(CHAR16 key);

/* Scenario reporting. */
void SimReportLine(const char *format, ...);
int  SimFailures(void);
void SimFail(const char *format, ...);
