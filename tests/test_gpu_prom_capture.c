/* SPDX-License-Identifier: GPL-2.0-only */
/* Independent host evidence for the legacy 1-MiB PROM read-loop port.
 * No UEFI services, BAR, GPU or firmware are accessed. */
#include "../src/gpu_prom_capture.h"
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { checks++; if(!(x)) { \
  fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1; \
} } while(0)

typedef struct {
  size_t reads;
  uint32_t last;
  size_t fail_at;
  unsigned bad_offset;
} MOCK;
static uint8_t output[GU_PROM_CAPTURE_BYTES];
/* Union forces alignment so the reader record can be deliberately placed
 * inside the output window for the isolation negative. */
static union {
  uint8_t bytes[GU_PROM_CAPTURE_BYTES];
  GU_PROM_READER reader;
  MOCK mock;
} overlapping;
static int Read(void *opaque, uint32_t offset, uint32_t *out) {
  MOCK *m=(MOCK *)opaque;
  if(!m || !out)return 0;
  if(offset!=GU_PROM_CAPTURE_BAR0_OFFSET + (uint32_t)m->reads*4U)
    m->bad_offset=1;
  if(m->reads++==m->fail_at)return 0;
  *out=offset ^ 0x89abcdefU;
  m->last=offset;
  return 1;
}
static int Cleared(void) {
  size_t i;
  for(i=0;i<sizeof(output);i++)if(output[i])return 0;
  return 1;
}
static int Untouched(size_t from,size_t to,uint8_t value) {
  size_t i;
  for(i=from;i<to;i++)if(overlapping.bytes[i]!=value)return 0;
  return 1;
}
int main(void) {
  MOCK mock={0};
  GU_PROM_READER reader={&mock,Read};
  uint32_t first=GU_PROM_CAPTURE_BAR0_OFFSET ^ 0x89abcdefU;
  uint32_t last=(GU_PROM_CAPTURE_BAR0_OFFSET+GU_PROM_CAPTURE_BYTES-4U)
                ^ 0x89abcdefU;
  mock.fail_at=(size_t)-1;
  CHECK(!GuPromCapture(NULL,output,sizeof(output)));
  CHECK(Cleared());
  CHECK(!GuPromCapture(&reader,NULL,sizeof(output)));
  CHECK(mock.reads==0);
  CHECK(!GuPromCapture(&reader,output,sizeof(output)-1U));
  CHECK(mock.reads==0);
  CHECK(GuPromCapture(&reader,output,sizeof(output)));
  CHECK(mock.reads==GU_PROM_CAPTURE_BYTES/4U && !mock.bad_offset);
  CHECK(mock.last==GU_PROM_CAPTURE_BAR0_OFFSET+GU_PROM_CAPTURE_BYTES-4U);
  CHECK(output[0]==(uint8_t)first && output[1]==(uint8_t)(first>>8));
  CHECK(output[2]==(uint8_t)(first>>16) && output[3]==(uint8_t)(first>>24));
  CHECK(output[sizeof(output)-4U]==(uint8_t)last &&
        output[sizeof(output)-1U]==(uint8_t)(last>>24));
  mock.reads=0;mock.fail_at=GU_PROM_CAPTURE_BYTES/8U;
  CHECK(!GuPromCapture(&reader,output,sizeof(output)));
  CHECK(mock.reads==mock.fail_at+1U && Cleared());
  mock.reads=0;mock.fail_at=0;
  CHECK(!GuPromCapture(&reader,output,sizeof(output)));
  CHECK(mock.reads==1 && Cleared());
  mock.reads=0;mock.fail_at=(size_t)-1;
  reader.read32=NULL;
  CHECK(!GuPromCapture(&reader,output,sizeof(output)));
  CHECK(mock.reads==0 && Cleared());

  /* The capture owns the whole output window. A reader record inside it must
   * be refused before the first write, not cleared and then repaired. */
  {
    GU_PROM_READER *inside=(GU_PROM_READER *)(void *)(overlapping.bytes+
        sizeof(overlapping.bytes)-sizeof(GU_PROM_READER));
    size_t before=sizeof(overlapping.bytes)-sizeof(GU_PROM_READER);
    memset(&overlapping,0xa5,sizeof(overlapping));
    inside->context=&mock;
    inside->read32=Read;
    mock.reads=0;
    CHECK(!GuPromCapture(inside,overlapping.bytes,sizeof(overlapping.bytes)));
    CHECK(mock.reads==0);
    CHECK(inside->context==&mock && inside->read32==Read);
    CHECK(Untouched(0,before,0xa5));
  }


  /* A caller context that points into the output window is equally unusable:
   * the capture would overwrite the state the reader depends on. */
  {
    struct { uint8_t bytes[GU_PROM_CAPTURE_BYTES]; MOCK mock; } live;
    GU_PROM_READER aliased;
    memset(&live,0xa5,sizeof(live));
    memset(&live.mock,0,sizeof(live.mock));
    live.mock.fail_at=(size_t)-1;
    aliased.context=&live.bytes[4096];
    aliased.read32=Read;
    CHECK(!GuPromCapture(&aliased,live.bytes,sizeof(live.bytes)));
    CHECK(live.mock.reads==0 && live.bytes[0]==0xa5 &&
          live.bytes[sizeof(live.bytes)-1U]==0xa5);
    /* The same context outside the window is a valid pairing. */
    aliased.context=&live.mock;
    CHECK(GuPromCapture(&aliased,live.bytes,sizeof(live.bytes)));
    CHECK(live.mock.reads==GU_PROM_CAPTURE_BYTES/4U);
  }

  /* Invalid arguments never publish or modify the caller's output. */
  memset(output,0xa5,sizeof(output));
  CHECK(!GuPromCapture(NULL,output,sizeof(output)));
  CHECK(output[0]==0xa5 && output[sizeof(output)-1U]==0xa5);
  CHECK(!GuPromCapture(&reader,NULL,sizeof(output)));
  memset(output,0,sizeof(output));
  printf("PASS GPU PROM capture: %u checks; exact 1MiB read loop, no hardware\n",checks);
  return 0;
}
