/* SPDX-License-Identifier: GPL-2.0-only */
/* Structural identity for a 1-MiB NV_PROM_DATA readout. Not signature proof. */
#pragma once
#include <stddef.h>
static inline int GuPromImageMatchesDevice(const unsigned char *rom,
                                             size_t bytes, unsigned device)
{
  size_t pcir;
  unsigned vendor, reported;
  unsigned signature;
  if (!rom || bytes != 1048576u || device > 0xffffu) return 0;
  signature=(unsigned)rom[0] | ((unsigned)rom[1]<<8);
  if (signature!=0xaa55u && signature!=0x4e56u && signature!=0xbb77u)
    return 0;
  pcir=(size_t)rom[0x18] | ((size_t)rom[0x19]<<8);
  if (pcir > bytes-0x16u) return 0;
  if (rom[pcir]!='P' || rom[pcir+1]!='C' ||
      rom[pcir+2]!='I' || rom[pcir+3]!='R') return 0;
  vendor=(unsigned)rom[pcir+4] | ((unsigned)rom[pcir+5]<<8);
  reported=(unsigned)rom[pcir+6] | ((unsigned)rom[pcir+7]<<8);
  return vendor==0x10deu && reported==device;
}
