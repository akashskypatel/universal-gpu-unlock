/* SPDX-License-Identifier: GPL-2.0-only */
/* Host simulator: modelled PCI function for an AD102 board.
 *
 * The model answers the register and configuration-space reads the real
 * sources make, and records every write so the harness can compare pre- and
 * post-execution state. Register values are a plausible AD102 cold state; they
 * are not a capture of any specific card.
 */
#include "sim_world.h"

#include <IndustryStandard/Acpi10.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* AD102 / RTX 4090. */
#define SIM_VENDOR_ID      0x10deu
#define SIM_DEVICE_ID      0x2684u
#define SIM_SUBSYSTEM      0xa14010deu
#define SIM_CLASS          0x030000u

/* Capability layout inside the 256-byte config header. */
#define SIM_CAP_PM         0x40u
#define SIM_CAP_MSI        0x50u
#define SIM_CAP_PCIE       0x60u

/* Register state that the active path reads before it writes anything. */
#define SIM_REG_PMC          0x000000u
#define SIM_REG_WPR2_HI      0x1fa828u
#define SIM_REG_SS0          0x82381cu
#define SIM_REG_SS1          0x823820u
#define SIM_REG_FB_MB        0x1183a4u
#define SIM_REG_VGA_WORK     0x625f04u
#define SIM_REG_MMU_PLM      0x1fa7c8u
#define SIM_REG_MMU_LO       0x1fa82cu
#define SIM_REG_MMU_HI       0x1fa830u
#define SIM_REG_WPR_CFG      0x1fa7ccu
#define SIM_REG_WPR          0x1fa7c4u
#define SIM_REG_BOOT0        0x8a0148u

/* Falcon engines the transaction addresses (base + 0x100 = CPUCTL). */
#define SIM_FALCON_MAILBOX0  0x040u
#define SIM_FALCON_MAILBOX1  0x044u
#define SIM_FALCON_CPUCTL    0x100u
#define SIM_FALCON_HALT_BIT  (1u << 4)
#define SIM_FALCON_START_BIT (1u << 1)
#define SIM_FALCON_BOOTVEC   0x104u
#define SIM_FALCON_HWCFG2    0x0f4u
#define SIM_FALCON_ENGINE    0x3c0u
#define SIM_FALCON_DMA_SRC_LO 0x110u
#define SIM_FALCON_DMA_DST    0x114u
#define SIM_FALCON_DMA_STATUS 0x118u
#define SIM_FALCON_DMA_OFF    0x11cu
#define SIM_FALCON_DMA_SRC_HI 0x128u
#define SIM_FALCON_DMA_BLOCK  256u

#define SIM_SEC2_BASE        0x00840000u
#define SIM_SEC2_FALCON2     0x00841000u
#define SIM_GSP_BASE         0x00110000u
#define SIM_GSP_FALCON2      0x00111000u

/* Registers the firmware-visible state machine reports. */
#define SIM_REG_FEAT_PLM     0x00823804u
#define SIM_REG_READOUT1     0x00823818u
#define SIM_REG_FBPA         0x009a0148u
#define SIM_REG_GFW_PLM      0x00118128u
#define SIM_REG_GFW_CPUCTL   0x00110100u
#define SIM_REG_GFW_BOOT     0x00118234u

#define SIM_SEC2_MARKER      0x67b00001u   /* AD102 payload completion marker */
#define SIM_GFW_PLM_BOOTED   0x00008b8fu
#define SIM_GFW_BOOT_DONE    0x000003ffu
#define SIM_PLM_LOCKED       0xffffff8fu   /* entry permissions, before payload */

typedef struct {
  void  *host;
  UINT64 device;
  UINT64 bytes;
  int    used;
} SIM_MAPPING;

/* Register-level model of one falcon engine. It reproduces what the driver can
 * observe: the reset/halt/start bits, DMA progress, and the permission changes
 * the signed payload produces. It does not execute firmware. */
typedef struct {
  UINT32  base, falcon2;
  UINT32  cpuctl, status, boot_vector, engine, mailbox0, mailbox1;
  UINT64  imem_bytes, dmem_bytes;
  UINT8  *imem, *dmem;
  UINT32  dma_dest, dma_off;
  UINT32  dma_src_lo, dma_src_hi;
  UINT64  dma_src;
  int     booted;
  int     dma_unmapped;
} SIM_FALCON;

#define SIM_FALCON_IMEM_BYTES 0x00040000u
#define SIM_FALCON_DMEM_BYTES 0x00010000u

/* One ordered record of every state-changing operation on the model. The
 * post-execution checks read this to prove ordering, not just end state.
 * Counters and first/last ordinals are kept separately from the detail ring so
 * a long DMA stream cannot hide the ordering evidence. */
#define SIM_TRACE_CAPACITY 512
typedef struct {
  UINT8  kind;
  UINT32 offset;
  UINT32 value;
} SIM_TRACE_ENTRY;

static SIM_TRACE_ENTRY g_trace[SIM_TRACE_CAPACITY];
static size_t g_trace_count;                 /* events stored */
static size_t g_trace_total;                 /* events seen */
static size_t g_trace_kind_total[16];
static long   g_trace_kind_first[16];
static long   g_trace_kind_last[16];

/* Watched registers: the checks ask "when was this written", which a bounded
 * detail ring cannot answer once a DMA stream fills it. */
#define SIM_TRACE_WATCH_MAX 8
static UINT32 g_watch_offset[SIM_TRACE_WATCH_MAX];
static unsigned g_watch_count;
static long g_watch_first[SIM_TRACE_WATCH_MAX];
static long g_watch_last[SIM_TRACE_WATCH_MAX];
static size_t g_watch_writes[SIM_TRACE_WATCH_MAX];
static UINTN  g_wpr2_opens;

static void TracePush(int kind, UINT32 offset, UINT32 value) {
  long ordinal = (long)g_trace_total++;
  if (kind == SIM_TRACE_BAR_WRITE) {
    for (unsigned i = 0; i < g_watch_count; i++)
      if (g_watch_offset[i] == offset) {
        g_watch_writes[i]++;
        if (g_watch_first[i] < 0) g_watch_first[i] = ordinal;
        g_watch_last[i] = ordinal;
      }
  }
  if (kind > 0 && kind < 16) {
    g_trace_kind_total[kind]++;
    if (g_trace_kind_first[kind] < 0) g_trace_kind_first[kind] = ordinal;
    g_trace_kind_last[kind] = ordinal;
  }
  if (g_trace_count < SIM_TRACE_CAPACITY) {
    g_trace[g_trace_count].kind = (UINT8)kind;
    g_trace[g_trace_count].offset = offset;
    g_trace[g_trace_count].value = value;
    g_trace_count++;
  }
}

struct SIM_GPU {
  EFI_PCI_IO_PROTOCOL io;
  EFI_HANDLE          handle;
  UINTN               segment, bus, device, function;
  UINT8              *config;      /* SIM_CONFIG_BYTES */
  UINT8              *bar;         /* SIM_BAR_BYTES */
  UINT8              *prom;        /* SIM_PROM_BYTES, mirrored into the BAR */
  UINT64              attributes;
  /* One individually allocated record per Map: the returned handle must stay
   * valid while later maps are added. */
  SIM_MAPPING       **mappings;
  size_t              mapping_count, mapping_capacity;
  SIM_FALCON          falcons[2];
  UINTN               flr_count;
  UINTN               bar_reads, bar_writes;
  UINTN               prom_writes;
  UINTN               config_writes;
  int                 stock_driver_bound;
  UINT64              bar_base;
  UINT64              bar_size;        /* firmware-assigned window */
  int                 device_ready;   /* model switch: BAR accessible */
  int                 flr_unsupported; /* scenario: the card refuses FLR */
  int                 high_dma;        /* scenario: allocator ignores the ceiling */
  int                 flr_pending;
};

static void Wr32(UINT8 *base, UINT32 offset, UINT32 value) {
  base[offset + 0] = (UINT8)value;
  base[offset + 1] = (UINT8)(value >> 8);
  base[offset + 2] = (UINT8)(value >> 16);
  base[offset + 3] = (UINT8)(value >> 24);
}

static void Wr16(UINT8 *base, UINT32 offset, UINT16 value) {
  base[offset + 0] = (UINT8)value;
  base[offset + 1] = (UINT8)(value >> 8);
}

static UINT32 Rd32(const UINT8 *base, UINT32 offset) {
  return (UINT32)base[offset] | ((UINT32)base[offset + 1] << 8) |
         ((UINT32)base[offset + 2] << 16) | ((UINT32)base[offset + 3] << 24);
}

static UINT16 Rd16(const UINT8 *base, UINT32 offset) {
  return (UINT16)(base[offset] | (base[offset + 1] << 8));
}

/* ------------------------------------------------------------------ */
/* ROM image                                                           */
/* ------------------------------------------------------------------ */

/* A structurally valid 1 MiB NV_PROM_DATA readout carrying one FWSEC V3
 * descriptor and one signature record. Structure only: no signature is
 * authenticated and no payload is real firmware. */
static void BuildProm(UINT8 *rom) {
  const UINT32 pcir = 0x40;
  const UINT32 bit = 0x100;
  const UINT32 token_table = 0x200;
  const UINT32 table = 0x300;
  const UINT32 desc = 0x10000;
  const UINT32 image_bytes = 4096;
  const UINT32 desc_bytes = 44 + 384;

  memset(rom, 0, SIM_PROM_BYTES);

  /* Expansion header. */
  Wr16(rom, 0x00, 0xaa55);
  Wr16(rom, 0x18, (UINT16)pcir);
  memcpy(rom + pcir, "PCIR", 4);
  Wr16(rom, pcir + 0x04, SIM_VENDOR_ID);
  Wr16(rom, pcir + 0x06, SIM_DEVICE_ID);
  Wr16(rom, pcir + 0x10, (UINT16)(SIM_PROM_BYTES / 512));  /* image size */
  rom[pcir + 0x14] = 0x00;      /* x86 code type */
  rom[pcir + 0x15] = 0x80;      /* last image */

  /* BIT block: size, stride, tokens; the block checksum must be zero. */
  rom[bit + 0] = 0xff;
  rom[bit + 1] = 0xb8;
  memcpy(rom + bit + 2, "BIT", 3);
  rom[bit + 5] = 0x00;
  rom[bit + 8] = 32;            /* block size */
  rom[bit + 9] = 8;             /* token stride */
  rom[bit + 10] = 1;            /* token count */
  /* Token 0: type 0x02 entry list at token_table. */
  rom[bit + 32 + 0] = 0x70;
  rom[bit + 32 + 1] = 0x02;
  Wr16(rom, bit + 32 + 2, 8);
  Wr32(rom, bit + 32 + 4, token_table);
  {
    UINT8 sum = 0;
    for (UINT32 i = 0; i < 32; i++) sum = (UINT8)(sum + rom[bit + i]);
    rom[bit + 7] = (UINT8)(0u - sum);   /* inside the summed block */
  }
  Wr32(rom, token_table, table);

  /* Ucode table: version 1, header 8, stride 8, one entry. */
  rom[table + 0] = 1;
  rom[table + 1] = 8;
  rom[table + 2] = 8;
  rom[table + 3] = 1;
  rom[table + 8] = 0x85;
  Wr32(rom, table + 8 + 2, desc);

  /* FWSEC V3 descriptor: one 384-byte signature record, GSP engine mask. */
  Wr32(rom, desc + 0, (desc_bytes << 16) | (3u << 8) | 1u);
  Wr32(rom, desc + 4, image_bytes);
  Wr32(rom, desc + 8, 0x100);        /* PKC offset inside DMEM */
  Wr32(rom, desc + 12, 0x80);        /* interface table offset */
  Wr32(rom, desc + 16, 0);           /* IMEM physical */
  Wr32(rom, desc + 20, 0x800);       /* IMEM size == DMEM data offset */
  Wr32(rom, desc + 24, 0);           /* IMEM virtual */
  Wr32(rom, desc + 28, 0);           /* DMEM physical */
  Wr32(rom, desc + 32, 0x400);       /* DMEM size */
  Wr16(rom, desc + 36, 0x400);       /* GSP engine mask */
  rom[desc + 38] = 1;                /* ucode id */
  rom[desc + 39] = 1;                /* signature count */
  Wr16(rom, desc + 40, 0x0001);      /* signature version bits */
  /* Signature records stay zero: nothing is authenticated. */

  /* DMEM interface table and mapper command block. */
  {
    UINT8 *dmem = rom + desc + desc_bytes + 0x800;
    dmem[0x80] = 1;                  /* version */
    dmem[0x81] = 4;                  /* header */
    dmem[0x82] = 8;                  /* stride */
    dmem[0x83] = 2;                  /* entries */
    Wr32(dmem, 0x84, 0);             /* entry 0 id */
    Wr32(dmem, 0x88, 0);             /* entry 0 destination */
    Wr32(dmem, 0x8c, 4);             /* entry 1 id: mapper */
    Wr32(dmem, 0x90, 0xe0);          /* mapper offset */
    Wr16(dmem, 0xe0 + 4, 3);         /* mapper version */
    Wr16(dmem, 0xe0 + 6, 64);        /* mapper size */
    /* The command block sits past the 384-byte PKC slot the driver fills at
     * data_off + pkc_off, so the two never overlap in this image. */
    Wr32(dmem, 0xe0 + 8, 0x2c0);     /* command offset */
    Wr32(dmem, 0xe0 + 12, 64);       /* command size */
  }
}

/* ------------------------------------------------------------------ */
/* PCI I/O protocol                                                    */
/* ------------------------------------------------------------------ */

static UINTN WidthBytes(EFI_PCI_IO_PROTOCOL_WIDTH width) {
  switch (width) {
    case EfiPciIoWidthUint8:  return 1;
    case EfiPciIoWidthUint16: return 2;
    case EfiPciIoWidthUint32: return 4;
    case EfiPciIoWidthUint64: return 8;
    default:                  return 0;
  }
}

/* ------------------------------------------------------------------ */
/* Falcon engines                                                      */
/* ------------------------------------------------------------------ */

static SIM_FALCON *FalconLookup(SIM_GPU *gpu, UINT32 offset) {
  for (unsigned i = 0; i < 2; i++) {
    SIM_FALCON *f = &gpu->falcons[i];
    if (!f->base) continue;
    if (offset >= f->base && offset < f->base + 0x1000) return f;
  }
  return NULL;
}

/* A device address the model may read: it must fall inside a mapping the
 * driver registered through Map(). Anything else is refused rather than
 * dereferenced, so a bad descriptor cannot crash the harness. */
static int FalconHostRange(SIM_GPU *gpu, UINT64 address, UINT64 bytes,
                           const UINT8 **host) {
  for (size_t i = 0; i < gpu->mapping_count; i++) {
    SIM_MAPPING *m = gpu->mappings[i];
    if (!m->used || address < m->device) continue;
    if (address + bytes > m->device + m->bytes) continue;
    *host = (const UINT8 *)m->host + (address - m->device);
    return 1;
  }
  return 0;
}

/* The FWSEC image carries its FRTS target in a 44-byte command record inside
 * the data section the host DMA'd into DMEM. The engine reads that record, so
 * the model does the same instead of being told the address out of band. */
static UINT32 FalconFrtsPage(SIM_FALCON *f) {
  for (UINT32 at = 0; at + 44 <= SIM_FALCON_DMEM_BYTES; at += 4) {
    const UINT8 *p = f->dmem + at;
    if (Rd32((UINT8 *)p, 0) != 1 || Rd32((UINT8 *)p, 4) != 24) continue;
    if (Rd32((UINT8 *)p, 20) != 2 || Rd32((UINT8 *)p, 24) != 1) continue;
    if (Rd32((UINT8 *)p, 28) != 20 || Rd32((UINT8 *)p, 36) != 0x100) continue;
    if (Rd32((UINT8 *)p, 40) != 2) continue;
    UINT32 page = Rd32((UINT8 *)p, 32);
    if (page) return page;
  }
  return 0;
}

/* The signed payload changes what the card permits. Model that as one
 * transition, taken only when a whole image has been pushed through DMA and
 * the host then starts the CPU: PLM opens, the permission words open, and the
 * engine halts with the completion marker in mailbox 0. */
static void FalconStart(SIM_GPU *gpu, SIM_FALCON *f) {
  if (!f->imem_bytes || f->booted) return;
  f->booted = 1;
  f->cpuctl = SIM_FALCON_HALT_BIT;
  if (f->base == SIM_SEC2_BASE) {
    f->mailbox0 = SIM_SEC2_MARKER;
    Wr32(gpu->bar, SIM_REG_FEAT_PLM, 0xffffffffu);
    Wr32(gpu->bar, SIM_REG_WPR, 0xffffffffu);
    Wr32(gpu->bar, SIM_REG_WPR_CFG, 0xfffff0ffu);
    Wr32(gpu->bar, SIM_REG_FBPA, 0xffffffffu);
    TracePush(SIM_TRACE_FW_START, SIM_SEC2_BASE, SIM_SEC2_MARKER);
  } else {
    /* FWSEC owns the FRTS: it opens WPR2 over the requested page. */
    UINT32 page = FalconFrtsPage(f);
    if (page) {
      Wr32(gpu->bar, 0x001fa824u, page << 4);
      Wr32(gpu->bar, 0x001fa828u, (page + 0x40u) << 4);
      g_wpr2_opens++;
      TracePush(SIM_TRACE_WPR2, page << 4, (page + 0x40u) << 4);
    }
    Wr32(gpu->bar, SIM_REG_GFW_CPUCTL, SIM_FALCON_HALT_BIT);
    TracePush(SIM_TRACE_FW_START, f->base, 0);
  }
}

/* One DMA block: 256 bytes from host memory into IMEM or DMEM. */
static void FalconDma(SIM_GPU *gpu, SIM_FALCON *f, UINT32 command) {
  const UINT8 *host = NULL;
  int imem = (command & (1u << 2)) != 0;
  UINT32 window = imem ? f->dma_dest : f->dma_off;
  UINT8 *target = imem ? f->imem : f->dmem;
  UINT32 limit = imem ? SIM_FALCON_IMEM_BYTES : SIM_FALCON_DMEM_BYTES;
  if (!FalconHostRange(gpu, f->dma_src, SIM_FALCON_DMA_BLOCK, &host)) {
    f->dma_unmapped++;
  } else if (window <= limit && SIM_FALCON_DMA_BLOCK <= limit - window) {
    memcpy(target + window, host, SIM_FALCON_DMA_BLOCK);
  }
  if (imem) {
    f->imem_bytes += SIM_FALCON_DMA_BLOCK;
    f->dma_dest += SIM_FALCON_DMA_BLOCK;
  } else {
    f->dmem_bytes += SIM_FALCON_DMA_BLOCK;
    f->dma_off += SIM_FALCON_DMA_BLOCK;
  }
  f->dma_src += SIM_FALCON_DMA_BLOCK;
  /* Idle with the completion bit set: the driver polls busy=0 then done=1. */
  f->status = 2;
}

static int FalconRead(SIM_GPU *gpu, UINT32 offset, UINT32 *value) {
  SIM_FALCON *f = FalconLookup(gpu, offset);
  if (!f) return 0;
  UINT32 reg = offset - f->base;
  switch (reg) {
    case SIM_FALCON_CPUCTL: *value = f->cpuctl; return 1;
    case SIM_FALCON_MAILBOX0: *value = f->mailbox0; return 1;
    case SIM_FALCON_MAILBOX1: *value = f->mailbox1; return 1;
    case SIM_FALCON_BOOTVEC: *value = f->boot_vector; return 1;
    case SIM_FALCON_ENGINE: *value = f->engine; return 1;
    case SIM_FALCON_DMA_STATUS: *value = f->status; return 1;
    /* HWCFG2: idle (bit 31) and no memory scrub in progress (bit 12). */
    case SIM_FALCON_HWCFG2: *value = 0x80000000u; return 1;
    default: return 0;
  }
}

static int FalconWrite(SIM_GPU *gpu, UINT32 offset, UINT32 value) {
  SIM_FALCON *f = FalconLookup(gpu, offset);
  if (!f) return 0;
  UINT32 reg = offset - f->base;
  switch (reg) {
    case SIM_FALCON_CPUCTL:
      f->cpuctl = value;
      if (value & SIM_FALCON_START_BIT) FalconStart(gpu, f);
      return 1;
    case SIM_FALCON_MAILBOX0: f->mailbox0 = value; return 1;
    case SIM_FALCON_MAILBOX1: f->mailbox1 = value; return 1;
    case SIM_FALCON_BOOTVEC: f->boot_vector = value; return 1;
    case SIM_FALCON_ENGINE:
      f->engine = value;
      if (value & 1u) {
        /* Engine reset: the CPU is held halted, the payload's one-shot
         * transition is disarmed, and DMA state is dropped. */
        f->cpuctl = SIM_FALCON_HALT_BIT;
        f->status = 0;
        f->booted = 0;
      }
      return 1;
    case SIM_FALCON_DMA_SRC_LO:
      f->dma_src_lo = value;
      f->dma_src = ((UINT64)f->dma_src_lo | (UINT64)f->dma_src_hi << 32) << 8;
      return 1;
    case SIM_FALCON_DMA_SRC_HI:
      f->dma_src_hi = value & 0x1ff;
      f->dma_src = ((UINT64)f->dma_src_lo | (UINT64)f->dma_src_hi << 32) << 8;
      return 1;
    case SIM_FALCON_DMA_DST: f->dma_dest = value; return 1;
    case SIM_FALCON_DMA_OFF: f->dma_off = value; return 1;
    case SIM_FALCON_DMA_STATUS: FalconDma(gpu, f, value); return 1;
    default: return 0;
  }
}

static EFI_STATUS EFIAPI SimPciMemRead(EFI_PCI_IO_PROTOCOL *self,
                                       EFI_PCI_IO_PROTOCOL_WIDTH width,
                                       UINT8 bar, UINT64 offset, UINTN count,
                                       VOID *buffer) {
  SIM_GPU *gpu = (SIM_GPU *)((UINT8 *)self - offsetof(SIM_GPU, io));
  UINTN unit = WidthBytes(width);
  if (!buffer || bar || !unit) return EFI_INVALID_PARAMETER;
  if (offset > SIM_BAR_BYTES || count > (SIM_BAR_BYTES - offset) / unit)
    return EFI_INVALID_PARAMETER;
  if (unit == 4 && count == 1) {
    UINT32 computed = 0;
    if (FalconRead(gpu, (UINT32)offset, &computed)) *(UINT32 *)buffer = computed;
    else memcpy(buffer, gpu->bar + offset, 4);
    gpu->bar_reads++;
    return EFI_SUCCESS;
  }
  memcpy(buffer, gpu->bar + offset, count * unit);
  gpu->bar_reads++;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimPciMemWrite(EFI_PCI_IO_PROTOCOL *self,
                                        EFI_PCI_IO_PROTOCOL_WIDTH width,
                                        UINT8 bar, UINT64 offset, UINTN count,
                                        VOID *buffer) {
  SIM_GPU *gpu = (SIM_GPU *)((UINT8 *)self - offsetof(SIM_GPU, io));
  UINTN unit = WidthBytes(width);
  if (!buffer || bar || !unit) return EFI_INVALID_PARAMETER;
  if (offset > SIM_BAR_BYTES || count > (SIM_BAR_BYTES - offset) / unit)
    return EFI_INVALID_PARAMETER;
  if (unit == 4 && count == 1 && FalconWrite(gpu, (UINT32)offset,
                                             *(UINT32 *)buffer)) {
    Wr32(gpu->bar, (UINT32)offset, *(UINT32 *)buffer);
    gpu->bar_writes++;
    TracePush(SIM_TRACE_BAR_WRITE, (UINT32)offset, *(UINT32 *)buffer);
    return EFI_SUCCESS;
  }
  if (offset < SIM_PROM_OFFSET + SIM_PROM_BYTES &&
      offset + count * unit > SIM_PROM_OFFSET) {
    gpu->prom_writes++;
    TracePush(SIM_TRACE_PROM_WRITE, offset,
              count == 1 ? (UINT32)*(UINT8 *)buffer
                         : (count >= 4 ? Rd32((UINT8 *)buffer, 0)
                                       : (UINT32)*(UINT16 *)buffer));
  }
  memcpy(gpu->bar + offset, buffer, count * unit);
  gpu->bar_writes++;
  TracePush(SIM_TRACE_BAR_WRITE, offset,
            count == 1 ? (UINT32)*(UINT8 *)buffer
                       : (count >= 4 ? Rd32((UINT8 *)buffer, 0)
                                     : (UINT32)*(UINT16 *)buffer));
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimPciConfigRead(EFI_PCI_IO_PROTOCOL *self,
                                          EFI_PCI_IO_PROTOCOL_WIDTH width,
                                          UINT32 offset, UINTN count,
                                          VOID *buffer) {
  SIM_GPU *gpu = (SIM_GPU *)((UINT8 *)self - offsetof(SIM_GPU, io));
  UINTN unit = WidthBytes(width);
  if (!buffer || !unit) return EFI_INVALID_PARAMETER;
  if (offset > SIM_CONFIG_BYTES || count > (SIM_CONFIG_BYTES - offset) / unit)
    return EFI_INVALID_PARAMETER;
  memcpy(buffer, gpu->config + offset, count * unit);
  return EFI_SUCCESS;
}

/* Function-level reset: the engines stop and the firmware-owned windows
 * close. The permission latch (PLM) and the speed-select registers are kept
 * because the post-FLR contract in the driver requires them to survive; the
 * real ROM GFW re-runs and re-latches them on hardware. */
static void SimGpuFunctionReset(SIM_GPU *gpu) {
  Wr32(gpu->bar, 0x001fa824u, 0);
  Wr32(gpu->bar, 0x001fa828u, 0);
  for (unsigned i = 0; i < 2; i++) {
    SIM_FALCON *f = &gpu->falcons[i];
    f->cpuctl = SIM_FALCON_HALT_BIT;
    f->status = 0;
    f->booted = 0;
    f->imem_bytes = 0;
    f->dmem_bytes = 0;
    f->dma_src = 0;
    f->dma_src_lo = 0;
    f->dma_src_hi = 0;
    f->dma_dest = 0;
    f->dma_off = 0;
  }
  Wr32(gpu->bar, SIM_REG_GFW_CPUCTL, SIM_FALCON_HALT_BIT);
}

static EFI_STATUS EFIAPI SimPciConfigWrite(EFI_PCI_IO_PROTOCOL *self,
                                           EFI_PCI_IO_PROTOCOL_WIDTH width,
                                           UINT32 offset, UINTN count,
                                           VOID *buffer) {
  SIM_GPU *gpu = (SIM_GPU *)((UINT8 *)self - offsetof(SIM_GPU, io));
  UINTN unit = WidthBytes(width);
  if (!buffer || !unit) return EFI_INVALID_PARAMETER;
  if (offset > SIM_CONFIG_BYTES || count > (SIM_CONFIG_BYTES - offset) / unit)
    return EFI_INVALID_PARAMETER;
  memcpy(gpu->config + offset, buffer, count * unit);
  gpu->config_writes++;
  TracePush(SIM_TRACE_CONFIG_WRITE, offset,
            count >= 4 ? Rd32((UINT8 *)buffer, 0)
                       : (count == 2 ? (UINT32)*(UINT16 *)buffer
                                     : (UINT32)*(UINT8 *)buffer));
  /* Function-level reset: DEVCTL bit 15 (PCIe capability + 8). */
  if (offset <= SIM_CAP_PCIE + 8 && offset + count * unit >= SIM_CAP_PCIE + 10) {
    UINT16 devctl = Rd16(gpu->config, SIM_CAP_PCIE + 8);
    if ((devctl & 0x8000) && gpu->flr_unsupported) {
      /* The function is not reset: the write is dropped and the permission
       * window stays open, which is exactly what the driver must notice. */
      devctl &= (UINT16)~0x8000u;
      Wr16(gpu->config, SIM_CAP_PCIE + 8, devctl);
      TracePush(SIM_TRACE_FLR_REFUSED, SIM_CAP_PCIE + 8, 0);
    } else if (devctl & 0x8000) {
      gpu->flr_count++;
      SimGpuFunctionReset(gpu);
      TracePush(SIM_TRACE_FLR, SIM_CAP_PCIE + 8, 0x8000);
      gpu->flr_pending = 0;
      devctl &= (UINT16)~0x8000u;
      Wr16(gpu->config, SIM_CAP_PCIE + 8, devctl);
    }
  }
  /* Keep the modelled attribute set in step with the command register. */
  {
    UINT16 command = Rd16(gpu->config, 0x04);
    UINT64 mirrored = gpu->attributes &
                      ~(EFI_PCI_IO_ATTRIBUTE_IO | EFI_PCI_IO_ATTRIBUTE_MEMORY);
    if (command & 0x1) mirrored |= EFI_PCI_IO_ATTRIBUTE_IO;
    if (command & 0x2) mirrored |= EFI_PCI_IO_ATTRIBUTE_MEMORY;
    gpu->attributes = mirrored;
  }
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimPciGetLocation(EFI_PCI_IO_PROTOCOL *self,
                                           UINTN *segment, UINTN *bus,
                                           UINTN *device, UINTN *function) {
  SIM_GPU *gpu = (SIM_GPU *)((UINT8 *)self - offsetof(SIM_GPU, io));
  if (segment) *segment = gpu->segment;
  if (bus) *bus = gpu->bus;
  if (device) *device = gpu->device;
  if (function) *function = gpu->function;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimPciAttributes(EFI_PCI_IO_PROTOCOL *self,
                                          EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION operation,
                                          UINT64 attributes, UINT64 *result) {
  SIM_GPU *gpu = (SIM_GPU *)((UINT8 *)self - offsetof(SIM_GPU, io));
  const UINT64 supported = EFI_PCI_IO_ATTRIBUTE_IO | EFI_PCI_IO_ATTRIBUTE_MEMORY |
                           EFI_PCI_IO_ATTRIBUTE_BUS_MASTER |
                           EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE;
  switch (operation) {
    case EfiPciIoAttributeOperationGet:
      if (result) *result = gpu->attributes;
      return EFI_SUCCESS;
    case EfiPciIoAttributeOperationSupported:
      if (result) *result = supported;
      return EFI_SUCCESS;
    case EfiPciIoAttributeOperationSet:
    case EfiPciIoAttributeOperationEnable:
    case EfiPciIoAttributeOperationDisable: {
      if (attributes & ~supported) return EFI_UNSUPPORTED;
      if (operation == EfiPciIoAttributeOperationDisable)
        gpu->attributes &= ~attributes;
      else
        gpu->attributes |= attributes;
      UINT16 command = Rd16(gpu->config, 0x04);
      if (gpu->attributes & EFI_PCI_IO_ATTRIBUTE_IO) command |= 0x1;
      else command &= (UINT16)~0x1u;
      if (gpu->attributes & EFI_PCI_IO_ATTRIBUTE_MEMORY) command |= 0x2;
      else command &= (UINT16)~0x2u;
      if (gpu->attributes & EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) command |= 0x4;
      else command &= (UINT16)~0x4u;
      Wr16(gpu->config, 0x04, command);
      if (operation == EfiPciIoAttributeOperationSet) {
        gpu->attributes = attributes;
        UINT16 rewritten = 0;
        if (attributes & EFI_PCI_IO_ATTRIBUTE_IO) rewritten |= 0x1;
        if (attributes & EFI_PCI_IO_ATTRIBUTE_MEMORY) rewritten |= 0x2;
        if (attributes & EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) rewritten |= 0x4;
        Wr16(gpu->config, 0x04, rewritten);
      }
      if (result) *result = gpu->attributes;
      gpu->config_writes++;
      return EFI_SUCCESS;
    }
    default:
      return EFI_UNSUPPORTED;
  }
}

static EFI_STATUS EFIAPI SimPciMap(EFI_PCI_IO_PROTOCOL *self,
                                   EFI_PCI_IO_PROTOCOL_OPERATION operation,
                                   VOID *host, UINTN *bytes,
                                   EFI_PHYSICAL_ADDRESS *device,
                                   VOID **mapping) {
  SIM_GPU *gpu = (SIM_GPU *)((UINT8 *)self - offsetof(SIM_GPU, io));
  (void)operation;
  if (!host || !bytes || !device || !mapping) return EFI_INVALID_PARAMETER;
  if (*bytes > (UINTN)SIM_DMA_WINDOW) return EFI_OUT_OF_RESOURCES;
  TracePush(SIM_TRACE_MAP, 0, (UINT32)*bytes);
  SIM_MAPPING *slot = calloc(1, sizeof(*slot));
  if (!slot) return EFI_OUT_OF_RESOURCES;
  if (gpu->mapping_count == gpu->mapping_capacity) {
    size_t capacity = gpu->mapping_capacity ? gpu->mapping_capacity * 2 : 64;
    SIM_MAPPING **grown = realloc(gpu->mappings, capacity * sizeof(*grown));
    if (!grown) { free(slot); return EFI_OUT_OF_RESOURCES; }
    gpu->mappings = grown;
    gpu->mapping_capacity = capacity;
  }
  slot->host = host;
  slot->device = (UINT64)(UINTN)host;   /* identity mapping */
  slot->bytes = *bytes;
  slot->used = 1;
  gpu->mappings[gpu->mapping_count++] = slot;
  *device = (EFI_PHYSICAL_ADDRESS)(UINTN)host;
  *mapping = slot;
  SimStats()->map_calls++;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimPciUnmap(EFI_PCI_IO_PROTOCOL *self, VOID *mapping) {
  (void)self;
  SIM_MAPPING *entry = (SIM_MAPPING *)mapping;
  if (!entry || !entry->used) return EFI_INVALID_PARAMETER;
  entry->used = 0;
  SimStats()->unmap_calls++;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimPciAllocateBuffer(EFI_PCI_IO_PROTOCOL *self,
                                              EFI_ALLOCATE_TYPE type,
                                              EFI_MEMORY_TYPE memory_type,
                                              UINTN pages, VOID **host,
                                              UINT64 attributes) {
  (void)memory_type; (void)attributes;
  if (!host || !pages) return EFI_INVALID_PARAMETER;
  SIM_GPU *gpu = (SIM_GPU *)((UINT8 *)self - offsetof(SIM_GPU, io));
  size_t bytes = pages * 4096u;
  /* A faithful allocator stays inside the caller's ceiling. The high-dma
   * scenario models a firmware that ignores it: the image must refuse the
   * buffer rather than hand a 64-bit address to a 32-bit DMA register. */
  void *p = MAP_FAILED;
  if (gpu->high_dma) {
    p = mmap((void *)0x100000000ull, bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED)
      p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  } else {
    (void)type;                    /* both allocation kinds stay 32-bit */
    p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  }
  if (p == MAP_FAILED) { *host = NULL; return EFI_OUT_OF_RESOURCES; }
  memset(p, 0, bytes);
  *host = p;
  SimStats()->allocate_buffer_calls++;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimPciFreeBuffer(EFI_PCI_IO_PROTOCOL *self,
                                          UINTN pages, VOID *host) {
  (void)self;
  if (!host || !pages) return EFI_INVALID_PARAMETER;
  munmap(host, pages * 4096u);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimPciFlush(EFI_PCI_IO_PROTOCOL *self) {
  (void)self;
  return EFI_SUCCESS;
}

/* ACPI QWORD address-space descriptor for BAR0: the exact shape an EDK2 PCI
 * bus driver produces, so the image's Bar() cross-check is meaningful. */
static EFI_STATUS EFIAPI SimPciGetBarAttributes(EFI_PCI_IO_PROTOCOL *self,
                                                UINT8 bar, UINT64 *supports,
                                                VOID **resources) {
  SIM_GPU *gpu = (SIM_GPU *)((UINT8 *)self - offsetof(SIM_GPU, io));
  if (supports) *supports = EFI_PCI_IO_ATTRIBUTE_MEMORY;
  if (!resources) return EFI_INVALID_PARAMETER;
  *resources = NULL;
  if (bar != 0) return EFI_UNSUPPORTED;
  /* Fresh allocation per call: the caller owns and frees it. */
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *desc =
      (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *)calloc(1, sizeof(*desc) + 8);
  if (!desc) return EFI_OUT_OF_RESOURCES;
  desc->Desc = 0x8a;
  desc->Len = 43;
  desc->ResType = ACPI_ADDRESS_SPACE_TYPE_MEM;
  desc->GenFlag = 0;
  desc->SpecificFlag = 0;
  desc->AddrSpaceGranularity = 0;
  desc->AddrRangeMin = gpu->bar_base;
  desc->AddrRangeMax = gpu->bar_base + gpu->bar_size - 1;
  desc->AddrTranslationOffset = 0;
  desc->AddrLen = gpu->bar_size;
  /* End tag so a consumer that walks the list stops here. */
  ((UINT8 *)desc)[sizeof(*desc)] = 0x79;
  *resources = desc;
  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Model construction                                                  */
/* ------------------------------------------------------------------ */

EFI_STATUS SimGpuBuild(SIM_GPU **out) {
  if (!out) return EFI_INVALID_PARAMETER;
  *out = NULL;
  SIM_GPU *gpu = calloc(1, sizeof(*gpu));
  if (!gpu) return EFI_OUT_OF_RESOURCES;
  gpu->config = calloc(1, SIM_CONFIG_BYTES);
  gpu->bar = calloc(1, SIM_BAR_BYTES);
  gpu->prom = calloc(1, SIM_PROM_BYTES);
  if (!gpu->config || !gpu->bar || !gpu->prom) {
    free(gpu->config); free(gpu->bar); free(gpu->prom); free(gpu);
    return EFI_OUT_OF_RESOURCES;
  }
  gpu->bar_base = 0x80000000ull;
  gpu->bar_size = SIM_BAR_BYTES;
  gpu->segment = 0;
  gpu->bus = 1;
  gpu->device = 0;
  gpu->function = 0;
  gpu->stock_driver_bound = 1;

  /* Configuration space. */
  Wr16(gpu->config, 0x00, SIM_VENDOR_ID);
  Wr16(gpu->config, 0x02, SIM_DEVICE_ID);
  Wr16(gpu->config, 0x04, 0x0002);          /* memory decode, no master yet */
  Wr16(gpu->config, 0x06, 0x0010);          /* capability list present */
  gpu->config[0x08] = 0xa1;                 /* revision */
  gpu->config[0x09] = 0x00;                 /* programming interface */
  gpu->config[0x0a] = 0x00;                 /* VGA controller */
  gpu->config[0x0b] = 0x03;                 /* display class */
  gpu->config[0x0e] = 0x00;                 /* single-function device */
  Wr16(gpu->config, 0x2c, SIM_VENDOR_ID);   /* subsystem vendor */
  Wr16(gpu->config, 0x2e, (UINT16)(SIM_SUBSYSTEM >> 16));
  Wr32(gpu->config, 0x30, 0x00000000);      /* expansion ROM disabled */
  gpu->config[0x34] = (UINT8)SIM_CAP_PM;    /* capability list head */

  /* Power management capability. Version 3, D0, PMCSR at +4. */
  gpu->config[SIM_CAP_PM + 0] = 0x01;
  gpu->config[SIM_CAP_PM + 1] = (UINT8)SIM_CAP_MSI;
  Wr16(gpu->config, SIM_CAP_PM + 2, 0x0003);
  Wr16(gpu->config, SIM_CAP_PM + 4, 0x0000);

  /* MSI capability: disabled, 32-bit, no per-vector masking (length 10). */
  gpu->config[SIM_CAP_MSI + 0] = 0x05;
  gpu->config[SIM_CAP_MSI + 1] = (UINT8)SIM_CAP_PCIE;
  Wr16(gpu->config, SIM_CAP_MSI + 2, 0x0000);

  /* PCI Express capability, endpoint v1, FLR supported. The card sits in a
   * real slot, so the link is trained, active, Gen4 x16, with ASPM disabled -
   * the state the admission policy now requires before anything is actuated. */
  gpu->config[SIM_CAP_PCIE + 0] = 0x10;
  gpu->config[SIM_CAP_PCIE + 1] = 0x00;
  Wr16(gpu->config, SIM_CAP_PCIE + 2, 0x0001);
  Wr32(gpu->config, SIM_CAP_PCIE + 4, 0x10000000);   /* DEVCTL: FLR capable */
  Wr16(gpu->config, SIM_CAP_PCIE + 8, 0x0000);       /* DEVCTL reserved bits clear */
  Wr16(gpu->config, SIM_CAP_PCIE + 0x0a, 0x0000);    /* DEVSTA: no errors, nothing pending */
  Wr32(gpu->config, SIM_CAP_PCIE + 0x0c, 4u | (16u << 4));/* max: Gen4 x16 */
  Wr16(gpu->config, SIM_CAP_PCIE + 0x10, 0x0000);    /* ASPM: disabled */
  Wr16(gpu->config, SIM_CAP_PCIE + 0x12,
       (UINT16)(4u | (16u << 4) | (1u << 13)));      /* Gen4 x16, trained, up */
  Wr32(gpu->config, SIM_CAP_PCIE + 0x14, 0x00000000);/* slot: no hot-plug */
  {
    UINT32 bar_base = 0x80000000u;
    Wr32(gpu->config, 0x10, bar_base);      /* BAR0: 16 MiB, 64-bit */
    Wr32(gpu->config, 0x14, 0x00000000);
  }

  /* BAR0 default state. */
  BuildProm(gpu->prom);
  memcpy(gpu->bar + SIM_PROM_OFFSET, gpu->prom, SIM_PROM_BYTES);
  Wr32(gpu->bar, SIM_REG_PMC, 0x192000a1);           /* AD102 chip id */
  Wr32(gpu->bar, SIM_REG_WPR2_HI, 0x00000000);       /* WPR2 idle */
  Wr32(gpu->bar, SIM_REG_SS0, 0x00000000);
  Wr32(gpu->bar, SIM_REG_SS1, 0x00000000);
  Wr32(gpu->bar, SIM_REG_FB_MB, 24576);              /* 24 GiB framebuffer */
  Wr32(gpu->bar, SIM_REG_VGA_WORK, 0x00000000);
  Wr32(gpu->bar, SIM_REG_MMU_PLM, 0x00000001);
  Wr32(gpu->bar, SIM_REG_MMU_LO, 0x00000100);
  Wr32(gpu->bar, SIM_REG_MMU_HI, 0x00000600);        /* FB top lock */
  Wr32(gpu->bar, SIM_REG_WPR_CFG, 0xfffff0ff);
  Wr32(gpu->bar, SIM_REG_WPR, 0xffffffff);
  /* Cold card: permissions are locked and the ROM GFW has finished booting.
   * The entry PLM is the locked value; the signed payload raises it. */
  Wr32(gpu->bar, SIM_REG_BOOT0, 0xffffffff);
  Wr32(gpu->bar, SIM_REG_WPR, 0x00000fffu);
  Wr32(gpu->bar, SIM_REG_WPR_CFG, 0xfffff000u);
  Wr32(gpu->bar, SIM_REG_FBPA, 0xffffffffu);
  Wr32(gpu->bar, SIM_REG_FEAT_PLM, SIM_PLM_LOCKED);
  Wr32(gpu->bar, SIM_REG_READOUT1, 0x00000000u);
  Wr32(gpu->bar, SIM_REG_GFW_PLM, SIM_GFW_PLM_BOOTED);
  Wr32(gpu->bar, SIM_REG_GFW_BOOT, SIM_GFW_BOOT_DONE);
  {
    SIM_FALCON *sec2 = &gpu->falcons[0];
    SIM_FALCON *gsp = &gpu->falcons[1];
    sec2->base = SIM_SEC2_BASE; sec2->falcon2 = SIM_SEC2_FALCON2;
    sec2->cpuctl = SIM_FALCON_HALT_BIT;
    sec2->imem = calloc(1, SIM_FALCON_IMEM_BYTES);
    sec2->dmem = calloc(1, SIM_FALCON_DMEM_BYTES);
    gsp->base = SIM_GSP_BASE; gsp->falcon2 = SIM_GSP_FALCON2;
    gsp->cpuctl = SIM_FALCON_HALT_BIT;
    gsp->imem = calloc(1, SIM_FALCON_IMEM_BYTES);
    gsp->dmem = calloc(1, SIM_FALCON_DMEM_BYTES);
    if (!sec2->imem || !sec2->dmem || !gsp->imem || !gsp->dmem)
      return EFI_OUT_OF_RESOURCES;
  }

  gpu->io.Mem.Read = SimPciMemRead;
  gpu->io.Mem.Write = SimPciMemWrite;
  gpu->io.Pci.Read = SimPciConfigRead;
  gpu->io.Pci.Write = SimPciConfigWrite;
  gpu->io.GetLocation = SimPciGetLocation;
  gpu->io.Attributes = SimPciAttributes;
  gpu->io.Map = SimPciMap;
  gpu->io.Unmap = SimPciUnmap;
  gpu->io.AllocateBuffer = SimPciAllocateBuffer;
  gpu->io.FreeBuffer = SimPciFreeBuffer;
  gpu->io.Flush = SimPciFlush;
  gpu->io.GetBarAttributes = SimPciGetBarAttributes;
  gpu->io.RomSize = SIM_PROM_BYTES;
  gpu->io.RomImage = gpu->prom;

  SIM_HANDLE *handle = NULL;
  extern EFI_HANDLE SimRegisterGpuHandle(const char *name);
  handle = SimRegisterGpuHandle("gpu-0000:01:00.0");
  if (!handle) { SimGpuFree(gpu); return EFI_OUT_OF_RESOURCES; }
  gpu->handle = handle;
  gpu->attributes = EFI_PCI_IO_ATTRIBUTE_MEMORY;
  *out = gpu;
  return EFI_SUCCESS;
}

void SimGpuFree(SIM_GPU *gpu) {
  if (!gpu) return;
  free(gpu->config);
  free(gpu->bar);
  free(gpu->prom);
  for (unsigned i = 0; i < 2; i++) {
    free(gpu->falcons[i].imem);
    free(gpu->falcons[i].dmem);
  }
  for (size_t i = 0; i < gpu->mapping_count; i++) {
    if (gpu->mappings[i]->used)
      munmap(gpu->mappings[i]->host, gpu->mappings[i]->bytes);
    free(gpu->mappings[i]);
  }
  free(gpu->mappings);
  free(gpu);
}

size_t SimTraceCount(void) { return g_trace_total; }
int SimTraceOverflowed(void) { return 0; }
int SimTraceKind(size_t index) {
  return index < g_trace_count ? (int)g_trace[index].kind : -1;
}
UINT32 SimTraceOffset(size_t index) {
  return index < g_trace_count ? g_trace[index].offset : 0xffffffffu;
}
UINT32 SimTraceValue(size_t index) {
  return index < g_trace_count ? g_trace[index].value : 0xffffffffu;
}
int SimTraceWatch(UINT32 offset) {
  if (g_watch_count >= SIM_TRACE_WATCH_MAX) return -1;
  int slot = (int)g_watch_count++;
  g_watch_offset[slot] = offset;
  g_watch_first[slot] = -1;
  g_watch_last[slot] = -1;
  g_watch_writes[slot] = 0;
  return slot;
}
long SimTraceWatchFirst(int slot) {
  return (slot >= 0 && (unsigned)slot < g_watch_count) ? g_watch_first[slot] : -1;
}
long SimTraceWatchLast(int slot) {
  return (slot >= 0 && (unsigned)slot < g_watch_count) ? g_watch_last[slot] : -1;
}
size_t SimTraceWatchWrites(int slot) {
  return (slot >= 0 && (unsigned)slot < g_watch_count) ? g_watch_writes[slot] : 0;
}

void SimTraceReset(void) {
  g_trace_count = 0;
  g_trace_total = 0;
  g_watch_count = 0;
  for (unsigned i = 0; i < 16; i++) {
    g_trace_kind_total[i] = 0;
    g_trace_kind_first[i] = -1;
    g_trace_kind_last[i] = -1;
  }
}
size_t SimTraceCountOf(int kind) {
  return (kind > 0 && kind < 16) ? g_trace_kind_total[kind] : 0;
}
long SimTraceFirstOf(int kind) {
  return (kind > 0 && kind < 16) ? g_trace_kind_first[kind] : -1;
}
long SimTraceLastOf(int kind) {
  return (kind > 0 && kind < 16) ? g_trace_kind_last[kind] : -1;
}

UINT32 SimGpuBarRead(SIM_GPU *gpu, UINT32 offset) {
  if (!gpu || offset + 4 > SIM_BAR_BYTES) return 0xffffffffu;
  return Rd32(gpu->bar, offset);
}

void SimGpuBarWrite(SIM_GPU *gpu, UINT32 offset, UINT32 value) {
  if (!gpu || offset + 4 > SIM_BAR_BYTES) return;
  Wr32(gpu->bar, offset, value);
}

/* Model a different SKU: the live config space and the PROM's PCIR record must
 * agree, because the identity checks compare one against the other. The BAR0
 * model is shared by every Ada SKU, which is exactly the assumption the
 * geometry gate refuses to make on the image side. */
/* Model the cold selector pair the pinned profile is built for. */
void SimGpuSetSelectors(SIM_GPU *gpu, UINT32 ss0, UINT32 ss1) {
  if (!gpu) return;
  Wr32(gpu->bar, SIM_REG_SS0, ss0);
  Wr32(gpu->bar, SIM_REG_SS1, ss1);
}

void SimGpuSetDeviceId(SIM_GPU *gpu, UINT16 device) {
  if (!gpu) return;
  Wr16(gpu->config, 0x02, device);
  Wr16(gpu->prom, 0x40 + 0x06, device);         /* PCIR device id */
  memcpy(gpu->bar + SIM_PROM_OFFSET, gpu->prom, SIM_PROM_BYTES);
}

const UINT8 *SimGpuProm(SIM_GPU *gpu) { return gpu ? gpu->prom : NULL; }
UINTN SimGpuFlrCount(SIM_GPU *gpu) { return gpu ? gpu->flr_count : 0; }
UINTN SimGpuBarWrites(SIM_GPU *gpu) { return gpu ? gpu->bar_writes : 0; }
UINTN SimGpuPromWrites(SIM_GPU *gpu) { return gpu ? gpu->prom_writes : 0; }
UINT32 SimGpuBarReadPublic(SIM_GPU *gpu, UINT32 offset) { return SimGpuBarRead(gpu, offset); }
UINTN SimGpuWpr2Opens(void) { return g_wpr2_opens; }
UINT32 SimGpuWpr2Low(SIM_GPU *gpu) {
  return gpu ? Rd32(gpu->bar, 0x001fa824u) : 0;
}
UINT32 SimGpuWpr2High(SIM_GPU *gpu) {
  return gpu ? Rd32(gpu->bar, 0x001fa828u) : 0;
}
UINT32 SimGpuConfigRead(SIM_GPU *gpu, UINT32 offset) {
  if (!gpu || offset + 4 > SIM_CONFIG_BYTES) return 0xffffffffu;
  return Rd32(gpu->config, offset);
}
int SimGpuStockDriverBound(SIM_GPU *gpu) { return gpu ? gpu->stock_driver_bound : 0; }

/* Scenario switches. Each models a card that the image must refuse or survive,
 * rather than special-casing a return value. */
void SimGpuEnableMsi(SIM_GPU *gpu, int enable) {
  if (!gpu) return;
  UINT16 control = Rd16(gpu->config, SIM_CAP_MSI + 2);
  if (enable) control |= 0x0001;
  else control &= (UINT16)~0x0001u;
  Wr16(gpu->config, SIM_CAP_MSI + 2, control);
}

/* Move BAR0 the way firmware would: a 64-bit memory BAR, 32-bit encoding when
 * the base fits, and the window size the descriptor must report. */
void SimGpuSetBarWindow(SIM_GPU *gpu, UINT64 base, UINT64 size) {
  if (!gpu || !size || (size & (size - 1)) || (base & (size - 1))) return;
  gpu->bar_base = base;
  gpu->bar_size = size;
  UINT32 low = (UINT32)(base & 0xfffffff0ull);
  if (base > 0xffffffffull) {
    low |= 0x4;                     /* 64-bit memory BAR */
    Wr32(gpu->config, 0x10, low);
    Wr32(gpu->config, 0x14, (UINT32)(base >> 32));
  } else {
    Wr32(gpu->config, 0x10, low);
    Wr32(gpu->config, 0x14, 0);
  }
}
void SimGpuForceHighDma(SIM_GPU *gpu, int on) {
  if (gpu) gpu->high_dma = on ? 1 : 0;
}

void SimGpuSetFlrSupported(SIM_GPU *gpu, int supported) {
  if (!gpu) return;
  gpu->flr_unsupported = supported ? 0 : 1;
}
EFI_HANDLE SimGpuHandle(SIM_GPU *gpu) { return gpu ? gpu->handle : NULL; }
EFI_PCI_IO_PROTOCOL *SimGpuIo(SIM_GPU *gpu) { return gpu ? &gpu->io : NULL; }

void SimGpuStockDriverDisconnected(SIM_GPU *gpu) {
  if (gpu) gpu->stock_driver_bound = 0;
}

size_t SimGpuSnapshotDwords(void) { return SIM_BAR_BYTES / 4; }
UINT64 SimGpuBarSize(SIM_GPU *gpu) { return gpu ? gpu->bar_size : 0; }
UINT64 SimGpuBarBase(SIM_GPU *gpu) { return gpu ? gpu->bar_base : 0; }

void SimGpuSnapshot(SIM_GPU *gpu, UINT32 *out, size_t dwords) {
  if (!gpu || !out) return;
  if (dwords > SimGpuSnapshotDwords()) dwords = SimGpuSnapshotDwords();
  for (size_t i = 0; i < dwords; i++)
    out[i] = Rd32(gpu->bar, (UINT32)(i * 4));
}

size_t SimGpuDiff(const UINT32 *before, const UINT32 *after,
                  UINT32 *offsets, UINT32 *values, size_t limit) {
  size_t found = 0;
  for (size_t i = 0; i < SimGpuSnapshotDwords() && found < limit; i++) {
    if (before[i] == after[i]) continue;
    offsets[found] = (UINT32)(i * 4);
    values[found] = after[i];
    found++;
  }
  return found;
}
