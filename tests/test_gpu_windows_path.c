/* SPDX-License-Identifier: GPL-2.0-only */
#include "../src/gpu_windows_path.h"
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { checks++; if(!(c)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); return 1; } } while(0)

/* A fake BootOrder/Boot#### store for the A2 cross-check. Name matching is
 * exact, including the UTF-16 terminator, so a name the walk builds wrongly
 * reads as absent rather than silently matching. */
typedef struct {
  const char *name;
  const uint8_t *data;
  size_t bytes;
  int fails;
} VAR;

static const VAR *store;
static size_t store_count;

static void UseStore(const VAR *vars, size_t count) { store = vars; store_count = count; }

static int FakeRead(const uint8_t *name, size_t name_bytes, uint8_t *out, size_t *size) {
  for (size_t i = 0; i < store_count; i++) {
    size_t want = 0;
    int same = 1;
    while (store[i].name[want]) want++;
    if (name_bytes != (want + 1) * 2) continue;
    for (size_t c = 0; c <= want; c++) {
      if (name[2 * c] != (uint8_t)store[i].name[c] || name[2 * c + 1] != 0) {
        same = 0;
        break;
      }
    }
    if (!same) continue;
    if (store[i].fails) return -1;
    if (store[i].bytes > *size) { *size = store[i].bytes; return -1; }
    memcpy(out, store[i].data, store[i].bytes);
    *size = store[i].bytes;
    return 1;
  }
  return 0;
}

/* An EFI_LOAD_OPTION whose file-path list is one volume prefix. */
static size_t Option(uint8_t *out, size_t cap, const uint8_t *volume, size_t volume_bytes) {
  const size_t desc_end = 6 + 2 * 8;   /* "Windows" and a terminator */
  if (!out || cap < desc_end + volume_bytes || volume_bytes > 0xffffU) return 0;
  memset(out, 0, cap);
  out[0] = 1;                          /* LOAD_OPTION_ACTIVE */
  out[4] = (uint8_t)(volume_bytes & 0xff);
  out[5] = (uint8_t)(volume_bytes >> 8);
  memcpy(out + 6, "W\0i\0n\0d\0o\0w\0s\0\0\0", desc_end - 6);
  memcpy(out + desc_end, volume, volume_bytes);
  return desc_end + volume_bytes;
}

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
  /* The runtime policy's structural test accepts the same shape without a
   * pin, and rejects everything the pinned test rejects structurally. */
  CHECK(GuWindowsEspPathCandidate(path,58));
  {
    uint8_t got[16];
    CHECK(GuWindowsEspPathGuid(path,58,got));
    CHECK(!memcmp(got,guid,sizeof(got)));
    CHECK(!GuWindowsEspPathGuid(path,57,got));
    CHECK(!GuWindowsEspPathGuid(path,58,NULL));
    memset(path+12+24,0,16);
    CHECK(!GuWindowsEspPathGuid(path,58,got));
    memcpy(path,save,58);
  }
  CHECK(GuWindowsEspPathCandidate(path,sizeof(path)));
  CHECK(!GuWindowsEspPathCandidate(path,57));
  CHECK(!GuWindowsEspPathCandidate(NULL,58));
  CHECK(!GuWindowsEspPathCandidate(path,4097));
  path[12+40]=1;CHECK(!GuWindowsEspPathCandidate(path,58));memcpy(path,save,58);
  path[12+41]=1;CHECK(!GuWindowsEspPathCandidate(path,58));memcpy(path,save,58);
  path[12+16]=0;CHECK(!GuWindowsEspPathCandidate(path,58));memcpy(path,save,58);
  path[12+2]=41;CHECK(!GuWindowsEspPathCandidate(path,58));memcpy(path,save,58);
  path[12+2]=43;CHECK(!GuWindowsEspPathCandidate(path,58));memcpy(path,save,58);
  /* A different partition on the same volume is still a valid candidate: the
   * pin, not the shape, is what the runtime policy drops. */
  path[12+24]^=0xff;CHECK(GuWindowsEspPathCandidate(path,58));memcpy(path,save,58);
  /* A repeated HD node on the same path is ambiguous, even if both match. */
  memmove(path+54,path+12,42);
  path[96]=0x7f;path[97]=0xff;path[98]=4;path[99]=0;
  CHECK(!GuWindowsEspPathMatches(path,100,guid));
  CHECK(GuWindowsPathLength(path,100)==100);
  memcpy(path,save,sizeof(path));

  /* A2: a Boot#### load option names a volume by its device-node prefix. The
   * description is UTF-16 and must be terminated; the file-path list follows
   * it and its declared length is the only authority for how much to read. */
  {
    uint8_t option[160];
    uint8_t bootmgfw[64];
    const size_t desc_units=21;          /* 20 characters and the terminator */
    const size_t desc_end=6+2*desc_units;/* 48 */
    size_t filepath;
    memset(option,0,sizeof(option));
    memset(bootmgfw,0,sizeof(bootmgfw));
    /* media FilePath node: type 4, subtype 4, length, then UTF-16 text */
    bootmgfw[0]=4;bootmgfw[1]=4;bootmgfw[2]=40;bootmgfw[3]=0;
    for(size_t i=0;i<17;i++){bootmgfw[4+2*i]=(uint8_t)"\\EFI\\Microsoft\\Boot"[i];}
    memcpy(option+desc_end,path,58);
    memcpy(option+desc_end+58,bootmgfw,40);
    filepath=58+40;
    option[0]=1;                          /* LOAD_OPTION_ACTIVE */
    option[4]=(uint8_t)(filepath&0xff);option[5]=(uint8_t)(filepath>>8);
    for(size_t i=0;i<20;i++){option[6+2*i]=(uint8_t)"Windows Boot Manager"[i];}
    CHECK(GuBootOptionNamesDevicePath(option,desc_end+filepath,path,58));
    /* the declared length, not the buffer, is what is compared */
    CHECK(!GuBootOptionNamesDevicePath(option,desc_end+filepath-1,path,58));
    /* an option that ends at the partition names it too */
    option[4]=(uint8_t)(58&0xff);option[5]=(uint8_t)(58>>8);
    CHECK(GuBootOptionNamesDevicePath(option,desc_end+58,path,58));
    /* a different partition on the same controller is a different volume */
    path[12+24]^=0xff;
    CHECK(!GuBootOptionNamesDevicePath(option,desc_end+58,path,58));
    memcpy(path,save,58);
    /* a longer prefix is not the same volume unless both sides carry it: the
     * same disk reached through an extra controller node is somewhere else */
    {
      uint8_t longer[176], deeper[80];
      const size_t extra=12;              /* a 12-byte controller/vendor node */
      memset(longer,0,sizeof(longer));
      memset(deeper,0,sizeof(deeper));
      memcpy(longer,option,desc_end);
      memcpy(longer+desc_end,path,54);    /* the volume, without the end node */
      longer[desc_end+54]=1;longer[desc_end+55]=3;
      longer[desc_end+56]=12;longer[desc_end+57]=0;
      memcpy(longer+desc_end+54+extra,bootmgfw,40);
      longer[0]=1;
      longer[4]=(uint8_t)((54+extra+40)&0xff);longer[5]=0;
      memcpy(deeper,path,54);
      deeper[54]=1;deeper[55]=3;deeper[56]=12;deeper[57]=0;
      deeper[54+extra]=0x7f;deeper[55+extra]=0xff;
      deeper[56+extra]=4;deeper[57+extra]=0;
      CHECK(GuBootOptionNamesDevicePath(longer,desc_end+54+extra+40,deeper,54+extra+4));
      CHECK(!GuBootOptionNamesDevicePath(longer,desc_end+54+extra+40,path,58));
    }
    option[4]=(uint8_t)(filepath&0xff);option[5]=(uint8_t)(filepath>>8);
    /* an unterminated description cannot be parsed */
    for(size_t i=0;i<desc_units;i++){option[6+2*i]='X';option[7+2*i]=0;}
    CHECK(!GuBootOptionNamesDevicePath(option,desc_end+filepath,path,58));
    option[6+2*(desc_units-1)]=0;option[7+2*(desc_units-1)]=0;
    /* a file-path length that runs past the blob is refused */
    option[4]=0xff;option[5]=0x00;
    CHECK(!GuBootOptionNamesDevicePath(option,desc_end+filepath,path,58));
    /* a length below the smallest device path is refused */
    option[4]=3;option[5]=0;
    CHECK(!GuBootOptionNamesDevicePath(option,desc_end+filepath,path,58));
    /* truncation, null arguments, and an oversized bound are refused */
    CHECK(!GuBootOptionNamesDevicePath(option,7,path,58));
    CHECK(!GuBootOptionNamesDevicePath(NULL,64,path,58));
    CHECK(!GuBootOptionNamesDevicePath(option,64,NULL,58));
    CHECK(!GuBootOptionNamesDevicePath(option,4097,path,58));
    /* a malformed node in the option's prefix is not a match */
    option[4]=(uint8_t)(filepath&0xff);option[5]=(uint8_t)(filepath>>8);
    option[desc_end+12+2]=2;    /* the hard-drive node's length is below 4 */
    CHECK(!GuBootOptionNamesDevicePath(option,desc_end+filepath,path,58));
  }
  /* A2: BootOrder is what the firmware will try. The walk is driven here with a
   * fake variable store, so every refusal below is the real code's. */
  {
    uint8_t order[8], order7[2], other[58], opt0[96], opt7[96];
    size_t opt0_bytes=0, opt7_bytes=0;
    memcpy(other,path,58);
    other[12+24]=0x11;other[12+25]=0x22;      /* a different partition */
    opt0_bytes=Option(opt0,sizeof(opt0),other,54);
    opt7_bytes=Option(opt7,sizeof(opt7),path,54);
    order[0]=0;order[1]=0;order[2]=7;order[3]=0;   /* BootOrder = {0000, 0007} */
    order7[0]=7;order7[1]=0;                       /* BootOrder = {0007} */
    /* one named option points at this volume */
    {
      VAR vars[]={{"BootOrder",order,4,0},{"Boot0000",opt0,opt0_bytes,0},
                  {"Boot0007",opt7,opt7_bytes,0}};
      UseStore(vars,3);
      CHECK(GuBootOrderNamesVolume(FakeRead,path,58)==GU_ORDER_NAMES);
    }
    /* readable, and nothing points here */
    {
      VAR vars[]={{"BootOrder",order,2,0},{"Boot0000",opt0,opt0_bytes,0},
                  {"Boot0007",opt7,opt7_bytes,0}};
      UseStore(vars,3);
      CHECK(GuBootOrderNamesVolume(FakeRead,path,58)==GU_ORDER_NONE);
    }
    /* an option named but absent is skipped, not guessed */
    {
      VAR vars[]={{"BootOrder",order,2,0},{"Boot0007",opt7,opt7_bytes,0}};
      UseStore(vars,2);
      CHECK(GuBootOrderNamesVolume(FakeRead,path,58)==GU_ORDER_NONE);
    }
    /* an option that cannot be read is never a match */
    {
      VAR vars[]={{"BootOrder",order,2,0},{"Boot0000",opt0,opt0_bytes,1},
                  {"Boot0007",opt7,opt7_bytes,0}};
      UseStore(vars,3);
      CHECK(GuBootOrderNamesVolume(FakeRead,path,58)==GU_ORDER_UNREADABLE);
    }
    /* no BootOrder at all: unchecked, not "unmatched" */
    {
      VAR vars[]={{"Boot0007",opt7,opt7_bytes,0}};
      UseStore(vars,1);
      CHECK(GuBootOrderNamesVolume(FakeRead,path,58)==GU_ORDER_UNREADABLE);
    }
    /* an option larger than the bound is unchecked too */
    {
      VAR vars[]={{"BootOrder",order7,2,0},{"Boot0007",opt7,4096,0}};
      UseStore(vars,2);
      CHECK(GuBootOrderNamesVolume(FakeRead,path,58)==GU_ORDER_UNREADABLE);
    }
    /* an odd-sized BootOrder is corrupt */
    {
      VAR vars[]={{"BootOrder",order7,1,0},{"Boot0007",opt7,opt7_bytes,0}};
      UseStore(vars,2);
      CHECK(GuBootOrderNamesVolume(FakeRead,path,58)==GU_ORDER_UNREADABLE);
    }
    /* unusable arguments are unreadable, never a pass */
    {
      VAR vars[]={{"BootOrder",order,4,0},{"Boot0007",opt7,opt7_bytes,0}};
      UseStore(vars,2);
      CHECK(GuBootOrderNamesVolume(NULL,path,58)==GU_ORDER_UNREADABLE);
      CHECK(GuBootOrderNamesVolume(FakeRead,NULL,58)==GU_ORDER_UNREADABLE);
      CHECK(GuBootOrderNamesVolume(FakeRead,path,0)==GU_ORDER_UNREADABLE);
    }
    UseStore(NULL,0);
  }
  puts("PASS pinned Windows ESP device path negatives; no EFI/hardware calls");
  printf("CHECKS=%u\n",checks);
  return 0;
}
