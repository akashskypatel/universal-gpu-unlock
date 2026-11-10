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
  /* A GOP is associated only by a valid complete controller path prefix. */
  {
    GU_TOPO_PATH controller=Path(1,0,0,0),child=controller;
    const uint8_t adr[]={2,3,8,0,0x40,3,1,0x80};
    const uint8_t end[]={0x7f,0xff,4,0};
    CHECK(GuTopoPathOnController(&controller,&controller));
    memcpy(child.bytes+controller.length-4,adr,sizeof(adr));
    memcpy(child.bytes+controller.length-4+sizeof(adr),end,sizeof(end));
    child.length+=sizeof(adr);
    CHECK(GuTopoPathOnController(&controller,&child));
    CHECK(!GuTopoPathOnController(&controller,&(GU_TOPO_PATH){0}));
    CHECK(!GuTopoPathOnController(&controller,NULL));
    CHECK(!GuTopoPathOnController(NULL,&child));
    {GU_TOPO_PATH sibling=Path(1,0,1,0);
     CHECK(!GuTopoPathOnController(&controller,&sibling));}
    child.bytes[child.length-3]=1;
    CHECK(!GuTopoPathOnController(&controller,&child));
  }
  CHECK(GuTopoClassifierStable(cls1,cls2));
  cls2[1]^=0x10000U;CHECK(GuTopoClassifierStable(cls1,cls2));
  cls2[0]^=0x10000U;CHECK(!GuTopoClassifierStable(cls1,cls2));
  cls2[0]=cls1[0];cls2[2]^=0x10000U;
  CHECK(!GuTopoClassifierStable(cls1,cls2));
  CHECK(!GuTopoClassifierStable(NULL,cls2));
  CHECK(!GuTopoClassifierStable(cls1,NULL));
  /* A console variable must retain the same entire ordered path set.
   * Valid path count alone cannot rule out a switched console owner. */
  {
    GU_TOPO_PATH initial[2], again[2];
    initial[0]=Path(-1,0,8,0);
    initial[1]=Path(1,0,0,0);
    again[0]=initial[0];again[1]=initial[1];
    CHECK(GuTopoConsolePathsEqual(initial,2,again,2));
    again[1]=Path(3,1,0,0);
    CHECK(!GuTopoConsolePathsEqual(initial,2,again,2));
    again[1]=initial[1];
    CHECK(!GuTopoConsolePathsEqual(initial,2,again,1));
    CHECK(!GuTopoConsolePathsEqual(initial,0,again,0));
    CHECK(!GuTopoConsolePathsEqual(NULL,2,again,2));
    CHECK(!GuTopoConsolePathsEqual(initial,2,NULL,2));
    again[0].length=0;
    CHECK(!GuTopoConsolePathsEqual(initial,2,again,2));
    again[0]=initial[0];
    again[0].bytes[again[0].length-3]=1;
    CHECK(!GuTopoConsolePathsEqual(initial,2,again,2));
    again[0]=initial[1];again[1]=initial[0];
    CHECK(!GuTopoConsolePathsEqual(initial,2,again,2));
  }

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

  /* Separate ErrOut ownership must not silently follow a clean ConOut.
   * This is observational and never authorizes active driver release. */
  {
    GU_TOPO_CONSOLE_OWNERS owners={0};
    GU_TOPO_PATH paths[2];
    Baseline();paths[0]=snapshot.pci[4].path;
    CHECK(GuTopoConsoleOwners(&snapshot,paths,1,&owners));
    CHECK(owners.independent==1 && owners.nvidia==0 &&
          owners.unmatched==0 && owners.ambiguous==0);
    paths[1]=snapshot.pci[1].path;
    CHECK(GuTopoConsoleOwners(&snapshot,paths,2,&owners));
    CHECK(owners.independent==1 && owners.nvidia==1);
    paths[0]=snapshot.pci[3].path;
    CHECK(GuTopoConsoleOwners(&snapshot,paths,2,&owners));
    CHECK(owners.independent==0 && owners.nvidia==2);
    paths[0]=Path(-1,0,15,0);
    CHECK(GuTopoConsoleOwners(&snapshot,paths,2,&owners));
    CHECK(owners.unmatched==1 && owners.nvidia==1);
    paths[0].length=0;
    CHECK(!GuTopoConsoleOwners(&snapshot,paths,2,&owners));
    CHECK(!GuTopoConsoleOwners(&snapshot,paths,GU_TOPO_MAX_CONSOLES+1,&owners));
    CHECK(!GuTopoConsoleOwners(NULL,paths,1,&owners));
    CHECK(!GuTopoConsoleOwners(&snapshot,NULL,1,&owners));
    CHECK(!GuTopoConsoleOwners(&snapshot,paths,1,NULL));

    /* A correctly formed AMD ErrOut cannot validate an invalid underlying
     * PCI graph. Before the fix this silently produced independent=1. */
    Baseline();paths[0]=snapshot.pci[4].path;
    memset(&owners,0x5a,sizeof(owners));
    snapshot.pci[3].identity.handle=snapshot.pci[1].identity.handle;
    CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
    CHECK(!GuTopoConsoleOwners(&snapshot,paths,1,&owners));
    CHECK(owners.nvidia==0 && owners.independent==0 &&
          owners.unmatched==0 && owners.ambiguous==0);

    /* A BDF/path mismatch must be rejected before claiming independent. */
    Baseline();paths[0]=snapshot.pci[4].path;
    memset(&owners,0x5a,sizeof(owners));
    snapshot.pci[4].path=Path(-1,0,9,0);
    CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
    CHECK(!GuTopoConsoleOwners(&snapshot,paths,1,&owners));
    CHECK(owners.nvidia==0 && owners.independent==0 &&
          owners.unmatched==0 && owners.ambiguous==0);

    /* Malformed requested path also clears a previously valid owner count. */
    Baseline();paths[0]=snapshot.pci[4].path;
    CHECK(GuTopoConsoleOwners(&snapshot,paths,1,&owners));
    CHECK(owners.independent==1);
    paths[0].length=0;
    CHECK(!GuTopoConsoleOwners(&snapshot,paths,1,&owners));
    CHECK(owners.nvidia==0 && owners.independent==0 &&
          owners.unmatched==0 && owners.ambiguous==0);

    /* Incomplete NVIDIA GOP (physical 0018/0019 shape) is not INVALID,
     * and can still identify AMD-only ErrOut without inferring Stop. */
    Baseline();paths[0]=snapshot.pci[4].path;
    snapshot.gop_count=1; snapshot.gop[0].path=snapshot.pci[4].path;
    CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
    CHECK(GuTopoConsoleOwners(&snapshot,paths,1,&owners));
    CHECK(owners.independent==1 && owners.nvidia==0 &&
          owners.unmatched==0 && owners.ambiguous==0);
  }

  /* An independent GOP for AMD cannot back an independent Intel console.
   * Old separate counts incorrectly permitted COMPLETE in this layout. */
  {
    GU_GPU_IDENTITY extra={0x9a498086U,0,0,0,9,0};
    GU_TOPO_CONSOLE_OWNERS owners={0};
    GU_TOPO_PATH errout;
    int graph;
    Baseline();
    snapshot.pci_count=6;
    Pci(5,extra,Path(-1,0,9,0),0x0300,0,0,0);
    errout=snapshot.pci[5].path;
    graph=GuTopoAssess(&snapshot,&report);
    CHECK(graph==GU_TOPO_COMPLETE); /* The AMD ConOut has a matching GOP. */
    CHECK(GuTopoConsoleOwners(&snapshot,&errout,1,&owners));
    CHECK(owners.independent==1 && owners.independent_without_gop==1);
    CHECK(GuTopoCombinedConsoleVerdict(graph,1,&owners)==GU_TOPO_INCOMPLETE);

    snapshot.console[0]=snapshot.pci[5].path;
    CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
    CHECK(report.independent_console_count==1 &&
          report.independent_console_without_gop==1);
    CHECK(!report.independent_console_observed);
    /* A same-controller GOP provides the missing bounded association,
     * still without proving that physical output survives driver Stop. */
    snapshot.gop[3].path=snapshot.pci[5].path;
    snapshot.gop_count=4;
    CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_COMPLETE);
    CHECK(report.independent_console_without_gop==0);
    CHECK(GuTopoConsoleOwners(&snapshot,&errout,1,&owners));
    CHECK(owners.independent==1 && owners.independent_without_gop==0);
    CHECK(GuTopoCombinedConsoleVerdict(GU_TOPO_COMPLETE,1,&owners)==GU_TOPO_COMPLETE);
  }

  /* Final passive verdict must include ErrOut, not only ConOut/GOP.
   * The previously separate ErrOut logging did not veto EFI_SUCCESS. */
  {
    GU_TOPO_CONSOLE_OWNERS owners={0};
    GU_TOPO_PATH errout[2];
    int graph;
    Baseline();
    graph=GuTopoAssess(&snapshot,&report);
    CHECK(graph==GU_TOPO_COMPLETE);
    errout[0]=snapshot.pci[4].path; /* AMD error console. */
    CHECK(GuTopoConsoleOwners(&snapshot,errout,1,&owners));
    CHECK(GuTopoCombinedConsoleVerdict(graph,1,&owners)==GU_TOPO_COMPLETE);

    errout[0]=snapshot.pci[1].path; /* NVIDIA-owned error console. */
    CHECK(GuTopoConsoleOwners(&snapshot,errout,1,&owners));
    CHECK(GuTopoCombinedConsoleVerdict(graph,1,&owners)==GU_TOPO_CONFLICT);
    errout[0]=snapshot.pci[4].path;
    errout[1]=snapshot.pci[3].path;
    CHECK(GuTopoConsoleOwners(&snapshot,errout,2,&owners));
    CHECK(GuTopoCombinedConsoleVerdict(graph,1,&owners)==GU_TOPO_CONFLICT);

    errout[0]=Path(-1,0,15,0); /* Unknown endpoint. */
    CHECK(GuTopoConsoleOwners(&snapshot,errout,1,&owners));
    CHECK(GuTopoCombinedConsoleVerdict(graph,1,&owners)==GU_TOPO_INCOMPLETE);
    CHECK(GuTopoCombinedConsoleVerdict(graph,0,&owners)==GU_TOPO_INCOMPLETE);
    CHECK(GuTopoCombinedConsoleVerdict(graph,1,NULL)==GU_TOPO_INCOMPLETE);
    owners=(GU_TOPO_CONSOLE_OWNERS){0};
    CHECK(GuTopoCombinedConsoleVerdict(graph,1,&owners)==GU_TOPO_INCOMPLETE);
    owners.ambiguous=1;
    CHECK(GuTopoCombinedConsoleVerdict(graph,1,&owners)==GU_TOPO_CONFLICT);

    owners=(GU_TOPO_CONSOLE_OWNERS){0};
    owners.independent=1;
    CHECK(GuTopoCombinedConsoleVerdict(GU_TOPO_INCOMPLETE,1,&owners)==GU_TOPO_INCOMPLETE);
    CHECK(GuTopoCombinedConsoleVerdict(GU_TOPO_CONFLICT,1,&owners)==GU_TOPO_CONFLICT);
    CHECK(GuTopoCombinedConsoleVerdict(GU_TOPO_INVALID,1,&owners)==GU_TOPO_INVALID);
    CHECK(GuTopoCombinedConsoleVerdict(-1,1,&owners)==GU_TOPO_INVALID);
    CHECK(GuTopoCombinedConsoleVerdict(GU_TOPO_COMPLETE,1,&owners)==GU_TOPO_COMPLETE);
  }

  Baseline();snapshot.console[0]=snapshot.pci[1].path;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_CONFLICT);
  CHECK(report.nvidia_console_count==1 && !report.independent_console_observed);
  Baseline();snapshot.console_count=0;snapshot.console_available=0;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  /* Physical passive 0018/0019 pattern: two bound NVIDIA PCI drivers,
   * zero NVIDIA GOP paths, one independent AMD GOP and AMD-only console.
   * Complete console isolation evidence is not complete NVIDIA teardown
   * evidence, and cannot imply a successful DriverBinding.Stop. */
  Baseline();
  snapshot.gop_count=1;
  snapshot.gop[0].path=snapshot.pci[4].path;
  CHECK(snapshot.pci[1].by_driver==1 && snapshot.pci[3].by_driver==1);
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  CHECK(report.gpu_count==2 && report.gop_count[0]==0 && report.gop_count[1]==0);
  CHECK(report.ancestry_verified[0] && report.ancestry_verified[1]);
  CHECK(report.independent_gop_count==1 && report.independent_console_count==1);
  CHECK(report.independent_console_observed && report.nvidia_console_count==0);
  CHECK(!report.gop_relationships_observed);
  /* Mixed-display 0017 is a different boot configuration. Configured
   * NVIDIA output paths must still force CONFLICT. */
  Baseline();
  snapshot.console_count=2;
  snapshot.console[0]=snapshot.pci[1].path;
  snapshot.console[1]=snapshot.pci[3].path;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_CONFLICT);
  CHECK(report.nvidia_console_count==2 && !report.independent_console_observed);
  Baseline();snapshot.gop_count=2;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  /* Every configured console path matters: an independently owned AMD
   * ConOut cannot mask an additional unknown, well-formed ConOut. Before
   * this fix the second path was skipped and the verdict was COMPLETE. */
  Baseline();
  snapshot.console_count=2;
  snapshot.console[1]=Path(-1,0,15,0);
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  CHECK(report.independent_console_count==1 && report.unmatched_console_count==1);
  CHECK(report.ambiguous_console_count==0 && !report.independent_console_observed);
  /* Every GOP also matters: a stray GOP with no display-owner match
   * cannot be ignored merely because both NVIDIA GOPs are present. */
  Baseline();
  snapshot.gop[3].path=Path(-1,0,15,0);
  snapshot.gop_count=4;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  CHECK(report.unmatched_gops==1 && report.gop_relationships_observed);
  CHECK(report.independent_console_count==1 && report.independent_console_observed);
  /* A genuinely complete graph still qualifies as a read-only observation. */
  Baseline();
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_COMPLETE);
  CHECK(report.unmatched_gops==0 && report.unmatched_console_count==0 &&
        report.ambiguous_console_count==0);
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
