/* SPDX-License-Identifier: GPL-2.0-only */
#include "gpu_loaded_pe_text.h"
#include <stdint.h>
#include <string.h>

static int within(size_t offset, size_t length, size_t total) {
  return offset<=total && length<=total-offset;
}
static unsigned rd16(const uint8_t *p) {
  return (unsigned)p[0] | (unsigned)p[1]<<8;
}
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1]<<8 |
         (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
int GuLoadedPeTextRange(const void *image, size_t mapped_size,
                        size_t *text_rva, size_t *text_size) {
  const uint8_t *p=(const uint8_t *)image;
  size_t pe, opt, sections, i, rva=0, length=0;
  unsigned count, opt_bytes, found=0;
  uint32_t size_image;
  if(!p || !text_rva || !text_size || !within(0,64,mapped_size))return 0;
  if(p[0]!='M' || p[1]!='Z')return 0;
  pe=(size_t)rd32(p+0x3c);
  if(!within(pe,24,mapped_size) || memcmp(p+pe,"PE\0\0",4))return 0;
  count=rd16(p+pe+6);
  opt_bytes=rd16(p+pe+20);
  opt=pe+24;
  if(!count || count>96U || opt_bytes<60U ||
     !within(opt,opt_bytes,mapped_size) || rd16(p+opt)!=0x20bU)return 0;
  size_image=rd32(p+opt+56);
  if(!size_image || size_image>mapped_size)return 0;
  sections=opt+opt_bytes;
  if(!within(sections,(size_t)count*40U,mapped_size))return 0;
  for(i=0;i<count;i++) {
    const uint8_t *entry=p+sections+40U*i;
    uint32_t vsize,offset,flags;
    if(memcmp(entry,".text\0\0\0",8))continue;
    if(found++)return 0; /* duplicate candidate is ambiguous */
    vsize=rd32(entry+8);
    offset=rd32(entry+12);
    flags=rd32(entry+36);
    if(!vsize || !(flags&0x20000000U) ||
       !within(offset,vsize,size_image) ||
       !within(offset,vsize,mapped_size))return 0;
    rva=(size_t)offset; length=(size_t)vsize;
  }
  if(found!=1)return 0;
  *text_rva=rva;
  *text_size=length;
  return 1;
}
