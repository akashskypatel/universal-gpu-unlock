/* SPDX-License-Identifier: GPL-2.0-only */
/* Accept only unchanged PCI COMMAND or the Bus Master bit being cleared. */
#pragma once
static inline int GuPciMasterClearOnly(unsigned before,unsigned after) {
  unsigned diff=before^after;
  return diff==0u || (diff==4u&&(before&4u)!=0u&&(after&4u)==0u);
}
