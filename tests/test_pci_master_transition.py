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


def source_body(name):
    match = re.search(
        r"\bstatic\s+(?:BOOLEAN|EFI_STATUS)\s+" + re.escape(name)
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
#define EFI_PCI_IO_ATTRIBUTE_BUS_MASTER 4u
#define EfiPciIoAttributeOperationGet 1
#define EfiPciIoAttributeOperationDisable 2
#define NOTE(...) ((void)0)

typedef struct EFI_PCI_IO_PROTOCOL EFI_PCI_IO_PROTOCOL;
struct EFI_PCI_IO_PROTOCOL {
  EFI_STATUS (*Attributes)(EFI_PCI_IO_PROTOCOL *, int, UINT64, UINT64 *);
};
static struct {
  int ready, mutated, stage, claimed, master_normalized;
  UINT32 config[64];
  void *table;
} Plan;
static struct {
  int owned,fault,retained,flrs;
  EFI_PCI_IO_PROTOCOL *pci;
} Gu;
static UINT32 current[64];
static UINT64 attrs;
static int peer_fail,disable_fail,no_effect,other_attr_change,read_fail,disable_count;
static EFI_PCI_IO_PROTOCOL pci;
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
  Gu.pci=&pci;
  peer_fail=disable_fail=no_effect=other_attr_change=read_fail=disable_count=0;
}
int main(void) {
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

  puts("STATUS=COMPLETE PASS=11 REAL_PCI_MASTER_TRANSITION_CASES");
  return 0;
}
"""


class ActualMasterTransitionTests(unittest.TestCase):
    def test_normalization_with_independent_mock_io(self):
        code = HARNESS.replace(
            "__ACTUAL_CODE__",
            "\n\n".join([source_body("ConfigStable"),
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
            self.assertIn("STATUS=COMPLETE PASS=11", execution.stdout)


if __name__ == "__main__":
    unittest.main()
