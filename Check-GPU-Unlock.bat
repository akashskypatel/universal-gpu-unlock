@echo off
rem = r'''
setlocal
title Universal GPU Unlock - Baseline Measurement (NOT an Unlock Verifier)
color 0F
set "PYTHON_EXE="
if defined CONDA_PREFIX if exist "%CONDA_PREFIX%\python.exe" set "PYTHON_EXE=%CONDA_PREFIX%\python.exe"
if not defined PYTHON_EXE (
    where python.exe >nul 2>&1
    if not errorlevel 1 set "PYTHON_EXE=python.exe"
)
if not defined PYTHON_EXE (
    where py.exe >nul 2>&1
    if not errorlevel 1 set "PYTHON_EXE=py.exe"
)
if not defined PYTHON_EXE (
    echo STATUS=ERROR REASON=PYTHON_UNAVAILABLE
    if not "%~1"=="--no-pause" pause
    exit /b 1
)
"%PYTHON_EXE%" -x "%~f0"
set "BENCH_ERR=%ERRORLEVEL%"
if not "%~1"=="--no-pause" pause
exit /b %BENCH_ERR%
'''
# This is an observational GEMM sample, not an unlock classification.
# Keep CUDA ordinals distinct from PCI BDFs; no inference from performance alone.
import sys

try:
    import torch
except ImportError:
    print("STATUS=ERROR REASON=PYTORCH_UNAVAILABLE")
    sys.exit(1)

if not torch.cuda.is_available():
    print("STATUS=ERROR REASON=CUDA_UNAVAILABLE")
    sys.exit(1)

print("GPU GEMM SAMPLE — NOT A FIRMWARE UNLOCK VERIFIER")
print("CUDA ordinal does not establish a physical GPU PCI BDF or UUID mapping.")
print("This test cannot confirm tensor issue-rate selectors or firmware acceptance.")
samples = 0

for ordinal in range(torch.cuda.device_count()):
    device = torch.device(f"cuda:{ordinal}")
    props = torch.cuda.get_device_properties(ordinal)
    size, warmups, trials = 4096, 15, 30
    try:
        a = torch.randn((size, size), dtype=torch.float16, device=device)
        b = torch.randn((size, size), dtype=torch.float16, device=device)
        for _ in range(warmups):
            c = torch.matmul(a, b)
        torch.cuda.synchronize(device)
        begin = torch.cuda.Event(enable_timing=True)
        end = torch.cuda.Event(enable_timing=True)
        begin.record()
        for _ in range(trials):
            c = torch.matmul(a, b)
        end.record()
        torch.cuda.synchronize(device)
        avg_ms = begin.elapsed_time(end) / trials
        if avg_ms <= 0:
            raise ValueError("nonpositive CUDA event timing")
        tflops = (2 * size**3 / (avg_ms * 1e-3)) / 1e12
        print(f"CUDA_ORDINAL={ordinal} GPU={props.name} SM={props.major}{props.minor} "
              f"FP16_GEMM_TFLOPS={tflops:.3f} AVG_MS={avg_ms:.3f}")
        samples += 1
        del a, b, c
    except Exception as exc:
        print(f"STATUS=ERROR CUDA_ORDINAL={ordinal} REASON={type(exc).__name__}: {exc}")
        sys.exit(1)

print(f"SAMPLES={samples}")
print("UNLOCK_VERIFIED=UNKNOWN")
print("Required: same-card stock/post-change raw MMA issue-rate, numerical controls, "
      "driver health and authenticated firmware/selector provenance.")
sys.exit(2)
