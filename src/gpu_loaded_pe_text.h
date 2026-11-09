/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stddef.h>

/* Locate executable .text bytes in a *mapped* PE32+ image. This is a bounded
 * read-only fingerprint span, NEVER an original-file signature validator. */
int GuLoadedPeTextRange(const void *image, size_t mapped_size,
                        size_t *text_rva, size_t *text_size);
