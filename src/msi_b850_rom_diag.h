/* SPDX-License-Identifier: GPL-2.0-only */
/* Read-only MSI MS-7E56 dual-NVIDIA inventory fingerprint. */
#pragma once
#include "gpu_topology.h"

/* The original private NVIDIA interface is not a firmware-release API.
 * Restrict observational OpenProtocolInformation entries to a fixed cap. */
#define GU_MSI_B850_PRIVATE_OPEN_LIMIT 64U
static inline int GuMsiB850PrivateOpensValid(size_t count, const void *entries) {
  return count<=GU_MSI_B850_PRIVATE_OPEN_LIMIT && (count==0 || entries!=NULL);
}
static inline int GuMsiB850IsOwnerNvidia(const GU_GPU_RECORD *id) {
  return id && id->class_code==0x0300 &&
    ((id->pci_id==0x268410deU && id->subsystem_id==0x88e61043U) ||
     (id->pci_id==0x220a10deU && id->subsystem_id==0xb61219daU));
}

static inline int GuMsiB850KnownRomDiagnosticHardware(const GU_TOPO_CAPTURE *snapshot) {
  size_t i;
  unsigned amd=0, ada=0, ampere=0, other_nvidia=0;
  if(!snapshot || !snapshot->pci_count || snapshot->pci_count>GU_TOPO_MAX_PCI)
    return 0;
  for(i=0;i<snapshot->pci_count;i++) {
    const GU_GPU_RECORD *id=&snapshot->pci[i].identity;
    if((id->class_code & 0xff00U)!=0x0300U)continue;
    if(id->pci_id==0x13c01002U && id->subsystem_id==0x7e561462U)amd++;
    if((id->pci_id & 0xffffU)!=0x10deU)continue;
    if(id->pci_id==0x268410deU && id->subsystem_id==0x88e61043U)ada++;
    else if(id->pci_id==0x220a10deU && id->subsystem_id==0xb61219daU)ampere++;
    else other_nvidia++;
  }
  return amd==1 && ada==1 && ampere==1 && other_nvidia==0;
}
