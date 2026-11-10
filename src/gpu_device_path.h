/* SPDX-License-Identifier: GPL-2.0-only */
/* Compare bounded UEFI device path nodes. No PCI/MMIO operations. */
#pragma once
#include <stddef.h>
static inline int GuDevicePathHasPrefix(const unsigned char *parent,
                                       const unsigned char *child,
                                       size_t max_bytes)
{
  size_t a=0,b=0;
  if(!parent||!child||max_bytes<4)return -1;
  for(unsigned nodes=0;nodes<64;nodes++){
    if(a>max_bytes-4||b>max_bytes-4)return -1;
    size_t an=(size_t)parent[a+2]+((size_t)parent[a+3]<<8);
    size_t bn=(size_t)child[b+2]+((size_t)child[b+3]<<8);
    if(an<4||bn<4||an>max_bytes-a||bn>max_bytes-b)return -1;
    if(parent[a]==0x7f){
      if(parent[a+1]!=0xff||an!=4)return -1;
      return 1; /* child has every preceding parent node */
    }
    if(an!=bn)return 0;
    for(size_t i=0;i<an;i++)
      if(parent[a+i]!=child[b+i])return 0;
    if(child[b]==0x7f)return 0; /* shorter than parent */
    a+=an;b+=bn;
  }
  return -1;
}
