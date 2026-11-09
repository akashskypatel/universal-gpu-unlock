/* A bounded PE32+ mapped section parser must fail on malformed metadata. */
#include "gpu_loaded_pe_text.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void wr16(uint8_t *b,size_t off,unsigned n) {
  b[off]=(uint8_t)n;b[off+1]=(uint8_t)(n>>8);
}
static void wr32(uint8_t *b,size_t off,uint32_t n) {
  b[off]=(uint8_t)n;b[off+1]=(uint8_t)(n>>8);
  b[off+2]=(uint8_t)(n>>16);b[off+3]=(uint8_t)(n>>24);
}
static void setup(uint8_t *b) {
  memset(b,0,1024);
  b[0]='M';b[1]='Z';wr32(b,0x3c,0x80);
  memcpy(b+0x80,"PE\0\0",4);
  wr16(b,0x86,1);wr16(b,0x94,0xf0);
  wr16(b,0x98,0x20b);wr32(b,0x98+56,1024);
  memcpy(b+0x188,".text",5);
  wr32(b,0x188+8,24);wr32(b,0x188+12,0x200);
  wr32(b,0x188+36,0x20000000);
}
int main(void) {
  uint8_t b[1024];size_t rva=999,count=999;
  setup(b);
  assert(GuLoadedPeTextRange(b,sizeof(b),&rva,&count));
  assert(rva==0x200 && count==24);
  assert(!GuLoadedPeTextRange(NULL,sizeof(b),&rva,&count));
  assert(!GuLoadedPeTextRange(b,63,&rva,&count));
  assert(!GuLoadedPeTextRange(b,sizeof(b),NULL,&count));
  assert(!GuLoadedPeTextRange(b,sizeof(b),&rva,NULL));
  b[0]=0;assert(!GuLoadedPeTextRange(b,sizeof(b),&rva,&count));setup(b);
  wr32(b,0x3c,0xfffffff0U);
  assert(!GuLoadedPeTextRange(b,sizeof(b),&rva,&count));setup(b);
  wr16(b,0x86,97);
  assert(!GuLoadedPeTextRange(b,sizeof(b),&rva,&count));setup(b);
  wr16(b,0x94,20);
  assert(!GuLoadedPeTextRange(b,sizeof(b),&rva,&count));setup(b);
  wr16(b,0x98,0x10b);
  assert(!GuLoadedPeTextRange(b,sizeof(b),&rva,&count));setup(b);
  wr32(b,0x98+56,0x1000);
  assert(!GuLoadedPeTextRange(b,sizeof(b),&rva,&count));setup(b);
  wr32(b,0x188+8,0);
  assert(!GuLoadedPeTextRange(b,sizeof(b),&rva,&count));setup(b);
  wr32(b,0x188+12,1008);
  assert(!GuLoadedPeTextRange(b,sizeof(b),&rva,&count));setup(b);
  wr32(b,0x188+36,0);
  assert(!GuLoadedPeTextRange(b,sizeof(b),&rva,&count));setup(b);
  wr16(b,0x86,2);memcpy(b+0x188+40,".text",5);
  assert(!GuLoadedPeTextRange(b,sizeof(b),&rva,&count));setup(b);
  memcpy(b+0x188,".data",5);
  assert(!GuLoadedPeTextRange(b,sizeof(b),&rva,&count));setup(b);
  /* Valid header, truncated actual section bytes must never be read. */
  assert(!GuLoadedPeTextRange(b,0x190,&rva,&count));
  puts("PASS loaded PE32+ mapped .text bounds/negative cases");
  return 0;
}
