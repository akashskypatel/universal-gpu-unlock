"""Execute the production Turing cold-register predicate and read path."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
SOURCE=(ROOT/"src/profile.c").read_text(encoding="utf-8")
start=SOURCE.index("static BOOLEAN TuringColdState(")
stop=SOURCE.index("BOOLEAN GuAd104ColdEntry(",start)
PRODUCTION=SOURCE[start:stop]

class TuringColdReadOnly(unittest.TestCase):
    def test_turing_cold_state_and_no_unmapped_selector_access(self):
        self.assertIn("if(Gu.is_turing)",PRODUCTION)
        self.assertIn("TuringColdState(pmc,wpr2_hi)",PRODUCTION)
        self.assertNotIn("GuWrite32(",PRODUCTION)
        self.assertNotIn("GuFlr(",PRODUCTION)
        src=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
typedef uint32_t UINT32;
typedef int BOOLEAN;
#define TRUE 1
#define FALSE 0
static struct {BOOLEAN fault,is_turing;} Gu;
static UINT32 pmc_value, wpr_value, ss0_value, ss1_value;
static unsigned selector_reads, all_reads;
static UINT32 GuRead32(UINT32 off) {
  all_reads++;
  if(off==0)return pmc_value;
  if(off==0x1fa828)return wpr_value;
  if(off==0x82381c){selector_reads++;return ss0_value;}
  if(off==0x823820){selector_reads++;return ss1_value;}
  Gu.fault=1;return 0xffffffffU;
}
static void GuLog(const char *format,...) {(void)format;}
''' + PRODUCTION + r'''
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
static void reset(void) {
  Gu.fault=0;Gu.is_turing=1;
  pmc_value=0x16000000U;wpr_value=0;
  ss0_value=0xffffffffU;ss1_value=0xffffffffU;
  selector_reads=all_reads=0;
}
int main(void) {
  UINT32 a=0xfeedU,b=0xbeefU;
  reset();CHECK(Registers(&a,&b) && a==0 && b==0);
  CHECK(selector_reads==0 && all_reads==2);
  reset();pmc_value=0x17000000U;
  CHECK(!Registers(&a,&b) && selector_reads==0);
  reset();wpr_value=0x10U;CHECK(!Registers(&a,&b) && selector_reads==0);
  reset();pmc_value=0xffffffffU;CHECK(!Registers(&a,&b) && selector_reads==0);
  reset();pmc_value=0xbadf0000U;CHECK(!Registers(&a,&b) && selector_reads==0);
  reset();Gu.fault=1;CHECK(!Registers(&a,&b) && selector_reads==0);
  reset();Gu.is_turing=0;ss0_value=0x12345678U;ss1_value=3;
  CHECK(Registers(&a,&b) && a==ss0_value && b==ss1_value);
  CHECK(selector_reads==2);
  reset();Gu.is_turing=0;ss0_value=0x88888888U;ss1_value=3;
  CHECK(!Registers(&a,&b));
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as td:
            source=Path(td)/"turing_cold.c"
            binary=Path(td)/"turing_cold"
            source.write_text(src,encoding="utf-8")
            built=subprocess.run(
                [os.environ.get("CC","gcc"),"-std=c11","-O2","-Wall",
                 "-Wextra","-Werror","-pedantic",str(source),"-o",str(binary)],
                 text=True,capture_output=True)
            self.assertEqual(built.returncode,0,built.stderr)
            result=subprocess.run([str(binary)],text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)

    def test_active_turing_preparation_is_not_unlocked_by_passive_probe(self):
        body=SOURCE.split("EFI_STATUS GuAd104Prepare(VOID) {",1)[1].split(
            "  if(Gu.started ||",1)[0]
        self.assertIn("if(Gu.is_turing)",body)
        self.assertIn("return EFI_UNSUPPORTED;",body)

if __name__=="__main__":
    unittest.main()
