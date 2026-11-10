#!/usr/bin/env python3
"""Read-only Tensor Core performance survey for each selected NVIDIA GPU.

Reports measured throughput, not firmware acceptance or proof of an unlock.
FP16 and BF16 use Tensor Core eligible matrix multiplies; FP32 with TF32
disabled is a control. Results depend on clocks, workload and driver state.
SPDX-License-Identifier: GPL-2.0-only
"""
from __future__ import annotations

import argparse
import math
import sys


def tflops(size: int, average_ms: float) -> float:
    if size < 1 or not math.isfinite(average_ms) or average_ms <= 0:
        raise ValueError("matrix dimension and timing must be positive")
    return (2 * size**3) / (average_ms * 1e-3) / 1e12


def selected_devices(spec: str, count: int) -> list[int]:
    if count < 1:
        raise ValueError("no CUDA devices found")
    if spec == "all":
        return list(range(count))
    try:
        selected = int(spec)
    except ValueError as exc:
        raise ValueError("device must be a CUDA index or 'all'") from exc
    if not 0 <= selected < count:
        raise ValueError(f"device {selected} is outside CUDA indices 0..{count-1}")
    return [selected]


def benchmark_gemm(torch, device, dtype, size: int, warmups: int, runs: int) -> tuple[float, float]:
    a = torch.randn((size, size), device=device, dtype=dtype)
    b = torch.randn((size, size), device=device, dtype=dtype)
    try:
        for _ in range(warmups):
            c = torch.matmul(a, b)
        torch.cuda.synchronize(device)
        start = torch.cuda.Event(enable_timing=True)
        end = torch.cuda.Event(enable_timing=True)
        start.record()
        for _ in range(runs):
            c = torch.matmul(a, b)
        end.record()
        torch.cuda.synchronize(device)
        average_ms = start.elapsed_time(end) / runs
        return average_ms, tflops(size, average_ms)
    finally:
        del a, b


def benchmark_device(torch, index: int, size: int, warmups: int, runs: int) -> None:
    device = torch.device(f"cuda:{index}")
    with torch.cuda.device(index):
        name = torch.cuda.get_device_name(index)
        props = torch.cuda.get_device_properties(index)
        print(f"DEVICE={index} NAME={name} ARCH=sm_{props.major}{props.minor}")
        results = []
        previous_tf32 = torch.backends.cuda.matmul.allow_tf32
        try:
            # Do not alter the process-wide TF32 state beyond this device test.
            torch.backends.cuda.matmul.allow_tf32 = False
            results.append(("FP16", benchmark_gemm(torch, device, torch.float16, size, warmups, runs)))
            bf16_ok = (
                hasattr(torch.cuda, "is_bf16_supported")
                and torch.cuda.is_bf16_supported()
            )
            if bf16_ok:
                results.append(("BF16", benchmark_gemm(torch, device, torch.bfloat16, size, warmups, runs)))
            torch.backends.cuda.matmul.allow_tf32 = True
            results.append(("TF32_ENABLED", benchmark_gemm(torch, device, torch.float32, size, warmups, runs)))
            torch.backends.cuda.matmul.allow_tf32 = False
            results.append(("FP32_CONTROL", benchmark_gemm(torch, device, torch.float32, size, warmups, runs)))
        finally:
            torch.backends.cuda.matmul.allow_tf32 = previous_tf32

        for label, (latency_ms, throughput) in results:
            print(f"DEVICE={index} MODE={label} LATENCY_MS={latency_ms:.5f} TFLOPS={throughput:.4f}")
        if not bf16_ok:
            print(f"DEVICE={index} MODE=BF16 STATUS=UNSUPPORTED")
        print(f"DEVICE={index} STATUS=MEASURED_ONLY UNLOCK_VERIFIED=false")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="all", help="CUDA index, or 'all' (default)")
    parser.add_argument("--size", type=int, default=4096)
    parser.add_argument("--warmups", type=int, default=8)
    parser.add_argument("--runs", type=int, default=25)
    args = parser.parse_args(argv)
    if args.size < 256 or args.size > 8192 or args.warmups < 1 or args.warmups > 100 or args.runs < 1 or args.runs > 200:
        parser.error("require 256 <= size <= 8192, 1 <= warmups <= 100, 1 <= runs <= 200")

    try:
        import torch
    except ImportError:
        print("STATUS=ERROR REASON=PyTorch_not_installed", file=sys.stderr)
        return 2
    if not torch.cuda.is_available():
        print("STATUS=ERROR REASON=CUDA_not_available", file=sys.stderr)
        return 2
    try:
        indices = selected_devices(args.device, torch.cuda.device_count())
        for index in indices:
            benchmark_device(torch, index, args.size, args.warmups, args.runs)
    except (ValueError, RuntimeError, AssertionError) as exc:
        print(f"STATUS=ERROR REASON={type(exc).__name__}: {exc}", file=sys.stderr)
        return 1
    print("STATUS=COMPLETE RESULT=MEASURED_NOT_VERIFIED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
