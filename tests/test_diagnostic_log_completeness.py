"""Host evidence for diagnostic-log completeness in the passive topology EFI.

The UEFI entrypoint itself is only compiled, never executed, in CI. These
tests extract the exact Say/SayDropped/SayCompleteness source block and drive
it with a bounded console/log harness. No EFI firmware, GPU or file system is
touched. A rejected formatted line must never be silently omitted.
SPDX-License-Identifier: GPL-2.0-only
"""
from __future__ import annotations

from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE = (Path(__file__).resolve().parents[1] / "src" / "gpu_topology_efi.c").read_text(
    encoding="utf-8"
)

START = "/* Number of formatted lines that could not be published."
END = "/* The supplementary protocol walks share this capture-local fault."

HARNESS = r"""
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned short CHAR16;
typedef void VOID;
typedef struct _EFI_BOOT_SERVICES EFI_BOOT_SERVICES;
typedef struct {
  int (*OutputString)(VOID *self,const CHAR16 *text);
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef struct {
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
} EFI_SYSTEM_TABLE;
static char console[16384];
static size_t console_used;
static int Output(VOID *self,const CHAR16 *text) {
  size_t i=0;
  (void)self;
  while(text[i] && console_used<sizeof(console)-1U) {
    console[console_used++]=(char)text[i++];
  }
  console[console_used]=0;
  return 0;
}
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL ConOut={Output};
static EFI_SYSTEM_TABLE Table={&ConOut};
static EFI_SYSTEM_TABLE *System=&Table;
#ifdef GU_TOPO_SAVE_LOG
static char LogBytes[131072];
static size_t LogUsed;
static unsigned LogOverflow;
#endif
static size_t GuFormatV(char *out,size_t capacity,const char *format,va_list ap) {
  int written=vsnprintf(out,capacity,format,ap);
  if(written<0)return SIZE_MAX;
  return (size_t)written;
}
/* ---- extracted source: no edits, no generated substitutions ---- */
%s
/* ---- end extracted source ---- */
static void ReportGopDisplayState(EFI_BOOT_SERVICES *bs) {(void)bs;}

static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL:%%d: %%s\n",__LINE__,#x);return 1;}}while(0)
static void Reset(void) {
  console_used=0;console[0]=0;
#ifdef GU_TOPO_SAVE_LOG
  LogUsed=0;LogOverflow=0;
#endif
  LogDropped=0;
}
int main(void) {
  char long_line[500];
  ReportGopDisplayState(NULL);
  Reset();
  Say("LINE one\r\n");
  Say("LINE two %%u\r\n",7U);
  CHECK(console_used>0 && strstr(console,"LINE one"));
  CHECK(strstr(console,"LINE two 7"));
  CHECK(!LogDropped);
#ifdef GU_TOPO_SAVE_LOG
  CHECK(LogUsed>0 && strstr(LogBytes,"LINE two 7") && !LogOverflow);
#endif
  /* An oversized line is refused as a whole: no partial content is written. */
  memset(long_line,'x',sizeof(long_line)-1U);long_line[sizeof(long_line)-1U]=0;
  Reset();
  Say("%%s\r\n",long_line);
  CHECK(LogDropped==1);
  CHECK(!strstr(console,"xxxx"));
  CHECK(strstr(console,"DIAGNOSTIC_LINE_DROPPED=1 CONTENT_WITHHELD=1"));
  /* Only the first refusal emits the bounded marker. */
  Say("%%s\r\n",long_line);
  CHECK(LogDropped==2);
  {
    const char *first=strstr(console,"DIAGNOSTIC_LINE_DROPPED=1");
    CHECK(first!=NULL && strstr(first+1,"DIAGNOSTIC_LINE_DROPPED=1")==NULL);
  }
  SayCompleteness();
  CHECK(strstr(console,"DIAGNOSTIC_LOG_COMPLETENESS=INCOMPLETE DROPPED=2"));
  /* A complete capture reports COMPLETE with a zero drop count. */
  Reset();
  Say("ONLY line\r\n");
  SayCompleteness();
  CHECK(strstr(console,"DIAGNOSTIC_LOG_COMPLETENESS=COMPLETE DROPPED=0"));
#ifdef GU_TOPO_SAVE_LOG
  CHECK(strstr(LogBytes,"DIAGNOSTIC_LOG_COMPLETENESS=COMPLETE"));
  /* A full log buffer is refused without a partial copy, and the overflow is
     reported by the same completeness line. */
  Reset();
  LogUsed=sizeof(LogBytes)-1U;
  memset(LogBytes,'L',LogUsed);
  Say("DOES_NOT_FIT_%%u\r\n",1U);
  CHECK(LogOverflow);
  CHECK(LogUsed==sizeof(LogBytes)-1U);
  CHECK(LogBytes[LogUsed]==0 || LogBytes[LogUsed]=='L');
  SayCompleteness();
  CHECK(strstr(console,"OVERFLOW=1"));
#endif
  printf("PASS diagnostic log completeness: %%u checks\n",checks);
  return 0;
}
"""


class DiagnosticLogCompletenessTests(unittest.TestCase):
    def test_dropped_lines_are_reported_never_hidden(self):
        self.assertIn(START, SOURCE)
        self.assertIn(END, SOURCE)
        block = SOURCE[SOURCE.index(START):SOURCE.index(END)]
        self.assertIn("SayDropped();return;", block)
        self.assertIn("DIAGNOSTIC_LINE_DROPPED=1 CONTENT_WITHHELD=1", block)
        self.assertIn("DIAGNOSTIC_LOG_COMPLETENESS=", block)
        self.assertNotIn("SayCompleteness();", block)  # not self-recursive

    def test_complete_source_reports_completeness_before_the_durable_write(self):
        main = SOURCE.split("EFI_STATUS EFIAPI UefiMain(", 1)[1]
        self.assertLess(main.index("SayCompleteness();"),
                        main.index("SAVE_LOG_STATUS="))
        self.assertIn('Say("DIAGNOSTIC FINISHED; GPU hardware untouched.\\r\\n");\n'
                      "  /* Emitted after every other line", main)

    def _run(self, compiler, define_save_log):
        block = SOURCE[SOURCE.index(START):SOURCE.index(END)]
        program = (HARNESS % block)
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / "log_completeness.c"
            c.write_text(program, encoding="utf-8")
            binary = Path(tmp) / compiler
            command = [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                       "-pedantic"]
            if define_save_log:
                command.append("-DGU_TOPO_SAVE_LOG")
            build = subprocess.run(command + [str(c), "-o", str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("PASS diagnostic log completeness", run.stdout)

    def test_extracted_block_with_durable_log(self):
        for compiler in ("gcc", "clang"):
            with self.subTest(compiler=compiler):
                self._run(compiler, True)

    def test_extracted_block_without_durable_log(self):
        for compiler in ("gcc", "clang"):
            with self.subTest(compiler=compiler):
                self._run(compiler, False)


    def test_read_only_experiment_boot_variable_capture(self):
        start = SOURCE.index("static EFI_STATUS ObserveBootVariable(")
        end = SOURCE.index("#endif", start)
        actual = SOURCE[start:end]
        main = SOURCE.split("EFI_STATUS EFIAPI UefiMain(", 1)[1]
        self.assertIn("ReportExperimentBootVariables(table);", main)
        mock = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef uint16_t CHAR16;
typedef uint8_t UINT8;
typedef uint32_t UINT32;
typedef size_t UINTN;
typedef uint64_t EFI_STATUS;
typedef struct { UINT32 a; uint16_t b,c; UINT8 d[8]; } EFI_GUID;
#define EFI_SUCCESS 0
#define EFI_NOT_FOUND 14
#define EFI_BUFFER_TOO_SMALL 5
#define EFI_UNSUPPORTED 3
#define EFI_COMPROMISED_DATA 33
typedef struct {
  EFI_STATUS (*GetVariable)(CHAR16 *,EFI_GUID *,UINT32 *,UINTN *,UINT8 *);
} RT;
typedef struct { RT *RuntimeServices; } EFI_SYSTEM_TABLE;
static EFI_GUID GlobalGuid={0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0,0xe0,0x98,0x03,0x2b,0x8c}};
static char logtext[8192];
static size_t used;
static void Say(const char *fmt,...) {
  va_list ap;va_start(ap,fmt);
  int n=vsnprintf(logtext+used,sizeof(logtext)-used,fmt,ap);
  va_end(ap);
  if(n>0&&(size_t)n<sizeof(logtext)-used)used+=(size_t)n;
}
static int equal(const CHAR16 *name,const char *s) {
  while(*name&&*s&&*name==(unsigned char)*s){name++;s++;}
  return !*name&&!*s;
}
static int mode,calls,wrong;
static EFI_STATUS Get(CHAR16 *name,EFI_GUID *guid,UINT32 *attrs,UINTN *size,UINT8 *data) {
  const UINT8 target[]="0000:01:00.0";
  const UINT8 state[]={0x47,0x55,0x42,0x31};
  const UINT8 order[]={0x34,0x12};
  const UINT8 option[]={1,0,0,0,0,0,0,0};
  const UINT8 *value=NULL;UINTN n=0;UINT32 expected=0;
  calls++;*attrs=7;
  if(equal(name,"GuTargets")){value=target;n=sizeof(target)-1;expected=0x5c1f2b40;}
  else if(equal(name,"GuBootState")){value=state;n=sizeof(state);expected=0x2a7d54c1;}
  else if(equal(name,"DriverOrder")){value=order;n=mode==2?1:sizeof(order);expected=GlobalGuid.a;}
  else if(equal(name,"Driver1234")){value=option;n=sizeof(option);expected=GlobalGuid.a;}
  else {wrong=1;return EFI_UNSUPPORTED;}
  if(guid->a!=expected){wrong=1;return EFI_UNSUPPORTED;}
  if(mode==1||(mode==3&&equal(name,"Driver1234")))return EFI_NOT_FOUND;
  if(mode==4&&equal(name,"GuTargets")){*size=4097;return EFI_BUFFER_TOO_SMALL;}
  if(*size<n){wrong=1;return EFI_BUFFER_TOO_SMALL;}
  memcpy(data,value,n);*size=n;return EFI_SUCCESS;
}
""" + actual + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d mode=%d: %s\n",__LINE__,mode,#x);return 1;}}while(0)
int main(void) {
  RT rt={Get};EFI_SYSTEM_TABLE table={&rt};
  for(mode=0;mode<5;mode++) {
    used=0;logtext[0]=0;calls=wrong=0;
    ReportExperimentBootVariables(&table);
    CHECK(!wrong);
    CHECK(strstr(logtext,"NAME=GuTargets STATUS="));
    CHECK(strstr(logtext,"NAME=GuBootState STATUS="));
    CHECK(strstr(logtext,"NAME=DriverOrder STATUS="));
    CHECK(calls==((mode==1||mode==2)?3:4));
    CHECK((strstr(logtext,"CAPTURE=COMPLETE")!=NULL)==(mode<2));
    CHECK((strstr(logtext,"CAPTURE=INCOMPLETE")!=NULL)==(mode>=2));
    if(mode==0) {
      CHECK(strstr(logtext,"NAME=GuTargets OFFSET=0 HEX=303030303a30313a30302e30"));
      CHECK(strstr(logtext,"NAME=DriverOrder OFFSET=0 HEX=3412"));
      CHECK(strstr(logtext,"NAME=Driver1234 OFFSET=0 HEX=0100000000000000"));
    }
    if(mode==1||mode==2)CHECK(!strstr(logtext,"NAME=Driver1234"));
    if(mode==3)CHECK(!strstr(logtext,"NAME=Driver1234 OFFSET="));
    if(mode==4)CHECK(!strstr(logtext,"NAME=GuTargets OFFSET="));
  }
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "boot_variables.c"
            src.write_text(mock, encoding="utf-8")
            for compiler in ("gcc", "clang"):
                with self.subTest(compiler=compiler):
                    binary = Path(tmp) / compiler
                    built = subprocess.run([compiler, "-std=c11", "-fshort-wchar",
                                            "-O2", "-Wall", "-Wextra", "-Werror",
                                            "-pedantic", str(src), "-o", str(binary)],
                                           capture_output=True, text=True)
                    self.assertEqual(built.returncode, 0, built.stderr)
                    ran = subprocess.run([str(binary)], capture_output=True, text=True)
                    self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == "__main__":
    unittest.main()
