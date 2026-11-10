"""Execute the original vendor Falcon halt reader with a mocked MMIO bus.

The test compiles the implementation from unlock_610.c, not a duplicate
predicate. No firmware or GPU device is accessed.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE = (Path(__file__).resolve().parents[1] / "src/vendor/unlock_610.c").read_text(encoding="utf-8")


def function_source(name):
    import re
    match = re.search(
        r"\bstatic\s+(?:bool|int)\s+" + name + r"\s*\([^;]*?\)\s*\{",
        SOURCE, re.DOTALL,
    )
    if match is None:
        raise AssertionError("missing real vendor implementation: " + name)
    depth, at, start = 1, match.end(), match.start()
    while at < len(SOURCE) and depth:
        depth += (SOURCE[at] == "{") - (SOURCE[at] == "}")
        at += 1
    if depth:
        raise AssertionError("unbalanced source function: " + name)
    return SOURCE[start:at]


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

typedef uint32_t u32;
typedef long long ktime_t;
struct falcon_eng { u32 base; u32 falcon2; };
struct gpu_dev { int dev; };
struct unlock_transaction_work { struct gpu_dev *pdev; };
static struct { bool fault; } Gu;
static const struct falcon_eng gu_gsp={0x1000u,0x2000u};
static const struct falcon_eng gu_sec2={0x3000u,0x4000u};
static const struct falcon_eng *active;
static u32 cpu_value, riscv_value;
static bool cpu_readable, riscv_readable;
static int riscv_reads;
static ktime_t now;
#define GU_RESULT(x) (x)
#define dev_info(dev, fmt, ...) ((void)0)
#define dev_err(dev, fmt, ...) ((void)0)

static int read_bar(struct unlock_transaction_work *work,u32 address,u32 *value)
{
    (void)work;
    if (Gu.fault) return -EIO;
    if (address==active->base+0x100) {
        if (!cpu_readable) return -EIO;
        *value=cpu_value; return 0;
    }
    if (address==active->falcon2+0x388) {
        riscv_reads++;
        if (!riscv_readable) return -EIO;
        *value=riscv_value; return 0;
    }
    return -EIO;
}
static ktime_t ktime_get(void){return now;}
static ktime_t ktime_add_us(ktime_t a,unsigned n){return a+n;}
static int ktime_compare(ktime_t a,ktime_t b){return (a>b)-(a<b);}
static void usleep_range(unsigned a,unsigned b){(void)b;now+=a;}

__READERS__

static void setup(const struct falcon_eng *engine,u32 cpu,u32 riscv,
                  bool cpu_ok,bool riscv_ok)
{
    active=engine;cpu_value=cpu;riscv_value=riscv;
    cpu_readable=cpu_ok;riscv_readable=riscv_ok;
    now=0;riscv_reads=0;Gu.fault=false;
}
int main(void)
{
    struct gpu_dev device={0};
    struct unlock_transaction_work work={&device};
    setup(&gu_sec2,0,0x10u,true,true);
    assert(poll_falcon_engine_halt(&work,&gu_sec2,300,true)==0);
    assert(is_falcon_engine_halted(&work,&gu_sec2));
    assert(riscv_reads>0);

    setup(&gu_sec2,0x10u,0,true,true);
    assert(poll_falcon_engine_halt(&work,&gu_sec2,300,true)==0);
    assert(is_falcon_engine_halted(&work,&gu_sec2));

    setup(&gu_gsp,0,0x10u,true,true);
    assert(poll_falcon_engine_halt(&work,&gu_gsp,300,true)==-ETIMEDOUT);
    assert(!is_falcon_engine_halted(&work,&gu_gsp));
    assert(riscv_reads==0);

    setup(&gu_gsp,0x10u,0,true,true);
    assert(poll_falcon_engine_halt(&work,&gu_gsp,300,true)==0);
    assert(is_falcon_engine_halted(&work,&gu_gsp));
    assert(riscv_reads==0);

    setup(&gu_sec2,~0u,~0u,true,true);
    assert(poll_falcon_engine_halt(&work,&gu_sec2,300,true)==-ETIMEDOUT);
    assert(!is_falcon_engine_halted(&work,&gu_sec2));

    setup(&gu_sec2,0,0x10u,false,false);
    assert(poll_falcon_engine_halt(&work,&gu_sec2,300,true)==-ETIMEDOUT);
    assert(!is_falcon_engine_halted(&work,&gu_sec2));

    setup(&gu_sec2,0x10u,0,true,true);
    Gu.fault=true;
    assert(poll_falcon_engine_halt(&work,&gu_sec2,300,true)==-ETIMEDOUT);
    assert(!is_falcon_engine_halted(&work,&gu_sec2));

    setup(&gu_sec2,0,0,true,true);
    assert(poll_falcon_engine_halt(&work,&gu_sec2,300,false)==-ETIMEDOUT);
    puts("STATUS=COMPLETE PASS=8 HALT_ENGINE_TESTS HARDWARE_ACTION=NONE");
    return 0;
}
"""


class HaltReaderTests(unittest.TestCase):
    def test_real_engine_reader_with_mocked_registers(self):
        code = HARNESS.replace(
            "__READERS__",
            "\n\n".join(
                function_source(name) for name in
                ("inaccessible", "poll_falcon_engine_halt", "is_falcon_engine_halted")
            ),
        )
        compiler = os.environ.get("CC", "gcc")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "halt_reader.c"
            binary = Path(directory) / "halt_reader"
            source.write_text(code, encoding="utf-8")
            compile_run = subprocess.run(
                [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                 str(source), "-o", str(binary)],
                text=True, capture_output=True, check=False, timeout=30,
            )
            self.assertEqual(compile_run.returncode, 0, compile_run.stderr)
            execute_run = subprocess.run(
                [str(binary)], text=True, capture_output=True,
                check=False, timeout=10,
            )
            self.assertEqual(execute_run.returncode, 0, execute_run.stderr)
            self.assertIn("STATUS=COMPLETE PASS=8", execute_run.stdout)


if __name__ == "__main__":
    unittest.main()
