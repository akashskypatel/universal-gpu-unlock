/* SPDX-License-Identifier: GPL-2.0-only */
#include "../src/gpu_windows_path.h"
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { checks++; if(!(c)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); return 1; } } while(0)

int main(void) {
  static const uint8_t guid[16]={0x8c,0x85,0x05,0xa0,0x87,0x44,0xe2,0x4e,
                                  0x97,0x6e,0x17,0x10,0xff,0x48,0x4c,0x31};
  /* ACPI root node, media HD (42 bytes), END_ENTIRE. */
  uint8_t path[120]={2,1,12,0,0,0,0,0,0,0,0,0,4,1,42,0};
  uint8_t save[120];
  memcpy(path+12+24,guid,16);
  path[12+16]=1; path[12+40]=2;path[12+41]=2;
  path[54]=0x7f;path[55]=0xff;path[56]=4;
  memcpy(save,path,sizeof(path));
  CHECK(GuWindowsPathLength(path,58)==58);
  CHECK(GuWindowsEspPathMatches(path,58,guid));
  CHECK(GuWindowsEspPathMatches(path,sizeof(path),guid));
  CHECK(!GuWindowsEspPathMatches(path,57,guid));
  CHECK(!GuWindowsEspPathMatches(path,3,guid));
  CHECK(!GuWindowsEspPathMatches(NULL,58,guid));
  CHECK(!GuWindowsEspPathMatches(path,58,NULL));
  CHECK(!GuWindowsEspPathMatches(path,4097,guid));
  path[12+24]^=0xff;CHECK(!GuWindowsEspPathMatches(path,58,guid));memcpy(path,save,58);
  path[12+40]=1;CHECK(!GuWindowsEspPathMatches(path,58,guid));memcpy(path,save,58);
  path[12+41]=1;CHECK(!GuWindowsEspPathMatches(path,58,guid));memcpy(path,save,58);
  path[12+16]=0;CHECK(!GuWindowsEspPathMatches(path,58,guid));memcpy(path,save,58);
  path[12+1]=4;CHECK(!GuWindowsEspPathMatches(path,58,guid));memcpy(path,save,58);
  path[12+2]=41;CHECK(!GuWindowsEspPathMatches(path,58,guid));memcpy(path,save,58);
  path[12+2]=43;CHECK(!GuWindowsEspPathMatches(path,58,guid));memcpy(path,save,58);
  path[55]=1;CHECK(GuWindowsPathLength(path,58)==0);memcpy(path,save,58);
  path[56]=5;CHECK(GuWindowsPathLength(path,58)==0);memcpy(path,save,58);
  path[0]=0x7f;CHECK(GuWindowsPathLength(path,58)==0);memcpy(path,save,58);
  path[2]=2;CHECK(GuWindowsPathLength(path,58)==0);memcpy(path,save,58);
  /* A repeated HD node on the same path is ambiguous, even if both match. */
  memmove(path+54,path+12,42);
  path[96]=0x7f;path[97]=0xff;path[98]=4;path[99]=0;
  CHECK(!GuWindowsEspPathMatches(path,100,guid));
  CHECK(GuWindowsPathLength(path,100)==100);
  memcpy(path,save,sizeof(path));
  puts("PASS pinned Windows ESP device path negatives; no EFI/hardware calls");
  printf("CHECKS=%u\n",checks);
  return 0;
}
