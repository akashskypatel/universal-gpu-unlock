"""Verify that one exact asset table drives both firmware loading and verification."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
R=Path(__file__).resolve().parents[1]/"src"
P=(R/"profile.c").read_text(encoding="utf-8")
B=(R/"boot_io.c").read_text(encoding="utf-8")
A=(R/"app.c").read_text(encoding="utf-8")
D=(R/"dxe_main.c").read_text(encoding="utf-8")
class AssetContract(unittest.TestCase):
    def test_five_exact_assets_and_hashes_for_each_gpu(self):
        for n in ("AdaAssets","AmpereAssets","TuringAssets"):
            part=P.split("static const GU_ARCH_ASSETS "+n+"=",1)[1].split("};",1)[0]
            self.assertEqual(len(re.findall(r'\{L"[^"]+","[^"]+","[0-9a-f]{64}",\d+U\}',part)),5)
    def test_passive_preparation_does_not_require_owner_asset_manifest(self):
        discovery=(R/"gpu_discovery.c").read_text(encoding="utf-8")
        self.assertNotIn("PreparedTargetHasReferenceAssets",discovery)
        self.assertNotIn("NO_EXACT_REFERENCE_MANIFEST",discovery)
        self.assertIn("GuGpuExecCheckNext(",discovery)
        self.assertIn("REASON=SELECTION_PREPARATION_ONLY",discovery)
    def test_load_and_validate_same_manifest(self):
        load=B.split("EFI_STATUS GuBootLoadAssets(VOID) {",1)[1].split("static EFI_STATUS ReadWindowsBinary(",1)[0]
        verify=P.split("BOOLEAN GuAd104AssetsValid(const GU_PLATFORM *platform) {",1)[1].split("BOOLEAN GuAd104PromMatches(",1)[0]
        self.assertIn("GuGetArchAssets()",load)
        self.assertIn("arch->entries[i]",load)
        self.assertIn("GuAd104AssetsValid(&Gu)",load)
        self.assertIn("PlatformManifest(platform)",verify)
        self.assertIn("GuHashMatches(",verify)
        ownership=P.split("BOOLEAN GuPlatformAssetsOwned(",1)[1].split("BOOLEAN GuAd104AssetsValid(",1)[0]
        self.assertIn("GuCpuPoolOwns(platform,",ownership)
        self.assertIn("entry->name!=a->name",ownership)
        self.assertLess(verify.index("GuPlatformAssetsOwned(platform)"),verify.index("GuHashMatches("))
        self.assertNotIn("GuHashMatches(",ownership)
        self.assertNotIn("GuPlatformActive",ownership)
        self.assertNotIn("ada_sizes",load)
    def test_diagnostics_are_read_only(self):
        x=P.split("EFI_STATUS GuDiagnosticScan(GU_PLATFORM *owner) {",1)[1].split("EFI_STATUS GuAd104Prepare(VOID) {",1)[0]
        for write in ("GuWrite32(","GuEnableMaster(","GuFlr(","GuMsiRelease(",
                      "GuLatchEntrySelectors(","Gu.profile_verified=TRUE"):
            self.assertNotIn(write,x)
        self.assertIn("GuAd104FwsecMatches(&fw)",x)
        self.assertIn("SIGNATURE_AUTHENTICATED=0",x)
        self.assertNotIn("Gu.",x)
        self.assertNotIn("owner->profile_verified=",x)
        self.assertNotIn("owner->prom_verified=",x)
    def test_read_only_diagnostic_retains_original_owner(self):
        start=P.index("static BOOLEAN DiagnosticOwnerMatches(")
        end=P.index("EFI_STATUS GuAd104Prepare(",start)
        actual=P[start:end]
        host=r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
typedef void VOID;
typedef int BOOLEAN;
typedef int EFI_STATUS;
typedef uintptr_t UINTN;
typedef uint32_t UINT32;
typedef void *EFI_HANDLE;
typedef struct {int id;} EFI_BOOT_SERVICES;
typedef struct {int id;} EFI_PCI_IO_PROTOCOL;
typedef struct {
  EFI_BOOT_SERVICES *bs;
  EFI_PCI_IO_PROTOCOL *pci;
  EFI_HANDLE handle;
  UINTN bar_base;
  int started,owned,retained,profile_verified,fault;
} GU_PLATFORM;
struct gpu_unlock_fwsec {int descriptor_version;};
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_NOT_READY 1
#define EFI_DEVICE_ERROR 2
#define EFI_OUT_OF_RESOURCES 3
#define EFI_UNSUPPORTED 4
#define EFI_COMPROMISED_DATA 5
static GU_PLATFORM p[2],*GuPlatformActive=&p[0];
static EFI_BOOT_SERVICES services[2];
static EFI_PCI_IO_PROTOCOL interfaces[2];
static int mode,configs,registers,logs,allocs,frees,copies,wrong,parses;
static void *buffer;
static GU_PLATFORM *buffer_owner;
static size_t buffer_bytes;
static BOOLEAN GuCpuPoolOwns(const GU_PLATFORM *o,const VOID *m,size_t n){
  return o==buffer_owner&&m==buffer&&n==buffer_bytes;
}
static void drift(void){GuPlatformActive=&p[1];}
static BOOLEAN ConfigFor(GU_PLATFORM *o){
  configs++;if(o!=&p[0]||o!=GuPlatformActive)wrong++;
  if(mode==1&&configs==1)drift();
  if(mode==6&&configs==2)drift();
  return TRUE;
}
static BOOLEAN RegistersFor(GU_PLATFORM *o,UINT32 *a,UINT32 *b){
  registers++;if(o!=&p[0]||o!=GuPlatformActive)wrong++;
  *a=1;*b=2;
  if(mode==2&&registers==1)drift();
  if(mode==7&&registers==2)drift();
  return TRUE;
}
static BOOLEAN GuAd104AssetsValid(const GU_PLATFORM *o){
  if(o!=&p[0]||o!=GuPlatformActive)wrong++;
  return TRUE;
}
static void GuLog(const char *fmt,...){
  (void)fmt;logs++;
  if(mode==3&&logs==1)drift();
  if(mode==8&&logs==2)drift();
  if(mode==20&&logs==2)buffer_owner=&p[1];
}
static VOID *GuAlloc(size_t n){
  allocs++;if(GuPlatformActive!=&p[0])wrong++;
  if(mode==10)return NULL;
  if(mode==22){p[0].fault=p[0].retained=1;return NULL;}
  buffer=malloc(n);
  buffer_owner=mode==18?&p[1]:&p[0];
  buffer_bytes=mode==21?n-1:n;
  if(mode==4)drift();
  return buffer;
}
static VOID GuFree(VOID *m){
  frees++;if(GuPlatformActive!=&p[0]||m!=buffer)wrong++;
  free(m);buffer=NULL;
  if(mode==9)drift();
}
static BOOLEAN GuCopyFromIo(GU_PLATFORM *o,VOID *dst,const VOID *src,size_t n){
  (void)src;(void)n;copies++;
  if(o!=&p[0]||o!=GuPlatformActive||dst!=buffer)wrong++;
  if(mode==5)drift();
  if(mode==11)o->bs=&services[1];
  if(mode==12)o->pci=&interfaces[1];
  if(mode==19)buffer_owner=&p[1];
  return TRUE;
}
static BOOLEAN GuAd104PromMatches(const GU_PLATFORM *o,const VOID *m,size_t n){
  (void)n;if(o!=&p[0]||o!=GuPlatformActive||m!=buffer)wrong++;
  return TRUE;
}
static int gpu_unlock_vbios_fwsec(const VOID *m,size_t n,struct gpu_unlock_fwsec *f){
  (void)n;parses++;if(m!=buffer)wrong++;f->descriptor_version=3;return 0;
}
static BOOLEAN GuAd104FwsecMatches(const struct gpu_unlock_fwsec *f){return f->descriptor_version==3;}
""" + actual + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"failed mode %d line %d: %s\n",mode,__LINE__,#x);return 1;}}while(0)
int main(int argc,char **argv){
  mode=argc>1?atoi(argv[1]):-1;
  p[0].bs=p[1].bs=&services[0];
  p[0].pci=&interfaces[0];p[1].pci=&interfaces[1];
  p[0].handle=&p[0];p[1].handle=&p[1];p[0].bar_base=0x1000;
  GU_PLATFORM *requested=&p[0];
  if(mode==13)requested=NULL;
  if(mode==14)requested=&p[1];
  if(mode==15)p[0].bs=NULL;
  if(mode==16)p[0].pci=NULL;
  if(mode==17)p[0].handle=NULL;
  EFI_STATUS status=GuDiagnosticScan(requested);
  CHECK(!wrong&&!p[1].fault&&!p[1].retained&&!p[1].profile_verified&&!p[0].profile_verified);
  if(mode==0){
    CHECK(status==EFI_SUCCESS&&allocs==1&&frees==1&&copies==1&&configs==2&&registers==2);
    CHECK(!p[0].fault&&!p[0].retained&&parses==1);
  }else if(mode==10){
    CHECK(status==EFI_OUT_OF_RESOURCES&&allocs==1&&!frees&&!copies&&!p[0].fault);
  }else if(mode>=13&&mode<=17){
    CHECK(status==EFI_NOT_READY&&!configs&&!registers&&!logs&&!allocs&&!copies&&!frees);
    CHECK(!p[0].fault&&!p[0].retained&&GuPlatformActive==&p[0]&&!buffer);
  }else{
    CHECK(status==EFI_DEVICE_ERROR&&p[0].fault&&p[0].retained);
    if(mode<=3)CHECK(!allocs&&!copies&&!frees);
    if(mode==1)CHECK(registers==0);
    if(mode==4)CHECK(allocs==1&&!copies&&!frees);
    if(mode==5||mode==11||mode==12)CHECK(copies==1&&configs==1&&!frees);
    if(mode==6)CHECK(configs==2&&registers==1&&!frees);
    if(mode==7||mode==8)CHECK(configs==2&&registers==2&&!frees);
    if(mode==9)CHECK(frees==1&&!buffer);
    if(mode==18||mode==21)CHECK(allocs==1&&!copies&&!parses&&!frees);
    if(mode==19)CHECK(allocs==1&&copies==1&&!parses&&!frees&&configs==1);
    if(mode==20)CHECK(allocs==1&&copies==1&&parses==1&&!frees&&configs==2);
    if(mode==22)CHECK(allocs==1&&!buffer&&!copies&&!parses&&!frees);
    free(buffer); /* Host fixture teardown, not production recovery. */
  }
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory)/"diagnostic_owner.c"
            binary=Path(directory)/"diagnostic_owner"
            source.write_text(host,encoding="utf-8")
            result=subprocess.run(["gcc","-std=c11","-O2","-Wall","-Wextra","-Werror","-pedantic",str(source),"-o",str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            for mode in range(23):
                with self.subTest(mode=mode):
                    result=subprocess.run([str(binary),str(mode)],capture_output=True,text=True)
                    self.assertEqual(result.returncode,0,result.stderr)
    def test_profile_uses_exact_discovery_identity_and_refuses_failed_release(self):
        config=P.split("static BOOLEAN ConfigFor(GU_PLATFORM *platform) {",1)[1].split("static BOOLEAN TuringColdState",1)[0]
        self.assertIn("id->handle!=platform->handle",config)
        self.assertIn("id->pci!=platform->pci",config)
        self.assertLess(config.index("b!=id->bus"),config.index("GuConfigReadFor(platform,"))
        for match in ("c[0]!=id->pci_id","c[11]!=id->subsystem_id","(c[2]>>16)!=id->class_code"):
            self.assertIn(match,config)
        prepare=P.split("EFI_STATUS GuAd104Prepare(VOID) {",1)[1]
        self.assertLess(prepare.index("GuFree(prom);"),prepare.index("if(Gu.fault||Gu.retained)return EFI_DEVICE_ERROR;"))
        self.assertLess(prepare.index("if(Gu.fault||Gu.retained)return EFI_DEVICE_ERROR;"),prepare.index("Gu.prom_verified=TRUE;"))
    def test_diagnostics_run_before_preparation(self):
        shared=(R/"unlock_execution.c").read_text(encoding="utf-8")
        prep=shared.split("EFI_STATUS GuPrepareBoundGpu(",1)[1].split("EFI_STATUS GuExecutePreparedSequence(",1)[0]
        self.assertLess(prep.index("GuBootLoadAssets()"),prep.index("GuDiagnosticScan(owner)"))
        self.assertLess(prep.index("GuDiagnosticScan(owner)"),prep.index("GuAd104Prepare()"))
        for src in (A,D):
            self.assertLess(src.index("GuPrepareBoundGpu("),src.index("GuExecutePreparedSequence("))
if __name__=="__main__":unittest.main()
