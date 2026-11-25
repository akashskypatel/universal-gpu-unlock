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

int GuWindowsEspPathCandidate(const uint8_t *path, size_t bound) {
  size_t at=0,length;
  unsigned partitions=0;
  if(!(length=GuWindowsPathLength(path,bound)))return 0;
  while(at+4<=length) {
    size_t n=(size_t)path[at+2] | ((size_t)path[at+3]<<8);
    if(path[at]==0x7fU)break;
    if(path[at]==4U && path[at+1]==1U) {
      if(n!=HARD_DRIVE_NODE_BYTES || ++partitions!=1U)return 0;
      if(!path[at+16] && !path[at+17] && !path[at+18] && !path[at+19] &&
         !path[at+20] && !path[at+21] && !path[at+22] && !path[at+23])return 0;
      if(path[at+40]!=2U || path[at+41]!=2U)return 0;
    }
    at+=n;
  }
  return partitions==1U;
}

int GuWindowsEspPathGuid(const uint8_t *path, size_t bound, uint8_t guid[16]) {
  if(!guid||!GuWindowsEspPathCandidate(path,bound))return 0;
  size_t length=GuWindowsPathLength(path,bound);
  for(size_t at=0;at+4<=length;) {
    size_t n=(size_t)path[at+2]|((size_t)path[at+3]<<8);
    if(path[at]==4U&&path[at+1]==1U) {
      unsigned nonzero=0;
      for(size_t i=0;i<16U;i++)nonzero|=path[at+24U+i];
      if(!nonzero)return 0;
      memcpy(guid,path+at+24U,16U);
      return 1;
    }
    at+=n;
  }
  return 0;
}

/* The device-node prefix both sides have in common, or zero for a mismatch. */
static size_t DevicePrefix(const uint8_t *path, size_t bound) {
  size_t at=0;
  while(at+4<=bound) {
    size_t n=(size_t)path[at+2] | ((size_t)path[at+3]<<8);
    if(n<4 || n>bound-at)return 0;
    /* A FilePath node names a file on the volume, not the volume; anything
     * else - including the media hard-drive node that carries the partition
     * GUID - is part of the identity being compared. */
    if(path[at]==0x7fU || (path[at]==4U && path[at+1]==4U))return at;
    at+=n;
  }
  /* Every node consumed: the prefix is the whole range. A boot option may end
   * its list this way, and a truncated tail still has to match byte for byte
   * on both sides before anything is called a match. */
  return at>=4U?at:0;
}

int GuBootOptionNamesDevicePath(const uint8_t *load_option, size_t option_bound,
                                const uint8_t *path, size_t path_bound) {
  size_t at,prefix,wanted,have;
  if(!load_option || option_bound<8 || option_bound>DP_MAX || !path)return 0;
  wanted=(size_t)load_option[4] | ((size_t)load_option[5]<<8);
  if(wanted<4)return 0;
  /* The description is UTF-16 and must be terminated inside the blob. */
  for(at=6;at+1<option_bound;at+=2)
    if(load_option[at]==0 && load_option[at+1]==0)break;
  if(at+2>option_bound)return 0;
  at+=2;
  if(at+wanted>option_bound)return 0;
  have=DevicePrefix(load_option+at,wanted);
  if(!have)return 0;
  prefix=DevicePrefix(path,path_bound);
  if(!prefix || prefix!=have)return 0;
  return memcmp(load_option+at,path,prefix)==0;
}

#define GU_ORDER_MAX_ENTRIES 64U
#define GU_ORDER_OPTION_BYTES 2048U

/* "BootOrder" and "Boot####", as the UTF-16LE bytes GetVariable takes, with the
 * terminator. The caller must supply at least 2 * (GU_ORDER_NAME_MAX + 1) bytes
 * and the length is returned including both terminator bytes. */
#define GU_ORDER_NAME_MAX 15U
static size_t Utf16Name(const char *ascii, uint8_t *out) {
  size_t n=0;
  if(!ascii||!out)return 0;
  for(;ascii[n];n++) {
    if(n>=GU_ORDER_NAME_MAX)return 0;
    out[2*n]=(uint8_t)ascii[n];
    out[2*n+1]=0;
  }
  out[2*n]=0;out[2*n+1]=0;
  return 2*n+2;
}

int GuBootOrderNamesVolume(GU_VARIABLE_READ read, const uint8_t *path,
                           size_t path_bound) {
  static const char hex[]="0123456789ABCDEF";
  uint8_t name[2U*(GU_ORDER_NAME_MAX+1U)];
  uint8_t order[GU_ORDER_MAX_ENTRIES*2U];
  uint8_t option[GU_ORDER_OPTION_BYTES];
  size_t name_bytes,order_size,option_size,entries;
  if(!read||!path||!path_bound)return GU_ORDER_UNREADABLE;
  if(!(name_bytes=Utf16Name("BootOrder",name)))return GU_ORDER_UNREADABLE;
  order_size=sizeof(order);
  if(read(name,name_bytes,order,&order_size)!=1)return GU_ORDER_UNREADABLE;
  if(order_size>sizeof(order)||order_size%2U)return GU_ORDER_UNREADABLE;
  /* The entry count is fixed here: the per-option call below uses its own size
   * variable, because reusing one across the loop would change the bound. */
  entries=order_size/2U;
  for(size_t i=0;i<entries;i++) {
    char text[9]={'B','o','o','t',0,0,0,0,0};
    unsigned entry=(unsigned)order[2*i] | ((unsigned)order[2*i+1]<<8);
    int got;
    text[4]=hex[(entry>>12)&0x0fU];text[5]=hex[(entry>>8)&0x0fU];
    text[6]=hex[(entry>>4)&0x0fU];text[7]=hex[entry&0x0fU];
    if(!(name_bytes=Utf16Name(text,name)))return GU_ORDER_UNREADABLE;
    option_size=sizeof(option);
    got=read(name,name_bytes,option,&option_size);
    if(got==0)continue;                                  /* named but absent */
    if(got!=1||option_size>sizeof(option))return GU_ORDER_UNREADABLE;
    if(GuBootOptionNamesDevicePath(option,option_size,path,path_bound))
      return GU_ORDER_NAMES;
  }
  return GU_ORDER_NONE;
}
