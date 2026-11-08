/* SPDX-License-Identifier: GPL-2.0-only */
/* Host-only evidence: arbitrary NVIDIA identities and locations, no firmware. */
#include "../src/gpu_inventory.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL:%d: %s\n",__LINE__,#x);return 1;}}while(0)

typedef struct {
  GU_GPU_RECORD records[GU_INVENTORY_MAX_NVIDIA+2];
  size_t count, reads, fail_at;
} MOCK;

static int Reader(void *opaque,size_t i,GU_GPU_RECORD *out) {
  MOCK *m=(MOCK *)opaque;
  m->reads++;
  if(i>=m->count || i==m->fail_at)return -1;
  *out=m->records[i];
  return 0;
}

static GU_GPU_RECORD Gpu(uint32_t pci,uint32_t sub,uint32_t segment,uint32_t bus,
                         uint32_t device,uint32_t function,uintptr_t token) {
  GU_GPU_RECORD r;
  memset(&r,0,sizeof(r));
  r.pci_id=pci;r.subsystem_id=sub;r.class_code=0x0300;
  r.segment=segment;r.bus=bus;r.device=device;r.function=function;
  r.handle=(void *)(token*2+1);
  r.pci=(void *)(token*2+2);
  return r;
}
static void Setup(MOCK *m) {
  memset(m,0,sizeof(*m));m->fail_at=(size_t)-1;m->count=2;
  m->records[0]=Gpu(0x20b010deU,0x987610deU,0,3,0,0,1);
  m->records[1]=Gpu(0x250310deU,0x432110deU,1,7,4,1,2);
}
static int Refuse(MOCK *m,int expected) {
  GU_GPU_INVENTORY inv;
  int status=GuCollectGpuInventory(m->count,Reader,m,&inv);
  CHECK(status==expected);
  CHECK(inv.count==0);
  return 0;
}
int main(void) {
  MOCK m;
  GU_GPU_INVENTORY inv;
  GU_GPU_LOCATION location={0,3,0,0},other={1,7,4,1};
  GU_GPU_IDENTITY exact={0x250310deU,0x432110deU,1,7,4,1};
  GU_GPU_RECORD swap;
  size_t index=99,i;

  Setup(&m);
  CHECK(GuCollectGpuInventory(m.count,Reader,&m,&inv)==GU_INVENTORY_OK);
  CHECK(inv.count==2 && m.reads==2);
  CHECK(GuFindGpuAtLocation(&inv,&location,&index)==GU_INVENTORY_OK && index==0);
  CHECK(GuFindGpuAtLocation(&inv,&other,&index)==GU_INVENTORY_OK && index==1);
  CHECK(GuFindExactGpu(&inv,&exact,&index)==GU_INVENTORY_OK && index==1);

  swap=m.records[0];m.records[0]=m.records[1];m.records[1]=swap;
  CHECK(GuCollectGpuInventory(m.count,Reader,&m,&inv)==GU_INVENTORY_OK);
  CHECK(inv.count==2);
  CHECK(GuFindGpuAtLocation(&inv,&location,&index)==GU_INVENTORY_OK && index==1);
  CHECK(GuFindGpuAtLocation(&inv,&other,&index)==GU_INVENTORY_OK && index==0);

  Setup(&m);m.count=3;
  m.records[2]=Gpu(0x278210deU,0xabcdef01U,0,9,2,0,3);
  CHECK(GuCollectGpuInventory(m.count,Reader,&m,&inv)==GU_INVENTORY_OK);
  CHECK(inv.count==3);
  CHECK(GuFindGpuAtLocation(&inv,&other,&index)==GU_INVENTORY_OK && index==1);
  location.bus=9;location.device=2;
  CHECK(GuFindGpuAtLocation(&inv,&location,&index)==GU_INVENTORY_OK && index==2);
  location.bus=3;location.device=0;

  Setup(&m);m.count=1;
  CHECK(GuCollectGpuInventory(m.count,Reader,&m,&inv)==GU_INVENTORY_OK);
  CHECK(inv.count==1);
  CHECK(GuFindGpuAtLocation(&inv,&location,&index)==GU_INVENTORY_OK && index==0);
  index=99;
  CHECK(GuFindGpuAtLocation(&inv,&other,&index)==GU_INVENTORY_NOT_FOUND && index==0);

  Setup(&m);m.count=3;
  m.records[2]=Gpu(0x164e1002U,0,0,12,0,0,3); /* AMD display */
  CHECK(GuCollectGpuInventory(m.count,Reader,&m,&inv)==GU_INVENTORY_OK && inv.count==2);
  m.records[2]=Gpu(0x10f010deU,0,0,12,0,0,3);
  m.records[2].class_code=0x0403; /* NVIDIA audio */
  CHECK(GuCollectGpuInventory(m.count,Reader,&m,&inv)==GU_INVENTORY_OK && inv.count==2);
  m.records[2].class_code=0x0302; /* NVIDIA 3D controller */
  CHECK(GuCollectGpuInventory(m.count,Reader,&m,&inv)==GU_INVENTORY_OK && inv.count==3);
  m.records[2].class_code=0x0380; /* Other PCI display subclass */
  CHECK(GuCollectGpuInventory(m.count,Reader,&m,&inv)==GU_INVENTORY_OK && inv.count==3);

  Setup(&m);m.records[1].subsystem_id^=1;
  CHECK(GuCollectGpuInventory(m.count,Reader,&m,&inv)==GU_INVENTORY_OK);
  index=99;
  CHECK(GuFindExactGpu(&inv,&exact,&index)==GU_INVENTORY_NOT_FOUND && index==0);
  CHECK(GuFindGpuAtLocation(&inv,&other,&index)==GU_INVENTORY_OK && index==1);
  Setup(&m);m.records[1].bus++;
  CHECK(GuCollectGpuInventory(m.count,Reader,&m,&inv)==GU_INVENTORY_OK);
  index=99;
  CHECK(GuFindGpuAtLocation(&inv,&other,&index)==GU_INVENTORY_NOT_FOUND && index==0);

  Setup(&m);m.records[1].bus=m.records[0].bus;
  m.records[1].device=m.records[0].device;m.records[1].function=m.records[0].function;
  m.records[1].segment=m.records[0].segment;
  CHECK(Refuse(&m,GU_INVENTORY_DUPLICATE)==0);
  Setup(&m);m.records[1].handle=m.records[0].handle;
  CHECK(Refuse(&m,GU_INVENTORY_DUPLICATE)==0);
  Setup(&m);m.records[1].pci=m.records[0].pci;
  CHECK(Refuse(&m,GU_INVENTORY_DUPLICATE)==0);

  Setup(&m);m.records[1].device=32;
  CHECK(Refuse(&m,GU_INVENTORY_READ_FAILED)==0);
  Setup(&m);m.records[1].handle=NULL;
  CHECK(Refuse(&m,GU_INVENTORY_READ_FAILED)==0);
  Setup(&m);m.fail_at=1;
  CHECK(Refuse(&m,GU_INVENTORY_READ_FAILED)==0 && m.reads==2);

  Setup(&m);m.count=GU_INVENTORY_MAX_NVIDIA+1;
  for(i=0;i<m.count;i++)
    m.records[i]=Gpu(0x278210deU,0,0,(uint32_t)(i+1),0,0,i+1);
  CHECK(Refuse(&m,GU_INVENTORY_TOO_MANY)==0);

  Setup(&m);CHECK(GuCollectGpuInventory(0,Reader,&m,&inv)==GU_INVENTORY_INVALID);
  CHECK(GuCollectGpuInventory(GU_INVENTORY_MAX_HANDLES+1,Reader,&m,&inv)==GU_INVENTORY_INVALID);
  CHECK(GuCollectGpuInventory(2,NULL,&m,&inv)==GU_INVENTORY_INVALID);
  CHECK(GuCollectGpuInventory(2,Reader,&m,NULL)==GU_INVENTORY_INVALID);
  CHECK(GuFindGpuAtLocation(NULL,&location,&index)==GU_INVENTORY_INVALID);
  CHECK(GuFindGpuAtLocation(&inv,NULL,&index)==GU_INVENTORY_INVALID);
  CHECK(GuFindGpuAtLocation(&inv,&location,NULL)==GU_INVENTORY_INVALID);
  location.device=32;
  CHECK(GuFindGpuAtLocation(&inv,&location,&index)==GU_INVENTORY_INVALID);
  location.device=0;location.segment=65536;
  CHECK(GuFindGpuAtLocation(&inv,&location,&index)==GU_INVENTORY_INVALID);
  CHECK(GuFindExactGpu(NULL,&exact,&index)==GU_INVENTORY_INVALID);
  CHECK(GuFindExactGpu(&inv,&exact,NULL)==GU_INVENTORY_INVALID);
  printf("PASS generic GPU inventory and BDF selector: %u checks; no EFI/GPU operations\n",checks);
  return 0;
}
