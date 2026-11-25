/* SPDX-License-Identifier: GPL-2.0-only */
/* Host evidence for the captured-ROM file name: src/gpu_topology_efi.c prints
 * it into the log and hands the same string to EFI_FILE_PROTOCOL::Open, and
 * src/gpu_prom_capture_efi.c would write the prepared capture under it, so the
 * format is tested once here instead of in each writer. No UEFI service, no
 * card and no file is touched. */
#include "../src/prom_filename.h"
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { checks++; if(!(x)) { \
  fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1; \
} } while(0)

/* The two cards on the bench, and the run indexes a stick accumulates. */
static const char *expect(unsigned segment, unsigned bus, unsigned device,
                          unsigned function, unsigned run) {
  static char text[GU_PROM_FILE_CHARS];
  static const char digits[]="0123456789";
  size_t n=0;
  text[n++]='\\';text[n++]='P';text[n++]='R';text[n++]='O';text[n++]='M';
  text[n++]='-';
  text[n++]= "0123456789ABCDEF"[(segment>>12)&15U];
  text[n++]= "0123456789ABCDEF"[(segment>>8)&15U];
  text[n++]= "0123456789ABCDEF"[(segment>>4)&15U];
  text[n++]= "0123456789ABCDEF"[segment&15U];
  text[n++]='-';
  text[n++]= "0123456789ABCDEF"[(bus>>4)&15U];
  text[n++]= "0123456789ABCDEF"[bus&15U];
  text[n++]='-';
  text[n++]= "0123456789ABCDEF"[(device>>4)&15U];
  text[n++]= "0123456789ABCDEF"[device&15U];
  text[n++]='-';
  text[n++]=(char)('0'+function);
  text[n++]='-';
  text[n++]=digits[run/1000U];
  text[n++]=digits[(run/100U)%10U];
  text[n++]=digits[(run/10U)%10U];
  text[n++]=digits[run%10U];
  text[n++]='.';text[n++]='B';text[n++]='I';text[n++]='N';
  text[n]=0;
  return text;
}

int main(void) {
  GU_PROM_CHAR wide[GU_PROM_FILE_CHARS];
  GU_PROM_CHAR untouched[GU_PROM_FILE_CHARS];
  char text[GU_PROM_FILE_CHARS];
  char text_untouched[GU_PROM_FILE_CHARS];
  const char *want;
  size_t i, written;

  /* The two cards of the bench, first capture and a later one. */
  want=expect(0U,1U,0U,0U,0U);
  CHECK(GuPromFileName(wide,GU_PROM_FILE_CHARS,0U,1U,0U,0U,0U)==27U);
  CHECK(want[0]=='\\' && strcmp(want,"\\PROM-0000-01-00-0-0000.BIN")==0);
  for(i=0;i<=27U;i++)CHECK(wide[i]==(GU_PROM_CHAR)want[i]);
  CHECK(GuPromFileText(text,GU_PROM_FILE_CHARS,0U,1U,0U,0U,0U)==27U);
  CHECK(strcmp(text,want)==0);

  want=expect(0U,4U,0U,0U,1U);
  CHECK(GuPromFileText(text,GU_PROM_FILE_CHARS,0U,4U,0U,0U,1U)==27U);
  CHECK(strcmp(text,"\\PROM-0000-04-00-0-0001.BIN")==0);
  CHECK(strcmp(text,want)==0);

  /* A second capture of the same card is a different name: this is the whole
   * reason the run index exists, since a fixed name would make the second boot
   * refuse to capture at all. */
  CHECK(GuPromFileText(text,GU_PROM_FILE_CHARS,0U,1U,0U,0U,1U)==27U);
  CHECK(strcmp(text,"\\PROM-0000-01-00-0-0001.BIN")==0);
  CHECK(strcmp(expect(0U,1U,0U,0U,0U),text)!=0);

  /* Field boundaries are rendered, not truncated into each other. */
  CHECK(GuPromFileText(text,GU_PROM_FILE_CHARS,0xffffU,0xffU,0x1fU,7U,9999U)==27U);
  CHECK(strcmp(text,"\\PROM-FFFF-FF-1F-7-9999.BIN")==0);
  CHECK(GuPromFileText(text,GU_PROM_FILE_CHARS,0U,0U,0U,0U,0U)==27U);
  CHECK(strcmp(text,"\\PROM-0000-00-00-0-0000.BIN")==0);
  /* Every digit position of the run index is exercised, including the carry. */
  CHECK(GuPromFileText(text,GU_PROM_FILE_CHARS,0U,1U,0U,0U,10U)==27U);
  CHECK(strcmp(text,"\\PROM-0000-01-00-0-0010.BIN")==0);
  CHECK(GuPromFileText(text,GU_PROM_FILE_CHARS,0U,1U,0U,0U,999U)==27U);
  CHECK(strcmp(text,"\\PROM-0000-01-00-0-0999.BIN")==0);

  /* A name that does not fit in the address is refused, and nothing is written
   * into the caller's buffer on refusal. */
  /* memset on a 16-bit element writes 0xa5a5, not 0xa5: fill explicitly so the
   * sentinel is one value in both buffers. */
  for(i=0;i<GU_PROM_FILE_CHARS;i++) {
    untouched[i]=0xa5a5U;
    text_untouched[i]=(char)0xa5;
  }
  CHECK(GuPromFileName(untouched,GU_PROM_FILE_CHARS,0x10000U,1U,0U,0U,0U)==0U);
  CHECK(GuPromFileName(untouched,GU_PROM_FILE_CHARS,0U,0x100U,0U,0U,0U)==0U);
  CHECK(GuPromFileName(untouched,GU_PROM_FILE_CHARS,0U,1U,0x20U,0U,0U)==0U);
  CHECK(GuPromFileName(untouched,GU_PROM_FILE_CHARS,0U,1U,0U,8U,0U)==0U);
  CHECK(GuPromFileName(untouched,GU_PROM_FILE_CHARS,0U,1U,0U,0U,10000U)==0U);
  CHECK(GuPromFileText(text_untouched,GU_PROM_FILE_CHARS,0U,1U,0U,0U,10000U)==0U);
  /* A buffer one character short is refused before the first write. */
  CHECK(GuPromFileName(untouched,GU_PROM_FILE_CHARS-1U,0U,1U,0U,0U,0U)==0U);
  CHECK(GuPromFileText(text_untouched,GU_PROM_FILE_CHARS-1U,0U,1U,0U,0U,0U)==0U);
  CHECK(GuPromFileName(NULL,GU_PROM_FILE_CHARS,0U,1U,0U,0U,0U)==0U);
  CHECK(GuPromFileText(NULL,GU_PROM_FILE_CHARS,0U,1U,0U,0U,0U)==0U);
  for(i=0;i<GU_PROM_FILE_CHARS;i++) {
    CHECK(untouched[i]==0xa5a5U && text_untouched[i]==(char)0xa5);
  }

  /* The two writers must agree character for character, so the log names the
   * file the file system was asked to create. */
  for(i=0;i<4U;i++) {
    unsigned bus=i?4U:1U;
    CHECK(GuPromFileName(wide,GU_PROM_FILE_CHARS,0U,bus,0U,0U,(unsigned)i)==27U);
    written=GuPromFileText(text,GU_PROM_FILE_CHARS,0U,bus,0U,0U,(unsigned)i);
    CHECK(written==27U);
    CHECK(strcmp(text,expect(0U,bus,0U,0U,(unsigned)i))==0);
    /* Element-wise equality without narrowing the wide form through char. */
    {
      size_t k;
      for(k=0;k<=written;k++)CHECK(wide[k]==(GU_PROM_CHAR)text[k]);
    }
  }

  printf("PASS PROM capture file name: %u checks; one format, refused on "
         "overflow, no hardware\n",checks);
  return 0;
}
