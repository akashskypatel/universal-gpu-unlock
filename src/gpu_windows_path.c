/* SPDX-License-Identifier: GPL-2.0-only */
#include "gpu_windows_path.h"
#include <string.h>

#define DP_MAX 4096U
#define HARD_DRIVE_NODE_BYTES 42U

size_t GuWindowsPathLength(const uint8_t *path, size_t bound) {
  size_t at=0,n;
  if(!path || bound<4 || bound>DP_MAX)return 0;
  while(at+4<=bound) {
    n=(size_t)path[at+2] | ((size_t)path[at+3]<<8);
    if(n<4 || n>bound-at)return 0;
    if(path[at]==0x7fU)
      return path[at+1]==0xffU && n==4 ? at+4 : 0;
    at+=n;
  }
  return 0;
}

int GuWindowsEspPathMatches(const uint8_t *path, size_t bound,
                            const uint8_t guid[16]) {
  size_t at=0,length;
  unsigned partitions=0;
  int matching=0;
  if(!guid || !(length=GuWindowsPathLength(path,bound)))return 0;
  while(at+4<=length) {
    size_t n=(size_t)path[at+2] | ((size_t)path[at+3]<<8);
    if(path[at]==0x7fU)break;
    if(path[at]==4U && path[at+1]==1U) {
      /* EFI media/HD: require a single nonempty GPT partition and GUID signature. */
      if(n!=HARD_DRIVE_NODE_BYTES || ++partitions!=1U)return 0;
      if(!path[at+16] && !path[at+17] && !path[at+18] && !path[at+19] &&
         !path[at+20] && !path[at+21] && !path[at+22] && !path[at+23])return 0;
      matching=(path[at+40]==2U && path[at+41]==2U &&
                memcmp(path+at+24,guid,16)==0);
    }
    at+=n;
  }
  return partitions==1U && matching;
}
