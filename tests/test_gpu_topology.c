/* SPDX-License-Identifier: GPL-2.0-only */
/* Pure host topology evidence for arbitrary PCI display layouts. */
#include "../src/gpu_topology.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static GU_TOPO_CAPTURE snapshot;
static GU_TOPO_REPORT report;
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL:%d: %s\n",__LINE__,#x);return 1;}}while(0)
static const uint8_t acpi[]={2,1,12,0,0xd0,0x41,3,0x0a,0,0,0,0};

static GU_TOPO_PATH Path(int bridge_dev,int bridge_fun,int end_dev,int end_fun) {
  GU_TOPO_PATH p;
  size_t at=0;
  memset(&p,0,sizeof(p));
  memcpy(p.bytes,acpi,sizeof(acpi));at+=sizeof(acpi);
  if(bridge_dev>=0) {
    uint8_t hop[]={1,1,6,0,(uint8_t)bridge_fun,(uint8_t)bridge_dev};
    memcpy(p.bytes+at,hop,sizeof(hop));at+=sizeof(hop);
  }
  if(end_dev>=0) {
    uint8_t hop[]={1,1,6,0,(uint8_t)end_fun,(uint8_t)end_dev};
    memcpy(p.bytes+at,hop,sizeof(hop));at+=sizeof(hop);
  }
  {const uint8_t end[]={0x7f,0xff,4,0};
   memcpy(p.bytes+at,end,sizeof(end));at+=sizeof(end);}
  p.length=at;return p;
}
static void Pci(size_t i,GU_GPU_IDENTITY id,GU_TOPO_PATH path,
                uint16_t cls,int root,uint8_t secondary,uint8_t subordinate) {
  GU_TOPO_PCI *p=&snapshot.pci[i];
  p->identity.pci_id=id.pci_id;p->identity.subsystem_id=id.subsystem_id;
  p->identity.segment=id.segment;p->identity.bus=id.bus;
  p->identity.device=id.device;p->identity.function=id.function;
  p->identity.class_code=cls;
  p->identity.handle=(void *)(uintptr_t)(i*2+1);
  p->identity.pci=(void *)(uintptr_t)(i*2+2);
  p->path=path;p->bridge=(cls==0x0604);p->root_port=(uint8_t)root;
  p->secondary=secondary;p->subordinate=subordinate;
  p->openers_known=1;p->by_driver=(cls==0x0300);
}
static void Baseline(void) {
  GU_GPU_IDENTITY root_a={0x12341022U,0,0,0,1,0};
  GU_GPU_IDENTITY root_b={0x12341022U,0,0,0,3,1};
  GU_GPU_IDENTITY nv_a={0x20b010deU,0x98ab10deU,0,1,0,0};
  GU_GPU_IDENTITY nv_b={0x250310deU,0xabcd10deU,0,4,0,0};
  GU_GPU_IDENTITY other={0x164e1002U,0,0,0,8,0};
  memset(&snapshot,0,sizeof(snapshot));snapshot.pci_count=5;
  Pci(0,root_a,Path(-1,0,1,0),0x0604,1,1,1);
  Pci(1,nv_a,Path(1,0,0,0),0x0300,0,0,0);
  Pci(2,root_b,Path(-1,0,3,1),0x0604,1,4,4);
  Pci(3,nv_b,Path(3,1,0,0),0x0300,0,0,0);
  Pci(4,other,Path(-1,0,8,0),0x0300,0,0,0);
  snapshot.gop_count=3;
  snapshot.gop[0].path=snapshot.pci[1].path;
  snapshot.gop[1].path=snapshot.pci[3].path;
  snapshot.gop[2].path=snapshot.pci[4].path;
  snapshot.console_available=1;snapshot.console_count=1;
  snapshot.console[0]=snapshot.pci[4].path;
}
int main(void) {
  GU_TOPO_PCI swapped;
  uint32_t cls1[3]={0x14d81022U,0x00100007U,0x06000010U};
  uint32_t cls2[3]={0x14d81022U,0x00100007U,0x06000010U};

  CHECK(!GuTopoClassRelevant(0x0600U));
  CHECK(!GuTopoClassRelevant(0x0601U));
  CHECK(!GuTopoClassRelevant(0x0200U));
  CHECK(!GuTopoClassRelevant(0x0403U));
  CHECK(GuTopoClassRelevant(0x0604U));
  CHECK(GuTopoClassRelevant(0x0300U));
  CHECK(GuTopoClassRelevant(0x0302U));
  CHECK(GuTopoClassRelevant(0x0380U));
  CHECK(GuTopoClassifierStable(cls1,cls2));
  cls2[1]^=0x10000U;CHECK(GuTopoClassifierStable(cls1,cls2));
  cls2[0]^=0x10000U;CHECK(!GuTopoClassifierStable(cls1,cls2));
  cls2[0]=cls1[0];cls2[2]^=0x10000U;
  CHECK(!GuTopoClassifierStable(cls1,cls2));
  CHECK(!GuTopoClassifierStable(NULL,cls2));
  CHECK(!GuTopoClassifierStable(cls1,NULL));

  Baseline();
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_COMPLETE);
  CHECK(report.gpu_count==2 && report.gpu_index[0]==1 && report.gpu_index[1]==3);
  CHECK(report.root_index[0]==0 && report.root_index[1]==2);
  CHECK(report.ancestry_verified[0] && report.ancestry_verified[1]);
  CHECK(report.root_port_verified[0] && report.root_port_verified[1]);
  CHECK(report.gop_count[0]==1 && report.gop_count[1]==1);
  CHECK(report.independent_gop_count==1 && report.independent_console_count==1);
  CHECK(report.independent_console_observed && !report.nvidia_console_count);

  Baseline();swapped=snapshot.pci[1];snapshot.pci[1]=snapshot.pci[3];snapshot.pci[3]=swapped;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_COMPLETE);
  CHECK(report.gpu_count==2 && report.gpu_index[0]==1 && report.gpu_index[1]==3);
  CHECK(report.root_index[0]==2 && report.root_index[1]==0);

  /* A single NVIDIA GPU is legitimate; other display devices are not targets. */
  Baseline();snapshot.pci[3].identity.pci_id=0x9a498086U;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_COMPLETE);
  CHECK(report.gpu_count==1 && report.gpu_index[0]==1);
  CHECK(report.independent_gop_count==2);

  /* Intel is also a valid non-NVIDIA firmware console, not only AMD. */
  Baseline();snapshot.pci[4].identity.pci_id=0x9a498086U;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_COMPLETE);
  CHECK(report.independent_console_observed && report.independent_console_count==1);

  /* A third NVIDIA GPU is accepted; shared parent root ports are valid. */
  Baseline();snapshot.pci_count=6;
  {GU_GPU_IDENTITY third={0x278210deU,0x567810deU,0,1,1,0};
   Pci(5,third,Path(1,0,1,0),0x0300,0,0,0);}
  snapshot.gop[3].path=snapshot.pci[5].path;snapshot.gop_count=4;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_COMPLETE);
  CHECK(report.gpu_count==3);
  CHECK(report.root_index[0]==0 && report.root_index[2]==0);
  CHECK(report.gop_count[2]==1 && report.independent_console_observed);

  Baseline();snapshot.pci[3].identity.bus=1;
  snapshot.pci[3].identity.device=2;
  snapshot.pci[3].path=Path(1,0,2,0);
  snapshot.gop[1].path=snapshot.pci[3].path;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_COMPLETE);
  CHECK(report.gpu_count==2 && report.root_index[0]==report.root_index[1]);

  /* Other PCI display subclasses are included and GOP-associated. */
  Baseline();snapshot.pci[3].identity.class_code=0x0380;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_COMPLETE);
  CHECK(report.gpu_count==2 && report.gop_count[1]==1);

  /* Model and subsystem changes do not invalidate dynamic hardware discovery. */
  Baseline();snapshot.pci[3].identity.subsystem_id^=1U;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_COMPLETE);
  Baseline();snapshot.pci[3].identity.pci_id=0x278210deU;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_COMPLETE);
  Baseline();snapshot.pci[3].identity.pci_id=0x9a498086U;
  snapshot.pci[1].identity.pci_id=0x164e1002U;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  CHECK(report.gpu_count==0);

  Baseline();snapshot.pci[3].identity.segment=1;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  Baseline();snapshot.pci[3].identity.bus=5;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_CONFLICT);

  Baseline();snapshot.pci[3].identity.bus=snapshot.pci[1].identity.bus;
  snapshot.pci[3].identity.device=snapshot.pci[1].identity.device;
  snapshot.pci[3].identity.function=snapshot.pci[1].identity.function;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
  Baseline();snapshot.pci[3].identity.handle=snapshot.pci[1].identity.handle;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
  Baseline();snapshot.pci[2].identity.pci=snapshot.pci[0].identity.pci;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);

  Baseline();snapshot.pci[0].root_port=0;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  Baseline();snapshot.pci[0].secondary=2;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_CONFLICT);
  Baseline();snapshot.pci[2].subordinate=3;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_CONFLICT);
  Baseline();snapshot.pci[0].bridge=0;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  Baseline();snapshot.pci[0].path=Path(-1,0,5,0);
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);

  Baseline();snapshot.console[0]=snapshot.pci[1].path;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_CONFLICT);
  CHECK(report.nvidia_console_count==1 && !report.independent_console_observed);
  Baseline();snapshot.console_count=0;snapshot.console_available=0;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  Baseline();snapshot.gop_count=2;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  Baseline();snapshot.console[0]=Path(-1,0,15,0);
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  Baseline();snapshot.gop[0].path=Path(-1,0,15,0);
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  CHECK(report.unmatched_gops==1 && report.gop_count[0]==0);

  Baseline();snapshot.pci[1].openers_known=0;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  Baseline();snapshot.pci[1].by_driver=0;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  Baseline();snapshot.pci[1].by_driver=2;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_CONFLICT);
  Baseline();snapshot.pci[3].path.bytes[23]=5;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
  Baseline();snapshot.pci[1].path.bytes[14]=1;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
  Baseline();snapshot.pci[1].path.length=GU_TOPO_PATH_BYTES+1;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
  Baseline();snapshot.console[0].bytes[snapshot.console[0].length-3]=1;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
  Baseline();snapshot.gop[0].path.length=0;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
  Baseline();snapshot.pci_count=GU_TOPO_MAX_PCI+1;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
  Baseline();snapshot.gop_count=GU_TOPO_MAX_GOPS+1;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
  Baseline();snapshot.console_count=GU_TOPO_MAX_CONSOLES+1;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
  CHECK(GuTopoAssess(NULL,&report)==GU_TOPO_INVALID);
  CHECK(GuTopoAssess(&snapshot,NULL)==GU_TOPO_INVALID);
  printf("PASS dynamic multi-GPU topology: %u checks; no EFI/hardware calls\n",checks);
  return 0;
}
