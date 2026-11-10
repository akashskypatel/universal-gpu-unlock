/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include "../src/prom_identity.h"
static unsigned char rom[1048576];
static void expected(void) {
  for(size_t i=0;i<sizeof(rom);i++)rom[i]=0;
  rom[0]=0x55;rom[1]=0xaa;rom[0x18]=0x20;
  rom[0x20]='P';rom[0x21]='C';rom[0x22]='I';rom[0x23]='R';
  rom[0x24]=0xde;rom[0x25]=0x10;
  rom[0x26]=0x84;rom[0x27]=0x26;
}
int main(void) {
  expected(); assert(GuPromImageMatchesDevice(rom,sizeof(rom),0x2684u));
  rom[0x26]=0x0a;rom[0x27]=0x22;
  assert(GuPromImageMatchesDevice(rom,sizeof(rom),0x220au));
  assert(!GuPromImageMatchesDevice(rom,sizeof(rom),0x2684u));
  expected();assert(!GuPromImageMatchesDevice(rom,sizeof(rom)-1,0x2684u));
  assert(!GuPromImageMatchesDevice(NULL,sizeof(rom),0x2684u));
  rom[0]=0;assert(!GuPromImageMatchesDevice(rom,sizeof(rom),0x2684u));
  expected();rom[0x25]=0;assert(!GuPromImageMatchesDevice(rom,sizeof(rom),0x2684u));
  expected();rom[0x22]='X';assert(!GuPromImageMatchesDevice(rom,sizeof(rom),0x2684u));
  expected();rom[0x18]=0xff;rom[0x19]=0xff;
  assert(!GuPromImageMatchesDevice(rom,sizeof(rom),0x2684u));
  expected();rom[0]=0x56;rom[1]=0x4e;
  assert(GuPromImageMatchesDevice(rom,sizeof(rom),0x2684u));
  puts("STATUS=COMPLETE PASS=10 PROM_IDENTITY_NEGATIVE_AND_POSITIVE");
  return 0;
}
