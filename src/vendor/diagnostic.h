/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_DIAGNOSTIC_H
#define GPU_UNLOCK_DIAGNOSTIC_H
#ifdef __KERNEL__
#include <linux/printk.h>
#define GU_NOTE(fmt, ...) pr_info("gpu_unlock: " fmt, ##__VA_ARGS__)
#define GU_ERROR(fmt, ...) pr_err("gpu_unlock: " fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
/* C11-compatible host diagnostics. The parent-style GNU comma-elision
 * form fails with Clang -Werror -pedantic during source-grounded tests. */
#define GU_NOTE(...) do { fputs("gpu_unlock: ", stderr); fprintf(stderr, __VA_ARGS__); } while (0)
#define GU_ERROR(...) do { fputs("gpu_unlock: ", stderr); fprintf(stderr, __VA_ARGS__); } while (0)
#endif

/* No extra device reads. Preserve the errno and identify the exact guard in
 * the matching source, including pure parser failures. */
static inline int gu_diagnostic_result(int error, const char *file,
                                      const char *function, unsigned int line)
{
    if (error)
        GU_ERROR("failure %s:%u %s error=%d\n", file, line, function, error);
    return error;
}
#define GU_DIAG(result) gu_diagnostic_result((result), __FILE__, __func__, __LINE__)
#endif
