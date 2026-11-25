#!/usr/bin/env python3
"""Legacy single-device GEMM and memory-bandwidth benchmark.

LEGACY_SINGLE_DEVICE_SCRIPT=1

This is the original fixed-device benchmark kept for historical comparison.
It measures exactly one explicitly selected CUDA ordinal and prints that
ordinal plus the device identity, so a result cannot be silently attributed to
a different physical card. A CUDA ordinal is not a PCI BDF, and measured
throughput is not evidence that any firmware state changed.

For per-device surveys use tools/quick_check.py or
tools/benchmark_all_gpus.py. Both report one result per CUDA device.

SPDX-License-Identifier: GPL-2.0-only
"""
from __future__ import annotations

import argparse
import sys

DEFAULT_DEVICE = 0
ATTRIBUTION = "MEASURED_NOT_VERIFIED"


def parse_index(spec: str) -> int:
    """Parse an explicit non-negative CUDA ordinal. No implicit default."""
    try:
        index = int(spec)
    except (TypeError, ValueError) as exc:
        raise ValueError("device must be an explicit CUDA index") from exc
    if index < 0:
        raise ValueError("device must be a non-negative CUDA index")
    return index


def selected_device(spec: str, count: int) -> int:
    """Resolve an explicit CUDA ordinal against the observed device count."""
    if count < 1:
        raise ValueError("no CUDA devices found")
    index = parse_index(spec)
    if index >= count:
        raise ValueError(f"device {index} is outside CUDA indices 0..{count - 1}")
    return index


def device_identity(index: int, props) -> str:
    """Identity line for result attribution; unknown fields stay explicit."""
    name = getattr(props, "name", None) or "UNKNOWN"
    bus_id = getattr(props, "pci_bus_id", None) or "UNKNOWN"
    return f"CUDA_INDEX={index} NAME={name} PCI_BUS_ID={bus_id} ATTRIBUTION={ATTRIBUTION}"


def run_benchmark(device_index: int) -> int:
    import torch
    from quick_check import check_numerical_accuracy

    if not torch.cuda.is_available():
        print("ERROR: CUDA is not available!")
        return 1
    count = torch.cuda.device_count()
    try:
        device_index = selected_device(str(device_index), count)
    except ValueError as exc:
        print(f"ERROR: {exc}")
        return 1

    device = torch.device(f"cuda:{device_index}")
    props = torch.cuda.get_device_properties(device_index)
    print("=" * 70)
    print("      LEGACY SINGLE-DEVICE GEMM BANDWIDTH BENCHMARK (ORDINAL 1-of-N)")
    print("=" * 70)
    print("LEGACY_SINGLE_DEVICE=1")
    print(f"DEFAULT_CUDA_INDEX={DEFAULT_DEVICE} SELECTED_CUDA_INDEX={device_index}")
    if device_index == DEFAULT_DEVICE:
        print("LEGACY_DEFAULT_DEVICE_USED=1 USE --device TO NAME THE MEASURED CARD")
    print(device_identity(device_index, props))
    print(f"Compute Capability : {props.major}.{props.minor}")
    print(f"Total VRAM         : {props.total_memory / (1024**3):.2f} GB")
    print(f"Multi-Processors   : {props.multi_processor_count} SMs")
    print(f"PyTorch Version    : {torch.__version__}")
    print(f"CUDA Version       : {torch.version.cuda}")
    print("=" * 70)

    # Matrix sizes for GEMM (General Matrix Multiply)
    sizes = [4096, 8192, 16384]

    dtypes = [
        ("FP16 Tensor Cores", torch.float16),
        ("BF16 Tensor Cores", torch.bfloat16),
        ("FP32 CUDA / TF32", torch.float32),
    ]

    print("\n--- 1. TENSOR CORE & CUDA COMPUTE THROUGHPUT (GEMM) ---")
    print(f"{'Precision':<20} | {'Matrix Size':<12} | {'Avg Latency (ms)':<16} | {'Throughput (TFLOPS)':<20}")
    print("-" * 75)

    failures = 0
    for dtype_name, dtype in dtypes:
        for N in sizes:
            # FLOPs for C = A @ B (where A is N x N, B is N x N) is 2 * N^3
            flops = 2.0 * (N ** 3)

            try:
                a = torch.randn(N, N, device=device, dtype=dtype)
                b = torch.randn(N, N, device=device, dtype=dtype)

                # Warmup iterations
                for _ in range(5):
                    c = torch.matmul(a, b)
                torch.cuda.synchronize(device)

                # Timed iterations
                num_iters = 10 if N <= 8192 else 5
                start_event = torch.cuda.Event(enable_timing=True)
                end_event = torch.cuda.Event(enable_timing=True)

                start_event.record(torch.cuda.current_stream(device))
                for _ in range(num_iters):
                    c = torch.matmul(a, b)
                end_event.record(torch.cuda.current_stream(device))
                torch.cuda.synchronize(device)

                elapsed_ms = start_event.elapsed_time(end_event) / num_iters
                tflops = (flops / (elapsed_ms / 1000.0)) / 1e12

                print(f"{dtype_name:<20} | {f'{N}x{N}':<12} | {elapsed_ms:>13.3f} ms | {tflops:>17.2f} TFLOPS")

                del a, b, c
                torch.cuda.empty_cache()
            except Exception as e:
                failures += 1
                print(f"{dtype_name:<20} | {f'{N}x{N}':<12} | ERROR: {e}")

    print("\n--- 2. VRAM MEMORY BANDWIDTH TEST ---")
    # Allocate 2 GB tensors and measure bandwidth
    num_elements = 512 * 1024 * 1024  # 512M elements of FP32 = 2 GB each
    try:
        x = torch.empty(num_elements, device=device, dtype=torch.float32)
        y = torch.empty(num_elements, device=device, dtype=torch.float32)

        # Warmup
        y.copy_(x)
        torch.cuda.synchronize(device)

        start_event = torch.cuda.Event(enable_timing=True)
        end_event = torch.cuda.Event(enable_timing=True)

        num_bw_iters = 10
        start_event.record(torch.cuda.current_stream(device))
        for _ in range(num_bw_iters):
            y.copy_(x)
        end_event.record(torch.cuda.current_stream(device))
        torch.cuda.synchronize(device)

        bw_ms = start_event.elapsed_time(end_event) / num_bw_iters
        # 2 GB read + 2 GB write = 4 GB transferred per iteration
        bytes_transferred = 2 * (num_elements * 4)
        gbps = (bytes_transferred / (bw_ms / 1000.0)) / (1024**3)

        print(f"Copy Bandwidth (2 GB -> 2 GB): {bw_ms:.3f} ms -> {gbps:.2f} GB/s")
        del x, y
        torch.cuda.empty_cache()
    except Exception as e:
        failures += 1
        print(f"VRAM Bandwidth ERROR: {e}")

    print("\n--- 3. NUMERICAL ACCURACY ---")
    failures += check_numerical_accuracy(torch, device, device_index)

    print("=" * 70)
    print(f"RESULT ATTRIBUTION: CUDA_INDEX={device_index} {ATTRIBUTION}")
    print("LEGACY_SINGLE_DEVICE_USED=1 THROUGHPUT_IS_NOT_UNLOCK_EVIDENCE=1")
    print(f"FAILURES={failures}")
    print("=" * 70)
    return 1 if failures else 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Legacy single-device GEMM benchmark; requires an explicit CUDA ordinal."
    )
    parser.add_argument(
        "--device",
        default=str(DEFAULT_DEVICE),
        help=f"explicit CUDA ordinal to measure (default: {DEFAULT_DEVICE}, reported as legacy)",
    )
    args = parser.parse_args(argv)
    try:
        index = parse_index(args.device)
    except ValueError as exc:
        print(f"ERROR: {exc}")
        return 2
    return run_benchmark(index)


if __name__ == "__main__":
    sys.exit(main())
