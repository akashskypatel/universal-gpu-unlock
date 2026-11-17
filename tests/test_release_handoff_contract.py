"""Prevent post-close GPU register reads and weakened PCI drift checks.

This is an explicit source-order test. It does not emulate EFI firmware.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

SOURCE = (Path(__file__).resolve().parents[1] / "src/msi_release.c").read_text(encoding="utf-8")


def function_body(name):
    match = re.search(
        r"\b(?:static\s+)?(?:BOOLEAN|EFI_STATUS)\s+" + re.escape(name) +
        r"\s*\([^)]*\)\s*\{",
        SOURCE,
    )
    if match is None:
        raise AssertionError("missing function: " + name)
    at, depth, start = match.end(), 1, match.end()
    while at < len(SOURCE) and depth:
        depth += (SOURCE[at] == "{") - (SOURCE[at] == "}")
        at += 1
    if depth:
        raise AssertionError("unterminated function: " + name)
    return SOURCE[start:at - 1]


class HandoverOrderTests(unittest.TestCase):
    def test_original_rtx_4090_and_3080_release_refuses_before_vendor_stop(self):
        """Source contract: known active GPUs cannot reach proprietary Stop."""
        release = function_body("GuMsiRelease")
        readiness = release.index("if(!Plan.ready||Plan.mutated")
        guard = re.search(
            r"if\(Gu\.device_id==0x2684U\|\|Gu\.device_id==0x220aU\)"
            r"\s*\{(?P<body>[^{}]*)\}",
            release,
        )
        self.assertIsNotNone(guard, "Missing exact AD102/GA102 safety refusal")
        self.assertIn("return EFI_UNSUPPORTED;", guard.group("body"))
        self.assertLess(readiness, guard.start())
        self.assertLess(guard.end(), release.index("Plan.table->BootServices"))
        self.assertLess(guard.end(), release.index("DisconnectController("))
        self.assertLess(guard.end(), release.index("GuAd104ColdEntry()"))
        self.assertIn("signed loader not verified", guard.group("body"))

    def test_preclose_reads_require_valid_claim_and_full_register_checks(self):
        pre = function_body("GuMsiHardwareVerifiedBeforeClose")
        self.assertEqual(pre.count("GuRead32("), 7)
        self.assertIn("!Plan.claimed", pre)
        self.assertIn("!Gu.owned", pre)
        self.assertIn("Gu.fault", pre)
        for assertion in (
            "architecture==0x06U", "architecture==0x07U",
            "architecture==0x09U", "(cpu&0x10)", "!(hi>>4)",
            "ss0==0x88888888", "ss1==8",
            "!(readout&0x07ffffff)", "plm==0xffffffff",
        ):
            with self.subTest(assertion=assertion):
                self.assertIn(assertion, pre)

    def test_finish_checks_hardware_before_closing_pci(self):
        finish = function_body("GuMsiFinish")
        check = finish.index("GuMsiHardwareVerifiedBeforeClose()")
        close = finish.index("CloseProtocol(")
        self.assertLess(check, close)
        self.assertIn("return EFI_DEVICE_ERROR;", finish[check:close])
        self.assertIn("Plan.hardware_verified=TRUE;", finish[check:close])
        self.assertIn("Plan.hardware_verified=FALSE;", finish[close:])
        self.assertIn("Snapshot(&Current)", finish[close:])
        self.assertIn("Expected(3,FALSE)", finish[close:])
        # Post-close success must include a fresh survivor path comparison
        # and independent AMD/peer ownership. A later handover check alone
        # must not hide a successful finish with a changed GOP path.
        after = finish[close:]
        path_check = after.index("Gops(FALSE)")
        amd_check = after.index("ReadOnlyAmdConsoleOwnership()")
        peer_check = after.index("GuVerifyActivePeers(Plan.table)")
        self.assertLess(after.index("Expected(3,FALSE)"), path_check)
        self.assertLess(path_check, amd_check)
        self.assertLess(amd_check, peer_check)
        self.assertLess(peer_check, after.rindex("return s;"))
        self.assertIn("Plan.hardware_verified=FALSE;", after)
        self.assertNotIn("GuRead32(", after)

    def test_handover_retains_ownership_checks_without_postclose_mmio(self):
        handover = function_body("GuMsiReadyForWindows")
        for requirement in (
            "!Plan.hardware_verified", "!GuSafeToReturn()",
            "Alive()", "Gops(FALSE)", "ReadConfig(c)", "ConfigStable(c)",
        ):
            with self.subTest(requirement=requirement):
                self.assertIn(requirement, handover)
        self.assertNotIn("GuRead32(", handover)
        self.assertNotIn("Snapshot(", handover)

    def test_failed_handover_revokes_verification_for_later_calls(self):
        handover = function_body("GuMsiReadyForWindows")
        # After the finish latch is set, any failed read-only post-close
        # revalidation must permanently refuse handover, not allow retry.
        verification_guard = handover.index("!Plan.hardware_verified")
        early_failure = handover.index("Plan.hardware_verified=FALSE;", verification_guard)
        early_return = handover.index("return FALSE;", early_failure)
        ready_check = handover.index("BOOLEAN ready=")
        self.assertLess(verification_guard, early_failure)
        self.assertLess(early_failure, early_return)
        self.assertLess(early_return, ready_check)
        invalidate = handover.index("if(!ready)Plan.hardware_verified=FALSE;")
        success_return = handover.index("return ready;")
        self.assertLess(verification_guard, ready_check)
        self.assertLess(ready_check, invalidate)
        self.assertLess(invalidate, success_return)
        for check in ("Alive()", "Gops(FALSE)", "ReadOnlyAmdConsoleOwnership()",
                      "ReadConfig(c)", "ConfigStable(c)",
                      "GuVerifyActivePeers(Plan.table)"):
            self.assertIn(check, handover[ready_check:invalidate])
        self.assertNotIn("GuRead32(", handover)
        self.assertNotIn("Snapshot(", handover)
        # This may not turn a failed handover into a new claim or replay.
        self.assertNotIn("ClaimRestore(", handover)
        self.assertNotIn("CloseProtocol(", handover)
        self.assertNotIn("DisconnectController(", handover)

    def test_bus_master_exception_rejects_set_or_unrelated_changes(self):
        stable = function_body("ConfigStable")
        for required in (
            "Gu.flrs<=1", "i==1", "drift==4u",
            "GuPciMasterClearOnly(Plan.config[i],c[i])",
            "if(!allowed)",
        ):
            with self.subTest(required=required):
                self.assertIn(required, stable)
        self.assertIn("Gu.flrs==1", stable)
        self.assertIn("drift&~0x00090000u", stable)


    def test_stop_then_bus_master_clear_then_exclusive_claim(self):
        release = function_body("GuMsiRelease")
        self.assertLess(release.index("DisconnectController("),
                        release.index("NormalizeMasterAfterStop()"))
        self.assertLess(release.index("NormalizeMasterAfterStop()"),
                        release.index("ClaimRestore()"))
        normalize = function_body("NormalizeMasterAfterStop")
        self.assertIn("GuVerifyActivePeers",normalize)
        self.assertIn("EfiPciIoAttributeOperationDisable",normalize)
        self.assertIn("Gu.retained=TRUE",normalize)
        self.assertNotIn("GuConfigWrite(",normalize)
        self.assertIn("!Plan.master_normalized",function_body("GuMsiReleased"))

    def test_amd_owner_rechecked_after_each_stop_and_before_handover(self):
        # A post-Stop controller identity change must not be accepted solely
        # because its previously captured GOP and console edges still exist.
        release = function_body("GuMsiRelease")
        stop = release.index("DisconnectController(")
        self.assertIn("ReadOnlyAmdConsoleOwnership()", release[:stop])
        self.assertIn("ReadOnlyAmdConsoleOwnership()", release[stop:])
        self.assertLess(release.index("ReadOnlyAmdConsoleOwnership()", stop),
                        release.index("NormalizeMasterAfterStop()"))
        released = function_body("GuMsiReleased")
        self.assertIn('ReadOnlyAmdConsoleOwnership()!=EFI_SUCCESS', released)
        self.assertLess(released.index("ReadOnlyAmdConsoleOwnership()"),
                        released.index("Snapshot(&Current)"))
        finish = function_body("GuMsiFinish")
        after_close = finish.index("CloseProtocol(")
        self.assertIn("ReadOnlyAmdConsoleOwnership()", finish[after_close:])
        self.assertIn("Plan.hardware_verified=FALSE;", finish[after_close:])
        handover = function_body("GuMsiReadyForWindows")
        self.assertIn("ReadOnlyAmdConsoleOwnership()==EFI_SUCCESS", handover)
        self.assertNotIn("Snapshot(", handover)
        self.assertNotIn("GuRead32(", handover)

    def test_gop_survivor_path_is_pinned_before_vendor_stop(self):
        gops = function_body("Gops")
        release = function_body("GuMsiRelease")
        self.assertIn("GuDevicePathLength(", gops)
        self.assertIn("record->path_bytes=path_bytes;", gops)
        self.assertIn("memcpy(record->path,live_path,path_bytes);", gops)
        self.assertIn("GuDevicePathUnchanged(", gops)
        self.assertLess(gops.index("if(first) {", gops.index("EFI_DEVICE_PATH_PROTOCOL *live_path")),
                        gops.index("GuDevicePathUnchanged("))
        self.assertLess(release.index("Gops(FALSE)"),release.index("DisconnectController("))
        self.assertIn("Gops(FALSE)",release[release.index("DisconnectController("):])
        self.assertIn("Gops(FALSE)",function_body("GuMsiReadyForWindows"))

    def test_dynamic_survivor_capture_and_recheck(self):
        # Exercise the actual Gops function, not a second implementation.
        start = SOURCE.index("#define MAX_SURVIVOR_PATH_BYTES")
        end = SOURCE.index("#define GRAPH_EDGE_LIMIT", start)
        records = SOURCE[start:end]
        body = ("static EFI_STATUS GopOnTarget(EFI_HANDLE gop,BOOLEAN *target) {" +
                function_body("GopOnTarget") + "}\n" +
                "static EFI_STATUS Gops(BOOLEAN first) {" +
                function_body("Gops") + "}\n")
        program = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu_device_path.h"
typedef void VOID;
typedef void *EFI_HANDLE;
typedef size_t UINTN;
typedef uint8_t UINT8;
typedef int BOOLEAN;
typedef int EFI_STATUS;
typedef int EFI_GUID;
typedef unsigned char EFI_DEVICE_PATH_PROTOCOL;
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_NOT_FOUND 1
#define EFI_UNSUPPORTED 2
#define EFI_ALREADY_STARTED 3
#define EFI_NOT_READY 4
#define EFI_BAD_BUFFER_SIZE 5
#define EFI_OUT_OF_RESOURCES 6
#define EFI_COMPROMISED_DATA 7
#define EFI_DEVICE_ERROR 8
#define EFI_INVALID_PARAMETER 9
#define EfiLoaderData 1
#define ByProtocol 2
typedef struct { unsigned value; } EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;
typedef struct {
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
  UINTN SizeOfInfo,FrameBufferBase,FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;
typedef struct { EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode; } EFI_GRAPHICS_OUTPUT_PROTOCOL;
typedef struct {
  EFI_STATUS (*LocateHandleBuffer)(int,EFI_GUID *,VOID *,UINTN *,EFI_HANDLE **);
  EFI_STATUS (*AllocatePool)(int,UINTN,VOID **);
  EFI_STATUS (*FreePool)(VOID *);
} EFI_BOOT_SERVICES;
typedef struct { EFI_BOOT_SERVICES *BootServices; } EFI_SYSTEM_TABLE;
""" + records + r"""
typedef struct {
  EFI_SYSTEM_TABLE *table;
  EFI_HANDLE pci,gop;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop_interface;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info;
  SURVIVOR_GOP *survivors;
  UINTN survivor_count,survivor_capacity,stage;
  BOOLEAN headless,survivor_baseline_valid;
} PLAN;
static PLAN first_plan,second_plan,failed_plan,duplicate_plan;
static PLAN *active=&first_plan;
#define Plan (*active)
enum { N=21 };
typedef struct {
  unsigned char path[10];
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
  EFI_GRAPHICS_OUTPUT_PROTOCOL gop,alias;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *interface;
} NODE;
static NODE nodes[N];
static EFI_HANDLE order[N];
static UINTN visible=N,allocations;
static int fail_alloc;
static EFI_GUID GopGuid=1,PathGuid=2;
static EFI_STATUS Locate(int kind,EFI_GUID *guid,VOID *key,UINTN *count,EFI_HANDLE **out) {
  (void)key;
  if(kind!=ByProtocol||guid!=&GopGuid)return EFI_INVALID_PARAMETER;
  *count=visible;
  *out=malloc(visible*sizeof(**out));
  if(!*out)return EFI_OUT_OF_RESOURCES;
  memcpy(*out,order,visible*sizeof(**out));
  return EFI_SUCCESS;
}
static EFI_STATUS Allocate(int kind,UINTN size,VOID **out) {
  if(kind!=EfiLoaderData||fail_alloc)return EFI_OUT_OF_RESOURCES;
  *out=calloc(1,size);
  if(!*out)return EFI_OUT_OF_RESOURCES;
  allocations++;
  return EFI_SUCCESS;
}
static EFI_STATUS Free(VOID *p) {free(p);return EFI_SUCCESS;}
static EFI_STATUS Interface(EFI_HANDLE h,EFI_GUID *guid,VOID **out) {
  NODE *node=h;
  if(!node)return EFI_NOT_FOUND;
  *out=guid==&GopGuid?(VOID *)node->interface:(VOID *)node->path;
  return EFI_SUCCESS;
}
static BOOLEAN Equal(const VOID *a,const VOID *b,UINTN n){return !memcmp(a,b,n);}
static void GuLog(const char *format,...){(void)format;}
""" + body + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(void){
  EFI_BOOT_SERVICES bs={Locate,Allocate,Free};
  EFI_SYSTEM_TABLE table={&bs};
  for(UINTN i=0;i<N;i++){
    unsigned char path[]={1,1,6,0,(unsigned char)i,0,0x7f,0xff,4,0};
    memcpy(nodes[i].path,path,sizeof(path));
    nodes[i].info.value=(unsigned)i;
    nodes[i].mode.Info=&nodes[i].info;
    nodes[i].mode.SizeOfInfo=sizeof(nodes[i].info);
    nodes[i].mode.FrameBufferBase=1+i;
    nodes[i].mode.FrameBufferSize=4096;
    nodes[i].gop.Mode=&nodes[i].mode;
    nodes[i].alias.Mode=&nodes[i].mode;
    nodes[i].interface=&nodes[i].gop;
    order[i]=&nodes[i];
  }
  Plan.table=&table;Plan.pci=&nodes[0];
  CHECK(Gops(TRUE)==EFI_SUCCESS);
  CHECK(Plan.survivor_count==20 && Plan.survivor_capacity==N);
  CHECK(Plan.survivor_baseline_valid && allocations==1);
  CHECK(Plan.survivors[0].handle==&nodes[1]);
  CHECK(Gops(TRUE)==EFI_ALREADY_STARTED);
  EFI_HANDLE saved=order[1];order[1]=order[20];order[20]=saved;
  CHECK(Gops(FALSE)==EFI_SUCCESS && allocations==1);
  order[20]=order[1];
  CHECK(Gops(FALSE)==EFI_NOT_READY);
  order[20]=saved;
  CHECK(Gops(FALSE)==EFI_SUCCESS);
  visible=N-1;
  CHECK(Gops(FALSE)==EFI_NOT_READY);
  visible=N;
  nodes[1].path[4]=99;
  CHECK(Gops(FALSE)==EFI_NOT_READY && Plan.survivors[0].path[4]==1);
  nodes[1].path[4]=1;
  nodes[1].interface=&nodes[1].alias;
  CHECK(Gops(FALSE)==EFI_NOT_READY);
  nodes[1].interface=&nodes[1].gop;
  nodes[1].info.value=99;
  CHECK(Gops(FALSE)==EFI_NOT_READY);
  nodes[1].info.value=1;
  CHECK(Gops(FALSE)==EFI_SUCCESS);
  active=&second_plan;Plan.table=&table;Plan.pci=&nodes[1];
  CHECK(Gops(TRUE)==EFI_SUCCESS && allocations==2);
  CHECK(Plan.survivors!=first_plan.survivors && Plan.survivor_count==20);
  CHECK(Plan.gop==&nodes[1] && first_plan.gop==&nodes[0]);
  CHECK(Gops(FALSE)==EFI_SUCCESS);
  active=&first_plan;
  CHECK(Gops(FALSE)==EFI_SUCCESS && first_plan.survivors[0].handle==&nodes[1]);
  active=&failed_plan;Plan.table=&table;Plan.pci=&nodes[0];
  fail_alloc=1;
  CHECK(Gops(TRUE)==EFI_OUT_OF_RESOURCES && !Plan.survivors);
  CHECK(!Plan.survivor_baseline_valid && Gops(FALSE)==EFI_NOT_READY);
  fail_alloc=0;
  active=&duplicate_plan;Plan.table=&table;Plan.pci=&nodes[0];
  nodes[1].interface=&nodes[20].gop;
  CHECK(Gops(TRUE)==EFI_COMPROMISED_DATA && !Plan.survivor_baseline_valid);
  CHECK(Gops(FALSE)==EFI_NOT_READY);
  free(first_plan.survivors);free(second_plan.survivors);
  free(duplicate_plan.survivors);
  return 0;
}
"""
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "survivors.c"
            source.write_text(program, encoding="utf-8")
            for compiler in ("gcc", "clang"):
                with self.subTest(compiler=compiler):
                    binary = Path(directory) / compiler
                    compiled = subprocess.run(
                        [compiler, "-std=c11", "-O2", "-Wall", "-Wextra",
                         "-Werror", "-pedantic", "-I", str(root / "src"),
                         str(source), "-o", str(binary)],
                        capture_output=True, text=True)
                    self.assertEqual(compiled.returncode, 0, compiled.stderr)
                    result = subprocess.run([str(binary)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_idle_output_isolated_from_protected_endpoints(self):
        # The original graph owns the child records. Test the actual
        # classification and removal rules with no firmware callbacks.
        start = SOURCE.index("#define GRAPH_EDGE_LIMIT")
        end = SOURCE.index("\ntypedef struct {\n  EFI_SYSTEM_TABLE", start)
        records = SOURCE[start:end]
        functions = (
            "static BOOLEAN Removed(const EDGE *e,UINTN stage) {" +
            function_body("Removed") + "}\n" +
            "static BOOLEAN ProtectedHandle(EFI_HANDLE h) {" +
            function_body("ProtectedHandle") + "}\n" +
            "static EFI_STATUS ClassifyIdleOutput(EDGE *e) {" +
            function_body("ClassifyIdleOutput") + "}\n")
        program = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
typedef void *EFI_HANDLE;
typedef size_t UINTN;
typedef uint32_t UINT32;
typedef int BOOLEAN;
typedef int EFI_STATUS;
typedef int EFI_GUID;
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_ACCESS_DENIED 1
#define EFI_DEVICE_ERROR 2
#define EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER 8
""" + records + r"""
typedef struct { EFI_HANDLE handle; } SURVIVOR_GOP;
typedef struct {
  EFI_HANDLE pci,gop,self,console,input,error,nv_agent,out_agent,gfx_agent;
  SURVIVOR_GOP *survivors;
  UINTN survivor_count;
} PLAN;
static PLAN Plan;
static GRAPH Original;
static EFI_GUID TextGuid=1,GopGuid=2,PathGuid=3;
static int peer,foreign;
static EFI_HANDLE bad_path;
static int Guid(const EFI_GUID *a,const EFI_GUID *b){return *a==*b;}
static int GuIsPreparedPeerHandle(EFI_HANDLE h){return h==&peer;}
static EFI_STATUS GopOnTarget(EFI_HANDLE h,BOOLEAN *on_target){
  if(h==bad_path)return EFI_DEVICE_ERROR;
  *on_target=h!=&foreign;return EFI_SUCCESS;
}
""" + functions + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(void) {
  int pci,agent,console,gop,self,input,error,out_agent,gfx_agent,child[12],survivor;
  SURVIVOR_GOP survivors[]={{&survivor}};
  EDGE edges[14]={0};
  Plan=(PLAN){&pci,&gop,&self,&console,&input,&error,&agent,
              &out_agent,&gfx_agent,survivors,1};
  Original=(GRAPH){12,14,edges};
  for(UINTN i=0;i<12;i++){
    edges[i]=(EDGE){.handle=&pci,.agent=&agent,.controller=&child[i],
                    .protocol=PathGuid,.attributes=8,.count=1};
  }
  for(UINTN i=0;i<12;i++){
    CHECK(ClassifyIdleOutput(&edges[i])==EFI_SUCCESS);
    CHECK(edges[i].idle_output && ProtectedHandle(&child[i]));
    CHECK(!Removed(&edges[i],2) && Removed(&edges[i],3));
    EDGE unrelated={.handle=&child[i],.controller=&foreign,.agent=&out_agent};
    CHECK(!Removed(&unrelated,3));
  }
  CHECK(ClassifyIdleOutput(&edges[0])==EFI_ACCESS_DENIED);
  Original.count=13;
  EDGE candidate={.handle=&pci,.agent=&agent,.controller=&peer,
                  .protocol=PathGuid,.attributes=8,.count=1};
  edges[12]=candidate;
  CHECK(ClassifyIdleOutput(&edges[12])==EFI_ACCESS_DENIED);
  CHECK(!edges[12].idle_output && ProtectedHandle(&peer));
  edges[12].controller=&console;
  CHECK(ClassifyIdleOutput(&edges[12])==EFI_ACCESS_DENIED);
  edges[12].controller=&survivor;
  CHECK(ClassifyIdleOutput(&edges[12])==EFI_ACCESS_DENIED);
  edges[12].controller=&foreign;
  CHECK(ClassifyIdleOutput(&edges[12])==EFI_ACCESS_DENIED);
  int other;
  edges[12].controller=&other;
  edges[12].agent=&out_agent;
  CHECK(ClassifyIdleOutput(&edges[12])==EFI_ACCESS_DENIED);
  edges[12].agent=&agent;edges[12].count=2;
  CHECK(ClassifyIdleOutput(&edges[12])==EFI_ACCESS_DENIED);
  edges[12].count=1;bad_path=&other;
  CHECK(ClassifyIdleOutput(&edges[12])==EFI_DEVICE_ERROR);
  bad_path=NULL;Original.count=14;
  edges[13]=(EDGE){.handle=&other,.controller=&foreign,.agent=&out_agent};
  CHECK(ClassifyIdleOutput(&edges[12])==EFI_ACCESS_DENIED);
  edges[13]=(EDGE){.handle=&foreign,.controller=&other,.agent=&out_agent};
  CHECK(ClassifyIdleOutput(&edges[12])==EFI_ACCESS_DENIED);
  edges[13]=(EDGE){.handle=&foreign,.controller=&foreign,.agent=&other};
  CHECK(ClassifyIdleOutput(&edges[12])==EFI_ACCESS_DENIED);
  CHECK(!edges[12].idle_output && !ProtectedHandle(&other));
  Original.count=13;
  CHECK(ClassifyIdleOutput(&edges[12])==EFI_SUCCESS && ProtectedHandle(&other));
  CHECK(!Removed(&edges[13],3));
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "idle_output.c"
            source.write_text(program, encoding="utf-8")
            for compiler in ("gcc", "clang"):
                with self.subTest(compiler=compiler):
                    binary = Path(directory) / compiler
                    built = subprocess.run(
                        [compiler, "-std=c11", "-O2", "-Wall", "-Wextra",
                         "-Werror", "-pedantic", str(source), "-o", str(binary)],
                        capture_output=True, text=True)
                    self.assertEqual(built.returncode, 0, built.stderr)
                    checked = subprocess.run([str(binary)], capture_output=True, text=True)
                    self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_amd_owner_validation_is_read_only(self):
        owner = function_body("ReadOnlyAmdConsoleOwnership")
        paths = function_body("AmdConsoleVariable")
        graph = function_body("ReadOnlyAmdConsoleGraph")
        self.assertIn("EfiPciIoWidthUint32", owner)
        self.assertIn("GuDevicePathAmdDisplayOwner(", owner)
        self.assertIn('AmdConsoleVariable(L"ConOut"', owner)
        self.assertIn('AmdConsoleVariable(L"ErrOut"', owner)
        self.assertIn("GetVariable(", paths)
        self.assertIn("matching_gops!=1", paths)
        self.assertIn("EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER", graph)
        for text in (owner, paths, graph):
            for forbidden in ("DisconnectController(", "Pci.Write(",
                              "GuWrite32(", "EfiPciIoAttributeOperationEnable",
                              "SetVariable(", "ResetSystem("):
                self.assertNotIn(forbidden, text)

    def test_release_context_belongs_to_each_platform(self):
        # Compile the actual CPU-only reservation code with mocked EFI allocation.
        left = SOURCE.index("typedef struct GU_MSI_OWNED_STATE {")
        right = SOURCE.index("static EFI_GUID PciGuid=", left)
        graph_start = SOURCE.index("#define GRAPH_EDGE_LIMIT")
        graph_end = SOURCE.index("\ntypedef struct {\n  EFI_SYSTEM_TABLE", graph_start)
        append_start = SOURCE.index("static EFI_STATUS GraphAppend(")
        append_end = SOURCE.index("static EFI_STATUS Snapshot(", append_start)
        production = (SOURCE[graph_start:graph_end] + "\n" + SOURCE[left:right]
                      + SOURCE[append_start:append_end])
        # Include the real public state queries, not mock equivalents.
        production += "BOOLEAN GuMsiMutated(VOID) {" + function_body("GuMsiMutated") + "}\n"
        start = SOURCE.index("const VOID *GuMsiPlanOwnerToken(")
        production += SOURCE[start:SOURCE.index("#ifdef GU_ACTIVE_SINGLE_TARGET", start)]
        production += "BOOLEAN GuMsiCompletedPlatform(const GU_PLATFORM *platform,const VOID *token) {" + function_body("GuMsiCompletedPlatform") + "}\n"
        program = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef void VOID;
typedef int BOOLEAN;
typedef int EFI_STATUS;
typedef size_t UINTN;
typedef uint32_t UINT32;
typedef struct {uint8_t bytes[16];} EFI_GUID;
typedef void *EFI_HANDLE;
typedef struct {int tag;} EFI_PCI_IO_PROTOCOL;
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_NOT_READY 1
#define EFI_ALREADY_STARTED 2
#define EFI_OUT_OF_RESOURCES 3
#define EFI_INVALID_PARAMETER 4
#define EFI_COMPROMISED_DATA 5
#define EFI_BAD_BUFFER_SIZE 6
#define EFI_DEVICE_ERROR 7
#define EfiLoaderData 7
typedef struct {void *table,*pci,*self; int mutated,claimed,ready,stage,master_normalized,hardware_verified;} PLAN;
typedef struct {
  EFI_STATUS (*AllocatePool)(int,size_t,VOID **);
  EFI_STATUS (*FreePool)(VOID *);
} EFI_BOOT_SERVICES;
typedef struct {
  EFI_BOOT_SERVICES *bs;
  EFI_HANDLE handle,image;
  EFI_PCI_IO_PROTOCOL *pci;
  int profile_verified,prom_verified,started,owned,armed,fault,retained;
  struct GU_MSI_OWNED_STATE *release_state;
  BOOLEAN release_state_reserved,release_owner_invalid;
} GU_PLATFORM;
typedef GU_PLATFORM MOCK_PLATFORM;
static MOCK_PLATFORM *platforms,*current;
#define Gu (*current)
static int fail_alloc,fail_free_once;
static EFI_STATUS Allocate(int kind,size_t size,VOID **out) {
  if(kind!=EfiLoaderData || fail_alloc) return EFI_OUT_OF_RESOURCES;
  *out=calloc(1,size);
  return *out?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS Free(VOID *p) {
  if(fail_free_once){fail_free_once=0;return EFI_DEVICE_ERROR;}
  free(p);return EFI_SUCCESS;
}
""" + production + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(void) {
  EFI_BOOT_SERVICES bs={Allocate,Free};
  const size_t count=8;
  const size_t large=4097;
  EFI_PCI_IO_PROTOCOL pci[8]={0};
  EFI_BOOT_SERVICES other_bs={Allocate,Free};
  platforms=calloc(count,sizeof(*platforms));
  CHECK(platforms);
  current=&platforms[0];
  for(size_t i=0;i<count;i++) {
    platforms[i].bs=&bs;
    platforms[i].handle=&platforms[i];
    platforms[i].image=&bs;platforms[i].pci=&pci[i];
    platforms[i].profile_verified=platforms[i].prom_verified=1;
  }
  CHECK(!GuMsiPlanOwnerToken() && !GuMsiMutated());
  CHECK(!GuMsiCompletedPlatform(current,NULL));
  CHECK(GuMsiReserveSelectedState(&bs)==EFI_SUCCESS);
  GU_MSI_OWNED_STATE *first=CurrentMsi;
  CHECK(first && GuMsiPlanOwnerToken()==first && Gu.release_state==first);
  CHECK(!Original.edges && !Current.edges && !Original.capacity && !Current.capacity);
  Plan.table=&bs;Plan.mutated=1;
  EDGE edge={.handle=&pci[0],.usb_baseline=TRUE};
  CHECK(GraphAppend(&bs,&Original,edge)==EFI_SUCCESS);
  edge.usb_baseline=FALSE;
  while(Original.count<Original.capacity)CHECK(GraphAppend(&bs,&Original,edge)==EFI_SUCCESS);
  EDGE *before=Original.edges;size_t capacity=Original.capacity;
  fail_alloc=1;
  CHECK(GraphAppend(&bs,&Original,edge)==EFI_OUT_OF_RESOURCES);
  CHECK(Original.edges==before && Original.capacity==capacity && Original.count==capacity);
  CHECK(Original.edges[0].usb_baseline && Original.edges[0].handle==&pci[0]);
  fail_alloc=0;fail_free_once=1;
  CHECK(GraphAppend(&bs,&Original,edge)==EFI_DEVICE_ERROR);
  CHECK(Original.edges==before && Original.capacity==capacity && Original.count==capacity);
  CHECK(!fail_free_once && Original.edges[0].usb_baseline);
  while(Original.count<large)CHECK(GraphAppend(&bs,&Original,edge)==EFI_SUCCESS);
  CHECK(Original.count==large && Original.capacity>=large && Original.edges[0].usb_baseline);
  CHECK(!Original.edges[large-1].usb_baseline);
  for(size_t i=0;i<7;i++)CHECK(GraphAppend(&bs,&Current,edge)==EFI_SUCCESS);
  CHECK(Current.edges!=Original.edges && !Current.edges[0].usb_baseline);
  CHECK(GuMsiMutated());
  CHECK(GuMsiReserveSelectedState(&bs)==EFI_ALREADY_STARTED);
  current=&platforms[1];
  CHECK(!Gu.release_state && !GuMsiPlanOwnerToken() && !GuMsiMutated());
  fail_alloc=1;
  CHECK(GuMsiReserveSelectedState(&bs)==EFI_OUT_OF_RESOURCES);
  CHECK(!Gu.release_state && !GuMsiPlanOwnerToken() && !GuMsiMutated());
  CHECK(first->original.count==large);
  fail_alloc=0;
  CHECK(GuMsiReserveSelectedState(&bs)==EFI_SUCCESS);
  GU_MSI_OWNED_STATE *second=CurrentMsi;
  CHECK(second!=first && GuMsiPlanOwnerToken()==second && Gu.release_state==second);
  CHECK(!Plan.table && !Original.count && !Current.count && !Original.edges && !Current.edges);
  for(size_t i=0;i<2;i++)CHECK(GraphAppend(&bs,&Original,edge)==EFI_SUCCESS);
  for(size_t i=0;i<3;i++)CHECK(GraphAppend(&bs,&Current,edge)==EFI_SUCCESS);
  CHECK(Original.edges!=first->original.edges && Current.edges!=first->current.edges);
  current=&platforms[0];
  CHECK(CurrentMsi==first && Plan.table==&bs && Original.count==large);
  CHECK(Current.count==7 && Original.edges[0].usb_baseline);
  /* Only the scratch graph may be reused; the original baseline stays intact. */
  EDGE *scratch=Current.edges;Current.count=0;
  for(size_t i=0;i<7;i++)CHECK(GraphAppend(&bs,&Current,edge)==EFI_SUCCESS);
  CHECK(Current.edges==scratch && Original.count==large && Original.edges[0].usb_baseline);
  current=&platforms[1];
  CHECK(CurrentMsi==second && Original.count==2 && Current.count==3);
  for(size_t i=2;i<count;i++) {
    current=&platforms[i];
    CHECK(!GuMsiPlanOwnerToken() && !GuMsiMutated());
    CHECK(GuMsiReserveSelectedState(&bs)==EFI_SUCCESS);
    CHECK(GuMsiPlanOwnerToken()==Gu.release_state && !Plan.table);
    for(size_t j=0;j<i;j++)CHECK(Gu.release_state!=platforms[j].release_state);
    for(size_t j=0;j<i+10;j++)CHECK(GraphAppend(&bs,&Original,edge)==EFI_SUCCESS);
  }
  current=&platforms[0];
  CHECK(GuMsiMutated() && Original.count==large && Original.edges[0].usb_baseline);
  {
    GU_PLATFORM *saved=current;
    const VOID *token=saved->release_state;
    saved->release_state->plan.table=&bs;
    saved->release_state->plan.pci=saved->handle;
    saved->release_state->plan.self=saved->image;
    saved->release_state->plan.ready=1;
    saved->release_state->plan.stage=3;
    saved->release_state->plan.master_normalized=1;
    saved->release_state->plan.hardware_verified=1;
    current=&platforms[1];
    CHECK(GuMsiCompletedPlatform(saved,token));
    CHECK(current==&platforms[1]);
    saved->release_state->plan.hardware_verified=0;
    CHECK(!GuMsiCompletedPlatform(saved,token));
    saved->release_state->plan.hardware_verified=1;
    saved->release_state->plan.claimed=1;
    CHECK(!GuMsiCompletedPlatform(saved,token));
    saved->release_state->plan.claimed=0;
    CHECK(!GuMsiCompletedPlatform(saved,current->release_state));
    saved->release_state->owner_handle=current->handle;
    CHECK(!GuMsiCompletedPlatform(saved,token));
    saved->release_state->owner_handle=saved->handle;
    CHECK(GuMsiCompletedPlatform(saved,token));
    current=saved;
  }
  current=&platforms[1];
  CHECK(!GuMsiMutated() && Original.count==2);
  for(size_t i=2;i<count;i++) {
    current=&platforms[i];
    CHECK(!GuMsiMutated() && Original.count==i+10);
  }
  /* A substitution must not expose another card's protocol graph. */
  current=&platforms[1];
  Gu.release_state=first;
  CHECK(!GuMsiPlanOwnerToken() && GuMsiMutated());
  CHECK(Gu.release_owner_invalid && Gu.fault && Gu.retained);
  CHECK(first->original.count==large && first->plan.mutated==1);
  Gu.release_state=second;
  CHECK(!GuMsiPlanOwnerToken() && GuMsiMutated());
  CHECK(GuMsiReserveSelectedState(&bs)==EFI_NOT_READY);
  /* Removing the pointer after reservation is not an unused context. */
  current=&platforms[2];
  GU_MSI_OWNED_STATE *third=CurrentMsi;
  Gu.release_state=NULL;
  CHECK(GuMsiMutated() && !GuMsiPlanOwnerToken() && Gu.retained);
  CHECK(GuMsiReserveSelectedState(&bs)==EFI_NOT_READY);
  Gu.release_state=third;
  CHECK(!GuMsiPlanOwnerToken());
  /* Also detect a changed handle, PCI protocol, boot-services or image. */
  current=&platforms[3];Gu.handle=&platforms[0];
  CHECK(!GuMsiPlanOwnerToken() && GuMsiMutated() && Gu.retained);
  current=&platforms[4];Gu.pci=&pci[0];
  CHECK(!GuMsiPlanOwnerToken() && GuMsiMutated() && Gu.retained);
  current=&platforms[5];Gu.image=&pci[0];
  CHECK(!GuMsiPlanOwnerToken() && GuMsiMutated() && Gu.retained);
  current=&platforms[6];Gu.bs=&other_bs;
  CHECK(GuMsiReserveSelectedState(&bs)==EFI_NOT_READY);
  CHECK(Gu.release_owner_invalid && Gu.retained && !GuMsiPlanOwnerToken());
  current=&platforms[7];
  GU_MSI_OWNED_STATE *eighth=CurrentMsi;
  Gu.release_state_reserved=0;
  CHECK(GuMsiReserveSelectedState(&bs)==EFI_NOT_READY);
  CHECK(GuMsiMutated() && !GuMsiPlanOwnerToken() && Gu.retained && Gu.release_state==eighth);
  current=&platforms[0];
  CHECK(GuMsiPlanOwnerToken()==first && GuMsiMutated() && !Gu.fault);
  /* Exercise the explicit resource limit without changing the original baseline. */
  while(Current.count<GRAPH_EDGE_LIMIT)CHECK(GraphAppend(&bs,&Current,edge)==EFI_SUCCESS);
  CHECK(GraphAppend(&bs,&Current,edge)==EFI_BAD_BUFFER_SIZE);
  CHECK(Current.count==GRAPH_EDGE_LIMIT && Original.count==large && Original.edges[0].usb_baseline);
  for(size_t i=0;i<count;i++) {
    free(platforms[i].release_state->original.edges);
    free(platforms[i].release_state->current.edges);
    free(platforms[i].release_state);
  }
  free(platforms);
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "release_context.c"
            source.write_text(program, encoding="utf-8")
            for compiler in ("gcc", "clang"):
                with self.subTest(compiler=compiler):
                    binary = Path(directory) / compiler
                    built = subprocess.run(
                        [compiler, "-std=c11", "-O2", "-Wall", "-Wextra",
                         "-Werror", "-pedantic", str(source), "-o", str(binary)],
                        capture_output=True, text=True)
                    self.assertEqual(built.returncode, 0, built.stderr)
                    checked = subprocess.run([str(binary)], capture_output=True, text=True)
                    self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_missing_release_context_refuses_before_access(self):
        # The guard must precede the first dereference in all public users.
        for name in ("GuMsiPreflight", "GuMsiRelease", "GuMsiReleased",
                     "GuMsiFinish", "GuMsiReadyForWindows"):
            with self.subTest(name=name):
                code = function_body(name).lstrip()
                expected = "FALSE" if name in ("GuMsiReleased", "GuMsiReadyForWindows") else "EFI_NOT_READY"
                self.assertTrue(code.startswith("if(!ReleaseOwnerMatches())return " + expected + ";"))
        self.assertNotIn("LegacyMsi", SOURCE)
        shared = (Path(__file__).resolve().parents[1] / "src/unlock_execution.c").read_text(encoding="utf-8")
        wrapper = shared.split("EFI_STATUS GuExecuteUnlock(", 1)[1]
        self.assertLess(wrapper.index("GuMsiReserveSelectedState("),
                        wrapper.index("#ifdef GU_ACTIVE_SINGLE_TARGET"))

    def test_full_non_target_protocol_ownership_is_preserved(self):
        self.assertIn("GuIsPreparedPeerHandle(h)",function_body("ProtectedHandle"))

if __name__ == "__main__":
    unittest.main()
