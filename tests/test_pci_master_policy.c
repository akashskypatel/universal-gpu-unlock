/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include "../src/pci_master_policy.h"
int main(void){
  assert(GuPciMasterClearOnly(6u,2u));
  assert(GuPciMasterClearOnly(6u,6u));
  assert(GuPciMasterClearOnly(2u,2u));
  assert(!GuPciMasterClearOnly(2u,6u));
  assert(!GuPciMasterClearOnly(6u,0u));
  assert(!GuPciMasterClearOnly(6u,3u));
  assert(!GuPciMasterClearOnly(0x40006u,2u));
  assert(!GuPciMasterClearOnly(6u,0x80006u));
  assert(!GuPciMasterClearOnly(0xffff0006u,0xffff0000u));
  assert(GuPciMasterClearOnly(0xffff0006u,0xffff0002u));
  puts("STATUS=COMPLETE PASS=10 PCI_MASTER_CLEAR_ONLY");
  return 0;
}
