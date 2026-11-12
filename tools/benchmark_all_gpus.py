"""Run the original matrix and memory-bandwidth benchmarks on each discrete CUDA GPU."""

import ctypes
import ctypes.util
from pathlib import Path
import sys

import torch


SIZES = (4096, 8192, 16384)
PRECISIONS = (
    ("FP16 Tensor Cores", torch.float16),
    ("BF16 Tensor Cores", torch.bfloat16),
    ("FP32 (TF32 allowed)", torch.float32),
)


def cuda_runtime():
    """Load the CUDA runtime shipped with PyTorch, if available."""
    torch_lib = Path(torch.__file__).resolve().parent / "lib"
    if sys.platform == "win32":
        candidates = sorted(torch_lib.glob("cudart64_*.dll"))
        candidates += ["cudart64_12.dll", "cudart64_13.dll"]
    else:
        candidates = sorted(torch_lib.glob("libcudart.so*"))
        candidates += ["libcudart.so.12", "libcudart.so.13", "libcudart.so"]

    found = ctypes.util.find_library("cudart")
    if found:
        candidates.append(found)

    errors = []
    for candidate in candidates:
        try:
            return ctypes.CDLL(str(candidate))
        except OSError as exc:
            errors.append(str(exc))
    raise RuntimeError("Cannot load CUDA runtime for integrated-GPU detection")


def is_integrated(runtime, index):
    """Read NVIDIA cudaDevAttrIntegrated (attribute 18) for this CUDA index."""
    function = runtime.cudaDeviceGetAttribute
    function.argtypes = (ctypes.POINTER(ctypes.c_int), ctypes.c_int, ctypes.c_int)
    function.restype = ctypes.c_int
    value = ctypes.c_int()
    status = function(ctypes.byref(value), 18, index)
    if status != 0:
        raise RuntimeError(f"CUDA attribute query failed with code {status}")
    return bool(value.value)


def benchmark_device(index):
    device = torch.device(f"cuda:{index}")
    torch.cuda.set_device(device)
    props = torch.cuda.get_device_properties(device)

    print("=" * 79)
    print(f"GPU {index}: {props.name}")
    print(f"CUDA index         : {index}")
    print(f"Compute capability : {props.major}.{props.minor}")
    print(f"Total GPU memory   : {props.total_memory / (1024**3):.2f} GiB")
    print(f"Multiprocessors    : {props.multi_processor_count}")
    print(f"PyTorch            : {torch.__version__} (CUDA {torch.version.cuda})")
    print("=" * 79)

    failures = 0
    results = {}
    print("\n--- MATRIX COMPUTE THROUGHPUT ---")
    print(f"{'Precision':<22} | {'Matrix':<12} | {'Latency (ms)':>14} | {'TFLOP/s':>12}")
    print("-" * 72)

    # Permit TF32 only for FP32 matmul. The backend chooses whether to use it.
    torch.backends.cuda.matmul.allow_tf32 = True
    for name, dtype in PRECISIONS:
        for n in SIZES:
            a = b = c = None
            try:
                if dtype == torch.bfloat16 and not torch.cuda.is_bf16_supported(including_emulation=False):
                    print(f"{name:<22} | {n}x{n:<6} | SKIP (no native BF16 support)")
                    continue
                a = torch.randn(n, n, device=device, dtype=dtype)
                b = torch.randn(n, n, device=device, dtype=dtype)
                for _ in range(5):
                    c = torch.matmul(a, b)
                torch.cuda.synchronize(device)

                iterations = 10 if n <= 8192 else 5
                start = torch.cuda.Event(enable_timing=True)
                end = torch.cuda.Event(enable_timing=True)
                start.record()
                for _ in range(iterations):
                    c = torch.matmul(a, b)
                end.record()
                torch.cuda.synchronize(device)

                elapsed_ms = start.elapsed_time(end) / iterations
                tflops = 2.0 * n**3 / (elapsed_ms / 1000.0) / 1e12
                results[(name, n)] = tflops
                print(f"{name:<22} | {n}x{n:<6} | {elapsed_ms:>14.3f} | {tflops:>12.2f}")
            except Exception as exc:
                failures += 1
                print(f"{name:<22} | {n}x{n:<6} | ERROR: {exc}")
            finally:
                del a, b, c
                torch.cuda.empty_cache()

    print("\n--- GPU MEMORY COPY BANDWIDTH ---")
    x = y = None
    bandwidth = None
    try:
        free_bytes, _ = torch.cuda.mem_get_info(device)
        reserve_bytes = 512 * 1024**2
        budget_bytes = max(0, free_bytes - reserve_bytes) * 0.75
        elements = min(512 * 1024 * 1024, int(budget_bytes // 8))
        if elements < 16 * 1024 * 1024:
            raise RuntimeError("Insufficient free GPU memory for bandwidth measurement")

        x = torch.empty(elements, device=device, dtype=torch.float32)
        y = torch.empty_like(x)
        x.fill_(1.0)
        y.copy_(x)
        torch.cuda.synchronize(device)

        start = torch.cuda.Event(enable_timing=True)
        end = torch.cuda.Event(enable_timing=True)
        iterations = 10
        start.record()
        for _ in range(iterations):
            y.copy_(x)
        end.record()
        torch.cuda.synchronize(device)

        elapsed_ms = start.elapsed_time(end) / iterations
        bandwidth = (2 * elements * 4) / (elapsed_ms / 1000.0) / 1024**3
        buffer_gib = elements * 4 / 1024**3
        print(f"Copy {buffer_gib:.2f} GiB -> {buffer_gib:.2f} GiB: "
              f"{elapsed_ms:.3f} ms, {bandwidth:.2f} GiB/s")
    except Exception as exc:
        failures += 1
        print(f"ERROR: {exc}")
    finally:
        del x, y
        torch.cuda.empty_cache()

    print(f"Measurements failed on GPU {index}: {failures}\n")
    return {"index": index, "name": props.name, "failures": failures,
            "results": results, "bandwidth": bandwidth}


def main():
    if not torch.cuda.is_available():
        print("ERROR: No CUDA GPU is available.")
        return 1
    if torch.version.hip is not None:
        print("ERROR: This benchmark requires NVIDIA CUDA, not ROCm.")
        return 1

    try:
        runtime = cuda_runtime()
    except Exception as exc:
        print(f"ERROR: {exc}. Cannot safely identify integrated devices.")
        return 1

    selected = []
    detection_failures = 0
    print("CUDA devices (integrated GPUs are excluded):")
    for index in range(torch.cuda.device_count()):
        name = torch.cuda.get_device_name(index)
        try:
            integrated = is_integrated(runtime, index)
        except Exception as exc:
            detection_failures += 1
            print(f"  cuda:{index}: {name} - SKIP (cannot classify: {exc})")
            continue
        if integrated:
            print(f"  cuda:{index}: {name} - SKIP (integrated)")
        else:
            print(f"  cuda:{index}: {name} - selected")
            selected.append(index)

    if not selected:
        print("No verified discrete CUDA GPUs to benchmark.")
        return 1

    reports = []
    for index in selected:
        try:
            reports.append(benchmark_device(index))
        except Exception as exc:
            detection_failures += 1
            print(f"ERROR: GPU {index} could not finish its benchmark: {exc}")

    print("=" * 79)
    print("PER-GPU SUMMARY")
    print(f"{'CUDA':<6} {'GPU':<29} {'FP16 8192':>11} {'BF16 8192':>11} "
          f"{'FP32 8192':>11} {'Copy GiB/s':>13} {'Failed':>7}")
    for report in reports:
        data = report["results"]
        def fmt(value):
            return f"{value:.2f}" if value is not None else "N/A"
        print(f"{report['index']:<6} {report['name'][:29]:<29} "
              f"{fmt(data.get(('FP16 Tensor Cores', 8192))):>11} "
              f"{fmt(data.get(('BF16 Tensor Cores', 8192))):>11} "
              f"{fmt(data.get(('FP32 (TF32 allowed)', 8192))):>11} "
              f"{fmt(report['bandwidth']):>13} {report['failures']:>7}")
    total_failures = detection_failures + sum(report["failures"] for report in reports)
    print(f"Discrete GPUs benchmarked: {len(reports)}; failures: {total_failures}")
    return 1 if total_failures else 0


if __name__ == "__main__":
    sys.exit(main())
