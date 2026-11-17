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
static int fail_alloc,fail_free,allocs,frees,steps;
static EFI_STATUS Allocate(int type,size_t bytes,VOID **out) {
  if(type!=EfiLoaderData || fail_alloc)return 1;
  *out=calloc(1,bytes);
  if(!*out)return 1;
  allocs++;
  return EFI_SUCCESS;
}
static EFI_STATUS Free(VOID *p) {free(p);frees++;return fail_free?1:EFI_SUCCESS;}
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
#define EFI_ERROR(x) ((x)!=EFI_SUCCESS)
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
static UINTN total=40;
static EFI_STATUS Allocate(int kind,UINTN size,VOID **out) {
  (void)kind;
  if(fail_alloc)return 1;
  *out=calloc(1,size);
  if(!*out)return 1;
  allocs++;return EFI_SUCCESS;
}
static EFI_STATUS Free(VOID *p) {
  free(p);frees++;
  return fail_free?1:EFI_SUCCESS;
}
static EFI_STATUS Locate(int kind,EFI_GUID *guid,VOID *key,UINTN *n,EFI_HANDLE **out) {
  (void)kind;(void)guid;(void)key;
  *n=total;
  if(!total)return EFI_SUCCESS;
  if(total>40)return 1;
  *out=calloc(total,sizeof(**out));
  if(!*out)return 1;
  allocs++;
  for(size_t i=0;i<total;i++)(*out)[i]=&paths[i];
  return EFI_SUCCESS;
}
static EFI_STATUS Handle(EFI_HANDLE handle,EFI_GUID *guid,VOID **out) {
  size_t i=(GU_TOPO_PATH *)handle-paths;
  if(fail_read && i==39)return 1;
  if(guid==&GopGuid)*out=&protocols[(alias&&i==39)?0:(drift&&i==39)?38:i];
  else *out=&paths[i];
  return EFI_SUCCESS;
}
static int PathCopy(EFI_DEVICE_PATH_PROTOCOL *path,GU_TOPO_PATH *out) {
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
  paths[39].bytes[5]=1;
  CHECK(!RevalidateGop(&bs) && allocs-frees==1);
  paths[39].bytes[5]=39;
  drift=1;
  CHECK(!RevalidateGop(&bs) && allocs-frees==1);
  drift=0;free(Snapshot.gop);frees++;
  memset(&Snapshot,0,sizeof(Snapshot));
  fail_alloc=1;
  CHECK(!CaptureGop(&bs) && !Snapshot.gop && allocs==frees);
  fail_alloc=0;fail_read=1;
  CHECK(!CaptureGop(&bs) && !Snapshot.gop && allocs==frees);
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
