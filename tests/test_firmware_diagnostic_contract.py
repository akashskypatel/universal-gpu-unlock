"""Firmware diagnostic and menu contract checks."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
P=Path(__file__).resolve().parents[1]/"src"
C=(P/"vendor/vbios_610.c").read_text(encoding="utf-8")
B=(P/"boot_io.c").read_text(encoding="utf-8")
A=(P/"app.c").read_text(encoding="utf-8")
V=(P/"version.h").read_text(encoding="utf-8")
class DiagnosticContract(unittest.TestCase):
    def test_bounded_fwsec_raw_read(self):
        descriptor=C.split("static int descriptor(",1)[1].split("int gpu_unlock_vbios_fwsec(",1)[0]
        self.assertLess(descriptor.index("range(length, off, 44)"),descriptor.index("raw descriptor[0:16]"))
        self.assertLess(descriptor.index("range(length, off, size)"),descriptor.index("raw descriptor[0:16]"))
        self.assertIn("SIGNATURE_AUTHENTICATED=0",descriptor)
    def test_msi_parent_image_observation_stays_behind_fingerprint_gate(self):
        source=(P/"gpu_topology_efi.c").read_text(encoding="utf-8")
        report=source.split("static void ReportMsiB850RomVariables(",1)[1].split("\n#endif",1)[0]
        gate="if(!pci_ok || !GuMsiB850KnownRomDiagnosticHardware(&Snapshot))"
        call="ReportMsiB850ImageParents(table->BootServices);"
        self.assertLess(report.index(gate),report.index("return;"))
        self.assertLess(report.index("return;"),report.index(call))
        parent=source.split("static void ReportMsiB850ImageParents(",1)[1].split(
            "static void ReportMsiB850RomVariables(",1)[0]
        self.assertIn("GuMsiB850IsOwnerNvidia(&node->identity)",parent)
        self.assertIn("node->by_driver!=1",parent)
        self.assertIn("child->ParentHandle",parent)
        self.assertIn("bs->HandleProtocol(child->ParentHandle,&LoadedGuid",parent)
        self.assertIn("PARENT_TEXT_SHA256=",parent)
        # The new evidence is the full LoadImage DevicePath, not just the
        # relative LoadedImage.FilePath previously captured by the probe.
        self.assertIn("LoadedImagePathGuid=EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID",source)
        self.assertIn("bs->HandleProtocol(binding->ImageHandle,&LoadedImagePathGuid",parent)
        self.assertIn("SOURCE=LOADED_IMAGE_DEVICE_PATH READ_ONLY=1",parent)
        self.assertIn("loaded_image_path&&PathCopy(loaded_image_path,&path)",parent)
        self.assertIn('SayPath("MSI_B850_LOAD_DEVICE_PATH",i,&path)',parent)
        self.assertNotIn("bs->OpenProtocol(",parent)
        self.assertNotIn("DisconnectController(",parent)
        self.assertNotIn("Stop(",parent)
        self.assertNotIn("StartImage(",parent)
        self.assertNotIn("SetVariable(",parent)

    def test_msi_policy_pointer_read_requires_original_image_and_map(self):
        source=(P/"gpu_topology_efi.c").read_text(encoding="utf-8")
        report=source.split("static void ReportMsiB850RomVariables(",1)[1].split(
            "\n#endif",1)[0]
        self.assertLess(report.index(
            "if(!pci_ok || !GuMsiB850KnownRomDiagnosticHardware(&Snapshot))"),
            report.index("ReportMsiB850PolicyPointers(table->BootServices);"))
        probe=source.split("static void ReportMsiB850PolicyPointers(",1)[1].split(
            "static void ReportMsiB850RomVariables(",1)[0]
        self.assertIn("child->ParentHandle",probe)
        self.assertIn("child->DeviceHandle!=(EFI_HANDLE)node->identity.handle",probe)
        self.assertIn("GuSha256(",probe)
        self.assertIn("memcmp(digest,MsiPciBusTextDigest",probe)
        self.assertIn("bs->GetMemoryMap(",probe)
        self.assertIn("MsiPolicyMapped(map,map_size,desc_size,pci,object_span)",probe)
        self.assertIn("config_id!=node->identity.pci_id",probe)
        self.assertIn("STATE=OBSERVED PCI_OBJECT=%p KIND=%u ENTRY=%p MODE=%u RECORD_OWNER=%p",probe)
        self.assertIn("kind!=expected_kind",probe)
        self.assertIn("obj[0xefU]!=(UINT8)(node->identity.class_code>>8)",probe)
        self.assertLess(probe.index("MsiPolicyMapped(map,map_size,desc_size,pci,object_span)"),
                        probe.index("memcpy(&modeptr,obj+0x3c0U,sizeof(modeptr))"))
        self.assertLess(probe.index("if(method<text_start || method>=text_end"),
                        probe.index("memcpy(&modeptr,obj+0x3c0U,sizeof(modeptr))"))
        self.assertLess(probe.index("MsiPolicyMapped(map,map_size,desc_size,modeptr,0x20U)"),
                        probe.index("mode=entry[0x18U]"))
        for unsafe in ("DisconnectController(", "Stop(", "StartImage(",
                       "SetVariable(", "OpenProtocol(", "Pci.Write(",
                       "Pci.Read(", "Mem.Write(", "Mem.Read("):
            self.assertNotIn(unsafe,probe)

    def test_msi_bds_policy_snapshot_requires_exact_original_image(self):
        source=(P/"gpu_topology_efi.c").read_text(encoding="utf-8")
        report=source.split("static void ReportMsiB850RomVariables(",1)[1].split(
            "\\n#endif",1)[0]
        gate="if(!pci_ok || !GuMsiB850KnownRomDiagnosticHardware(&Snapshot))"
        call="ReportMsiB850BdsPolicyState(table->BootServices);"
        self.assertLess(report.index(gate),report.index("return;"))
        self.assertLess(report.index("return;"),report.index(call))
        probe=source.split("static void ReportMsiB850BdsPolicyState(",1)[1].split(
            "static void ReportMsiB850RomVariables(",1)[0]
        self.assertIn("MsiBdsTextDigest[32]",source)
        self.assertIn("expected_size=113568U,expected_rva=704U,expected_text=89076U;",probe)
        digest_initializer=source.split("static const UINT8 MsiBdsTextDigest[32]={",1)[1].split("};",1)[0]
        digest=bytes(int(word,16) for word in re.findall(r"0x([0-9a-fA-F]{2})\b",digest_initializer))
        self.assertEqual(digest.hex(),"b17dc40833da35c094671ff1da25b1e7ac367daf77637eebc65b7d6cf5f00151")
        self.assertIn("MsiBdsExecutableMapped(map,map_bytes,desc_size,li->ImageBase,expected_size)",probe)
        self.assertIn("GuLoadedPeTextRange(",probe)
        self.assertIn("memcmp(digest,MsiBdsTextDigest,sizeof(digest))",probe)
        self.assertIn("policy!=(VOID *)(base+0x16330U)",probe)
        self.assertIn("revision!=0x10000U",probe)
        self.assertIn("method_set!=(VOID *)(base+0x33e4U)",probe)
        self.assertIn("head=(const UINT8 *)policy+0x70U;",probe)
        self.assertLess(probe.index("memcmp(digest,MsiBdsTextDigest,sizeof(digest))"),
                        probe.index("memcpy(&head_forward,head,sizeof(head_forward))"))
        self.assertLess(probe.index("INTERFACE_METHODS_UNVERIFIED"),
                        probe.index("memcpy(&head_forward,head,sizeof(head_forward))"))
        for action in ("DisconnectController(", "ConnectController(",
                       "Stop(", "StartImage(", "SetVariable(",
                       "OpenProtocol(", "Pci.Write(", "Pci.Read(",
                       "Mem.Write(", "Mem.Read(", "method_get(",
                       "method_set(", "method_class("):
            self.assertNotIn(action,probe)
        self.assertIn("NO_VENDOR_CALLBACK=1 NO_LIST_TRAVERSAL=1",probe)

    def test_opt_in_sec2_fuse_is_independent_read_only_reference(self):
        source=(P/"gpu_topology_efi.c").read_text(encoding="utf-8")
        probe=source.split("static void ReportMsiB850FwsecFuseVersion(",1)[1].split("\n#endif",1)[0]
        gate="if(!pci_ok || !GuMsiB850KnownRomDiagnosticHardware(&Snapshot))"
        assert_owner="!node->openers_known || node->by_driver!=1U"
        self.assertLess(probe.index(gate),probe.index("sec2_status=pci->Mem.Read("))
        self.assertLess(probe.index(assert_owner),probe.index("sec2_status=pci->Mem.Read("))
        self.assertEqual(probe.count("pci->Mem.Read("),2)
        self.assertIn("0x00824148U,1U,&sec2_value",probe)
        self.assertIn("0x008241e0U,1U,&value",probe)
        self.assertIn("MSI_B850_SEC2_FUSE_END",probe)
        self.assertIn("WINDOWS_61742_SIGNATURE_SLOT=UNKNOWN",probe)
        self.assertIn("NO_FALLBACK=1",probe)
        for mutation in ("Mem.Write(", "Pci.Write(", "DisconnectController(", "Stop(", "SetVariable(", "ResetSystem("):
            self.assertNotIn(mutation,probe)

    def test_opt_in_sec2_ucode1_reads_only_matching_original_register(self):
        source=(P/"gpu_topology_efi.c").read_text(encoding="utf-8")
        probe=source.split("static void ReportMsiB850Sec2Ucode1(",1)[1].split("\n#endif",1)[0]
        gate="if(!pci_ok || !GuMsiB850KnownRomDiagnosticHardware(&Snapshot))"
        assert_owner="!node->openers_known || node->by_driver!=1U"
        assert_location="id->segment!=0U || id->bus!=expected_bus"
        register_read="status=pci->Mem.Read(pci,EfiPciIoWidthUint32,0U,0x00824140U,1U,&raw)"
        for check in (gate, assert_owner, assert_location):
            self.assertLess(probe.index(check),probe.index(register_read))
        self.assertEqual(probe.count("pci->Mem.Read("),1)
        self.assertIn(register_read,probe)
        self.assertIn("if(!GuMsiB850IsOwnerNvidia(id))continue;",probe)
        self.assertIn("!node->first_owner_agent",probe)
        self.assertIn("0x268410deU",probe)
        self.assertIn("MSI_B850_SEC2_UCODE1_END READS_SUCCEEDED=%u EXPECTED=2",probe)
        self.assertIn("NO_FALLBACK=1",probe)
        self.assertNotIn("0x00824148U",probe)
        self.assertNotIn("0x008241e0U",probe)
        for mutation in ("Mem.Write(", "Pci.Write(", "DisconnectController(",
                         "Stop(", "SetVariable(", "ResetSystem(", "Stall("):
            self.assertNotIn(mutation,probe)
        self.assertIn("#ifdef GU_MSI_B850_SEC2_UCODE1\n  ReportMsiB850Sec2Ucode1(pci_ok);\n#endif",source)
        build=(P/"Build-Gpu-Topology-Diagnostic.ps1").read_text(encoding="utf-8")
        self.assertIn("if ($MsiB850Sec2Ucode1 -and -not $SaveLog)",build)
        self.assertIn("/DGU_MSI_B850_SEC2_UCODE1",build)
        self.assertIn("MSI-B850-SEC2-UCODE1-DIAGNOSTIC.EFI",build)
        self.assertIn("MSI-B850-SEC2-UCODE1-WINDOWS-HANDOFF-DIAGNOSTIC.EFI",build)

    def test_passive_selection_uses_runtime_inventory(self):
        source=(P/"gpu_topology_efi.c").read_text(encoding="utf-8")
        reader=source.split("static int SelectionRead(",1)[1].split("\n#endif",1)[0]
        reader="static int SelectionRead("+reader
        select=source.split("static int ReportPassiveSelections(",1)[1].split("\n#endif",1)[0]
        select="static int ReportPassiveSelections("+select
        program = r"""
#include "gpu_topology.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void VOID;
typedef int EFI_STATUS;
#define EFI_SUCCESS 0
#define EfiLoaderData 1
typedef struct {
  EFI_STATUS (*AllocatePool)(int,size_t,VOID **);
  EFI_STATUS (*FreePool)(VOID *);
} EFI_BOOT_SERVICES;
#define GU_GPU_SELECTION_COUNT 18U
static GU_GPU_LOCATION SelectedLocations[GU_GPU_SELECTION_COUNT];
static GU_TOPO_CAPTURE Snapshot;
static GU_TOPO_REPORT Report;
static GU_TOPO_GPU_REPORT gpu_reports[GU_GPU_SELECTION_COUNT];
static GU_TOPO_GOP existing_gop;
static int fail_alloc,fail_free,allocs,frees,steps;
static GU_GPU_RECORD uncertain_inventory;
static int uncertain_frees;
static EFI_STATUS Allocate(int type,size_t bytes,VOID **out) {
  if(fail_alloc==2) {*out=&uncertain_inventory;return 1;}
  if(fail_alloc==3) {*out=gpu_reports;return EFI_SUCCESS;}
  if(fail_alloc==4) {*out=&existing_gop;return EFI_SUCCESS;}
  if(type!=EfiLoaderData || fail_alloc)return 1;
  *out=calloc(1,bytes);
  if(!*out)return 1;
  allocs++;
  return EFI_SUCCESS;
}
static EFI_STATUS Free(VOID *p) {
  if(p==&uncertain_inventory || p==gpu_reports || p==&existing_gop) {uncertain_frees++;return 1;}
  free(p);frees++;return fail_free?1:EFI_SUCCESS;
}
static void Say(const char *fmt,...) {
  if(strstr(fmt,"=BEGIN BDF="))steps++;
}
""" + reader + select + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void) {
  EFI_BOOT_SERVICES bs={Allocate,Free};
  Snapshot.pci_count=19;Report.gpu_count=18;Report.gpus=gpu_reports;
  for(size_t i=0;i<Snapshot.pci_count;i++) {
    GU_GPU_RECORD *r=&Snapshot.pci[i].identity;
    r->handle=&Snapshot.pci[i];r->pci=&Snapshot.pci[i].path;
    r->pci_id=i?0x268410deU:0x164e1002U;
    r->class_code=0x0300;r->bus=(uint32_t)i;
    if(i)gpu_reports[i-1].gpu_index=i;
  }
  for(size_t i=0;i<18;i++)SelectedLocations[i].bus=(uint32_t)(18-i);
  CHECK(ReportPassiveSelections(&bs,1,GU_TOPO_COMPLETE));
  CHECK(steps==18 && allocs==1 && frees==1);
  steps=0;fail_alloc=1;
  CHECK(!ReportPassiveSelections(&bs,1,GU_TOPO_COMPLETE));
  CHECK(!steps && allocs==1 && frees==1);
  memset(&uncertain_inventory,0xa5,sizeof(uncertain_inventory));
  GU_GPU_RECORD original_uncertain;
  memcpy(&original_uncertain,&uncertain_inventory,sizeof(original_uncertain));
  fail_alloc=2;
  CHECK(!ReportPassiveSelections(&bs,1,GU_TOPO_COMPLETE));
  CHECK(!steps && allocs==1 && frees==1 && !uncertain_frees);
  CHECK(!memcmp(&original_uncertain,&uncertain_inventory,sizeof(original_uncertain)));
  Snapshot.gop=&existing_gop;
  Snapshot.gop_count=Snapshot.gop_capacity=1;
  GU_TOPO_GPU_REPORT original_reports[GU_GPU_SELECTION_COUNT];
  GU_TOPO_GOP original_gop;
  memcpy(original_reports,gpu_reports,sizeof(original_reports));
  memset(&existing_gop,0xa5,sizeof(existing_gop));
  memcpy(&original_gop,&existing_gop,sizeof(original_gop));
  for(fail_alloc=3;fail_alloc<=4;fail_alloc++) {
    CHECK(!ReportPassiveSelections(&bs,1,GU_TOPO_COMPLETE));
    CHECK(!steps && allocs==1 && frees==1 && !uncertain_frees);
    CHECK(!memcmp(original_reports,gpu_reports,sizeof(original_reports)));
    CHECK(!memcmp(&original_gop,&existing_gop,sizeof(original_gop)));
  }
  Snapshot.gop=NULL;
  Snapshot.gop_count=Snapshot.gop_capacity=0;
  fail_alloc=0;fail_free=1;
  CHECK(!ReportPassiveSelections(&bs,1,GU_TOPO_COMPLETE));
  CHECK(!steps && allocs==2 && frees==2);
  fail_free=0;
  SelectedLocations[17]=SelectedLocations[0];
  CHECK(!ReportPassiveSelections(&bs,1,GU_TOPO_COMPLETE));
  CHECK(!steps && allocs==3 && frees==3);
  SelectedLocations[17].bus=99;
  CHECK(!ReportPassiveSelections(&bs,1,GU_TOPO_COMPLETE));
  CHECK(!steps && allocs==4 && frees==4);
  SelectedLocations[17].bus=1;
  gpu_reports[0].gpu_index=2;
  CHECK(!ReportPassiveSelections(&bs,1,GU_TOPO_COMPLETE));
  CHECK(!steps && allocs==5 && frees==5);
  gpu_reports[0].gpu_index=1;Report.gpus=NULL;
  CHECK(!ReportPassiveSelections(&bs,1,GU_TOPO_COMPLETE));
  CHECK(!steps && allocs==5 && frees==5);
  Report.gpus=gpu_reports;
  CHECK(!ReportPassiveSelections(&bs,1,GU_TOPO_INVALID));
  CHECK(!steps && allocs==5 && frees==5);
  CHECK(!ReportPassiveSelections(&bs,0,GU_TOPO_COMPLETE));
  CHECK(!steps && allocs==5 && frees==5);
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/"selection.c"
            c.write_text(program,encoding="utf-8")
            for compiler in ("gcc","clang"):
                with self.subTest(compiler=compiler):
                    binary=Path(tmp)/compiler
                    build=subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror","-pedantic","-I",str(P),str(c),str(P/"gpu_inventory.c"),"-o",str(binary)],capture_output=True,text=True)
                    self.assertEqual(build.returncode,0,build.stderr)
                    run=subprocess.run([str(binary)],capture_output=True,text=True)
                    self.assertEqual(run.returncode,0,run.stdout+run.stderr)

    def test_gop_capture_owns_runtime_storage(self):
        source=(P/"gpu_topology_efi.c").read_text(encoding="utf-8")
        capture="static int CaptureGopRecords("+source.split("static int CaptureGopRecords(",1)[1].split("\n#ifdef GU_TOPO_SAVE_LOG",1)[0]
        program = r"""
#include "gpu_topology.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void VOID;
typedef void *EFI_HANDLE;
typedef size_t UINTN;
typedef int EFI_STATUS;
typedef int EFI_GUID;
typedef GU_TOPO_PATH EFI_DEVICE_PATH_PROTOCOL;
typedef int EFI_GRAPHICS_OUTPUT_PROTOCOL;
#define EFI_SUCCESS 0
/* EFI warnings are nonzero but are not EFI errors. */
#define EFI_ERROR(x) ((x)<0)
#define EfiLoaderData 1
#define ByProtocol 2
typedef struct {
  EFI_STATUS (*LocateHandleBuffer)(int,EFI_GUID *,VOID *,UINTN *,EFI_HANDLE **);
  EFI_STATUS (*HandleProtocol)(EFI_HANDLE,EFI_GUID *,VOID **);
  EFI_STATUS (*AllocatePool)(int,UINTN,VOID **);
  EFI_STATUS (*FreePool)(VOID *);
} EFI_BOOT_SERVICES;
static EFI_GUID GopGuid,PathGuid;
static GU_TOPO_CAPTURE Snapshot;
static GU_TOPO_PATH paths[40];
static int protocols[40],allocs,frees,fail_alloc,fail_read,alias,drift,fail_free;
static int fail_locate,uncertain_frees,reads,allocation_alias,enumeration_alias,path_copies;
static EFI_HANDLE *located;
static EFI_HANDLE uncertain_handles[40];
static GU_TOPO_GOP uncertain_records[40];
static UINTN total=40;
static EFI_STATUS Allocate(int kind,UINTN size,VOID **out) {
  (void)kind;
  if(fail_alloc==2) {*out=uncertain_records;return 1;}
  if(fail_alloc)return 1;
  if(allocation_alias==1){*out=located;return EFI_SUCCESS;}
  if(allocation_alias==2){*out=Snapshot.gop;return EFI_SUCCESS;}
  *out=calloc(1,size);
  if(!*out)return 1;
  allocs++;return EFI_SUCCESS;
}
static EFI_STATUS Free(VOID *p) {
  if(p==uncertain_records || p==uncertain_handles) {uncertain_frees++;return 1;}
  free(p);frees++;
  return fail_free?1:EFI_SUCCESS;
}
static EFI_STATUS Locate(int kind,EFI_GUID *guid,VOID *key,UINTN *n,EFI_HANDLE **out) {
  (void)kind;(void)guid;(void)key;
  *n=total;
  if(fail_locate) {*out=uncertain_handles;return 1;}
  if(enumeration_alias){*out=(EFI_HANDLE *)(void *)Snapshot.gop;return EFI_SUCCESS;}
  if(!total)return EFI_SUCCESS;
  if(total>40)return 1;
  *out=calloc(total,sizeof(**out));
  if(!*out)return 1;
  allocs++;
  for(size_t i=0;i<total;i++)(*out)[i]=&paths[i];
  located=*out;
  return EFI_SUCCESS;
}
static EFI_STATUS Handle(EFI_HANDLE handle,EFI_GUID *guid,VOID **out) {
  size_t i;
  reads++;
  if(fail_locate || fail_alloc==2)return 1;
  i=(GU_TOPO_PATH *)handle-paths;
  if(fail_read==1 && i==39)return -1;
  if(guid==&GopGuid)*out=&protocols[(alias&&i==39)?0:(drift&&i==39)?38:i];
  else *out=&paths[i];
  if(i==0 && ((fail_read==2&&guid==&GopGuid)||(fail_read==3&&guid==&PathGuid)))return 1;
  return EFI_SUCCESS;
}
static int PathCopy(EFI_DEVICE_PATH_PROTOCOL *path,GU_TOPO_PATH *out) {
  path_copies++;
  if(!GuTopoPathValid(path))return 0;
  *out=*path;return 1;
}
static void Say(const char *fmt,...) {(void)fmt;}
""" + capture + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void) {
  EFI_BOOT_SERVICES bs={Locate,Handle,Allocate,Free};
  for(size_t i=0;i<40;i++) {
    const uint8_t bytes[]={1,1,6,0,0,(uint8_t)i,0x7f,0xff,4,0};
    memcpy(paths[i].bytes,bytes,sizeof(bytes));paths[i].length=sizeof(bytes);
  }
  CHECK(CaptureGop(&bs));
  CHECK(Snapshot.gop_count==40 && Snapshot.gop_capacity==40 && allocs-frees==1);
  CHECK(!CaptureGop(&bs) && allocs-frees==1);
  CHECK(RevalidateGop(&bs) && allocs-frees==1);
  GU_TOPO_GOP original[40];
  memcpy(original,Snapshot.gop,sizeof(original));
  reads=0;enumeration_alias=1;
  CHECK(!RevalidateGop(&bs)&&!reads&&allocs-frees==1);
  CHECK(!memcmp(original,Snapshot.gop,sizeof(original)));
  enumeration_alias=0;allocation_alias=2;
  CHECK(!RevalidateGop(&bs)&&!reads&&allocs-frees==1);
  CHECK(!memcmp(original,Snapshot.gop,sizeof(original)));
  allocation_alias=1;
  CHECK(!RevalidateGop(&bs)&&!reads&&allocs-frees==1);
  CHECK(!memcmp(original,Snapshot.gop,sizeof(original)));
  allocation_alias=0;
  paths[39].bytes[5]=1;
  CHECK(!RevalidateGop(&bs) && allocs-frees==1);
  paths[39].bytes[5]=39;
  drift=1;
  CHECK(!RevalidateGop(&bs) && allocs-frees==1);
  drift=0;free(Snapshot.gop);frees++;
  memset(&Snapshot,0,sizeof(Snapshot));
  allocation_alias=1;reads=0;
  CHECK(!CaptureGop(&bs)&&!Snapshot.gop&&allocs==frees&&!reads);
  allocation_alias=0;fail_alloc=1;
  CHECK(!CaptureGop(&bs) && !Snapshot.gop && allocs==frees);
  fail_alloc=2;
  memset(uncertain_records,0xa5,sizeof(uncertain_records));
  GU_TOPO_GOP original_uncertain[40];
  memcpy(original_uncertain,uncertain_records,sizeof(original_uncertain));
  reads=0;
  CHECK(!CaptureGop(&bs) && !Snapshot.gop && allocs==frees && !reads && !uncertain_frees);
  CHECK(!memcmp(original_uncertain,uncertain_records,sizeof(original_uncertain)));
  fail_alloc=0;fail_locate=1;
  CHECK(!CaptureGop(&bs) && !Snapshot.gop && allocs==frees && !reads && !uncertain_frees);
  fail_locate=0;fail_read=1;
  CHECK(!CaptureGop(&bs) && !Snapshot.gop && allocs==frees);
  fail_read=2;reads=path_copies=0;
  CHECK(!CaptureGop(&bs) && !Snapshot.gop && allocs==frees && reads==1 && !path_copies);
  fail_read=3;reads=path_copies=0;
  CHECK(!CaptureGop(&bs) && !Snapshot.gop && allocs==frees && reads==2 && !path_copies);
  fail_read=0;alias=1;
  CHECK(!CaptureGop(&bs) && !Snapshot.gop && allocs==frees);
  alias=0;fail_free=1;
  CHECK(!CaptureGop(&bs) && !Snapshot.gop && allocs==frees);
  fail_free=0;total=0;
  CHECK(CaptureGop(&bs) && !Snapshot.gop && !Snapshot.gop_count);
  CHECK(RevalidateGop(&bs) && allocs==frees);
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/"capture.c"
            c.write_text(program,encoding="utf-8")
            for compiler in ("gcc","clang"):
                with self.subTest(compiler=compiler):
                    binary=Path(tmp)/compiler
                    build=subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror","-pedantic","-I",str(P),str(c),str(P/"gpu_topology.c"),"-o",str(binary)],capture_output=True,text=True)
                    self.assertEqual(build.returncode,0,build.stderr)
                    run=subprocess.run([str(binary)],capture_output=True,text=True)
                    self.assertEqual(run.returncode,0,run.stdout+run.stderr)


    def test_strict_pci_observation_pool_ownership(self):
        source=(P/"gpu_topology_efi.c").read_text(encoding="utf-8")
        openers=source.split("static int ReadPciOpeners(",1)[1].split("\nstatic int ReadPci(",1)[0]
        openers="static int ReadPciOpeners("+openers
        capture=source.split("static int CapturePci(",1)[1].split("\n/* Adapt the bounded AllHandles",1)[0]
        capture="static int CapturePci("+capture
        self.assertIn("if(!ReadPciOpeners(bs,h,node))return 0;",source)
        program = r"""
#include "gpu_topology.h"
#include <stdio.h>
#include <string.h>
typedef void VOID;
typedef void *EFI_HANDLE;
typedef size_t UINTN;
typedef int EFI_STATUS;
typedef int EFI_GUID;
#define EFI_SUCCESS 0
#define EFI_UNSUPPORTED 1
#define EFI_COMPROMISED_DATA 2
#define EFI_OPEN_PROTOCOL_BY_DRIVER 16U
#define ByProtocol 1
typedef struct { EFI_HANDLE AgentHandle; unsigned Attributes; } EFI_OPEN_PROTOCOL_INFORMATION_ENTRY;
typedef struct {
  EFI_STATUS (*OpenProtocolInformation)(EFI_HANDLE,EFI_GUID *,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **,UINTN *);
  EFI_STATUS (*LocateHandleBuffer)(int,EFI_GUID *,VOID *,UINTN *,EFI_HANDLE **);
  EFI_STATUS (*FreePool)(VOID *);
} EFI_BOOT_SERVICES;
static EFI_GUID PciGuid;
static GU_TOPO_CAPTURE Snapshot;
static EFI_OPEN_PROTOCOL_INFORMATION_ENTRY entries[2],uncertain;
static EFI_HANDLE handles[2],uncertain_handles[2];
static int mode,reads,frees,foreign_frees,failures;
static void Say(const char *fmt,...) {(void)fmt;}
static int PciCaptureFailure(const char *stage,EFI_STATUS status) {(void)stage;(void)status;failures++;return 0;}
static EFI_STATUS Openers(EFI_HANDLE h,EFI_GUID *guid,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **p,UINTN *n) {
  (void)h;(void)guid;
  if(mode==1) {*p=&uncertain;*n=1;return 1;}
  if(mode==2)return 1;
  if(mode==3) {*p=entries;*n=1025;return 0;}
  if(mode==4) {*n=1;return 0;}
  *p=entries;*n=2;return 0;
}
static EFI_STATUS Locate(int search,EFI_GUID *guid,VOID *key,UINTN *n,EFI_HANDLE **p) {
  (void)search;(void)guid;(void)key;
  *n=2;
  if(mode==6) {*p=uncertain_handles;return 1;}
  if(mode==7)return 1;
  *p=handles;
  if(mode==8)*n=0;
  return 0;
}
static EFI_STATUS Release(VOID *p) {
  if(p==&uncertain || p==uncertain_handles){foreign_frees++;return 1;}
  frees++;return mode==5?1:0;
}
static int ReadPci(EFI_BOOT_SERVICES *bs,EFI_HANDLE h,GU_TOPO_PCI *node) {
  (void)bs;(void)h;
  reads++;memset(node,0,sizeof(*node));return 1;
}
""" + openers + capture + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void) {
  EFI_BOOT_SERVICES bs={Openers,Locate,Release};
  GU_TOPO_PCI node={0};
  entries[0].Attributes=EFI_OPEN_PROTOCOL_BY_DRIVER;
  entries[0].AgentHandle=&node;
  CHECK(ReadPciOpeners(&bs,&node,&node) && node.openers_known && node.by_driver==1 && node.first_owner_agent==&node && frees==1);
  for(mode=1;mode<=5;mode++) {
    memset(&node,0,sizeof(node));frees=failures=0;
    CHECK(ReadPciOpeners(&bs,&node,&node)==(mode==2));
    CHECK(!node.openers_known && !node.by_driver && !node.first_owner_agent && !foreign_frees);
    CHECK(frees==(mode==3 || mode==5) && failures==(mode!=2));
  }
  for(mode=6;mode<=8;mode++) {
    memset(&Snapshot,0,sizeof(Snapshot));reads=frees=0;
    CHECK(!CapturePci(&bs) && !Snapshot.pci_count && !reads && !foreign_frees && frees==(mode==8));
  }
  mode=5;reads=frees=0;
  CHECK(!CapturePci(&bs) && reads==2 && frees==1);
  mode=0;reads=frees=0;memset(&Snapshot,0,sizeof(Snapshot));
  CHECK(CapturePci(&bs) && Snapshot.pci_count==2 && reads==2 && frees==1);
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/"pci_observation.c"
            c.write_text(program,encoding="utf-8")
            for compiler in ("gcc","clang"):
                with self.subTest(compiler=compiler):
                    binary=Path(tmp)/compiler
                    build=subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror","-pedantic","-I",str(P),str(c),"-o",str(binary)],capture_output=True,text=True)
                    self.assertEqual(build.returncode,0,build.stderr)
                    run=subprocess.run([str(binary)],capture_output=True,text=True)
                    self.assertEqual(run.returncode,0,run.stdout+run.stderr)


    def test_supplementary_protocol_pool_ownership(self):
        source=(P/"gpu_topology_efi.c").read_text(encoding="utf-8")
        helpers=source.split("static int ObservationPoolFault;",1)[1].split("\n#ifdef GU_MSI_B850_FUSE_VERSION",1)[0]
        helpers="static int ObservationPoolFault;"+helpers
        for stage in ("msi-private","textout","nvidia-pci","nvidia-gop","independent-gop"):
            self.assertRegex(source, r'ObservationOpeners\([^;]*"'+stage+r'"\);')
            self.assertRegex(source, r'ObservationPoolRelease\([^;]*"'+stage+r'"\);')
        self.assertIn('if(!ObservationPoolConfirmed(status,handles,"msi-bds-handles"))handles=NULL;',source)
        self.assertIn('ObservationPoolRelease(bs,handles,"msi-bds-handles")',source)
        self.assertIn('ObservationOpeners(bs,handle,&TextOutGuid,&entries,&count,outer,"textout")',source)
        self.assertEqual(source.count('"GOP_HANDLE",opens);'),2)
        self.assertEqual(source.count('"GOP_OPEN_CONTROLLER",opens);'),2)
        program=r"""
#include <stdio.h>
#include <stddef.h>
typedef void VOID;
typedef void *EFI_HANDLE;
typedef int EFI_GUID;
typedef int EFI_STATUS;
typedef size_t UINTN;
#define EFI_SUCCESS 0
#define EFI_DEVICE_ERROR 1
typedef struct {unsigned marker;} EFI_OPEN_PROTOCOL_INFORMATION_ENTRY;
typedef struct {
    EFI_STATUS (*OpenProtocolInformation)(EFI_HANDLE,EFI_GUID *,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **,UINTN *);
    EFI_STATUS (*FreePool)(VOID *);
} EFI_BOOT_SERVICES;
static EFI_OPEN_PROTOCOL_INFORMATION_ENTRY outer={42}, uncertain={99};
static struct {VOID *gop;} Snapshot;
static struct {VOID *gpus;} Report;
static int mode,calls,releases,foreign_frees,fail_release,logs;
static void Say(const char *format,...) {(void)format;logs++;}
static EFI_STATUS Open(EFI_HANDLE h,EFI_GUID *g,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **out,UINTN *count) {
    (void)h;(void)g;calls++;
    if(mode==1){*out=&uncertain;*count=1;return 1;}
    if(mode==2)return 1;
    if(mode==3){*out=&uncertain;*count=1;return 2;}
    *out=&outer;*count=1;return 0;
}
static EFI_STATUS Release(VOID *p) {
    if(p!=&outer){foreign_frees++;return 1;}
    releases++;return fail_release?1:0;
}
""" + helpers + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void) {
    EFI_BOOT_SERVICES bs={Open,Release};
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *p=NULL,*nested=NULL;
    UINTN count=0,n=0;
    CHECK(ObservationOpeners(&bs,NULL,NULL,&p,&count,NULL,"outer")==0 && p==&outer && count==1);
    mode=1;
    CHECK(ObservationOpeners(&bs,NULL,NULL,&nested,&n,NULL,"nested")==1 && !nested && ObservationPoolFault && logs==1);
    CHECK(ObservationOpeners(&bs,NULL,NULL,&nested,&n,NULL,"later")==1 && !nested && !n && calls==2);
    CHECK(ObservationPoolRelease(&bs,nested,"nested")==0 && releases==0);
    CHECK(ObservationPoolRelease(&bs,p,"outer")==0 && releases==1 && !foreign_frees && outer.marker==42 && uncertain.marker==99);
    ObservationPoolFault=0;mode=2;
    CHECK(ObservationOpeners(&bs,NULL,NULL,&p,&count,NULL,"ordinary")==1 && !p && !ObservationPoolFault);
    mode=0;
    CHECK(ObservationOpeners(&bs,NULL,NULL,&p,&count,NULL,"normal")==0 && p==&outer);
    fail_release=1;
    CHECK(ObservationPoolRelease(&bs,p,"normal")==1 && ObservationPoolFault && releases==2);
    CHECK(ObservationOpeners(&bs,NULL,NULL,&p,&count,NULL,"blocked")==1 && !p && !count && calls==4);
    ObservationPoolFault=0;mode=3;
    CHECK(ObservationOpeners(&bs,NULL,NULL,&p,&count,NULL,"warning")==2 && !p && ObservationPoolFault);
    CHECK(!foreign_frees && releases==2 && uncertain.marker==99);
    ObservationPoolFault=0;mode=0;fail_release=0;
    CHECK(ObservationOpeners(&bs,NULL,NULL,&p,&count,NULL,"parent")==0 && p==&outer);
    CHECK(ObservationOpeners(&bs,NULL,NULL,&nested,&n,p,"alias")==1 && !nested && ObservationPoolFault);
    CHECK(ObservationPoolRelease(&bs,nested,"alias")==0 && releases==2);
    CHECK(ObservationPoolRelease(&bs,p,"parent")==0 && releases==3);
    ObservationPoolFault=0;Snapshot.gop=&outer;
    CHECK(ObservationOpeners(&bs,NULL,NULL,&p,&count,NULL,"live-gop")==1 && !p && ObservationPoolFault);
    CHECK(ObservationPoolRelease(&bs,p,"live-gop")==0 && releases==3);
    ObservationPoolFault=0;Snapshot.gop=NULL;Report.gpus=&outer;
    CHECK(ObservationOpeners(&bs,NULL,NULL,&p,&count,NULL,"live-report")==1 && !p && ObservationPoolFault);
    CHECK(ObservationPoolRelease(&bs,p,"live-report")==0 && releases==3);
    CHECK(!foreign_frees && outer.marker==42 && uncertain.marker==99);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/"protocol_pool.c"
            c.write_text(program,encoding="utf-8")
            for compiler in ("gcc","clang"):
                with self.subTest(compiler=compiler):
                    binary=Path(tmp)/compiler
                    build=subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror","-pedantic",str(c),"-o",str(binary)],capture_output=True,text=True)
                    self.assertEqual(build.returncode,0,build.stderr)
                    run=subprocess.run([str(binary)],capture_output=True,text=True)
                    self.assertEqual(run.returncode,0,run.stdout+run.stderr)



    def test_incomplete_text_output_observation_propagates(self):
        source=(P/"gpu_topology_efi.c").read_text(encoding="utf-8")
        actual="static int ReportTextOutputOpeners("+source.split("static int ReportTextOutputOpeners(",1)[1].split("\n/* Read the current PCI controller opener graph.",1)[0]
        self.assertEqual(source.count("unresolved_text+=!ReportTextOutputOpeners("),4)
        self.assertIn("!unresolved_agents&&!unresolved_text&&!ObservationPoolFault",source)
        self.assertIn("if(freed!=EFI_SUCCESS||!by_driver||unresolved_text||ObservationPoolFault)unresolved++;",source)
        program=r"""
#include <stdio.h>
#include <stddef.h>
typedef void VOID;
typedef void *EFI_HANDLE;
typedef int EFI_STATUS;
typedef int EFI_GUID;
typedef size_t UINTN;
typedef int EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef struct {size_t length;} GU_TOPO_PATH;
typedef struct {EFI_HANDLE ImageHandle;} EFI_DRIVER_BINDING_PROTOCOL;
typedef struct {size_t ImageSize;const void *FilePath;} EFI_LOADED_IMAGE_PROTOCOL;
typedef struct {unsigned Attributes,OpenCount;EFI_HANDLE AgentHandle,ControllerHandle;} EFI_OPEN_PROTOCOL_INFORMATION_ENTRY;
#define EFI_SUCCESS 0
#define EFI_UNSUPPORTED 1
#define EFI_DEVICE_ERROR 2
#define EFI_OPEN_PROTOCOL_BY_DRIVER 16U
#define EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER 8U
typedef struct EFI_BOOT_SERVICES {
 EFI_STATUS (*HandleProtocol)(EFI_HANDLE,EFI_GUID *,VOID **);
 EFI_STATUS (*OpenProtocolInformation)(EFI_HANDLE,EFI_GUID *,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **,UINTN *);
 EFI_STATUS (*FreePool)(VOID *);
} EFI_BOOT_SERVICES;
static EFI_GUID TextOutGuid,DriverBindingGuid,LoadedGuid;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL text;
static EFI_DRIVER_BINDING_PROTOCOL binding;
static EFI_LOADED_IMAGE_PROTOCOL loaded;
static EFI_OPEN_PROTOCOL_INFORMATION_ENTRY entry;
static int mode,ObservationPoolFault,open_calls,release_calls,borrowed_reads;
static void Say(const char *fmt,...){(void)fmt;}
static void SayPath(const char *s,size_t n,const GU_TOPO_PATH *p){(void)s;(void)n;(void)p;}
static int PathCopy(const void *p,GU_TOPO_PATH *path){(void)p;path->length=1;return mode!=12;}
static void ReportSharedConsoleMappedText(EFI_HANDLE h,EFI_LOADED_IMAGE_PROTOCOL *p,const char *s){(void)h;(void)p;(void)s;}
static EFI_STATUS Handle(EFI_HANDLE h,EFI_GUID *guid,VOID **out){
 (void)h;borrowed_reads++;*out=NULL;
 if(guid==&TextOutGuid){
   if(mode==1)return EFI_UNSUPPORTED;
   if(mode==2)return EFI_DEVICE_ERROR;
   if(mode==3){*out=&text;return 3;}
   if(mode==4)return EFI_SUCCESS;
   *out=&text;return EFI_SUCCESS;
 }
 if(guid==&DriverBindingGuid){if(mode==9)return EFI_DEVICE_ERROR;*out=&binding;return EFI_SUCCESS;}
 if(mode==10)return EFI_DEVICE_ERROR;
 *out=&loaded;return EFI_SUCCESS;
}
static EFI_STATUS Open(EFI_HANDLE h,EFI_GUID *g,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **out,UINTN *count){
 (void)h;(void)g;open_calls++;*out=NULL;*count=0;
 if(mode==6)return EFI_DEVICE_ERROR;
 if(mode==0)return EFI_SUCCESS;
 *out=&entry;*count=mode==7?257:1;return EFI_SUCCESS;
}
static EFI_STATUS Free(VOID *p){if(p!=&entry)return EFI_DEVICE_ERROR;release_calls++;return mode==13?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS ObservationOpeners(EFI_BOOT_SERVICES *bs,EFI_HANDLE h,EFI_GUID *g,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **out,UINTN *count,const VOID *outer,const char *stage){
 (void)outer;(void)stage;return bs->OpenProtocolInformation(h,g,out,count);
}
static EFI_STATUS ObservationPoolRelease(EFI_BOOT_SERVICES *bs,VOID *p,const char *stage){(void)stage;return p?bs->FreePool(p):EFI_SUCCESS;}
""" + actual + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d mode %d: %s\n",__LINE__,mode,#x);return 1;}}while(0)
int main(void){
 EFI_BOOT_SERVICES bs={Handle,Open,Free};
 binding.ImageHandle=&binding;loaded.FilePath=&loaded;
 entry.Attributes=EFI_OPEN_PROTOCOL_BY_DRIVER;
 for(mode=0;mode<=13;mode++){
   open_calls=release_calls=borrowed_reads=0;ObservationPoolFault=0;
   entry.AgentHandle=mode==8?NULL:&binding;
   loaded.FilePath=mode==11?NULL:&loaded;
   bs.OpenProtocolInformation=mode==5?NULL:Open;
   CHECK(ReportTextOutputOpeners(&bs,"TEST",0,0,0,&text,"fixture",NULL)==(mode==0||mode==1));
   CHECK(open_calls==(mode==0||mode>=6));
   CHECK(release_calls==(mode>=7));
   if(mode>=1&&mode<=5)CHECK(borrowed_reads==1);
 }
 mode=14;open_calls=release_calls=borrowed_reads=0;
 CHECK(ReportTextOutputOpeners(&bs,"TEST",0,0,0,&text,"fixture",NULL));
 CHECK(open_calls==1&&release_calls==1&&borrowed_reads==3);
 ObservationPoolFault=1;
 CHECK(!ReportTextOutputOpeners(&bs,"TEST",0,0,0,&text,"fixture",NULL));
 CHECK(open_calls==1&&release_calls==1&&borrowed_reads==3);
 return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/"text_output.c"
            c.write_text(program,encoding="utf-8")
            for compiler in ("gcc","clang"):
                with self.subTest(compiler=compiler):
                    binary=Path(tmp)/compiler
                    build=subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror","-pedantic",str(c),"-o",str(binary)],capture_output=True,text=True)
                    self.assertEqual(build.returncode,0,build.stderr)
                    run=subprocess.run([str(binary)],capture_output=True,text=True)
                    self.assertEqual(run.returncode,0,run.stdout+run.stderr)



    def test_isolated_descriptor_pool_ownership(self):
        source=(P/"gpu_prom_capture_efi.c").read_text(encoding="utf-8")
        actual="static UINTN UncertainBarResourceAddress;"+source.split("static UINTN UncertainBarResourceAddress;",1)[1].split("\nEFI_STATUS GuPromCapturePrepared(",1)[0]
        program=r"""
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t UINT8;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uintptr_t UINTN;
typedef uint64_t EFI_STATUS;
typedef void VOID;
#define EFI_SUCCESS 0ULL
#define EFI_DEVICE_ERROR (0x8000000000000007ULL)
#define ACPI_ADDRESS_SPACE_DESCRIPTOR 0x8a
#define ACPI_END_TAG_DESCRIPTOR 0x79
typedef struct {
 UINT8 Desc,ResType;
 unsigned Len,AddrSpaceGranularity;
 UINT64 AddrRangeMin,AddrLen,AddrTranslationOffset,AddrRangeMax;
} EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR;
typedef struct {EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR descriptor;UINT8 end[2];} RESOURCE;
typedef struct io EFI_PCI_IO_PROTOCOL;
struct io{
 EFI_STATUS (*GetBarAttributes)(EFI_PCI_IO_PROTOCOL *,unsigned,VOID *,VOID **);
 struct {int (*Read)(void);} Mem;
};
typedef struct {EFI_STATUS (*FreePool)(VOID *);} EFI_BOOT_SERVICES;
typedef struct {EFI_BOOT_SERVICES *BootServices;} EFI_SYSTEM_TABLE;
static RESOURCE resource;
static UINT8 unconfirmed[sizeof(RESOURCE)];
static int mode,gets,frees,foreign_frees;
static int NeverRead(void){return 0;}
static EFI_STATUS Attributes(EFI_PCI_IO_PROTOCOL *io,unsigned bar,VOID *supports,VOID **raw){
 (void)io;(void)bar;(void)supports;
 gets++;
 if(mode==1||mode==2){*raw=unconfirmed;return mode==1?EFI_DEVICE_ERROR:1;}
 if(mode==3)return EFI_DEVICE_ERROR;
 if(mode==4)return EFI_SUCCESS;
 *raw=&resource.descriptor;return EFI_SUCCESS;
}
static EFI_STATUS Release(VOID *p){
 if(p!=&resource.descriptor){foreign_frees++;return EFI_DEVICE_ERROR;}
 frees++;return mode==5?EFI_DEVICE_ERROR:mode==6?1:EFI_SUCCESS;
}
""" + actual + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d mode %d: %s\n",__LINE__,mode,#x);return 1;}}while(0)
int main(void){
 EFI_BOOT_SERVICES bs={Release};EFI_SYSTEM_TABLE table={&bs};
 EFI_PCI_IO_PROTOCOL io={Attributes,{NeverRead}};
 UINT32 config[64]={0};config[1]=2;config[4]=0x1000000;
 resource.descriptor.Desc=ACPI_ADDRESS_SPACE_DESCRIPTOR;
 resource.descriptor.Len=sizeof(resource.descriptor)-3U;
 resource.descriptor.AddrSpaceGranularity=32;
 resource.descriptor.AddrRangeMin=config[4];
 resource.descriptor.AddrLen=0x1000000U;
 resource.descriptor.AddrRangeMax=0xffffffU;
 resource.end[0]=ACPI_END_TAG_DESCRIPTOR;
 memset(unconfirmed,0xa5,sizeof(unconfirmed));
 for(mode=0;mode<=7;mode++){
  gets=frees=foreign_frees=0;UncertainBarResourceAddress=0;UncertainBarResourceStatus=0;
  resource.descriptor.ResType=mode==7?1:0;
  CHECK(BarEnvelope(&table,&io,config)==(mode==0));
  CHECK(gets==1&&!foreign_frees&&frees==(mode==0||mode>=5));
  int uncertain=(mode==1||mode==2||mode==5||mode==6);
  CHECK((UncertainBarResourceAddress!=0)==uncertain);
  CHECK((UncertainBarResourceStatus!=0)==uncertain);
  if(uncertain){
   CHECK(!BarEnvelope(&table,&io,config));
   CHECK(gets==1&&!foreign_frees&&frees==(mode>=5));
  }
  for(size_t i=0;i<sizeof(unconfirmed);i++)CHECK(unconfirmed[i]==0xa5);
 }
 return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/"capture_descriptor.c"
            c.write_text(program,encoding="utf-8")
            for compiler in ("gcc","clang"):
                with self.subTest(compiler=compiler):
                    binary=Path(tmp)/compiler
                    build=subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror","-pedantic",str(c),"-o",str(binary)],capture_output=True,text=True)
                    self.assertEqual(build.returncode,0,build.stderr)
                    run=subprocess.run([str(binary)],capture_output=True,text=True)
                    self.assertEqual(run.returncode,0,run.stdout+run.stderr)

    def test_isolated_capture_file_ownership(self):
        source=(P/"gpu_prom_capture_efi.c").read_text(encoding="utf-8")
        actual="static int CaptureFileFault;"+source.split("static int CaptureFileFault;",1)[1]
        program=r"""
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
typedef uint8_t UINT8;
typedef uint32_t UINT32;
typedef uint64_t EFI_STATUS;
typedef size_t UINTN;
typedef wchar_t CHAR16;
typedef void VOID;
typedef struct {UINT32 segment,bus,device,function;} GU_GPU_RECORD;
#define GU_PROM_CAPTURE_BYTES 16U
#define EFI_SUCCESS 0ULL
#define EFIERR(n) ((1ULL<<63)|(n))
#define EFI_ERROR(s) (((s)>>63)!=0)
#define EFI_INVALID_PARAMETER EFIERR(2)
#define EFI_UNSUPPORTED EFIERR(3)
#define EFI_DEVICE_ERROR EFIERR(7)
#define EFI_NOT_FOUND EFIERR(14)
#define EFI_ALREADY_STARTED EFIERR(20)
#define EFI_END_OF_FILE EFIERR(31)
#define EFI_COMPROMISED_DATA EFIERR(33)
#define EFI_FILE_MODE_READ 1ULL
#define EFI_FILE_MODE_WRITE 2ULL
#define EFI_FILE_MODE_CREATE 4ULL
typedef struct file EFI_FILE_PROTOCOL;
struct file {
 EFI_STATUS (*Open)(EFI_FILE_PROTOCOL *,EFI_FILE_PROTOCOL **,CHAR16 *,uint64_t,uint64_t);
 EFI_STATUS (*Close)(EFI_FILE_PROTOCOL *);
 EFI_STATUS (*Write)(EFI_FILE_PROTOCOL *,UINTN *,VOID *);
 EFI_STATUS (*Flush)(EFI_FILE_PROTOCOL *);
 EFI_STATUS (*Delete)(EFI_FILE_PROTOCOL *);
};
static EFI_FILE_PROTOCOL root,file,prior,foreign;
static int mode,opens,creates,writes,flushes,deletes,prior_closes,file_closes,invalid;
static EFI_STATUS Close(EFI_FILE_PROTOCOL *p){
 if(p==&prior){prior_closes++;return mode==5?EFI_DEVICE_ERROR:EFI_SUCCESS;}
 if(p==&file){file_closes++;return mode==11?1:EFI_SUCCESS;}
 invalid++;return EFI_DEVICE_ERROR;
}
static EFI_STATUS Open(EFI_FILE_PROTOCOL *p,EFI_FILE_PROTOCOL **out,CHAR16 *name,uint64_t flags,uint64_t attr){
 (void)name;(void)attr;opens++;
 if(p!=&root){invalid++;return EFI_DEVICE_ERROR;}
 if(flags&EFI_FILE_MODE_CREATE){
  creates++;
  if(mode==6||mode==17){*out=&foreign;return mode==6?EFI_DEVICE_ERROR:1;}
  if(mode==7)return 1;
  if(mode==8){*out=&root;return EFI_SUCCESS;}
  if(mode==13)return EFI_SUCCESS;
  *out=&file;return EFI_SUCCESS;
 }
 if(mode==1||mode==5){*out=&prior;return EFI_SUCCESS;}
 if(mode==2||mode==16){*out=&foreign;return mode==2?EFI_NOT_FOUND:1;}
 if(mode==3)return 1;
 if(mode==4){*out=&root;return EFI_SUCCESS;}
 if(mode==12)return EFI_SUCCESS;
 return EFI_NOT_FOUND;
}
static EFI_STATUS Write(EFI_FILE_PROTOCOL *p,UINTN *n,VOID *data){
 (void)n;(void)data;
 if(p!=&file)invalid++;
 writes++;
 if(mode==9||mode==10)return EFI_DEVICE_ERROR;
 return mode==14?1:EFI_SUCCESS;
}
static EFI_STATUS Flush(EFI_FILE_PROTOCOL *p){
 if(p!=&file)invalid++;
 flushes++;return mode==15?1:EFI_SUCCESS;
}
static EFI_STATUS Delete(EFI_FILE_PROTOCOL *p){
 if(p!=&file)invalid++;
 deletes++;return mode==10?1:EFI_SUCCESS;
}
""" + actual + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d mode %d: %s\n",__LINE__,mode,#x);return 1;}}while(0)
int main(void){
 GU_GPU_RECORD target={0};
 UINT8 data[GU_PROM_CAPTURE_BYTES]={0};
 root=(EFI_FILE_PROTOCOL){Open,Close,NULL,NULL,NULL};
 file=(EFI_FILE_PROTOCOL){NULL,Close,Write,Flush,Delete};
 prior=(EFI_FILE_PROTOCOL){NULL,Close,NULL,NULL,NULL};
 foreign=prior;
 for(mode=0;mode<=17;mode++){
  opens=creates=writes=flushes=deletes=prior_closes=file_closes=invalid=0;
  CaptureFileFault=0;
  EFI_STATUS s=GuPromSaveNew(&root,&target,data,sizeof(data));
  CHECK(mode==0?s==EFI_SUCCESS:EFI_ERROR(s));
  if(mode==1)CHECK(s==EFI_ALREADY_STARTED);
  CHECK(!invalid);
  CHECK(prior_closes==(mode==1||mode==5));
  CHECK(creates==(mode==0||(mode>=6&&mode<=11)||(mode>=13&&mode<=15)||mode==17));
  CHECK(writes==(mode==0||mode==9||mode==10||mode==11||mode==14||mode==15));
  CHECK(flushes==(mode==0||mode==11||mode==15));
  CHECK(deletes==(mode==9||mode==10||mode==14||mode==15));
  CHECK(file_closes==(mode==0||mode==11));
  int uncertain=mode==2||mode==4||mode==5||mode==6||mode==8||mode==10||mode==11||mode==16||mode==17;
  CHECK(CaptureFileFault==uncertain);
  if(uncertain){
    int ops=opens+creates+writes+flushes+deletes+prior_closes+file_closes+invalid;
    CHECK(GuPromSaveNew(&root,&target,data,sizeof(data))==EFI_DEVICE_ERROR);
    CHECK(ops==opens+creates+writes+flushes+deletes+prior_closes+file_closes+invalid);
  }
 }
 return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/"capture_file.c"
            c.write_text(program,encoding="utf-8")
            for compiler in ("gcc","clang"):
                with self.subTest(compiler=compiler):
                    binary=Path(tmp)/compiler
                    build=subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror","-pedantic",str(c),"-o",str(binary)],capture_output=True,text=True)
                    self.assertEqual(build.returncode,0,build.stderr)
                    run=subprocess.run([str(binary)],capture_output=True,text=True)
                    self.assertEqual(run.returncode,0,run.stdout+run.stderr)

    def test_diagnostic_log_file_ownership(self):
        source=(P/"gpu_topology_efi.c").read_text(encoding="utf-8")
        actual=source.split("static int LogFileFault;",1)[1].split("\n#endif",1)[0]
        actual="static int LogFileFault;"+actual
        program=r"""
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
typedef void VOID;
typedef void *EFI_HANDLE;
typedef uint64_t EFI_STATUS;
typedef size_t UINTN;
typedef wchar_t CHAR16;
typedef int EFI_GUID;
#define EFI_SUCCESS 0ULL
#define EFI_ERROR(x) (((x)>>63)!=0)
#define EFI_BAD_BUFFER_SIZE (0x8000000000000004ULL)
#define EFI_DEVICE_ERROR (0x8000000000000007ULL)
#define EFI_NOT_FOUND (0x8000000000000014ULL)
#define EFI_UNSUPPORTED (0x8000000000000003ULL)
#define EFI_COMPROMISED_DATA (0x8000000000000021ULL)
#define EFI_VOLUME_FULL (0x800000000000000bULL)
#define EFI_FILE_MODE_READ 1
#define EFI_FILE_MODE_WRITE 2
#define EFI_FILE_MODE_CREATE 4
typedef struct file EFI_FILE_PROTOCOL;
struct file {
  EFI_STATUS (*Open)(EFI_FILE_PROTOCOL *,EFI_FILE_PROTOCOL **,CHAR16 *,uint64_t,uint64_t);
  EFI_STATUS (*Close)(EFI_FILE_PROTOCOL *);
  EFI_STATUS (*Write)(EFI_FILE_PROTOCOL *,UINTN *,VOID *);
  EFI_STATUS (*Flush)(EFI_FILE_PROTOCOL *);
};
typedef struct fs EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
struct fs{EFI_STATUS (*OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *,EFI_FILE_PROTOCOL **);};
typedef struct {EFI_HANDLE DeviceHandle;} EFI_LOADED_IMAGE_PROTOCOL;
typedef struct {EFI_STATUS (*HandleProtocol)(EFI_HANDLE,EFI_GUID *,VOID **);} EFI_BOOT_SERVICES;
typedef struct {EFI_BOOT_SERVICES *BootServices;} EFI_SYSTEM_TABLE;
static EFI_GUID LoadedGuid,FsGuid;
static unsigned LogOverflow;
static size_t LogUsed=4;
static char LogBytes[8]="log!";
static unsigned mode,opens,creates,writes,flushes,root_closes,file_closes,prior_closes,foreign_closes,protocols,volumes;
static EFI_FILE_PROTOCOL root,file,prior,foreign;
static void Say(const char *format,...) {(void)format;}
static EFI_STATUS Close(EFI_FILE_PROTOCOL *f) {
  if(f==&root){root_closes++;return mode==8?EFI_DEVICE_ERROR:EFI_SUCCESS;}
  if(f==&file){file_closes++;return mode==7?EFI_DEVICE_ERROR:EFI_SUCCESS;}
  if(f==&prior){prior_closes++;return mode==5?EFI_DEVICE_ERROR:EFI_SUCCESS;}
  foreign_closes++;return EFI_DEVICE_ERROR;
}
static EFI_STATUS Open(EFI_FILE_PROTOCOL *f,EFI_FILE_PROTOCOL **out,CHAR16 *name,uint64_t flags,uint64_t attrs) {
  (void)f;(void)name;(void)attrs;
  opens++;
  if(flags&EFI_FILE_MODE_CREATE){
    creates++;
    if(mode==3){*out=&foreign;return EFI_DEVICE_ERROR;}
    if(mode==9){*out=&root;return EFI_SUCCESS;}
    if(mode==12)return 1;
    *out=&file;return EFI_SUCCESS;
  }
  if(mode==2){*out=&foreign;return EFI_NOT_FOUND;}
  if(mode==11)return 1;
  if(mode==6){*out=&root;return EFI_SUCCESS;}
  if((mode==4||mode==5)&&opens==1){*out=&prior;return EFI_SUCCESS;}
  return EFI_NOT_FOUND;
}
static EFI_STATUS Write(EFI_FILE_PROTOCOL *f,UINTN *n,VOID *p) {
  (void)f;(void)n;(void)p;writes++;return EFI_SUCCESS;
}
static EFI_STATUS Flush(EFI_FILE_PROTOCOL *f) {(void)f;flushes++;return EFI_SUCCESS;}
static EFI_STATUS Volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs,EFI_FILE_PROTOCOL **out) {
  (void)fs;volumes++;
  if(mode==1){*out=&foreign;return EFI_DEVICE_ERROR;}
  if(mode==10)return 1;
  *out=&root;return EFI_SUCCESS;
}
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL fs={Volume};
static EFI_LOADED_IMAGE_PROTOCOL li={(EFI_HANDLE)1};
static EFI_STATUS Protocol(EFI_HANDLE h,EFI_GUID *guid,VOID **out) {
  (void)h;protocols++;
  if(guid==&LoadedGuid)*out=&li;else *out=&fs;
  return EFI_SUCCESS;
}
""" + actual + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d mode %u: %s\n",__LINE__,mode,#x);return 1;}}while(0)
int main(void){
  EFI_BOOT_SERVICES bs={Protocol};EFI_SYSTEM_TABLE table={&bs};
  root=(EFI_FILE_PROTOCOL){Open,Close,NULL,NULL};
  file=(EFI_FILE_PROTOCOL){NULL,Close,Write,Flush};
  prior=(EFI_FILE_PROTOCOL){NULL,Close,NULL,NULL};
  foreign=prior;
  for(mode=0;mode<=12;mode++){
    opens=creates=writes=flushes=root_closes=file_closes=prior_closes=foreign_closes=protocols=volumes=0;
    LogFileFault=0;
    EFI_STATUS s=SaveLog((EFI_HANDLE)1,&table);
    CHECK((s==EFI_SUCCESS)==(mode==0||mode==4));
    CHECK((s==EFI_SUCCESS)||EFI_ERROR(s));
    CHECK(!foreign_closes && root_closes==(mode!=1&&mode!=10));
    CHECK(file_closes==(mode==0||mode==4||mode==7||mode==8));
    CHECK(prior_closes==(mode==4||mode==5));
    CHECK(writes==file_closes&&flushes==writes);
    CHECK(creates==(mode==0||mode==3||mode==4||mode==7||mode==8||mode==9||mode==12));
    CHECK(LogFileFault==(mode>=1&&mode<=9&&mode!=4));
    if(LogFileFault){
      unsigned prior_ops=opens+creates+writes+flushes+root_closes+file_closes+prior_closes+foreign_closes;
      CHECK(SaveLog((EFI_HANDLE)1,&table)==EFI_DEVICE_ERROR);
      CHECK(protocols==2&&volumes==1);
      CHECK(prior_ops==opens+creates+writes+flushes+root_closes+file_closes+prior_closes+foreign_closes);
    }
  }
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/"log_ownership.c"
            c.write_text(program,encoding="utf-8")
            for compiler in ("gcc","clang"):
                with self.subTest(compiler=compiler):
                    binary=Path(tmp)/compiler
                    build=subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror","-pedantic",str(c),"-o",str(binary)],capture_output=True,text=True)
                    self.assertEqual(build.returncode,0,build.stderr)
                    run=subprocess.run([str(binary)],capture_output=True,text=True)
                    self.assertEqual(run.returncode,0,run.stdout+run.stderr)


    def test_final_topology_storage_cleanup(self):
        source=(P/"gpu_topology_efi.c").read_text(encoding="utf-8")
        actual="static int ReleaseTopologyStorage("+source.split("static int ReleaseTopologyStorage(",1)[1].split("\nEFI_STATUS EFIAPI UefiMain(",1)[0]
        main=source.split("EFI_STATUS EFIAPI UefiMain(",1)[1]
        self.assertLess(main.index("TOPOLOGY_REPORT_STORAGE_ALIAS"),main.index("GuTopoAssess("))
        self.assertLess(main.index("TOPOLOGY_VERDICT="),main.index("ReleaseTopologyStorage(bs,&report_storage)"))
        self.assertIn("if(!ReleaseTopologyStorage(bs,&report_storage))result=GU_TOPO_INVALID;",main)
        program=r"""
#include "gpu_topology.h"
#include <stdio.h>
#include <stddef.h>
#include <string.h>
typedef void VOID;
typedef int EFI_STATUS;
#define EFI_SUCCESS 0
typedef struct { EFI_STATUS (*FreePool)(VOID *); } EFI_BOOT_SERVICES;
static GU_TOPO_CAPTURE Snapshot;
static GU_TOPO_REPORT Report;
static GU_TOPO_GPU_REPORT report;
static GU_TOPO_GOP gop;
static GU_TOPO_GPU_REPORT *storage;
static int report_frees,gop_frees,unexpected,mode,logs;
static void Say(const char *fmt,...){(void)fmt;logs++;}
static EFI_STATUS Release(VOID *p) {
  if(storage||Report.gpus||Report.gpu_count||Snapshot.gop||
     Snapshot.gop_count||Snapshot.gop_capacity)unexpected++;
  if(p==&report){report_frees++;return mode==1?1:mode==4?2:0;}
  if(p==&gop){gop_frees++;return mode==2?1:0;}
  unexpected++;return 1;
}
""" + actual + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d mode %d: %s\n",__LINE__,mode,#x);return 1;}}while(0)
int main(void) {
  EFI_BOOT_SERVICES bs={Release};
  for(mode=0;mode<=5;mode++){
    report_frees=gop_frees=unexpected=logs=0;
    storage=mode==3?(GU_TOPO_GPU_REPORT *)(void *)&gop:mode==5?NULL:&report;
    Report.gpus=storage;Report.gpu_count=storage?1:0;
    Snapshot.gop=&gop;Snapshot.gop_count=Snapshot.gop_capacity=1;
    CHECK(ReleaseTopologyStorage(&bs,&storage)==(mode==0||mode==5));
    CHECK(!storage&&!Report.gpus&&!Report.gpu_count&&!Snapshot.gop&&!Snapshot.gop_count&&!Snapshot.gop_capacity);
    CHECK(!unexpected&&report_frees==(mode!=3&&mode!=5)&&gop_frees==1);
    CHECK(logs==(mode==0||mode==5?1:2));
    CHECK(ReleaseTopologyStorage(&bs,&storage)&&!unexpected&&report_frees==(mode!=3&&mode!=5)&&gop_frees==1);
  }
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/"final_topology.c"
            c.write_text(program,encoding="utf-8")
            for compiler in ("gcc","clang"):
                with self.subTest(compiler=compiler):
                    binary=Path(tmp)/compiler
                    build=subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror","-pedantic","-I",str(P),str(c),"-o",str(binary)],capture_output=True,text=True)
                    self.assertEqual(build.returncode,0,build.stderr)
                    run=subprocess.run([str(binary)],capture_output=True,text=True)
                    self.assertEqual(run.returncode,0,run.stdout+run.stderr)

    def test_version_does_not_claim_parent_release(self):
        self.assertIn("GPU-Unlock fork: experimental",V)
        self.assertIn("GU_FORK_VERSION_WIDE",B)
        self.assertIn("GU_FORK_VERSION",A)
        self.assertNotIn("proven rock-solid",B)
        self.assertNotIn("UNIVERSAL UNLOCK v1.7.5",A)
    def test_stock_windows_countdown_keeps_default(self):
        self.assertIn("*choice=L'S';",B)
        self.assertIn("i<300",B)
        self.assertIn("Stall(100000)",B)
        self.assertIn("if(i%10==0)",B)
        self.assertIn("seconds=(300-i+9)/10",B)
if __name__=="__main__":unittest.main()
