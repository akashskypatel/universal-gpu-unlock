/* SPDX-License-Identifier: GPL-2.0-only */
/* Compare two separately bounded UEFI device paths by complete nodes. */
#pragma once
#include <stddef.h>
/* Inspect the full bounded path before any prefix comparison. A mismatch
 * at the first node must not conceal a malformed later node. The caller's
 * byte bound may exceed the actual path size (UEFI protocol handle), so
 * the first valid END_ENTIRE terminates parsing. */
static inline int GuDevicePathValid(const unsigned char *path,size_t bytes)
{
  size_t pos=0;
  if(!path||bytes<4)return 0;
  for(unsigned nodes=0;nodes<64;nodes++) {
    if(pos>bytes-4)return 0;
    size_t length=(size_t)path[pos+2]+((size_t)path[pos+3]<<8);
    if(length<4||length>bytes-pos)return 0;
    if(path[pos]==0x7f)
      return path[pos+1]==0xff&&length==4;
    pos+=length;
  }
  return 0;
}
static inline int GuDevicePathHasPrefix(const unsigned char *parent,
                                       size_t parent_bytes,
                                       const unsigned char *child,
                                       size_t child_bytes)
{
  size_t a=0,b=0;
  if(!GuDevicePathValid(parent,parent_bytes)||
     !GuDevicePathValid(child,child_bytes))return -1;
  for(unsigned nodes=0;nodes<64;nodes++) {
    if(a>parent_bytes-4||b>child_bytes-4)return -1;
    size_t an=(size_t)parent[a+2]+((size_t)parent[a+3]<<8);
    size_t bn=(size_t)child[b+2]+((size_t)child[b+3]<<8);
    if(an<4||bn<4||an>parent_bytes-a||bn>child_bytes-b)return -1;
    if(parent[a]==0x7f) {
      if(parent[a+1]!=0xff||an!=4)return -1;
      /* A matching prefix is insufficient if the candidate suffix is
       * malformed. Inspect every remaining node through the final END. */
      for(unsigned tail=0;tail<64;tail++) {
        if(b>child_bytes-4)return -1;
        bn=(size_t)child[b+2]+((size_t)child[b+3]<<8);
        if(bn<4||bn>child_bytes-b)return -1;
        if(child[b]==0x7f)
          return (child[b+1]==0xff&&bn==4)?1:-1;
        b+=bn;
      }
      return -1;
    }
    if(child[b]==0x7f) {
      if(child[b+1]!=0xff||bn!=4)return -1;
      return 0; /* candidate ended before parent */
    }
    if(an!=bn)return 0;
    for(size_t i=0;i<an;i++)
      if(parent[a+i]!=child[b+i])return 0;
    a+=an;b+=bn;
  }
  return -1;
}

/* Current-boot display-controller identities only; paths are read-only.
 * Return 1 for exactly one AMD display owner, 0 for no AMD owner or a
 * different display vendor, -1 for invalid input or ambiguous ancestry.
 * A root-port-only prefix and a GOP count do not identify the owner. */
#define GU_DISPLAY_OWNER_MAX 32u
typedef struct {
  const unsigned char *path;
  size_t path_bytes;
  unsigned short vendor;
  unsigned short display_class;
} GU_DEVICE_PATH_DISPLAY_OWNER;

static inline int GuDevicePathAmdDisplayOwner(
    const unsigned char *candidate, size_t candidate_bytes,
    const GU_DEVICE_PATH_DISPLAY_OWNER *owners, size_t count)
{
  unsigned matches=0;
  int amd=0;
  if(!candidate||!owners||!count||count>GU_DISPLAY_OWNER_MAX)return -1;
  for(size_t i=0;i<count;i++) {
    if(!owners[i].path ||
       (owners[i].display_class&0xff00u)!=0x0300u)return -1;
    int match=GuDevicePathHasPrefix(
        owners[i].path,owners[i].path_bytes,candidate,candidate_bytes);
    if(match<0)return -1;
    if(match) {
      if(++matches>1)return -1;
      amd=owners[i].vendor==0x1002u;
    }
  }
  return matches&&amd?1:0;
}
