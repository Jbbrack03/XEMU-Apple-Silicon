/*
 * S3TC Texture Decompression
 *
 * Copyright (c) 2020 Wilhelm Kovatch
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"

#ifdef __aarch64__
#include <arm_neon.h>
#endif

#include "s3tc.h"
#include "swizzle.h"

/* Test hook: when true, the NEON block writer is bypassed so the self-test can
 * compare scalar vs NEON output. Always false in normal operation. */
bool s3tc_disable_neon = false;

static void decode_bc1_colors(uint16_t c0, uint16_t c1, uint8_t r[4],
                              uint8_t g[4], uint8_t b[4], uint8_t a[16],
                              bool transparent)
{
    r[0] = ((c0 & 0xF800) >> 8) * 0xFF / 0xF8,
    g[0] = ((c0 & 0x07E0) >> 3) * 0xFF / 0xFC,
    b[0] = ((c0 & 0x001F) << 3) * 0xFF / 0xF8,
    a[0] = 255;

    r[1] = ((c1 & 0xF800) >> 8) * 0xFF / 0xF8,
    g[1] = ((c1 & 0x07E0) >> 3) * 0xFF / 0xFC,
    b[1] = ((c1 & 0x001F) << 3) * 0xFF / 0xF8,
    a[1] = 255;

    if (transparent) {
        r[2] = (r[0]+r[1])/2;
        g[2] = (g[0]+g[1])/2;
        b[2] = (b[0]+b[1])/2;
        a[2] = 255;

        r[3] = 0;
        g[3] = 0;
        b[3] = 0;
        a[3] = 0;
    } else {
        r[2] = (2*r[0]+r[1])/3;
        g[2] = (2*g[0]+g[1])/3,
        b[2] = (2*b[0]+b[1])/3;
        a[2] = 255;

        r[3] = (r[0]+2*r[1])/3;
        g[3] = (g[0]+2*g[1])/3;
        b[3] = (b[0]+2*b[1])/3;
        a[3] = 255;
    }
}

#ifdef __aarch64__
static inline uint8x8_t s3tc_make_lookup_table(uint8_t v0, uint8_t v1,
                                               uint8_t v2, uint8_t v3)
{
    uint32_t packed = v0 | ((uint32_t)v1 << 8) | ((uint32_t)v2 << 16) |
                      ((uint32_t)v3 << 24);
    return vreinterpret_u8_u32(vdup_n_u32(packed));
}

static inline uint8x8_t s3tc_make_index_vector(uint8_t packed_indices)
{
    uint64_t indices = (packed_indices & 0x03) |
                       (((uint64_t)((packed_indices >> 2) & 0x03)) << 8) |
                       (((uint64_t)((packed_indices >> 4) & 0x03)) << 16) |
                       (((uint64_t)((packed_indices >> 6) & 0x03)) << 24);
    return vcreate_u8(indices);
}

static inline uint8x8_t s3tc_load_alpha_row(const uint8_t *alpha_row)
{
    uint32_t packed = alpha_row[0] | ((uint32_t)alpha_row[1] << 8) |
                      ((uint32_t)alpha_row[2] << 16) |
                      ((uint32_t)alpha_row[3] << 24);
    return vreinterpret_u8_u32(vdup_n_u32(packed));
}

static inline void s3tc_store_rgba_row(uint8_t *dst, uint8x8_t r,
                                       uint8x8_t g, uint8x8_t b, uint8x8_t a)
{
    uint8x8_t rg = vzip1_u8(r, g);
    uint8x8_t ba = vzip1_u8(b, a);
    uint16x4_t rgba_lo =
        vzip1_u16(vreinterpret_u16_u8(rg), vreinterpret_u16_u8(ba));
    uint16x4_t rgba_hi =
        vzip2_u16(vreinterpret_u16_u8(rg), vreinterpret_u16_u8(ba));

    vst1q_u8(dst, vcombine_u8(vreinterpret_u8_u16(rgba_lo),
                              vreinterpret_u8_u16(rgba_hi)));
}

static bool write_block_to_texture_neon(uint8_t *converted_data, uint32_t indices,
                                        int i, int j, int width, int height,
                                        int z_pos_factor, const uint8_t r[4],
                                        const uint8_t g[4], const uint8_t b[4],
                                        const uint8_t a[16],
                                        bool separate_alpha)
{
    int x0 = i * 4;
    int y0 = j * 4;
    uint8x8_t r_table;
    uint8x8_t g_table;
    uint8x8_t b_table;
    uint8x8_t a_table;

    if (x0 + 4 > width || y0 + 4 > height) {
        return false;
    }

    r_table = s3tc_make_lookup_table(r[0], r[1], r[2], r[3]);
    g_table = s3tc_make_lookup_table(g[0], g[1], g[2], g[3]);
    b_table = s3tc_make_lookup_table(b[0], b[1], b[2], b[3]);
    a_table = s3tc_make_lookup_table(a[0], a[1], a[2], a[3]);

    for (int row = 0; row < 4; row++) {
        uint8_t packed_row_indices = (indices >> (row * 8)) & 0xFF;
        uint8x8_t row_indices = s3tc_make_index_vector(packed_row_indices);
        uint8x8_t row_r = vtbl1_u8(r_table, row_indices);
        uint8x8_t row_g = vtbl1_u8(g_table, row_indices);
        uint8x8_t row_b = vtbl1_u8(b_table, row_indices);
        uint8x8_t row_a = separate_alpha
                              ? s3tc_load_alpha_row(a + row * 4)
                              : vtbl1_u8(a_table, row_indices);
        uint8_t *dst = converted_data +
                       (z_pos_factor + (y0 + row) * width + x0) * 4;

        s3tc_store_rgba_row(dst, row_r, row_g, row_b, row_a);
    }

    return true;
}
#endif

static void write_block_to_texture(uint8_t *converted_data, uint32_t indices,
                                   int i, int j, int width, int height,
                                   int z_pos_factor, uint8_t r[4],
                                   uint8_t g[4], uint8_t b[4], uint8_t a[16],
                                   bool separate_alpha)
{
    int x0 = i * 4,
        y0 = j * 4;

    int x1 = x0 + 4,
        y1 = y0 + 4;

#ifdef __aarch64__
    if (!s3tc_disable_neon &&
        write_block_to_texture_neon(converted_data, indices, i, j, width,
                                    height, z_pos_factor, r, g, b, a,
                                    separate_alpha)) {
        return;
    }
#endif

    for (int y = y0; y < y1 && y < height; y++) {
        int y_index = 4 * (y - y0);
        int z_plus_y_pos_factor = z_pos_factor + y * width;
        for (int x = x0; x < x1 && x < width; x++) {
            int xy_index = y_index + x - x0;
            uint8_t index = (indices >> 2 * xy_index) & 0x03;
            uint8_t alpha_index = separate_alpha ? xy_index : index;
            uint8_t *p = converted_data + (z_plus_y_pos_factor + x) * 4;
            *p++ = r[index];
            *p++ = g[index];
            *p++ = b[index];
            *p++ = a[alpha_index];
        }
    }
}

static void decompress_dxt1_block(const uint8_t block_data[8],
                                  uint8_t *converted_data, int i, int j,
                                  int width, int height, int z_pos_factor)
{
    uint16_t c0 = ((uint16_t*)block_data)[0],
             c1 = ((uint16_t*)block_data)[1];
    uint8_t r[4], g[4], b[4], a[16];
    decode_bc1_colors(c0, c1, r, g, b, a, c0 <= c1);

    uint32_t indices = ((uint32_t*)block_data)[1];
    write_block_to_texture(converted_data, indices,
                           i, j, width, height, z_pos_factor,
                           r, g, b, a, false);
}

static void decompress_dxt3_block(const uint8_t block_data[16],
                                  uint8_t *converted_data, int i, int j,
                                  int width, int height, int z_pos_factor)
{
    uint16_t c0 = ((uint16_t*)block_data)[4],
             c1 = ((uint16_t*)block_data)[5];
    uint8_t r[4], g[4], b[4], a[16];
    decode_bc1_colors(c0, c1, r, g, b, a, false);

    uint64_t alpha = ((uint64_t*)block_data)[0];
    for (int a_i=0; a_i < 16; a_i++) {
        a[a_i] = (((alpha >> 4*a_i) & 0x0F) << 4) * 0xFF / 0xF0;
    }

    uint32_t indices = ((uint32_t*)block_data)[3];
    write_block_to_texture(converted_data, indices,
                           i, j, width, height, z_pos_factor,
                           r, g, b, a, true);
}

static void decompress_dxt5_block(const uint8_t block_data[16],
                                  uint8_t *converted_data, int i, int j,
                                  int width, int height, int z_pos_factor)
{
    uint16_t c0 = ((uint16_t*)block_data)[4],
             c1 = ((uint16_t*)block_data)[5];
    uint8_t r[4], g[4], b[4], a[16];
    decode_bc1_colors(c0, c1, r, g, b, a, false);

    uint64_t alpha = ((uint64_t*)block_data)[0];
    uint8_t a0 = block_data[0];
    uint8_t a1 = block_data[1];
    uint8_t a_palette[8];
    a_palette[0] = a0;
    a_palette[1] = a1;
    if (a0 > a1) {
        a_palette[2] = (6*a0+1*a1)/7;
        a_palette[3] = (5*a0+2*a1)/7;
        a_palette[4] = (4*a0+3*a1)/7;
        a_palette[5] = (3*a0+4*a1)/7;
        a_palette[6] = (2*a0+5*a1)/7;
        a_palette[7] = (1*a0+6*a1)/7;
    } else {
        a_palette[2] = (4*a0+1*a1)/5;
        a_palette[3] = (3*a0+2*a1)/5;
        a_palette[4] = (2*a0+3*a1)/5;
        a_palette[5] = (1*a0+4*a1)/5;
        a_palette[6] = 0;
        a_palette[7] = 255;
    }
    for (int a_i = 0; a_i < 16; a_i++) {
        a[a_i] = a_palette[(alpha >> (16+3*a_i)) & 0x07];
    }

    uint32_t indices = ((uint32_t*)block_data)[3];
    write_block_to_texture(converted_data, indices,
                           i, j, width, height, z_pos_factor,
                           r, g, b, a, true);
}

void s3tc_decompress_3d_to(uint8_t *converted_data,
                           enum S3TC_DECOMPRESS_FORMAT color_format,
                           const uint8_t *data, unsigned int width,
                           unsigned int height, unsigned int depth)
{
    assert(width > 0);
    assert(height > 0);
    assert(depth > 0);
    unsigned int physical_width = (width + 3) & ~3,
                 physical_height = (height + 3) & ~3;
    int num_blocks_x = physical_width/4,
        num_blocks_y = physical_height/4,
        num_blocks_z = (depth + 3)/4;
    int cur_depth = 0;
    int sub_block_index = 0;
    for (int k = 0; k < num_blocks_z; k++) {
        int residual_depth = depth - cur_depth;
        int block_depth = MIN(residual_depth, 4);
        for (int j = 0; j < num_blocks_y; j++) {
            for (int i = 0; i < num_blocks_x; i++) {
                for (int slice = 0; slice < block_depth; slice++) {
                    int z_pos_factor = (cur_depth + slice) * width * height;
                    if (color_format == S3TC_DECOMPRESS_FORMAT_DXT1) {
                        decompress_dxt1_block(data + 8 * sub_block_index, converted_data,
                                              i, j, width, height, z_pos_factor);
                    } else if (color_format == S3TC_DECOMPRESS_FORMAT_DXT3) {
                        decompress_dxt3_block(data + 16 * sub_block_index, converted_data,
                                              i, j, width, height, z_pos_factor);
                    } else if (color_format == S3TC_DECOMPRESS_FORMAT_DXT5) {
                        decompress_dxt5_block(data + 16 * sub_block_index, converted_data,
                                              i, j, width, height, z_pos_factor);
                    } else {
                        assert(false);
                    }
                    sub_block_index++;
                }
            }
        }
        cur_depth += block_depth;
    }
}

uint8_t *s3tc_decompress_3d(enum S3TC_DECOMPRESS_FORMAT color_format,
                            const uint8_t *data, unsigned int width,
                            unsigned int height, unsigned int depth)
{
    uint8_t *converted_data = (uint8_t *)g_malloc(width * height * depth * 4);
    s3tc_decompress_3d_to(converted_data, color_format, data, width, height,
                          depth);
    return converted_data;
}

void s3tc_decompress_2d_to(uint8_t *converted_data,
                           enum S3TC_DECOMPRESS_FORMAT color_format,
                           const uint8_t *data, unsigned int width,
                           unsigned int height)
{
    assert(width > 0);
    assert(height > 0);
    unsigned int physical_width = (width + 3) & ~3,
                 physical_height = (height + 3) & ~3;
    int num_blocks_x = physical_width / 4, num_blocks_y = physical_height / 4;
    for (int j = 0; j < num_blocks_y; j++) {
        for (int i = 0; i < num_blocks_x; i++) {
            int block_index = j * num_blocks_x + i;
            if (color_format == S3TC_DECOMPRESS_FORMAT_DXT1) {
                decompress_dxt1_block(data + 8 * block_index,
                                      converted_data, i, j, width, height, 0);
            } else if (color_format == S3TC_DECOMPRESS_FORMAT_DXT3) {
                decompress_dxt3_block(data + 16 * block_index,
                                      converted_data, i, j, width, height, 0);
            } else if (color_format == S3TC_DECOMPRESS_FORMAT_DXT5) {
                decompress_dxt5_block(data + 16 * block_index,
                                      converted_data, i, j, width, height, 0);
            } else {
                assert(false);
            }
        }
    }
}

uint8_t *s3tc_decompress_2d(enum S3TC_DECOMPRESS_FORMAT color_format,
                            const uint8_t *data, unsigned int width,
                            unsigned int height)
{
    uint8_t *converted_data = (uint8_t *)g_malloc(width * height * 4);
    s3tc_decompress_2d_to(converted_data, color_format, data, width, height);
    return converted_data;
}

/*
 * NEON-vs-scalar bit-exactness self-test for the texture decode paths
 * (DXT1/3/5 block decode and swizzle/unswizzle). Also checks the new
 * decode-into-caller-buffer variants against the malloc-returning ones.
 * Deterministic (fixed PRNG) so results are reproducible. Trigger via the
 * XEMU_TEX_SELFTEST env var at renderer init (see pgraph_vk_init_textures).
 * Returns true if every check passed.
 */
static uint32_t selftest_rand(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s;
}

bool pgraph_texture_selftest(int *out_checks, int *out_failures)
{
    uint32_t seed = 0x1234abcdu;
    int checks = 0, failures = 0;

    static const enum S3TC_DECOMPRESS_FORMAT fmts[] = {
        S3TC_DECOMPRESS_FORMAT_DXT1,
        S3TC_DECOMPRESS_FORMAT_DXT3,
        S3TC_DECOMPRESS_FORMAT_DXT5,
    };
    static const struct { unsigned int w, h; } dims2d[] = {
        { 4, 4 }, { 8, 8 }, { 16, 8 }, { 7, 5 }, { 13, 3 }, { 64, 64 }
    };
    static const struct { unsigned int w, h, d; } dims3d[] = {
        { 4, 4, 4 }, { 8, 8, 2 }, { 16, 8, 5 }, { 7, 5, 3 }
    };

    for (size_t fi = 0; fi < ARRAY_SIZE(fmts); fi++) {
        unsigned int block_bytes =
            (fmts[fi] == S3TC_DECOMPRESS_FORMAT_DXT1) ? 8 : 16;

        for (size_t di = 0; di < ARRAY_SIZE(dims2d); di++) {
            unsigned int w = dims2d[di].w, h = dims2d[di].h;
            unsigned int pw = (w + 3) & ~3u, ph = (h + 3) & ~3u;
            size_t src_len = (size_t)(pw / 4) * (ph / 4) * block_bytes;
            size_t out_len = (size_t)w * h * 4;
            uint8_t *src = g_malloc(src_len);
            for (size_t b = 0; b < src_len; b++) {
                src[b] = selftest_rand(&seed) >> 24;
            }

            s3tc_disable_neon = false;
            uint8_t *neon = s3tc_decompress_2d(fmts[fi], src, w, h);
            s3tc_disable_neon = true;
            uint8_t *scalar = s3tc_decompress_2d(fmts[fi], src, w, h);
            s3tc_disable_neon = false;
            uint8_t *into = g_malloc(out_len);
            s3tc_decompress_2d_to(into, fmts[fi], src, w, h);

            checks += 2;
            if (memcmp(neon, scalar, out_len) != 0) {
                failures++;
                fprintf(stderr, "tex_selftest: DXT%d 2D %ux%u NEON!=scalar\n",
                        fmts[fi] == S3TC_DECOMPRESS_FORMAT_DXT1 ? 1 :
                        fmts[fi] == S3TC_DECOMPRESS_FORMAT_DXT3 ? 3 : 5, w, h);
            }
            if (memcmp(neon, into, out_len) != 0) {
                failures++;
                fprintf(stderr, "tex_selftest: DXT 2D %ux%u _to!=malloc\n", w, h);
            }
            g_free(neon);
            g_free(scalar);
            g_free(into);
            g_free(src);
        }

        for (size_t di = 0; di < ARRAY_SIZE(dims3d); di++) {
            unsigned int w = dims3d[di].w, h = dims3d[di].h, dd = dims3d[di].d;
            unsigned int pw = (w + 3) & ~3u, ph = (h + 3) & ~3u;
            size_t src_len =
                (size_t)(pw / 4) * (ph / 4) * dd * block_bytes;
            size_t out_len = (size_t)w * h * dd * 4;
            uint8_t *src = g_malloc(src_len);
            for (size_t b = 0; b < src_len; b++) {
                src[b] = selftest_rand(&seed) >> 24;
            }

            s3tc_disable_neon = false;
            uint8_t *neon = s3tc_decompress_3d(fmts[fi], src, w, h, dd);
            s3tc_disable_neon = true;
            uint8_t *scalar = s3tc_decompress_3d(fmts[fi], src, w, h, dd);
            s3tc_disable_neon = false;
            uint8_t *into = g_malloc(out_len);
            s3tc_decompress_3d_to(into, fmts[fi], src, w, h, dd);

            checks += 2;
            if (memcmp(neon, scalar, out_len) != 0) {
                failures++;
                fprintf(stderr, "tex_selftest: DXT 3D %ux%ux%u NEON!=scalar\n",
                        w, h, dd);
            }
            if (memcmp(neon, into, out_len) != 0) {
                failures++;
                fprintf(stderr, "tex_selftest: DXT 3D %ux%ux%u _to!=malloc\n",
                        w, h, dd);
            }
            g_free(neon);
            g_free(scalar);
            g_free(into);
            g_free(src);
        }
    }

    /* swizzle / unswizzle: NEON vs scalar + round-trip identity. Dimensions
     * must be powers of two (swizzle mask requirement). */
    static const struct { unsigned int w, h; } swz[] = {
        { 2, 2 }, { 4, 4 }, { 8, 4 }, { 16, 16 }, { 32, 8 }, { 64, 64 }
    };
    for (unsigned int bpp = 1; bpp <= 4; bpp++) {
        for (size_t di = 0; di < ARRAY_SIZE(swz); di++) {
            unsigned int w = swz[di].w, h = swz[di].h;
            unsigned int pitch = w * bpp;
            size_t n = (size_t)h * pitch;
            uint8_t *lin = g_malloc(n);
            for (size_t b = 0; b < n; b++) {
                lin[b] = selftest_rand(&seed) >> 24;
            }
            uint8_t *sw_neon = g_malloc(n);
            uint8_t *sw_scal = g_malloc(n);
            uint8_t *us_neon = g_malloc(n);
            uint8_t *us_scal = g_malloc(n);

            swizzle_disable_neon = false;
            swizzle_rect(lin, w, h, sw_neon, pitch, bpp);
            swizzle_disable_neon = true;
            swizzle_rect(lin, w, h, sw_scal, pitch, bpp);

            swizzle_disable_neon = false;
            unswizzle_rect(sw_neon, w, h, us_neon, pitch, bpp);
            swizzle_disable_neon = true;
            unswizzle_rect(sw_neon, w, h, us_scal, pitch, bpp);
            swizzle_disable_neon = false;

            checks += 3;
            if (memcmp(sw_neon, sw_scal, n) != 0) {
                failures++;
                fprintf(stderr, "tex_selftest: swizzle %ux%u bpp%u NEON!=scalar\n",
                        w, h, bpp);
            }
            if (memcmp(us_neon, us_scal, n) != 0) {
                failures++;
                fprintf(stderr, "tex_selftest: unswizzle %ux%u bpp%u NEON!=scalar\n",
                        w, h, bpp);
            }
            if (memcmp(us_neon, lin, n) != 0) {
                failures++;
                fprintf(stderr, "tex_selftest: swizzle roundtrip %ux%u bpp%u\n",
                        w, h, bpp);
            }
            g_free(lin);
            g_free(sw_neon);
            g_free(sw_scal);
            g_free(us_neon);
            g_free(us_scal);
        }
    }

    if (out_checks) *out_checks = checks;
    if (out_failures) *out_failures = failures;
    return failures == 0;
}
