"""Compile and execute the actual selected-driver post-Stop normalization code.

Host mocks cover register/attribute mismatch, failed UEFI Attributes operations,
unexpected peer drift, and a strict pre-FLR phase. No PCI/GPU hardware is used.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

SOURCE = (Path(__file__).resolve().parents[1] / "src" / "msi_release.c").read_text(encoding="utf-8")
INCLUDE_DIR = Path(__file__).resolve().parents[1] / "src"


def source_define(name):
    match = re.search(r"^#define\s+" + re.escape(name) + r"\s+([^\r\n]+)",
                      SOURCE, re.MULTILINE)
    if not match:
        raise AssertionError("missing real source define: " + name)
    return re.sub(r"/\*.*?\*/", "", match.group(1)).strip()


def source_body(name):
    match = re.search(
        r"\bstatic\s+(?:BOOLEAN|EFI_STATUS|UINT64)\s+" + re.escape(name)
        + r"\s*\([^;]*?\)\s*\{", SOURCE, re.DOTALL
    )
    if not match:
        raise AssertionError("missing real source function: " + name)
    pos = match.end()
    depth = 1
    while depth and pos < len(SOURCE):
        depth += (SOURCE[pos] == "{") - (SOURCE[pos] == "}")
        pos += 1
    if depth:
        raise AssertionError("unterminated source function: " + name)
    return SOURCE[match.start():pos]


HARNESS = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pci_master_policy.h"
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef unsigned short UINT16;
typedef size_t UINTN;
typedef void *EFI_HANDLE;
typedef uint8_t UINT8;
#define VOID void
typedef int EFI_STATUS;
typedef int BOOLEAN;
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_NOT_READY 2
#define EFI_UNSUPPORTED 3
#define EFI_DEVICE_ERROR 4
#define EFI_COMPROMISED_DATA 5
#define EFI_ACCESS_DENIED 6
#define EFI_PCI_IO_ATTRIBUTE_IO 0x100u
#define EFI_PCI_IO_ATTRIBUTE_MEMORY 0x200u
#define EFI_PCI_IO_ATTRIBUTE_BUS_MASTER 4u
#define EFI_ERROR(s) ((s)!=EFI_SUCCESS)
#define EfiPciIoAttributeOperationGet 1
#define EfiPciIoAttributeOperationDisable 2
/* The real NOTE is a GuLog. A no-op macro would leave the arguments unused and,
 * with -Werror, would make this harness fail on formatting that the firmware
 * build compiles happily - so the sink consumes them. */
static void NoteSink(const char *format,...){(void)format;}
#define NOTE(...) NoteSink(__VA_ARGS__)
#define GU_TX_PENDING_POLLS %%s

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
typedef struct EFI_BOOT_SERVICES EFI_BOOT_SERVICES;
struct EFI_BOOT_SERVICES { EFI_STATUS (*Stall)(UINTN); };
typedef struct { EFI_BOOT_SERVICES *BootServices; } EFI_SYSTEM_TABLE;
static struct {
  int ready, mutated, stage, claimed, master_normalized;
  UINT32 config[64];
  UINT16 pcie_cap;
  EFI_SYSTEM_TABLE *table;
  EFI_HANDLE pci;
} Plan;
static struct {
  int owned,fault,retained,flrs;
  EFI_PCI_IO_PROTOCOL *pci;
} Gu;
static UINT32 current[64];
static UINT64 attrs;
static int peer_fail,disable_fail,no_effect,other_attr_change,read_fail,disable_count;
/* The decode-restore path: how many Command words were written, whether the write
 * fails, and whether the readback lies. */
static int decode_writes,decode_fail,decode_readback_lies;
static EFI_PCI_IO_PROTOCOL pci;
static EFI_STATUS PciWrite(EFI_PCI_IO_PROTOCOL *self,UINT8 width,UINT64 offset,
                          UINTN count,VOID *buffer) {
  (void)self;
  if(width!=EfiPciIoWidthUint16||offset!=4u||count!=1u||!buffer)return EFI_UNSUPPORTED;
  decode_writes++;
  if(decode_fail)return EFI_DEVICE_ERROR;
  current[1]=(current[1]&0xffff0000u)|(*(UINT16 *)buffer);
  return EFI_SUCCESS;
}
static EFI_STATUS PciRead(EFI_PCI_IO_PROTOCOL *self,UINT8 width,UINT64 offset,
                          UINTN count,VOID *buffer) {
  (void)self;
  if(width!=EfiPciIoWidthUint16||offset!=4u||count!=1u||!buffer)return EFI_UNSUPPORTED;
  *(UINT16 *)buffer=decode_readback_lies?(UINT16)0u:(UINT16)(current[1]&0xffffu);
  return EFI_SUCCESS;
}
/* The real path reaches the config space through the device handle, not through a
 * cached protocol pointer: open PCI_IO on Plan.pci and require it to be the same
 * instance the rest of the file uses. */
static int PciGuid;
static EFI_STATUS Interface(EFI_HANDLE handle,void *guid,VOID **out) {
  (void)guid;
  if(handle!=(EFI_HANDLE)&pci||!out)return EFI_UNSUPPORTED;
  *out=&pci;
  return EFI_SUCCESS;
}
static EFI_STATUS GuVerifyActivePeers(void *t) {
  (void)t;
  return peer_fail ? EFI_UNSUPPORTED : EFI_SUCCESS;
}
static EFI_STATUS ReadConfigMode(UINT32 c[64],BOOLEAN permit_initial_master) {
  if(read_fail)return EFI_DEVICE_ERROR;
  if(!permit_initial_master && (current[1]&4u))return EFI_UNSUPPORTED;
  memcpy(c,current,sizeof(current));
  return EFI_SUCCESS;
}
static EFI_STATUS ReadConfig(UINT32 c[64]) {
  return ReadConfigMode(c,FALSE);
}
static EFI_STATUS Attributes(EFI_PCI_IO_PROTOCOL *unused,int op,UINT64 mask,UINT64 *result) {
  (void)unused;
  if(op==EfiPciIoAttributeOperationGet) {
    if(!result)return EFI_DEVICE_ERROR;
    *result=attrs;
    return EFI_SUCCESS;
  }
  if(op!=EfiPciIoAttributeOperationDisable || mask!=4u)return EFI_UNSUPPORTED;
  disable_count++;
  if(disable_fail)return EFI_DEVICE_ERROR;
  if(!no_effect) {
    current[1]&=~4u;
    attrs&=~4u;
  }
  if(other_attr_change)attrs|=8u;
  return EFI_SUCCESS;
}
__ACTUAL_CODE__
static void reset_case(unsigned cmd) {
  memset(&Plan,0,sizeof(Plan));
  memset(&Gu,0,sizeof(Gu));
  memset(current,0,sizeof(current));
  Plan.config[1]=cmd;
  current[1]=cmd;
  Plan.ready=1;
  Plan.mutated=1;
  Plan.stage=3;
  attrs=cmd;
  pci.Attributes=Attributes;
  pci.Pci.Read=PciRead;
  pci.Pci.Write=PciWrite;
  Plan.pci=(EFI_HANDLE)&pci;
  Gu.pci=&pci;
  peer_fail=disable_fail=no_effect=other_attr_change=read_fail=disable_count=0;
  decode_writes=decode_fail=decode_readback_lies=0;
}
int main(void) {
  /* RUN0023/25/28: raw PCI config was restored to IO|MEM, but PCI_IO's own
   * attribute state still read zero. The latter must be independently enabled. */
  assert(ClaimRestoreAttributeMask(0x300u,0u)==0x300u);
  assert(ClaimRestoreAttributeMask(0x300u,0x100u)==0x200u);
  assert(ClaimRestoreAttributeMask(0x100u,0u)==0x100u);
  assert((ClaimRestoreAttributeMask(0x300u,0u)&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER)==0);

  reset_case(6u);
  assert(NormalizeMasterAfterStop()==EFI_SUCCESS);
  assert(Plan.master_normalized && !(current[1]&4u) && !(attrs&4u));
  assert(disable_count==1 && !Gu.retained);

  reset_case(2u);
  assert(NormalizeMasterAfterStop()==EFI_SUCCESS);
  assert(Plan.master_normalized && disable_count==0);

  reset_case(6u);
  peer_fail=1;
  assert(NormalizeMasterAfterStop()==EFI_NOT_READY);
  assert(!Plan.master_normalized && disable_count==0);

  reset_case(6u);
  attrs=2u; /* UEFI says master OFF, config register says ON */
  assert(NormalizeMasterAfterStop()==EFI_COMPROMISED_DATA);
  assert(disable_count==0);

  reset_case(6u);
  disable_fail=1;
  assert(NormalizeMasterAfterStop()==EFI_DEVICE_ERROR);
  assert(Gu.retained && !Plan.master_normalized);

  reset_case(6u);
  no_effect=1;
  assert(NormalizeMasterAfterStop()!=EFI_SUCCESS);
  assert(Gu.retained && !Plan.master_normalized);

  reset_case(6u);
  other_attr_change=1;
  assert(NormalizeMasterAfterStop()==EFI_COMPROMISED_DATA);
  assert(Gu.retained && !Plan.master_normalized);

  reset_case(2u);
  current[1]=6u; /* unexpected master enable is not a valid Stop change */
  attrs=6u;
  assert(NormalizeMasterAfterStop()==EFI_NOT_READY);
  assert(disable_count==0);

  reset_case(6u);
  Gu.flrs=1; /* transaction is too late: FLR already executed */
  assert(NormalizeMasterAfterStop()==EFI_NOT_READY);
  assert(disable_count==0);

  reset_case(6u);
  Plan.stage=2; /* Stop not completed */
  assert(NormalizeMasterAfterStop()==EFI_NOT_READY);
  assert(disable_count==0);

  reset_case(6u);
  read_fail=1;
  assert(NormalizeMasterAfterStop()==EFI_NOT_READY);
  assert(disable_count==0);

  /* The bench case, RUN0012/RUN0013: the final detach cleared IO and memory
   * decode (Command 0x0003 -> 0x0000) and left everything else alone. It is
   * accepted, the Command word is written back, and the snapshot the rest of the
   * path compares against is true again. */
  reset_case(2u);
  Plan.config[1]=0x100003u;      /* Status bit 20 set, Command IO|MEM */
  current[1]=0x100000u;          /* decode cleared by the detaching driver */
  assert(NormalizeMasterAfterStop()==EFI_SUCCESS);
  assert(decode_writes==1 && (current[1]&0xffffu)==0x3u);
  assert(Plan.master_normalized && !Gu.retained);

  /* Clearing only one of the two enable bits is the same fact. */
  reset_case(2u);
  Plan.config[1]=0x100003u;
  current[1]=0x100002u;
  assert(NormalizeMasterAfterStop()==EFI_SUCCESS);
  assert(decode_writes==1 && (current[1]&0xffffu)==0x3u);

  /* A decode bit being SET is not the detach letting go, and never accepted. */
  reset_case(2u);
  Plan.config[1]=0x100000u;
  current[1]=0x100003u;
  assert(NormalizeMasterAfterStop()!=EFI_SUCCESS);
  assert(decode_writes==0);

  /* Another word changing at the same time is drift, not a detach. */
  reset_case(2u);
  Plan.config[1]=0x100003u;
  current[1]=0x100000u;
  current[2]=0xdeadbeefu;
  assert(NormalizeMasterAfterStop()==EFI_NOT_READY);
  assert(decode_writes==0);

  /* The read-only Status half changing is not ours to explain either. */
  reset_case(2u);
  Plan.config[1]=0x100003u;
  current[1]=0x101000u;
  assert(NormalizeMasterAfterStop()==EFI_NOT_READY);
  assert(decode_writes==0);

  /* A write that fails, or a readback that does not match, refuses. */
  reset_case(2u);
  Plan.config[1]=0x100003u;
  current[1]=0x100000u;
  decode_fail=1;
  assert(NormalizeMasterAfterStop()==EFI_NOT_READY);
  assert(decode_writes==1);

  reset_case(2u);
  Plan.config[1]=0x100003u;
  current[1]=0x100000u;
  decode_readback_lies=1;
  assert(NormalizeMasterAfterStop()==EFI_NOT_READY);
  assert(decode_writes==1);

  puts("STATUS=COMPLETE PASS=22 REAL_PCI_MASTER_TRANSITION_CASES");
  return 0;
}
"""


class ActualMasterTransitionTests(unittest.TestCase):
    def test_claim_restore_uses_protocol_attribute_state_after_config_restore(self):
        claim = source_body("ClaimRestore")
        derive = "enable_mask=ClaimRestoreAttributeMask(Plan.attributes,attrs)"
        self.assertIn(derive, claim)
        self.assertLess(claim.index(derive),
                        claim.index("EfiPciIoAttributeOperationSupported"))
        self.assertIn("EfiPciIoAttributeOperationSupported,0,&supported", claim)
        self.assertIn("EfiPciIoAttributeOperationEnable,enable_mask", claim)

    def test_normalization_with_independent_mock_io(self):
        code = HARNESS.replace("%%s", source_define("GU_TX_PENDING_POLLS")).replace(
            "__ACTUAL_CODE__",
            "\n\n".join([source_body("ConfigStable"),
                         source_body("TxPending"),
                         source_body("ConfigStablePostStop"),
                         source_body("WaitTxPendingClear"),
                         source_body("RestoreDecodeClearedByDetach"),
                         source_body("ClaimRestoreAttributeMask"),
                         source_body("NormalizeMasterAfterStop")]),
        )
        compiler = os.environ.get("CC", "gcc")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "normalize.c"
            binary = Path(directory) / "normalize"
            source.write_text(code, encoding="utf-8")
            cc = subprocess.run(
                [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-I", str(INCLUDE_DIR), str(source), "-o", str(binary)],
                capture_output=True, text=True, timeout=30, check=False,
            )
            self.assertEqual(cc.returncode, 0, cc.stderr)
            execution = subprocess.run(
                [str(binary)], capture_output=True, text=True,
                timeout=10, check=False,
            )
            self.assertEqual(execution.returncode, 0, execution.stderr)
            self.assertIn("STATUS=COMPLETE PASS=22", execution.stdout)


if __name__ == "__main__":
    unittest.main()
