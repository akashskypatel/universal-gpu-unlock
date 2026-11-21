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
 * The CONTEXT_ macros below are the same derivation over the writer's reads:
 * words it inspects but never writes, including the ones whose value decides
 * whether it proceeds (an inaccessible read is poll_us()'s -EIO).
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

/* Read-only context: every word the writer READS without writing it.
 *
 * The write set above is what the transaction mutates; this list is what it
 * reads to decide whether it may run and what to put in those words. The two
 * are different hazards. A write to a word nobody read is recoverable and
 * visible; a read that comes back inaccessible is the transaction's own stop
 * signal, because poll_us() and every read_bar()+inaccessible() pair return
 * -EIO/-ENODEV on that shape (src/vendor/unlock_610.c). So the probe reports
 * these too: a NOT_ACCESSIBLE here on an actuating boot is the line that says
 * the run died, or will die, before or inside its first write.
 *
 * The list is derived, not curated: tests/test_vram_block_probe.py computes
 * every statically resolvable read_bar()/poll_us() target in
 * src/vendor/unlock_610.c, subtracts the write set, and fails unless this list
 * matches it exactly. Addresses the writer computes from a value it read (the
 * fuse windows at 0x8241c0 and GU_FUSE_SEC2_UCODE1, and 0x1400 + engine * 4)
 * are outside that set on purpose: a probe cannot name a word it would have to
 * compute from a word it is trying to identify. */
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

/* The rest of the read set. Each delta below appears at both engine bases,
 * exactly as the write set does; the absolute words are outside an engine
 * window. */
/* log_falcon_state(): read_bar(work, base + 0x08, &irqstat) */
#define GU_VRAM_BLOCK_CONTEXT_SEC2_IRQSTAT 0x00840008u
#define GU_VRAM_BLOCK_CONTEXT_GSP_IRQSTAT 0x00110008u
/* log_falcon_state(): read_bar(work, base + 0x80, &os) */
#define GU_VRAM_BLOCK_CONTEXT_SEC2_RM_OS 0x00840080u
#define GU_VRAM_BLOCK_CONTEXT_GSP_RM_OS 0x00110080u
/* log_falcon_state(): read_bar(work, base + 0x94, &debug); also the chip word
 * unlock_transaction_boot() re-reads as SEC2_BASE + 0x94 */
#define GU_VRAM_BLOCK_CONTEXT_SEC2_DEBUG 0x00840094u
#define GU_VRAM_BLOCK_CONTEXT_GSP_DEBUG 0x00110094u
/* log_falcon_state(): read_bar(work, base + 0x108, &hwcfg). Not the 0xf4 word
 * falcon_reset() polls: this one is the falcon2-adjacent HwCfg copy. */
#define GU_VRAM_BLOCK_CONTEXT_SEC2_HWCFG2 0x00840108u
#define GU_VRAM_BLOCK_CONTEXT_GSP_HWCFG2 0x00110108u
/* The RISC-V block's halt word: read_bar(work, eng->falcon2 + 0x388, &riscv)
 * in poll_falcon_engine_halt() and is_falcon_engine_halted(). ga102
 * dev_riscv_pri.h, as nouveau's ga102_flcn_riscv_active() reads it. */
#define GU_VRAM_BLOCK_CONTEXT_SEC2_RISCV 0x00841388u
#define GU_VRAM_BLOCK_CONTEXT_GSP_RISCV 0x00111388u
/* GFW handshake words, read before the layout is trusted:
 * poll_us/read_bar(work, GU_GFW_PLM_OFFSET, ...) gates a boot at the entry
 * signal, and GU_GFW_BOOT_OFFSET reports the ROM's own progress. */
#define GU_VRAM_BLOCK_CONTEXT_GFW_PLM 0x00118128u
#define GU_VRAM_BLOCK_CONTEXT_GFW_BOOT 0x00118234u
/* The board's usable framebuffer size: read_bar(work, 0x001183a4, &mb). A word
 * that does not read costs the actuating run its entire layout. */
#define GU_VRAM_BLOCK_CONTEXT_BOOT_MEM_MB 0x001183a4u
/* Memory-controller lock and WPR2 bookkeeping:
 * 0x001fa7c8 the pre-lock PLM the layout reads first,
 * 0x001fa824/0x001fa828 the WPR2 LOW/HIGH pair every stage re-reads,
 * 0x001fa82c/0x001fa830 the boot descriptor's own low/high pair. */
#define GU_VRAM_BLOCK_CONTEXT_PLM_LOCK 0x001fa7c8u
#define GU_VRAM_BLOCK_CONTEXT_WPR2_LO 0x001fa824u
#define GU_VRAM_BLOCK_CONTEXT_WPR2_HI 0x001fa828u
#define GU_VRAM_BLOCK_CONTEXT_BOOT_DESC_LO 0x001fa82cu
#define GU_VRAM_BLOCK_CONTEXT_BOOT_DESC_HI 0x001fa830u
/* read_bar(work, 0x00625f04, &vga): the VGA/RM decoder word the layout uses to
 * place the reserved window. */
#define GU_VRAM_BLOCK_CONTEXT_VGA_CONFIG 0x00625f04u

#endif
