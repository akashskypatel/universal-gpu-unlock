/* SPDX-License-Identifier: GPL-2.0-only */
/* A changed motherboard fingerprint or missing peer must refuse. */
#include "../src/msi_b850_rom_diag.h"
#include <stdio.h>
#include <string.h>
static GU_TOPO_CAPTURE capture;
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL:%d %s\n",__LINE__,#x);return 1;}}while(0)
static void Setup(void) {
  memset(&capture,0,sizeof(capture));
  capture.pci_count=3;
  capture.pci[0].identity=(GU_GPU_RECORD){.pci_id=0x13c01002U,.subsystem_id=0x7e561462U,.class_code=0x0300};
  capture.pci[1].identity=(GU_GPU_RECORD){.pci_id=0x268410deU,.subsystem_id=0x88e61043U,.class_code=0x0300};
  capture.pci[2].identity=(GU_GPU_RECORD){.pci_id=0x220a10deU,.subsystem_id=0xb61219daU,.class_code=0x0300};
}
int main(void) {
  CHECK(!GuMsiB850KnownRomDiagnosticHardware(NULL));
  Setup();CHECK(GuMsiB850KnownRomDiagnosticHardware(&capture));
  {GU_TOPO_PCI t=capture.pci[1];capture.pci[1]=capture.pci[2];capture.pci[2]=t;}
  CHECK(GuMsiB850KnownRomDiagnosticHardware(&capture));
  Setup();capture.pci[0].identity.subsystem_id=0x11111462U;
  CHECK(!GuMsiB850KnownRomDiagnosticHardware(&capture));
  Setup();capture.pci[0].identity.pci_id=0x11111002U;
  CHECK(!GuMsiB850KnownRomDiagnosticHardware(&capture));
  Setup();capture.pci[1].identity.subsystem_id=0x00001043U;
  CHECK(!GuMsiB850KnownRomDiagnosticHardware(&capture));
  Setup();capture.pci_count=2;
  CHECK(!GuMsiB850KnownRomDiagnosticHardware(&capture));
  Setup();capture.pci_count=4;capture.pci[3]=capture.pci[2];
  CHECK(!GuMsiB850KnownRomDiagnosticHardware(&capture));
  Setup();capture.pci_count=4;capture.pci[3].identity=(GU_GPU_RECORD){.pci_id=0x222210deU,.class_code=0x0300};
  CHECK(!GuMsiB850KnownRomDiagnosticHardware(&capture));
  Setup();capture.pci[1].identity.class_code=0x0200;
  CHECK(!GuMsiB850KnownRomDiagnosticHardware(&capture));
  Setup();capture.pci_count=GU_TOPO_MAX_PCI+1;
  CHECK(!GuMsiB850KnownRomDiagnosticHardware(&capture));
  Setup();capture.pci_count=4;capture.pci[3].identity=(GU_GPU_RECORD){.pci_id=0x12348086U,.class_code=0x0604};
  CHECK(GuMsiB850KnownRomDiagnosticHardware(&capture));
  Setup();
  CHECK(GuMsiB850IsOwnerNvidia(&capture.pci[1].identity));
  CHECK(GuMsiB850IsOwnerNvidia(&capture.pci[2].identity));
  CHECK(!GuMsiB850IsOwnerNvidia(NULL));
  CHECK(!GuMsiB850IsOwnerNvidia(&capture.pci[0].identity));
  capture.pci[2].identity.subsystem_id=0x123419daU;
  CHECK(!GuMsiB850IsOwnerNvidia(&capture.pci[2].identity));
  capture.pci[2].identity.subsystem_id=0xb61219daU;
  capture.pci[2].identity.class_code=0x0200;
  CHECK(!GuMsiB850IsOwnerNvidia(&capture.pci[2].identity));
  CHECK(GuMsiB850PrivateOpensValid(0,NULL));
  CHECK(!GuMsiB850PrivateOpensValid(1,NULL));
  CHECK(GuMsiB850PrivateOpensValid(1,&capture));
  CHECK(GuMsiB850PrivateOpensValid(GU_MSI_B850_PRIVATE_OPEN_LIMIT,&capture));
  CHECK(!GuMsiB850PrivateOpensValid(GU_MSI_B850_PRIVATE_OPEN_LIMIT+1U,&capture));
  CHECK(!GuMsiB850PrivateOpensValid((size_t)-1,&capture));
  printf("MSI_B850_PLATFORM_TESTS=%u PASS\n",checks);
  return 0;
}
