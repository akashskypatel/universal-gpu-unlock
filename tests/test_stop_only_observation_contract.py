"""Checks for a deliberately isolated, single-card Stop experiment.

These static guards are not proof of firmware recovery or successful Stop.
MSVC compile and a real physical log are independent verification steps.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT/"src/msi_stop_observation.c").read_text(encoding="utf-8")
BUILD = (ROOT/"src/Build-Msi-Stop-Observation.ps1").read_text(encoding="utf-8")
PASSIVE = (ROOT/"src/Build-Gpu-Topology-Diagnostic.ps1").read_text(encoding="utf-8")


class StopOnlyObservation(unittest.TestCase):
    def test_ga102_identity_and_amd_only_preflight(self):
        ga=SOURCE.split("#ifdef GU_STOP_TARGET_3080",1)[1].split("#else",1)[0]
        ad=SOURCE.split("#else",1)[1].split("#endif",1)[0]
        for value in ('0000:04:00.0', 'Buses[3]={4,1,15}', 'ExactRomSize=148992U',
                      'ExactImageSize=158432U', 'ExactStopRva=0x678U',
                      'a9da4af0bf448feb458f55da926329e135b2739e043dd0bfda69915747b728e9'):
            self.assertIn(value,ga)
        for value in ('0000:01:00.0','Buses[3]={1,4,15}','ExactStopRva=0x6ecU'):
            self.assertIn(value,ad)
        gop=SOURCE.split("static EFI_STATUS Gops(VOID) {",1)[1].split("static EFI_STATUS ConsoleVar(",1)[0]
        ga_guard=gop.split("#ifdef GU_STOP_TARGET_3080",1)[1].split("#else",1)[0]
        self.assertIn("count!=1 || each[0]!=0 || each[1]!=0 || each[2]!=1",ga_guard)
        self.assertIn("$flags+=' /DGU_STOP_TARGET_3080'",BUILD)
        self.assertIn('MSI-$targetName-STOP-WINDOWS-HANDOFF-EXPERIMENT.EFI',BUILD)

    def test_one_vendor_stop_to_exact_original_driver_after_durable_log(self):
        # Actual direct Boot Services call count, not substring occurrences in comments.
        calls = re.findall(r"->DisconnectController\s*\(", SOURCE)
        self.assertEqual(len(calls), 1)
        self.assertIn("DisconnectController(Cards[0].handle,", SOURCE)
        self.assertIn("expected_agent,NULL)", SOURCE)
        self.assertIn("GuHashMatches(image->ImageBase", SOURCE)
        self.assertIn("binding->Stop!=(EFI_DRIVER_BINDING_STOP)", SOURCE)
        self.assertIn("image->ImageSize!=ExactImageSize", SOURCE)
        self.assertIn("OriginalImageSha", SOURCE)
        self.assertIn("Cards[0].io->RomSize!=ExactRomSize", SOURCE)
        self.assertIn("TargetHasNoChildren()",SOURCE)
        for pinned in ("8e2b676ed8eccbb582ca6a80d6bf39196dcc574a1c3c4cc73d35b53c2d4de312",
                       "b96b0411dcd378e6433c609f94f6828ea18d8713f5055e4d76ffe9c4901b2908",
                       "fcb89e38b347b79e6fd126ade23dab174a5c5bfb9bfe7686b1072835986038e0"):
            self.assertIn(pinned,SOURCE)
        self.assertIn("GuHashMatches(Cards[i].path,Cards[i].path_size,ExactPaths[i])",SOURCE)
        self.assertIn("if(s==EFI_SUCCESS)s=Owner(&Cards[1]);",SOURCE)
        self.assertIn("if(s==EFI_SUCCESS)s=Owner(&Cards[2]);",SOURCE)
        self.assertIn("POST_STOP_PEER_OWNER=",SOURCE)
        # A refused Inventory() result must identify its exact read-only gate.
        for record in ("STOP_PREFLIGHT_PCI_ENUM STATUS=",
                       "STOP_PREFLIGHT_PCI_ERROR INDEX=",
                       "STOP_PREFLIGHT_PCI_IDENTITY_COUNTS DISPLAY=",
                       "STOP_PREFLIGHT_PCI_OPENS SLOT=",
                       "STOP_PREFLIGHT_PCI_OPEN SLOT=",
                       "STOP_PREFLIGHT_PCI_OWNER SLOT=",
                       "STOP_PREFLIGHT_OWNER_RESULT SLOT=",
                       "STOP_PREFLIGHT_TARGET_PROTOCOLS STATUS=",
                       "STOP_PREFLIGHT_TARGET_PROTOCOL INDEX=",
                       "STOP_PREFLIGHT_TARGET_OPEN_BLOCKER PROTOCOL=",
                       "STOP_PREFLIGHT_CHILD_RESULT STATUS="):
            self.assertIn(record, SOURCE)
        inventory=SOURCE.split("static EFI_STATUS Inventory(VOID) {",1)[1].split(
            "static EFI_STATUS Gops(VOID) {",1)[0]
        self.assertLess(inventory.index("s=Owner(&Cards[i]);"),
                        inventory.index("STOP_PREFLIGHT_OWNER_RESULT SLOT="))
        owner_checks=inventory.split("for(i=0;i<3;i++) {",1)[1]
        self.assertLess(owner_checks.index("STOP_PREFLIGHT_OWNER_RESULT SLOT="),
                        owner_checks.index("if(EFI_ERROR(s))return s;"))
        self.assertLess(inventory.index("s=TargetHasNoChildren();"),
                        inventory.index("STOP_PREFLIGHT_CHILD_RESULT STATUS="))
        self.assertIn("if(EFI_ERROR(s))return s;", inventory)
        for phase in ("STAGE=HANDLE_PCI","STAGE=GET_LOCATION","STAGE=READ_CONFIG",
                      "STAGE=DEVICE_PATH"):
            self.assertIn(phase, inventory)
        self.assertIn("INVENTORY_ACCEPTED=%u", SOURCE)
        self.assertIn("(unsigned)(s==EFI_SUCCESS)", SOURCE)
        self.assertNotIn("IDENTITY_STATUS=%#llx EXACT_3_DISPLAY=1", SOURCE)
        for field in ("GOP_POLICY_ACCEPTED=%u", "CONSOLE_POLICY_ACCEPTED=%u",
                      "DRIVER_VERIFIED=%u"):
            self.assertIn(field, SOURCE)



        self.assertIn("STOP_ATTEMPT=IMMINENT", SOURCE)
        before=SOURCE.index("s=Flush();\n  if(EFI_ERROR(s)||!Log)goto refuse;", SOURCE.index("STOP_ATTEMPT=IMMINENT"))
        attempt=SOURCE.index("s=table->BootServices->DisconnectController(")
        self.assertLess(before, attempt)
        self.assertLess(SOURCE.index("s=Log->Close(Log);",before),attempt)
        self.assertLess(SOURCE.index("s=Root->Close(Root);",before),attempt)
        self.assertGreater(SOURCE.index("EFI_STATUS postlog=StartLog(image);"),attempt)

        self.assertIn("if(EFI_ERROR(s)||!Log)goto refuse;",SOURCE)
        self.assertIn("s=KeyY();",SOURCE)
        self.assertIn("for(;;)(VOID)Sys->BootServices->Stall(1000000);",SOURCE)
        self.assertIn("NO_WINDOWS_HANDOFF=1 POWER_OFF_REQUIRED=1",SOURCE)

    def test_gpu_peer_and_console_guards_precede_stop(self):
        before = SOURCE[:SOURCE.index("STOP_ATTEMPT=IMMINENT")]
        for required in ('display!=3', 'found[0]!=1', 'found[1]!=1', 'found[2]!=1',
                         'each[0]!=0 || each[2]!=1',
                         '(count==1 && each[1]==0)',
                         '(count==2 && each[1]==1)',
                         'ConsoleVar(L"ConOut")','ConsoleVar(L"ErrOut")',
                         'Cards[0].owner!=expected_agent',
                         'ExactOriginalDriver()',
                         'EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER'):
            self.assertIn(required, before)
        for disallowed in ('SetVariable(', 'Pci.Write(', 'Mem.Write(',
                           'ResetSystem(', 'StartImage(', 'LoadImage('):
            self.assertNotIn(disallowed,SOURCE)

    def test_owner_child_open_behavior_from_production_c(self):
        # Compile the actual C Owner function. The fixtures reproduce the two
        # physical AMD PCI open records and exercise unsafe negative cases.
        import shutil
        import subprocess
        import tempfile
        compiler = shutil.which("gcc") or shutil.which("clang")
        if not compiler:
            self.skipTest("A host C compiler is required")
        start = SOURCE.index("static EFI_STATUS Owner(GPU *card) {")
        end = SOURCE.index("/* A bound child", start)
        owner_c = SOURCE[start:end]
        prefix = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef void *EFI_HANDLE;
typedef uint64_t EFI_STATUS;
typedef uintptr_t UINTN;
typedef uint32_t UINT32;
typedef struct {
  EFI_HANDLE AgentHandle, ControllerHandle;
  UINT32 Attributes, OpenCount;
} EFI_OPEN_PROTOCOL_INFORMATION_ENTRY;
typedef struct {
  EFI_HANDLE handle, owner;
} GPU;
typedef struct {
  EFI_STATUS (*OpenProtocolInformation)(
    EFI_HANDLE, void *, EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **, UINTN *);
  EFI_STATUS (*FreePool)(void *);
} EFI_BOOT_SERVICES;
typedef struct { EFI_BOOT_SERVICES *BootServices; } SYSTEM;
#define EFI_SUCCESS ((EFI_STATUS)0)
#define EFI_ACCESS_DENIED UINT64_C(0x800000000000000f)
#define EFI_DEVICE_ERROR UINT64_C(0x8000000000000007)
#define EFI_COMPROMISED_DATA UINT64_C(0x8000000000000021)
#define EFI_ERROR(s) (((s) & UINT64_C(0x8000000000000000)) != 0)
#define EFI_OPEN_PROTOCOL_GET_PROTOCOL 0x01U
#define EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER 0x08U
#define EFI_OPEN_PROTOCOL_BY_DRIVER 0x10U
#define EFI_OPEN_PROTOCOL_EXCLUSIVE 0x20U
static GPU Cards[3];
static int PciGuid;
static EFI_OPEN_PROTOCOL_INFORMATION_ENTRY records[5];
static UINTN count;
static EFI_STATUS Open(EFI_HANDLE h, void *guid,
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **out, UINTN *n) {
  (void)h; (void)guid; *out=records; *n=count; return EFI_SUCCESS;
}
static EFI_STATUS Free(void *p) { (void)p; return EFI_SUCCESS; }
static EFI_BOOT_SERVICES Boot={Open,Free};
static SYSTEM Table={&Boot};
static SYSTEM *Sys=&Table;
static void Say(const char *fmt, ...) { (void)fmt; }
"""
        suffix = r"""
static int check(unsigned slot, unsigned mode, EFI_STATUS expected) {
  EFI_STATUS actual;
  EFI_HANDLE h=Cards[slot].handle;
  count=0;
  Cards[slot].owner=NULL;
  records[count++]=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){
    (EFI_HANDLE)(uintptr_t)0x7000, NULL, 0x01, 38};
  if(mode!=6)
    records[count++]=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){
      (EFI_HANDLE)(uintptr_t)0x7010, h, 0x10, 1};
  if(mode==1 || mode==2 || mode==3 || mode==4) {
    records[count++]=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){
      mode==2?NULL:(EFI_HANDLE)(uintptr_t)0x7020,
      mode==3?h:(EFI_HANDLE)(uintptr_t)0x7030, 0x08,
      mode==4?2U:1U};
  }
  if(mode==5 || mode==8)
    records[count++]=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){
      (EFI_HANDLE)(uintptr_t)0x7040,h,
      mode==8?0x30U:0x20U,1};
  if(mode==7)
    records[count++]=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){
      (EFI_HANDLE)(uintptr_t)0x7050,h,0x10,1};
  actual=Owner(&Cards[slot]);
  if(actual!=expected) {
    fprintf(stderr,"slot=%u mode=%u expected=%llx actual=%llx\n",
      slot,mode,(unsigned long long)expected,(unsigned long long)actual);
    return 1;
  }
  if(actual==EFI_SUCCESS && Cards[slot].owner!=(EFI_HANDLE)(uintptr_t)0x7010)
    return 2;
  return 0;
}
int main(void) {
  unsigned i;
  const struct { unsigned slot,mode; EFI_STATUS expected; } cases[]={
    {2,1,EFI_SUCCESS}, {1,1,EFI_SUCCESS},
    {0,1,EFI_ACCESS_DENIED}, {0,0,EFI_SUCCESS},
    {2,2,EFI_ACCESS_DENIED}, {2,3,EFI_ACCESS_DENIED},
    {2,4,EFI_ACCESS_DENIED}, {2,5,EFI_ACCESS_DENIED},
    {2,6,EFI_ACCESS_DENIED}, {2,7,EFI_ACCESS_DENIED},
    {2,8,EFI_ACCESS_DENIED}
  };
  for(i=0;i<3;i++)Cards[i].handle=(EFI_HANDLE)(uintptr_t)(0x1000+i);
  for(i=0;i<sizeof(cases)/sizeof(cases[0]);i++)
    if(check(cases[i].slot,cases[i].mode,cases[i].expected))
      return 1;
  puts("PASS actual Owner() C: 11 positive/negative ownership scenarios");
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "owner.c"
            executable = Path(directory) / "owner"
            source.write_text(prefix + owner_c + suffix, encoding="utf-8")
            result = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 str(source), "-o", str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("11 positive/negative ownership scenarios", result.stdout)

    def test_gop_classifier_executes_original_c_and_keeps_strict_admission(self):
        # Use the actual C code, not a rewritten Python model. These tests
        # do not show what the original firmware exposes on the real machine.
        import shutil
        import subprocess
        import tempfile
        compiler = shutil.which("gcc") or shutil.which("clang")
        if not compiler:
            self.skipTest("A host C compiler is required")
        path_start = SOURCE.index("static UINTN PathBytes(")
        path_end = SOURCE.index("static EFI_STATUS Owner(", path_start)
        gop_start = SOURCE.index("static EFI_STATUS Gops(VOID) {")
        gop_end = SOURCE.index("static EFI_STATUS ConsoleVar(", gop_start)
        console_end = SOURCE.index("static EFI_STATUS ExactOriginalDriver(", gop_end)
        code = SOURCE[path_start:path_end] + SOURCE[gop_start:console_end]
        prefix = r"""
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
typedef void VOID;
typedef void *EFI_HANDLE;
typedef uint64_t EFI_STATUS;
typedef uintptr_t UINTN;
typedef uint8_t UINT8;
typedef uint16_t CHAR16;
typedef uint32_t UINT32;
typedef struct { UINT8 Type,SubType,Length[2]; } EFI_DEVICE_PATH_PROTOCOL;
typedef struct {
  EFI_HANDLE handle,owner;
  void *io;
  EFI_DEVICE_PATH_PROTOCOL *path;
  UINTN path_size;
} GPU;
typedef struct {
  EFI_STATUS (*LocateHandleBuffer)(
    int,void *,void *,UINTN *,EFI_HANDLE **);
  EFI_STATUS (*HandleProtocol)(EFI_HANDLE,void *,void **);
  EFI_STATUS (*FreePool)(void *);
} EFI_BOOT_SERVICES;
typedef struct {
  EFI_STATUS (*GetVariable)(CHAR16 *,void *,UINT32 *,UINTN *,UINT8 *);
} EFI_RUNTIME_SERVICES;
typedef struct {
  EFI_BOOT_SERVICES *BootServices;
  EFI_RUNTIME_SERVICES *RuntimeServices;
} SYSTEM;
#define ByProtocol 2
#define EFI_VARIABLE_BOOTSERVICE_ACCESS 0x2U
#define EFI_COMPROMISED_DATA UINT64_C(0x8000000000000021)
#define EFI_SUCCESS ((EFI_STATUS)0)
#define EFI_ACCESS_DENIED UINT64_C(0x800000000000000f)
#define EFI_DEVICE_ERROR UINT64_C(0x8000000000000007)
#define EFI_ERROR(s) (((s) & UINT64_C(0x8000000000000000)) != 0)
static int GopGuid,PathGuid;
static GPU Cards[3];
static UINTN AcceptedPeerGopCount=(UINTN)-1;
static EFI_HANDLE handles[65];
static UINT8 card_paths[3][12],gop_paths[6][16];
static UINTN active_count;
static EFI_STATUS Locate(int mode,void *guid,void *unused,
                         UINTN *count,EFI_HANDLE **out) {
  (void)mode; (void)guid; (void)unused;
  *count=active_count; *out=handles; return EFI_SUCCESS;
}
static EFI_STATUS Handle(EFI_HANDLE handle,void *guid,void **path) {
  uintptr_t index=(uintptr_t)handle;
  (void)guid;
  if(index>=6 || index==5)return EFI_DEVICE_ERROR;
  *path=(void *)gop_paths[index];
  return EFI_SUCCESS;
}
static EFI_STATUS Free(void *pointer) { (void)pointer; return EFI_SUCCESS; }
static EFI_BOOT_SERVICES Boot={Locate,Handle,Free};
static UINT8 var_bytes[128];
static UINTN var_size;
static UINT32 var_attrs=7;
static int GlobalGuid;
static EFI_STATUS GetVar(CHAR16 *name,void *guid,UINT32 *attrs,
                         UINTN *size,UINT8 *out) {
  (void)name; (void)guid;
  if(*size<var_size)return EFI_DEVICE_ERROR;
  *attrs=var_attrs; *size=var_size;
  memcpy(out,var_bytes,var_size);
  return EFI_SUCCESS;
}
static EFI_RUNTIME_SERVICES Runtime={GetVar};
static SYSTEM Table={&Boot,&Runtime},*Sys=&Table;
static void Say(const char *fmt,...) { (void)fmt; }
"""
        suffix = r"""
static void make_path(UINT8 *data,UINT8 subtype,UINTN total) {
  UINTN i;
  memset(data,0,total);
  data[0]=1; data[1]=subtype; data[2]=4;
  data[4]=2; data[5]=subtype; data[6]=4;
  for(i=8;i+4<total;i+=4) {
    data[i]=3; data[i+1]=(UINT8)(i/4); data[i+2]=4;
  }
  data[total-4]=0x7f; data[total-3]=0xff; data[total-2]=4;
}
static int run(unsigned id,UINTN count,int a,int b,int c,EFI_STATUS expected) {
  EFI_STATUS got;
  active_count=count;
  handles[0]=(EFI_HANDLE)(uintptr_t)a;
  handles[1]=(EFI_HANDLE)(uintptr_t)b;
  handles[2]=(EFI_HANDLE)(uintptr_t)c;
  got=Gops();
  if(got!=expected) {
    fprintf(stderr,"case %u: got %llx expected %llx\n",
            id,(unsigned long long)got,(unsigned long long)expected);
    return 1;
  }
  return 0;
}
static void make_single_amd_console(void) {
  memset(var_bytes,0,sizeof(var_bytes));
  var_size=36;
  memcpy(var_bytes,card_paths[2],8);
  var_bytes[8]=3; var_bytes[9]=1; var_bytes[10]=24;
  var_bytes[32]=0x7f; var_bytes[33]=0xff; var_bytes[34]=4;
}
static void make_mixed_console(void) {
  memset(var_bytes,0,sizeof(var_bytes));
  var_size=84;
  memcpy(var_bytes,card_paths[1],8);
  var_bytes[8]=3; var_bytes[9]=1; var_bytes[10]=32;
  var_bytes[40]=0x7f; var_bytes[41]=1; var_bytes[42]=4;
  memcpy(var_bytes+44,card_paths[2],8);
  var_bytes[52]=3; var_bytes[53]=1; var_bytes[54]=28;
  var_bytes[80]=0x7f; var_bytes[81]=0xff; var_bytes[82]=4;
}
static int check_console(unsigned id,EFI_STATUS expected) {
  CHAR16 name[]={'C','o','n','O','u','t',0};
  EFI_STATUS got=ConsoleVar(name);
  if(got!=expected) {
    fprintf(stderr,"console case %u: got %llx expected %llx\\n",
            id,(unsigned long long)got,(unsigned long long)expected);
    return 1;
  }
  return 0;
}
int main(void) {
  UINTN i;
  for(i=0;i<3;i++) {
    make_path(card_paths[i],(UINT8)(i+1),12);
    Cards[i].path=(EFI_DEVICE_PATH_PROTOCOL *)card_paths[i];
    Cards[i].path_size=12;
    make_path(gop_paths[i],(UINT8)(i+1),16);
  }
  make_path(gop_paths[3],0x20,16);
  make_path(gop_paths[4],3,16);
  gop_paths[4][14]=0; /* invalid terminating node length */
  make_path(gop_paths[5],2,16);
  if(run(1,2,1,2,0,EFI_SUCCESS))return 1;
  if(run(2,2,2,1,0,EFI_SUCCESS))return 1;
  if(run(3,1,2,0,0,EFI_SUCCESS))return 1;
  if(run(4,2,0,2,0,EFI_ACCESS_DENIED))return 1;
  if(run(5,3,1,2,0,EFI_ACCESS_DENIED))return 1;
  if(run(6,2,1,1,0,EFI_ACCESS_DENIED))return 1;
  if(run(7,2,1,3,0,EFI_ACCESS_DENIED))return 1;
  if(run(8,2,1,4,0,EFI_ACCESS_DENIED))return 1;
  if(run(9,2,1,5,0,EFI_ACCESS_DENIED))return 1;
  if(run(10,0,1,2,0,EFI_ACCESS_DENIED))return 1;
  if(run(11,65,1,2,0,EFI_ACCESS_DENIED))return 1;
  if(run(12,1,1,0,0,EFI_ACCESS_DENIED))return 1;
  if(run(13,1,0,0,0,EFI_ACCESS_DENIED))return 1;
  if(run(14,1,3,0,0,EFI_ACCESS_DENIED))return 1;
  if(run(15,1,4,0,0,EFI_ACCESS_DENIED))return 1;
  if(run(16,1,5,0,0,EFI_ACCESS_DENIED))return 1;
  if(run(17,2,2,2,0,EFI_ACCESS_DENIED))return 1;
  if(run(18,1,2,0,0,EFI_SUCCESS))return 1;
  make_single_amd_console();
  if(check_console(19,EFI_SUCCESS))return 1;
  var_bytes[1]=1; /* RTX4090 path: must not be accepted */
  if(check_console(20,EFI_ACCESS_DENIED))return 1;
  make_single_amd_console();
  var_bytes[33]=1; /* end-instance instead of end-entire */
  if(check_console(21,EFI_COMPROMISED_DATA))return 1;
  make_single_amd_console();
  var_attrs=0;
  if(check_console(22,EFI_ACCESS_DENIED))return 1;
  var_attrs=7;
  if(run(23,2,1,2,0,EFI_SUCCESS))return 1;
  make_mixed_console();
  if(check_console(24,EFI_SUCCESS))return 1;
  make_single_amd_console();
  if(check_console(25,EFI_ACCESS_DENIED))return 1;
  if(run(26,1,0,0,0,EFI_ACCESS_DENIED))return 1;
  make_single_amd_console();
  if(check_console(27,EFI_ACCESS_DENIED))return 1;
  puts("PASS actual GOP/ConsoleVar() C: 27 strict admission cases");
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "gops.c"
            executable = Path(directory) / "gops"
            source.write_text(prefix + code + suffix, encoding="utf-8")
            result = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 str(source), "-o", str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("27 strict admission cases", result.stdout)

    def test_gop_denial_explains_count_and_each_path(self):
        before_stop = SOURCE[:SOURCE.index("STOP_ATTEMPT=IMMINENT")]
        for label in ("STOP_PREFLIGHT_GOP_ENUM STATUS=",
                      "STOP_PREFLIGHT_GOP_ENTRY INDEX=",
                      "STOP_PREFLIGHT_GOP_COUNTS TARGET=",
                      "STOP_PREFLIGHT_CONSOLE_VARIABLE NAME=",
                      "STOP_PREFLIGHT_CONSOLE_COUNTS NAME=",
                      "STOP_PREFLIGHT_CONSOLE_AFTER_GOP_REFUSAL"):
            self.assertIn(label, before_stop)
        gops = SOURCE.split("static EFI_STATUS Gops(VOID) {", 1)[1].split(
            "static EFI_STATUS ConsoleVar(", 1)[0]
        self.assertLess(gops.index("STOP_PREFLIGHT_GOP_ENTRY INDEX="),
                        gops.index("STOP_PREFLIGHT_GOP_COUNTS TARGET="))
        self.assertIn("if(invalid || each[0]!=0 || each[2]!=1", gops)
        self.assertIn("(count==1 && each[1]==0)", gops)
        self.assertIn("(count==2 && each[1]==1)", gops)
        self.assertIn("AcceptedPeerGopCount=each[1]", gops)
        self.assertLess(before_stop.index("STOP_PREFLIGHT_CONSOLE_AFTER_GOP_REFUSAL"),
                        before_stop.index("s=KeyY();"))

    def test_optional_windows_handoff_is_separately_gated_and_fail_closed(self):
        self.assertIn("if ($WindowsEspGuid -and -not $AcceptWindowsHandoffRisk)",BUILD)
        self.assertIn("if ($AcceptWindowsHandoffRisk -and -not $WindowsEspGuid)",BUILD)
        self.assertIn("$sources+=@('gpu_windows_handoff.c','gpu_windows_path.c')",BUILD)
        self.assertIn("$flags+=' /DGU_STOP_WINDOWS_HANDOFF'",BUILD)
        self.assertIn('MSI-$targetName-STOP-WINDOWS-HANDOFF-EXPERIMENT.EFI',BUILD)
        # Windows image preparation happens after explicit Y, before the Stop;
        # a post-Stop boot is forbidden unless every relevant guard passes.
        self.assertIn("#ifdef GU_STOP_WINDOWS_HANDOFF",SOURCE)
        self.assertIn("GuWindowsBootManagerPrepare(image,table,&PreparedWindowsImage)",SOURCE)
        self.assertIn("GuWindowsBootManagerStartPrepared(",SOURCE)
        self.assertLess(SOURCE.index("s=KeyY();"),
                        SOURCE.index("GuWindowsBootManagerPrepare("))
        self.assertLess(SOURCE.index("GuWindowsBootManagerPrepare("),
                        SOURCE.index("s=table->BootServices->DisconnectController("))
        self.assertLess(SOURCE.index("s=table->BootServices->DisconnectController("),
                        SOURCE.index("GuWindowsBootManagerStartPrepared("))
        guarded=SOURCE.split("BOOLEAN eligible=",1)[1].split("if(eligible && Log && Root)",1)[0]
        for required in ("post_unbound==EFI_SUCCESS",
                         "post_target==EFI_ACCESS_DENIED",
                         "post_peer==EFI_SUCCESS && Cards[1].owner==expected_peer",
                         "post_amd==EFI_SUCCESS && Cards[2].owner==expected_amd",
                         "post_gop==EFI_SUCCESS",
                         "post_conout==EFI_SUCCESS && post_errout==EFI_SUCCESS",
                         "PreparedWindowsImage!=NULL"):
            self.assertIn(required,guarded)
        before_start=SOURCE.split("if(eligible && Log && Root)",1)[1].split(
            "GuWindowsBootManagerStartPrepared(",1)[0]
        self.assertIn("WINDOWS_HANDOFF=STARTING_PINNED_BOOT_MANAGER",before_start)
        self.assertIn("record=Flush();",before_start)
        self.assertIn("record=Log->Close(Log);",before_start)
        self.assertIn("record=Root->Close(Root);",before_start)
        # The original one-card Stop count and production fail-closed remain.
        self.assertIn("HaltAfterAttempt();",SOURCE)

    def test_post_stop_unbound_evidence_is_actual_c_not_owner_denial(self):
        import shutil
        import subprocess
        import tempfile
        compiler = shutil.which("gcc") or shutil.which("clang")
        if not compiler:
            self.skipTest("A host C compiler is required")
        start = SOURCE.index("static EFI_STATUS TargetHasNoDriverAfterStop(VOID) {")
        end = SOURCE.index("#endif", start)
        actual = SOURCE[start:end]
        prefix = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef void VOID;
typedef void *EFI_HANDLE;
typedef uint64_t EFI_STATUS;
typedef uintptr_t UINTN;
typedef uint32_t UINT32;
typedef struct {
  EFI_HANDLE AgentHandle, ControllerHandle;
  UINT32 Attributes, OpenCount;
} EFI_OPEN_PROTOCOL_INFORMATION_ENTRY;
#define EFI_SUCCESS UINT64_C(0)
#define EFI_ACCESS_DENIED UINT64_C(0x800000000000000f)
#define EFI_DEVICE_ERROR UINT64_C(0x8000000000000007)
#define EFI_OPEN_PROTOCOL_BY_DRIVER 0x10
#define EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER 0x8
#define EFI_OPEN_PROTOCOL_EXCLUSIVE 0x20
#define EFI_OPEN_PROTOCOL_GET_PROTOCOL 0x2
#define EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL 0x1
#define EFI_ERROR(s) (!!((s)&UINT64_C(0x8000000000000000)))
static EFI_OPEN_PROTOCOL_INFORMATION_ENTRY entries[2];
static UINTN entry_count=1;
static EFI_STATUS open_status=EFI_SUCCESS;
static EFI_STATUS free_status=EFI_SUCCESS;
static int PciGuid;
typedef struct {EFI_HANDLE handle;} GPU;
static GPU Cards[3];
typedef struct {
  EFI_STATUS (*OpenProtocolInformation)(
    EFI_HANDLE,void *,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **,UINTN *);
  EFI_STATUS (*FreePool)(void *);
} EFI_BOOT_SERVICES;
typedef struct {EFI_BOOT_SERVICES *BootServices;} SYSTEM;
static EFI_STATUS Open(EFI_HANDLE handle,void *guid,
                       EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **out,UINTN *count) {
  (void)guid;
  if(handle!=Cards[0].handle)return EFI_DEVICE_ERROR;
  if(EFI_ERROR(open_status))return open_status;
  *out=entries; *count=entry_count; return EFI_SUCCESS;
}
static EFI_STATUS Free(void *data) {
  return data==entries?free_status:EFI_DEVICE_ERROR;
}
static EFI_BOOT_SERVICES Boot={Open,Free};
static SYSTEM Table={&Boot},*Sys=&Table;
static void Say(const char *format,...) {(void)format;}
"""
        suffix = r"""
static int check(unsigned id,EFI_STATUS expected) {
  EFI_STATUS actual=TargetHasNoDriverAfterStop();
  if(actual!=expected) {
    fprintf(stderr,"case %u: got %#llx wanted %#llx\n",id,
            (unsigned long long)actual,(unsigned long long)expected);
    return 1;
  }
  return 0;
}
int main(void) {
  Cards[0].handle=(EFI_HANDLE)(uintptr_t)0x123;
  entries[0]=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){
    (EFI_HANDLE)(uintptr_t)1,NULL,EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL,38};
  if(check(1,EFI_SUCCESS))return 1;
  entries[0].Attributes=EFI_OPEN_PROTOCOL_GET_PROTOCOL;
  if(check(2,EFI_SUCCESS))return 1;
  entries[0].Attributes=EFI_OPEN_PROTOCOL_BY_DRIVER;
  if(check(3,EFI_ACCESS_DENIED))return 1;
  entries[0].Attributes=EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER;
  if(check(4,EFI_ACCESS_DENIED))return 1;
  entries[0].Attributes=EFI_OPEN_PROTOCOL_EXCLUSIVE;
  if(check(5,EFI_ACCESS_DENIED))return 1;
  entries[0].Attributes=0x40;
  if(check(6,EFI_ACCESS_DENIED))return 1;
  entries[0].Attributes=EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL;
  entries[0].ControllerHandle=(EFI_HANDLE)(uintptr_t)2;
  if(check(7,EFI_ACCESS_DENIED))return 1;
  entries[0].ControllerHandle=NULL;
  entries[0].AgentHandle=NULL;
  if(check(8,EFI_ACCESS_DENIED))return 1;
  entries[0].AgentHandle=(EFI_HANDLE)(uintptr_t)1;
  entries[0].OpenCount=0;
  if(check(9,EFI_ACCESS_DENIED))return 1;
  entries[0].OpenCount=38;
  entry_count=0;
  if(check(10,EFI_ACCESS_DENIED))return 1;
  entry_count=65;
  if(check(11,EFI_ACCESS_DENIED))return 1;
  entry_count=1;
  open_status=EFI_DEVICE_ERROR;
  if(check(12,EFI_ACCESS_DENIED))return 1;
  open_status=EFI_SUCCESS;
  free_status=EFI_DEVICE_ERROR;
  if(check(13,EFI_DEVICE_ERROR))return 1;
  free_status=EFI_SUCCESS;
  entry_count=2;
  entries[1]=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){
    (EFI_HANDLE)(uintptr_t)2,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL,1};
  if(check(14,EFI_SUCCESS))return 1;
  entries[1].Attributes=EFI_OPEN_PROTOCOL_BY_DRIVER;
  if(check(15,EFI_ACCESS_DENIED))return 1;
  puts("PASS actual post-Stop target-unbound C: 15 independent scenarios");
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "unbound.c"
            exe = Path(directory) / "unbound"
            source.write_text(prefix + actual + suffix, encoding="utf-8")
            result = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 str(source), "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("15 independent scenarios", result.stdout)

    def test_build_is_explicitly_opt_in_and_does_not_change_passive_image(self):
        self.assertIn("if (-not $AcceptUnboundedStopRisk)",BUILD)
        self.assertIn("'msi_stop_observation.c','sha256.c','format.c','runtime.c'",BUILD)
        self.assertIn('MSI-$targetName-STOP-ONCE-EXPERIMENT.EFI',BUILD)
        for source in ("unlock_execution.c","msi_release.c","gpu_topology_efi.c",
                       "app.c","dxe_main.c"):
            self.assertNotIn("'"+source+"'", BUILD)
        self.assertNotIn("GU_EXPERIMENTAL_STOP_OBSERVATION", PASSIVE)
        self.assertIn("GU_GPU_DIAGNOSTIC_ONLY", PASSIVE)


if __name__=="__main__":
    unittest.main()
