"""Compile and execute the real post-Stop DMA-quiescence code.

Clearing the Bus Master enable stops *new* transactions. C2's open question is
every transaction already posted: this image owns no IOMMU, reads back no engine
queue and takes no bus trace, so the only standard observable left is the PCIe
Device Status "Transaction Pending" bit.

`WaitTxPendingClear` polls that bit a bounded number of times and refuses when it
never drains. This module extracts that function, `TxPending`, `ConfigStable` and
`NormalizeMasterAfterStop` from ``src/msi_release.c`` and drives them against a
modelled card whose pending bit clears, clears late, or never clears - plus the
failure modes that must retain buffers: a failed config read, config drift, a
missing Stall service, and a missing PCIe capability.

No PCI/GPU hardware is touched: the PCIe capability and its Device Status word
are crafted bytes.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "msi_release.c").read_text(encoding="utf-8")


def source_body(name):
    pattern = (
        r"\b(?:static\s+)?(?:BOOLEAN|EFI_STATUS|VOID)\s+"
        + re.escape(name)
        + r"\s*\([^;]*?\)\s*\{"
    )
    match = re.search(pattern, SOURCE, re.DOTALL)
    if not match:
        raise AssertionError("missing real source function: " + name)
    pos = match.end()
    depth = 1
    while depth and pos < len(SOURCE):
        depth += (SOURCE[pos] == "{") - (SOURCE[pos] == "}")
        pos += 1
    if depth:
        raise AssertionError("unterminated source function: " + name)
    start = SOURCE.rfind("\n", 0, match.start()) + 1
    return SOURCE[start:pos]


def source_define(name):
    match = re.search(r"^#define\s+" + re.escape(name) + r"\s+([^\r\n]+)", SOURCE, re.MULTILINE)
    if not match:
        raise AssertionError("missing real source define: " + name)
    return re.sub(r"/\*.*?\*/", "", match.group(1)).strip()


HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "pci_master_policy.h"

typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;
typedef unsigned long      UINTN;
typedef int                EFI_STATUS;
typedef int                BOOLEAN;
typedef void               VOID;

#define TRUE 1
#define FALSE 0
#ifndef NULL
#define NULL ((void *)0)
#endif

#define EFI_SUCCESS           0
#define EFI_NOT_READY         2
#define EFI_UNSUPPORTED       3
#define EFI_DEVICE_ERROR      4
#define EFI_COMPROMISED_DATA  5
#define EFI_ACCESS_DENIED     6

#define EFI_PCI_IO_ATTRIBUTE_BUS_MASTER 4u
#define EfiPciIoAttributeOperationGet     1
#define EfiPciIoAttributeOperationDisable 2

#define __POLL_BOUND__

static char LogText[8192];
static size_t LogUsed;
static void NoteAppend(const char *text) {
  size_t n = strlen(text);
  if (LogUsed + n >= sizeof(LogText)) n = sizeof(LogText) - 1 - LogUsed;
  memcpy(LogText + LogUsed, text, n);
  LogUsed += n;
  LogText[LogUsed] = 0;
}
static int LogHas(const char *needle) { return strstr(LogText, needle) != NULL; }
#define NOTE(...) do { char b_[256]; snprintf(b_, sizeof b_, __VA_ARGS__); NoteAppend(b_); } while (0)

typedef struct EFI_BOOT_SERVICES EFI_BOOT_SERVICES;
struct EFI_BOOT_SERVICES { EFI_STATUS (*Stall)(UINTN Microseconds); };
typedef struct { EFI_BOOT_SERVICES *BootServices; } EFI_SYSTEM_TABLE;

typedef void *EFI_HANDLE;
typedef struct EFI_PCI_IO_PROTOCOL EFI_PCI_IO_PROTOCOL;
typedef struct {
  EFI_STATUS (*Read)(EFI_PCI_IO_PROTOCOL *, UINT8, UINT64, UINTN, VOID *);
  EFI_STATUS (*Write)(EFI_PCI_IO_PROTOCOL *, UINT8, UINT64, UINTN, VOID *);
} EFI_PCI_IO_ACCESS;
struct EFI_PCI_IO_PROTOCOL {
  EFI_STATUS (*Attributes)(EFI_PCI_IO_PROTOCOL *, int, UINT64, UINT64 *);
  EFI_PCI_IO_ACCESS Pci;
};
#define EfiPciIoWidthUint16 1
#define EFI_ERROR(s) ((s) != EFI_SUCCESS)

static struct {
  int ready, mutated, stage, claimed, master_normalized;
  UINT32 config[64];
  UINT16 pcie_cap;
  EFI_SYSTEM_TABLE *table;
  EFI_HANDLE pci;
} Plan;
static struct { int owned, fault, retained, flrs; EFI_PCI_IO_PROTOCOL *pci; } Gu;

static UINT32 current[64];
static UINT64 attrs;
static int peer_fail, disable_fail, read_fail, disable_count;
static int fail_after = -1;   /* fail a config read once this many reads happened */
static int drift_after = -1;  /* move an unrelated word once this many reads happened */
static unsigned stalls;
static unsigned pending_reads;   /* how many reads still see Transaction Pending */
static unsigned reads;
static EFI_PCI_IO_PROTOCOL pci;
static EFI_BOOT_SERVICES bs;
static EFI_SYSTEM_TABLE table;

static EFI_STATUS Stall(UINTN Microseconds) {
  stalls++;
  (void)Microseconds;
  return EFI_SUCCESS;
}

/* The decode restore reaches the Command register through the device handle.
 * This harness keeps the card's Command word fixed, so the restore stays inert
 * here; `tests/test_pci_master_transition.py` drives it directly. */
static int PciGuid;
static EFI_STATUS Interface(EFI_HANDLE handle, void *guid, VOID **out) {
  (void)guid;
  if (handle != (EFI_HANDLE)&pci || !out) return EFI_UNSUPPORTED;
  *out = &pci;
  return EFI_SUCCESS;
}
static unsigned decode_writes;
static EFI_STATUS PciWrite(EFI_PCI_IO_PROTOCOL *self, UINT8 width, UINT64 offset,
                           UINTN count, VOID *buffer) {
  (void)self;
  if (width != EfiPciIoWidthUint16 || offset != 4u || count != 1u || !buffer) return EFI_UNSUPPORTED;
  decode_writes++;
  current[1] = (current[1] & 0xFFFF0000u) | (*(UINT16 *)buffer);
  return EFI_SUCCESS;
}
static EFI_STATUS PciRead(EFI_PCI_IO_PROTOCOL *self, UINT8 width, UINT64 offset,
                          UINTN count, VOID *buffer) {
  (void)self;
  if (width != EfiPciIoWidthUint16 || offset != 4u || count != 1u || !buffer) return EFI_UNSUPPORTED;
  *(UINT16 *)buffer = (UINT16)(current[1] & 0xFFFFu);
  return EFI_SUCCESS;
}

static EFI_STATUS GuVerifyActivePeers(void *t) {
  (void)t;
  return peer_fail ? EFI_UNSUPPORTED : EFI_SUCCESS;
}

/* The mock card: config bytes come from `current`, and the pending bit is
 * cleared after `pending_reads` successful reads of the snapshot. */
static void Set8(UINT32 c[64], UINTN off, UINT8 value) {
  UINTN index = off / 4u, shift = (off & 3u) * 8u;
  c[index] = (c[index] & ~(0xFFu << shift)) | ((UINT32)value << shift);
}

static EFI_STATUS ReadConfigMode(UINT32 c[64], BOOLEAN permit_initial_master) {
  if (read_fail) return EFI_DEVICE_ERROR;
  if (fail_after >= 0 && (int)reads >= fail_after) return EFI_DEVICE_ERROR;
  reads++;
  memcpy(c, current, sizeof(current));
  if (drift_after >= 0 && (int)reads > drift_after) c[0x10 / 4] = 0xDEADBEEFu;
  if (pending_reads) {           /* the modelled card still has one in flight */
    pending_reads--;
    Set8(c, (UINTN)Plan.pcie_cap + 0x0au, (UINT8)(c[((UINTN)Plan.pcie_cap + 0x0au) / 4u]
         >> ((((UINTN)Plan.pcie_cap + 0x0au) & 3u) * 8u)) | 0x20u);
  }
  return permit_initial_master || !(c[1] & 4u) ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

static EFI_STATUS ReadConfig(UINT32 c[64]) { return ReadConfigMode(c, FALSE); }

static EFI_STATUS Attributes(EFI_PCI_IO_PROTOCOL *unused, int op, UINT64 mask, UINT64 *result) {
  (void)unused;
  if (op == EfiPciIoAttributeOperationGet) {
    if (!result) return EFI_DEVICE_ERROR;
    *result = attrs;
    return EFI_SUCCESS;
  }
  if (op != EfiPciIoAttributeOperationDisable || mask != 4u) return EFI_UNSUPPORTED;
  disable_count++;
  if (disable_fail) return EFI_DEVICE_ERROR;
  current[1] &= ~4u;
  attrs &= ~4u;
  return EFI_SUCCESS;
}

__ACTUAL_CODE__

/* A complete snapshot with a PCIe capability at 0x50: capabilities word 0x0002
 * (version 2, endpoint) and Device Status at 0x5A. */
static void Snapshot(UINT32 c[64], int pending, unsigned command) {
  memset(c, 0, 64u * sizeof(UINT32));
  c[0] = 0x268410DEu;
  c[2] = 0x03000000u;
  c[1] = command;
  c[0x0e / 4] = 0;
  c[0x34 / 4] |= 0x40u;                 /* capability list at 0x40 */
  c[0x40 / 4] = 0x00000001u;            /* PM capability, next = 0 */
  c[0x50 / 4] = 0x00000010u;            /* PCIe capability, next = 0 */
  c[0x52 / 4] |= 0x0002u;               /* version 2, endpoint */
  c[0x54 / 4] |= 1u << 28;              /* FLR capable */
  (void)pending;                        /* the mock injects Pending per read */
}

static void Reset(int pending, unsigned command) {
  memset(&Plan, 0, sizeof(Plan));
  memset(&Gu, 0, sizeof(Gu));
  memset(LogText, 0, sizeof(LogText));
  LogUsed = 0;
  Snapshot(current, pending, command);
  Snapshot(Plan.config, pending, command);
  Plan.ready = 1;
  Plan.mutated = 1;
  Plan.stage = 3;
  Plan.pcie_cap = 0x50;
  table.BootServices = &bs;
  bs.Stall = Stall;
  Plan.table = &table;
  attrs = command;
  pci.Attributes = Attributes;
  pci.Pci.Read = PciRead;
  pci.Pci.Write = PciWrite;
  Plan.pci = (EFI_HANDLE)&pci;
  decode_writes = 0;
  Gu.pci = &pci;
  peer_fail = disable_fail = read_fail = 0;
  fail_after = -1;
  drift_after = -1;
  disable_count = 0;
  stalls = 0;
  reads = 0;
  pending_reads = 0;
}

static int Failures = 0;
static int Checks = 0;
#define EXPECT(cond) do { \
  ++Checks; \
  if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++Failures; } \
} while (0)

int main(void) {
  UINTN polls;

  /* 1. Nothing pending: one observation is enough and the log says what was
   *    and was not established. */
  Reset(0, 6u);
  EXPECT(NormalizeMasterAfterStop() == EFI_SUCCESS);
  EXPECT(Plan.master_normalized && !(current[1] & 4u) && !(attrs & 4u));
  EXPECT(disable_count == 1 && !Gu.retained);
  EXPECT(LogHas("POST_STOP_QUIESCENCE=BUS_MASTER_ONLY TX_PENDING_CLEAR=1 POLLS=1"));
  EXPECT(LogHas("IOMMU=UNMANAGED QUEUE_READBACK=0 BUS_TRACE=0"));
  EXPECT(LogHas("POST_STOP_BUS_MASTER=DISABLED VERIFIED=1 PREVIOUS=1"));
  EXPECT(stalls == 0);

  /* 2. A transaction that is still in flight when the bit is cleared drains
   *    within the bound: the first read sees it, the second poll does not. */
  Reset(0, 6u);
  pending_reads = 3;      /* reads 1-3 see the bit set, the second poll does not */
  EXPECT(NormalizeMasterAfterStop() == EFI_SUCCESS);
  EXPECT(Plan.master_normalized && !Gu.retained);
  EXPECT(LogHas("TX_PENDING_CLEAR=1 POLLS=2"));
  EXPECT(stalls == 1);

  /* 3. A transaction that never drains is refused, buffers are retained, and
   *    the run is not marked normalized. */
  Reset(1, 6u);
  pending_reads = 1000u;
  EXPECT(NormalizeMasterAfterStop() == EFI_NOT_READY);
  EXPECT(Gu.retained && !Plan.master_normalized);
  EXPECT(LogHas("POST_STOP_QUIESCENCE=REFUSED REASON=TX_PENDING_STUCK POLLS=5"));
  EXPECT(stalls == 4);

  /* 4. The bound is a bound: no more than five observations and four waits.
   *    The enable is already clear here, which is the state the poll runs in. */
  Reset(1, 2u);
  pending_reads = 1000u;
  polls = 0;
  EXPECT(WaitTxPendingClear(&polls) == EFI_NOT_READY);
  EXPECT(polls == 5 && stalls == 4);

  /* 5. Without a PCIe capability the bit cannot be observed, and the log says
   *    UNOBSERVED rather than pretending it was clear. */
  Reset(1, 6u);
  Plan.pcie_cap = 0;
  EXPECT(NormalizeMasterAfterStop() == EFI_SUCCESS);
  EXPECT(Plan.master_normalized && !Gu.retained);
  EXPECT(LogHas("TX_PENDING=UNOBSERVED POLLS=0"));

  /* 6. No Stall service: the observation is a single read, and a pending
   *    transaction still refuses (fail closed). */
  Reset(1, 6u);
  pending_reads = 1000u;
  bs.Stall = NULL;
  EXPECT(NormalizeMasterAfterStop() == EFI_NOT_READY);
  EXPECT(Gu.retained && !Plan.master_normalized);
  EXPECT(LogHas("REASON=TX_PENDING_STUCK POLLS=1"));

  /* 7. A config read that fails during the observation is an error, not a
   *    pass: the first read (before the disable) succeeds, the poll does not. */
  Reset(1, 6u);
  pending_reads = 1000u;
  fail_after = 2;
  EXPECT(NormalizeMasterAfterStop() == EFI_DEVICE_ERROR);
  EXPECT(Gu.retained && !Plan.master_normalized);

  /* 8. Drift already present when the post-Stop read starts is refused before
   *    anything is touched. */
  Reset(0, 6u);
  current[0x10 / 4] = 0xDEADBEEFu;
  EXPECT(NormalizeMasterAfterStop() == EFI_NOT_READY);
  EXPECT(!Plan.master_normalized && disable_count == 0);

  /* 9. Drift that appears between observations is compromised data, and the
   *    buffers are retained rather than reported as a clean stop. */
  Reset(0, 6u);
  drift_after = 1;
  EXPECT(NormalizeMasterAfterStop() == EFI_COMPROMISED_DATA);
  EXPECT(Gu.retained && !Plan.master_normalized);

  /* 10. Nothing was pending and Bus Master was already clear: no disable, no
   *    wait, still normalized. */
  Reset(0, 2u);
  EXPECT(NormalizeMasterAfterStop() == EFI_SUCCESS);
  EXPECT(disable_count == 0 && stalls == 0 && Plan.master_normalized);
  EXPECT(LogHas("PREVIOUS=0"));

  /* 11. A second normalization is refused, so a stuck card cannot be talked
   *     into "done" by calling again. */
  Reset(0, 6u);
  EXPECT(NormalizeMasterAfterStop() == EFI_SUCCESS);
  EXPECT(NormalizeMasterAfterStop() == EFI_NOT_READY);

  /* 12. The pending bit is read from the Device Status word of the offset the
   *     policy reported, not from a hard-coded address. */
  Reset(0, 6u);
  memset(current, 0, sizeof(current));
  Set8(current, 0x6a, 0x20);      /* Device Status of the capability at 0x60 */
  EXPECT(TxPending(current, 0x60));
  EXPECT(!TxPending(current, 0x50));
  EXPECT(!TxPending(current, 0));
  Set8(current, 0x5a, 0x20);      /* the capability at 0x50 has its own word */
  EXPECT(TxPending(current, 0x50));
  EXPECT(TxPending(current, 0x60));

  if (Failures) { fprintf(stderr, "FAILURES=%d\n", Failures); return 1; }
  printf("STATUS=COMPLETE PASS=%d DMA_QUIESCENCE_CASES\n", Checks);
  return 0;
}
"""


def harness() -> str:
    code = "\n\n".join(
        source_body(name)
        for name in ("ConfigStable", "TxPending", "ConfigStablePostStop",
                     "WaitTxPendingClear", "RestoreDecodeClearedByDetach",
                     "NormalizeMasterAfterStop")
    )
    return HARNESS.replace("#define __POLL_BOUND__", "#define GU_TX_PENDING_POLLS %s"
                           % source_define("GU_TX_PENDING_POLLS")).replace(
        "__ACTUAL_CODE__", code)


class DmaQuiescence(unittest.TestCase):
    def test_pending_transactions_are_observed_and_bounded(self):
        compiler = os.environ.get("CC", "gcc")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "quiescence.c"
            binary = Path(directory) / "quiescence"
            source.write_text(harness(), encoding="utf-8")
            built = subprocess.run(
                [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "src"), str(source), "-o", str(binary)],
                capture_output=True, text=True, timeout=60, check=False,
            )
            self.assertEqual(built.returncode, 0, built.stderr[-3000:])
            executed = subprocess.run([str(binary)], capture_output=True, text=True,
                                      timeout=30, check=False)
            self.assertEqual(executed.returncode, 0, executed.stderr[-3000:])
            expected = harness().count("EXPECT(") - 1
            self.assertGreaterEqual(expected, 25)
            self.assertIn("STATUS=COMPLETE PASS=%d DMA_QUIESCENCE_CASES" % expected,
                          executed.stdout)

    def test_radix_cleanup_releases_newest_first_and_stops_on_failure(self):
        vendor = (ROOT / "src/vendor/unlock_610.c").read_text(encoding="utf-8")
        start = vendor.index("static bool free_coherent(")
        actual = vendor[start:vendor.index("static int unlock_transaction_unwind(", start)]
        host = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t dma_addr_t;
#define GU_RADIX_PAGE 4096U
#define GU_V67_SIZE 4096U
#define GU_WPR_META_SIZE 4096U
typedef struct {bool retained;} GU_PLATFORM;
struct pci_dev {int dev;};
struct page {void *cpu;dma_addr_t dma;};
struct unlock_transaction_work {
  struct pci_dev *pdev;
  u32 pages_allocated;
  struct page *pages;
  struct {size_t image_size;} boot_desc;
  void *boot_cpu,*fwsec_cpu,*booter_cpu,*v67_cpu,*stock_cpu,*meta_cpu;
  size_t fwsec_size,booter_image_size,stock_size;
  dma_addr_t boot_dma,fwsec_dma,booter_dma,v67_dma,stock_dma,meta_dma;
};
static GU_PLATFORM owner;
static struct pci_dev pci;
static unsigned count,fail_at,pool_frees;
static dma_addr_t order[8];
static GU_PLATFORM *GuCompatOwner(struct pci_dev *p) {return p==&pci?&owner:NULL;}
static void dma_free_coherent(int *dev,size_t n,void *p,dma_addr_t addr) {
  (void)dev;(void)n;(void)p;order[count++]=addr;
  if(count==fail_at)owner.retained=true;
}
static bool GuCompatFree(struct pci_dev *p,void *mem){(void)p;(void)mem;pool_frees++;return true;}
''' + actual + r'''
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);return 1;}}while(0)
int main(void) {
  struct page pages[3]={{(void *)1,11},{(void *)2,22},{(void *)3,33}};
  for(unsigned mode=0;mode<4;mode++) {
    struct unlock_transaction_work work={0};
    count=pool_frees=0;fail_at=mode==1?2:0;owner.retained=mode==2;
    work.pdev=mode==3?NULL:&pci;work.pages_allocated=3;work.pages=pages;
    work.boot_cpu=(void *)4;work.boot_dma=44;work.boot_desc.image_size=GU_RADIX_PAGE;
    bool result=free_coherent(&work);
    if(mode==0) {
      CHECK(result&&count==4&&order[0]==33&&order[1]==22&&order[2]==11&&order[3]==44);
      CHECK(pool_frees==1&&!work.pages&&!work.pages_allocated);
    } else if(mode==1) {
      CHECK(!result&&owner.retained&&count==2&&order[0]==33&&order[1]==22&&!pool_frees);
      CHECK(work.pages==pages&&work.pages_allocated==3);
    } else CHECK(!result&&!count&&!pool_frees);
  }
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "cleanup.c"
            binary = Path(directory) / "cleanup"
            source.write_text(host, encoding="utf-8")
            built = subprocess.run(
                [os.environ.get("CC", "gcc"), "-std=c11", "-O2", "-Wall", "-Wextra",
                 "-Werror", str(source), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)

    def test_the_observation_is_attached_to_the_stop_path(self):
        body = source_body("NormalizeMasterAfterStop")
        self.assertIn("WaitTxPendingClear(&polls)", body)
        self.assertLess(body.index("WaitTxPendingClear(&polls)"),
                        body.index("Plan.master_normalized=TRUE"))
        # A refusal must retain the caller's buffers, not merely return.
        tail = body[body.index("WaitTxPendingClear(&polls)"):]
        self.assertIn("Gu.retained=TRUE", tail)

    def test_the_quiescence_claim_stops_at_the_bus_master(self):
        """The log must never imply an IOMMU, queue readback or bus trace."""
        body = source_body("WaitTxPendingClear")
        for field in ("IOMMU=UNMANAGED", "QUEUE_READBACK=0", "BUS_TRACE=0"):
            with self.subTest(field=field):
                self.assertIn(field, body)
        self.assertNotIn("IOMMU=MANAGED", body)
        self.assertNotIn("VERIFIED=1 QUEUE", body)

    def test_the_capability_offset_comes_from_the_admission_policy(self):
        self.assertIn("GuCheckConfigInitialMaster(Plan.config", SOURCE)
        self.assertIn("Plan.pcie_cap=caps.pcie", SOURCE)
        # A card the policy will not describe leaves the observation disabled.
        self.assertIn("if(!Plan.pcie_cap) {", source_body("WaitTxPendingClear"))

    def test_the_poll_bound_is_a_count_not_a_measured_budget(self):
        self.assertEqual(source_define("GU_TX_PENDING_POLLS"), "5U")
        self.assertIn("not a measured budget", SOURCE)
        # 1 ms per wait x 4 waits is the most time this can spend.
        self.assertIn("Stall(1000U)", source_body("WaitTxPendingClear"))


if __name__ == "__main__":
    unittest.main()
