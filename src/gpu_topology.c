/* SPDX-License-Identifier: GPL-2.0-only */
#include "gpu_topology.h"
#include <string.h>

int GuTopoClassRelevant(uint16_t class_code) {
  return class_code == 0x0604U || (class_code & 0xff00U) == 0x0300U;
}

int GuTopoClassifierStable(const uint32_t first[3], const uint32_t second[3]) {
  return first && second && first[0] == second[0] && first[2] == second[2];
}

static size_t Body(const GU_TOPO_PATH *p) { return p->length - 4U; }

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

static int Record(void *opaque,size_t index,GU_GPU_RECORD *out) {
  const GU_TOPO_CAPTURE *capture=(const GU_TOPO_CAPTURE *)opaque;
  *out=capture->pci[index].identity;
  return 0;
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
      if (after[j].handle == after[i].handle) return 0;
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

/* Match a separately read firmware console variable against the captured
 * display endpoints. Unknown or ambiguous ownership remains explicit. */
int GuTopoConsoleOwners(const GU_TOPO_CAPTURE *capture, const GU_TOPO_PATH *paths,
                        size_t count, GU_TOPO_CONSOLE_OWNERS *owners) {
  size_t i;
  GU_TOPO_CONSOLE_OWNERS result = {0};
  GU_TOPO_REPORT validated;
  if (owners) *owners = result;
  if(!capture || !paths || !owners || !capture->pci_count ||
     capture->pci_count>GU_TOPO_MAX_PCI || count>GU_TOPO_MAX_CONSOLES)
    return 0;
  /* A valid ErrOut path alone cannot authenticate the capture that supplies
   * its presumed PCI owner. Reuse the same complete graph validator used by
   * the strict topology verdict. Incomplete/CONFLICT are still observable;
   * structurally INVALID PCI/console/GOP captures cannot claim independent
   * ErrOut ownership. This never establishes safe GPU release. */
  if (GuTopoAssess(capture, &validated) == GU_TOPO_INVALID) return 0;
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
int GuTopoAssess(const GU_TOPO_CAPTURE *s, GU_TOPO_REPORT *r) {
  GU_GPU_INVENTORY inventory;
  size_t i,j;
  int result,bad=0,incomplete=0;
  if(!r)return GU_TOPO_INVALID;
  memset(r,0,sizeof(*r));r->status=GU_TOPO_INVALID;
  for(i=0;i<GU_INVENTORY_MAX_NVIDIA;i++)
    r->gpu_index[i]=r->parent_index[i]=r->root_index[i]=GU_TOPO_MAX_PCI;
  if(!s || !s->pci_count || s->pci_count>GU_TOPO_MAX_PCI ||
     s->gop_count>GU_TOPO_MAX_GOPS || s->console_count>GU_TOPO_MAX_CONSOLES ||
     (s->console_count && !s->console_available))return r->status;
  for(i=0;i<s->pci_count;i++) {
    const GU_GPU_RECORD *id=&s->pci[i].identity;
    if(!id->handle || !id->pci || id->segment>65535 || id->bus>255 ||
       id->device>31 || id->function>7 || !GuTopoPathValid(&s->pci[i].path) ||
       !EndpointMatches(&s->pci[i]))
      return r->status;
    for(j=0;j<i;j++)
      if(SameBdf(id,&s->pci[j].identity) ||
         id->handle==s->pci[j].identity.handle ||
         id->pci==s->pci[j].identity.pci)return r->status;
  }
  for(i=0;i<s->gop_count;i++)
    if(!GuTopoPathValid(&s->gop[i].path))return r->status;
  for(i=0;i<s->console_count;i++)
    if(!GuTopoPathValid(&s->console[i]))return r->status;
  result=GuCollectGpuInventory(s->pci_count,Record,(void *)s,&inventory);
  if(result!=GU_INVENTORY_OK)return r->status;
  r->gpu_count=inventory.count;
  if(!r->gpu_count) {
    r->status=GU_TOPO_INCOMPLETE;
    return r->status;
  }
  /* Map every NVIDIA record back to the same captured PCI handle. */
  for(j=0;j<r->gpu_count;j++) {
    const GU_GPU_RECORD *gpu=&inventory.nvidia[j];
    for(i=0;i<s->pci_count;i++) {
      if(s->pci[i].identity.handle!=gpu->handle)continue;
      if(r->gpu_index[j]!=GU_TOPO_MAX_PCI)return r->status;
      r->gpu_index[j]=i;
    }
    if(r->gpu_index[j]==GU_TOPO_MAX_PCI)return r->status;
    {
      int found=Root(s,r->gpu_index[j],&r->parent_index[j],
                     &r->root_index[j],&r->depth[j]);
      if(found<0)bad=1;
      else if(found>0)r->ancestry_verified[j]=r->root_port_verified[j]=1;
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
        if((size_t)owner==r->gpu_index[j])r->gop_count[j]++;
    } else r->independent_gop_count++;
  }
  r->gop_relationships_observed=1;
  for(j=0;j<r->gpu_count;j++) {
    const GU_TOPO_PCI *node=&s->pci[r->gpu_index[j]];
    if(!r->gop_count[j]) {
      incomplete=1;
      r->gop_relationships_observed=0;
    }
    if(!node->openers_known || !node->by_driver)incomplete=1;
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
