/* SPDX-License-Identifier: GPL-2.0-only */
/* The BAR0 words the waived transaction WRITES, named once so a read-only
 * probe can read exactly the same addresses before the first actuation.
 *
 * Every GU_VRAM_BLOCK_<X> below corresponds to one write_bar() call in
 * src/vendor/unlock_610.c. The absolute offset is the writer's own expression
 * with eng->base / eng->falcon2 / eng->fbif resolved for both engines
 * (gu_sec2 and gu_gsp). tests/test_vram_block_probe.py derives the writer's
 * write set from unlock_610.c and fails when this list and that set disagree,
 * so a new write_bar() site cannot be added without a matching probe row.
 *
 * The macros are plain numbers: this header carries the address map only and
 * must never grow a write path. The probe that consumes it lives behind
 * GU_VRAM_BLOCK_PROBE in gpu_topology_efi.c and issues BAR0 reads only.
 */
#ifndef GU_VRAM_BLOCK_PROBE_BLOCKS_H
#define GU_VRAM_BLOCK_PROBE_BLOCKS_H

/* SEC2 (Booter) engine: base 0x00840000, falcon2 0x00841000, fbif 0x00840600.
 * The writer reaches these as eng->base/eng->falcon2/eng->fbif + delta with
 * eng == &gu_sec2, and once explicitly as SEC2_BASE + 0x3c0. */
/* write_bar(work, eng->base + 0x3c0, engine | 1) / (engine & ~1u) */
#define GU_VRAM_BLOCK_SEC2_ENGINE_ID 0x008403c0u
/* write_bar(work, eng->base + 0x84, chip) */
#define GU_VRAM_BLOCK_SEC2_CHIP 0x00840084u
/* write_bar(work, eng->base + 0x100, 1u << 1) */
#define GU_VRAM_BLOCK_SEC2_CPUCTL 0x00840100u
/* write_bar(work, eng->base + 0x130, 1u << 1) */
#define GU_VRAM_BLOCK_SEC2_CPUCTL_ALT 0x00840130u
/* write_bar(work, eng->base + 0x104, code_va) */
#define GU_VRAM_BLOCK_SEC2_CODE_VA 0x00840104u
/* write_bar(work, eng->base + 0x40, mailbox0) */
#define GU_VRAM_BLOCK_SEC2_MAILBOX0 0x00840040u
/* write_bar(work, eng->base + 0x44, mailbox1) */
#define GU_VRAM_BLOCK_SEC2_MAILBOX1 0x00840044u
/* write_bar(work, eng->base + 0x10c, 0) */
#define GU_VRAM_BLOCK_SEC2_DMA_CTRL 0x0084010cu
/* falcon_dma(): write_bar(work, base + 0x110, ...) */
#define GU_VRAM_BLOCK_SEC2_DMA_SRC_LO 0x00840110u
/* falcon_dma(): write_bar(work, base + 0x128, ...) */
#define GU_VRAM_BLOCK_SEC2_DMA_SRC_HI 0x00840128u
/* falcon_dma(): write_bar(work, base + 0x114, ...) */
#define GU_VRAM_BLOCK_SEC2_DMA_DEST 0x00840114u
/* falcon_dma(): write_bar(work, base + 0x11c, ...) */
#define GU_VRAM_BLOCK_SEC2_DMA_MEM_OFF 0x0084011cu
/* falcon_dma(): write_bar(work, base + 0x118, ...) */
#define GU_VRAM_BLOCK_SEC2_DMA_CMD 0x00840118u
/* write_bar(work, eng->falcon2 + 0x180, 1) */
#define GU_VRAM_BLOCK_SEC2_BOOTVEC 0x00841180u
/* write_bar(work, eng->falcon2 + 0x198, ucode_id & 0xff) */
#define GU_VRAM_BLOCK_SEC2_UCODE_ID 0x00841198u
/* write_bar(work, eng->falcon2 + 0x19c, engine_mask) */
#define GU_VRAM_BLOCK_SEC2_ENGINE_MASK 0x0084119cu
/* write_bar(work, eng->falcon2 + 0x210, sig_dmem) */
#define GU_VRAM_BLOCK_SEC2_SIG_DMEM 0x00841210u
/* write_bar(work, eng->falcon2 + 0x668, 0) */
#define GU_VRAM_BLOCK_SEC2_BCR 0x00841668u
/* write_bar(work, eng->fbif, trans) */
#define GU_VRAM_BLOCK_SEC2_FBIF_TRANS 0x00840600u
/* write_bar(work, eng->fbif + 0x24, trans) */
#define GU_VRAM_BLOCK_SEC2_FBIF_TRANS_24 0x00840624u

/* GSP engine: base 0x00110000, falcon2 0x00111000, fbif 0x00110600. The
 * writer reaches these through the same eng->base/falcon2/fbif deltas with
 * eng == &gu_gsp, so each delta appears at both engine bases. */
#define GU_VRAM_BLOCK_GSP_ENGINE_ID 0x001103c0u
#define GU_VRAM_BLOCK_GSP_CHIP 0x00110084u
#define GU_VRAM_BLOCK_GSP_CPUCTL 0x00110100u
#define GU_VRAM_BLOCK_GSP_CPUCTL_ALT 0x00110130u
#define GU_VRAM_BLOCK_GSP_CODE_VA 0x00110104u
#define GU_VRAM_BLOCK_GSP_MAILBOX0 0x00110040u
#define GU_VRAM_BLOCK_GSP_MAILBOX1 0x00110044u
#define GU_VRAM_BLOCK_GSP_DMA_CTRL 0x0011010cu
#define GU_VRAM_BLOCK_GSP_DMA_SRC_LO 0x00110110u
#define GU_VRAM_BLOCK_GSP_DMA_SRC_HI 0x00110128u
#define GU_VRAM_BLOCK_GSP_DMA_DEST 0x00110114u
#define GU_VRAM_BLOCK_GSP_DMA_MEM_OFF 0x0011011cu
#define GU_VRAM_BLOCK_GSP_DMA_CMD 0x00110118u
#define GU_VRAM_BLOCK_GSP_BOOTVEC 0x00111180u
#define GU_VRAM_BLOCK_GSP_UCODE_ID 0x00111198u
#define GU_VRAM_BLOCK_GSP_ENGINE_MASK 0x0011119cu
#define GU_VRAM_BLOCK_GSP_SIG_DMEM 0x00111210u
#define GU_VRAM_BLOCK_GSP_BCR 0x00111668u
#define GU_VRAM_BLOCK_GSP_FBIF_TRANS 0x00110600u
#define GU_VRAM_BLOCK_GSP_FBIF_TRANS_24 0x00110624u

/* Speed-selector unlock: write_selector(work, "SS0", GU_FEAT_SS0_OFFSET, ...)
 * and write_selector(work, "SS1", GU_FEAT_SS1_OFFSET, ...). These are the only
 * two writes outside an engine window, and the AD104 selector policy decides
 * whether they are reached at all. */
#define GU_VRAM_BLOCK_FEAT_SS0 0x0082381cu
#define GU_VRAM_BLOCK_FEAT_SS1 0x00823820u

/* Read-only context: the writer READS these words to decide what to write, so
 * the probe reports them too. They are not part of the write set and the drift
 * check excludes them by the CONTEXT_ prefix. */
/* check_entry_selectors(): read_bar(work, GU_FEAT_PLM_OFFSET, &plm) */
#define GU_VRAM_BLOCK_CONTEXT_FEAT_PLM 0x00823804u
/* read_bar(work, GU_FEAT_READOUT_1_OFFSET, ...) */
#define GU_VRAM_BLOCK_CONTEXT_FEAT_READOUT1 0x00823818u
/* The first register falcon_reset() touches: poll_us(work, eng->base + 0xf4,
 * ...) and two later poll_us/read_bar calls on the same word. poll_us refuses
 * with -EIO the moment this word reads as inaccessible, so it decides whether
 * an actuating run gets as far as its first write. */
/* poll_us(work, eng->base + 0xf4, ...) with eng == &gu_sec2 */
#define GU_VRAM_BLOCK_CONTEXT_SEC2_HWCFG 0x008400f4u
/* poll_us(work, eng->base + 0xf4, ...) with eng == &gu_gsp */
#define GU_VRAM_BLOCK_CONTEXT_GSP_HWCFG 0x001100f4u
/* read_bar(work, 0, &chip), re-read later as SEC2_BASE + 0x94 / base + 0x94:
 * the chip word the transaction copies into eng->base + 0x84. */
#define GU_VRAM_BLOCK_CONTEXT_CHIP 0x00000000u

#endif
