/*
 * nv2a_regs_image_blit.h — local NV09F / NV062 method-offset and
 * RAMHT-handle constants for this XBE.
 *
 * Pulled verbatim from `hw/xbox/nv2a/nv2a_regs.h:818-841` (W1) so
 * the XBE source is self-contained (nxdk's pbkit headers do not
 * re-export the xemu-side method offsets). Keep in sync if W1 ever
 * changes; that would be a behavioral change worth its own decision.
 */
#ifndef NV2A_REGS_IMAGE_BLIT_H
#define NV2A_REGS_IMAGE_BLIT_H

/* NV_CONTEXT_SURFACES_2D (class 0x62) — `nv2a_regs.h:818-831`. */
#define NV062_SET_OBJECT                          0x00000000
#define NV062_SET_CONTEXT_DMA_IMAGE_SOURCE        0x00000184
#define NV062_SET_CONTEXT_DMA_IMAGE_DESTIN        0x00000188
#define NV062_SET_COLOR_FORMAT                    0x00000300
#define NV062_SET_COLOR_FORMAT_LE_Y8              0x01
#define NV062_SET_COLOR_FORMAT_LE_R5G6B5          0x04
#define NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8 0x06
#define NV062_SET_COLOR_FORMAT_LE_X8R8G8B8        0x07
#define NV062_SET_COLOR_FORMAT_LE_A8R8G8B8        0x0A
#define NV062_SET_COLOR_FORMAT_LE_Y32             0x0B
#define NV062_SET_PITCH                           0x00000304
#define NV062_SET_OFFSET_SOURCE                   0x00000308
#define NV062_SET_OFFSET_DESTIN                   0x0000030C

/* NV_IMAGE_BLIT (class 0x9F) — `nv2a_regs.h:833-841`. */
#define NV09F_SET_OBJECT                          0x00000000
#define NV09F_SET_CONTEXT_SURFACES                0x0000019C
#define NV09F_SET_OPERATION                       0x000002FC
#define NV09F_SET_OPERATION_BLEND_AND             2
#define NV09F_SET_OPERATION_SRCCOPY               3
#define NV09F_CONTROL_POINT_IN                    0x00000300
#define NV09F_CONTROL_POINT_OUT                   0x00000304
#define NV09F_SIZE                                0x00000308

/* RAMHT handles for the NV_IMAGE_BLIT (0x9F) + NV_CONTEXT_SURFACES_2D
 * (0x62) graphics objects + DMA contexts the blit needs as source
 * and destination.
 *
 * Why these specific values: PFIFO dispatches SET_OBJECT (method 0)
 * and the "context DMA" methods in the [0x180, 0x200) range through
 * `ramht_lookup` (pfifo.c:572). Lookup hashes the handle into a table
 * sized by `NV_PFIFO_RAMHT_SIZE` (default 4 KiB on pbkit since the
 * field is left at 0 in `pb_init`'s `VIDEOREG(NV_PFIFO_RAMHT) =
 * (base|SEARCH_128)` write — see `lib/pbkit/pbkit.c:2407`). With a
 * 4 KiB table the hash must satisfy `hash * 8 < 4096` (pfifo.c:578
 * assertion). Large handles like the canonical D3D-runtime ones
 * (`0x14d00 / 0x14d10 / 0x11120 / 0x11170`) hash past that bound and
 * trip the assertion — D3D only gets away with them because it
 * separately programs `NV_PFIFO_RAMHT_SIZE` to a larger table.
 *
 * nxdk's `pb_init` registers small ChannelIDs that hash safely
 * within the default 4 KiB table.
 *
 * **DMA channel selection — do NOT use 9 or 11.** Although pbkit
 * creates `sDmaObject9` / `sDmaObject11` with Limit=MAXRAM at
 * `pbkit.c:2647-2649`, `pb_init` then calls
 * `pb_target_back_buffer()` (line 3260), which calls
 * `set_draw_buffer()` (`pbkit.c:1611-1668`). `set_draw_buffer`
 * **reprograms** the PRAMIN entries for `pb_DmaChID9Inst` and
 * `pb_DmaChID11Inst`:
 *   - `addr  = framebuffer_base` (instead of 0)
 *   - `limit = height * pitch - 1` (instead of MAXRAM)
 * For a 640x480 LE_A8R8G8B8 back buffer the limit becomes
 * `0x0012BFFF` (= 1.2 MiB), and `addr` is the framebuffer's
 * physical address. After this reprogram channels 9 and 11 are
 * essentially "the back/front buffer DMA," not general-purpose RAM.
 * Empirically verified by `qemu`-side
 * `trace-events nv2a_dma_map obj 0x00011150` showing
 * `addr 0x03bd4000 limit 0x0012bfff` instead of MAXRAM. An
 * NV062 blit using channel 9 as source/destin asserts at
 * `blit.c:170 (source_offset < source_dma_len)` the moment our
 * source buffer's physical address exceeds 0x0012BFFF, which it
 * always does (the back buffer takes the lowest VRAM addresses and
 * everything else lives above it).
 *
 * **Use channels 3 and 4 instead.** Both are created with base=0
 * and Limit=MAXRAM at `pbkit.c:2643-2645` and are NEVER
 * reprogrammed by pbkit after `pb_init` returns. Sufficient for
 * arbitrary-RAM source-to-RAM blits across the full 64 MiB.
 *   - ChannelID  3: `sDmaObject3`, class 0x3D (DMA_3D), Base=0,
 *                   Limit=MAXRAM    (`pbkit.c:2643`)
 *   - ChannelID  4: `sDmaObject4`, class 0x3  (DMA in memory),
 *                   Base=0, Limit=MAXRAM (`pbkit.c:2645`)
 *   - ChannelID 16: `sGrObject16`, class 0x9F (`pbkit.c:2743`)
 *   - ChannelID 17: `sGrObject17`, class 0x62 (`pbkit.c:2744`)
 *
 * NV062 (`NV_CONTEXT_SURFACES_2D`) does not validate the class of
 * the source/dest DMA channel (xemu-side `pgraph_mtl_image_blit`
 * just calls `nv_dma_map` on the bound channel), so channels 3
 * (CLASS_3D) and 4 (CLASS_3) are both legal NV062 source/dest
 * channels.
 *
 * The assertion `context_surfaces->object_instance ==
 * image_blit->context_surfaces` (`mtl/blit.c:144`) compares the
 * post-RAMHT-lookup `entry.instance` values, which are equal because
 * we route both NV062_SET_OBJECT and NV09F_SET_CONTEXT_SURFACES
 * through handle 17 (= sGrObject17). */
#define IMAGE_BLIT_NV09F_OBJECT_HANDLE   16u
#define IMAGE_BLIT_NV062_OBJECT_HANDLE   17u
#define IMAGE_BLIT_DMA_HANDLE_SRC        3u
#define IMAGE_BLIT_DMA_HANDLE_DST        4u

#endif /* NV2A_REGS_IMAGE_BLIT_H */
