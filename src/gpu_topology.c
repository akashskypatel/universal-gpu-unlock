/* SPDX-License-Identifier: GPL-2.0-only */
#include "gpu_topology.h"
#include <string.h>

int GuTopoClassRelevant(uint16_t class_code) {
  return class_code == 0x0604U || (class_code & 0xff00U) == 0x0300U;
}

int GuTopoReadPcieLink(const uint32_t config[64], GU_TOPO_LINK_OBS *out) {
  uint64_t seen=0;
  unsigned pointer,steps=0;
  int saw_pcie=0;
  GU_TOPO_LINK_OBS result={0};
  if(out)memset(out,0,sizeof(*out));
  if(!config||!out)return -1;
  if((config[0x04U/4U]&(1U<<20U))==0)return 0;
  pointer=(config[0x34U/4U]>>0)&0xffU;
  while(pointer) {
    uint32_t header;
    unsigned id,next,version,type;
    uint64_t bit;
    if(++steps>48U || pointer<0x40U || pointer>0xfcU || (pointer&3U))return -1;
    bit=UINT64_C(1)<<(pointer/4U);
    if(seen&bit)return -1;
    seen|=bit;
    header=config[pointer/4U];
    id=header&0xffU;next=(header>>8U)&0xffU;
    if(id==0x10U) {
      uint16_t pcie=(uint16_t)(header>>16U);
      version=pcie&0x0fU;type=(pcie>>4U)&0x0fU;
      if(saw_pcie || (version!=1U&&version!=2U)||type>10U)return -1;
      saw_pcie=1;
      /* Root-complex integrated endpoints and event collectors (types 9/10)
       * have no PCIe Link Capability/Control/Status registers; still validate
       * the rest of the capability chain before reporting that fact. */
      if(type!=9U&&type!=10U) {
        uint32_t link_capabilities,link_control_status,device_control_status;
        if(pointer>0xe8U)return -1; /* Link Status must fit in config space. */
        link_capabilities=config[(pointer+0x0cU)/4U];
        link_control_status=config[(pointer+0x10U)/4U];
        device_control_status=config[(pointer+0x08U)/4U];
        result.present=1;
        result.port_type=(uint8_t)type;
        result.capability_offset=(uint16_t)pointer;
        result.max_speed=(uint8_t)(link_capabilities&0x0fU);
        result.max_width=(uint8_t)((link_capabilities>>4U)&0x3fU);
        result.aspm=(uint8_t)(link_control_status&3U);
        result.speed=(uint8_t)((link_control_status>>16U)&0x0fU);
        result.width=(uint8_t)((link_control_status>>20U)&0x3fU);
        result.training=(uint8_t)((link_control_status>>27U)&1U);
        result.dll_active=(uint8_t)((link_control_status>>29U)&1U);
        result.device_errors=(uint8_t)((device_control_status>>16U)&0x0fU);
      }
    }
    pointer=next;
  }
  *out=result;
  return result.present?1:0;
}

int GuTopoClassifierStable(const uint32_t first[3], const uint32_t second[3]) {
  return first && second && first[0] == second[0] && first[2] == second[2];
}

int GuTopoDriverOwnerStable(const GU_TOPO_PCI *before, size_t driver_count,
                            const void *first_agent) {
  if (!before || !before->openers_known || before->by_driver != driver_count)
    return 0;
  if (driver_count == 0)
    return !before->first_owner_agent && !first_agent;
  return driver_count == 1 && before->first_owner_agent &&
         before->first_owner_agent == first_agent;
}

static size_t Body(const GU_TOPO_PATH *p) { return p->length - 4U; }

/* Half-open range comparison for caller-owned capture, report and storage.
 * A range that could wrap the address space is treated as overlapping. */
static int RangesOverlap(const void *a, size_t a_bytes,
                         const void *b, size_t b_bytes) {
  size_t a0 = (size_t)(uintptr_t)a, b0 = (size_t)(uintptr_t)b;
  if (!a || !b) return 0;
  if (a0 > SIZE_MAX - a_bytes || b0 > SIZE_MAX - b_bytes) return 1;
  return a0 < b0 + b_bytes && b0 < a0 + a_bytes;
}

int GuTopoPathValid(const GU_TOPO_PATH *p) {
  size_t at = 0;
  if (!p || p->length < 4 || p->length > GU_TOPO_PATH_BYTES) return 0;
  while (at + 4 <= p->length) {
    size_t length = (size_t)p->bytes[at+2] | ((size_t)p->bytes[at+3] << 8);
    if (length < 4 || length > p->length-at) return 0;
    if (p->bytes[at] == 0x7f) {
      return p->bytes[at+1] == 0xff && length == 4 &&
             at+length == p->length;
    }
    at += length;
  }
  return 0;
}

/* The last PCI node must identify the same function as GetLocation().
 * Otherwise a fabricated/changed path could assign the wrong bridge chain. */
static int EndpointMatches(const GU_TOPO_PCI *node) {
  const GU_TOPO_PATH *p=&node->path;
  size_t body=Body(p),at=0,last=0;
  if(body<6)return 0;
  while(at<body) {
    size_t n=(size_t)p->bytes[at+2] | ((size_t)p->bytes[at+3]<<8);
    if(n<4 || n>body-at)return 0;
    last=at;at+=n;
  }
  return p->bytes[last]==1U && p->bytes[last+1]==1U &&
    body-last==6U && p->bytes[last+4]==node->identity.function &&
    p->bytes[last+5]==node->identity.device;
}

static int Prefix(const GU_TOPO_PATH *prefix, const GU_TOPO_PATH *path) {
  size_t length;
  if (!GuTopoPathValid(prefix) || !GuTopoPathValid(path)) return 0;
  length = Body(prefix);
  return length <= Body(path) && !memcmp(prefix->bytes,path->bytes,length);
}

int GuTopoPathOnController(const GU_TOPO_PATH *controller,
                           const GU_TOPO_PATH *candidate) {
  return controller && candidate && GuTopoPathValid(controller) &&
    GuTopoPathValid(candidate) && Body(controller)>=6U &&
    Body(controller)<=Body(candidate) &&
    !memcmp(controller->bytes,candidate->bytes,Body(controller));
}

static int SameBdf(const GU_GPU_RECORD *a, const GU_GPU_RECORD *b) {
  return a->segment == b->segment && a->bus == b->bus &&
    a->device == b->device && a->function == b->function;
}

/* Only a complete PCI device path prefix identifies an ancestor. A nearest
 * parent must be exactly one PCI node (six bytes) earlier; missing bridges
 * never get silently skipped. */
static int Parent(const GU_TOPO_CAPTURE *s, size_t child, size_t *parent) {
  size_t i, found=GU_TOPO_MAX_PCI, depth=0;
  const GU_TOPO_PATH *path=&s->pci[child].path;
  if (Body(path) < 6) return 0;
  for(i=0;i<s->pci_count;i++) {
    const GU_TOPO_PCI *p=&s->pci[i];
    if(i==child || p->identity.segment!=s->pci[child].identity.segment ||
       !p->bridge || Body(&p->path)+6 != Body(path) ||
       !Prefix(&p->path,path)) continue;
    if(found != GU_TOPO_MAX_PCI) return -1;
    found=i;depth++;
  }
  if(depth!=1) return 0;
  *parent=found;
  return 1;
}

static int Root(const GU_TOPO_CAPTURE *s, size_t gpu, size_t *parent,
                size_t *root, unsigned *depth) {
  size_t current=gpu, closest=GU_TOPO_MAX_PCI, seen=0;
  *depth=0;
  *parent=*root=GU_TOPO_MAX_PCI;
  /* A child bus must fall within each ancestor bridge window. */
  while (*depth < 16) {
    size_t index;
    int found=Parent(s,current,&index);
    if (found<0) return -1;
    if (!found) break;
    if (!*depth) closest=index;
    /* Verify each bridge's primary bus against GetLocation and each
     * immediate PCI bridge-to-child bus edge. A leaf-only range test can
     * accept an incorrectly addressed intermediate bridge. */
    if (s->pci[index].identity.segment != s->pci[gpu].identity.segment ||
        s->pci[index].primary != s->pci[index].identity.bus ||
        s->pci[index].secondary > s->pci[index].subordinate ||
        s->pci[current].identity.bus < s->pci[index].secondary ||
        s->pci[current].identity.bus > s->pci[index].subordinate ||
        s->pci[gpu].identity.bus < s->pci[index].secondary ||
        s->pci[gpu].identity.bus > s->pci[index].subordinate) return -1;
    if (s->pci[index].root_port) {
      if (seen++) return -1; /* multiple root ports on the same chain */
      *root=index;
    }
    current=index;(*depth)++;
  }
  if (*depth==16) return -1;
  *parent=closest;
  return closest!=GU_TOPO_MAX_PCI && *root!=GU_TOPO_MAX_PCI ? 1:0;
}

/* Longest complete PCI prefix identifies a GOP/ConOut path's exact endpoint.
 * Root prefixes alone are not accepted as display ownership. */
static int DisplayOwner(const GU_TOPO_CAPTURE *s, const GU_TOPO_PATH *path) {
  size_t i, longest=0;
  int result=-1;
  for(i=0;i<s->pci_count;i++) {
    const GU_GPU_RECORD *r=&s->pci[i].identity;
    size_t length=Body(&s->pci[i].path);
    if((r->class_code & 0xff00U) != 0x0300U) continue;
    if(length < 6 || length < longest || !Prefix(&s->pci[i].path,path)) continue;
    if(length == longest) { result=-2; continue; }
    longest=length;result=(int)i;
  }
  return result;
}

/* Compare full configured-console path inventories captured at separate
 * read-only points. An order change is treated as drift, not normalized. */
int GuTopoConsolePathsEqual(const GU_TOPO_PATH *first, size_t first_count,
                             const GU_TOPO_PATH *second, size_t second_count) {
  size_t i;
  if (!first || !second || !first_count ||
      first_count > GU_TOPO_MAX_CONSOLES || first_count != second_count)
    return 0;
  for (i=0; i<first_count; ++i) {
    if (!GuTopoPathValid(&first[i]) || !GuTopoPathValid(&second[i]) ||
        first[i].length != second[i].length ||
        memcmp(first[i].bytes, second[i].bytes, first[i].length))
      return 0;
  }
  return 1;
}

/* GOP association is a current-boot set, not an enumeration index.
 * Rebinding a protocol or moving a path must invalidate previous evidence. */
int GuTopoGopSnapshotsEqual(const GU_TOPO_GOP *before, size_t before_count,
                            const GU_TOPO_GOP *after, size_t after_count) {
  size_t i,j;
  if (before_count != after_count || before_count > GU_TOPO_MAX_GOPS) return 0;
  if (!before_count) return 1;
  if (!before || !after) return 0;
  for (i=0; i<after_count; ++i) {
    size_t matches=0;
    if (!after[i].handle || !after[i].protocol ||
        !GuTopoPathValid(&after[i].path)) return 0;
    for (j=0; j<i; ++j)
      if (after[j].handle == after[i].handle ||
          after[j].protocol == after[i].protocol) return 0;
    for (j=0; j<before_count; ++j) {
      if (before[j].handle != after[i].handle) continue;
      ++matches;
      if (!before[j].protocol || before[j].protocol != after[i].protocol ||
          !GuTopoPathValid(&before[j].path) ||
          before[j].path.length != after[i].path.length ||
          memcmp(before[j].path.bytes,after[i].path.bytes,after[i].path.length))
        return 0;
    }
    if (matches != 1) return 0;
  }
  return 1;
}

/* Only a GOP on the SAME PCI display endpoint backs the console path.
 * Another independent GPU's GOP cannot establish that relationship. */
static int HasGopForDisplay(const GU_TOPO_CAPTURE *s, int owner) {
  size_t i;
  for (i=0; i<s->gop_count; ++i)
    if (DisplayOwner(s, &s->gop[i].path) == owner) return 1;
  return 0;
}

/* Structural validation is shared by both topology and console reports.
 * It does not allocate or depend on the number of NVIDIA devices. */
static int CaptureValid(const GU_TOPO_CAPTURE *s) {
  size_t i,j;
  if(!s || !s->pci_count || s->pci_count>GU_TOPO_MAX_PCI ||
     s->gop_capacity>GU_TOPO_MAX_GOPS || s->gop_count>s->gop_capacity ||
     (s->gop_count && !s->gop) || s->console_count>GU_TOPO_MAX_CONSOLES ||
     (s->console_count && !s->console_available))return 0;
  for(i=0;i<s->pci_count;i++) {
    const GU_GPU_RECORD *id=&s->pci[i].identity;
    if(!id->handle || !id->pci || id->segment>65535 || id->bus>255 ||
       id->device>31 || id->function>7 || !GuTopoPathValid(&s->pci[i].path) ||
       !EndpointMatches(&s->pci[i]))
      return 0;
    for(j=0;j<i;j++)
      if(SameBdf(id,&s->pci[j].identity) ||
         id->handle==s->pci[j].identity.handle ||
         id->pci==s->pci[j].identity.pci)return 0;
  }
  for(i=0;i<s->gop_count;i++)
    if(!GuTopoPathValid(&s->gop[i].path))return 0;
  for(i=0;i<s->console_count;i++)
    if(!GuTopoPathValid(&s->console[i]))return 0;
  return 1;
}

/* Match a separately read firmware console variable against the captured
 * display endpoints. Unknown or ambiguous ownership remains explicit. */
int GuTopoConsoleOwners(const GU_TOPO_CAPTURE *capture, const GU_TOPO_PATH *paths,
                        size_t count, GU_TOPO_CONSOLE_OWNERS *owners) {
  size_t i;
  GU_TOPO_CONSOLE_OWNERS result = {0};
  if (owners) *owners = result;
  if(!capture || !paths || !owners || !capture->pci_count ||
     capture->pci_count>GU_TOPO_MAX_PCI || count>GU_TOPO_MAX_CONSOLES)
    return 0;
  /* A valid ErrOut path alone cannot authenticate the capture that supplies
   * its presumed PCI owner. Reuse the same complete graph validator used by
   * the strict topology verdict. Incomplete/CONFLICT are still observable;
   * structurally INVALID PCI/console/GOP captures cannot claim independent
   * ErrOut ownership. This never establishes safe GPU release. */
  if (!CaptureValid(capture)) return 0;
  for(i=0;i<count;i++) {
    int owner;
    if(!GuTopoPathValid(&paths[i])) return 0;
    owner=DisplayOwner(capture,&paths[i]);
    if(owner==-2) result.ambiguous++;
    else if(owner<0) result.unmatched++;
    else if((capture->pci[owner].identity.pci_id&0xffffU)==0x10deU)
      result.nvidia++;
    else {
      result.independent++;
      if (!HasGopForDisplay(capture, owner))
        result.independent_without_gop++;
    }
  }
  *owners=result;
  return 1;
}

/* Reconcile the graph/ConOut verdict with separately captured ErrOut.
 * EFI success for a clean ConOut is not valid when ErrOut still depends on
 * NVIDIA, is ambiguous, or was not captured. No actuation is authorized. */
int GuTopoCombinedConsoleVerdict(int graph_status, int errout_classified,
                                 const GU_TOPO_CONSOLE_OWNERS *errout) {
  if (graph_status == GU_TOPO_INVALID) return GU_TOPO_INVALID;
  if (graph_status != GU_TOPO_COMPLETE &&
      graph_status != GU_TOPO_INCOMPLETE &&
      graph_status != GU_TOPO_CONFLICT) return GU_TOPO_INVALID;
  if (errout_classified && errout &&
      (errout->nvidia || errout->ambiguous)) return GU_TOPO_CONFLICT;
  if (graph_status == GU_TOPO_CONFLICT) return GU_TOPO_CONFLICT;
  if (!errout_classified || !errout || errout->unmatched ||
      errout->independent_without_gop || !errout->independent)
    return GU_TOPO_INCOMPLETE;
  return graph_status;
}

/* Topology is derived from the captured device paths, not a selected board,
 * GPU model, PCI slot ordering or assumption that root ports are distinct. */
int GuTopoAssess(const GU_TOPO_CAPTURE *s, GU_TOPO_REPORT *r,
                 GU_TOPO_GPU_REPORT *storage, size_t capacity) {
  size_t i,j,count=0,storage_bytes=0;
  int bad=0,incomplete=0;
  if(!r)return GU_TOPO_INVALID;
  /* The report and the per-GPU storage are independent caller-owned outputs.
   * Verify that independence before the first output byte is written: a
   * storage range inside the report or the live capture cannot be initialized
   * or filled without destroying evidence the assessment still reads. */
  if(capacity>GU_TOPO_MAX_PCI)return GU_TOPO_INVALID;
  if(capacity && !storage)return GU_TOPO_INVALID;
  if(capacity) {
    if(capacity>SIZE_MAX/sizeof(*storage))return GU_TOPO_INVALID;
    storage_bytes=capacity*sizeof(*storage);
  }
  if(RangesOverlap(r,sizeof(*r),s,sizeof(*s)) ||
     RangesOverlap(storage,storage_bytes,r,sizeof(*r)) ||
     RangesOverlap(storage,storage_bytes,s,sizeof(*s)))
    return GU_TOPO_INVALID;
  memset(r,0,sizeof(*r));r->status=GU_TOPO_INVALID;
  if(!CaptureValid(s))return r->status;
  for(i=0;i<s->pci_count;i++) {
    const GU_GPU_RECORD *id=&s->pci[i].identity;
    if((id->pci_id&0xffffU)==0x10deU && (id->class_code&0xff00U)==0x0300U)
      count++;
  }
  if(count>capacity)return r->status;
  r->gpus=storage;
  if(capacity)memset(storage,0,capacity*sizeof(*storage));
  if(!count) {
    r->status=GU_TOPO_INCOMPLETE;
    return r->status;
  }
  /* Preserve PCI enumeration order. CaptureValid already excludes duplicate
   * handles, PCI interfaces and locations across the entire captured set. */
  for(i=0;i<s->pci_count;i++) {
    const GU_GPU_RECORD *id=&s->pci[i].identity;
    GU_TOPO_GPU_REPORT *gpu;
    if((id->pci_id&0xffffU)!=0x10deU || (id->class_code&0xff00U)!=0x0300U)
      continue;
    gpu=&storage[r->gpu_count++];
    gpu->gpu_index=i;
    {
      int found=Root(s,i,&gpu->parent_index,&gpu->root_index,&gpu->depth);
      if(found<0)bad=1;
      else if(found>0)gpu->ancestry_verified=gpu->root_port_verified=1;
      else incomplete=1;
    }
  }
  for(i=0;i<s->gop_count;i++) {
    int owner=DisplayOwner(s,&s->gop[i].path);
    if(owner==-2)bad=1;
    if(owner<0) {
      r->unmatched_gops++;
      incomplete=1; /* Unresolved GOP owner cannot qualify full topology. */
      continue;
    }
    if((s->pci[owner].identity.pci_id&0xffffU)==0x10deU) {
      for(j=0;j<r->gpu_count;j++)
        if((size_t)owner==r->gpus[j].gpu_index)r->gpus[j].gop_count++;
    } else r->independent_gop_count++;
  }
  r->gop_relationships_observed=1;
  for(j=0;j<r->gpu_count;j++) {
    const GU_TOPO_PCI *node=&s->pci[r->gpus[j].gpu_index];
    if(!r->gpus[j].gop_count) {
      incomplete=1;
      r->gop_relationships_observed=0;
    }
    if(!node->openers_known || !node->by_driver ||
       !node->first_owner_agent)incomplete=1;
    if(node->by_driver>1)bad=1;
  }
  for(i=0;i<s->console_count;i++) {
    int owner=DisplayOwner(s,&s->console[i]);
    if(owner==-2) {
      r->ambiguous_console_count++;
      bad=1;
      continue;
    }
    if(owner<0) {
      r->unmatched_console_count++;
      incomplete=1; /* One AMD console cannot excuse an unknown ConOut. */
      continue;
    }
    if((s->pci[owner].identity.pci_id&0xffffU)==0x10deU)
      r->nvidia_console_count++;
    else {
      r->independent_console_count++;
      if (!HasGopForDisplay(s, owner))
        r->independent_console_without_gop++;
    }
  }
  if(r->nvidia_console_count)bad=1;
  r->independent_console_observed=(s->console_available &&
      r->independent_console_count && r->independent_gop_count &&
      !r->independent_console_without_gop &&
      !r->unmatched_console_count && !r->ambiguous_console_count &&
      !r->nvidia_console_count);
  if(!r->independent_console_observed)incomplete=1;
  r->status=bad?GU_TOPO_CONFLICT:incomplete?GU_TOPO_INCOMPLETE:GU_TOPO_COMPLETE;
  return r->status;
}
