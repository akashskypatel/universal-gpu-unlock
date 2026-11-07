/* SPDX-License-Identifier: GPL-2.0-only */
/* Host-only EFI mocks: exercise the handoff's fail-closed gating. */
#include "gpu_windows_handoff_mock.h"
#include "../src/gpu_windows_handoff.h"
#include "../src/gpu_windows_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { checks++; if(!(c)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); return 1; } } while(0)
static EFI_HANDLE own=(EFI_HANDLE)(uintptr_t)0x200,volume=(EFI_HANDLE)(uintptr_t)0x300;
static EFI_HANDLE child=(EFI_HANDLE)(uintptr_t)0x400,image=(EFI_HANDLE)(uintptr_t)0x100;
static EFI_HANDLE duplicate=(EFI_HANDLE)(uintptr_t)0x500;
static EFI_LOADED_IMAGE_PROTOCOL ownImg,winImg;
static uint8_t base[80],different[80];
static const uint8_t guid[16]={0x8c,0x85,0x05,0xa0,0x87,0x44,0xe2,0x4e,
                                0x97,0x6e,0x17,0x10,0xff,0x48,0x4c,0x31};
static int mismatch,tooMany,wrongChild,loadFailure,freeFailure;
static unsigned loadCalls,startCalls,freeCalls,allocCalls;

static EFI_STATUS locate(int search,EFI_GUID*g,VOID*key,UINTN*count,EFI_HANDLE**out) {
  (void)search;(void)g;(void)key;
  *count=tooMany?3:2;
  *out=(EFI_HANDLE *)malloc(*count*sizeof(EFI_HANDLE));
  if(!*out)return EFI_OUT_OF_RESOURCES;
  (*out)[0]=own;(*out)[1]=volume;
  if(tooMany)(*out)[2]=duplicate;
  return EFI_SUCCESS;
}
static EFI_STATUS protocol(EFI_HANDLE h,EFI_GUID *g,VOID **out) {
  if(g->Data1==1) {
    if(h==image){ownImg.DeviceHandle=own;*out=&ownImg;return EFI_SUCCESS;}
    if(h==child){winImg.DeviceHandle=wrongChild?own:volume;*out=&winImg;return EFI_SUCCESS;}
  }
  if(g->Data1==15) {
    if(h==volume){*out=mismatch?different:base;return EFI_SUCCESS;}
    if(h==duplicate){*out=base;return EFI_SUCCESS;}
  }
  return EFI_NOT_FOUND;
}
static EFI_STATUS free_pool(VOID*ptr){freeCalls++;free(ptr);return freeFailure?EFI_ACCESS_DENIED:EFI_SUCCESS;}
static EFI_STATUS alloc(int type,UINTN n,VOID **out){(void)type;allocCalls++;*out=malloc((size_t)n);return *out?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;}
static EFI_STATUS load(BOOLEAN policy,EFI_HANDLE parent,EFI_DEVICE_PATH_PROTOCOL *p,VOID*src,UINTN len,EFI_HANDLE*out){
  (void)src;(void)len;
  static const CHAR16 expected[]=L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
  const uint8_t *bytes=(const uint8_t *)p;
  size_t n=GuWindowsPathLength(bytes,512),at=0;
  unsigned filepath=0;
  if(policy || parent!=image || !p || !n ||
     !GuWindowsEspPathMatches(bytes,n,guid))return EFI_COMPROMISED_DATA;
  while(at+4<=n) {
    size_t node=(size_t)bytes[at+2] | ((size_t)bytes[at+3]<<8);
    if(bytes[at]==MEDIA_DEVICE_PATH && bytes[at+1]==MEDIA_FILEPATH_DP) {
      if(node!=4+sizeof(expected) || memcmp(bytes+at+4,expected,sizeof(expected)))
        return EFI_COMPROMISED_DATA;
      filepath++;
    }
    at+=node;
  }
  if(filepath!=1)return EFI_COMPROMISED_DATA;
  loadCalls++;
  if(loadFailure)return EFI_ACCESS_DENIED;
  *out=child;return EFI_SUCCESS;
}
static EFI_STATUS start(EFI_HANDLE h,UINTN*len,CHAR16**text){
  (void)len;(void)text;
  if(h!=child)return EFI_INVALID_PARAMETER;
  startCalls++;return EFI_ACCESS_DENIED; /* Child returns: no automatic retry. */
}

int main(void){
  EFI_BOOT_SERVICES bs={protocol,locate,free_pool,alloc,load,start};
  EFI_SYSTEM_TABLE t={&bs};
  memset(base,0,sizeof(base));
  base[0]=2;base[1]=1;base[2]=12;
  base[12]=4;base[13]=1;base[14]=42;
  memcpy(base+12+24,guid,16);base[12+16]=1;base[12+40]=2;base[12+41]=2;
  base[54]=0x7f;base[55]=0xff;base[56]=4;
  memcpy(different,base,sizeof(base));different[36]^=1;
  CHECK(GuWindowsBootManager(image,&t)==EFI_ACCESS_DENIED);
  CHECK(loadCalls==1 && startCalls==1 && allocCalls==1);
  CHECK(GuWindowsBootManager(NULL,&t)==EFI_INVALID_PARAMETER);
  CHECK(startCalls==1);
  mismatch=1;
  CHECK(GuWindowsBootManager(image,&t)==EFI_NOT_FOUND);
  CHECK(loadCalls==1 && startCalls==1);
  mismatch=0;tooMany=1;
  CHECK(GuWindowsBootManager(image,&t)==EFI_COMPROMISED_DATA);
  CHECK(loadCalls==1 && startCalls==1);
  tooMany=0;wrongChild=1;
  CHECK(GuWindowsBootManager(image,&t)==EFI_COMPROMISED_DATA);
  CHECK(startCalls==1 && loadCalls==2);
  wrongChild=0;loadFailure=1;
  CHECK(GuWindowsBootManager(image,&t)==EFI_ACCESS_DENIED);
  CHECK(startCalls==1 && loadCalls==3);
  loadFailure=0;freeFailure=1;
  CHECK(GuWindowsBootManager(image,&t)==EFI_ACCESS_DENIED);
  CHECK(startCalls==1 && loadCalls==3);
  freeFailure=0;
  bs.LoadImage=NULL;
  CHECK(GuWindowsBootManager(image,&t)==EFI_UNSUPPORTED);
  CHECK(startCalls==1);
  CHECK(freeCalls>0);
  puts("PASS pinned Windows handoff host mocks; no EFI/hardware calls");
  printf("CHECKS=%u\n",checks);
  return 0;
}
