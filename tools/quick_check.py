#!/usr/bin/env python3
"""Read-only Tensor Core performance survey for each selected NVIDIA GPU.

Reports measured throughput and a separate fixed-input numerical screen, not
firmware acceptance or proof of an unlock.
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


# Fixed bounds for a numerical screen, not a proof of every CUDA operation.
# L2 is relative to the CPU reference norm; peak error is scaled by its RMS.
ACCURACY_LIMITS = {
    "FP16": (1e-3, 8e-3),
    "BF16": (5e-3, 3e-2),
    "TF32_ENABLED": (2e-3, 1.2e-2),
    "FP32_CONTROL": (2e-5, 2e-4),
}
ACCURACY_CASES = ((512, 1729, True), (1024, 1730, False), (1024, 1731, False))
ACCURACY_REPEATS = 3


def accuracy_metrics(torch, actual, reference):
    """Compare CPU tensors in float64; reject nonfinite or malformed output."""
    if actual.shape != reference.shape or not actual.numel():
        raise ValueError("invalid numerical result shape")
    actual = actual.to(dtype=torch.float64, device="cpu")
    reference = reference.to(dtype=torch.float64, device="cpu")
    if not bool(torch.isfinite(actual).all()) or not bool(torch.isfinite(reference).all()):
        return float("inf"), float("inf")
    norm = float(torch.linalg.vector_norm(reference))
    if not math.isfinite(norm) or norm <= 0:
        raise ValueError("invalid numerical reference norm")
    error = actual - reference
    return (float(torch.linalg.vector_norm(error)) / norm,
            float(error.abs().max()) / (norm / math.sqrt(reference.numel())))


def accuracy_passes(mode, exact, relative_l2, peak_rms):
    if not math.isfinite(relative_l2) or not math.isfinite(peak_rms):
        return False
    if exact:
        return relative_l2 == 0 and peak_rms == 0
    l2_limit, peak_limit = ACCURACY_LIMITS[mode]
    return 0 <= relative_l2 <= l2_limit and 0 <= peak_rms <= peak_limit


def accuracy_inputs(torch, dtype, size, seed, exact):
    generator = torch.Generator(device="cpu").manual_seed(seed)
    if exact:
        # Dot products of 512 values in {-1,+1} are even integers in
        # [-512,512], exactly representable even in BF16.
        a = torch.randint(0, 2, (size, size), generator=generator) * 2 - 1
        b = torch.randint(0, 2, (size, size), generator=generator) * 2 - 1
    else:
        a = torch.randn((size, size), dtype=torch.float64, generator=generator)
        b = torch.randn((size, size), dtype=torch.float64, generator=generator)
    a, b = a.to(dtype=dtype), b.to(dtype=dtype)
    # Reference the actual rounded operands, not the original random values.
    reference = a.to(torch.float64) @ b.to(torch.float64)
    return a, b, reference


def check_numerical_accuracy(torch, device, index):
    """Check every result against an independent CPU reference outside timing."""
    failures = 0
    completed = 0
    with torch.cuda.device(device):
        previous_tf32 = torch.backends.cuda.matmul.allow_tf32
        try:
            modes = [("FP16", torch.float16, False),
                     ("BF16", torch.bfloat16, False),
                     ("TF32_ENABLED", torch.float32, True),
                     ("FP32_CONTROL", torch.float32, False)]
            for mode, dtype, tf32 in modes:
                if mode == "BF16" and not torch.cuda.is_bf16_supported(including_emulation=False):
                    print(f"ACCURACY DEVICE={index} MODE={mode} STATUS=UNSUPPORTED")
                    continue
                torch.backends.cuda.matmul.allow_tf32 = tf32
                for size, seed, exact in ACCURACY_CASES:
                    try:
                        a, b, reference = accuracy_inputs(torch, dtype, size, seed, exact)
                        a, b = a.to(device), b.to(device)
                        worst_l2 = worst_peak = repeat_peak = 0.0
                        first = None
                        identical = True
                        passed = True
                        for _ in range(ACCURACY_REPEATS):
                            actual = (a @ b).to(device="cpu", dtype=torch.float64)
                            torch.cuda.synchronize(device)
                            l2, peak = accuracy_metrics(torch, actual, reference)
                            worst_l2, worst_peak = max(worst_l2, l2), max(worst_peak, peak)
                            passed &= accuracy_passes(mode, exact, l2, peak)
                            if first is None:
                                first = actual
                            else:
                                identical &= bool(torch.equal(first, actual))
                                _, delta = accuracy_metrics(torch, actual, first)
                                repeat_peak = max(repeat_peak, delta)
                        l2_limit, peak_limit = (0.0, 0.0) if exact else ACCURACY_LIMITS[mode]
                        print(f"ACCURACY DEVICE={index} MODE={mode} SIZE={size} SEED={seed} "
                              f"CASE={'EXACT' if exact else 'RANDOM'} REPEATS={ACCURACY_REPEATS} "
                              f"REL_L2={worst_l2:.8g} PEAK_RMS={worst_peak:.8g} "
                              f"L2_LIMIT={l2_limit:.8g} PEAK_LIMIT={peak_limit:.8g} "
                              f"REPEAT_IDENTICAL={int(identical)} REPEAT_PEAK_RMS={repeat_peak:.8g} "
                              f"STATUS={'PASS' if passed else 'FAIL'}")
                        completed += 1
                        failures += int(not passed)
                    except (ValueError, RuntimeError, AssertionError) as exc:
                        failures += 1
                        print(f"ACCURACY DEVICE={index} MODE={mode} SIZE={size} SEED={seed} STATUS=ERROR REASON={exc}")
        finally:
            torch.backends.cuda.matmul.allow_tf32 = previous_tf32
    print(f"ACCURACY DEVICE={index} COMPLETED={completed} FAILURES={failures} "
          f"RESULT={'PASS' if completed and not failures else 'FAIL'} REFERENCE=CPU_FP64 UNLOCK_VERIFIED=false")
    return failures if completed or failures else 1


def benchmark_device(torch, index: int, size: int, warmups: int, runs: int) -> int:
    device = torch.device(f"cuda:{index}")
    with torch.cuda.device(index):
        name = torch.cuda.get_device_name(index)
        props = torch.cuda.get_device_properties(index)
        print(f"DEVICE={index} NAME={name} ARCH=sm_{props.major}{props.minor} "
              f"PCI_BUS_ID={getattr(props, 'pci_bus_id', None) or 'UNKNOWN'} "
              f"UUID={getattr(props, 'uuid', None) or 'UNKNOWN'}")
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
        failures = check_numerical_accuracy(torch, device, index)
        print(f"DEVICE={index} STATUS=MEASURED_ONLY UNLOCK_VERIFIED=false")
        return failures


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
        failures = 0
        for index in indices:
            failures += benchmark_device(torch, index, args.size, args.warmups, args.runs)
    except (ValueError, RuntimeError, AssertionError) as exc:
        print(f"STATUS=ERROR REASON={type(exc).__name__}: {exc}", file=sys.stderr)
        return 1
    print(f"STATUS=COMPLETE RESULT=MEASURED_NOT_VERIFIED NUMERICAL_FAILURES={failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
