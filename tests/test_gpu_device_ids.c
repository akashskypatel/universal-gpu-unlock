/* SPDX-License-Identifier: GPL-2.0-only */
/* Independently pinned parent-supported device list. */
#include "../src/vendor/prep_610.h"
#include <stdio.h>
#include <stddef.h>
static const u16 ada[]={0x2684,0x2685,0x2702,0x2704,0x2703,0x2705,0x2782,0x2783,0x2786,0x2860,0x2882};
static const u16 ampere[]={0x2203,0x2204,0x2205,0x2206,0x2208,0x220a,0x220b,0x2216,0x2414,0x2207,0x2482,0x2484,0x2486,0x2487,0x2488,0x2489,0x2501,0x2503,0x2504,0x2507,0x2508,0x2582,0x2520,0x2560};
static const u16 turing[]={0x1e04,0x1e07,0x1e02,0x1e09,0x1e30,0x1e35,0x1e81,0x1e82,0x1e87,0x1e84,0x1eb0,0x1eb1,0x1f02,0x1f07,0x1f06,0x1f47,0x1f03,0x1f08,0x1f12,0x1f36};
static int contains(u16 id,const u16 *v,size_t count){for(size_t i=0;i<count;i++)if(v[i]==id)return 1;return 0;}
#define COUNT(a) (sizeof(a)/sizeof((a)[0]))
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void) {
  for(unsigned id=0;id<=65535U;id++){
    int a=contains((u16)id,ada,COUNT(ada));
    int b=contains((u16)id,ampere,COUNT(ampere));
    int c=contains((u16)id,turing,COUNT(turing));
    CHECK((int)gpu_is_ada_device((u16)id)==a);
    CHECK((int)gpu_is_ampere_device((u16)id)==b);
    CHECK((int)gpu_is_turing_device((u16)id)==c);
    CHECK((int)gpu_is_supported_device((u16)id)==(a||b||c));
  }
  printf("PASS parent device IDs: %zu Ada, %zu Ampere, %zu Turing\n",COUNT(ada),COUNT(ampere),COUNT(turing));
  return 0;
}
