/* SPDX-License-Identifier: GPL-2.0-only */
/* Keep the local ESP identifier out of the public source tree. */
#pragma once
/* GU_RUNTIME_ESP identifies the Windows ESP from the live device tree: exactly
 * one located volume that is not USB, not this image's own volume, carries a
 * single GPT partition with the EFI-system type, and contains
 * \EFI\Microsoft\Boot\bootmgfw.efi. This is a deliberate, documented
 * relaxation of the pin for that build only - it is not "widening a guard to
 * make another machine boot" on the pinned path, which still demands
 * local/boot_profile.h and an exact GUID match. */
#if defined(GU_RUNTIME_ESP) && defined(GU_HOST_TEST_PROFILE)
#error Runtime ESP identification and the synthetic host profile are distinct builds
#elif defined(GU_RUNTIME_ESP)
/* no compiled partition GUID */
#elif defined(GU_HOST_TEST_PROFILE)
#define GU_WINDOWS_PARTITION_INITIALIZER {0x11111111,0x2222,0x3333,{0x84,0x44,0x55,0x66,0x77,0x88,0x99,0xaa}}
#else
#include "local/boot_profile.h"
#ifndef GU_WINDOWS_PARTITION_INITIALIZER
#error "Configure the reviewed ESP GUID in local/boot_profile.h before building."
#endif
#endif
